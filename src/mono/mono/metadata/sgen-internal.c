#ifdef HAVE_SGEN_GC

#include "utils/mono-counters.h"
#include "metadata/sgen-gc.h"
#include "utils/lock-free-alloc.h"

/* keep each size a multiple of ALLOC_ALIGN */
static const int allocator_sizes [] = {
	   8,   16,   24,   32,   40,   48,   64,   80,
	  96,  128,  160,  192,  224,  248,  320,  384,
	 448,  528,  584,  680,  816, 1088, 1360, 2040,
	2336, 2728, 3272, 4088, 5456, 8184 };

#define NUM_ALLOCATORS	(sizeof (allocator_sizes) / sizeof (int))

static MonoLockFreeAllocSizeClass size_classes [NUM_ALLOCATORS];
static MonoLockFreeAllocator allocators [NUM_ALLOCATORS];

/*
 * Find the allocator index for memory chunks that can contain @size
 * objects.
 */
static int
index_for_size (size_t size)
{
	int slot;
	/* do a binary search or lookup table later. */
	for (slot = 0; slot < NUM_ALLOCATORS; ++slot) {
		if (allocator_sizes [slot] >= size)
			return slot;
	}
	g_assert_not_reached ();
	return -1;
}

/*
 * Allocator indexes for the fixed INTERNAL_MEM_XXX types.  -1 if that
 * type is dynamic.
 */
static int fixed_type_allocator_indexes [INTERNAL_MEM_MAX];

void
mono_sgen_register_fixed_internal_mem_type (int type, size_t size)
{
	int slot;

	g_assert (type >= 0 && type < INTERNAL_MEM_MAX);
	g_assert (fixed_type_allocator_indexes [type] == -1);

	slot = index_for_size (size);
	g_assert (slot >= 0);

	fixed_type_allocator_indexes [type] = slot;
}

void*
mono_sgen_alloc_internal_dynamic (size_t size, int type)
{
	int index;
	void *p;

	if (size > allocator_sizes [NUM_ALLOCATORS - 1])
		return mono_sgen_alloc_os_memory (size, TRUE);

	index = index_for_size (size);

	p = mono_lock_free_alloc (&allocators [index]);
	memset (p, 0, size);
	return p;
}

void
mono_sgen_free_internal_dynamic (void *addr, size_t size, int type)
{
	int index;

	if (!addr)
		return;

	if (size > allocator_sizes [NUM_ALLOCATORS - 1])
		return mono_sgen_free_os_memory (addr, size);

	index = index_for_size (size);

	mono_lock_free_free (addr);
}

void*
mono_sgen_alloc_internal (int type)
{
	int index = fixed_type_allocator_indexes [type];
	void *p;
	g_assert (index >= 0 && index < NUM_ALLOCATORS);
	p = mono_lock_free_alloc (&allocators [index]);
	memset (p, 0, allocator_sizes [index]);
	return p;
}

void
mono_sgen_free_internal (void *addr, int type)
{
	int index;

	if (!addr)
		return;

	index = fixed_type_allocator_indexes [type];
	g_assert (index >= 0 && index < NUM_ALLOCATORS);

	mono_lock_free_free (addr);
}

void
mono_sgen_dump_internal_mem_usage (FILE *heap_dump_file)
{
	/*
	static char const *internal_mem_names [] = { "pin-queue", "fragment", "section", "scan-starts",
						     "fin-table", "finalize-entry", "dislink-table",
						     "dislink", "roots-table", "root-record", "statistics",
						     "remset", "gray-queue", "store-remset", "marksweep-tables",
						     "marksweep-block-info", "ephemeron-link", "worker-data",
						     "bridge-data", "job-queue-entry" };

	int i;

	fprintf (heap_dump_file, "<other-mem-usage type=\"large-internal\" size=\"%lld\"/>\n", large_internal_bytes_alloced);
	fprintf (heap_dump_file, "<other-mem-usage type=\"pinned-chunks\" size=\"%lld\"/>\n", pinned_chunk_bytes_alloced);
	for (i = 0; i < INTERNAL_MEM_MAX; ++i) {
		fprintf (heap_dump_file, "<other-mem-usage type=\"%s\" size=\"%ld\"/>\n",
				internal_mem_names [i], unmanaged_allocator.small_internal_mem_bytes [i]);
	}
	*/
}

void
mono_sgen_report_internal_mem_usage (void)
{
	/* FIXME: implement */
	printf ("not implemented yet\n");
}

void
mono_sgen_init_internal_allocator (void)
{
	int i;

	for (i = 0; i < INTERNAL_MEM_MAX; ++i)
		fixed_type_allocator_indexes [i] = -1;

	for (i = 0; i < NUM_ALLOCATORS; ++i) {
		mono_lock_free_allocator_init_size_class (&size_classes [i], allocator_sizes [i]);
		mono_lock_free_allocator_init_allocator (&allocators [i], &size_classes [i]);
	}

#ifdef HEAVY_STATISTICS
	mono_counters_register ("Internal allocs", MONO_COUNTER_GC | MONO_COUNTER_LONG, &stat_internal_alloc);
#endif
}

#endif
