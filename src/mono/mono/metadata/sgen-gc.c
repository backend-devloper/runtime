/*
 * sgen-gc.c: Simple generational GC.
 *
 * Author:
 * 	Paolo Molaro (lupus@ximian.com)
 *
 * Copyright 2005-2009 Novell, Inc (http://www.novell.com)
 *
 * Thread start/stop adapted from Boehm's GC:
 * Copyright (c) 1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1996 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1998 by Fergus Henderson.  All rights reserved.
 * Copyright (c) 2000-2004 by Hewlett-Packard Company.  All rights reserved.
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose,  provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 *
 * All the rest of the code is LGPL.
 *
 * Important: allocation provides always zeroed memory, having to do
 * a memset after allocation is deadly for performance.
 * Memory usage at startup is currently as follows:
 * 64 KB pinned space
 * 64 KB internal space
 * size of nursery
 * We should provide a small memory config with half the sizes
 *
 * We currently try to make as few mono assumptions as possible:
 * 1) 2-word header with no GC pointers in it (first vtable, second to store the
 *    forwarding ptr)
 * 2) gc descriptor is the second word in the vtable (first word in the class)
 * 3) 8 byte alignment is the minimum and enough (not true for special structures, FIXME)
 * 4) there is a function to get an object's size and the number of
 *    elements in an array.
 * 5) we know the special way bounds are allocated for complex arrays
 *
 * Always try to keep stack usage to a minimum: no recursive behaviour
 * and no large stack allocs.
 *
 * General description.
 * Objects are initially allocated in a nursery using a fast bump-pointer technique.
 * When the nursery is full we start a nursery collection: this is performed with a
 * copying GC.
 * When the old generation is full we start a copying GC of the old generation as well:
 * this will be changed to mark/compact in the future.
 * The things that complicate this description are:
 * *) pinned objects: we can't move them so we need to keep track of them
 * *) no precise info of the thread stacks and registers: we need to be able to
 *    quickly find the objects that may be referenced conservatively and pin them
 *    (this makes the first issues more important)
 * *) large objects are too expensive to be dealt with using copying GC: we handle them
 *    with mark/sweep during major collections
 * *) some objects need to not move even if they are small (interned strings, Type handles):
 *    we use mark/sweep for them, too: they are not allocated in the nursery, but inside
 *    PinnedChunks regions
 */

/*
 * TODO:
 *) change the jit to emit write barrier calls when needed (we
  can have specialized write barriers): done with icalls, still need to
  use some specialized barriers
 *) we could have a function pointer in MonoClass to implement
  customized write barriers for value types
 *) the write barrier code could be isolated in a couple of functions: when a
  thread is stopped if it's inside the barrier it is let go again
  until we stop outside of them (not really needed, see below GC-safe points)
 *) investigate the stuff needed to advance a thread to a GC-safe
  point (single-stepping, read from unmapped memory etc) and implement it
  Not needed yet: since we treat the objects reachable from the stack/regs as
  roots, we store the ptr and exec the write barrier so there is no race.
  We may need this to solve the issue with setting the length of arrays and strings.
  We may need this also for handling precise info on stacks, even simple things
  as having uninitialized data on the stack and having to wait for the prolog
  to zero it. Not an issue for the last frame that we scan conservatively.
  We could always not trust the value in the slots anyway.
 *) make the jit info table lock free
 *) modify the jit to save info about references in stack locations:
  this can be done just for locals as a start, so that at least
  part of the stack is handled precisely.
 *) Make the debug printf stuff thread and signal safe.
 *) test/fix 64 bit issues
 *) test/fix endianess issues
 *) port to non-Linux
 *) add batch moving profile info
 *) add more timing info
 *) there is a possible race when an array or string is created: the vtable is set,
    but the length is set only later so if the GC needs to scan the object in that window,
    it won't get the correct size for the object. The object can't have references and it will
    be pinned, but a free memory fragment may be created that overlaps with it.
    We should change the array max_length field to be at the same offset as the string length:
    this way we can have a single special alloc function for them that sets the length.
    Multi-dim arrays have the same issue for rank == 1 for the bounds data.
 *) implement a card table as the write barrier instead of remembered sets?
 *) some sort of blacklist support?
 *) fin_ready_list and critical_fin_list are part of the root set, too
 *) consider lowering the large object min size to 16/32KB or so and benchmark
 *) once mark-compact is implemented we could still keep the
    copying collector for the old generation and use it if we think
    it is better (small heaps and no pinning object in the old
    generation)
  *) avoid the memory store from copy_object when not needed.
  *) optimize the write barriers fastpath to happen in managed code
  *) add an option to mmap the whole heap in one chunk: it makes for many
     simplifications in the checks (put the nursery at the top and just use a single
     check for inclusion/exclusion): the issue this has is that on 32 bit systems it's
     not flexible (too much of the address space may be used by default or we can't
     increase the heap as needed) and we'd need a race-free mechanism to return memory
     back to the system (mprotect(PROT_NONE) will still keep the memory allocated if it
     was written to, munmap is needed, but the following mmap may not find the same segment
     free...)
   *) memzero the fragments after restarting the world and optionally a smaller chunk at a time
   *) an additional strategy to realloc/expand the nursery when fully pinned is to start
      allocating objects in the old generation. This means that we can't optimize away write
      barrier calls in ctors (but that is not valid for other reasons, too).
   *) add write barriers to the Clone methods
 */
#include "config.h"
#ifdef HAVE_SGEN_GC

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <semaphore.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include "metadata/metadata-internals.h"
#include "metadata/class-internals.h"
#include "metadata/gc-internal.h"
#include "metadata/object-internals.h"
#include "metadata/threads.h"
#include "metadata/sgen-gc.h"
#include "metadata/sgen-archdep.h"
#include "metadata/mono-gc.h"
#include "metadata/method-builder.h"
#include "metadata/profiler-private.h"
#include "metadata/monitor.h"
#include "metadata/threadpool-internals.h"
#include "metadata/mempool-internals.h"
#include "utils/mono-mmap.h"
#include "utils/mono-time.h"
#include "utils/mono-semaphore.h"
#include "utils/mono-counters.h"

#include <mono/utils/memcheck.h>

#define OPDEF(a,b,c,d,e,f,g,h,i,j) \
	a = i,

enum {
#include "mono/cil/opcode.def"
	CEE_LAST
};

#undef OPDEF

/*
 * ######################################################################
 * ########  Types and constants used by the GC.
 * ######################################################################
 */
#if SIZEOF_VOID_P == 4
typedef guint32 mword;
#else
typedef guint64 mword;
#endif

static int gc_initialized = 0;
static int gc_debug_level = 0;
static FILE* gc_debug_file;
/* If set, do a minor collection before every allocation */
static gboolean collect_before_allocs = FALSE;
/* If set, do a heap consistency check before each minor collection */
static gboolean consistency_check_at_minor_collection = FALSE;
/* If set, check that there are no references to the domain left at domain unload */
static gboolean xdomain_checks = FALSE;
/* If not null, dump the heap after each collection into this file */
static FILE *heap_dump_file = NULL;

/*
 * Turning on heavy statistics will turn off the managed allocator and
 * the managed write barrier.
 */
//#define HEAVY_STATISTICS

#ifdef HEAVY_STATISTICS
#define HEAVY_STAT(x)	x
#else
#define HEAVY_STAT(x)
#endif

#ifdef HEAVY_STATISTICS
static long stat_objects_alloced = 0;
static long stat_copy_object_called_nursery = 0;
static long stat_objects_copied_nursery = 0;
static long stat_copy_object_called_major = 0;
static long stat_objects_copied_major = 0;

static long stat_copy_object_failed_from_space = 0;
static long stat_copy_object_failed_forwarded = 0;
static long stat_copy_object_failed_pinned = 0;
static long stat_copy_object_failed_large_pinned = 0;
static long stat_copy_object_failed_to_space = 0;

static long stat_store_remsets = 0;
static long stat_store_remsets_unique = 0;
static long stat_saved_remsets_1 = 0;
static long stat_saved_remsets_2 = 0;
static long stat_global_remsets_added = 0;
static long stat_global_remsets_processed = 0;

static long num_copy_object_called = 0;
static long num_objects_copied = 0;

static int stat_wbarrier_set_field = 0;
static int stat_wbarrier_set_arrayref = 0;
static int stat_wbarrier_arrayref_copy = 0;
static int stat_wbarrier_generic_store = 0;
static int stat_wbarrier_generic_store_remset = 0;
static int stat_wbarrier_set_root = 0;
static int stat_wbarrier_value_copy = 0;
static int stat_wbarrier_object_copy = 0;
#endif

static long pinned_chunk_bytes_alloced = 0;
static long large_internal_bytes_alloced = 0;

enum {
	INTERNAL_MEM_PIN_QUEUE,
	INTERNAL_MEM_FRAGMENT,
	INTERNAL_MEM_SECTION,
	INTERNAL_MEM_SCAN_STARTS,
	INTERNAL_MEM_FIN_TABLE,
	INTERNAL_MEM_FINALIZE_ENTRY,
	INTERNAL_MEM_DISLINK_TABLE,
	INTERNAL_MEM_DISLINK,
	INTERNAL_MEM_ROOTS_TABLE,
	INTERNAL_MEM_ROOT_RECORD,
	INTERNAL_MEM_STATISTICS,
	INTERNAL_MEM_REMSET,
	INTERNAL_MEM_GRAY_QUEUE,
	INTERNAL_MEM_STORE_REMSET,
	INTERNAL_MEM_MAX
};

static long small_internal_mem_bytes [INTERNAL_MEM_MAX];

/*
void
mono_gc_flush_info (void)
{
	fflush (gc_debug_file);
}
*/

#define MAX_DEBUG_LEVEL 8
#define DEBUG(level,a) do {if (G_UNLIKELY ((level) <= MAX_DEBUG_LEVEL && (level) <= gc_debug_level)) a;} while (0)

#define TV_DECLARE(name) gint64 name
#define TV_GETTIME(tv) tv = mono_100ns_ticks ()
#define TV_ELAPSED(start,end) (int)((end-start) / 10)

#define GC_BITS_PER_WORD (sizeof (mword) * 8)

enum {
	MEMORY_ROLE_GEN0,
	MEMORY_ROLE_GEN1,
	MEMORY_ROLE_PINNED
};

typedef struct _Block Block;
struct _Block {
	void *next;
	unsigned char role;
};

/* each request from the OS ends up in a GCMemSection */
typedef struct _GCMemSection GCMemSection;
struct _GCMemSection {
	Block block;
	char *data;
	mword size;
	/* pointer where more data could be allocated if it fits */
	char *next_data;
	char *end_data;
	/* 
	 * scan starts is an array of pointers to objects equally spaced in the allocation area
	 * They let use quickly find pinned objects from pinning pointers.
	 */
	char **scan_starts;
	/* in major collections indexes in the pin_queue for objects that pin this section */
	int pin_queue_start;
	int pin_queue_end;
	unsigned short num_scan_start;
	gboolean is_to_space;
};

#define SIZEOF_GC_MEM_SECTION	((sizeof (GCMemSection) + 7) & ~7)

/* large object space struct: 64+ KB */
/* we could make this limit much smaller to avoid memcpy copy
 * and potentially have more room in the GC descriptor: need to measure
 * This also means that such small OS objects will need to be
 * allocated in a different way (using pinned chunks).
 * We may want to put large but smaller than 64k objects in the fixed space
 * when we move the object from one generation to another (to limit the
 * pig in the snake effect).
 * Note: it may be worth to have an optimized copy function, since we can
 * assume that objects are aligned and have a multiple of 8 size.
 * FIXME: This structure needs to be a multiple of 8 bytes in size: this is not
 * true if MONO_ZERO_LEN_ARRAY is nonzero.
 */
typedef struct _LOSObject LOSObject;
struct _LOSObject {
	LOSObject *next;
	mword size; /* this is the object size */
	int dummy; /* to have a sizeof (LOSObject) a multiple of ALLOC_ALIGN  and data starting at same alignment */
	guint16 role;
	guint16 scanned;
	char data [MONO_ZERO_LEN_ARRAY];
};

/* Pinned objects are allocated in the LOS space if bigger than half a page
 * or from freelists otherwise. We assume that pinned objects are relatively few
 * and they have a slow dying speed (like interned strings, thread objects).
 * As such they will be collected only at major collections.
 * free lists are not global: when we need memory we allocate a PinnedChunk.
 * Each pinned chunk is made of several pages, the first of wich is used
 * internally for bookeeping (here think of a page as 4KB). The bookeeping
 * includes the freelists vectors and info about the object size of each page
 * in the pinned chunk. So, when needed, a free page is found in a pinned chunk,
 * a size is assigned to it, the page is divided in the proper chunks and each
 * chunk is added to the freelist. To not waste space, the remaining space in the
 * first page is used as objects of size 16 or 32 (need to measure which are more
 * common).
 * We use this same structure to allocate memory used internally by the GC, so
 * we never use malloc/free if we need to alloc during collection: the world is stopped
 * and malloc/free will deadlock.
 * When we want to iterate over pinned objects, we just scan a page at a time
 * linearly according to the size of objects in the page: the next pointer used to link
 * the items in the freelist uses the same word as the vtable. Since we keep freelists
 * for each pinned chunk, if the word points outside the pinned chunk it means
 * it is an object.
 * We could avoid this expensive scanning in creative ways. We could have a policy
 * of putting in the pinned space only objects we know about that have no struct fields
 * with references and we can easily use a even expensive write barrier for them,
 * since pointer writes on such objects should be rare.
 * The best compromise is to just alloc interned strings and System.MonoType in them.
 * It would be nice to allocate MonoThread in it, too: must check that we properly
 * use write barriers so we don't have to do any expensive scanning of the whole pinned
 * chunk list during minor collections. We can avoid it now because we alloc in it only
 * reference-free objects.
 */
#define PINNED_FIRST_SLOT_SIZE (sizeof (gpointer) * 4)
#define MAX_FREELIST_SIZE 2048
#define PINNED_PAGE_SIZE (4096)
#define PINNED_CHUNK_MIN_SIZE (4096*8)
typedef struct _PinnedChunk PinnedChunk;
struct _PinnedChunk {
	Block block;
	int num_pages;
	int *page_sizes; /* a 0 means the page is still unused */
	void **free_list;
	void *start_data;
	void *data [1]; /* page sizes and free lists are stored here */
};

/* The method used to clear the nursery */
/* Clearing at nursery collections is the safest, but has bad interactions with caches.
 * Clearing at TLAB creation is much faster, but more complex and it might expose hard
 * to find bugs.
 */
typedef enum {
	CLEAR_AT_GC,
	CLEAR_AT_TLAB_CREATION
} NurseryClearPolicy;

static NurseryClearPolicy nursery_clear_policy = CLEAR_AT_TLAB_CREATION;

/* 
 * If this is set, the nursery is aligned to an address aligned to its size, ie.
 * a 1MB nursery will be aligned to an address divisible by 1MB. This allows us to
 * speed up ptr_in_nursery () checks which are very frequent. This requires the
 * nursery size to be a compile time constant.
 */
#define ALIGN_NURSERY 1

/*
 * The young generation is divided into fragments. This is because
 * we can hand one fragments to a thread for lock-less fast alloc and
 * because the young generation ends up fragmented anyway by pinned objects.
 * Once a collection is done, a list of fragments is created. When doing
 * thread local alloc we use smallish nurseries so we allow new threads to
 * allocate memory from gen0 without triggering a collection. Threads that
 * are found to allocate lots of memory are given bigger fragments. This
 * should make the finalizer thread use little nursery memory after a while.
 * We should start assigning threads very small fragments: if there are many
 * threads the nursery will be full of reserved space that the threads may not
 * use at all, slowing down allocation speed.
 * Thread local allocation is done from areas of memory Hotspot calls Thread Local 
 * Allocation Buffers (TLABs).
 */
typedef struct _Fragment Fragment;

struct _Fragment {
	Fragment *next;
	char *fragment_start;
	char *fragment_limit; /* the current soft limit for allocation */
	char *fragment_end;
};

/* the runtime can register areas of memory as roots: we keep two lists of roots,
 * a pinned root set for conservatively scanned roots and a normal one for
 * precisely scanned roots (currently implemented as a single list).
 */
typedef struct _RootRecord RootRecord;
struct _RootRecord {
	RootRecord *next;
	char *start_root;
	char *end_root;
	mword root_desc;
};

/* for use with write barriers */
typedef struct _RememberedSet RememberedSet;
struct _RememberedSet {
	mword *store_next;
	mword *end_set;
	RememberedSet *next;
	mword data [MONO_ZERO_LEN_ARRAY];
};

/*
 * We're never actually using the first element.  It's always set to
 * NULL to simplify the elimination of consecutive duplicate
 * entries.
 */
#define STORE_REMSET_BUFFER_SIZE	1024

typedef struct _GenericStoreRememberedSet GenericStoreRememberedSet;
struct _GenericStoreRememberedSet {
	GenericStoreRememberedSet *next;
	/* We need one entry less because the first entry of store
	   remset buffers is always a dummy and we don't copy it. */
	gpointer data [STORE_REMSET_BUFFER_SIZE - 1];
};

/* we have 4 possible values in the low 2 bits */
enum {
	REMSET_LOCATION, /* just a pointer to the exact location */
	REMSET_RANGE,    /* range of pointer fields */
	REMSET_OBJECT,   /* mark all the object for scanning */
	REMSET_OTHER,    /* all others */
	REMSET_TYPE_MASK = 0x3
};

/* Subtypes of REMSET_OTHER */
enum {
	REMSET_VTYPE, /* a valuetype array described by a gc descriptor and a count */
	REMSET_ROOT_LOCATION, /* a location inside a root */
};

#ifdef HAVE_KW_THREAD
static __thread RememberedSet *remembered_set MONO_TLS_FAST;
#endif
static pthread_key_t remembered_set_key;
static RememberedSet *global_remset;
static RememberedSet *freed_thread_remsets;
//static int store_to_global_remset = 0;
static GenericStoreRememberedSet *generic_store_remsets = NULL;

/* FIXME: later choose a size that takes into account the RememberedSet struct
 * and doesn't waste any alloc paddin space.
 */
#define DEFAULT_REMSET_SIZE 1024
static RememberedSet* alloc_remset (int size, gpointer id);

/* Structure that corresponds to a MonoVTable: desc is a mword so requires
 * no cast from a pointer to an integer
 */
typedef struct {
	MonoClass *klass;
	mword desc;
} GCVTable;

/* these bits are set in the object vtable: we could merge them since an object can be
 * either pinned or forwarded but not both.
 * We store them in the vtable slot because the bits are used in the sync block for
 * other purposes: if we merge them and alloc the sync blocks aligned to 8 bytes, we can change
 * this and use bit 3 in the syncblock (with the lower two bits both set for forwarded, that
 * would be an invalid combination for the monitor and hash code).
 * The values are already shifted.
 * The forwarding address is stored in the sync block.
 */
#define FORWARDED_BIT 1
#define PINNED_BIT 2
#define VTABLE_BITS_MASK 0x3

/* returns NULL if not forwarded, or the forwarded address */
#define object_is_forwarded(obj) (((mword*)(obj))[0] & FORWARDED_BIT? (void*)(((mword*)(obj))[1]): NULL)
/* set the forwarded address fw_addr for object obj */
#define forward_object(obj,fw_addr) do {	\
		((mword*)(obj))[0] |= FORWARDED_BIT;	\
		((mword*)(obj))[1] = (mword)(fw_addr);	\
	} while (0)

#define object_is_pinned(obj) (((mword*)(obj))[0] & PINNED_BIT)
#define pin_object(obj) do {	\
		((mword*)(obj))[0] |= PINNED_BIT;	\
	} while (0)
#define unpin_object(obj) do {	\
		((mword*)(obj))[0] &= ~PINNED_BIT;	\
	} while (0)

#ifdef ALIGN_NURSERY
#define ptr_in_nursery(ptr) (((mword)(ptr) & ~((1 << DEFAULT_NURSERY_BITS) - 1)) == (mword)nursery_start)
#else
#define ptr_in_nursery(ptr) ((char*)(ptr) >= nursery_start && (char*)(ptr) < nursery_real_end)
#endif

/*
 * Since we set bits in the vtable, use the macro to load it from the pointer to
 * an object that is potentially pinned.
 */
#define LOAD_VTABLE(addr) ((*(mword*)(addr)) & ~VTABLE_BITS_MASK)

static const char*
safe_name (void* obj)
{
	MonoVTable *vt = (MonoVTable*)LOAD_VTABLE (obj);
	return vt->klass->name;
}

static inline guint
safe_object_get_size (MonoObject* o)
{
	MonoClass *klass = ((MonoVTable*)LOAD_VTABLE (o))->klass;
	if (klass == mono_defaults.string_class) {
		return sizeof (MonoString) + 2 * mono_string_length ((MonoString*) o) + 2;
	} else if (klass->rank) {
		MonoArray *array = (MonoArray*)o;
		size_t size = sizeof (MonoArray) + mono_array_element_size (klass) * mono_array_length (array);
		if (G_UNLIKELY (array->bounds)) {
			size += sizeof (mono_array_size_t) - 1;
			size &= ~(sizeof (mono_array_size_t) - 1);
			size += sizeof (MonoArrayBounds) * klass->rank;
		}
		return size;
	} else {
		/* from a created object: the class must be inited already */
		return klass->instance_size;
	}
}

static inline gboolean
is_maybe_half_constructed (MonoObject *o)
{
	MonoClass *klass;

	klass = ((MonoVTable*)LOAD_VTABLE (o))->klass;
	if ((klass == mono_defaults.string_class && mono_string_length ((MonoString*)o) == 0) ||
		(klass->rank && mono_array_length ((MonoArray*)o) == 0))
		return TRUE;
	else
		return FALSE;
}

/*
 * ######################################################################
 * ########  Global data.
 * ######################################################################
 */
static LOCK_DECLARE (gc_mutex);
static int gc_disabled = 0;
static int num_minor_gcs = 0;
static int num_major_gcs = 0;

/* good sizes are 512KB-1MB: larger ones increase a lot memzeroing time */
//#define DEFAULT_NURSERY_SIZE (1024*512*125+4096*118)
#define DEFAULT_NURSERY_SIZE (1024*512*2)
/* The number of trailing 0 bits in DEFAULT_NURSERY_SIZE */
#define DEFAULT_NURSERY_BITS 20
#define MAJOR_SECTION_SIZE	(128*1024)
#define BLOCK_FOR_OBJECT(o)		((Block*)(((mword)(o)) & ~(MAJOR_SECTION_SIZE - 1)))
#define MAJOR_SECTION_FOR_OBJECT(o)	((GCMemSection*)BLOCK_FOR_OBJECT ((o)))
#define DEFAULT_MINOR_COLLECTION_SECTION_ALLOWANCE	(DEFAULT_NURSERY_SIZE * 3 / MAJOR_SECTION_SIZE)
#define DEFAULT_LOS_COLLECTION_TARGET (DEFAULT_NURSERY_SIZE * 2)
/* to quickly find the head of an object pinned by a conservative address
 * we keep track of the objects allocated for each SCAN_START_SIZE memory
 * chunk in the nursery or other memory sections. Larger values have less
 * memory overhead and bigger runtime cost. 4-8 KB are reasonable values.
 */
#define SCAN_START_SIZE (4096*2)
/* the minimum size of a fragment that we consider useful for allocation */
#define FRAGMENT_MIN_SIZE (512)
/* This is a fixed value used for pinned chunks, not the system pagesize */
#define FREELIST_PAGESIZE 4096

static mword pagesize = 4096;
static mword nursery_size = DEFAULT_NURSERY_SIZE;
static int section_size_used = 0;
static int degraded_mode = 0;

static int minor_collection_section_allowance = DEFAULT_MINOR_COLLECTION_SECTION_ALLOWANCE;
static int minor_collection_sections_alloced = 0;
static int sections_alloced = 0; /* will be reset frequently */

static LOSObject *los_object_list = NULL;
static mword los_memory_usage = 0;
static mword los_num_objects = 0;
static mword next_los_collection = 2*1024*1024; /* 2 MB, need to tune */
static mword total_alloc = 0;
/* use this to tune when to do a major/minor collection */
static mword memory_pressure = 0;

static GCMemSection *section_list = NULL;
static GCMemSection *nursery_section = NULL;
static mword lowest_heap_address = ~(mword)0;
static mword highest_heap_address = 0;

typedef struct _FinalizeEntry FinalizeEntry;
struct _FinalizeEntry {
	FinalizeEntry *next;
	void *object;
};

typedef struct _FinalizeEntryHashTable FinalizeEntryHashTable;
struct _FinalizeEntryHashTable {
	FinalizeEntry **table;
	mword size;
	int num_registered;
};

typedef struct _DisappearingLink DisappearingLink;
struct _DisappearingLink {
	DisappearingLink *next;
	void **link;
};

typedef struct _DisappearingLinkHashTable DisappearingLinkHashTable;
struct _DisappearingLinkHashTable {
	DisappearingLink **table;
	mword size;
	int num_links;
};

#define LARGE_INTERNAL_MEM_HEADER_MAGIC	0x7d289f3a

typedef struct _LargeInternalMemHeader LargeInternalMemHeader;
struct _LargeInternalMemHeader {
	guint32 magic;
	size_t size;
	double data[0];
};

enum {
	GENERATION_NURSERY,
	GENERATION_OLD,
	GENERATION_MAX
};

/*
 * The link pointer is hidden by negating each bit.  We use the lowest
 * bit of the link (before negation) to store whether it needs
 * resurrection tracking.
 */
#define HIDE_POINTER(p,t)	((gpointer)(~((gulong)(p)|((t)?1:0))))
#define REVEAL_POINTER(p)	((gpointer)((~(gulong)(p))&~3L))

#define DISLINK_OBJECT(d)	(REVEAL_POINTER (*(d)->link))
#define DISLINK_TRACK(d)	((~(gulong)(*(d)->link)) & 1)

/*
 * The finalizable hash has the object as the key, the 
 * disappearing_link hash, has the link address as key.
 */
static FinalizeEntryHashTable minor_finalizable_hash;
static FinalizeEntryHashTable major_finalizable_hash;
/* objects that are ready to be finalized */
static FinalizeEntry *fin_ready_list = NULL;
static FinalizeEntry *critical_fin_list = NULL;

static DisappearingLinkHashTable minor_disappearing_link_hash;
static DisappearingLinkHashTable major_disappearing_link_hash;

static int num_ready_finalizers = 0;
static int no_finalize = 0;

/* keep each size a multiple of ALLOC_ALIGN */
/* on 64 bit systems 8 is likely completely unused. */
static const int freelist_sizes [] = {
	8, 16, 24, 32, 40, 48, 64, 80,
	96, 128, 160, 192, 224, 256, 320, 384,
	448, 512, 584, 680, 816, 1024, 1360, 2048};
#define FREELIST_NUM_SLOTS (sizeof (freelist_sizes) / sizeof (freelist_sizes [0]))

static char* max_pinned_chunk_addr = NULL;
static char* min_pinned_chunk_addr = (char*)-1;
/* pinned_chunk_list is used for allocations of objects that are never moved */
static PinnedChunk *pinned_chunk_list = NULL;
/* internal_chunk_list is used for allocating structures needed by the GC */
static PinnedChunk *internal_chunk_list = NULL;

static gboolean
obj_is_from_pinned_alloc (char *p)
{
	return BLOCK_FOR_OBJECT (p)->role == MEMORY_ROLE_PINNED;
}

static int slot_for_size (size_t size);

static void
free_pinned_object (PinnedChunk *chunk, char *obj, size_t size)
{
	void **p = (void**)obj;
	int slot = slot_for_size (size);

	g_assert (obj >= (char*)chunk->start_data && obj < ((char*)chunk + chunk->num_pages * FREELIST_PAGESIZE));
	*p = chunk->free_list [slot];
	chunk->free_list [slot] = p;
}

enum {
	ROOT_TYPE_NORMAL = 0, /* "normal" roots */
	ROOT_TYPE_PINNED = 1, /* roots without a GC descriptor */
	ROOT_TYPE_WBARRIER = 2, /* roots with a write barrier */
	ROOT_TYPE_NUM
};

/* registered roots: the key to the hash is the root start address */
/* 
 * Different kinds of roots are kept separate to speed up pin_from_roots () for example.
 */
static RootRecord **roots_hash [ROOT_TYPE_NUM] = { NULL, NULL };
static int roots_hash_size [ROOT_TYPE_NUM] = { 0, 0, 0 };
static mword roots_size = 0; /* amount of memory in the root set */
static int num_roots_entries [ROOT_TYPE_NUM] = { 0, 0, 0 };

/* 
 * The current allocation cursors
 * We allocate objects in the nursery.
 * The nursery is the area between nursery_start and nursery_real_end.
 * Allocation is done from a Thread Local Allocation Buffer (TLAB). TLABs are allocated
 * from nursery fragments.
 * tlab_next is the pointer to the space inside the TLAB where the next object will 
 * be allocated.
 * tlab_temp_end is the pointer to the end of the temporary space reserved for
 * the allocation: it allows us to set the scan starts at reasonable intervals.
 * tlab_real_end points to the end of the TLAB.
 * nursery_frag_real_end points to the end of the currently used nursery fragment.
 * nursery_first_pinned_start points to the start of the first pinned object in the nursery
 * nursery_last_pinned_end points to the end of the last pinned object in the nursery
 * At the next allocation, the area of the nursery where objects can be present is
 * between MIN(nursery_first_pinned_start, first_fragment_start) and
 * MAX(nursery_last_pinned_end, nursery_frag_real_end)
 */
static char *nursery_start = NULL;

/* eventually share with MonoThread? */
typedef struct _SgenThreadInfo SgenThreadInfo;

struct _SgenThreadInfo {
	SgenThreadInfo *next;
	ARCH_THREAD_TYPE id;
	unsigned int stop_count; /* to catch duplicate signals */
	int signal;
	int skip;
	void *stack_end;
	void *stack_start;
	void *stack_start_limit;
	char **tlab_next_addr;
	char **tlab_start_addr;
	char **tlab_temp_end_addr;
	char **tlab_real_end_addr;
	gpointer **store_remset_buffer_addr;
	int *store_remset_buffer_index_addr;
	RememberedSet *remset;
	gpointer runtime_data;
	gpointer stopped_ip;	/* only valid if the thread is stopped */
	MonoDomain *stopped_domain; /* ditto */
	gpointer *stopped_regs;	    /* ditto */
#ifndef HAVE_KW_THREAD
	char *tlab_start;
	char *tlab_next;
	char *tlab_temp_end;
	char *tlab_real_end;
	gpointer *store_remset_buffer;
	int store_remset_buffer_index;
#endif
};

#ifdef HAVE_KW_THREAD
#define TLAB_ACCESS_INIT
#define TLAB_START	tlab_start
#define TLAB_NEXT	tlab_next
#define TLAB_TEMP_END	tlab_temp_end
#define TLAB_REAL_END	tlab_real_end
#define REMEMBERED_SET	remembered_set
#define STORE_REMSET_BUFFER	store_remset_buffer
#define STORE_REMSET_BUFFER_INDEX	store_remset_buffer_index
#else
static pthread_key_t thread_info_key;
#define TLAB_ACCESS_INIT	SgenThreadInfo *__thread_info__ = pthread_getspecific (thread_info_key)
#define TLAB_START	(__thread_info__->tlab_start)
#define TLAB_NEXT	(__thread_info__->tlab_next)
#define TLAB_TEMP_END	(__thread_info__->tlab_temp_end)
#define TLAB_REAL_END	(__thread_info__->tlab_real_end)
#define REMEMBERED_SET	(__thread_info__->remset)
#define STORE_REMSET_BUFFER	(__thread_info__->store_remset_buffer)
#define STORE_REMSET_BUFFER_INDEX	(__thread_info__->store_remset_buffer_index)
#endif

/*
 * FIXME: What is faster, a TLS variable pointing to a structure, or separate TLS 
 * variables for next+temp_end ?
 */
#ifdef HAVE_KW_THREAD
static __thread char *tlab_start;
static __thread char *tlab_next;
static __thread char *tlab_temp_end;
static __thread char *tlab_real_end;
static __thread gpointer *store_remset_buffer;
static __thread int store_remset_buffer_index;
/* Used by the managed allocator/wbarrier */
static __thread char **tlab_next_addr;
static __thread char *stack_end;
static __thread int *store_remset_buffer_index_addr;
#endif
static char *nursery_next = NULL;
static char *nursery_frag_real_end = NULL;
static char *nursery_real_end = NULL;
//static char *nursery_first_pinned_start = NULL;
static char *nursery_last_pinned_end = NULL;

/* The size of a TLAB */
/* The bigger the value, the less often we have to go to the slow path to allocate a new 
 * one, but the more space is wasted by threads not allocating much memory.
 * FIXME: Tune this.
 * FIXME: Make this self-tuning for each thread.
 */
static guint32 tlab_size = (1024 * 4);

/* fragments that are free and ready to be used for allocation */
static Fragment *nursery_fragments = NULL;
/* freeelist of fragment structures */
static Fragment *fragment_freelist = NULL;

/* 
 * used when moving the objects
 */
static char *to_space_bumper = NULL;
static char *to_space_top = NULL;
static GCMemSection *to_space_section = NULL;

/* objects bigger then this go into the large object space */
#define MAX_SMALL_OBJ_SIZE MAX_FREELIST_SIZE

/* Functions supplied by the runtime to be called by the GC */
static MonoGCCallbacks gc_callbacks;

/*
 * ######################################################################
 * ########  Macros and function declarations.
 * ######################################################################
 */

#define UPDATE_HEAP_BOUNDARIES(low,high) do {	\
		if ((mword)(low) < lowest_heap_address)	\
			lowest_heap_address = (mword)(low);	\
		if ((mword)(high) > highest_heap_address)	\
			highest_heap_address = (mword)(high);	\
	} while (0)
#define ADDR_IN_HEAP_BOUNDARIES(addr) ((p) >= lowest_heap_address && (p) < highest_heap_address)

inline static void*
align_pointer (void *ptr)
{
	mword p = (mword)ptr;
	p += sizeof (gpointer) - 1;
	p &= ~ (sizeof (gpointer) - 1);
	return (void*)p;
}

/* forward declarations */
static void* get_internal_mem          (size_t size, int type);
static void  free_internal_mem         (void *addr, int type);
static void* get_os_memory             (size_t size, int activate);
static void  free_os_memory            (void *addr, size_t size);
static G_GNUC_UNUSED void  report_internal_mem_usage (void);

static int stop_world (void);
static int restart_world (void);
static void scan_thread_data (void *start_nursery, void *end_nursery, gboolean precise);
static void scan_from_remsets (void *start_nursery, void *end_nursery);
static void find_pinning_ref_from_thread (char *obj, size_t size);
static void update_current_thread_stack (void *start);
static GCMemSection* alloc_major_section (void);
static void finalize_in_range (char *start, char *end, int generation);
static void add_or_remove_disappearing_link (MonoObject *obj, void **link, gboolean track, int generation);
static void null_link_in_range (char *start, char *end, int generation);
static void null_links_for_domain (MonoDomain *domain, int generation);
static gboolean search_fragment_for_size (size_t size);
static void mark_pinned_from_addresses (PinnedChunk *chunk, void **start, void **end);
static void clear_remsets (void);
static void clear_tlabs (void);
static char *find_tlab_next_from_address (char *addr);
typedef void (*ScanPinnedObjectCallbackFunc) (PinnedChunk*, char*, size_t, void*);
static void scan_pinned_objects (ScanPinnedObjectCallbackFunc callback, void *callback_data);
static void sweep_pinned_objects (void);
static void scan_from_pinned_objects (char *addr_start, char *addr_end);
static void free_large_object (LOSObject *obj);
static void free_major_section (GCMemSection *section);
static void to_space_expand (void);

static void mono_gc_register_disappearing_link (MonoObject *obj, void **link, gboolean track);

void describe_ptr (char *ptr);
void check_consistency (void);
char* check_object (char *start);

void mono_gc_scan_for_specific_ref (MonoObject *key);

/*
 * ######################################################################
 * ########  GC descriptors
 * ######################################################################
 * Used to quickly get the info the GC needs about an object: size and
 * where the references are held.
 */
/* objects are aligned to 8 bytes boundaries
 * A descriptor is a pointer in MonoVTable, so 32 or 64 bits of size.
 * The low 3 bits define the type of the descriptor. The other bits
 * depend on the type.
 * As a general rule the 13 remaining low bits define the size, either
 * of the whole object or of the elements in the arrays. While for objects
 * the size is already in bytes, for arrays we need to shift, because
 * array elements might be smaller than 8 bytes. In case of arrays, we
 * use two bits to describe what the additional high bits represents,
 * so the default behaviour can handle element sizes less than 2048 bytes.
 * The high 16 bits, if 0 it means the object is pointer-free.
 * This design should make it easy and fast to skip over ptr-free data.
 * The first 4 types should cover >95% of the objects.
 * Note that since the size of objects is limited to 64K, larger objects
 * will be allocated in the large object heap.
 * If we want 4-bytes alignment, we need to put vector and small bitmap
 * inside complex.
 */
enum {
	DESC_TYPE_RUN_LENGTH,   /* 16 bits aligned byte size | 1-3 (offset, numptr) bytes tuples */
	DESC_TYPE_SMALL_BITMAP, /* 16 bits aligned byte size | 16-48 bit bitmap */
	DESC_TYPE_STRING,       /* nothing */
	DESC_TYPE_COMPLEX,      /* index for bitmap into complex_descriptors */
	DESC_TYPE_VECTOR,       /* 10 bits element size | 1 bit array | 2 bits desc | element desc */
	DESC_TYPE_ARRAY,        /* 10 bits element size | 1 bit array | 2 bits desc | element desc */
	DESC_TYPE_LARGE_BITMAP, /* | 29-61 bitmap bits */
	DESC_TYPE_COMPLEX_ARR,  /* index for bitmap into complex_descriptors */
	/* subtypes for arrays and vectors */
	DESC_TYPE_V_PTRFREE = 0,/* there are no refs: keep first so it has a zero value  */
	DESC_TYPE_V_REFS,       /* all the array elements are refs */
	DESC_TYPE_V_RUN_LEN,    /* elements are run-length encoded as DESC_TYPE_RUN_LENGTH */
	DESC_TYPE_V_BITMAP      /* elements are as the bitmap in DESC_TYPE_SMALL_BITMAP */
};

#define OBJECT_HEADER_WORDS (sizeof(MonoObject)/sizeof(gpointer))
#define LOW_TYPE_BITS 3
#define SMALL_BITMAP_SHIFT 16
#define SMALL_BITMAP_SIZE (GC_BITS_PER_WORD - SMALL_BITMAP_SHIFT)
#define VECTOR_INFO_SHIFT 14
#define VECTOR_ELSIZE_SHIFT 3
#define LARGE_BITMAP_SIZE (GC_BITS_PER_WORD - LOW_TYPE_BITS)
#define MAX_SMALL_SIZE ((1 << SMALL_BITMAP_SHIFT) - 1)
#define SMALL_SIZE_MASK 0xfff8
#define MAX_ELEMENT_SIZE 0x3ff
#define ELEMENT_SIZE_MASK (0x3ff << LOW_TYPE_BITS)
#define VECTOR_SUBTYPE_PTRFREE (DESC_TYPE_V_PTRFREE << VECTOR_INFO_SHIFT)
#define VECTOR_SUBTYPE_REFS    (DESC_TYPE_V_REFS << VECTOR_INFO_SHIFT)
#define VECTOR_SUBTYPE_RUN_LEN (DESC_TYPE_V_RUN_LEN << VECTOR_INFO_SHIFT)
#define VECTOR_SUBTYPE_BITMAP  (DESC_TYPE_V_BITMAP << VECTOR_INFO_SHIFT)

#define ALLOC_ALIGN 8


/* Root bitmap descriptors are simpler: the lower three bits describe the type
 * and we either have 30/62 bitmap bits or nibble-based run-length,
 * or a complex descriptor, or a user defined marker function.
 */
enum {
	ROOT_DESC_CONSERVATIVE, /* 0, so matches NULL value */
	ROOT_DESC_BITMAP,
	ROOT_DESC_RUN_LEN, 
	ROOT_DESC_COMPLEX,
	ROOT_DESC_USER,
	ROOT_DESC_TYPE_MASK = 0x7,
	ROOT_DESC_TYPE_SHIFT = 3,
};

#define MAKE_ROOT_DESC(type,val) ((type) | ((val) << ROOT_DESC_TYPE_SHIFT))

#define MAX_USER_DESCRIPTORS 16

static gsize* complex_descriptors = NULL;
static int complex_descriptors_size = 0;
static int complex_descriptors_next = 0;
static MonoGCMarkFunc user_descriptors [MAX_USER_DESCRIPTORS];
static int user_descriptors_next = 0;

static int
alloc_complex_descriptor (gsize *bitmap, int numbits)
{
	int nwords = numbits/GC_BITS_PER_WORD + 2;
	int res;
	int i;

	LOCK_GC;
	res = complex_descriptors_next;
	/* linear search, so we don't have duplicates with domain load/unload
	 * this should not be performance critical or we'd have bigger issues
	 * (the number and size of complex descriptors should be small).
	 */
	for (i = 0; i < complex_descriptors_next; ) {
		if (complex_descriptors [i] == nwords) {
			int j, found = TRUE;
			for (j = 0; j < nwords - 1; ++j) {
				if (complex_descriptors [i + 1 + j] != bitmap [j]) {
					found = FALSE;
					break;
				}
			}
			if (found) {
				UNLOCK_GC;
				return i;
			}
		}
		i += complex_descriptors [i];
	}
	if (complex_descriptors_next + nwords > complex_descriptors_size) {
		int new_size = complex_descriptors_size * 2 + nwords;
		complex_descriptors = g_realloc (complex_descriptors, new_size * sizeof (gsize));
		complex_descriptors_size = new_size;
	}
	DEBUG (6, fprintf (gc_debug_file, "Complex descriptor %d, size: %d (total desc memory: %d)\n", res, nwords, complex_descriptors_size));
	complex_descriptors_next += nwords;
	complex_descriptors [res] = nwords;
	for (i = 0; i < nwords - 1; ++i) {
		complex_descriptors [res + 1 + i] = bitmap [i];
		DEBUG (6, fprintf (gc_debug_file, "\tvalue: %p\n", (void*)complex_descriptors [res + 1 + i]));
	}
	UNLOCK_GC;
	return res;
}

/*
 * Descriptor builders.
 */
void*
mono_gc_make_descr_for_string (gsize *bitmap, int numbits)
{
	return (void*) DESC_TYPE_STRING;
}

void*
mono_gc_make_descr_for_object (gsize *bitmap, int numbits, size_t obj_size)
{
	int first_set = -1, num_set = 0, last_set = -1, i;
	mword desc = 0;
	size_t stored_size = obj_size;
	stored_size += ALLOC_ALIGN - 1;
	stored_size &= ~(ALLOC_ALIGN - 1);
	for (i = 0; i < numbits; ++i) {
		if (bitmap [i / GC_BITS_PER_WORD] & ((gsize)1 << (i % GC_BITS_PER_WORD))) {
			if (first_set < 0)
				first_set = i;
			last_set = i;
			num_set++;
		}
	}
	if (stored_size <= MAX_SMALL_OBJ_SIZE) {
		/* check run-length encoding first: one byte offset, one byte number of pointers
		 * on 64 bit archs, we can have 3 runs, just one on 32.
		 * It may be better to use nibbles.
		 */
		if (first_set < 0) {
			desc = DESC_TYPE_RUN_LENGTH | stored_size;
			DEBUG (6, fprintf (gc_debug_file, "Ptrfree descriptor %p, size: %zd\n", (void*)desc, stored_size));
			return (void*) desc;
		} else if (first_set < 256 && num_set < 256 && (first_set + num_set == last_set + 1)) {
			desc = DESC_TYPE_RUN_LENGTH | stored_size | (first_set << 16) | (num_set << 24);
			DEBUG (6, fprintf (gc_debug_file, "Runlen descriptor %p, size: %zd, first set: %d, num set: %d\n", (void*)desc, stored_size, first_set, num_set));
			return (void*) desc;
		}
		/* we know the 2-word header is ptr-free */
		if (last_set < SMALL_BITMAP_SIZE + OBJECT_HEADER_WORDS) {
			desc = DESC_TYPE_SMALL_BITMAP | stored_size | ((*bitmap >> OBJECT_HEADER_WORDS) << SMALL_BITMAP_SHIFT);
			DEBUG (6, fprintf (gc_debug_file, "Smallbitmap descriptor %p, size: %zd, last set: %d\n", (void*)desc, stored_size, last_set));
			return (void*) desc;
		}
	}
	/* we know the 2-word header is ptr-free */
	if (last_set < LARGE_BITMAP_SIZE + OBJECT_HEADER_WORDS) {
		desc = DESC_TYPE_LARGE_BITMAP | ((*bitmap >> OBJECT_HEADER_WORDS) << LOW_TYPE_BITS);
		DEBUG (6, fprintf (gc_debug_file, "Largebitmap descriptor %p, size: %zd, last set: %d\n", (void*)desc, stored_size, last_set));
		return (void*) desc;
	}
	/* it's a complex object ... */
	desc = DESC_TYPE_COMPLEX | (alloc_complex_descriptor (bitmap, last_set + 1) << LOW_TYPE_BITS);
	return (void*) desc;
}

/* If the array holds references, numbits == 1 and the first bit is set in elem_bitmap */
void*
mono_gc_make_descr_for_array (int vector, gsize *elem_bitmap, int numbits, size_t elem_size)
{
	int first_set = -1, num_set = 0, last_set = -1, i;
	mword desc = vector? DESC_TYPE_VECTOR: DESC_TYPE_ARRAY;
	for (i = 0; i < numbits; ++i) {
		if (elem_bitmap [i / GC_BITS_PER_WORD] & ((gsize)1 << (i % GC_BITS_PER_WORD))) {
			if (first_set < 0)
				first_set = i;
			last_set = i;
			num_set++;
		}
	}
	if (elem_size <= MAX_ELEMENT_SIZE) {
		desc |= elem_size << VECTOR_ELSIZE_SHIFT;
		if (!num_set) {
			return (void*)(desc | VECTOR_SUBTYPE_PTRFREE);
		}
		/* Note: we also handle structs with just ref fields */
		if (num_set * sizeof (gpointer) == elem_size) {
			return (void*)(desc | VECTOR_SUBTYPE_REFS | ((gssize)(-1) << 16));
		}
		/* FIXME: try run-len first */
		/* Note: we can't skip the object header here, because it's not present */
		if (last_set <= SMALL_BITMAP_SIZE) {
			return (void*)(desc | VECTOR_SUBTYPE_BITMAP | (*elem_bitmap << 16));
		}
	}
	/* it's am array of complex structs ... */
	desc = DESC_TYPE_COMPLEX_ARR;
	desc |= alloc_complex_descriptor (elem_bitmap, last_set + 1) << LOW_TYPE_BITS;
	return (void*) desc;
}

/* Return the bitmap encoded by a descriptor */
gsize*
mono_gc_get_bitmap_for_descr (void *descr, int *numbits)
{
	mword d = (mword)descr;
	gsize *bitmap;

	switch (d & 0x7) {
	case DESC_TYPE_RUN_LENGTH: {		
		int first_set = (d >> 16) & 0xff;
		int num_set = (d >> 16) & 0xff;
		int i;

		bitmap = g_new0 (gsize, (first_set + num_set + 7) / 8);

		for (i = first_set; i < first_set + num_set; ++i)
			bitmap [i / GC_BITS_PER_WORD] |= ((gsize)1 << (i % GC_BITS_PER_WORD));

		*numbits = first_set + num_set;

		return bitmap;
	}
	case DESC_TYPE_SMALL_BITMAP:
		bitmap = g_new0 (gsize, 1);

		bitmap [0] = (d >> SMALL_BITMAP_SHIFT) << OBJECT_HEADER_WORDS;

	    *numbits = GC_BITS_PER_WORD;
		
		return bitmap;
	default:
		g_assert_not_reached ();
	}
}

/* helper macros to scan and traverse objects, macros because we resue them in many functions */
#define STRING_SIZE(size,str) do {	\
		(size) = sizeof (MonoString) + 2 * (mono_string_length ((MonoString*)(str)) + 1);	\
		(size) += (ALLOC_ALIGN - 1);	\
		(size) &= ~(ALLOC_ALIGN - 1);	\
	} while (0)

#define OBJ_RUN_LEN_SIZE(size,desc,obj) do { \
        (size) = (desc) & 0xfff8; \
    } while (0)

#define OBJ_BITMAP_SIZE(size,desc,obj) do { \
        (size) = (desc) & 0xfff8; \
    } while (0)

//#define PREFETCH(addr) __asm__ __volatile__ ("     prefetchnta     %0": : "m"(*(char *)(addr)))
#define PREFETCH(addr)

/* code using these macros must define a HANDLE_PTR(ptr) macro that does the work */
#define OBJ_RUN_LEN_FOREACH_PTR(desc,obj)	do {	\
		if ((desc) & 0xffff0000) {	\
			/* there are pointers */	\
			void **_objptr_end;	\
			void **_objptr = (void**)(obj);	\
			_objptr += ((desc) >> 16) & 0xff;	\
			_objptr_end = _objptr + (((desc) >> 24) & 0xff);	\
			while (_objptr < _objptr_end) {	\
				HANDLE_PTR (_objptr, (obj));	\
				_objptr++;	\
			}	\
		}	\
	} while (0)

/* a bitmap desc means that there are pointer references or we'd have
 * choosen run-length, instead: add an assert to check.
 */
#define OBJ_BITMAP_FOREACH_PTR(desc,obj)	do {	\
		/* there are pointers */	\
		void **_objptr = (void**)(obj);	\
		gsize _bmap = (desc) >> 16;	\
		_objptr += OBJECT_HEADER_WORDS;	\
		while (_bmap) {	\
			if ((_bmap & 1)) {	\
				HANDLE_PTR (_objptr, (obj));	\
			}	\
			_bmap >>= 1;	\
			++_objptr;	\
		}	\
	} while (0)

#define OBJ_LARGE_BITMAP_FOREACH_PTR(vt,obj)	do {	\
		/* there are pointers */	\
		void **_objptr = (void**)(obj);	\
		gsize _bmap = (vt)->desc >> LOW_TYPE_BITS;	\
		_objptr += OBJECT_HEADER_WORDS;	\
		while (_bmap) {	\
			if ((_bmap & 1)) {	\
				HANDLE_PTR (_objptr, (obj));	\
			}	\
			_bmap >>= 1;	\
			++_objptr;	\
		}	\
	} while (0)

#define OBJ_COMPLEX_FOREACH_PTR(vt,obj)	do {	\
		/* there are pointers */	\
		void **_objptr = (void**)(obj);	\
		gsize *bitmap_data = complex_descriptors + ((vt)->desc >> LOW_TYPE_BITS);	\
		int bwords = (*bitmap_data) - 1;	\
		void **start_run = _objptr;	\
		bitmap_data++;	\
		if (0) {	\
			MonoObject *myobj = (MonoObject*)obj;	\
			g_print ("found %d at %p (0x%zx): %s.%s\n", bwords, (obj), (vt)->desc, myobj->vtable->klass->name_space, myobj->vtable->klass->name);	\
		}	\
		while (bwords-- > 0) {	\
			gsize _bmap = *bitmap_data++;	\
			_objptr = start_run;	\
			/*g_print ("bitmap: 0x%x/%d at %p\n", _bmap, bwords, _objptr);*/	\
			while (_bmap) {	\
				if ((_bmap & 1)) {	\
					HANDLE_PTR (_objptr, (obj));	\
				}	\
				_bmap >>= 1;	\
				++_objptr;	\
			}	\
			start_run += GC_BITS_PER_WORD;	\
		}	\
	} while (0)

/* this one is untested */
#define OBJ_COMPLEX_ARR_FOREACH_PTR(vt,obj)	do {	\
		/* there are pointers */	\
		gsize *mbitmap_data = complex_descriptors + ((vt)->desc >> LOW_TYPE_BITS);	\
		int mbwords = (*mbitmap_data++) - 1;	\
		int el_size = mono_array_element_size (((MonoObject*)(obj))->vtable->klass);	\
		char *e_start = (char*)(obj) +  G_STRUCT_OFFSET (MonoArray, vector);	\
		char *e_end = e_start + el_size * mono_array_length ((MonoArray*)(obj));	\
		if (0) {	\
			MonoObject *myobj = (MonoObject*)start;	\
			g_print ("found %d at %p (0x%zx): %s.%s\n", mbwords, (obj), (vt)->desc, myobj->vtable->klass->name_space, myobj->vtable->klass->name);	\
		}	\
		while (e_start < e_end) {	\
			void **_objptr = (void**)e_start;	\
			gsize *bitmap_data = mbitmap_data;	\
			unsigned int bwords = mbwords;	\
			while (bwords-- > 0) {	\
				gsize _bmap = *bitmap_data++;	\
				void **start_run = _objptr;	\
				/*g_print ("bitmap: 0x%x\n", _bmap);*/	\
				while (_bmap) {	\
					if ((_bmap & 1)) {	\
						HANDLE_PTR (_objptr, (obj));	\
					}	\
					_bmap >>= 1;	\
					++_objptr;	\
				}	\
				_objptr = start_run + GC_BITS_PER_WORD;	\
			}	\
			e_start += el_size;	\
		}	\
	} while (0)

#define OBJ_VECTOR_FOREACH_PTR(vt,obj)	do {	\
		/* note: 0xffffc000 excludes DESC_TYPE_V_PTRFREE */	\
		if ((vt)->desc & 0xffffc000) {	\
			int el_size = ((vt)->desc >> 3) & MAX_ELEMENT_SIZE;	\
			/* there are pointers */	\
			int etype = (vt)->desc & 0xc000;	\
			if (etype == (DESC_TYPE_V_REFS << 14)) {	\
				void **p = (void**)((char*)(obj) + G_STRUCT_OFFSET (MonoArray, vector));	\
				void **end_refs = (void**)((char*)p + el_size * mono_array_length ((MonoArray*)(obj)));	\
				/* Note: this code can handle also arrays of struct with only references in them */	\
				while (p < end_refs) {	\
					HANDLE_PTR (p, (obj));	\
					++p;	\
				}	\
			} else if (etype == DESC_TYPE_V_RUN_LEN << 14) {	\
				int offset = ((vt)->desc >> 16) & 0xff;	\
				int num_refs = ((vt)->desc >> 24) & 0xff;	\
				char *e_start = (char*)(obj) + G_STRUCT_OFFSET (MonoArray, vector);	\
				char *e_end = e_start + el_size * mono_array_length ((MonoArray*)(obj));	\
				while (e_start < e_end) {	\
					void **p = (void**)e_start;	\
					int i;	\
					p += offset;	\
					for (i = 0; i < num_refs; ++i) {	\
						HANDLE_PTR (p + i, (obj));	\
					}	\
					e_start += el_size;	\
				}	\
			} else if (etype == DESC_TYPE_V_BITMAP << 14) {	\
				char *e_start = (char*)(obj) +  G_STRUCT_OFFSET (MonoArray, vector);	\
				char *e_end = e_start + el_size * mono_array_length ((MonoArray*)(obj));	\
				while (e_start < e_end) {	\
					void **p = (void**)e_start;	\
					gsize _bmap = (vt)->desc >> 16;	\
					/* Note: there is no object header here to skip */	\
					while (_bmap) {	\
						if ((_bmap & 1)) {	\
							HANDLE_PTR (p, (obj));	\
						}	\
						_bmap >>= 1;	\
						++p;	\
					}	\
					e_start += el_size;	\
				}	\
			}	\
		}	\
	} while (0)

static mword new_obj_references = 0;
static mword obj_references_checked = 0;

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr) && (char*)*(ptr) >= nursery_start && (char*)*(ptr) < nursery_next) {	\
			new_obj_references++;	\
			/*printf ("bogus ptr %p found at %p in object %p (%s.%s)\n", *(ptr), (ptr), o, o->vtable->klass->name_space, o->vtable->klass->name);*/	\
		} else {	\
			obj_references_checked++;	\
		}	\
	} while (0)

/*
 * ######################################################################
 * ########  Detecting and removing garbage.
 * ######################################################################
 * This section of code deals with detecting the objects no longer in use
 * and reclaiming the memory.
 */

#define COUNT_OBJECT_TYPES do {						\
	switch (desc & 0x7) {						\
	case DESC_TYPE_STRING: type_str++; break;			\
	case DESC_TYPE_RUN_LENGTH: type_rlen++; break;			\
	case DESC_TYPE_ARRAY: case DESC_TYPE_VECTOR: type_vector++; break; \
	case DESC_TYPE_SMALL_BITMAP: type_bitmap++; break;		\
	case DESC_TYPE_LARGE_BITMAP: type_lbit++; break;		\
	case DESC_TYPE_COMPLEX: type_complex++; break;			\
	case DESC_TYPE_COMPLEX_ARR: type_complex++; break;		\
	default: g_assert_not_reached ();				\
	}								\
	} while (0)

static void __attribute__((noinline))
scan_area (char *start, char *end)
{
	GCVTable *vt;
	int type_str = 0, type_rlen = 0, type_bitmap = 0, type_vector = 0, type_lbit = 0, type_complex = 0;
	new_obj_references = 0;
	obj_references_checked = 0;
	while (start < end) {
		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}
		vt = (GCVTable*)LOAD_VTABLE (start);
		DEBUG (8, fprintf (gc_debug_file, "Scanning object %p, vtable: %p (%s)\n", start, vt, vt->klass->name));
		if (0) {
			MonoObject *obj = (MonoObject*)start;
			g_print ("found at %p (0x%zx): %s.%s\n", start, vt->desc, obj->vtable->klass->name_space, obj->vtable->klass->name);
		}

#define SCAN_OBJECT_ACTION COUNT_OBJECT_TYPES
#include "sgen-scan-object.h"
	}
	/*printf ("references to new nursery %p-%p (size: %dk): %d, checked: %d\n", old_start, end, (end-old_start)/1024, new_obj_references, obj_references_checked);
	printf ("\tstrings: %d, runl: %d, vector: %d, bitmaps: %d, lbitmaps: %d, complex: %d\n",
		type_str, type_rlen, type_vector, type_bitmap, type_lbit, type_complex);*/
}

static gboolean
is_xdomain_ref_allowed (gpointer *ptr, char *obj, MonoDomain *domain)
{
	MonoObject *o = (MonoObject*)(obj);
	MonoObject *ref = (MonoObject*)*(ptr);
	int offset = (char*)(ptr) - (char*)o;

	if (o->vtable->klass == mono_defaults.thread_class && offset == G_STRUCT_OFFSET (MonoThread, internal_thread))
		return TRUE;
	if (o->vtable->klass == mono_defaults.internal_thread_class && offset == G_STRUCT_OFFSET (MonoInternalThread, current_appcontext))
		return TRUE;
	if (mono_class_has_parent (o->vtable->klass, mono_defaults.real_proxy_class) &&
			offset == G_STRUCT_OFFSET (MonoRealProxy, unwrapped_server))
		return TRUE;
	/* Thread.cached_culture_info */
	if (!strcmp (ref->vtable->klass->name_space, "System.Globalization") &&
			!strcmp (ref->vtable->klass->name, "CultureInfo") &&
			!strcmp(o->vtable->klass->name_space, "System") &&
			!strcmp(o->vtable->klass->name, "Object[]"))
		return TRUE;
	/*
	 *  at System.IO.MemoryStream.InternalConstructor (byte[],int,int,bool,bool) [0x0004d] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.IO/MemoryStream.cs:121
	 * at System.IO.MemoryStream..ctor (byte[]) [0x00017] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.IO/MemoryStream.cs:81
	 * at (wrapper remoting-invoke-with-check) System.IO.MemoryStream..ctor (byte[]) <IL 0x00020, 0xffffffff>
	 * at System.Runtime.Remoting.Messaging.CADMethodCallMessage.GetArguments () [0x0000d] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Messaging/CADMessages.cs:327
	 * at System.Runtime.Remoting.Messaging.MethodCall..ctor (System.Runtime.Remoting.Messaging.CADMethodCallMessage) [0x00017] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Messaging/MethodCall.cs:87
	 * at System.AppDomain.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage,byte[]&,System.Runtime.Remoting.Messaging.CADMethodReturnMessage&) [0x00018] in /home/schani/Work/novell/trunk/mcs/class/corlib/System/AppDomain.cs:1213
	 * at (wrapper remoting-invoke-with-check) System.AppDomain.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage,byte[]&,System.Runtime.Remoting.Messaging.CADMethodReturnMessage&) <IL 0x0003d, 0xffffffff>
	 * at System.Runtime.Remoting.Channels.CrossAppDomainSink.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage) [0x00008] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Channels/CrossAppDomainChannel.cs:198
	 * at (wrapper runtime-invoke) object.runtime_invoke_CrossAppDomainSink/ProcessMessageRes_object_object (object,intptr,intptr,intptr) <IL 0x0004c, 0xffffffff>
	 */
	if (!strcmp (ref->vtable->klass->name_space, "System") &&
			!strcmp (ref->vtable->klass->name, "Byte[]") &&
			!strcmp (o->vtable->klass->name_space, "System.IO") &&
			!strcmp (o->vtable->klass->name, "MemoryStream"))
		return TRUE;
	/* append_job() in threadpool.c */
	if (!strcmp (ref->vtable->klass->name_space, "System.Runtime.Remoting.Messaging") &&
			!strcmp (ref->vtable->klass->name, "AsyncResult") &&
			!strcmp (o->vtable->klass->name_space, "System") &&
			!strcmp (o->vtable->klass->name, "Object[]") &&
			mono_thread_pool_is_queue_array ((MonoArray*) o))
		return TRUE;
	return FALSE;
}

static void
check_reference_for_xdomain (gpointer *ptr, char *obj, MonoDomain *domain)
{
	MonoObject *o = (MonoObject*)(obj);
	MonoObject *ref = (MonoObject*)*(ptr);
	int offset = (char*)(ptr) - (char*)o;
	MonoClass *class;
	MonoClassField *field;
	char *str;

	if (!ref || ref->vtable->domain == domain)
		return;
	if (is_xdomain_ref_allowed (ptr, obj, domain))
		return;

	field = NULL;
	for (class = o->vtable->klass; class; class = class->parent) {
		int i;

		for (i = 0; i < class->field.count; ++i) {
			if (class->fields[i].offset == offset) {
				field = &class->fields[i];
				break;
			}
		}
		if (field)
			break;
	}

	if (ref->vtable->klass == mono_defaults.string_class)
		str = mono_string_to_utf8 ((MonoString*)ref);
	else
		str = NULL;
	g_print ("xdomain reference in %p (%s.%s) at offset %d (%s) to %p (%s.%s) (%s)  -  pointed to by:\n",
			o, o->vtable->klass->name_space, o->vtable->klass->name,
			offset, field ? field->name : "",
			ref, ref->vtable->klass->name_space, ref->vtable->klass->name, str ? str : "");
	mono_gc_scan_for_specific_ref (o);
	if (str)
		g_free (str);
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	check_reference_for_xdomain ((ptr), (obj), domain)

static char*
scan_object_for_xdomain_refs (char *start)
{
	MonoDomain *domain = ((MonoObject*)start)->vtable->domain;

	#include "sgen-scan-object.h"

	return start;
}

static void
scan_area_for_xdomain_refs (char *start, char *end)
{
	while (start < end) {
		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}

		start = scan_object_for_xdomain_refs (start);
	}
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj) do {		\
	if ((MonoObject*)*(ptr) == key) {	\
	g_print ("found ref to %p in object %p (%s) at offset %d\n",	\
			key, (obj), safe_name ((obj)), ((char*)(ptr) - (char*)(obj))); \
	}								\
	} while (0)

static char*
scan_object_for_specific_ref (char *start, MonoObject *key)
{
	#include "sgen-scan-object.h"

	return start;
}

static void
scan_area_for_specific_ref (char *start, char *end, MonoObject *key)
{
	while (start < end) {
		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}

		start = scan_object_for_specific_ref (start, key);
	}
}

static void
scan_pinned_object_for_specific_ref_callback (PinnedChunk *chunk, char *obj, size_t size, MonoObject *key)
{
	scan_object_for_specific_ref (obj, key);
}

static void
check_root_obj_specific_ref (RootRecord *root, MonoObject *key, MonoObject *obj)
{
	if (key != obj)
		return;
	g_print ("found ref to %p in root record %p\n", key, root);
}

static MonoObject *check_key = NULL;
static RootRecord *check_root = NULL;

static void*
check_root_obj_specific_ref_from_marker (void *obj)
{
	check_root_obj_specific_ref (check_root, check_key, obj);
	return obj;
}

static void
scan_roots_for_specific_ref (MonoObject *key, int root_type)
{
	int i;
	RootRecord *root;
	check_key = key;
	for (i = 0; i < roots_hash_size [root_type]; ++i) {
		for (root = roots_hash [root_type][i]; root; root = root->next) {
			void **start_root = (void**)root->start_root;
			mword desc = root->root_desc;

			check_root = root;

			switch (desc & ROOT_DESC_TYPE_MASK) {
			case ROOT_DESC_BITMAP:
				desc >>= ROOT_DESC_TYPE_SHIFT;
				while (desc) {
					if (desc & 1)
						check_root_obj_specific_ref (root, key, *start_root);
					desc >>= 1;
					start_root++;
				}
				return;
			case ROOT_DESC_COMPLEX: {
				gsize *bitmap_data = complex_descriptors + (desc >> ROOT_DESC_TYPE_SHIFT);
				int bwords = (*bitmap_data) - 1;
				void **start_run = start_root;
				bitmap_data++;
				while (bwords-- > 0) {
					gsize bmap = *bitmap_data++;
					void **objptr = start_run;
					while (bmap) {
						if (bmap & 1)
							check_root_obj_specific_ref (root, key, *objptr);
						bmap >>= 1;
						++objptr;
					}
					start_run += GC_BITS_PER_WORD;
				}
				break;
			}
			case ROOT_DESC_USER: {
				MonoGCMarkFunc marker = user_descriptors [desc >> ROOT_DESC_TYPE_SHIFT];
				marker (start_root, check_root_obj_specific_ref_from_marker);
				break;
			}
			case ROOT_DESC_RUN_LEN:
				g_assert_not_reached ();
			default:
				g_assert_not_reached ();
			}
		}
	}
	check_key = NULL;
	check_root = NULL;
}

void
mono_gc_scan_for_specific_ref (MonoObject *key)
{
	GCMemSection *section;
	LOSObject *bigobj;
	RootRecord *root;
	int i;

	for (section = section_list; section; section = section->block.next)
		scan_area_for_specific_ref (section->data, section->end_data, key);

	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		scan_object_for_specific_ref (bigobj->data, key);

	scan_pinned_objects ((ScanPinnedObjectCallbackFunc)scan_pinned_object_for_specific_ref_callback, key);

	scan_roots_for_specific_ref (key, ROOT_TYPE_NORMAL);
	scan_roots_for_specific_ref (key, ROOT_TYPE_WBARRIER);

	for (i = 0; i < roots_hash_size [ROOT_TYPE_PINNED]; ++i) {
		for (root = roots_hash [ROOT_TYPE_PINNED][i]; root; root = root->next) {
			void **ptr = (void**)root->start_root;

			while (ptr < (void**)root->end_root) {
				check_root_obj_specific_ref (root, *ptr, key);
				++ptr;
			}
		}
	}
}

static gboolean
need_remove_object_for_domain (char *start, MonoDomain *domain)
{
	if (mono_object_domain (start) == domain) {
		DEBUG (1, fprintf (gc_debug_file, "Need to cleanup object %p\n", start));
		return TRUE;
	}
	return FALSE;
}

static void
process_object_for_domain_clearing (char *start, MonoDomain *domain)
{
	GCVTable *vt = (GCVTable*)LOAD_VTABLE (start);
	if (vt->klass == mono_defaults.internal_thread_class)
		g_assert (mono_object_domain (start) == mono_get_root_domain ());
	/* The object could be a proxy for an object in the domain
	   we're deleting. */
	if (mono_class_has_parent (vt->klass, mono_defaults.real_proxy_class)) {
		MonoObject *server = ((MonoRealProxy*)start)->unwrapped_server;

		/* The server could already have been zeroed out, so
		   we need to check for that, too. */
		if (server && (!LOAD_VTABLE (server) || mono_object_domain (server) == domain)) {
			DEBUG (1, fprintf (gc_debug_file, "Cleaning up remote pointer in %p to object %p\n",
					start, server));
			((MonoRealProxy*)start)->unwrapped_server = NULL;
		}
	}
}

static void __attribute__((noinline))
scan_area_for_domain (MonoDomain *domain, char *start, char *end)
{
	GCVTable *vt;
	gboolean remove;

	while (start < end) {
		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}
		vt = (GCVTable*)LOAD_VTABLE (start);
		process_object_for_domain_clearing (start, domain);
		remove = need_remove_object_for_domain (start, domain);
		if (remove && ((MonoObject*)start)->synchronisation) {
			void **dislink = mono_monitor_get_object_monitor_weak_link ((MonoObject*)start);
			if (dislink)
				mono_gc_register_disappearing_link (NULL, dislink, FALSE);
		}

#define SCAN_OBJECT_NOSCAN
#define SCAN_OBJECT_ACTION do {						\
			if (remove) memset (start, 0, skip_size);	\
		} while (0)
#include "sgen-scan-object.h"
	}
}

static MonoDomain *check_domain = NULL;

static void*
check_obj_not_in_domain (void *o)
{
	g_assert (((MonoObject*)o)->vtable->domain != check_domain);
	return o;
}

static void
scan_for_registered_roots_in_domain (MonoDomain *domain, int root_type)
{
	int i;
	RootRecord *root;
	check_domain = domain;
	for (i = 0; i < roots_hash_size [root_type]; ++i) {
		for (root = roots_hash [root_type][i]; root; root = root->next) {
			void **start_root = (void**)root->start_root;
			mword desc = root->root_desc;

			/* The MonoDomain struct is allowed to hold
			   references to objects in its own domain. */
			if (start_root == (void**)domain)
				continue;

			switch (desc & ROOT_DESC_TYPE_MASK) {
			case ROOT_DESC_BITMAP:
				desc >>= ROOT_DESC_TYPE_SHIFT;
				while (desc) {
					if ((desc & 1) && *start_root)
						check_obj_not_in_domain (*start_root);
					desc >>= 1;
					start_root++;
				}
				break;
			case ROOT_DESC_COMPLEX: {
				gsize *bitmap_data = complex_descriptors + (desc >> ROOT_DESC_TYPE_SHIFT);
				int bwords = (*bitmap_data) - 1;
				void **start_run = start_root;
				bitmap_data++;
				while (bwords-- > 0) {
					gsize bmap = *bitmap_data++;
					void **objptr = start_run;
					while (bmap) {
						if ((bmap & 1) && *objptr)
							check_obj_not_in_domain (*objptr);
						bmap >>= 1;
						++objptr;
					}
					start_run += GC_BITS_PER_WORD;
				}
				break;
			}
			case ROOT_DESC_USER: {
				MonoGCMarkFunc marker = user_descriptors [desc >> ROOT_DESC_TYPE_SHIFT];
				marker (start_root, check_obj_not_in_domain);
				break;
			}
			case ROOT_DESC_RUN_LEN:
				g_assert_not_reached ();
			default:
				g_assert_not_reached ();
			}
		}
	}
	check_domain = NULL;
}

static void
clear_domain_process_pinned_object_callback (PinnedChunk *chunk, char *obj, size_t size, MonoDomain *domain)
{
	process_object_for_domain_clearing (obj, domain);
}

static void
clear_domain_free_pinned_object_callback (PinnedChunk *chunk, char *obj, size_t size, MonoDomain *domain)
{
	if (need_remove_object_for_domain (obj, domain))
		free_pinned_object (chunk, obj, size);
}

static void
scan_pinned_object_for_xdomain_refs_callback (PinnedChunk *chunk, char *obj, size_t size, gpointer dummy)
{
	scan_object_for_xdomain_refs (obj);
}

static void
check_for_xdomain_refs (void)
{
	GCMemSection *section;
	LOSObject *bigobj;

	for (section = section_list; section; section = section->block.next)
		scan_area_for_xdomain_refs (section->data, section->end_data);

	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		scan_object_for_xdomain_refs (bigobj->data);

	scan_pinned_objects (scan_pinned_object_for_xdomain_refs_callback, NULL);
}

/*
 * When appdomains are unloaded we can easily remove objects that have finalizers,
 * but all the others could still be present in random places on the heap.
 * We need a sweep to get rid of them even though it's going to be costly
 * with big heaps.
 * The reason we need to remove them is because we access the vtable and class
 * structures to know the object size and the reference bitmap: once the domain is
 * unloaded the point to random memory.
 */
void
mono_gc_clear_domain (MonoDomain * domain)
{
	GCMemSection *section;
	LOSObject *bigobj, *prev;
	Fragment *frag;
	int i;

	LOCK_GC;
	/* Clear all remaining nursery fragments */
	if (nursery_clear_policy == CLEAR_AT_TLAB_CREATION) {
		g_assert (nursery_next <= nursery_frag_real_end);
		memset (nursery_next, 0, nursery_frag_real_end - nursery_next);
		for (frag = nursery_fragments; frag; frag = frag->next) {
			memset (frag->fragment_start, 0, frag->fragment_end - frag->fragment_start);
		}
	}

	if (xdomain_checks && domain != mono_get_root_domain ()) {
		scan_for_registered_roots_in_domain (domain, ROOT_TYPE_NORMAL);
		scan_for_registered_roots_in_domain (domain, ROOT_TYPE_WBARRIER);
		check_for_xdomain_refs ();
	}

	for (section = section_list; section; section = section->block.next) {
		scan_area_for_domain (domain, section->data, section->end_data);
	}

	/* We need two passes over pinned and large objects because
	   freeing such an object gives its memory back to the OS (in
	   the case of large objects) or obliterates its vtable
	   (pinned objects), but we might need to dereference a
	   pointer from an object to another object if the first
	   object is a proxy. */
	scan_pinned_objects ((ScanPinnedObjectCallbackFunc)clear_domain_process_pinned_object_callback, domain);
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		process_object_for_domain_clearing (bigobj->data, domain);

	prev = NULL;
	for (bigobj = los_object_list; bigobj;) {
		if (need_remove_object_for_domain (bigobj->data, domain)) {
			LOSObject *to_free = bigobj;
			if (prev)
				prev->next = bigobj->next;
			else
				los_object_list = bigobj->next;
			bigobj = bigobj->next;
			DEBUG (1, fprintf (gc_debug_file, "Freeing large object %p\n",
					bigobj->data));
			free_large_object (to_free);
			continue;
		}
		prev = bigobj;
		bigobj = bigobj->next;
	}
	scan_pinned_objects ((ScanPinnedObjectCallbackFunc)clear_domain_free_pinned_object_callback, domain);

	for (i = GENERATION_NURSERY; i < GENERATION_MAX; ++i)
		null_links_for_domain (domain, i);

	UNLOCK_GC;
}

/*
 * add_to_global_remset:
 *
 *   The global remset contains locations which point into newspace after
 * a minor collection. This can happen if the objects they point to are pinned.
 */
static void
add_to_global_remset (gpointer ptr, gboolean root)
{
	RememberedSet *rs;

	DEBUG (8, fprintf (gc_debug_file, "Adding global remset for %p\n", ptr));

	g_assert (!root);
	g_assert (!ptr_in_nursery (ptr) && ptr_in_nursery (*(gpointer*)ptr));

	HEAVY_STAT (++stat_global_remsets_added);

	/* 
	 * FIXME: If an object remains pinned, we need to add it at every minor collection.
	 * To avoid uncontrolled growth of the global remset, only add each pointer once.
	 */
	if (global_remset->store_next + 3 < global_remset->end_set) {
		if (root) {
			*(global_remset->store_next++) = (mword)ptr | REMSET_OTHER;
			*(global_remset->store_next++) = (mword)REMSET_ROOT_LOCATION;
		} else {
			*(global_remset->store_next++) = (mword)ptr;
		}
		return;
	}
	rs = alloc_remset (global_remset->end_set - global_remset->data, NULL);
	rs->next = global_remset;
	global_remset = rs;
	if (root) {
		*(global_remset->store_next++) = (mword)ptr | REMSET_OTHER;
		*(global_remset->store_next++) = (mword)REMSET_ROOT_LOCATION;
	} else {
		*(global_remset->store_next++) = (mword)ptr;
	}

	{
		int global_rs_size = 0;

		for (rs = global_remset; rs; rs = rs->next) {
			global_rs_size += rs->store_next - rs->data;
		}
		DEBUG (4, fprintf (gc_debug_file, "Global remset now has size %d\n", global_rs_size));
	}
}

#include "sgen-gray.c"

/*
 * This is how the copying happens from the nursery to the old generation.
 * We assume that at this time all the pinned objects have been identified and
 * marked as such.
 * We run scan_object() for each pinned object so that each referenced
 * objects if possible are copied. The new gray objects created can have
 * scan_object() run on them right away, too.
 * Then we run copy_object() for the precisely tracked roots. At this point
 * all the roots are either gray or black. We run scan_object() on the gray
 * objects until no more gray objects are created.
 * At the end of the process we walk again the pinned list and we unmark
 * the pinned flag. As we go we also create the list of free space for use
 * in the next allocation runs.
 *
 * We need to remember objects from the old generation that point to the new one
 * (or just addresses?).
 *
 * copy_object could be made into a macro once debugged (use inline for now).
 */

static char* __attribute__((noinline))
copy_object (char *obj, char *from_space_start, char *from_space_end)
{
	static void *copy_labels [] = { &&LAB_0, &&LAB_1, &&LAB_2, &&LAB_3, &&LAB_4, &&LAB_5, &&LAB_6, &&LAB_7, &&LAB_8 };

	char *forwarded;
	mword objsize;
	MonoVTable *vt;

	HEAVY_STAT (++num_copy_object_called);

	if (!(obj >= from_space_start && obj < from_space_end)) {
		DEBUG (9, fprintf (gc_debug_file, "Not copying %p because it's not in from space (%p-%p)\n",
						obj, from_space_start, from_space_end));
		HEAVY_STAT (++stat_copy_object_failed_from_space);
		return obj;
	}

	DEBUG (9, fprintf (gc_debug_file, "Precise copy of %p", obj));

	/*
	 * obj must belong to one of:
	 *
	 * 1. the nursery
	 * 2. the LOS
	 * 3. a pinned chunk
	 * 4. a non-to-space section of the major heap
	 * 5. a to-space section of the major heap
	 *
	 * In addition, objects in 1, 2 and 4 might also be pinned.
	 * Objects in 1 and 4 might be forwarded.
	 *
	 * Before we can copy the object we must make sure that we are
	 * allowed to, i.e. that the object not pinned, not already
	 * forwarded and doesn't belong to the LOS, a pinned chunk, or
	 * a to-space section.
	 *
	 * We are usually called for to-space objects (5) when we have
	 * two remset entries for the same reference.  The first entry
	 * copies the object and updates the reference and the second
	 * calls us with the updated reference that points into
	 * to-space.  There might also be other circumstances where we
	 * get to-space objects.
	 */

	if ((forwarded = object_is_forwarded (obj))) {
		g_assert (((MonoVTable*)LOAD_VTABLE(obj))->gc_descr);
		DEBUG (9, fprintf (gc_debug_file, " (already forwarded to %p)\n", forwarded));
		HEAVY_STAT (++stat_copy_object_failed_forwarded);
		return forwarded;
	}
	if (object_is_pinned (obj)) {
		g_assert (((MonoVTable*)LOAD_VTABLE(obj))->gc_descr);
		DEBUG (9, fprintf (gc_debug_file, " (pinned, no change)\n"));
		HEAVY_STAT (++stat_copy_object_failed_pinned);
		return obj;
	}

	objsize = safe_object_get_size ((MonoObject*)obj);
	objsize += ALLOC_ALIGN - 1;
	objsize &= ~(ALLOC_ALIGN - 1);

	if (ptr_in_nursery (obj))
		goto copy;

	/*
	 * At this point we know obj is not pinned, not forwarded and
	 * belongs to 2, 3, 4, or 5.
	 *
	 * LOS object (2) are simple, at least until we always follow
	 * the rule: if objsize > MAX_SMALL_OBJ_SIZE, pin the object
	 * and return it.  At the end of major collections, we walk
	 * the los list and if the object is pinned, it is marked,
	 * otherwise it can be freed.
	 *
	 * Pinned chunks (3) and major heap sections (4, 5) both
	 * reside in blocks, which are always aligned, so once we've
	 * eliminated LOS objects, we can just access the block and
	 * see whether it's a pinned chunk or a major heap section.
	 */
	if (G_UNLIKELY (objsize > MAX_SMALL_OBJ_SIZE || obj_is_from_pinned_alloc (obj))) {
		DEBUG (9, fprintf (gc_debug_file, " (marked LOS/Pinned %p (%s), size: %zd)\n", obj, safe_name (obj), objsize));
		pin_object (obj);
		HEAVY_STAT (++stat_copy_object_failed_large_pinned);
		return obj;
	}

	/*
	 * Now we know the object is in a major heap section.  All we
	 * need to do is check whether it's already in to-space (5) or
	 * not (4).
	 */
	if (MAJOR_SECTION_FOR_OBJECT (obj)->is_to_space) {
		g_assert (objsize <= MAX_SMALL_OBJ_SIZE);
		DEBUG (9, fprintf (gc_debug_file, " (already copied)\n"));
		HEAVY_STAT (++stat_copy_object_failed_to_space);
		return obj;
	}

 copy:
	DEBUG (9, fprintf (gc_debug_file, " (to %p, %s size: %zd)\n", to_space_bumper, ((MonoObject*)obj)->vtable->klass->name, objsize));

	HEAVY_STAT (++num_objects_copied);

	/* Make sure we have enough space available */
	if (to_space_bumper + objsize > to_space_top) {
		to_space_expand ();
		g_assert (to_space_bumper + objsize <= to_space_top);
	}

	if (objsize <= sizeof (gpointer) * 8) {
		mword *dest = (mword*)to_space_bumper;
		goto *copy_labels [objsize / sizeof (gpointer)];
	LAB_8:
		(dest) [7] = ((mword*)obj) [7];
	LAB_7:
		(dest) [6] = ((mword*)obj) [6];
	LAB_6:
		(dest) [5] = ((mword*)obj) [5];
	LAB_5:
		(dest) [4] = ((mword*)obj) [4];
	LAB_4:
		(dest) [3] = ((mword*)obj) [3];
	LAB_3:
		(dest) [2] = ((mword*)obj) [2];
	LAB_2:
		(dest) [1] = ((mword*)obj) [1];
	LAB_1:
		(dest) [0] = ((mword*)obj) [0];
	LAB_0:
		;
	} else {
#if 0
		{
			int ecx;
			char* esi = obj;
			char* edi = to_space_bumper;
			__asm__ __volatile__(
				"rep; movsl"
				: "=&c" (ecx), "=&D" (edi), "=&S" (esi)
				: "0" (objsize/4), "1" (edi),"2" (esi)
				: "memory"
					     );
		}
#else
		memcpy (to_space_bumper, obj, objsize);
#endif
	}
	/* adjust array->bounds */
	vt = ((MonoObject*)obj)->vtable;
	g_assert (vt->gc_descr);
	if (G_UNLIKELY (vt->rank && ((MonoArray*)obj)->bounds)) {
		MonoArray *array = (MonoArray*)to_space_bumper;
		array->bounds = (MonoArrayBounds*)((char*)to_space_bumper + ((char*)((MonoArray*)obj)->bounds - (char*)obj));
		DEBUG (9, fprintf (gc_debug_file, "Array instance %p: size: %zd, rank: %d, length: %d\n", array, objsize, vt->rank, mono_array_length (array)));
	}
	/* set the forwarding pointer */
	forward_object (obj, to_space_bumper);
	obj = to_space_bumper;
	to_space_section->scan_starts [((char*)obj - (char*)to_space_section->data)/SCAN_START_SIZE] = obj;
	to_space_bumper += objsize;
	DEBUG (9, fprintf (gc_debug_file, "Enqueuing gray object %p (%s)\n", obj, safe_name (obj)));
	gray_object_enqueue (obj);
	DEBUG (8, g_assert (to_space_bumper <= to_space_top));
	return obj;
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		void *__old = *(ptr);	\
		void *__copy;		\
		if (__old) {	\
			*(ptr) = __copy = copy_object (__old, from_start, from_end);	\
			DEBUG (9, if (__old != __copy) fprintf (gc_debug_file, "Overwrote field at %p with %p (was: %p)\n", (ptr), *(ptr), __old));	\
			if (G_UNLIKELY (ptr_in_nursery (__copy) && !ptr_in_nursery ((ptr)))) \
				add_to_global_remset ((ptr), FALSE);							\
		}	\
	} while (0)

/*
 * Scan the object pointed to by @start for references to
 * other objects between @from_start and @from_end and copy
 * them to the gray_objects area.
 * Returns a pointer to the end of the object.
 */
static char*
scan_object (char *start, char* from_start, char* from_end)
{
#include "sgen-scan-object.h"

	return start;
}

/*
 * drain_gray_stack:
 *
 *   Scan objects in the gray stack until the stack is empty. This should be called
 * frequently after each object is copied, to achieve better locality and cache
 * usage.
 */
static void inline
drain_gray_stack (char *start_addr, char *end_addr)
{
	char *obj;

	while ((obj = gray_object_dequeue ())) {
		DEBUG (9, fprintf (gc_debug_file, "Precise gray object scan %p (%s)\n", obj, safe_name (obj)));
		scan_object (obj, start_addr, end_addr);
	}
}

/*
 * scan_vtype:
 *
 * Scan the valuetype pointed to by START, described by DESC for references to
 * other objects between @from_start and @from_end and copy them to the gray_objects area.
 * Returns a pointer to the end of the object.
 */
static char*
scan_vtype (char *start, mword desc, char* from_start, char* from_end)
{
	size_t skip_size;

	/* The descriptors include info about the MonoObject header as well */
	start -= sizeof (MonoObject);

	switch (desc & 0x7) {
	case DESC_TYPE_RUN_LENGTH:
		OBJ_RUN_LEN_FOREACH_PTR (desc,start);
		OBJ_RUN_LEN_SIZE (skip_size, desc, start);
		g_assert (skip_size);
		return start + skip_size;
	case DESC_TYPE_SMALL_BITMAP:
		OBJ_BITMAP_FOREACH_PTR (desc,start);
		OBJ_BITMAP_SIZE (skip_size, desc, start);
		return start + skip_size;
	case DESC_TYPE_LARGE_BITMAP:
	case DESC_TYPE_COMPLEX:
		// FIXME:
		g_assert_not_reached ();
		break;
	default:
		// The other descriptors can't happen with vtypes
		g_assert_not_reached ();
		break;
	}
	return NULL;
}

#include "sgen-pinning-stats.c"

/*
 * Addresses from start to end are already sorted. This function finds
 * the object header for each address and pins the object. The
 * addresses must be inside the passed section.  The (start of the)
 * address array is overwritten with the addresses of the actually
 * pinned objects.  Return the number of pinned objects.
 */
static int
pin_objects_from_addresses (GCMemSection *section, void **start, void **end, void *start_nursery, void *end_nursery)
{
	void *last = NULL;
	int count = 0;
	void *search_start;
	void *last_obj = NULL;
	size_t last_obj_size = 0;
	void *addr;
	int idx;
	void **definitely_pinned = start;
	while (start < end) {
		addr = *start;
		/* the range check should be reduntant */
		if (addr != last && addr >= start_nursery && addr < end_nursery) {
			DEBUG (5, fprintf (gc_debug_file, "Considering pinning addr %p\n", addr));
			/* multiple pointers to the same object */
			if (addr >= last_obj && (char*)addr < (char*)last_obj + last_obj_size) {
				start++;
				continue;
			}
			idx = ((char*)addr - (char*)section->data) / SCAN_START_SIZE;
			search_start = (void*)section->scan_starts [idx];
			if (!search_start || search_start > addr) {
				while (idx) {
					--idx;
					search_start = section->scan_starts [idx];
					if (search_start && search_start <= addr)
						break;
				}
				if (!search_start || search_start > addr)
					search_start = start_nursery;
			}
			if (search_start < last_obj)
				search_start = (char*)last_obj + last_obj_size;
			/* now addr should be in an object a short distance from search_start
			 * Note that search_start must point to zeroed mem or point to an object.
			 */
			do {
				if (!*(void**)search_start) {
					mword p = (mword)search_start;
					p += sizeof (gpointer);
					p += ALLOC_ALIGN - 1;
					p &= ~(ALLOC_ALIGN - 1);
					search_start = (void*)p;
					continue;
				}
				last_obj = search_start;
				last_obj_size = safe_object_get_size ((MonoObject*)search_start);
				last_obj_size += ALLOC_ALIGN - 1;
				last_obj_size &= ~(ALLOC_ALIGN - 1);
				DEBUG (8, fprintf (gc_debug_file, "Pinned try match %p (%s), size %zd\n", last_obj, safe_name (last_obj), last_obj_size));
				if (addr >= search_start && (char*)addr < (char*)last_obj + last_obj_size) {
					DEBUG (4, fprintf (gc_debug_file, "Pinned object %p, vtable %p (%s), count %d\n", search_start, *(void**)search_start, safe_name (search_start), count));
					pin_object (search_start);
					if (heap_dump_file)
						pin_stats_register_object (search_start, last_obj_size);
					definitely_pinned [count] = search_start;
					count++;
					break;
				}
				/* skip to the next object */
				search_start = (void*)((char*)search_start + last_obj_size);
			} while (search_start <= addr);
			/* we either pinned the correct object or we ignored the addr because
			 * it points to unused zeroed memory.
			 */
			last = addr;
		}
		start++;
	}
	//printf ("effective pinned: %d (at the end: %d)\n", count, (char*)end_nursery - (char*)last);
	return count;
}

static void** pin_queue;
static int pin_queue_size = 0;
static int next_pin_slot = 0;

static int
new_gap (int gap)
{
	gap = (gap * 10) / 13;
	if (gap == 9 || gap == 10)
		return 11;
	if (gap < 1)
		return 1;
	return gap;
}

#if 0
static int
compare_addr (const void *a, const void *b)
{
	return *(const void **)a - *(const void **)b;
}
#endif

/* sort the addresses in array in increasing order */
static void
sort_addresses (void **array, int size)
{
	/*
	 * qsort is slower as predicted.
	 * qsort (array, size, sizeof (gpointer), compare_addr);
	 * return;
	 */
	int gap = size;
	int swapped, end;
	while (TRUE) {
		int i;
		gap = new_gap (gap);
		swapped = FALSE;
		end = size - gap;
		for (i = 0; i < end; i++) {
			int j = i + gap;
			if (array [i] > array [j]) {
				void* val = array [i];
				array [i] = array [j];
				array [j] = val;
				swapped = TRUE;
			}
		}
		if (gap == 1 && !swapped)
			break;
	}
}

static G_GNUC_UNUSED void
print_nursery_gaps (void* start_nursery, void *end_nursery)
{
	int i;
	gpointer first = start_nursery;
	gpointer next;
	for (i = 0; i < next_pin_slot; ++i) {
		next = pin_queue [i];
		fprintf (gc_debug_file, "Nursery range: %p-%p, size: %zd\n", first, next, (char*)next-(char*)first);
		first = next;
	}
	next = end_nursery;
	fprintf (gc_debug_file, "Nursery range: %p-%p, size: %zd\n", first, next, (char*)next-(char*)first);
}

/* reduce the info in the pin queue, removing duplicate pointers and sorting them */
static void
optimize_pin_queue (int start_slot)
{
	void **start, **cur, **end;
	/* sort and uniq pin_queue: we just sort and we let the rest discard multiple values */
	/* it may be better to keep ranges of pinned memory instead of individually pinning objects */
	DEBUG (5, fprintf (gc_debug_file, "Sorting pin queue, size: %d\n", next_pin_slot));
	if ((next_pin_slot - start_slot) > 1)
		sort_addresses (pin_queue + start_slot, next_pin_slot - start_slot);
	start = cur = pin_queue + start_slot;
	end = pin_queue + next_pin_slot;
	while (cur < end) {
		*start = *cur++;
		while (*start == *cur && cur < end)
			cur++;
		start++;
	};
	next_pin_slot = start - pin_queue;
	DEBUG (5, fprintf (gc_debug_file, "Pin queue reduced to size: %d\n", next_pin_slot));
	//DEBUG (6, print_nursery_gaps (start_nursery, end_nursery));
	
}

static int
optimized_pin_queue_search (void *addr)
{
	int first = 0, last = next_pin_slot;
	while (first < last) {
		int middle = first + ((last - first) >> 1);
		if (addr <= pin_queue [middle])
			last = middle;
		else
			first = middle + 1;
	}
	g_assert (first == last);
	return first;
}

static void
find_optimized_pin_queue_area (void *start, void *end, int *first, int *last)
{
	*first = optimized_pin_queue_search (start);
	*last = optimized_pin_queue_search (end);
}

static void
realloc_pin_queue (void)
{
	int new_size = pin_queue_size? pin_queue_size + pin_queue_size/2: 1024;
	void **new_pin = get_internal_mem (sizeof (void*) * new_size, INTERNAL_MEM_PIN_QUEUE);
	memcpy (new_pin, pin_queue, sizeof (void*) * next_pin_slot);
	free_internal_mem (pin_queue, INTERNAL_MEM_PIN_QUEUE);
	pin_queue = new_pin;
	pin_queue_size = new_size;
	DEBUG (4, fprintf (gc_debug_file, "Reallocated pin queue to size: %d\n", new_size));
}

#include "sgen-pinning.c"

/* 
 * Scan the memory between start and end and queue values which could be pointers
 * to the area between start_nursery and end_nursery for later consideration.
 * Typically used for thread stacks.
 */
static void
conservatively_pin_objects_from (void **start, void **end, void *start_nursery, void *end_nursery, int pin_type)
{
	int count = 0;
	while (start < end) {
		if (*start >= start_nursery && *start < end_nursery) {
			/*
			 * *start can point to the middle of an object
			 * note: should we handle pointing at the end of an object?
			 * pinning in C# code disallows pointing at the end of an object
			 * but there is some small chance that an optimizing C compiler
			 * may keep the only reference to an object by pointing
			 * at the end of it. We ignore this small chance for now.
			 * Pointers to the end of an object are indistinguishable
			 * from pointers to the start of the next object in memory
			 * so if we allow that we'd need to pin two objects...
			 * We queue the pointer in an array, the
			 * array will then be sorted and uniqued. This way
			 * we can coalesce several pinning pointers and it should
			 * be faster since we'd do a memory scan with increasing
			 * addresses. Note: we can align the address to the allocation
			 * alignment, so the unique process is more effective.
			 */
			mword addr = (mword)*start;
			addr &= ~(ALLOC_ALIGN - 1);
			if (addr >= (mword)start_nursery && addr < (mword)end_nursery)
				pin_stage_ptr ((void*)addr);
			if (heap_dump_file)
				pin_stats_register_address ((char*)addr, pin_type);
			DEBUG (6, if (count) fprintf (gc_debug_file, "Pinning address %p\n", (void*)addr));
			count++;
		}
		start++;
	}
	DEBUG (7, if (count) fprintf (gc_debug_file, "found %d potential pinned heap pointers\n", count));
}

/* 
 * If generation is 0, just mark objects in the nursery, the others we don't care,
 * since they are not going to move anyway.
 * There are different areas that are scanned for pinned pointers:
 * *) the thread stacks (when jit support is ready only the unmanaged frames)
 * *) the pinned handle table
 * *) the pinned roots
 *
 * Note: when we'll use a write barrier for old to new gen references, we need to
 * keep track of old gen objects that point to pinned new gen objects because in that
 * case the referenced object will be moved maybe at the next collection, but there
 * is no write in the old generation area where the pinned object is referenced
 * and we may not consider it as reachable.
 */
static G_GNUC_UNUSED void
mark_pinned_objects (int generation)
{
}

/*
 * Debugging function: find in the conservative roots where @obj is being pinned.
 */
static G_GNUC_UNUSED void
find_pinning_reference (char *obj, size_t size)
{
	RootRecord *root;
	int i;
	char *endobj = obj + size;
	for (i = 0; i < roots_hash_size [0]; ++i) {
		for (root = roots_hash [0][i]; root; root = root->next) {
			/* if desc is non-null it has precise info */
			if (!root->root_desc) {
				char ** start = (char**)root->start_root;
				while (start < (char**)root->end_root) {
					if (*start >= obj && *start < endobj) {
						DEBUG (0, fprintf (gc_debug_file, "Object %p referenced in pinned roots %p-%p (at %p in record %p)\n", obj, root->start_root, root->end_root, start, root));
					}
					start++;
				}
			}
		}
	}
	find_pinning_ref_from_thread (obj, size);
}

/*
 * The first thing we do in a collection is to identify pinned objects.
 * This function considers all the areas of memory that need to be
 * conservatively scanned.
 */
static void
pin_from_roots (void *start_nursery, void *end_nursery)
{
	RootRecord *root;
	int i;
	DEBUG (2, fprintf (gc_debug_file, "Scanning pinned roots (%d bytes, %d/%d entries)\n", (int)roots_size, num_roots_entries [ROOT_TYPE_NORMAL], num_roots_entries [ROOT_TYPE_PINNED]));
	/* objects pinned from the API are inside these roots */
	for (i = 0; i < roots_hash_size [ROOT_TYPE_PINNED]; ++i) {
		for (root = roots_hash [ROOT_TYPE_PINNED][i]; root; root = root->next) {
			DEBUG (6, fprintf (gc_debug_file, "Pinned roots %p-%p\n", root->start_root, root->end_root));
			conservatively_pin_objects_from ((void**)root->start_root, (void**)root->end_root, start_nursery, end_nursery, PIN_TYPE_OTHER);
		}
	}
	/* now deal with the thread stacks
	 * in the future we should be able to conservatively scan only:
	 * *) the cpu registers
	 * *) the unmanaged stack frames
	 * *) the _last_ managed stack frame
	 * *) pointers slots in managed frames
	 */
	scan_thread_data (start_nursery, end_nursery, FALSE);

	evacuate_pin_staging_area ();
}

/* Copy function called from user defined mark functions */
static char *user_copy_n_start;
static char *user_copy_n_end;

static void*
user_copy (void *addr)
{
	if (addr)
		return copy_object (addr, user_copy_n_start, user_copy_n_end);
	else
		return NULL;
}

/*
 * The memory area from start_root to end_root contains pointers to objects.
 * Their position is precisely described by @desc (this means that the pointer
 * can be either NULL or the pointer to the start of an object).
 * This functions copies them to to_space updates them.
 */
static void
precisely_scan_objects_from (void** start_root, void** end_root, char* n_start, char *n_end, mword desc)
{
	switch (desc & ROOT_DESC_TYPE_MASK) {
	case ROOT_DESC_BITMAP:
		desc >>= ROOT_DESC_TYPE_SHIFT;
		while (desc) {
			if ((desc & 1) && *start_root) {
				*start_root = copy_object (*start_root, n_start, n_end);
				DEBUG (9, fprintf (gc_debug_file, "Overwrote root at %p with %p\n", start_root, *start_root));
				drain_gray_stack (n_start, n_end);
			}
			desc >>= 1;
			start_root++;
		}
		return;
	case ROOT_DESC_COMPLEX: {
		gsize *bitmap_data = complex_descriptors + (desc >> ROOT_DESC_TYPE_SHIFT);
		int bwords = (*bitmap_data) - 1;
		void **start_run = start_root;
		bitmap_data++;
		while (bwords-- > 0) {
			gsize bmap = *bitmap_data++;
			void **objptr = start_run;
			while (bmap) {
				if ((bmap & 1) && *objptr) {
					*objptr = copy_object (*objptr, n_start, n_end);
					DEBUG (9, fprintf (gc_debug_file, "Overwrote root at %p with %p\n", objptr, *objptr));
					drain_gray_stack (n_start, n_end);
				}
				bmap >>= 1;
				++objptr;
			}
			start_run += GC_BITS_PER_WORD;
		}
		break;
	}
	case ROOT_DESC_USER: {
		MonoGCMarkFunc marker = user_descriptors [desc >> ROOT_DESC_TYPE_SHIFT];

		user_copy_n_start = n_start;
		user_copy_n_end = n_end;
		marker (start_root, user_copy);
		break;
	}
	case ROOT_DESC_RUN_LEN:
		g_assert_not_reached ();
	default:
		g_assert_not_reached ();
	}
}

static Fragment*
alloc_fragment (void)
{
	Fragment *frag = fragment_freelist;
	if (frag) {
		fragment_freelist = frag->next;
		frag->next = NULL;
		return frag;
	}
	frag = get_internal_mem (sizeof (Fragment), INTERNAL_MEM_FRAGMENT);
	frag->next = NULL;
	return frag;
}

/* size must be a power of 2 */
static void*
get_os_memory_aligned (mword size, gboolean activate)
{
	/* Allocate twice the memory to be able to put the block on an aligned address */
	char *mem = get_os_memory (size * 2, activate);
	char *aligned;

	g_assert (mem);

	aligned = (char*)((mword)(mem + (size - 1)) & ~(size - 1));
	g_assert (aligned >= mem && aligned + size <= mem + size * 2 && !((mword)aligned & (size - 1)));

	if (aligned > mem)
		free_os_memory (mem, aligned - mem);
	if (aligned + size < mem + size * 2)
		free_os_memory (aligned + size, (mem + size * 2) - (aligned + size));

	return aligned;
}

/*
 * Allocate and setup the data structures needed to be able to allocate objects
 * in the nursery. The nursery is stored in nursery_section.
 */
static void
alloc_nursery (void)
{
	GCMemSection *section;
	char *data;
	int scan_starts;
	Fragment *frag;
	int alloc_size;

	if (nursery_section)
		return;
	DEBUG (2, fprintf (gc_debug_file, "Allocating nursery size: %zd\n", nursery_size));
	/* later we will alloc a larger area for the nursery but only activate
	 * what we need. The rest will be used as expansion if we have too many pinned
	 * objects in the existing nursery.
	 */
	/* FIXME: handle OOM */
	section = get_internal_mem (SIZEOF_GC_MEM_SECTION, INTERNAL_MEM_SECTION);

	g_assert (nursery_size == DEFAULT_NURSERY_SIZE);
	alloc_size = nursery_size;
#ifdef ALIGN_NURSERY
	data = get_os_memory_aligned (alloc_size, TRUE);
#else
	data = get_os_memory (alloc_size, TRUE);
#endif
	nursery_start = data;
	nursery_real_end = nursery_start + nursery_size;
	UPDATE_HEAP_BOUNDARIES (nursery_start, nursery_real_end);
	nursery_next = nursery_start;
	total_alloc += alloc_size;
	DEBUG (4, fprintf (gc_debug_file, "Expanding nursery size (%p-%p): %zd, total: %zd\n", data, data + alloc_size, nursery_size, total_alloc));
	section->data = section->next_data = data;
	section->size = alloc_size;
	section->end_data = nursery_real_end;
	scan_starts = alloc_size / SCAN_START_SIZE;
	section->scan_starts = get_internal_mem (sizeof (char*) * scan_starts, INTERNAL_MEM_SCAN_STARTS);
	section->num_scan_start = scan_starts;
	section->block.role = MEMORY_ROLE_GEN0;

	/* add to the section list */
	section->block.next = section_list;
	section_list = section;

	nursery_section = section;

	/* Setup the single first large fragment */
	frag = alloc_fragment ();
	frag->fragment_start = nursery_start;
	frag->fragment_limit = nursery_start;
	frag->fragment_end = nursery_real_end;
	nursery_frag_real_end = nursery_real_end;
	/* FIXME: frag here is lost */
}

static void
scan_finalizer_entries (FinalizeEntry *list, char *start, char *end) {
	FinalizeEntry *fin;

	for (fin = list; fin; fin = fin->next) {
		if (!fin->object)
			continue;
		DEBUG (5, fprintf (gc_debug_file, "Scan of fin ready object: %p (%s)\n", fin->object, safe_name (fin->object)));
		fin->object = copy_object (fin->object, start, end);
	}
}

/*
 * Update roots in the old generation. Since we currently don't have the
 * info from the write barriers, we just scan all the objects.
 */
static G_GNUC_UNUSED void
scan_old_generation (char *start, char* end)
{
	GCMemSection *section;
	LOSObject *big_object;
	char *p;

	for (section = section_list; section; section = section->block.next) {
		if (section == nursery_section)
			continue;
		DEBUG (2, fprintf (gc_debug_file, "Scan of old section: %p-%p, size: %d\n", section->data, section->next_data, (int)(section->next_data - section->data)));
		/* we have to deal with zeroed holes in old generation (truncated strings ...) */
		p = section->data;
		while (p < section->next_data) {
			if (!*(void**)p) {
				p += ALLOC_ALIGN;
				continue;
			}
			DEBUG (8, fprintf (gc_debug_file, "Precise old object scan of %p (%s)\n", p, safe_name (p)));
			p = scan_object (p, start, end);
		}
	}
	/* scan the old object space, too */
	for (big_object = los_object_list; big_object; big_object = big_object->next) {
		DEBUG (5, fprintf (gc_debug_file, "Scan of big object: %p (%s), size: %zd\n", big_object->data, safe_name (big_object->data), big_object->size));
		scan_object (big_object->data, start, end);
	}
	/* scan the list of objects ready for finalization */
	scan_finalizer_entries (fin_ready_list, start, end);
	scan_finalizer_entries (critical_fin_list, start, end);
}

static mword fragment_total = 0;
/*
 * We found a fragment of free memory in the nursery: memzero it and if
 * it is big enough, add it to the list of fragments that can be used for
 * allocation.
 */
static void
add_nursery_frag (size_t frag_size, char* frag_start, char* frag_end)
{
	Fragment *fragment;
	DEBUG (4, fprintf (gc_debug_file, "Found empty fragment: %p-%p, size: %zd\n", frag_start, frag_end, frag_size));
	/* memsetting just the first chunk start is bound to provide better cache locality */
	if (nursery_clear_policy == CLEAR_AT_GC)
		memset (frag_start, 0, frag_size);
	/* Not worth dealing with smaller fragments: need to tune */
	if (frag_size >= FRAGMENT_MIN_SIZE) {
		fragment = alloc_fragment ();
		fragment->fragment_start = frag_start;
		fragment->fragment_limit = frag_start;
		fragment->fragment_end = frag_end;
		fragment->next = nursery_fragments;
		nursery_fragments = fragment;
		fragment_total += frag_size;
	} else {
		/* Clear unused fragments, pinning depends on this */
		memset (frag_start, 0, frag_size);
	}
}

static int
scan_needed_big_objects (char *start_addr, char *end_addr)
{
	LOSObject *big_object;
	int count = 0;
	for (big_object = los_object_list; big_object; big_object = big_object->next) {
		if (!big_object->scanned && object_is_pinned (big_object->data)) {
			DEBUG (5, fprintf (gc_debug_file, "Scan of big object: %p (%s), size: %zd\n", big_object->data, safe_name (big_object->data), big_object->size));
			scan_object (big_object->data, start_addr, end_addr);
			big_object->scanned = TRUE;
			count++;
		}
	}
	return count;
}

static const char*
generation_name (int generation)
{
	switch (generation) {
	case GENERATION_NURSERY: return "nursery";
	case GENERATION_OLD: return "old";
	default: g_assert_not_reached ();
	}
}

static DisappearingLinkHashTable*
get_dislink_hash_table (int generation)
{
	switch (generation) {
	case GENERATION_NURSERY: return &minor_disappearing_link_hash;
	case GENERATION_OLD: return &major_disappearing_link_hash;
	default: g_assert_not_reached ();
	}
}

static FinalizeEntryHashTable*
get_finalize_entry_hash_table (int generation)
{
	switch (generation) {
	case GENERATION_NURSERY: return &minor_finalizable_hash;
	case GENERATION_OLD: return &major_finalizable_hash;
	default: g_assert_not_reached ();
	}
}

static void
new_to_space_section (void)
{
	/* FIXME: if the current to_space_section is empty, we don't
	   have to allocate a new one */

	to_space_section = alloc_major_section ();
	to_space_bumper = to_space_section->next_data;
	to_space_top = to_space_section->end_data;
}

static void
to_space_expand (void)
{
	if (to_space_section) {
		g_assert (to_space_top == to_space_section->end_data);
		g_assert (to_space_bumper >= to_space_section->next_data && to_space_bumper <= to_space_top);

		to_space_section->next_data = to_space_bumper;
	}

	new_to_space_section ();
}

static void
to_space_set_next_data (void)
{
	g_assert (to_space_bumper >= to_space_section->next_data && to_space_bumper <= to_space_section->end_data);
	to_space_section->next_data = to_space_bumper;
}

static void
unset_to_space (void)
{
	/* between collections the to_space_bumper is invalidated
	   because degraded allocations might occur, so we set it to
	   NULL, just to make it explicit */
	to_space_bumper = NULL;

	/* don't unset to_space_section if we implement the FIXME in
	   new_to_space_section */
	to_space_section = NULL;
}

static gboolean
object_is_in_to_space (char *obj)
{
	mword objsize;

	/* nursery */
	if (ptr_in_nursery (obj))
		return FALSE;

	objsize = safe_object_get_size ((MonoObject*)obj);
	objsize += ALLOC_ALIGN - 1;
	objsize &= ~(ALLOC_ALIGN - 1);

	/* LOS */
	if (objsize > MAX_SMALL_OBJ_SIZE)
		return FALSE;

	/* pinned chunk */
	if (obj_is_from_pinned_alloc (obj))
		return FALSE;

	/* now we know it's in a major heap section */
	return MAJOR_SECTION_FOR_OBJECT (obj)->is_to_space;
}

static void
finish_gray_stack (char *start_addr, char *end_addr, int generation)
{
	TV_DECLARE (atv);
	TV_DECLARE (btv);
	int fin_ready, bigo_scanned_num;

	/*
	 * We copied all the reachable objects. Now it's the time to copy
	 * the objects that were not referenced by the roots, but by the copied objects.
	 * we built a stack of objects pointed to by gray_start: they are
	 * additional roots and we may add more items as we go.
	 * We loop until gray_start == gray_objects which means no more objects have
	 * been added. Note this is iterative: no recursion is involved.
	 * We need to walk the LO list as well in search of marked big objects
	 * (use a flag since this is needed only on major collections). We need to loop
	 * here as well, so keep a counter of marked LO (increasing it in copy_object).
	 *   To achieve better cache locality and cache usage, we drain the gray stack 
	 * frequently, after each object is copied, and just finish the work here.
	 */
	drain_gray_stack (start_addr, end_addr);
	TV_GETTIME (atv);
	//scan_old_generation (start_addr, end_addr);
	DEBUG (2, fprintf (gc_debug_file, "%s generation done\n", generation_name (generation)));
	/* walk the finalization queue and move also the objects that need to be
	 * finalized: use the finalized objects as new roots so the objects they depend
	 * on are also not reclaimed. As with the roots above, only objects in the nursery
	 * are marked/copied.
	 * We need a loop here, since objects ready for finalizers may reference other objects
	 * that are fin-ready. Speedup with a flag?
	 */
	do {
		fin_ready = num_ready_finalizers;
		finalize_in_range (start_addr, end_addr, generation);
		if (generation == GENERATION_OLD)
			finalize_in_range (nursery_start, nursery_real_end, GENERATION_NURSERY);
		bigo_scanned_num = scan_needed_big_objects (start_addr, end_addr);

		/* drain the new stack that might have been created */
		DEBUG (6, fprintf (gc_debug_file, "Precise scan of gray area post fin\n"));
		drain_gray_stack (start_addr, end_addr);
	} while (fin_ready != num_ready_finalizers || bigo_scanned_num);
	TV_GETTIME (btv);
	DEBUG (2, fprintf (gc_debug_file, "Finalize queue handling scan for %s generation: %d usecs\n", generation_name (generation), TV_ELAPSED (atv, btv)));

	/*
	 * handle disappearing links
	 * Note we do this after checking the finalization queue because if an object
	 * survives (at least long enough to be finalized) we don't clear the link.
	 * This also deals with a possible issue with the monitor reclamation: with the Boehm
	 * GC a finalized object my lose the monitor because it is cleared before the finalizer is
	 * called.
	 */
	g_assert (gray_object_queue_is_empty ());
	for (;;) {
		null_link_in_range (start_addr, end_addr, generation);
		if (generation == GENERATION_OLD)
			null_link_in_range (start_addr, end_addr, GENERATION_NURSERY);
		if (gray_object_queue_is_empty ())
			break;
		drain_gray_stack (start_addr, end_addr);
	}

	g_assert (gray_object_queue_is_empty ());
	/* DEBUG (2, fprintf (gc_debug_file, "Copied from %s to old space: %d bytes (%p-%p)\n", generation_name (generation), (int)(to_space_bumper - to_space), to_space, to_space_bumper)); */
	to_space_set_next_data ();
}

static int last_num_pinned = 0;

static void
build_nursery_fragments (int start_pin, int end_pin)
{
	char *frag_start, *frag_end;
	size_t frag_size;
	int i;

	while (nursery_fragments) {
		Fragment *next = nursery_fragments->next;
		nursery_fragments->next = fragment_freelist;
		fragment_freelist = nursery_fragments;
		nursery_fragments = next;
	}
	frag_start = nursery_start;
	fragment_total = 0;
	/* clear scan starts */
	memset (nursery_section->scan_starts, 0, nursery_section->num_scan_start * sizeof (gpointer));
	for (i = start_pin; i < end_pin; ++i) {
		frag_end = pin_queue [i];
		/* remove the pin bit from pinned objects */
		unpin_object (frag_end);
		nursery_section->scan_starts [((char*)frag_end - (char*)nursery_section->data)/SCAN_START_SIZE] = frag_end;
		frag_size = frag_end - frag_start;
		if (frag_size)
			add_nursery_frag (frag_size, frag_start, frag_end);
		frag_size = safe_object_get_size ((MonoObject*)pin_queue [i]);
		frag_size += ALLOC_ALIGN - 1;
		frag_size &= ~(ALLOC_ALIGN - 1);
		frag_start = (char*)pin_queue [i] + frag_size;
		/* 
		 * pin_queue [i] might point to a half-constructed string or vector whose
		 * length field is not set. In that case, frag_start points inside the 
		 * (zero initialized) object. Find the end of the object by scanning forward.
		 * 
		 */
		if (is_maybe_half_constructed (pin_queue [i])) {
			char *tlab_end;

			/* This is also hit for zero length arrays/strings */

			/* Find the end of the TLAB which contained this allocation */
			tlab_end = find_tlab_next_from_address (pin_queue [i]);

			if (tlab_end) {
				while ((frag_start < tlab_end) && *(mword*)frag_start == 0)
					frag_start += sizeof (mword);
			} else {
				/*
				 * FIXME: The object is either not allocated in a TLAB, or it isn't a
				 * half constructed object.
				 */
			}
		}
	}
	nursery_last_pinned_end = frag_start;
	frag_end = nursery_real_end;
	frag_size = frag_end - frag_start;
	if (frag_size)
		add_nursery_frag (frag_size, frag_start, frag_end);
	if (!nursery_fragments) {
		DEBUG (1, fprintf (gc_debug_file, "Nursery fully pinned (%d)\n", end_pin - start_pin));
		for (i = start_pin; i < end_pin; ++i) {
			DEBUG (3, fprintf (gc_debug_file, "Bastard pinning obj %p (%s), size: %d\n", pin_queue [i], safe_name (pin_queue [i]), safe_object_get_size (pin_queue [i])));
		}
		degraded_mode = 1;
	}

	nursery_next = nursery_frag_real_end = NULL;

	/* Clear TLABs for all threads */
	clear_tlabs ();
}

/* FIXME: later reduce code duplication here with the above
 * We don't keep track of section fragments for non-nursery sections yet, so
 * just memset to 0.
 */
static void
build_section_fragments (GCMemSection *section)
{
	int i;
	char *frag_start, *frag_end;
	size_t frag_size;

	/* clear scan starts */
	memset (section->scan_starts, 0, section->num_scan_start * sizeof (gpointer));
	frag_start = section->data;
	section->next_data = section->data;
	for (i = section->pin_queue_start; i < section->pin_queue_end; ++i) {
		frag_end = pin_queue [i];
		/* remove the pin bit from pinned objects */
		unpin_object (frag_end);
		if (frag_end >= section->data + section->size) {
			frag_end = section->data + section->size;
		} else {
			section->scan_starts [((char*)frag_end - (char*)section->data)/SCAN_START_SIZE] = frag_end;
		}
		frag_size = frag_end - frag_start;
		if (frag_size)
			memset (frag_start, 0, frag_size);
		frag_size = safe_object_get_size ((MonoObject*)pin_queue [i]);
		frag_size += ALLOC_ALIGN - 1;
		frag_size &= ~(ALLOC_ALIGN - 1);
		frag_start = (char*)pin_queue [i] + frag_size;
		section->next_data = MAX (section->next_data, frag_start);
	}
	frag_end = section->end_data;
	frag_size = frag_end - frag_start;
	if (frag_size)
		memset (frag_start, 0, frag_size);
}

static void
scan_from_registered_roots (char *addr_start, char *addr_end, int root_type)
{
	int i;
	RootRecord *root;
	for (i = 0; i < roots_hash_size [root_type]; ++i) {
		for (root = roots_hash [root_type][i]; root; root = root->next) {
			DEBUG (6, fprintf (gc_debug_file, "Precise root scan %p-%p (desc: %p)\n", root->start_root, root->end_root, (void*)root->root_desc));
			precisely_scan_objects_from ((void**)root->start_root, (void**)root->end_root, addr_start, addr_end, root->root_desc);
		}
	}
}

static void
dump_occupied (char *start, char *end, char *section_start)
{
	fprintf (heap_dump_file, "<occupied offset=\"%d\" size=\"%d\"/>\n", start - section_start, end - start);
}

static void
dump_section (GCMemSection *section, const char *type)
{
	char *start = section->data;
	char *end = section->data + section->size;
	char *occ_start = NULL;
	int pin_slot = 0;
	GCVTable *vt;
	char *old_start = NULL;	/* just for debugging */

	fprintf (heap_dump_file, "<section type=\"%s\" size=\"%d\">\n", type, section->size);

	while (start < end) {
		guint size;
		MonoClass *class;

		if (!*(void**)start) {
			if (occ_start) {
				dump_occupied (occ_start, start, section->data);
				occ_start = NULL;
			}
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}
		g_assert (start < section->next_data);

		if (!occ_start)
			occ_start = start;

		vt = (GCVTable*)LOAD_VTABLE (start);
		class = vt->klass;

		size = safe_object_get_size ((MonoObject*) start);
		size += ALLOC_ALIGN - 1;
		size &= ~(ALLOC_ALIGN - 1);

		/*
		fprintf (heap_dump_file, "<object offset=\"%d\" class=\"%s.%s\" size=\"%d\"/>\n",
				start - section->data,
				vt->klass->name_space, vt->klass->name,
				size);
		*/

		old_start = start;
		start += size;
	}
	if (occ_start)
		dump_occupied (occ_start, start, section->data);

	fprintf (heap_dump_file, "</section>\n");
}

static void
dump_heap (const char *type, int num, const char *reason)
{
	static char *internal_mem_names [] = { "pin-queue", "fragment", "section", "scan-starts",
					       "fin-table", "finalize-entry", "dislink-table",
					       "dislink", "roots-table", "root-record", "statistics",
					       "remset", "gray-queue", "store-remset" };

	GCMemSection *section;
	LOSObject *bigobj;
	int i;

	fprintf (heap_dump_file, "<collection type=\"%s\" num=\"%d\"", type, num);
	if (reason)
		fprintf (heap_dump_file, " reason=\"%s\"", reason);
	fprintf (heap_dump_file, ">\n");
	fprintf (heap_dump_file, "<other-mem-usage type=\"pinned-chunks\" size=\"%ld\"/>\n", pinned_chunk_bytes_alloced);
	fprintf (heap_dump_file, "<other-mem-usage type=\"large-internal\" size=\"%ld\"/>\n", large_internal_bytes_alloced);
	fprintf (heap_dump_file, "<other-mem-usage type=\"mempools\" size=\"%ld\"/>\n", mono_mempool_get_bytes_allocated ());
	for (i = 0; i < INTERNAL_MEM_MAX; ++i)
		fprintf (heap_dump_file, "<other-mem-usage type=\"%s\" size=\"%ld\"/>\n", internal_mem_names [i], small_internal_mem_bytes [i]);
	fprintf (heap_dump_file, "<pinned type=\"stack\" bytes=\"%d\"/>\n", pinned_byte_counts [PIN_TYPE_STACK]);
	/* fprintf (heap_dump_file, "<pinned type=\"static-data\" bytes=\"%d\"/>\n", pinned_byte_counts [PIN_TYPE_STATIC_DATA]); */
	fprintf (heap_dump_file, "<pinned type=\"other\" bytes=\"%d\"/>\n", pinned_byte_counts [PIN_TYPE_OTHER]);

	dump_section (nursery_section, "nursery");

	for (section = section_list; section; section = section->block.next) {
		if (section != nursery_section)
			dump_section (section, "old");
	}

	fprintf (heap_dump_file, "<los>\n");
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		MonoObject *obj = (MonoObject*) bigobj->data;
		MonoClass *class = mono_object_class (obj);

		fprintf (heap_dump_file, "<object class=\"%s.%s\" size=\"%d\"/>\n",
				class->name_space, class->name,
				safe_object_get_size (obj));
	}
	fprintf (heap_dump_file, "</los>\n");

	fprintf (heap_dump_file, "</collection>\n");
}

static void
init_stats (void)
{
	static gboolean inited = FALSE;

#ifdef HEAVY_STATISTICS
	num_copy_object_called = 0;
	num_objects_copied = 0;
#endif

	if (inited)
		return;

#ifdef HEAVY_STATISTICS
	mono_counters_register ("WBarrier set field", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_field);
	mono_counters_register ("WBarrier set arrayref", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_arrayref);
	mono_counters_register ("WBarrier arrayref copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_arrayref_copy);
	mono_counters_register ("WBarrier generic store called", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_generic_store);
	mono_counters_register ("WBarrier generic store stored", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_generic_store_remset);
	mono_counters_register ("WBarrier set root", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_set_root);
	mono_counters_register ("WBarrier value copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_value_copy);
	mono_counters_register ("WBarrier object copy", MONO_COUNTER_GC | MONO_COUNTER_INT, &stat_wbarrier_object_copy);

	mono_counters_register ("# objects allocated", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_objects_alloced);
	mono_counters_register ("# copy_object() called (nursery)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_called_nursery);
	mono_counters_register ("# objects copied (nursery)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_objects_copied_nursery);
	mono_counters_register ("# copy_object() called (major)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_called_major);
	mono_counters_register ("# objects copied (major)", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_objects_copied_major);

	mono_counters_register ("# copy_object() failed from space", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_failed_from_space);
	mono_counters_register ("# copy_object() failed forwarded", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_failed_forwarded);
	mono_counters_register ("# copy_object() failed pinned", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_failed_pinned);
	mono_counters_register ("# copy_object() failed large or pinned chunk", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_failed_large_pinned);
	mono_counters_register ("# copy_object() failed to space", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_copy_object_failed_to_space);

	mono_counters_register ("Store remsets", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_store_remsets);
	mono_counters_register ("Unique store remsets", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_store_remsets_unique);
	mono_counters_register ("Saved remsets 1", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_saved_remsets_1);
	mono_counters_register ("Saved remsets 2", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_saved_remsets_2);
	mono_counters_register ("Global remsets added", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_global_remsets_added);
	mono_counters_register ("Global remsets processed", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_global_remsets_processed);
#endif

	inited = TRUE;
}

static void
commit_stats (int generation)
{
#ifdef HEAVY_STATISTICS
	if (generation == GENERATION_NURSERY) {
		stat_copy_object_called_nursery += num_copy_object_called;
		stat_objects_copied_nursery += num_objects_copied;
	} else {
		g_assert (generation == GENERATION_OLD);
		stat_copy_object_called_major += num_copy_object_called;
		stat_objects_copied_major += num_objects_copied;
	}
#endif
}

/*
 * Collect objects in the nursery.  Returns whether to trigger a major
 * collection.
 */
static gboolean
collect_nursery (size_t requested_size)
{
	size_t max_garbage_amount;
	int i;
	char *orig_nursery_next;
	Fragment *frag;
	GCMemSection *section;
	TV_DECLARE (all_atv);
	TV_DECLARE (all_btv);
	TV_DECLARE (atv);
	TV_DECLARE (btv);

	init_stats ();

	degraded_mode = 0;
	orig_nursery_next = nursery_next;
	nursery_next = MAX (nursery_next, nursery_last_pinned_end);
	/* FIXME: optimize later to use the higher address where an object can be present */
	nursery_next = MAX (nursery_next, nursery_real_end);

	if (consistency_check_at_minor_collection)
		check_consistency ();

	DEBUG (1, fprintf (gc_debug_file, "Start nursery collection %d %p-%p, size: %d\n", num_minor_gcs, nursery_start, nursery_next, (int)(nursery_next - nursery_start)));
	max_garbage_amount = nursery_next - nursery_start;
	g_assert (nursery_section->size >= max_garbage_amount);

	/* Clear all remaining nursery fragments, pinning depends on this */
	if (nursery_clear_policy == CLEAR_AT_TLAB_CREATION) {
		g_assert (orig_nursery_next <= nursery_frag_real_end);
		memset (orig_nursery_next, 0, nursery_frag_real_end - orig_nursery_next);
		for (frag = nursery_fragments; frag; frag = frag->next) {
			memset (frag->fragment_start, 0, frag->fragment_end - frag->fragment_start);
		}
	}

	if (xdomain_checks)
		check_for_xdomain_refs ();

	nursery_section->next_data = nursery_next;

	sections_alloced = 0;

	new_to_space_section ();
	gray_object_queue_init ();

	num_minor_gcs++;
	mono_stats.minor_gc_count ++;
	/* world must be stopped already */
	TV_GETTIME (all_atv);
	TV_GETTIME (atv);
	/* pin from pinned handles */
	init_pinning ();
	pin_from_roots (nursery_start, nursery_next);
	/* identify pinned objects */
	optimize_pin_queue (0);
	next_pin_slot = pin_objects_from_addresses (nursery_section, pin_queue, pin_queue + next_pin_slot, nursery_start, nursery_next);
	TV_GETTIME (btv);
	DEBUG (2, fprintf (gc_debug_file, "Finding pinned pointers: %d in %d usecs\n", next_pin_slot, TV_ELAPSED (atv, btv)));
	DEBUG (4, fprintf (gc_debug_file, "Start scan with %d pinned objects\n", next_pin_slot));

	/* 
	 * walk all the roots and copy the young objects to the old generation,
	 * starting from to_space
	 */

	scan_from_remsets (nursery_start, nursery_next);
	/* we don't have complete write barrier yet, so we scan all the old generation sections */
	TV_GETTIME (atv);
	DEBUG (2, fprintf (gc_debug_file, "Old generation scan: %d usecs\n", TV_ELAPSED (btv, atv)));

	/* the pinned objects are roots */
	for (i = 0; i < next_pin_slot; ++i) {
		DEBUG (6, fprintf (gc_debug_file, "Precise object scan %d of pinned %p (%s)\n", i, pin_queue [i], safe_name (pin_queue [i])));
		scan_object (pin_queue [i], nursery_start, nursery_next);
	}
	/* registered roots, this includes static fields */
	scan_from_registered_roots (nursery_start, nursery_next, ROOT_TYPE_NORMAL);
	scan_from_registered_roots (nursery_start, nursery_next, ROOT_TYPE_WBARRIER);
	scan_thread_data (nursery_start, nursery_next, TRUE);
	/* alloc_pinned objects */
	scan_from_pinned_objects (nursery_start, nursery_next);
	TV_GETTIME (btv);
	DEBUG (2, fprintf (gc_debug_file, "Root scan: %d usecs\n", TV_ELAPSED (atv, btv)));

	finish_gray_stack (nursery_start, nursery_next, GENERATION_NURSERY);

	unset_to_space ();

	/* walk the pin_queue, build up the fragment list of free memory, unmark
	 * pinned objects as we go, memzero() the empty fragments so they are ready for the
	 * next allocations.
	 */
	build_nursery_fragments (0, next_pin_slot);
	TV_GETTIME (atv);
	DEBUG (2, fprintf (gc_debug_file, "Fragment creation: %d usecs, %zd bytes available\n", TV_ELAPSED (btv, atv), fragment_total));

	for (section = section_list; section; section = section->block.next) {
		if (section->is_to_space)
			section->is_to_space = FALSE;
	}

	TV_GETTIME (all_btv);
	mono_stats.minor_gc_time_usecs += TV_ELAPSED (all_atv, all_btv);

	if (heap_dump_file)
		dump_heap ("minor", num_minor_gcs - 1, NULL);

	/* prepare the pin queue for the next collection */
	last_num_pinned = next_pin_slot;
	next_pin_slot = 0;
	if (fin_ready_list || critical_fin_list) {
		DEBUG (4, fprintf (gc_debug_file, "Finalizer-thread wakeup: ready %d\n", num_ready_finalizers));
		mono_gc_finalize_notify ();
	}
	pin_stats_reset ();

	g_assert (gray_object_queue_is_empty ());

	commit_stats (GENERATION_NURSERY);

	minor_collection_sections_alloced += sections_alloced;

	return minor_collection_sections_alloced > minor_collection_section_allowance;
}

static int
count_major_sections (void)
{
	GCMemSection *section;
	int count = 0;

	for (section = section_list; section; section = section->block.next)
		if (section != nursery_section)
			++count;
	return count;
}

static void
scan_from_pinned_chunk_if_marked (PinnedChunk *chunk, char *obj, void *dummy)
{
	if (object_is_pinned (obj))
		scan_object (obj, NULL, (char*)-1);
}

static void
major_collection (const char *reason)
{
	GCMemSection *section, *prev_section;
	LOSObject *bigobj, *prevbo;
	int i;
	PinnedChunk *chunk;
	Fragment *frag;
	int count;
	TV_DECLARE (all_atv);
	TV_DECLARE (all_btv);
	TV_DECLARE (atv);
	TV_DECLARE (btv);
	/* FIXME: only use these values for the precise scan
	 * note that to_space pointers should be excluded anyway...
	 */
	char *heap_start = NULL;
	char *heap_end = (char*)-1;
	size_t copy_space_required = 0;

	init_stats ();

	degraded_mode = 0;
	DEBUG (1, fprintf (gc_debug_file, "Start major collection %d\n", num_major_gcs));
	num_major_gcs++;
	mono_stats.major_gc_count ++;

	/* Clear all remaining nursery fragments, pinning depends on this */
	if (nursery_clear_policy == CLEAR_AT_TLAB_CREATION) {
		g_assert (nursery_next <= nursery_frag_real_end);
		memset (nursery_next, 0, nursery_frag_real_end - nursery_next);
		for (frag = nursery_fragments; frag; frag = frag->next) {
			memset (frag->fragment_start, 0, frag->fragment_end - frag->fragment_start);
		}
	}

	if (xdomain_checks)
		check_for_xdomain_refs ();

	/* 
	 * FIXME: implement Mark/Compact
	 * Until that is done, we can just apply mostly the same alg as for the nursery:
	 * this means we need a big section to potentially copy all the other sections, so
	 * it is not ideal specially with large heaps.
	 */
	if (g_getenv ("MONO_GC_NO_MAJOR")) {
		collect_nursery (0);
		return;
	}
	TV_GETTIME (all_atv);
	/* FIXME: make sure the nursery next_data ptr is updated */
	nursery_section->next_data = nursery_real_end;
	/* we should also coalesce scanning from sections close to each other
	 * and deal with pointers outside of the sections later.
	 */
	/* The remsets are not useful for a major collection */
	clear_remsets ();
	/* world must be stopped already */
	TV_GETTIME (atv);
	init_pinning ();
	DEBUG (6, fprintf (gc_debug_file, "Collecting pinned addresses\n"));
	pin_from_roots (lowest_heap_address, highest_heap_address);
	optimize_pin_queue (0);

	/*
	 * pin_queue now contains all candidate pointers, sorted and
	 * uniqued.  We must do two passes now to figure out which
	 * objects are pinned.
	 *
	 * The first is to find within the pin_queue the area for each
	 * section.  This requires that the pin_queue be sorted.  We
	 * also process the LOS objects and pinned chunks here.
	 *
	 * The second, destructive, pass is to reduce the section
	 * areas to pointers to the actually pinned objects.
	 */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from sections\n"));
	/* first pass for the sections */
	for (section = section_list; section; section = section->block.next) {
		int start, end;
		DEBUG (6, fprintf (gc_debug_file, "Pinning from section %p (%p-%p)\n", section, section->data, section->end_data));
		find_optimized_pin_queue_area (section->data, section->end_data, &start, &end);
		DEBUG (6, fprintf (gc_debug_file, "Found %d pinning addresses in section %p (%d-%d)\n",
						end - start, section, start, end));
		section->pin_queue_start = start;
		section->pin_queue_end = end;
	}
	/* identify possible pointers to the insize of large objects */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from large objects\n"));
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		int start, end;
		find_optimized_pin_queue_area (bigobj->data, (char*)bigobj->data + bigobj->size, &start, &end);
		if (start != end) {
			pin_object (bigobj->data);
			if (heap_dump_file)
				pin_stats_register_object ((char*) bigobj->data, safe_object_get_size (bigobj->data));
			DEBUG (6, fprintf (gc_debug_file, "Marked large object %p (%s) size: %zd from roots\n", bigobj->data, safe_name (bigobj->data), bigobj->size));
		}
	}
	/* look for pinned addresses for pinned-alloc objects */
	DEBUG (6, fprintf (gc_debug_file, "Pinning from pinned-alloc objects\n"));
	for (chunk = pinned_chunk_list; chunk; chunk = chunk->block.next) {
		int start, end;
		find_optimized_pin_queue_area (chunk->start_data, (char*)chunk + chunk->num_pages * FREELIST_PAGESIZE, &start, &end);
		if (start != end)
			mark_pinned_from_addresses (chunk, pin_queue + start, pin_queue + end);
	}
	/* second pass for the sections */
	for (section = section_list; section; section = section->block.next) {
		int start = section->pin_queue_start;
		int end = section->pin_queue_end;
		if (start != end) {
			int reduced_to;
			reduced_to = pin_objects_from_addresses (section, pin_queue + start, pin_queue + end,
					section->data, section->next_data);
			section->pin_queue_start = start;
			section->pin_queue_end = start + reduced_to;
		}
		copy_space_required += (char*)section->next_data - (char*)section->data;
	}

	TV_GETTIME (btv);
	DEBUG (2, fprintf (gc_debug_file, "Finding pinned pointers: %d in %d usecs\n", next_pin_slot, TV_ELAPSED (atv, btv)));
	DEBUG (4, fprintf (gc_debug_file, "Start scan with %d pinned objects\n", next_pin_slot));

	new_to_space_section ();
	gray_object_queue_init ();

	/* the old generation doesn't need to be scanned (no remembered sets or card
	 * table needed either): the only objects that must survive are those pinned and
	 * those referenced by the precise roots.
	 * mark any section without pinned objects, so we can free it since we will be able to
	 * move all the objects.
	 */
	/* the pinned objects are roots (big objects are included in this list, too) */
	for (section = section_list; section; section = section->block.next) {
		for (i = section->pin_queue_start; i < section->pin_queue_end; ++i) {
			DEBUG (6, fprintf (gc_debug_file, "Precise object scan %d of pinned %p (%s)\n",
							i, pin_queue [i], safe_name (pin_queue [i])));
			scan_object (pin_queue [i], heap_start, heap_end);
		}
	}
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		if (object_is_pinned (bigobj->data)) {
			DEBUG (6, fprintf (gc_debug_file, "Precise object scan pinned LOS object %p (%s)\n",
							bigobj->data, safe_name (bigobj->data)));
			scan_object (bigobj->data, heap_start, heap_end);
		}
	}
	scan_pinned_objects (scan_from_pinned_chunk_if_marked, NULL);
	/* registered roots, this includes static fields */
	scan_from_registered_roots (heap_start, heap_end, ROOT_TYPE_NORMAL);
	scan_from_registered_roots (heap_start, heap_end, ROOT_TYPE_WBARRIER);
	/* Threads */
	scan_thread_data (heap_start, heap_end, TRUE);
	/* alloc_pinned objects */
	scan_from_pinned_objects (heap_start, heap_end);
	/* scan the list of objects ready for finalization */
	scan_finalizer_entries (fin_ready_list, heap_start, heap_end);
	scan_finalizer_entries (critical_fin_list, heap_start, heap_end);
	TV_GETTIME (atv);
	DEBUG (2, fprintf (gc_debug_file, "Root scan: %d usecs\n", TV_ELAPSED (btv, atv)));

	/* we need to go over the big object list to see if any was marked and scan it
	 * And we need to make this in a loop, considering that objects referenced by finalizable
	 * objects could reference big objects (this happens in finish_gray_stack ())
	 */
	scan_needed_big_objects (heap_start, heap_end);
	/* all the objects in the heap */
	finish_gray_stack (heap_start, heap_end, GENERATION_OLD);

	unset_to_space ();

	/* sweep the big objects list */
	prevbo = NULL;
	for (bigobj = los_object_list; bigobj;) {
		if (object_is_pinned (bigobj->data)) {
			unpin_object (bigobj->data);
			bigobj->scanned = FALSE;
		} else {
			LOSObject *to_free;
			/* not referenced anywhere, so we can free it */
			if (prevbo)
				prevbo->next = bigobj->next;
			else
				los_object_list = bigobj->next;
			to_free = bigobj;
			bigobj = bigobj->next;
			free_large_object (to_free);
			continue;
		}
		prevbo = bigobj;
		bigobj = bigobj->next;
	}
	/* unpin objects from the pinned chunks and free the unmarked ones */
	sweep_pinned_objects ();

	/* free the unused sections */
	prev_section = NULL;
	for (section = section_list; section;) {
		/* to_space doesn't need handling here and the nursery is special */
		if (section->is_to_space || section == nursery_section) {
			if (section->is_to_space)
				section->is_to_space = FALSE;
			prev_section = section;
			section = section->block.next;
			continue;
		}
		/* no pinning object, so the section is free */
		if (section->pin_queue_start == section->pin_queue_end) {
			GCMemSection *to_free;
			if (prev_section)
				prev_section->block.next = section->block.next;
			else
				section_list = section->block.next;
			to_free = section;
			section = section->block.next;
			free_major_section (to_free);
			continue;
		} else {
			DEBUG (6, fprintf (gc_debug_file, "Section %p has still pinned objects (%d)\n", section, section->pin_queue_end - section->pin_queue_start));
			build_section_fragments (section);
		}
		prev_section = section;
		section = section->block.next;
	}

	/* walk the pin_queue, build up the fragment list of free memory, unmark
	 * pinned objects as we go, memzero() the empty fragments so they are ready for the
	 * next allocations.
	 */
	build_nursery_fragments (nursery_section->pin_queue_start, nursery_section->pin_queue_end);

	TV_GETTIME (all_btv);
	mono_stats.major_gc_time_usecs += TV_ELAPSED (all_atv, all_btv);

	if (heap_dump_file)
		dump_heap ("major", num_major_gcs - 1, reason);

	/* prepare the pin queue for the next collection */
	next_pin_slot = 0;
	if (fin_ready_list || critical_fin_list) {
		DEBUG (4, fprintf (gc_debug_file, "Finalizer-thread wakeup: ready %d\n", num_ready_finalizers));
		mono_gc_finalize_notify ();
	}
	pin_stats_reset ();

	g_assert (gray_object_queue_is_empty ());

	commit_stats (GENERATION_OLD);

	minor_collection_sections_alloced = 0;
	minor_collection_section_allowance = MAX (DEFAULT_MINOR_COLLECTION_SECTION_ALLOWANCE, count_major_sections () / 3);
}

/*
 * Allocate a new section of memory to be used as old generation.
 */
static GCMemSection*
alloc_major_section (void)
{
	GCMemSection *section;
	int scan_starts;

	section = get_os_memory_aligned (MAJOR_SECTION_SIZE, TRUE);
	section->next_data = section->data = (char*)section + SIZEOF_GC_MEM_SECTION;
	g_assert (!((mword)section->data & 7));
	section->size = MAJOR_SECTION_SIZE - SIZEOF_GC_MEM_SECTION;
	section->end_data = section->data + section->size;
	UPDATE_HEAP_BOUNDARIES (section->data, section->end_data);
	total_alloc += section->size;
	DEBUG (3, fprintf (gc_debug_file, "New major heap section: (%p-%p), total: %zd\n", section->data, section->end_data, total_alloc));
	scan_starts = section->size / SCAN_START_SIZE;
	section->scan_starts = get_internal_mem (sizeof (char*) * scan_starts, INTERNAL_MEM_SCAN_STARTS);
	section->num_scan_start = scan_starts;
	section->block.role = MEMORY_ROLE_GEN1;
	section->is_to_space = TRUE;

	/* add to the section list */
	section->block.next = section_list;
	section_list = section;

	++sections_alloced;

	return section;
}

static void
free_major_section (GCMemSection *section)
{
	DEBUG (3, fprintf (gc_debug_file, "Freed major section %p (%p-%p)\n", section, section->data, section->end_data));
	free_internal_mem (section->scan_starts, INTERNAL_MEM_SCAN_STARTS);
	free_os_memory (section, MAJOR_SECTION_SIZE);
	total_alloc -= MAJOR_SECTION_SIZE - SIZEOF_GC_MEM_SECTION;
}

/*
 * When deciding if it's better to collect or to expand, keep track
 * of how much garbage was reclaimed with the last collection: if it's too
 * little, expand.
 * This is called when we could not allocate a small object.
 */
static void __attribute__((noinline))
minor_collect_or_expand_inner (size_t size)
{
	int do_minor_collection = 1;

	if (!nursery_section) {
		alloc_nursery ();
		return;
	}
	if (do_minor_collection) {
		stop_world ();
		if (collect_nursery (size))
			major_collection ("minor overflow");
		DEBUG (2, fprintf (gc_debug_file, "Heap size: %zd, LOS size: %zd\n", total_alloc, los_memory_usage));
		restart_world ();
		/* this also sets the proper pointers for the next allocation */
		if (!search_fragment_for_size (size)) {
			int i;
			/* TypeBuilder and MonoMethod are killing mcs with fragmentation */
			DEBUG (1, fprintf (gc_debug_file, "nursery collection didn't find enough room for %zd alloc (%d pinned)\n", size, last_num_pinned));
			for (i = 0; i < last_num_pinned; ++i) {
				DEBUG (3, fprintf (gc_debug_file, "Bastard pinning obj %p (%s), size: %d\n", pin_queue [i], safe_name (pin_queue [i]), safe_object_get_size (pin_queue [i])));
			}
			degraded_mode = 1;
		}
	}
	//report_internal_mem_usage ();
}

/*
 * ######################################################################
 * ########  Memory allocation from the OS
 * ######################################################################
 * This section of code deals with getting memory from the OS and
 * allocating memory for GC-internal data structures.
 * Internal memory can be handled with a freelist for small objects.
 */

/*
 * Allocate a big chunk of memory from the OS (usually 64KB to several megabytes).
 * This must not require any lock.
 */
static void*
get_os_memory (size_t size, int activate)
{
	void *ptr;
	unsigned long prot_flags = activate? MONO_MMAP_READ|MONO_MMAP_WRITE: MONO_MMAP_NONE;

	prot_flags |= MONO_MMAP_PRIVATE | MONO_MMAP_ANON;
	size += pagesize - 1;
	size &= ~(pagesize - 1);
	ptr = mono_valloc (0, size, prot_flags);
	return ptr;
}

/*
 * Free the memory returned by get_os_memory (), returning it to the OS.
 */
static void
free_os_memory (void *addr, size_t size)
{
	mono_vfree (addr, size);
}

/*
 * Debug reporting.
 */
static void
report_pinned_chunk (PinnedChunk *chunk, int seq) {
	void **p;
	int i, free_pages, num_free, free_mem;
	free_pages = 0;
	for (i = 0; i < chunk->num_pages; ++i) {
		if (!chunk->page_sizes [i])
			free_pages++;
	}
	printf ("Pinned chunk %d at %p, size: %d, pages: %d, free: %d\n", seq, chunk, chunk->num_pages * FREELIST_PAGESIZE, chunk->num_pages, free_pages);
	free_mem = FREELIST_PAGESIZE * free_pages;
	for (i = 0; i < FREELIST_NUM_SLOTS; ++i) {
		if (!chunk->free_list [i])
			continue;
		num_free = 0;
		p = chunk->free_list [i];
		while (p) {
			num_free++;
			p = *p;
		}
		printf ("\tfree list of size %d, %d items\n", freelist_sizes [i], num_free);
		free_mem += freelist_sizes [i] * num_free;
	}
	printf ("\tfree memory in chunk: %d\n", free_mem);
}

/*
 * Debug reporting.
 */
static G_GNUC_UNUSED void
report_internal_mem_usage (void) {
	PinnedChunk *chunk;
	int i;
	printf ("Internal memory usage:\n");
	i = 0;
	for (chunk = internal_chunk_list; chunk; chunk = chunk->block.next) {
		report_pinned_chunk (chunk, i++);
	}
	printf ("Pinned memory usage:\n");
	i = 0;
	for (chunk = pinned_chunk_list; chunk; chunk = chunk->block.next) {
		report_pinned_chunk (chunk, i++);
	}
}

/*
 * the array of pointers from @start to @end contains conservative
 * pointers to objects inside @chunk: mark each referenced object
 * with the PIN bit.
 */
static void
mark_pinned_from_addresses (PinnedChunk *chunk, void **start, void **end)
{
	for (; start < end; start++) {
		char *addr = *start;
		int offset = (char*)addr - (char*)chunk;
		int page = offset / FREELIST_PAGESIZE;
		int obj_offset = page == 0? offset - ((char*)chunk->start_data - (char*)chunk): offset % FREELIST_PAGESIZE;
		int slot_size = chunk->page_sizes [page];
		void **ptr;
		/* the page is not allocated */
		if (!slot_size)
			continue;
		/* would be faster if we restrict the sizes to power of two,
		 * but that's a waste of memory: need to measure. it could reduce
		 * fragmentation since there are less pages needed, if for example
		 * someone interns strings of each size we end up with one page per
		 * interned string (still this is just ~40 KB): with more fine-grained sizes
		 * this increases the number of used pages.
		 */
		if (page == 0) {
			obj_offset /= slot_size;
			obj_offset *= slot_size;
			addr = (char*)chunk->start_data + obj_offset;
		} else {
			obj_offset /= slot_size;
			obj_offset *= slot_size;
			addr = (char*)chunk + page * FREELIST_PAGESIZE + obj_offset;
		}
		ptr = (void**)addr;
		/* if the vtable is inside the chunk it's on the freelist, so skip */
		if (*ptr && (*ptr < (void*)chunk->start_data || *ptr > (void*)((char*)chunk + chunk->num_pages * FREELIST_PAGESIZE))) {
			pin_object (addr);
			if (heap_dump_file)
				pin_stats_register_object ((char*) addr, safe_object_get_size (addr));
			DEBUG (6, fprintf (gc_debug_file, "Marked pinned object %p (%s) from roots\n", addr, safe_name (addr)));
		}
	}
}

static void
scan_pinned_objects (ScanPinnedObjectCallbackFunc callback, void *callback_data)
{
	PinnedChunk *chunk;
	int i, obj_size;
	char *p, *endp;
	void **ptr;
	void *end_chunk;
	for (chunk = pinned_chunk_list; chunk; chunk = chunk->block.next) {
		end_chunk = (char*)chunk + chunk->num_pages * FREELIST_PAGESIZE;
		DEBUG (6, fprintf (gc_debug_file, "Scanning pinned chunk %p (range: %p-%p)\n", chunk, chunk->start_data, end_chunk));
		for (i = 0; i < chunk->num_pages; ++i) {
			obj_size = chunk->page_sizes [i];
			if (!obj_size)
				continue;
			p = i? (char*)chunk + i * FREELIST_PAGESIZE: chunk->start_data;
			endp = i? p + FREELIST_PAGESIZE: (char*)chunk + FREELIST_PAGESIZE;
			DEBUG (6, fprintf (gc_debug_file, "Page %d (size: %d, range: %p-%p)\n", i, obj_size, p, endp));
			while (p + obj_size <= endp) {
				ptr = (void**)p;
				DEBUG (9, fprintf (gc_debug_file, "Considering %p (vtable: %p)\n", ptr, *ptr));
				/* if the first word (the vtable) is outside the chunk we have an object */
				if (*ptr && (*ptr < (void*)chunk || *ptr >= end_chunk))
					callback (chunk, (char*)ptr, obj_size, callback_data);
				p += obj_size;
			}
		}
	}
}

static void
sweep_pinned_objects_callback (PinnedChunk *chunk, char *ptr, size_t size, void *data)
{
	if (object_is_pinned (ptr)) {
		unpin_object (ptr);
		DEBUG (6, fprintf (gc_debug_file, "Unmarked pinned object %p (%s)\n", ptr, safe_name (ptr)));
	} else {
		DEBUG (6, fprintf (gc_debug_file, "Freeing unmarked pinned object %p (%s)\n", ptr, safe_name (ptr)));
		free_pinned_object (chunk, ptr, size);
	}
}

static void
sweep_pinned_objects (void)
{
	scan_pinned_objects (sweep_pinned_objects_callback, NULL);
}

static void
scan_object_callback (PinnedChunk *chunk, char *ptr, size_t size, char **data)
{
	DEBUG (6, fprintf (gc_debug_file, "Precise object scan of alloc_pinned %p (%s)\n", ptr, safe_name (ptr)));
	/* FIXME: Put objects without references into separate chunks
	   which do not need to be scanned */
	scan_object (ptr, data [0], data [1]);
}

static void
scan_from_pinned_objects (char *addr_start, char *addr_end)
{
	char *data [2] = { addr_start, addr_end };
	scan_pinned_objects ((ScanPinnedObjectCallbackFunc)scan_object_callback, data);
}

/*
 * Find the slot number in the freelist for memory chunks that
 * can contain @size objects.
 */
static int
slot_for_size (size_t size)
{
	int slot;
	/* do a binary search or lookup table later. */
	for (slot = 0; slot < FREELIST_NUM_SLOTS; ++slot) {
		if (freelist_sizes [slot] >= size)
			return slot;
	}
	g_assert_not_reached ();
	return -1;
}

/*
 * Build a free list for @size memory chunks from the memory area between
 * start_page and end_page.
 */
static void
build_freelist (PinnedChunk *chunk, int slot, int size, char *start_page, char *end_page)
{
	void **p, **end;
	int count = 0;
	/*g_print ("building freelist for slot %d, size %d in %p\n", slot, size, chunk);*/
	p = (void**)start_page;
	end = (void**)(end_page - size);
	g_assert (!chunk->free_list [slot]);
	chunk->free_list [slot] = p;
	while ((char*)p + size <= (char*)end) {
		count++;
		*p = (void*)((char*)p + size);
		p = *p;
	}
	*p = NULL;
	/*g_print ("%d items created, max: %d\n", count, (end_page - start_page) / size);*/
}

static PinnedChunk*
alloc_pinned_chunk (void)
{
	PinnedChunk *chunk;
	int offset;
	int size = MAJOR_SECTION_SIZE;

	chunk = get_os_memory_aligned (size, TRUE);
	chunk->block.role = MEMORY_ROLE_PINNED;

	UPDATE_HEAP_BOUNDARIES (chunk, ((char*)chunk + size));
	total_alloc += size;
	pinned_chunk_bytes_alloced += size;

	/* setup the bookeeping fields */
	chunk->num_pages = size / FREELIST_PAGESIZE;
	offset = G_STRUCT_OFFSET (PinnedChunk, data);
	chunk->page_sizes = (void*)((char*)chunk + offset);
	offset += sizeof (int) * chunk->num_pages;
	offset += ALLOC_ALIGN - 1;
	offset &= ~(ALLOC_ALIGN - 1);
	chunk->free_list = (void*)((char*)chunk + offset);
	offset += sizeof (void*) * FREELIST_NUM_SLOTS;
	offset += ALLOC_ALIGN - 1;
	offset &= ~(ALLOC_ALIGN - 1);
	chunk->start_data = (void*)((char*)chunk + offset);

	/* allocate the first page to the freelist */
	chunk->page_sizes [0] = PINNED_FIRST_SLOT_SIZE;
	build_freelist (chunk, slot_for_size (PINNED_FIRST_SLOT_SIZE), PINNED_FIRST_SLOT_SIZE, chunk->start_data, ((char*)chunk + FREELIST_PAGESIZE));
	DEBUG (4, fprintf (gc_debug_file, "Allocated pinned chunk %p, size: %zd\n", chunk, size));
	min_pinned_chunk_addr = MIN (min_pinned_chunk_addr, (char*)chunk->start_data);
	max_pinned_chunk_addr = MAX (max_pinned_chunk_addr, ((char*)chunk + size));
	return chunk;
}

/* assumes freelist for slot is empty, so try to alloc a new page */
static void*
get_chunk_freelist (PinnedChunk *chunk, int slot)
{
	int i;
	void **p;
	p = chunk->free_list [slot];
	if (p) {
		chunk->free_list [slot] = *p;
		return p;
	}
	for (i = 0; i < chunk->num_pages; ++i) {
		int size;
		if (chunk->page_sizes [i])
			continue;
		size = freelist_sizes [slot];
		chunk->page_sizes [i] = size;
		build_freelist (chunk, slot, size, (char*)chunk + FREELIST_PAGESIZE * i, (char*)chunk + FREELIST_PAGESIZE * (i + 1));
		break;
	}
	/* try again */
	p = chunk->free_list [slot];
	if (p) {
		chunk->free_list [slot] = *p;
		return p;
	}
	return NULL;
}

static void*
alloc_from_freelist (size_t size)
{
	int slot;
	void *res = NULL;
	PinnedChunk *pchunk;
	slot = slot_for_size (size);
	/*g_print ("using slot %d for size %d (slot size: %d)\n", slot, size, freelist_sizes [slot]);*/
	g_assert (size <= freelist_sizes [slot]);
	for (pchunk = pinned_chunk_list; pchunk; pchunk = pchunk->block.next) {
		void **p = pchunk->free_list [slot];
		if (p) {
			/*g_print ("found freelist for slot %d in chunk %p, returning %p, next %p\n", slot, pchunk, p, *p);*/
			pchunk->free_list [slot] = *p;
			return p;
		}
	}
	for (pchunk = pinned_chunk_list; pchunk; pchunk = pchunk->block.next) {
		res = get_chunk_freelist (pchunk, slot);
		if (res)
			return res;
	}
	pchunk = alloc_pinned_chunk ();
	/* FIXME: handle OOM */
	pchunk->block.next = pinned_chunk_list;
	pinned_chunk_list = pchunk;
	res = get_chunk_freelist (pchunk, slot);
	return res;
}

/* used for the GC-internal data structures */
/* FIXME: add support for bigger sizes by allocating more than one page
 * in the chunk.
 */
static void*
get_internal_mem (size_t size, int type)
{
	int slot;
	void *res = NULL;
	PinnedChunk *pchunk;

	if (size > freelist_sizes [FREELIST_NUM_SLOTS - 1]) {
		LargeInternalMemHeader *mh;

		size += sizeof (LargeInternalMemHeader);
		mh = get_os_memory (size, TRUE);
		mh->magic = LARGE_INTERNAL_MEM_HEADER_MAGIC;
		mh->size = size;

		large_internal_bytes_alloced += size;

		return mh->data;
	}

	slot = slot_for_size (size);
	g_assert (size <= freelist_sizes [slot]);

	small_internal_mem_bytes [type] += freelist_sizes [slot];

	for (pchunk = internal_chunk_list; pchunk; pchunk = pchunk->block.next) {
		void **p = pchunk->free_list [slot];
		if (p) {
			pchunk->free_list [slot] = *p;
			memset (p, 0, size);
			return p;
		}
	}
	for (pchunk = internal_chunk_list; pchunk; pchunk = pchunk->block.next) {
		res = get_chunk_freelist (pchunk, slot);
		if (res) {
			memset (res, 0, size);
			return res;
		}
	}
	pchunk = alloc_pinned_chunk ();
	/* FIXME: handle OOM */
	pchunk->block.next = internal_chunk_list;
	internal_chunk_list = pchunk;
	res = get_chunk_freelist (pchunk, slot);
	memset (res, 0, size);
	return res;
}

static void
free_internal_mem (void *addr, int type)
{
	PinnedChunk *pchunk;
	LargeInternalMemHeader *mh;
	if (!addr)
		return;
	for (pchunk = internal_chunk_list; pchunk; pchunk = pchunk->block.next) {
		/*printf ("trying to free %p in %p (pages: %d)\n", addr, pchunk, pchunk->num_pages);*/
		if (addr >= (void*)pchunk && (char*)addr < (char*)pchunk + pchunk->num_pages * FREELIST_PAGESIZE) {
			int offset = (char*)addr - (char*)pchunk;
			int page = offset / FREELIST_PAGESIZE;
			int slot = slot_for_size (pchunk->page_sizes [page]);
			void **p = addr;
			*p = pchunk->free_list [slot];
			pchunk->free_list [slot] = p;

			small_internal_mem_bytes [type] -= freelist_sizes [slot];

			return;
		}
	}
	mh = (LargeInternalMemHeader*)((char*)addr - G_STRUCT_OFFSET (LargeInternalMemHeader, data));
	g_assert (mh->magic == LARGE_INTERNAL_MEM_HEADER_MAGIC);
	large_internal_bytes_alloced -= mh->size;
	free_os_memory (mh, mh->size);
}

/*
 * ######################################################################
 * ########  Object allocation
 * ######################################################################
 * This section of code deals with allocating memory for objects.
 * There are several ways:
 * *) allocate large objects
 * *) allocate normal objects
 * *) fast lock-free allocation
 * *) allocation of pinned objects
 */

static void
free_large_object (LOSObject *obj)
{
	size_t size = obj->size;
	DEBUG (4, fprintf (gc_debug_file, "Freed large object %p, size %zd\n", obj->data, obj->size));

	los_memory_usage -= size;
	size += sizeof (LOSObject);
	size += pagesize - 1;
	size &= ~(pagesize - 1);
	total_alloc -= size;
	los_num_objects--;
	free_os_memory (obj, size);
}

/*
 * Objects with size >= 64KB are allocated in the large object space.
 * They are currently kept track of with a linked list.
 * They don't move, so there is no need to pin them during collection
 * and we avoid the memcpy overhead.
 */
static void* __attribute__((noinline))
alloc_large_inner (MonoVTable *vtable, size_t size)
{
	LOSObject *obj;
	void **vtslot;
	size_t alloc_size;
	int just_did_major_gc = FALSE;

	g_assert (size > MAX_SMALL_OBJ_SIZE);

	if (los_memory_usage > next_los_collection) {
		DEBUG (4, fprintf (gc_debug_file, "Should trigger major collection: req size %zd (los already: %zu, limit: %zu)\n", size, los_memory_usage, next_los_collection));
		just_did_major_gc = TRUE;
		stop_world ();
		major_collection ("LOS overflow");
		restart_world ();
		/* later increase based on a percent of the heap size */
		next_los_collection = los_memory_usage + 5*1024*1024;
	}
	alloc_size = size;
	alloc_size += sizeof (LOSObject);
	alloc_size += pagesize - 1;
	alloc_size &= ~(pagesize - 1);
	/* FIXME: handle OOM */
	obj = get_os_memory (alloc_size, TRUE);
	obj->size = size;
	vtslot = (void**)obj->data;
	*vtslot = vtable;
	total_alloc += alloc_size;
	UPDATE_HEAP_BOUNDARIES (obj->data, (char*)obj->data + size);
	obj->next = los_object_list;
	los_object_list = obj;
	los_memory_usage += size;
	los_num_objects++;
	DEBUG (4, fprintf (gc_debug_file, "Allocated large object %p, vtable: %p (%s), size: %zd\n", obj->data, vtable, vtable->klass->name, size));
	return obj->data;
}

/* check if we have a suitable fragment in nursery_fragments to be able to allocate
 * an object of size @size
 * Return FALSE if not found (which means we need a collection)
 */
static gboolean
search_fragment_for_size (size_t size)
{
	Fragment *frag, *prev;
	DEBUG (4, fprintf (gc_debug_file, "Searching nursery fragment %p, size: %zd\n", nursery_frag_real_end, size));

	if (nursery_frag_real_end > nursery_next && nursery_clear_policy == CLEAR_AT_TLAB_CREATION)
		/* Clear the remaining space, pinning depends on this */
		memset (nursery_next, 0, nursery_frag_real_end - nursery_next);

	prev = NULL;
	for (frag = nursery_fragments; frag; frag = frag->next) {
		if (size <= (frag->fragment_end - frag->fragment_start)) {
			/* remove from the list */
			if (prev)
				prev->next = frag->next;
			else
				nursery_fragments = frag->next;
			nursery_next = frag->fragment_start;
			nursery_frag_real_end = frag->fragment_end;

			DEBUG (4, fprintf (gc_debug_file, "Using nursery fragment %p-%p, size: %zd (req: %zd)\n", nursery_next, nursery_frag_real_end, nursery_frag_real_end - nursery_next, size));
			frag->next = fragment_freelist;
			fragment_freelist = frag;
			return TRUE;
		}
		prev = frag;
	}
	return FALSE;
}

/*
 * size is already rounded up and we hold the GC lock.
 */
static void*
alloc_degraded (MonoVTable *vtable, size_t size)
{
	GCMemSection *section;
	void **p = NULL;
	g_assert (size <= MAX_SMALL_OBJ_SIZE);
	for (section = section_list; section; section = section->block.next) {
		if (section != nursery_section && (section->end_data - section->next_data) >= size) {
			p = (void**)section->next_data;
			break;
		}
	}
	if (!p) {
		section = alloc_major_section ();
		section->is_to_space = FALSE;
		/* FIXME: handle OOM */
		p = (void**)section->next_data;
	}
	section->next_data += size;
	degraded_mode += size;
	DEBUG (3, fprintf (gc_debug_file, "Allocated (degraded) object %p, vtable: %p (%s), size: %zd in section %p\n", p, vtable, vtable->klass->name, size, section));
	*p = vtable;
	return p;
}

/*
 * Provide a variant that takes just the vtable for small fixed-size objects.
 * The aligned size is already computed and stored in vt->gc_descr.
 * Note: every SCAN_START_SIZE or so we are given the chance to do some special
 * processing. We can keep track of where objects start, for example,
 * so when we scan the thread stacks for pinned objects, we can start
 * a search for the pinned object in SCAN_START_SIZE chunks.
 */
static void*
mono_gc_alloc_obj_nolock (MonoVTable *vtable, size_t size)
{
	/* FIXME: handle OOM */
	void **p;
	char *new_next;
	gboolean res;
	TLAB_ACCESS_INIT;

	HEAVY_STAT (++stat_objects_alloced);

	size += ALLOC_ALIGN - 1;
	size &= ~(ALLOC_ALIGN - 1);

	g_assert (vtable->gc_descr);

	if (G_UNLIKELY (collect_before_allocs)) {
		if (nursery_section) {
			stop_world ();
			collect_nursery (0);
			restart_world ();
			if (!degraded_mode && !search_fragment_for_size (size)) {
				// FIXME:
				g_assert_not_reached ();
			}
		}
	}

	/*
	 * We must already have the lock here instead of after the
	 * fast path because we might be interrupted in the fast path
	 * (after confirming that new_next < TLAB_TEMP_END) by the GC,
	 * and we'll end up allocating an object in a fragment which
	 * no longer belongs to us.
	 *
	 * The managed allocator does not do this, but it's treated
	 * specially by the world-stopping code.
	 */

	if (size > MAX_SMALL_OBJ_SIZE) {
		p = alloc_large_inner (vtable, size);
	} else {
		/* tlab_next and tlab_temp_end are TLS vars so accessing them might be expensive */

		p = (void**)TLAB_NEXT;
		/* FIXME: handle overflow */
		new_next = (char*)p + size;
		TLAB_NEXT = new_next;

		if (G_LIKELY (new_next < TLAB_TEMP_END)) {
			/* Fast path */

			/* 
			 * FIXME: We might need a memory barrier here so the change to tlab_next is 
			 * visible before the vtable store.
			 */

			DEBUG (6, fprintf (gc_debug_file, "Allocated object %p, vtable: %p (%s), size: %zd\n", p, vtable, vtable->klass->name, size));
			g_assert (*p == NULL);
			*p = vtable;

			g_assert (TLAB_NEXT == new_next);

			return p;
		}

		/* Slow path */

		/* there are two cases: the object is too big or we run out of space in the TLAB */
		/* we also reach here when the thread does its first allocation after a minor 
		 * collection, since the tlab_ variables are initialized to NULL.
		 * there can be another case (from ORP), if we cooperate with the runtime a bit:
		 * objects that need finalizers can have the high bit set in their size
		 * so the above check fails and we can readily add the object to the queue.
		 * This avoids taking again the GC lock when registering, but this is moot when
		 * doing thread-local allocation, so it may not be a good idea.
		 */
		g_assert (TLAB_NEXT == new_next);
		if (TLAB_NEXT >= TLAB_REAL_END) {
			/* 
			 * Run out of space in the TLAB. When this happens, some amount of space
			 * remains in the TLAB, but not enough to satisfy the current allocation
			 * request. Currently, we retire the TLAB in all cases, later we could
			 * keep it if the remaining space is above a treshold, and satisfy the
			 * allocation directly from the nursery.
			 */
			TLAB_NEXT -= size;
			/* when running in degraded mode, we continue allocing that way
			 * for a while, to decrease the number of useless nursery collections.
			 */
			if (degraded_mode && degraded_mode < DEFAULT_NURSERY_SIZE) {
				p = alloc_degraded (vtable, size);
				return p;
			}

			if (size > tlab_size) {
				/* Allocate directly from the nursery */
				if (nursery_next + size >= nursery_frag_real_end) {
					if (!search_fragment_for_size (size)) {
						minor_collect_or_expand_inner (size);
						if (degraded_mode) {
							p = alloc_degraded (vtable, size);
							return p;
						}
					}
				}

				p = (void*)nursery_next;
				nursery_next += size;
				if (nursery_next > nursery_frag_real_end) {
					// no space left
					g_assert (0);
				}

				if (nursery_clear_policy == CLEAR_AT_TLAB_CREATION)
					memset (p, 0, size);
			} else {
				if (TLAB_START)
					DEBUG (3, fprintf (gc_debug_file, "Retire TLAB: %p-%p [%ld]\n", TLAB_START, TLAB_REAL_END, (long)(TLAB_REAL_END - TLAB_NEXT - size)));

				if (nursery_next + tlab_size >= nursery_frag_real_end) {
					res = search_fragment_for_size (tlab_size);
					if (!res) {
						minor_collect_or_expand_inner (tlab_size);
						if (degraded_mode) {
							p = alloc_degraded (vtable, size);
							return p;
						}
					}
				}

				/* Allocate a new TLAB from the current nursery fragment */
				TLAB_START = nursery_next;
				nursery_next += tlab_size;
				TLAB_NEXT = TLAB_START;
				TLAB_REAL_END = TLAB_START + tlab_size;
				TLAB_TEMP_END = TLAB_START + MIN (SCAN_START_SIZE, tlab_size);

				if (nursery_clear_policy == CLEAR_AT_TLAB_CREATION)
					memset (TLAB_START, 0, tlab_size);

				/* Allocate from the TLAB */
				p = (void*)TLAB_NEXT;
				TLAB_NEXT += size;
				g_assert (TLAB_NEXT <= TLAB_REAL_END);

				nursery_section->scan_starts [((char*)p - (char*)nursery_section->data)/SCAN_START_SIZE] = (char*)p;
			}
		} else {
			/* Reached tlab_temp_end */

			/* record the scan start so we can find pinned objects more easily */
			nursery_section->scan_starts [((char*)p - (char*)nursery_section->data)/SCAN_START_SIZE] = (char*)p;
			/* we just bump tlab_temp_end as well */
			TLAB_TEMP_END = MIN (TLAB_REAL_END, TLAB_NEXT + SCAN_START_SIZE);
			DEBUG (5, fprintf (gc_debug_file, "Expanding local alloc: %p-%p\n", TLAB_NEXT, TLAB_TEMP_END));
		}
	}

	DEBUG (6, fprintf (gc_debug_file, "Allocated object %p, vtable: %p (%s), size: %zd\n", p, vtable, vtable->klass->name, size));
	*p = vtable;

	return p;
}

void*
mono_gc_alloc_obj (MonoVTable *vtable, size_t size)
{
	void *res;
	LOCK_GC;
	res = mono_gc_alloc_obj_nolock (vtable, size);
	UNLOCK_GC;
	return res;
}

void*
mono_gc_alloc_vector (MonoVTable *vtable, size_t size, mono_array_size_t max_length)
{
	MonoArray *arr;

	LOCK_GC;

	arr = mono_gc_alloc_obj_nolock (vtable, size);
	arr->max_length = max_length;

	UNLOCK_GC;

	return arr;
}

void*
mono_gc_alloc_array (MonoVTable *vtable, size_t size, mono_array_size_t max_length, mono_array_size_t bounds_size)
{
	MonoArray *arr;
	MonoArrayBounds *bounds;

	LOCK_GC;

	arr = mono_gc_alloc_obj_nolock (vtable, size);
	arr->max_length = max_length;

	bounds = (MonoArrayBounds*)((char*)arr + size - bounds_size);
	arr->bounds = bounds;

	UNLOCK_GC;

	return arr;
}

/*
 * To be used for interned strings and possibly MonoThread, reflection handles.
 * We may want to explicitly free these objects.
 */
void*
mono_gc_alloc_pinned_obj (MonoVTable *vtable, size_t size)
{
	/* FIXME: handle OOM */
	void **p;
	size += ALLOC_ALIGN - 1;
	size &= ~(ALLOC_ALIGN - 1);
	LOCK_GC;
	if (size > MAX_FREELIST_SIZE) {
		/* large objects are always pinned anyway */
		p = alloc_large_inner (vtable, size);
	} else {
		p = alloc_from_freelist (size);
		memset (p, 0, size);
	}
	DEBUG (6, fprintf (gc_debug_file, "Allocated pinned object %p, vtable: %p (%s), size: %zd\n", p, vtable, vtable->klass->name, size));
	*p = vtable;
	UNLOCK_GC;
	return p;
}

/*
 * ######################################################################
 * ########  Finalization support
 * ######################################################################
 */

/*
 * this is valid for the nursery: if the object has been forwarded it means it's
 * still refrenced from a root. If it is pinned it's still alive as well.
 * Return TRUE if @obj is ready to be finalized.
 */
#define object_is_fin_ready(obj) (!object_is_pinned (obj) && !object_is_forwarded (obj))

static gboolean
is_critical_finalizer (FinalizeEntry *entry)
{
	MonoObject *obj;
	MonoClass *class;

	if (!mono_defaults.critical_finalizer_object)
		return FALSE;

	obj = entry->object;
	class = ((MonoVTable*)LOAD_VTABLE (obj))->klass;

	return mono_class_has_parent (class, mono_defaults.critical_finalizer_object);
}

static void
queue_finalization_entry (FinalizeEntry *entry) {
	if (is_critical_finalizer (entry)) {
		entry->next = critical_fin_list;
		critical_fin_list = entry;
	} else {
		entry->next = fin_ready_list;
		fin_ready_list = entry;
	}
}

/* LOCKING: requires that the GC lock is held */
static void
rehash_fin_table (FinalizeEntryHashTable *hash_table)
{
	FinalizeEntry **finalizable_hash = hash_table->table;
	mword finalizable_hash_size = hash_table->size;
	int i;
	unsigned int hash;
	FinalizeEntry **new_hash;
	FinalizeEntry *entry, *next;
	int new_size = g_spaced_primes_closest (hash_table->num_registered);

	new_hash = get_internal_mem (new_size * sizeof (FinalizeEntry*), INTERNAL_MEM_FIN_TABLE);
	for (i = 0; i < finalizable_hash_size; ++i) {
		for (entry = finalizable_hash [i]; entry; entry = next) {
			hash = mono_object_hash (entry->object) % new_size;
			next = entry->next;
			entry->next = new_hash [hash];
			new_hash [hash] = entry;
		}
	}
	free_internal_mem (finalizable_hash, INTERNAL_MEM_FIN_TABLE);
	hash_table->table = new_hash;
	hash_table->size = new_size;
}

/* LOCKING: requires that the GC lock is held */
static void
rehash_fin_table_if_necessary (FinalizeEntryHashTable *hash_table)
{
	if (hash_table->num_registered >= hash_table->size * 2)
		rehash_fin_table (hash_table);
}

/* LOCKING: requires that the GC lock is held */
static void
finalize_in_range (char *start, char *end, int generation)
{
	FinalizeEntryHashTable *hash_table = get_finalize_entry_hash_table (generation);
	FinalizeEntry *entry, *prev;
	int i;
	FinalizeEntry **finalizable_hash = hash_table->table;
	mword finalizable_hash_size = hash_table->size;

	if (no_finalize)
		return;
	for (i = 0; i < finalizable_hash_size; ++i) {
		prev = NULL;
		for (entry = finalizable_hash [i]; entry;) {
			if ((char*)entry->object >= start && (char*)entry->object < end && !object_is_in_to_space (entry->object)) {
				gboolean is_fin_ready = object_is_fin_ready (entry->object);
				char *copy = copy_object (entry->object, start, end);
				if (is_fin_ready) {
					char *from;
					FinalizeEntry *next;
					/* remove and put in fin_ready_list */
					if (prev)
						prev->next = entry->next;
					else
						finalizable_hash [i] = entry->next;
					next = entry->next;
					num_ready_finalizers++;
					hash_table->num_registered--;
					queue_finalization_entry (entry);
					/* Make it survive */
					from = entry->object;
					entry->object = copy;
					DEBUG (5, fprintf (gc_debug_file, "Queueing object for finalization: %p (%s) (was at %p) (%d/%d)\n", entry->object, safe_name (entry->object), from, num_ready_finalizers, hash_table->num_registered));
					entry = next;
					continue;
				} else {
					char *from = entry->object;
					if (hash_table == &minor_finalizable_hash && !ptr_in_nursery (copy)) {
						FinalizeEntry *next = entry->next;
						unsigned int major_hash;
						/* remove from the list */
						if (prev)
							prev->next = entry->next;
						else
							finalizable_hash [i] = entry->next;
						hash_table->num_registered--;

						entry->object = copy;

						/* insert it into the major hash */
						rehash_fin_table_if_necessary (&major_finalizable_hash);
						major_hash = mono_object_hash ((MonoObject*) copy) %
							major_finalizable_hash.size;
						entry->next = major_finalizable_hash.table [major_hash];
						major_finalizable_hash.table [major_hash] = entry;
						major_finalizable_hash.num_registered++;

						DEBUG (5, fprintf (gc_debug_file, "Promoting finalization of object %p (%s) (was at %p) to major table\n", copy, safe_name (copy), from));

						entry = next;
						continue;
					} else {
						/* update pointer */
						DEBUG (5, fprintf (gc_debug_file, "Updating object for finalization: %p (%s) (was at %p)\n", entry->object, safe_name (entry->object), from));
						entry->object = copy;
					}
				}
			}
			prev = entry;
			entry = entry->next;
		}
	}
}

/* LOCKING: requires that the GC lock is held */
static void
null_link_in_range (char *start, char *end, int generation)
{
	DisappearingLinkHashTable *hash = get_dislink_hash_table (generation);
	DisappearingLink **disappearing_link_hash = hash->table;
	int disappearing_link_hash_size = hash->size;
	DisappearingLink *entry, *prev;
	int i;
	if (!hash->num_links)
		return;
	for (i = 0; i < disappearing_link_hash_size; ++i) {
		prev = NULL;
		for (entry = disappearing_link_hash [i]; entry;) {
			char *object = DISLINK_OBJECT (entry);
			if (object >= start && object < end && !object_is_in_to_space (object)) {
				gboolean track = DISLINK_TRACK (entry);
				if (!track && object_is_fin_ready (object)) {
					void **p = entry->link;
					DisappearingLink *old;
					*p = NULL;
					/* remove from list */
					if (prev)
						prev->next = entry->next;
					else
						disappearing_link_hash [i] = entry->next;
					DEBUG (5, fprintf (gc_debug_file, "Dislink nullified at %p to GCed object %p\n", p, object));
					old = entry->next;
					free_internal_mem (entry, INTERNAL_MEM_DISLINK);
					entry = old;
					hash->num_links--;
					continue;
				} else {
					char *copy = copy_object (object, start, end);

					/* Update pointer if it's moved.  If the object
					 * has been moved out of the nursery, we need to
					 * remove the link from the minor hash table to
					 * the major one.
					 *
					 * FIXME: what if an object is moved earlier?
					 */

					if (hash == &minor_disappearing_link_hash && !ptr_in_nursery (copy)) {
						void **link = entry->link;
						DisappearingLink *old;
						/* remove from list */
						if (prev)
							prev->next = entry->next;
						else
							disappearing_link_hash [i] = entry->next;
						old = entry->next;
						free_internal_mem (entry, INTERNAL_MEM_DISLINK);
						entry = old;
						hash->num_links--;

						add_or_remove_disappearing_link ((MonoObject*)copy, link,
							track, GENERATION_OLD);

						DEBUG (5, fprintf (gc_debug_file, "Upgraded dislink at %p to major because object %p moved to %p\n", link, object, copy));

						continue;
					} else {
						/* We set the track resurrection bit to
						 * FALSE if the object is to be finalized
						 * so that the object can be collected in
						 * the next cycle (i.e. after it was
						 * finalized).
						 */
						*entry->link = HIDE_POINTER (copy,
							object_is_fin_ready (object) ? FALSE : track);
						DEBUG (5, fprintf (gc_debug_file, "Updated dislink at %p to %p\n", entry->link, DISLINK_OBJECT (entry)));
					}
				}
			}
			prev = entry;
			entry = entry->next;
		}
	}
}

/* LOCKING: requires that the GC lock is held */
static void
null_links_for_domain (MonoDomain *domain, int generation)
{
	DisappearingLinkHashTable *hash = get_dislink_hash_table (generation);
	DisappearingLink **disappearing_link_hash = hash->table;
	int disappearing_link_hash_size = hash->size;
	DisappearingLink *entry, *prev;
	int i;
	for (i = 0; i < disappearing_link_hash_size; ++i) {
		prev = NULL;
		for (entry = disappearing_link_hash [i]; entry; ) {
			char *object = DISLINK_OBJECT (entry);
			/* FIXME: actually there should be no object
			   left in the domain with a non-null vtable
			   (provided we remove the Thread special
			   case) */
			if (object && (!((MonoObject*)object)->vtable || mono_object_domain (object) == domain)) {
				DisappearingLink *next = entry->next;

				if (prev)
					prev->next = next;
				else
					disappearing_link_hash [i] = next;

				if (*(entry->link)) {
					*(entry->link) = NULL;
					g_warning ("Disappearing link %p not freed", entry->link);
				} else {
					free_internal_mem (entry, INTERNAL_MEM_DISLINK);
				}

				entry = next;
				continue;
			}
			prev = entry;
			entry = entry->next;
		}
	}
}

/* LOCKING: requires that the GC lock is held */
static int
finalizers_for_domain (MonoDomain *domain, MonoObject **out_array, int out_size,
	FinalizeEntryHashTable *hash_table)
{
	FinalizeEntry **finalizable_hash = hash_table->table;
	mword finalizable_hash_size = hash_table->size;
	FinalizeEntry *entry, *prev;
	int i, count;

	if (no_finalize || !out_size || !out_array)
		return 0;
	count = 0;
	for (i = 0; i < finalizable_hash_size; ++i) {
		prev = NULL;
		for (entry = finalizable_hash [i]; entry;) {
			if (mono_object_domain (entry->object) == domain) {
				FinalizeEntry *next;
				/* remove and put in out_array */
				if (prev)
					prev->next = entry->next;
				else
					finalizable_hash [i] = entry->next;
				next = entry->next;
				hash_table->num_registered--;
				out_array [count ++] = entry->object;
				DEBUG (5, fprintf (gc_debug_file, "Collecting object for finalization: %p (%s) (%d/%d)\n", entry->object, safe_name (entry->object), num_ready_finalizers, hash_table->num_registered));
				entry = next;
				if (count == out_size)
					return count;
				continue;
			}
			prev = entry;
			entry = entry->next;
		}
	}
	return count;
}

/**
 * mono_gc_finalizers_for_domain:
 * @domain: the unloading appdomain
 * @out_array: output array
 * @out_size: size of output array
 *
 * Store inside @out_array up to @out_size objects that belong to the unloading
 * appdomain @domain. Returns the number of stored items. Can be called repeteadly
 * until it returns 0.
 * The items are removed from the finalizer data structure, so the caller is supposed
 * to finalize them.
 * @out_array should be on the stack to allow the GC to know the objects are still alive.
 */
int
mono_gc_finalizers_for_domain (MonoDomain *domain, MonoObject **out_array, int out_size)
{
	int result;

	LOCK_GC;
	result = finalizers_for_domain (domain, out_array, out_size, &minor_finalizable_hash);
	if (result < out_size) {
		result += finalizers_for_domain (domain, out_array + result, out_size - result,
			&major_finalizable_hash);
	}
	UNLOCK_GC;

	return result;
}

static void
register_for_finalization (MonoObject *obj, void *user_data, int generation)
{
	FinalizeEntryHashTable *hash_table = get_finalize_entry_hash_table (generation);
	FinalizeEntry **finalizable_hash;
	mword finalizable_hash_size;
	FinalizeEntry *entry, *prev;
	unsigned int hash;
	if (no_finalize)
		return;
	g_assert (user_data == NULL || user_data == mono_gc_run_finalize);
	hash = mono_object_hash (obj);
	LOCK_GC;
	rehash_fin_table_if_necessary (hash_table);
	finalizable_hash = hash_table->table;
	finalizable_hash_size = hash_table->size;
	hash %= finalizable_hash_size;
	prev = NULL;
	for (entry = finalizable_hash [hash]; entry; entry = entry->next) {
		if (entry->object == obj) {
			if (!user_data) {
				/* remove from the list */
				if (prev)
					prev->next = entry->next;
				else
					finalizable_hash [hash] = entry->next;
				hash_table->num_registered--;
				DEBUG (5, fprintf (gc_debug_file, "Removed finalizer %p for object: %p (%s) (%d)\n", entry, obj, obj->vtable->klass->name, hash_table->num_registered));
				free_internal_mem (entry, INTERNAL_MEM_FINALIZE_ENTRY);
			}
			UNLOCK_GC;
			return;
		}
		prev = entry;
	}
	if (!user_data) {
		/* request to deregister, but already out of the list */
		UNLOCK_GC;
		return;
	}
	entry = get_internal_mem (sizeof (FinalizeEntry), INTERNAL_MEM_FINALIZE_ENTRY);
	entry->object = obj;
	entry->next = finalizable_hash [hash];
	finalizable_hash [hash] = entry;
	hash_table->num_registered++;
	DEBUG (5, fprintf (gc_debug_file, "Added finalizer %p for object: %p (%s) (%d) to %s table\n", entry, obj, obj->vtable->klass->name, hash_table->num_registered, generation_name (generation)));
	UNLOCK_GC;
}

void
mono_gc_register_for_finalization (MonoObject *obj, void *user_data)
{
	if (ptr_in_nursery (obj))
		register_for_finalization (obj, user_data, GENERATION_NURSERY);
	else
		register_for_finalization (obj, user_data, GENERATION_OLD);
}

static void
rehash_dislink (DisappearingLinkHashTable *hash_table)
{
	DisappearingLink **disappearing_link_hash = hash_table->table;
	int disappearing_link_hash_size = hash_table->size;
	int i;
	unsigned int hash;
	DisappearingLink **new_hash;
	DisappearingLink *entry, *next;
	int new_size = g_spaced_primes_closest (hash_table->num_links);

	new_hash = get_internal_mem (new_size * sizeof (DisappearingLink*), INTERNAL_MEM_DISLINK_TABLE);
	for (i = 0; i < disappearing_link_hash_size; ++i) {
		for (entry = disappearing_link_hash [i]; entry; entry = next) {
			hash = mono_aligned_addr_hash (entry->link) % new_size;
			next = entry->next;
			entry->next = new_hash [hash];
			new_hash [hash] = entry;
		}
	}
	free_internal_mem (disappearing_link_hash, INTERNAL_MEM_DISLINK_TABLE);
	hash_table->table = new_hash;
	hash_table->size = new_size;
}

/* LOCKING: assumes the GC lock is held */
static void
add_or_remove_disappearing_link (MonoObject *obj, void **link, gboolean track, int generation)
{
	DisappearingLinkHashTable *hash_table = get_dislink_hash_table (generation);
	DisappearingLink *entry, *prev;
	unsigned int hash;
	DisappearingLink **disappearing_link_hash = hash_table->table;
	int disappearing_link_hash_size = hash_table->size;

	if (hash_table->num_links >= disappearing_link_hash_size * 2) {
		rehash_dislink (hash_table);
		disappearing_link_hash = hash_table->table;
		disappearing_link_hash_size = hash_table->size;
	}
	/* FIXME: add check that link is not in the heap */
	hash = mono_aligned_addr_hash (link) % disappearing_link_hash_size;
	entry = disappearing_link_hash [hash];
	prev = NULL;
	for (; entry; entry = entry->next) {
		/* link already added */
		if (link == entry->link) {
			/* NULL obj means remove */
			if (obj == NULL) {
				if (prev)
					prev->next = entry->next;
				else
					disappearing_link_hash [hash] = entry->next;
				hash_table->num_links--;
				DEBUG (5, fprintf (gc_debug_file, "Removed dislink %p (%d) from %s table\n", entry, hash_table->num_links, generation_name (generation)));
				free_internal_mem (entry, INTERNAL_MEM_DISLINK);
				*link = NULL;
			} else {
				*link = HIDE_POINTER (obj, track); /* we allow the change of object */
			}
			return;
		}
		prev = entry;
	}
	if (obj == NULL)
		return;
	entry = get_internal_mem (sizeof (DisappearingLink), INTERNAL_MEM_DISLINK);
	*link = HIDE_POINTER (obj, track);
	entry->link = link;
	entry->next = disappearing_link_hash [hash];
	disappearing_link_hash [hash] = entry;
	hash_table->num_links++;
	DEBUG (5, fprintf (gc_debug_file, "Added dislink %p for object: %p (%s) at %p to %s table\n", entry, obj, obj->vtable->klass->name, link, generation_name (generation)));
}

/* LOCKING: assumes the GC lock is held */
static void
mono_gc_register_disappearing_link (MonoObject *obj, void **link, gboolean track)
{
	add_or_remove_disappearing_link (NULL, link, FALSE, GENERATION_NURSERY);
	add_or_remove_disappearing_link (NULL, link, FALSE, GENERATION_OLD);
	if (obj) {
		if (ptr_in_nursery (obj))
			add_or_remove_disappearing_link (obj, link, track, GENERATION_NURSERY);
		else
			add_or_remove_disappearing_link (obj, link, track, GENERATION_OLD);
	}
}

int
mono_gc_invoke_finalizers (void)
{
	FinalizeEntry *entry = NULL;
	gboolean entry_is_critical;
	int count = 0;
	void *obj;
	/* FIXME: batch to reduce lock contention */
	while (fin_ready_list || critical_fin_list) {
		LOCK_GC;

		if (entry) {
			FinalizeEntry **list = entry_is_critical ? &critical_fin_list : &fin_ready_list;

			/* We have finalized entry in the last
			   interation, now we need to remove it from
			   the list. */
			if (*list == entry)
				*list = entry->next;
			else {
				FinalizeEntry *e = *list;
				while (e->next != entry)
					e = e->next;
				e->next = entry->next;
			}
			free_internal_mem (entry, INTERNAL_MEM_FINALIZE_ENTRY);
			entry = NULL;
		}

		/* Now look for the first non-null entry. */
		for (entry = fin_ready_list; entry && !entry->object; entry = entry->next)
			;
		if (entry) {
			entry_is_critical = FALSE;
		} else {
			entry_is_critical = TRUE;
			for (entry = critical_fin_list; entry && !entry->object; entry = entry->next)
				;
		}

		if (entry) {
			g_assert (entry->object);
			num_ready_finalizers--;
			obj = entry->object;
			entry->object = NULL;
			DEBUG (7, fprintf (gc_debug_file, "Finalizing object %p (%s)\n", obj, safe_name (obj)));
		}

		UNLOCK_GC;

		if (!entry)
			break;

		g_assert (entry->object == NULL);
		count++;
		/* the object is on the stack so it is pinned */
		/*g_print ("Calling finalizer for object: %p (%s)\n", entry->object, safe_name (entry->object));*/
		mono_gc_run_finalize (obj, NULL);
	}
	g_assert (!entry);
	return count;
}

gboolean
mono_gc_pending_finalizers (void)
{
	return fin_ready_list || critical_fin_list;
}

/* Negative value to remove */
void
mono_gc_add_memory_pressure (gint64 value)
{
	/* FIXME: Use interlocked functions */
	LOCK_GC;
	memory_pressure += value;
	UNLOCK_GC;
}

/*
 * ######################################################################
 * ########  registered roots support
 * ######################################################################
 */

static void
rehash_roots (gboolean pinned)
{
	int i;
	unsigned int hash;
	RootRecord **new_hash;
	RootRecord *entry, *next;
	int new_size;

	new_size = g_spaced_primes_closest (num_roots_entries [pinned]);
	new_hash = get_internal_mem (new_size * sizeof (RootRecord*), INTERNAL_MEM_ROOTS_TABLE);
	for (i = 0; i < roots_hash_size [pinned]; ++i) {
		for (entry = roots_hash [pinned][i]; entry; entry = next) {
			hash = mono_aligned_addr_hash (entry->start_root) % new_size;
			next = entry->next;
			entry->next = new_hash [hash];
			new_hash [hash] = entry;
		}
	}
	free_internal_mem (roots_hash [pinned], INTERNAL_MEM_ROOTS_TABLE);
	roots_hash [pinned] = new_hash;
	roots_hash_size [pinned] = new_size;
}

static RootRecord*
find_root (int root_type, char *start, guint32 addr_hash)
{
	RootRecord *new_root;

	guint32 hash = addr_hash % roots_hash_size [root_type];
	for (new_root = roots_hash [root_type][hash]; new_root; new_root = new_root->next) {
		/* we allow changing the size and the descriptor (for thread statics etc) */
		if (new_root->start_root == start) {
			return new_root;
		}
	}

	return NULL;
}

/*
 * We do not coalesce roots.
 */
static int
mono_gc_register_root_inner (char *start, size_t size, void *descr, int root_type)
{
	RootRecord *new_root;
	unsigned int hash, addr_hash = mono_aligned_addr_hash (start);
	int i;
	LOCK_GC;
	for (i = 0; i < ROOT_TYPE_NUM; ++i) {
		if (num_roots_entries [i] >= roots_hash_size [i] * 2)
			rehash_roots (i);
	}
	for (i = 0; i < ROOT_TYPE_NUM; ++i) {
		new_root = find_root (i, start, addr_hash);
		/* we allow changing the size and the descriptor (for thread statics etc) */
		if (new_root) {
			size_t old_size = new_root->end_root - new_root->start_root;
			new_root->end_root = new_root->start_root + size;
			g_assert (((new_root->root_desc != 0) && (descr != NULL)) ||
					  ((new_root->root_desc == 0) && (descr == NULL)));
			new_root->root_desc = (mword)descr;
			roots_size += size;
			roots_size -= old_size;
			UNLOCK_GC;
			return TRUE;
		}
	}
	new_root = get_internal_mem (sizeof (RootRecord), INTERNAL_MEM_ROOT_RECORD);
	if (new_root) {
		new_root->start_root = start;
		new_root->end_root = new_root->start_root + size;
		new_root->root_desc = (mword)descr;
		roots_size += size;
		hash = addr_hash % roots_hash_size [root_type];
		num_roots_entries [root_type]++;
		new_root->next = roots_hash [root_type] [hash];
		roots_hash [root_type][hash] = new_root;
		DEBUG (3, fprintf (gc_debug_file, "Added root %p for range: %p-%p, descr: %p  (%d/%d bytes)\n", new_root, new_root->start_root, new_root->end_root, descr, (int)size, (int)roots_size));
	} else {
		UNLOCK_GC;
		return FALSE;
	}
	UNLOCK_GC;
	return TRUE;
}

int
mono_gc_register_root (char *start, size_t size, void *descr)
{
	return mono_gc_register_root_inner (start, size, descr, descr ? ROOT_TYPE_NORMAL : ROOT_TYPE_PINNED);
}

int
mono_gc_register_root_wbarrier (char *start, size_t size, void *descr)
{
	return mono_gc_register_root_inner (start, size, descr, ROOT_TYPE_WBARRIER);
}

void
mono_gc_deregister_root (char* addr)
{
	RootRecord *tmp, *prev;
	unsigned int hash, addr_hash = mono_aligned_addr_hash (addr);
	int root_type;

	LOCK_GC;
	for (root_type = 0; root_type < ROOT_TYPE_NUM; ++root_type) {
		hash = addr_hash % roots_hash_size [root_type];
		tmp = roots_hash [root_type][hash];
		prev = NULL;
		while (tmp) {
			if (tmp->start_root == (char*)addr) {
				if (prev)
					prev->next = tmp->next;
				else
					roots_hash [root_type][hash] = tmp->next;
				roots_size -= (tmp->end_root - tmp->start_root);
				num_roots_entries [root_type]--;
				DEBUG (3, fprintf (gc_debug_file, "Removed root %p for range: %p-%p\n", tmp, tmp->start_root, tmp->end_root));
				free_internal_mem (tmp, INTERNAL_MEM_ROOT_RECORD);
				break;
			}
			prev = tmp;
			tmp = tmp->next;
		}
	}
	UNLOCK_GC;
}

/*
 * ######################################################################
 * ########  Thread handling (stop/start code)
 * ######################################################################
 */

/* FIXME: handle large/small config */
#define THREAD_HASH_SIZE 11
#define HASH_PTHREAD_T(id) (((unsigned int)(id) >> 4) * 2654435761u)

static SgenThreadInfo* thread_table [THREAD_HASH_SIZE];

#if USE_SIGNAL_BASED_START_STOP_WORLD

static MonoSemType suspend_ack_semaphore;
static MonoSemType *suspend_ack_semaphore_ptr;
static unsigned int global_stop_count = 0;
#ifdef __APPLE__
static int suspend_signal_num = SIGXFSZ;
#else
static int suspend_signal_num = SIGPWR;
#endif
static int restart_signal_num = SIGXCPU;
static sigset_t suspend_signal_mask;
static mword cur_thread_regs [ARCH_NUM_REGS] = {0};

/* LOCKING: assumes the GC lock is held */
static SgenThreadInfo*
thread_info_lookup (ARCH_THREAD_TYPE id)
{
	unsigned int hash = HASH_PTHREAD_T (id) % THREAD_HASH_SIZE;
	SgenThreadInfo *info;

	info = thread_table [hash];
	while (info && !ARCH_THREAD_EQUALS (info->id, id)) {
		info = info->next;
	}
	return info;
}

static void
update_current_thread_stack (void *start)
{
	void *ptr = cur_thread_regs;
	SgenThreadInfo *info = thread_info_lookup (ARCH_GET_THREAD ());
	info->stack_start = align_pointer (&ptr);
	g_assert (info->stack_start >= info->stack_start_limit && info->stack_start < info->stack_end);
	ARCH_STORE_REGS (ptr);
	info->stopped_regs = ptr;
	if (gc_callbacks.thread_suspend_func)
		gc_callbacks.thread_suspend_func (info->runtime_data, NULL);
}

static const char*
signal_desc (int signum)
{
	if (signum == suspend_signal_num)
		return "suspend";
	if (signum == restart_signal_num)
		return "restart";
	return "unknown";
}

/*
 * Define this and use the "xdomain-checks" MONO_GC_DEBUG option to
 * have cross-domain checks in the write barrier.
 */
//#define XDOMAIN_CHECKS_IN_WBARRIER

#ifndef HEAVY_STATISTICS
#define MANAGED_ALLOCATION
#ifndef XDOMAIN_CHECKS_IN_WBARRIER
#define MANAGED_WBARRIER
#endif
#endif

static gboolean
is_ip_in_managed_allocator (MonoDomain *domain, gpointer ip);

static void
wait_for_suspend_ack (int count)
{
	int i, result;

	for (i = 0; i < count; ++i) {
		while ((result = MONO_SEM_WAIT (suspend_ack_semaphore_ptr)) != 0) {
			if (errno != EINTR) {
				g_error ("sem_wait ()");
			}
		}
	}
}

/* LOCKING: assumes the GC lock is held */
static int
thread_handshake (int signum)
{
	int count, i, result;
	SgenThreadInfo *info;
	pthread_t me = pthread_self ();

	count = 0;
	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			DEBUG (4, fprintf (gc_debug_file, "considering thread %p for signal %d (%s)\n", info, signum, signal_desc (signum)));
			if (ARCH_THREAD_EQUALS (info->id, me)) {
				DEBUG (4, fprintf (gc_debug_file, "Skip (equal): %p, %p\n", (void*)me, (void*)info->id));
				continue;
			}
			/*if (signum == suspend_signal_num && info->stop_count == global_stop_count)
				continue;*/
			result = pthread_kill (info->id, signum);
			if (result == 0) {
				DEBUG (4, fprintf (gc_debug_file, "thread %p signal sent\n", info));
				count++;
			} else {
				DEBUG (4, fprintf (gc_debug_file, "thread %p signal failed: %d (%s)\n", (void*)info->id, result, strerror (result)));
				info->skip = 1;
			}
		}
	}

	wait_for_suspend_ack (count);

	return count;
}

static int
restart_threads_until_none_in_managed_allocator (void)
{
	SgenThreadInfo *info;
	int i, result, num_threads_died = 0;
	int sleep_duration = -1;

	for (;;) {
		int restart_count = 0, restarted_count = 0;
		/* restart all threads that stopped in the
		   allocator */
		for (i = 0; i < THREAD_HASH_SIZE; ++i) {
			for (info = thread_table [i]; info; info = info->next) {
				if (info->skip)
					continue;
				if (!info->stack_start ||
						is_ip_in_managed_allocator (info->stopped_domain, info->stopped_ip)) {
					result = pthread_kill (info->id, restart_signal_num);
					if (result == 0) {
						++restart_count;
					} else {
						info->skip = 1;
					}
				} else {
					/* we set the stopped_ip to
					   NULL for threads which
					   we're not restarting so
					   that we can easily identify
					   the others */
					info->stopped_ip = NULL;
					info->stopped_domain = NULL;
				}
			}
		}
		/* if no threads were restarted, we're done */
		if (restart_count == 0)
			break;

		/* wait for the threads to signal their restart */
		wait_for_suspend_ack (restart_count);

		if (sleep_duration < 0) {
			sched_yield ();
			sleep_duration = 0;
		} else {
			g_usleep (sleep_duration);
			sleep_duration += 10;
		}

		/* stop them again */
		for (i = 0; i < THREAD_HASH_SIZE; ++i) {
			for (info = thread_table [i]; info; info = info->next) {
				if (info->skip || info->stopped_ip == NULL)
					continue;
				result = pthread_kill (info->id, suspend_signal_num);
				if (result == 0) {
					++restarted_count;
				} else {
					info->skip = 1;
				}
			}
		}
		/* some threads might have died */
		num_threads_died += restart_count - restarted_count;
		/* wait for the threads to signal their suspension
		   again */
		wait_for_suspend_ack (restart_count);
	}

	return num_threads_died;
}

/* LOCKING: assumes the GC lock is held (by the stopping thread) */
static void
suspend_handler (int sig, siginfo_t *siginfo, void *context)
{
	SgenThreadInfo *info;
	pthread_t id;
	int stop_count;
	int old_errno = errno;
	gpointer regs [ARCH_NUM_REGS];
	gpointer stack_start;

	id = pthread_self ();
	info = thread_info_lookup (id);
	info->stopped_domain = mono_domain_get ();
	info->stopped_ip = (gpointer) ARCH_SIGCTX_IP (context);
	stop_count = global_stop_count;
	/* duplicate signal */
	if (0 && info->stop_count == stop_count) {
		errno = old_errno;
		return;
	}
#ifdef HAVE_KW_THREAD
	/* update the remset info in the thread data structure */
	info->remset = remembered_set;
#endif
	stack_start = (char*) ARCH_SIGCTX_SP (context) - REDZONE_SIZE;
	/* If stack_start is not within the limits, then don't set it
	   in info and we will be restarted. */
	if (stack_start >= info->stack_start_limit && info->stack_start <= info->stack_end) {
		info->stack_start = stack_start;

		ARCH_COPY_SIGCTX_REGS (regs, context);
		info->stopped_regs = regs;
	} else {
		g_assert (!info->stack_start);
	}

	/* Notify the JIT */
	if (gc_callbacks.thread_suspend_func)
		gc_callbacks.thread_suspend_func (info->runtime_data, context);

	/* notify the waiting thread */
	MONO_SEM_POST (suspend_ack_semaphore_ptr);
	info->stop_count = stop_count;

	/* wait until we receive the restart signal */
	do {
		info->signal = 0;
		sigsuspend (&suspend_signal_mask);
	} while (info->signal != restart_signal_num);

	/* notify the waiting thread */
	MONO_SEM_POST (suspend_ack_semaphore_ptr);

	errno = old_errno;
}

static void
restart_handler (int sig)
{
	SgenThreadInfo *info;
	int old_errno = errno;

	info = thread_info_lookup (pthread_self ());
	info->signal = restart_signal_num;

	errno = old_errno;
}

static TV_DECLARE (stop_world_time);
static unsigned long max_pause_usec = 0;

/* LOCKING: assumes the GC lock is held */
static int
stop_world (void)
{
	int count;

	update_current_thread_stack (&count);

	global_stop_count++;
	DEBUG (3, fprintf (gc_debug_file, "stopping world n %d from %p %p\n", global_stop_count, thread_info_lookup (ARCH_GET_THREAD ()), (gpointer)ARCH_GET_THREAD ()));
	TV_GETTIME (stop_world_time);
	count = thread_handshake (suspend_signal_num);
	count -= restart_threads_until_none_in_managed_allocator ();
	g_assert (count >= 0);
	DEBUG (3, fprintf (gc_debug_file, "world stopped %d thread(s)\n", count));
	return count;
}

/* LOCKING: assumes the GC lock is held */
static int
restart_world (void)
{
	int count, i;
	SgenThreadInfo *info;
	TV_DECLARE (end_sw);
	unsigned long usec;

	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			info->stack_start = NULL;
			info->stopped_regs = NULL;
		}
	}

	count = thread_handshake (restart_signal_num);
	TV_GETTIME (end_sw);
	usec = TV_ELAPSED (stop_world_time, end_sw);
	max_pause_usec = MAX (usec, max_pause_usec);
	DEBUG (2, fprintf (gc_debug_file, "restarted %d thread(s) (pause time: %d usec, max: %d)\n", count, (int)usec, (int)max_pause_usec));
	return count;
}

#endif /* USE_SIGNAL_BASED_START_STOP_WORLD */

void
mono_gc_set_gc_callbacks (MonoGCCallbacks *callbacks)
{
	gc_callbacks = *callbacks;
}

/* Variables holding start/end nursery so it won't have to be passed at every call */
static void *scan_area_arg_start, *scan_area_arg_end;

void
mono_gc_conservatively_scan_area (void *start, void *end)
{
	g_assert_not_reached ();
	conservatively_pin_objects_from (start, end, scan_area_arg_start, scan_area_arg_end, PIN_TYPE_OTHER);
}

void*
mono_gc_scan_object (void *obj)
{
	return copy_object (obj, scan_area_arg_start, scan_area_arg_end);
}
	
/*
 * Mark from thread stacks and registers.
 */
static void
scan_thread_data (void *start_nursery, void *end_nursery, gboolean precise)
{
	int i;
	SgenThreadInfo *info;

	scan_area_arg_start = start_nursery;
	scan_area_arg_end = end_nursery;

	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			if (info->skip) {
				DEBUG (2, fprintf (gc_debug_file, "Skipping dead thread %p, range: %p-%p, size: %zd\n", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start));
				continue;
			}
			DEBUG (3, fprintf (gc_debug_file, "Scanning thread %p, range: %p-%p, size: %zd, pinned=%d\n", info, info->stack_start, info->stack_end, (char*)info->stack_end - (char*)info->stack_start, next_pin_slot));
			if (gc_callbacks.thread_mark_func)
				gc_callbacks.thread_mark_func (info->runtime_data, info->stack_start, info->stack_end, precise);
			else if (!precise)
				conservatively_pin_objects_from (info->stack_start, info->stack_end, start_nursery, end_nursery, PIN_TYPE_STACK);

			if (!precise)
				conservatively_pin_objects_from (info->stopped_regs, info->stopped_regs + ARCH_NUM_REGS,
						start_nursery, end_nursery, PIN_TYPE_STACK);
		}
	}
}

static void
find_pinning_ref_from_thread (char *obj, size_t size)
{
	int i;
	SgenThreadInfo *info;
	char *endobj = obj + size;

	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			char **start = (char**)info->stack_start;
			if (info->skip)
				continue;
			while (start < (char**)info->stack_end) {
				if (*start >= obj && *start < endobj) {
					DEBUG (0, fprintf (gc_debug_file, "Object %p referenced in thread %p (id %p) at %p, stack: %p-%p\n", obj, info, (gpointer)info->id, start, info->stack_start, info->stack_end));
				}
				start++;
			}

			/* FIXME: check info->stopped_regs */
		}
	}
}

static gboolean
ptr_on_stack (void *ptr)
{
	gpointer stack_start = &stack_start;
	SgenThreadInfo *info = thread_info_lookup (ARCH_GET_THREAD ());

	if (ptr >= stack_start && ptr < (gpointer)info->stack_end)
		return TRUE;
	return FALSE;
}

/* return TRUE if ptr points inside the managed heap */
static gboolean
ptr_in_heap (void* ptr)
{
	mword p = (mword)ptr;
	LOSObject *bigobj;
	GCMemSection *section;

	if (!ADDR_IN_HEAP_BOUNDARIES (p))
		return FALSE;

	if (ptr_in_nursery (ptr))
		return TRUE;

	if (ptr_on_stack (ptr))
		return FALSE;

	for (section = section_list; section; section = section->block.next) {
		if (ptr >= (gpointer)section->data && ptr < (gpointer)(section->data + section->size))
			return TRUE;
	}

	if (obj_is_from_pinned_alloc (ptr))
		return TRUE;

	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		if (ptr >= (gpointer)bigobj->data && ptr < (gpointer)(bigobj->data + bigobj->size))
			return TRUE;
	}

	return FALSE;
}

static mword*
handle_remset (mword *p, void *start_nursery, void *end_nursery, gboolean global)
{
	void **ptr;
	mword count;
	mword desc;

	if (global)
		HEAVY_STAT (++stat_global_remsets_processed);

	/* FIXME: exclude stack locations */
	switch ((*p) & REMSET_TYPE_MASK) {
	case REMSET_LOCATION:
		ptr = (void**)(*p);
		//__builtin_prefetch (ptr);
		if (((void*)ptr < start_nursery || (void*)ptr >= end_nursery)) {
			*ptr = copy_object (*ptr, start_nursery, end_nursery);
			DEBUG (9, fprintf (gc_debug_file, "Overwrote remset at %p with %p\n", ptr, *ptr));
			if (!global && *ptr >= start_nursery && *ptr < end_nursery) {
				/*
				 * If the object is pinned, each reference to it from nonpinned objects
				 * becomes part of the global remset, which can grow very large.
				 */
				DEBUG (9, fprintf (gc_debug_file, "Add to global remset because of pinning %p (%p %s)\n", ptr, *ptr, safe_name (*ptr)));
				add_to_global_remset (ptr, FALSE);
			}
		} else {
			DEBUG (9, fprintf (gc_debug_file, "Skipping remset at %p holding %p\n", ptr, *ptr));
		}
		return p + 1;
	case REMSET_RANGE:
		ptr = (void**)(*p & ~REMSET_TYPE_MASK);
		if (((void*)ptr >= start_nursery && (void*)ptr < end_nursery))
			return p + 2;
		count = p [1];
		while (count-- > 0) {
			*ptr = copy_object (*ptr, start_nursery, end_nursery);
			DEBUG (9, fprintf (gc_debug_file, "Overwrote remset at %p with %p (count: %d)\n", ptr, *ptr, (int)count));
			if (!global && *ptr >= start_nursery && *ptr < end_nursery)
				add_to_global_remset (ptr, FALSE);
			++ptr;
		}
		return p + 2;
	case REMSET_OBJECT:
		ptr = (void**)(*p & ~REMSET_TYPE_MASK);
		if (((void*)ptr >= start_nursery && (void*)ptr < end_nursery))
			return p + 1;
		scan_object ((char*)ptr, start_nursery, end_nursery);
		return p + 1;
	case REMSET_OTHER: {
		ptr = (void**)(*p & ~REMSET_TYPE_MASK);

		switch (p [1]) {
		case REMSET_VTYPE:
			if (((void*)ptr >= start_nursery && (void*)ptr < end_nursery))
				return p + 4;
			desc = p [2];
			count = p [3];
			while (count-- > 0)
				ptr = (void**) scan_vtype ((char*)ptr, desc, start_nursery, end_nursery);
			return p + 4;
		case REMSET_ROOT_LOCATION:
			/* Same as REMSET_LOCATION, but the address is not required to be in the heap */
			*ptr = copy_object (*ptr, start_nursery, end_nursery);
			DEBUG (9, fprintf (gc_debug_file, "Overwrote root location remset at %p with %p\n", ptr, *ptr));
			if (!global && *ptr >= start_nursery && *ptr < end_nursery) {
				/*
				 * If the object is pinned, each reference to it from nonpinned objects
				 * becomes part of the global remset, which can grow very large.
				 */
				DEBUG (9, fprintf (gc_debug_file, "Add to global remset because of pinning %p (%p %s)\n", ptr, *ptr, safe_name (*ptr)));
				add_to_global_remset (ptr, TRUE);
			}
			return p + 2;
		default:
			g_assert_not_reached ();
		}
		break;
	}
	default:
		g_assert_not_reached ();
	}
	return NULL;
}

#ifdef HEAVY_STATISTICS
static mword*
collect_store_remsets (RememberedSet *remset, mword *bumper)
{
	mword *p = remset->data;
	mword last = 0;
	mword last1 = 0;
	mword last2 = 0;

	while (p < remset->store_next) {
		switch ((*p) & REMSET_TYPE_MASK) {
		case REMSET_LOCATION:
			*bumper++ = *p;
			if (*p == last)
				++stat_saved_remsets_1;
			last = *p;
			if (*p == last1 || *p == last2) {
				++stat_saved_remsets_2;
			} else {
				last2 = last1;
				last1 = *p;
			}
			p += 1;
			break;
		case REMSET_RANGE:
			p += 2;
			break;
		case REMSET_OBJECT:
			p += 1;
			break;
		case REMSET_OTHER:
			switch (p [1]) {
			case REMSET_VTYPE:
				p += 4;
				break;
			case REMSET_ROOT_LOCATION:
				p += 2;
				break;
			default:
				g_assert_not_reached ();
			}
			break;
		default:
			g_assert_not_reached ();
		}
	}

	return bumper;
}

static void
remset_stats (void)
{
	RememberedSet *remset;
	int size = 0;
	SgenThreadInfo *info;
	int i;
	mword *addresses, *bumper, *p, *r;

	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			for (remset = info->remset; remset; remset = remset->next)
				size += remset->store_next - remset->data;
		}
	}
	for (remset = freed_thread_remsets; remset; remset = remset->next)
		size += remset->store_next - remset->data;
	for (remset = global_remset; remset; remset = remset->next)
		size += remset->store_next - remset->data;

	bumper = addresses = get_internal_mem (sizeof (mword) * size, INTERNAL_MEM_STATISTICS);

	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			for (remset = info->remset; remset; remset = remset->next)
				bumper = collect_store_remsets (remset, bumper);
		}
	}
	for (remset = global_remset; remset; remset = remset->next)
		bumper = collect_store_remsets (remset, bumper);
	for (remset = freed_thread_remsets; remset; remset = remset->next)
		bumper = collect_store_remsets (remset, bumper);

	g_assert (bumper <= addresses + size);

	stat_store_remsets += bumper - addresses;

	sort_addresses ((void**)addresses, bumper - addresses);
	p = addresses;
	r = addresses + 1;
	while (r < bumper) {
		if (*r != *p)
			*++p = *r;
		++r;
	}

	stat_store_remsets_unique += p - addresses;

	free_internal_mem (addresses, INTERNAL_MEM_STATISTICS);
}
#endif

static void
clear_thread_store_remset_buffer (SgenThreadInfo *info)
{
	*info->store_remset_buffer_index_addr = 0;
	memset (*info->store_remset_buffer_addr, 0, sizeof (gpointer) * STORE_REMSET_BUFFER_SIZE);
}

static void
scan_from_remsets (void *start_nursery, void *end_nursery)
{
	int i;
	SgenThreadInfo *info;
	RememberedSet *remset;
	GenericStoreRememberedSet *store_remset;
	mword *p, *next_p, *store_pos;

#ifdef HEAVY_STATISTICS
	remset_stats ();
#endif

	/* the global one */
	for (remset = global_remset; remset; remset = remset->next) {
		DEBUG (4, fprintf (gc_debug_file, "Scanning global remset range: %p-%p, size: %zd\n", remset->data, remset->store_next, remset->store_next - remset->data));
		store_pos = remset->data;
		for (p = remset->data; p < remset->store_next; p = next_p) {
			mword ptr;

			next_p = handle_remset (p, start_nursery, end_nursery, TRUE);

			/* 
			 * Clear global remsets of locations which no longer point to the 
			 * nursery. Otherwise, they could grow indefinitely between major 
			 * collections.
			 */
			ptr = (p [0] & ~REMSET_TYPE_MASK);
			if ((p [0] & REMSET_TYPE_MASK) == REMSET_LOCATION) {
				if (ptr_in_nursery (*(void**)ptr))
					*store_pos ++ = p [0];
			} else {
				g_assert ((p [0] & REMSET_TYPE_MASK) == REMSET_OTHER);
				g_assert (p [1] == REMSET_ROOT_LOCATION);
				if (ptr_in_nursery (*(void**)ptr)) {
					*store_pos ++ = p [0];
					*store_pos ++ = p [1];
				}
			}
		}

		/* Truncate the remset */
		remset->store_next = store_pos;
	}

	/* the generic store ones */
	store_remset = generic_store_remsets;
	while (store_remset) {
		GenericStoreRememberedSet *next = store_remset->next;

		for (i = 0; i < STORE_REMSET_BUFFER_SIZE - 1; ++i) {
			gpointer addr = store_remset->data [i];
			if (addr)
				handle_remset ((mword*)&addr, start_nursery, end_nursery, FALSE);
		}

		free_internal_mem (store_remset, INTERNAL_MEM_STORE_REMSET);

		store_remset = next;
	}
	generic_store_remsets = NULL;

	/* the per-thread ones */
	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			RememberedSet *next;
			int j;
			for (remset = info->remset; remset; remset = next) {
				DEBUG (4, fprintf (gc_debug_file, "Scanning remset for thread %p, range: %p-%p, size: %zd\n", info, remset->data, remset->store_next, remset->store_next - remset->data));
				for (p = remset->data; p < remset->store_next;) {
					p = handle_remset (p, start_nursery, end_nursery, FALSE);
				}
				remset->store_next = remset->data;
				next = remset->next;
				remset->next = NULL;
				if (remset != info->remset) {
					DEBUG (4, fprintf (gc_debug_file, "Freed remset at %p\n", remset->data));
					free_internal_mem (remset, INTERNAL_MEM_REMSET);
				}
			}
			for (j = 0; j < *info->store_remset_buffer_index_addr; ++j)
				handle_remset ((mword*)*info->store_remset_buffer_addr + j + 1, start_nursery, end_nursery, FALSE);
			clear_thread_store_remset_buffer (info);
		}
	}

	/* the freed thread ones */
	while (freed_thread_remsets) {
		RememberedSet *next;
		remset = freed_thread_remsets;
		DEBUG (4, fprintf (gc_debug_file, "Scanning remset for freed thread, range: %p-%p, size: %zd\n", remset->data, remset->store_next, remset->store_next - remset->data));
		for (p = remset->data; p < remset->store_next;) {
			p = handle_remset (p, start_nursery, end_nursery, FALSE);
		}
		next = remset->next;
		DEBUG (4, fprintf (gc_debug_file, "Freed remset at %p\n", remset->data));
		free_internal_mem (remset, INTERNAL_MEM_REMSET);
		freed_thread_remsets = next;
	}
}

/*
 * Clear the info in the remembered sets: we're doing a major collection, so
 * the per-thread ones are not needed and the global ones will be reconstructed
 * during the copy.
 */
static void
clear_remsets (void)
{
	int i;
	SgenThreadInfo *info;
	RememberedSet *remset, *next;

	/* the global list */
	for (remset = global_remset; remset; remset = next) {
		remset->store_next = remset->data;
		next = remset->next;
		remset->next = NULL;
		if (remset != global_remset) {
			DEBUG (4, fprintf (gc_debug_file, "Freed remset at %p\n", remset->data));
			free_internal_mem (remset, INTERNAL_MEM_REMSET);
		}
	}
	/* the generic store ones */
	while (generic_store_remsets) {
		GenericStoreRememberedSet *gs_next = generic_store_remsets->next;
		free_internal_mem (generic_store_remsets, INTERNAL_MEM_STORE_REMSET);
		generic_store_remsets = gs_next;
	}
	/* the per-thread ones */
	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			for (remset = info->remset; remset; remset = next) {
				remset->store_next = remset->data;
				next = remset->next;
				remset->next = NULL;
				if (remset != info->remset) {
					DEBUG (1, fprintf (gc_debug_file, "Freed remset at %p\n", remset->data));
					free_internal_mem (remset, INTERNAL_MEM_REMSET);
				}
			}
			clear_thread_store_remset_buffer (info);
		}
	}

	/* the freed thread ones */
	while (freed_thread_remsets) {
		next = freed_thread_remsets->next;
		DEBUG (4, fprintf (gc_debug_file, "Freed remset at %p\n", freed_thread_remsets->data));
		free_internal_mem (freed_thread_remsets, INTERNAL_MEM_REMSET);
		freed_thread_remsets = next;
	}
}

/*
 * Clear the thread local TLAB variables for all threads.
 */
static void
clear_tlabs (void)
{
	SgenThreadInfo *info;
	int i;

	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			/* A new TLAB will be allocated when the thread does its first allocation */
			*info->tlab_start_addr = NULL;
			*info->tlab_next_addr = NULL;
			*info->tlab_temp_end_addr = NULL;
			*info->tlab_real_end_addr = NULL;
		}
	}
}

/*
 * Find the tlab_next value of the TLAB which contains ADDR.
 */
static char*
find_tlab_next_from_address (char *addr)
{
	SgenThreadInfo *info;
	int i;

	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			/*
			 * The allocator increments tlab_next before
			 * checking whether that address is still in
			 * the TLAB, so we have to check here.
			 */
			char *next_addr = *info->tlab_next_addr;
			char *end_addr = *info->tlab_real_end_addr;
			if (next_addr > end_addr)
				next_addr = end_addr;
			if (addr >= *info->tlab_start_addr && addr < next_addr)
				return next_addr;
		}
	}

	return NULL;
}

/* LOCKING: assumes the GC lock is held */
static SgenThreadInfo*
gc_register_current_thread (void *addr)
{
	int hash;
	SgenThreadInfo* info = malloc (sizeof (SgenThreadInfo));
#ifndef HAVE_KW_THREAD
	SgenThreadInfo *__thread_info__ = info;
#endif

	if (!info)
		return NULL;

#ifndef HAVE_KW_THREAD
	info->tlab_start = info->tlab_next = info->tlab_temp_end = info->tlab_real_end = NULL;

	g_assert (!pthread_getspecific (thread_info_key));
	pthread_setspecific (thread_info_key, info);
#endif

	info->id = ARCH_GET_THREAD ();
	info->stop_count = -1;
	info->skip = 0;
	info->signal = 0;
	info->stack_start = NULL;
	info->tlab_start_addr = &TLAB_START;
	info->tlab_next_addr = &TLAB_NEXT;
	info->tlab_temp_end_addr = &TLAB_TEMP_END;
	info->tlab_real_end_addr = &TLAB_REAL_END;
	info->store_remset_buffer_addr = &STORE_REMSET_BUFFER;
	info->store_remset_buffer_index_addr = &STORE_REMSET_BUFFER_INDEX;
	info->stopped_ip = NULL;
	info->stopped_domain = NULL;
	info->stopped_regs = NULL;

#ifdef HAVE_KW_THREAD
	tlab_next_addr = &tlab_next;
	store_remset_buffer_index_addr = &store_remset_buffer_index;
#endif

	/* try to get it with attributes first */
#if defined(HAVE_PTHREAD_GETATTR_NP) && defined(HAVE_PTHREAD_ATTR_GETSTACK)
	{
		size_t size;
		void *sstart;
		pthread_attr_t attr;
		pthread_getattr_np (pthread_self (), &attr);
		pthread_attr_getstack (&attr, &sstart, &size);
		info->stack_start_limit = sstart;
		info->stack_end = (char*)sstart + size;
		pthread_attr_destroy (&attr);
	}
#elif defined(HAVE_PTHREAD_GET_STACKSIZE_NP) && defined(HAVE_PTHREAD_GET_STACKADDR_NP)
		 info->stack_end = (char*)pthread_get_stackaddr_np (pthread_self ());
#else
	{
		/* FIXME: we assume the stack grows down */
		gsize stack_bottom = (gsize)addr;
		stack_bottom += 4095;
		stack_bottom &= ~4095;
		info->stack_end = (char*)stack_bottom;
	}
#endif

#ifdef HAVE_KW_THREAD
	stack_end = info->stack_end;
#endif

	/* hash into the table */
	hash = HASH_PTHREAD_T (info->id) % THREAD_HASH_SIZE;
	info->next = thread_table [hash];
	thread_table [hash] = info;

	info->remset = alloc_remset (DEFAULT_REMSET_SIZE, info);
	pthread_setspecific (remembered_set_key, info->remset);
#ifdef HAVE_KW_THREAD
	remembered_set = info->remset;
#endif

	STORE_REMSET_BUFFER = get_internal_mem (sizeof (gpointer) * STORE_REMSET_BUFFER_SIZE, INTERNAL_MEM_STORE_REMSET);
	STORE_REMSET_BUFFER_INDEX = 0;

	DEBUG (3, fprintf (gc_debug_file, "registered thread %p (%p) (hash: %d)\n", info, (gpointer)info->id, hash));

	if (gc_callbacks.thread_attach_func)
		info->runtime_data = gc_callbacks.thread_attach_func ();

	return info;
}

static void
add_generic_store_remset_from_buffer (gpointer *buffer)
{
	GenericStoreRememberedSet *remset = get_internal_mem (sizeof (GenericStoreRememberedSet), INTERNAL_MEM_STORE_REMSET);
	memcpy (remset->data, buffer + 1, sizeof (gpointer) * (STORE_REMSET_BUFFER_SIZE - 1));
	remset->next = generic_store_remsets;
	generic_store_remsets = remset;
}

static void
unregister_current_thread (void)
{
	int hash;
	SgenThreadInfo *prev = NULL;
	SgenThreadInfo *p;
	RememberedSet *rset;
	ARCH_THREAD_TYPE id = ARCH_GET_THREAD ();

	hash = HASH_PTHREAD_T (id) % THREAD_HASH_SIZE;
	p = thread_table [hash];
	assert (p);
	DEBUG (3, fprintf (gc_debug_file, "unregister thread %p (%p)\n", p, (gpointer)p->id));
	while (!ARCH_THREAD_EQUALS (p->id, id)) {
		prev = p;
		p = p->next;
	}
	if (prev == NULL) {
		thread_table [hash] = p->next;
	} else {
		prev->next = p->next;
	}
	if (p->remset) {
		if (freed_thread_remsets) {
			for (rset = p->remset; rset->next; rset = rset->next)
				;
			rset->next = freed_thread_remsets;
			freed_thread_remsets = p->remset;
		} else {
			freed_thread_remsets = p->remset;
		}
	}
	if (*p->store_remset_buffer_index_addr)
		add_generic_store_remset_from_buffer (*p->store_remset_buffer_addr);
	free_internal_mem (*p->store_remset_buffer_addr, INTERNAL_MEM_STORE_REMSET);
	free (p);
}

static void
unregister_thread (void *k)
{
	g_assert (!mono_domain_get ());
	LOCK_GC;
	unregister_current_thread ();
	UNLOCK_GC;
}

gboolean
mono_gc_register_thread (void *baseptr)
{
	SgenThreadInfo *info;

	LOCK_GC;
	init_stats ();
	info = thread_info_lookup (ARCH_GET_THREAD ());
	if (info == NULL)
		info = gc_register_current_thread (baseptr);
	UNLOCK_GC;
	return info != NULL;
}

#if USE_PTHREAD_INTERCEPT

#undef pthread_create
#undef pthread_join
#undef pthread_detach

typedef struct {
	void *(*start_routine) (void *);
	void *arg;
	int flags;
	MonoSemType registered;
} SgenThreadStartInfo;

static void*
gc_start_thread (void *arg)
{
	SgenThreadStartInfo *start_info = arg;
	SgenThreadInfo* info;
	void *t_arg = start_info->arg;
	void *(*start_func) (void*) = start_info->start_routine;
	void *result;
	int post_result;

	LOCK_GC;
	info = gc_register_current_thread (&result);
	UNLOCK_GC;
	post_result = MONO_SEM_POST (&(start_info->registered));
	g_assert (!post_result);
	result = start_func (t_arg);
	g_assert (!mono_domain_get ());
	/*
	 * this is done by the pthread key dtor
	LOCK_GC;
	unregister_current_thread ();
	UNLOCK_GC;
	*/

	return result;
}

int
mono_gc_pthread_create (pthread_t *new_thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
	SgenThreadStartInfo *start_info;
	int result;

	start_info = malloc (sizeof (SgenThreadStartInfo));
	if (!start_info)
		return ENOMEM;
	result = MONO_SEM_INIT (&(start_info->registered), 0);
	g_assert (!result);
	start_info->arg = arg;
	start_info->start_routine = start_routine;

	result = pthread_create (new_thread, attr, gc_start_thread, start_info);
	if (result == 0) {
		while (MONO_SEM_WAIT (&(start_info->registered)) != 0) {
			/*if (EINTR != errno) ABORT("sem_wait failed"); */
		}
	}
	MONO_SEM_DESTROY (&(start_info->registered));
	free (start_info);
	return result;
}

int
mono_gc_pthread_join (pthread_t thread, void **retval)
{
	return pthread_join (thread, retval);
}

int
mono_gc_pthread_detach (pthread_t thread)
{
	return pthread_detach (thread);
}

#endif /* USE_PTHREAD_INTERCEPT */

/*
 * ######################################################################
 * ########  Write barriers
 * ######################################################################
 */

static RememberedSet*
alloc_remset (int size, gpointer id) {
	RememberedSet* res = get_internal_mem (sizeof (RememberedSet) + (size * sizeof (gpointer)), INTERNAL_MEM_REMSET);
	res->store_next = res->data;
	res->end_set = res->data + size;
	res->next = NULL;
	DEBUG (4, fprintf (gc_debug_file, "Allocated remset size %d at %p for %p\n", size, res->data, id));
	return res;
}

/*
 * Note: the write barriers first do the needed GC work and then do the actual store:
 * this way the value is visible to the conservative GC scan after the write barrier
 * itself. If a GC interrupts the barrier in the middle, value will be kept alive by
 * the conservative scan, otherwise by the remembered set scan. FIXME: figure out what
 * happens when we need to record which pointers contain references to the new generation.
 * The write barrier will be executed, but the pointer is still not stored.
 */
void
mono_gc_wbarrier_set_field (MonoObject *obj, gpointer field_ptr, MonoObject* value)
{
	RememberedSet *rs;
	TLAB_ACCESS_INIT;
	HEAVY_STAT (++stat_wbarrier_set_field);
	if (ptr_in_nursery (field_ptr)) {
		*(void**)field_ptr = value;
		return;
	}
	DEBUG (8, fprintf (gc_debug_file, "Adding remset at %p\n", field_ptr));
	LOCK_GC;
	rs = REMEMBERED_SET;
	if (rs->store_next < rs->end_set) {
		*(rs->store_next++) = (mword)field_ptr;
		*(void**)field_ptr = value;
		UNLOCK_GC;
		return;
	}
	rs = alloc_remset (rs->end_set - rs->data, (void*)1);
	rs->next = REMEMBERED_SET;
	REMEMBERED_SET = rs;
#ifdef HAVE_KW_THREAD
	thread_info_lookup (ARCH_GET_THREAD ())->remset = rs;
#endif
	*(rs->store_next++) = (mword)field_ptr;
	*(void**)field_ptr = value;
	UNLOCK_GC;
}

void
mono_gc_wbarrier_set_arrayref (MonoArray *arr, gpointer slot_ptr, MonoObject* value)
{
	RememberedSet *rs;
	TLAB_ACCESS_INIT;
	HEAVY_STAT (++stat_wbarrier_set_arrayref);
	if (ptr_in_nursery (slot_ptr)) {
		*(void**)slot_ptr = value;
		return;
	}
	DEBUG (8, fprintf (gc_debug_file, "Adding remset at %p\n", slot_ptr));
	LOCK_GC;
	rs = REMEMBERED_SET;
	if (rs->store_next < rs->end_set) {
		*(rs->store_next++) = (mword)slot_ptr;
		*(void**)slot_ptr = value;
		UNLOCK_GC;
		return;
	}
	rs = alloc_remset (rs->end_set - rs->data, (void*)1);
	rs->next = REMEMBERED_SET;
	REMEMBERED_SET = rs;
#ifdef HAVE_KW_THREAD
	thread_info_lookup (ARCH_GET_THREAD ())->remset = rs;
#endif
	*(rs->store_next++) = (mword)slot_ptr;
	*(void**)slot_ptr = value;
	UNLOCK_GC;
}

void
mono_gc_wbarrier_arrayref_copy (gpointer dest_ptr, gpointer src_ptr, int count)
{
	RememberedSet *rs;
	TLAB_ACCESS_INIT;
	HEAVY_STAT (++stat_wbarrier_arrayref_copy);
	LOCK_GC;
	memmove (dest_ptr, src_ptr, count * sizeof (gpointer));
	if (ptr_in_nursery (dest_ptr)) {
		UNLOCK_GC;
		return;
	}
	rs = REMEMBERED_SET;
	DEBUG (8, fprintf (gc_debug_file, "Adding remset at %p, %d\n", dest_ptr, count));
	if (rs->store_next + 1 < rs->end_set) {
		*(rs->store_next++) = (mword)dest_ptr | REMSET_RANGE;
		*(rs->store_next++) = count;
		UNLOCK_GC;
		return;
	}
	rs = alloc_remset (rs->end_set - rs->data, (void*)1);
	rs->next = REMEMBERED_SET;
	REMEMBERED_SET = rs;
#ifdef HAVE_KW_THREAD
	thread_info_lookup (ARCH_GET_THREAD ())->remset = rs;
#endif
	*(rs->store_next++) = (mword)dest_ptr | REMSET_RANGE;
	*(rs->store_next++) = count;
	UNLOCK_GC;
}

static char*
find_object_for_ptr_in_area (char *ptr, char *start, char *end)
{
	while (start < end) {
		char *old_start;

		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}

		old_start = start;

		#define SCAN_OBJECT_NOSCAN
		#include "sgen-scan-object.h"

		if (ptr >= old_start && ptr < start)
			return old_start;
	}

	return NULL;
}

static char *found_obj;

static void
find_object_for_ptr_in_pinned_chunk_callback (PinnedChunk *chunk, char *obj, size_t size, char *ptr)
{
	if (ptr >= obj && ptr < obj + size) {
		g_assert (!found_obj);
		found_obj = obj;
	}
}

static char*
find_object_for_ptr (char *ptr)
{
	GCMemSection *section;
	LOSObject *bigobj;

	for (section = section_list; section; section = section->block.next) {
		if (ptr >= section->data && ptr < section->end_data)
			return find_object_for_ptr_in_area (ptr, section->data, section->end_data);
	}

	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next) {
		if (ptr >= bigobj->data && ptr < bigobj->data + bigobj->size)
			return bigobj->data;
	}

	found_obj = NULL;
	scan_pinned_objects ((ScanPinnedObjectCallbackFunc)find_object_for_ptr_in_pinned_chunk_callback, ptr);
	return found_obj;
}

static void
evacuate_remset_buffer (void)
{
	gpointer *buffer;
	TLAB_ACCESS_INIT;

	buffer = STORE_REMSET_BUFFER;

	add_generic_store_remset_from_buffer (buffer);
	memset (buffer, 0, sizeof (gpointer) * STORE_REMSET_BUFFER_SIZE);

	STORE_REMSET_BUFFER_INDEX = 0;
}

void
mono_gc_wbarrier_generic_nostore (gpointer ptr)
{
	gpointer *buffer;
	int index;
	TLAB_ACCESS_INIT;

	HEAVY_STAT (++stat_wbarrier_generic_store);

#ifdef XDOMAIN_CHECKS_IN_WBARRIER
	/* FIXME: ptr_in_heap must be called with the GC lock held */
	if (xdomain_checks && *(MonoObject**)ptr && ptr_in_heap (ptr)) {
		char *start = find_object_for_ptr (ptr);
		MonoObject *value = *(MonoObject**)ptr;
		LOCK_GC;
		g_assert (start);
		if (start) {
			MonoObject *obj = (MonoObject*)start;
			if (obj->vtable->domain != value->vtable->domain)
				g_assert (is_xdomain_ref_allowed (ptr, start, obj->vtable->domain));
		}
		UNLOCK_GC;
	}
#endif

	LOCK_GC;
	if (ptr_in_nursery (ptr) || ptr_on_stack (ptr) || !ptr_in_nursery (*(gpointer*)ptr)) {
		DEBUG (8, fprintf (gc_debug_file, "Skipping remset at %p\n", ptr));
		UNLOCK_GC;
		return;
	}

	buffer = STORE_REMSET_BUFFER;
	index = STORE_REMSET_BUFFER_INDEX;
	/* This simple optimization eliminates a sizable portion of
	   entries.  Comparing it to the last but one entry as well
	   doesn't eliminate significantly more entries. */
	if (buffer [index] == ptr) {
		UNLOCK_GC;
		return;
	}

	DEBUG (8, fprintf (gc_debug_file, "Adding remset at %p\n", ptr));
	HEAVY_STAT (++stat_wbarrier_generic_store_remset);

	++index;
	if (index >= STORE_REMSET_BUFFER_SIZE) {
		evacuate_remset_buffer ();
		index = STORE_REMSET_BUFFER_INDEX;
		g_assert (index == 0);
		++index;
	}
	buffer [index] = ptr;
	STORE_REMSET_BUFFER_INDEX = index;

	UNLOCK_GC;
}

void
mono_gc_wbarrier_generic_store (gpointer ptr, MonoObject* value)
{
	DEBUG (8, fprintf (gc_debug_file, "Wbarrier store at %p to %p (%s)\n", ptr, value, value ? safe_name (value) : "null"));
	*(void**)ptr = value;
	if (ptr_in_nursery (value))
		mono_gc_wbarrier_generic_nostore (ptr);
}

void
mono_gc_wbarrier_set_root (gpointer ptr, MonoObject *value)
{
	RememberedSet *rs;
	TLAB_ACCESS_INIT;
	HEAVY_STAT (++stat_wbarrier_set_root);
	if (ptr_in_nursery (ptr))
		return;
	DEBUG (8, fprintf (gc_debug_file, "Adding root remset at %p (%s)\n", ptr, value ? safe_name (value) : "null"));

	rs = REMEMBERED_SET;
	if (rs->store_next + 2 < rs->end_set) {
		*(rs->store_next++) = (mword)ptr | REMSET_OTHER;
		*(rs->store_next++) = (mword)REMSET_ROOT_LOCATION;
		*(void**)ptr = value;
		return;
	}
	rs = alloc_remset (rs->end_set - rs->data, (void*)1);
	rs->next = REMEMBERED_SET;
	REMEMBERED_SET = rs;
#ifdef HAVE_KW_THREAD
	thread_info_lookup (ARCH_GET_THREAD ())->remset = rs;
#endif
	*(rs->store_next++) = (mword)ptr | REMSET_OTHER;
	*(rs->store_next++) = (mword)REMSET_ROOT_LOCATION;

	*(void**)ptr = value;
}

void
mono_gc_wbarrier_value_copy (gpointer dest, gpointer src, int count, MonoClass *klass)
{
	RememberedSet *rs;
	TLAB_ACCESS_INIT;
	HEAVY_STAT (++stat_wbarrier_value_copy);
	g_assert (klass->valuetype);
	LOCK_GC;
	memmove (dest, src, count * mono_class_value_size (klass, NULL));
	rs = REMEMBERED_SET;
	if (ptr_in_nursery (dest) || ptr_on_stack (dest)) {
		UNLOCK_GC;
		return;
	}
	g_assert (klass->gc_descr_inited);
	DEBUG (8, fprintf (gc_debug_file, "Adding value remset at %p, count %d, descr %p for class %s (%p)\n", dest, count, klass->gc_descr, klass->name, klass));

	if (rs->store_next + 3 < rs->end_set) {
		*(rs->store_next++) = (mword)dest | REMSET_OTHER;
		*(rs->store_next++) = (mword)REMSET_VTYPE;
		*(rs->store_next++) = (mword)klass->gc_descr;
		*(rs->store_next++) = (mword)count;
		UNLOCK_GC;
		return;
	}
	rs = alloc_remset (rs->end_set - rs->data, (void*)1);
	rs->next = REMEMBERED_SET;
	REMEMBERED_SET = rs;
#ifdef HAVE_KW_THREAD
	thread_info_lookup (ARCH_GET_THREAD ())->remset = rs;
#endif
	*(rs->store_next++) = (mword)dest | REMSET_OTHER;
	*(rs->store_next++) = (mword)REMSET_VTYPE;
	*(rs->store_next++) = (mword)klass->gc_descr;
	*(rs->store_next++) = (mword)count;
	UNLOCK_GC;
}

/**
 * mono_gc_wbarrier_object_copy:
 *
 * Write barrier to call when obj is the result of a clone or copy of an object.
 */
void
mono_gc_wbarrier_object_copy (MonoObject* obj, MonoObject *src)
{
	RememberedSet *rs;
	int size;

	TLAB_ACCESS_INIT;
	HEAVY_STAT (++stat_wbarrier_object_copy);
	rs = REMEMBERED_SET;
	DEBUG (1, fprintf (gc_debug_file, "Adding object remset for %p\n", obj));
	size = mono_object_class (obj)->instance_size;
	LOCK_GC;
	/* do not copy the sync state */
	memcpy ((char*)obj + sizeof (MonoObject), (char*)src + sizeof (MonoObject),
			size - sizeof (MonoObject));
	if (ptr_in_nursery (obj) || ptr_on_stack (obj)) {
		UNLOCK_GC;
		return;
	}
	if (rs->store_next < rs->end_set) {
		*(rs->store_next++) = (mword)obj | REMSET_OBJECT;
		UNLOCK_GC;
		return;
	}
	rs = alloc_remset (rs->end_set - rs->data, (void*)1);
	rs->next = REMEMBERED_SET;
	REMEMBERED_SET = rs;
#ifdef HAVE_KW_THREAD
	thread_info_lookup (ARCH_GET_THREAD ())->remset = rs;
#endif
	*(rs->store_next++) = (mword)obj | REMSET_OBJECT;
	UNLOCK_GC;
}

/*
 * ######################################################################
 * ########  Collector debugging
 * ######################################################################
 */

const char*descriptor_types [] = {
	"run_length",
	"small_bitmap",
	"string",
	"complex",
	"vector",
	"array",
	"large_bitmap",
	"complex_arr"
};

void
describe_ptr (char *ptr)
{
	GCMemSection *section;
	MonoVTable *vtable;
	mword desc;
	int type;

	if (ptr_in_nursery (ptr)) {
		printf ("Pointer inside nursery.\n");
	} else {
		for (section = section_list; section;) {
			if (ptr >= section->data && ptr < section->data + section->size)
				break;
			section = section->block.next;
		}

		if (section) {
			printf ("Pointer inside oldspace.\n");
		} else if (obj_is_from_pinned_alloc (ptr)) {
			printf ("Pointer is inside a pinned chunk.\n");
		} else {
			printf ("Pointer unknown.\n");
			return;
		}
	}

	if (object_is_pinned (ptr))
		printf ("Object is pinned.\n");

	if (object_is_forwarded (ptr))
		printf ("Object is forwared.\n");

	// FIXME: Handle pointers to the inside of objects
	vtable = (MonoVTable*)LOAD_VTABLE (ptr);

	printf ("VTable: %p\n", vtable);
	if (vtable == NULL) {
		printf ("VTable is invalid (empty).\n");
		return;
	}
	if (ptr_in_nursery (vtable)) {
		printf ("VTable is invalid (points inside nursery).\n");
		return;
	}
	printf ("Class: %s\n", vtable->klass->name);

	desc = ((GCVTable*)vtable)->desc;
	printf ("Descriptor: %lx\n", (long)desc);

	type = desc & 0x7;
	printf ("Descriptor type: %d (%s)\n", type, descriptor_types [type]);
}

static mword*
find_in_remset_loc (mword *p, char *addr, gboolean *found)
{
	void **ptr;
	mword count, desc;
	size_t skip_size;

	switch ((*p) & REMSET_TYPE_MASK) {
	case REMSET_LOCATION:
		if (*p == (mword)addr)
			*found = TRUE;
		return p + 1;
	case REMSET_RANGE:
		ptr = (void**)(*p & ~REMSET_TYPE_MASK);
		count = p [1];
		if ((void**)addr >= ptr && (void**)addr < ptr + count)
			*found = TRUE;
		return p + 2;
	case REMSET_OBJECT:
		ptr = (void**)(*p & ~REMSET_TYPE_MASK);
		count = safe_object_get_size ((MonoObject*)ptr); 
		count += (ALLOC_ALIGN - 1);
		count &= (ALLOC_ALIGN - 1);
		count /= sizeof (mword);
		if ((void**)addr >= ptr && (void**)addr < ptr + count)
			*found = TRUE;
		return p + 1;
	case REMSET_OTHER: {
		switch (p [1]) {
		case REMSET_VTYPE:
			ptr = (void**)(*p & ~REMSET_TYPE_MASK);
			desc = p [2];
			count = p [3];

			switch (desc & 0x7) {
			case DESC_TYPE_RUN_LENGTH:
				OBJ_RUN_LEN_SIZE (skip_size, desc, ptr);
				/* The descriptor includes the size of MonoObject */
				skip_size -= sizeof (MonoObject);
				skip_size *= count;
				if ((void**)addr >= ptr && (void**)addr < ptr + (skip_size / sizeof (gpointer)))
					*found = TRUE;
				break;
			default:
				// FIXME:
				g_assert_not_reached ();
			}

			return p + 4;
		case REMSET_ROOT_LOCATION:
			return p + 2;
		default:
			g_assert_not_reached ();
		}
		break;
	}
	default:
		g_assert_not_reached ();
	}
	return NULL;
}

/*
 * Return whenever ADDR occurs in the remembered sets
 */
static gboolean
find_in_remsets (char *addr)
{
	int i;
	SgenThreadInfo *info;
	RememberedSet *remset;
	GenericStoreRememberedSet *store_remset;
	mword *p;
	gboolean found = FALSE;

	/* the global one */
	for (remset = global_remset; remset; remset = remset->next) {
		DEBUG (4, fprintf (gc_debug_file, "Scanning global remset range: %p-%p, size: %zd\n", remset->data, remset->store_next, remset->store_next - remset->data));
		for (p = remset->data; p < remset->store_next;) {
			p = find_in_remset_loc (p, addr, &found);
			if (found)
				return TRUE;
		}
	}

	/* the generic store ones */
	for (store_remset = generic_store_remsets; store_remset; store_remset = store_remset->next) {
		for (i = 0; i < STORE_REMSET_BUFFER_SIZE - 1; ++i) {
			if (store_remset->data [i] == addr)
				return TRUE;
		}
	}

	/* the per-thread ones */
	for (i = 0; i < THREAD_HASH_SIZE; ++i) {
		for (info = thread_table [i]; info; info = info->next) {
			int j;
			for (remset = info->remset; remset; remset = remset->next) {
				DEBUG (4, fprintf (gc_debug_file, "Scanning remset for thread %p, range: %p-%p, size: %zd\n", info, remset->data, remset->store_next, remset->store_next - remset->data));
				for (p = remset->data; p < remset->store_next;) {
					p = find_in_remset_loc (p, addr, &found);
					if (found)
						return TRUE;
				}
			}
			for (j = 0; j < *info->store_remset_buffer_index_addr; ++j) {
				if ((*info->store_remset_buffer_addr) [j + 1] == addr)
					return TRUE;
			}
		}
	}

	/* the freed thread ones */
	for (remset = freed_thread_remsets; remset; remset = remset->next) {
		DEBUG (4, fprintf (gc_debug_file, "Scanning remset for freed thread, range: %p-%p, size: %zd\n", remset->data, remset->store_next, remset->store_next - remset->data));
		for (p = remset->data; p < remset->store_next;) {
			p = find_in_remset_loc (p, addr, &found);
			if (found)
				return TRUE;
		}
	}

	return FALSE;
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr) && (char*)*(ptr) >= nursery_start && (char*)*(ptr) < nursery_next) {	\
            if (!find_in_remsets ((char*)(ptr))) { \
                fprintf (gc_debug_file, "Oldspace->newspace reference %p at offset %zd in object %p (%s.%s) not found in remsets.\n", *(ptr), (char*)(ptr) - (char*)(obj), (obj), ((MonoObject*)(obj))->vtable->klass->name_space, ((MonoObject*)(obj))->vtable->klass->name); \
                g_assert_not_reached (); \
            } \
        } \
	} while (0)

/*
 * Check that each object reference inside the area which points into the nursery
 * can be found in the remembered sets.
 */
static void __attribute__((noinline))
check_remsets_for_area (char *start, char *end)
{
	GCVTable *vt;
	int type_str = 0, type_rlen = 0, type_bitmap = 0, type_vector = 0, type_lbit = 0, type_complex = 0;
	new_obj_references = 0;
	obj_references_checked = 0;
	while (start < end) {
		if (!*(void**)start) {
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}
		vt = (GCVTable*)LOAD_VTABLE (start);
		DEBUG (8, fprintf (gc_debug_file, "Scanning object %p, vtable: %p (%s)\n", start, vt, vt->klass->name));
		if (0) {
			MonoObject *obj = (MonoObject*)start;
			g_print ("found at %p (0x%lx): %s.%s\n", start, (long)vt->desc, obj->vtable->klass->name_space, obj->vtable->klass->name);
		}

#define SCAN_OBJECT_ACTION COUNT_OBJECT_TYPES
#include "sgen-scan-object.h"
	}
}

/*
 * Perform consistency check of the heap.
 *
 * Assumes the world is stopped.
 */
void
check_consistency (void)
{
	GCMemSection *section;

	// Need to add more checks
	// FIXME: Create a general heap enumeration function and use that

	DEBUG (1, fprintf (gc_debug_file, "Begin heap consistency check...\n"));

	// Check that oldspace->newspace pointers are registered with the collector
	for (section = section_list; section; section = section->block.next) {
		if (section->block.role == MEMORY_ROLE_GEN0)
			continue;
		DEBUG (2, fprintf (gc_debug_file, "Scan of old section: %p-%p, size: %d\n", section->data, section->next_data, (int)(section->next_data - section->data)));
		check_remsets_for_area (section->data, section->next_data);
	}

	DEBUG (1, fprintf (gc_debug_file, "Heap consistency check done.\n"));
}

/* Check that the reference is valid */
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr)) {	\
			g_assert (safe_name (*(ptr)) != NULL);	\
		}	\
	} while (0)

/*
 * check_object:
 *
 *   Perform consistency check on an object. Currently we only check that the
 * reference fields are valid.
 */
char*
check_object (char *start)
{
	if (!start)
		return NULL;

#include "sgen-scan-object.h"

	return start;
}

/*
 * ######################################################################
 * ########  Other mono public interface functions.
 * ######################################################################
 */

void
mono_gc_collect (int generation)
{
	LOCK_GC;
	stop_world ();
	if (generation == 0) {
		collect_nursery (0);
	} else {
		major_collection ("user request");
	}
	restart_world ();
	UNLOCK_GC;
}

int
mono_gc_max_generation (void)
{
	return 1;
}

int
mono_gc_collection_count (int generation)
{
	if (generation == 0)
		return num_minor_gcs;
	return num_major_gcs;
}

gint64
mono_gc_get_used_size (void)
{
	gint64 tot = 0;
	GCMemSection *section;
	LOCK_GC;
	tot = los_memory_usage;
	for (section = section_list; section; section = section->block.next) {
		/* this is approximate... */
		tot += section->next_data - section->data;
	}
	/* FIXME: account for pinned objects */
	UNLOCK_GC;
	return tot;
}

gint64
mono_gc_get_heap_size (void)
{
	return total_alloc;
}

void
mono_gc_disable (void)
{
	LOCK_GC;
	gc_disabled++;
	UNLOCK_GC;
}

void
mono_gc_enable (void)
{
	LOCK_GC;
	gc_disabled--;
	UNLOCK_GC;
}

int
mono_gc_get_los_limit (void)
{
	return MAX_SMALL_OBJ_SIZE;
}

gboolean
mono_object_is_alive (MonoObject* o)
{
	return TRUE;
}

int
mono_gc_get_generation (MonoObject *obj)
{
	if (ptr_in_nursery (obj))
		return 0;
	return 1;
}

void
mono_gc_enable_events (void)
{
}

void
mono_gc_weak_link_add (void **link_addr, MonoObject *obj, gboolean track)
{
	LOCK_GC;
	mono_gc_register_disappearing_link (obj, link_addr, track);
	UNLOCK_GC;
}

void
mono_gc_weak_link_remove (void **link_addr)
{
	LOCK_GC;
	mono_gc_register_disappearing_link (NULL, link_addr, FALSE);
	UNLOCK_GC;
}

MonoObject*
mono_gc_weak_link_get (void **link_addr)
{
	if (!*link_addr)
		return NULL;
	return (MonoObject*) REVEAL_POINTER (*link_addr);
}

void*
mono_gc_make_descr_from_bitmap (gsize *bitmap, int numbits)
{
	if (numbits < ((sizeof (*bitmap) * 8) - ROOT_DESC_TYPE_SHIFT)) {
		return (void*)MAKE_ROOT_DESC (ROOT_DESC_BITMAP, bitmap [0]);
	} else {
		mword complex = alloc_complex_descriptor (bitmap, numbits + 1);
		return (void*)MAKE_ROOT_DESC (ROOT_DESC_COMPLEX, complex);
	}
}

void*
mono_gc_make_root_descr_user (MonoGCMarkFunc marker)
{
	void *descr;

	g_assert (user_descriptors_next < MAX_USER_DESCRIPTORS);
	descr = (void*)MAKE_ROOT_DESC (ROOT_DESC_USER, (mword)user_descriptors_next);
	user_descriptors [user_descriptors_next ++] = marker;

	return descr;
}

void*
mono_gc_alloc_fixed (size_t size, void *descr)
{
	/* FIXME: do a single allocation */
	void *res = calloc (1, size);
	if (!res)
		return NULL;
	if (!mono_gc_register_root (res, size, descr)) {
		free (res);
		res = NULL;
	}
	return res;
}

void
mono_gc_free_fixed (void* addr)
{
	mono_gc_deregister_root (addr);
	free (addr);
}

gboolean
mono_gc_is_gc_thread (void)
{
	gboolean result;
	LOCK_GC;
        result = thread_info_lookup (ARCH_GET_THREAD ()) != NULL;
	UNLOCK_GC;
	return result;
}

void
mono_gc_base_init (void)
{
	char *env;
	char **opts, **ptr;
	struct sigaction sinfo;

	LOCK_INIT (gc_mutex);
	LOCK_GC;
	if (gc_initialized) {
		UNLOCK_GC;
		return;
	}
	pagesize = mono_pagesize ();
	gc_debug_file = stderr;
	if ((env = getenv ("MONO_GC_DEBUG"))) {
		opts = g_strsplit (env, ",", -1);
		for (ptr = opts; ptr && *ptr; ptr ++) {
			char *opt = *ptr;
			if (opt [0] >= '0' && opt [0] <= '9') {
				gc_debug_level = atoi (opt);
				opt++;
				if (opt [0] == ':')
					opt++;
				if (opt [0]) {
					char *rf = g_strdup_printf ("%s.%d", opt, getpid ());
					gc_debug_file = fopen (rf, "wb");
					if (!gc_debug_file)
						gc_debug_file = stderr;
					g_free (rf);
				}
			} else if (!strcmp (opt, "collect-before-allocs")) {
				collect_before_allocs = TRUE;
			} else if (!strcmp (opt, "check-at-minor-collections")) {
				consistency_check_at_minor_collection = TRUE;
			} else if (!strcmp (opt, "xdomain-checks")) {
				xdomain_checks = TRUE;
			} else if (!strcmp (opt, "clear-at-gc")) {
				nursery_clear_policy = CLEAR_AT_GC;
			} else if (g_str_has_prefix (opt, "heap-dump=")) {
				char *filename = strchr (opt, '=') + 1;
				nursery_clear_policy = CLEAR_AT_GC;
				heap_dump_file = fopen (filename, "w");
				if (heap_dump_file)
					fprintf (heap_dump_file, "<sgen-dump>\n");
			} else {
				fprintf (stderr, "Invalid format for the MONO_GC_DEBUG env variable: '%s'\n", env);
				fprintf (stderr, "The format is: MONO_GC_DEBUG=[l[:filename]|<option>]+ where l is a debug level 0-9.\n");
				fprintf (stderr, "Valid options are: collect-before-allocs, check-at-minor-collections, xdomain-checks, clear-at-gc.\n");
				exit (1);
			}
		}
		g_strfreev (opts);
	}

	suspend_ack_semaphore_ptr = &suspend_ack_semaphore;
	MONO_SEM_INIT (&suspend_ack_semaphore, 0);

	sigfillset (&sinfo.sa_mask);
	sinfo.sa_flags = SA_RESTART | SA_SIGINFO;
	sinfo.sa_sigaction = suspend_handler;
	if (sigaction (suspend_signal_num, &sinfo, NULL) != 0) {
		g_error ("failed sigaction");
	}

	sinfo.sa_handler = restart_handler;
	if (sigaction (restart_signal_num, &sinfo, NULL) != 0) {
		g_error ("failed sigaction");
	}

	sigfillset (&suspend_signal_mask);
	sigdelset (&suspend_signal_mask, restart_signal_num);

	global_remset = alloc_remset (1024, NULL);
	global_remset->next = NULL;

	pthread_key_create (&remembered_set_key, unregister_thread);

#ifndef HAVE_KW_THREAD
	pthread_key_create (&thread_info_key, NULL);
#endif

	gc_initialized = TRUE;
	UNLOCK_GC;
	mono_gc_register_thread (&sinfo);
}

int
mono_gc_get_suspend_signal (void)
{
	return suspend_signal_num;
}

enum {
	ATYPE_NORMAL,
	ATYPE_VECTOR,
	ATYPE_NUM
};

#ifdef HAVE_KW_THREAD
#define EMIT_TLS_ACCESS(mb,dummy,offset)	do {	\
	mono_mb_emit_byte ((mb), MONO_CUSTOM_PREFIX);	\
	mono_mb_emit_byte ((mb), CEE_MONO_TLS);		\
	mono_mb_emit_i4 ((mb), (offset));		\
	} while (0)
#else
#define EMIT_TLS_ACCESS(mb,member,dummy)	do {	\
	mono_mb_emit_byte ((mb), MONO_CUSTOM_PREFIX);	\
	mono_mb_emit_byte ((mb), CEE_MONO_TLS);		\
	mono_mb_emit_i4 ((mb), thread_info_key);	\
	mono_mb_emit_icon ((mb), G_STRUCT_OFFSET (SgenThreadInfo, member));	\
	mono_mb_emit_byte ((mb), CEE_ADD);		\
	mono_mb_emit_byte ((mb), CEE_LDIND_I);		\
	} while (0)
#endif

#ifdef MANAGED_ALLOCATION
/* FIXME: Do this in the JIT, where specialized allocation sequences can be created
 * for each class. This is currently not easy to do, as it is hard to generate basic 
 * blocks + branches, but it is easy with the linear IL codebase.
 *
 * For this to work we'd need to solve the TLAB race, first.  Now we
 * require the allocator to be in a few known methods to make sure
 * that they are executed atomically via the restart mechanism.
 */
static MonoMethod*
create_allocator (int atype)
{
	int p_var, size_var;
	guint32 slowpath_branch, max_size_branch;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	MonoMethodSignature *csig;
	static gboolean registered = FALSE;
	int tlab_next_addr_var, new_next_var;
	int num_params, i;

#ifdef HAVE_KW_THREAD
	int tlab_next_addr_offset = -1;
	int tlab_temp_end_offset = -1;

	MONO_THREAD_VAR_OFFSET (tlab_next_addr, tlab_next_addr_offset);
	MONO_THREAD_VAR_OFFSET (tlab_temp_end, tlab_temp_end_offset);

	g_assert (tlab_next_addr_offset != -1);
	g_assert (tlab_temp_end_offset != -1);
#endif

	if (!registered) {
		mono_register_jit_icall (mono_gc_alloc_obj, "mono_gc_alloc_obj", mono_create_icall_signature ("object ptr int"), FALSE);
		mono_register_jit_icall (mono_gc_alloc_vector, "mono_gc_alloc_vector", mono_create_icall_signature ("object ptr int int"), FALSE);
		registered = TRUE;
	}

	if (atype == ATYPE_NORMAL)
		num_params = 1;
	else if (atype == ATYPE_VECTOR)
		num_params = 2;
	else
		g_assert_not_reached ();

	csig = mono_metadata_signature_alloc (mono_defaults.corlib, num_params);
	csig->ret = &mono_defaults.object_class->byval_arg;
	for (i = 0; i < num_params; ++i)
		csig->params [i] = &mono_defaults.int_class->byval_arg;

	mb = mono_mb_new (mono_defaults.object_class, "Alloc", MONO_WRAPPER_ALLOC);
	size_var = mono_mb_add_local (mb, &mono_defaults.int32_class->byval_arg);
	if (atype == ATYPE_NORMAL) {
		/* size = vtable->klass->instance_size; */
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_icon (mb, G_STRUCT_OFFSET (MonoVTable, klass));
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_byte (mb, CEE_LDIND_I);
		mono_mb_emit_icon (mb, G_STRUCT_OFFSET (MonoClass, instance_size));
		mono_mb_emit_byte (mb, CEE_ADD);
		/* FIXME: assert instance_size stays a 4 byte integer */
		mono_mb_emit_byte (mb, CEE_LDIND_U4);
		mono_mb_emit_stloc (mb, size_var);
	} else if (atype == ATYPE_VECTOR) {
		MonoExceptionClause *clause;
		int pos_leave;
		MonoClass *oom_exc_class;
		MonoMethod *ctor;

		clause = mono_image_alloc0 (mono_defaults.corlib, sizeof (MonoExceptionClause));
		clause->try_offset = mono_mb_get_label (mb);

		/* vtable->klass->sizes.element_size */
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_icon (mb, G_STRUCT_OFFSET (MonoVTable, klass));
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_byte (mb, CEE_LDIND_I);
		mono_mb_emit_icon (mb, G_STRUCT_OFFSET (MonoClass, sizes.element_size));
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_byte (mb, CEE_LDIND_U4);

		/* * n */
		mono_mb_emit_ldarg (mb, 1);
		mono_mb_emit_byte (mb, CEE_MUL_OVF_UN);
		/* + sizeof (MonoArray) */
		mono_mb_emit_icon (mb, sizeof (MonoArray));
		mono_mb_emit_byte (mb, CEE_ADD_OVF_UN);
		mono_mb_emit_stloc (mb, size_var);

		pos_leave = mono_mb_emit_branch (mb, CEE_LEAVE);

		/* catch */
		clause->flags = MONO_EXCEPTION_CLAUSE_NONE;
		clause->try_len = mono_mb_get_pos (mb) - clause->try_offset;
		clause->data.catch_class = mono_class_from_name (mono_defaults.corlib,
				"System", "OverflowException");
		g_assert (clause->data.catch_class);
		clause->handler_offset = mono_mb_get_label (mb);

		oom_exc_class = mono_class_from_name (mono_defaults.corlib,
				"System", "OutOfMemoryException");
		g_assert (oom_exc_class);
		ctor = mono_class_get_method_from_name (oom_exc_class, ".ctor", 0);
		g_assert (ctor);

		mono_mb_emit_op (mb, CEE_NEWOBJ, ctor);
		mono_mb_emit_byte (mb, CEE_THROW);

		clause->handler_len = mono_mb_get_pos (mb) - clause->handler_offset;
		mono_mb_set_clauses (mb, 1, clause);
		mono_mb_patch_branch (mb, pos_leave);
		/* end catch */
	} else {
		g_assert_not_reached ();
	}

	/* size += ALLOC_ALIGN - 1; */
	mono_mb_emit_ldloc (mb, size_var);
	mono_mb_emit_icon (mb, ALLOC_ALIGN - 1);
	mono_mb_emit_byte (mb, CEE_ADD);
	/* size &= ~(ALLOC_ALIGN - 1); */
	mono_mb_emit_icon (mb, ~(ALLOC_ALIGN - 1));
	mono_mb_emit_byte (mb, CEE_AND);
	mono_mb_emit_stloc (mb, size_var);

	/* if (size > MAX_SMALL_OBJ_SIZE) goto slowpath */
	mono_mb_emit_ldloc (mb, size_var);
	mono_mb_emit_icon (mb, MAX_SMALL_OBJ_SIZE);
	max_size_branch = mono_mb_emit_short_branch (mb, MONO_CEE_BGT_S);

	/*
	 * We need to modify tlab_next, but the JIT only supports reading, so we read
	 * another tls var holding its address instead.
	 */

	/* tlab_next_addr (local) = tlab_next_addr (TLS var) */
	tlab_next_addr_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
	EMIT_TLS_ACCESS (mb, tlab_next_addr, tlab_next_addr_offset);
	mono_mb_emit_stloc (mb, tlab_next_addr_var);

	/* p = (void**)tlab_next; */
	p_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
	mono_mb_emit_ldloc (mb, tlab_next_addr_var);
	mono_mb_emit_byte (mb, CEE_LDIND_I);
	mono_mb_emit_stloc (mb, p_var);
	
	/* new_next = (char*)p + size; */
	new_next_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
	mono_mb_emit_ldloc (mb, p_var);
	mono_mb_emit_ldloc (mb, size_var);
	mono_mb_emit_byte (mb, CEE_CONV_I);
	mono_mb_emit_byte (mb, CEE_ADD);
	mono_mb_emit_stloc (mb, new_next_var);

	/* tlab_next = new_next */
	mono_mb_emit_ldloc (mb, tlab_next_addr_var);
	mono_mb_emit_ldloc (mb, new_next_var);
	mono_mb_emit_byte (mb, CEE_STIND_I);

	/* if (G_LIKELY (new_next < tlab_temp_end)) */
	mono_mb_emit_ldloc (mb, new_next_var);
	EMIT_TLS_ACCESS (mb, tlab_temp_end, tlab_temp_end_offset);
	slowpath_branch = mono_mb_emit_short_branch (mb, MONO_CEE_BLT_UN_S);

	/* Slowpath */

	mono_mb_patch_short_branch (mb, max_size_branch);

	mono_mb_emit_byte (mb, MONO_CUSTOM_PREFIX);
	mono_mb_emit_byte (mb, CEE_MONO_NOT_TAKEN);

	/* FIXME: mono_gc_alloc_obj takes a 'size_t' as an argument, not an int32 */
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldloc (mb, size_var);
	if (atype == ATYPE_NORMAL) {
		mono_mb_emit_icall (mb, mono_gc_alloc_obj);
	} else if (atype == ATYPE_VECTOR) {
		mono_mb_emit_ldarg (mb, 1);
		mono_mb_emit_icall (mb, mono_gc_alloc_vector);
	} else {
		g_assert_not_reached ();
	}
	mono_mb_emit_byte (mb, CEE_RET);

	/* Fastpath */
	mono_mb_patch_short_branch (mb, slowpath_branch);

	/* FIXME: Memory barrier */

	/* *p = vtable; */
	mono_mb_emit_ldloc (mb, p_var);
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_byte (mb, CEE_STIND_I);

	if (atype == ATYPE_VECTOR) {
		/* arr->max_length = max_length; */
		mono_mb_emit_ldloc (mb, p_var);
		mono_mb_emit_ldflda (mb, G_STRUCT_OFFSET (MonoArray, max_length));
		mono_mb_emit_ldarg (mb, 1);
		mono_mb_emit_byte (mb, CEE_STIND_I);
	}

	/* return p */
	mono_mb_emit_ldloc (mb, p_var);
	mono_mb_emit_byte (mb, CEE_RET);

	res = mono_mb_create_method (mb, csig, 8);
	mono_mb_free (mb);
	mono_method_get_header (res)->init_locals = FALSE;
	return res;
}
#endif

static MonoMethod* alloc_method_cache [ATYPE_NUM];
static MonoMethod *write_barrier_method;

static gboolean
is_ip_in_managed_allocator (MonoDomain *domain, gpointer ip)
{
	MonoJitInfo *ji;
	MonoMethod *method;
	int i;

	if (!ip || !domain)
		return FALSE;
	ji = mono_jit_info_table_find (domain, ip);
	if (!ji)
		return FALSE;
	method = ji->method;

	if (method == write_barrier_method)
		return TRUE;
	for (i = 0; i < ATYPE_NUM; ++i)
		if (method == alloc_method_cache [i])
			return TRUE;
	return FALSE;
}

/*
 * Generate an allocator method implementing the fast path of mono_gc_alloc_obj ().
 * The signature of the called method is:
 * 	object allocate (MonoVTable *vtable)
 */
MonoMethod*
mono_gc_get_managed_allocator (MonoVTable *vtable, gboolean for_box)
{
#ifdef MANAGED_ALLOCATION
	MonoClass *klass = vtable->klass;

#ifdef HAVE_KW_THREAD
	int tlab_next_offset = -1;
	int tlab_temp_end_offset = -1;
	MONO_THREAD_VAR_OFFSET (tlab_next, tlab_next_offset);
	MONO_THREAD_VAR_OFFSET (tlab_temp_end, tlab_temp_end_offset);

	if (tlab_next_offset == -1 || tlab_temp_end_offset == -1)
		return NULL;
#endif

	if (!mono_runtime_has_tls_get ())
		return NULL;
	if (klass->instance_size > tlab_size)
		return NULL;
	if (klass->has_finalize || klass->marshalbyref || (mono_profiler_get_events () & MONO_PROFILE_ALLOCATIONS))
		return NULL;
	if (klass->rank)
		return NULL;
	if (klass->byval_arg.type == MONO_TYPE_STRING)
		return NULL;
	if (collect_before_allocs)
		return NULL;

	return mono_gc_get_managed_allocator_by_type (ATYPE_NORMAL);
#else
	return NULL;
#endif
}

MonoMethod*
mono_gc_get_managed_array_allocator (MonoVTable *vtable, int rank)
{
#ifdef MANAGED_ALLOCATION
	MonoClass *klass = vtable->klass;

#ifdef HAVE_KW_THREAD
	int tlab_next_offset = -1;
	int tlab_temp_end_offset = -1;
	MONO_THREAD_VAR_OFFSET (tlab_next, tlab_next_offset);
	MONO_THREAD_VAR_OFFSET (tlab_temp_end, tlab_temp_end_offset);

	if (tlab_next_offset == -1 || tlab_temp_end_offset == -1)
		return NULL;
#endif

	if (rank != 1)
		return NULL;
	if (!mono_runtime_has_tls_get ())
		return NULL;
	if (mono_profiler_get_events () & MONO_PROFILE_ALLOCATIONS)
		return NULL;
	if (collect_before_allocs)
		return NULL;
	g_assert (!klass->has_finalize && !klass->marshalbyref);

	return mono_gc_get_managed_allocator_by_type (ATYPE_VECTOR);
#else
	return NULL;
#endif
}

int
mono_gc_get_managed_allocator_type (MonoMethod *managed_alloc)
{
#ifdef MANAGED_ALLOCATION
	int i;

	for (i = 0; i < ATYPE_NUM; ++i)
		if (managed_alloc == alloc_method_cache [i])
			return i;
#endif
	g_assert_not_reached ();
	return -1;
}

MonoMethod*
mono_gc_get_managed_allocator_by_type (int atype)
{
#ifdef MANAGED_ALLOCATION
	MonoMethod *res;

	if (!mono_runtime_has_tls_get ())
		return NULL;

	mono_loader_lock ();
	res = alloc_method_cache [atype];
	if (!res)
		res = alloc_method_cache [atype] = create_allocator (atype);
	mono_loader_unlock ();
	return res;
#else
	return NULL;
#endif
}

guint32
mono_gc_get_managed_allocator_types (void)
{
	return ATYPE_NUM;
}


MonoMethod*
mono_gc_get_write_barrier (void)
{
	MonoMethod *res;
	MonoMethodBuilder *mb;
	MonoMethodSignature *sig;
#ifdef MANAGED_WBARRIER
	int label_no_wb_1, label_no_wb_2, label_no_wb_3, label_no_wb_4, label_need_wb, label_slow_path;
	int buffer_var, buffer_index_var, dummy_var;

#ifdef HAVE_KW_THREAD
	int stack_end_offset = -1, store_remset_buffer_offset = -1;
	int store_remset_buffer_index_offset = -1, store_remset_buffer_index_addr_offset = -1;

	MONO_THREAD_VAR_OFFSET (stack_end, stack_end_offset);
	g_assert (stack_end_offset != -1);
	MONO_THREAD_VAR_OFFSET (store_remset_buffer, store_remset_buffer_offset);
	g_assert (store_remset_buffer_offset != -1);
	MONO_THREAD_VAR_OFFSET (store_remset_buffer_index, store_remset_buffer_index_offset);
	g_assert (store_remset_buffer_index_offset != -1);
	MONO_THREAD_VAR_OFFSET (store_remset_buffer_index_addr, store_remset_buffer_index_addr_offset);
	g_assert (store_remset_buffer_index_addr_offset != -1);
#endif
#endif

	// FIXME: Maybe create a separate version for ctors (the branch would be
	// correctly predicted more times)
	if (write_barrier_method)
		return write_barrier_method;

	/* Create the IL version of mono_gc_barrier_generic_store () */
	sig = mono_metadata_signature_alloc (mono_defaults.corlib, 1);
	sig->ret = &mono_defaults.void_class->byval_arg;
	sig->params [0] = &mono_defaults.int_class->byval_arg;

	mb = mono_mb_new (mono_defaults.object_class, "wbarrier", MONO_WRAPPER_WRITE_BARRIER);

#ifdef MANAGED_WBARRIER
	if (mono_runtime_has_tls_get ()) {
#ifdef ALIGN_NURSERY
		// if (ptr_in_nursery (ptr)) return;
		/*
		 * Masking out the bits might be faster, but we would have to use 64 bit
		 * immediates, which might be slower.
		 */
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_icon (mb, DEFAULT_NURSERY_BITS);
		mono_mb_emit_byte (mb, CEE_SHR_UN);
		mono_mb_emit_icon (mb, (mword)nursery_start >> DEFAULT_NURSERY_BITS);
		label_no_wb_1 = mono_mb_emit_branch (mb, CEE_BEQ);

		// if (!ptr_in_nursery (*ptr)) return;
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_byte (mb, CEE_LDIND_I);
		mono_mb_emit_icon (mb, DEFAULT_NURSERY_BITS);
		mono_mb_emit_byte (mb, CEE_SHR_UN);
		mono_mb_emit_icon (mb, (mword)nursery_start >> DEFAULT_NURSERY_BITS);
		label_no_wb_2 = mono_mb_emit_branch (mb, CEE_BNE_UN);
#else
		// FIXME:
		g_assert_not_reached ();
#endif

		// if (ptr >= stack_end) goto need_wb;
		mono_mb_emit_ldarg (mb, 0);
		EMIT_TLS_ACCESS (mb, stack_end, stack_end_offset);
		label_need_wb = mono_mb_emit_branch (mb, CEE_BGE_UN);

		// if (ptr >= stack_start) return;
		dummy_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_ldloc_addr (mb, dummy_var);
		label_no_wb_3 = mono_mb_emit_branch (mb, CEE_BGE_UN);

		// need_wb:
		mono_mb_patch_branch (mb, label_need_wb);

		// buffer = STORE_REMSET_BUFFER;
		buffer_var = mono_mb_add_local (mb, &mono_defaults.int_class->byval_arg);
		EMIT_TLS_ACCESS (mb, store_remset_buffer, store_remset_buffer_offset);
		mono_mb_emit_stloc (mb, buffer_var);

		// buffer_index = STORE_REMSET_BUFFER_INDEX;
		buffer_index_var = mono_mb_add_local (mb, &mono_defaults.int32_class->byval_arg);
		EMIT_TLS_ACCESS (mb, store_remset_buffer_index, store_remset_buffer_index_offset);
		mono_mb_emit_stloc (mb, buffer_index_var);

		// if (buffer [buffer_index] == ptr) return;
		mono_mb_emit_ldloc (mb, buffer_var);
		mono_mb_emit_ldloc (mb, buffer_index_var);
		g_assert (sizeof (gpointer) == 4 || sizeof (gpointer) == 8);
		mono_mb_emit_icon (mb, sizeof (gpointer) == 4 ? 2 : 3);
		mono_mb_emit_byte (mb, CEE_SHL);
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_byte (mb, CEE_LDIND_I);
		mono_mb_emit_ldarg (mb, 0);
		label_no_wb_4 = mono_mb_emit_branch (mb, CEE_BEQ);

		// ++buffer_index;
		mono_mb_emit_ldloc (mb, buffer_index_var);
		mono_mb_emit_icon (mb, 1);
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_stloc (mb, buffer_index_var);

		// if (buffer_index >= STORE_REMSET_BUFFER_SIZE) goto slow_path;
		mono_mb_emit_ldloc (mb, buffer_index_var);
		mono_mb_emit_icon (mb, STORE_REMSET_BUFFER_SIZE);
		label_slow_path = mono_mb_emit_branch (mb, CEE_BGE);

		// buffer [buffer_index] = ptr;
		mono_mb_emit_ldloc (mb, buffer_var);
		mono_mb_emit_ldloc (mb, buffer_index_var);
		g_assert (sizeof (gpointer) == 4 || sizeof (gpointer) == 8);
		mono_mb_emit_icon (mb, sizeof (gpointer) == 4 ? 2 : 3);
		mono_mb_emit_byte (mb, CEE_SHL);
		mono_mb_emit_byte (mb, CEE_ADD);
		mono_mb_emit_ldarg (mb, 0);
		mono_mb_emit_byte (mb, CEE_STIND_I);

		// STORE_REMSET_BUFFER_INDEX = buffer_index;
		EMIT_TLS_ACCESS (mb, store_remset_buffer_index_addr, store_remset_buffer_index_addr_offset);
		mono_mb_emit_ldloc (mb, buffer_index_var);
		mono_mb_emit_byte (mb, CEE_STIND_I4);

		// return;
		mono_mb_patch_branch (mb, label_no_wb_1);
		mono_mb_patch_branch (mb, label_no_wb_2);
		mono_mb_patch_branch (mb, label_no_wb_3);
		mono_mb_patch_branch (mb, label_no_wb_4);
		mono_mb_emit_byte (mb, CEE_RET);

		// slow path
		mono_mb_patch_branch (mb, label_slow_path);
	}
#endif

	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_icall (mb, mono_gc_wbarrier_generic_nostore);
	mono_mb_emit_byte (mb, CEE_RET);

	res = mono_mb_create_method (mb, sig, 16);
	mono_mb_free (mb);

	mono_loader_lock ();
	if (write_barrier_method) {
		/* Already created */
		mono_free_method (res);
	} else {
		/* double-checked locking */
		mono_memory_barrier ();
		write_barrier_method = res;
	}
	mono_loader_unlock ();

	return write_barrier_method;
}

#endif /* HAVE_SGEN_GC */

