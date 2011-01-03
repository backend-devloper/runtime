/*
 * mini-gc.c: GC interface for the mono JIT
 *
 * Author:
 *   Zoltan Varga (vargaz@gmail.com)
 *
 * Copyright 2009 Novell, Inc (http://www.novell.com)
 */

#include "config.h"
#include "mini-gc.h"
#include <mono/metadata/gc-internal.h>

//#if 0
#ifdef HAVE_SGEN_GC

#include <mono/metadata/gc-internal.h>
#include <mono/utils/mono-counters.h>

/* Contains state needed by the GC Map construction code */
typedef struct {
	/*
	 * This contains information about stack slots initialized in the prolog, encoded using
	 * (slot_index << 16) | slot_type. The slot_index is relative to the CFA, i.e. 0
	 * means cfa+0, 1 means cfa-4/8, etc.
	 */
	GSList *stack_slots_from_cfa;
	/* Same for stack slots relative to the frame pointer */
	GSList *stack_slots_from_fp;

	/* Number of slots in the map */
	int nslots;
	/* The number of registers in the map */
	int nregs;
	/* Min and Max offsets of the stack frame relative to fp */
	int min_offset, max_offset;
	/* Same for the locals area */
	int locals_min_offset, locals_max_offset;

	/* The call sites where this frame can be stopped during GC */
	GCCallSite **callsites;
	/* The number of call sites */
	int ncallsites;

	/*
	 * The width of the stack bitmaps in bytes. This is not equal to the bitmap width at
     * runtime, since it includes columns which are 0.
	 */
	int bitmap_width;
	/* 
	 * A bitmap whose width equals nslots, and whose height equals ncallsites.
	 * The bitmap contains a 1 if the corresponding stack slot has type SLOT_REF at the
	 * given callsite.
	 */
	guint8 *ref_bitmap;
	/* Same for SLOT_PIN */
	guint8 *pin_bitmap;

	/*
	 * Similar bitmaps for registers. These have width MONO_MAX_IREGS in bits.
	 */
	int reg_bitmap_width;
	guint8 *reg_ref_bitmap;
	guint8 *reg_pin_bitmap;
} MonoCompileGC;

#define ALIGN_TO(val,align) ((((guint64)val) + ((align) - 1)) & ~((align) - 1))

#if 0
#define DEBUG(s) do { s; } while (0)
#define DEBUG_ENABLED 1
#else
#define DEBUG(s)
#endif

#if 1
#define DEBUG_GC_MAP(s) do { s; fflush (stdout); } while (0)
#else
#define DEBUG_GC_MAP(s)
#endif

#define GC_BITS_PER_WORD (sizeof (gsize) * 8)

/*
 * Contains information collected during the conservative stack marking pass,
 * used during the precise pass. This helps to avoid doing a stack walk twice, which
 * is expensive.
 */
typedef struct {
	guint8 *bitmap;
	int nslots;
    int frame_start_offset;
	int nreg_locations;
	/* Relative to stack_start */
	int reg_locations [MONO_MAX_IREGS];
#ifdef DEBUG_ENABLED
	int regs [MONO_MAX_IREGS];
#endif
} FrameInfo;

/* Max number of frames stored in the TLS data */
#define MAX_FRAMES 50

/*
 * Per-thread data kept by this module. This is stored in the GC and passed to us as
 * parameters, instead of being stored in a TLS variable, since during a collection,
 * only the collection thread is active.
 */
typedef struct {
	MonoLMF *lmf;
	MonoContext ctx;
	gboolean has_context;
	MonoJitTlsData *jit_tls;
	/* Number of frames collected during the !precise pass */
	int nframes;
	FrameInfo frames [MAX_FRAMES];
} TlsData;

/* These are constant so don't store them in the GC Maps */
/* Number of registers stored in gc maps */
#define NREGS MONO_MAX_IREGS

/* 
 * The GC Map itself.
 * Contains information needed to mark a stack frame.
 * This is a transient structure, created from a compressed representation on-demand.
 */
typedef struct {
	/*
	 * The offsets of the GC tracked area inside the stack frame relative to the frame pointer.
	 * This includes memory which is NOREF thus doesn't need GC maps.
	 */
	int start_offset;
	int end_offset;
	/*
	 * The offset relative to frame_offset where the the memory described by the GC maps
	 * begins.
	 */
	int map_offset;
	/* The number of stack slots in the map */
	int nslots;
	/* The frame pointer register */
	guint8 frame_reg;
	/* The size of each callsite table entry */
	guint8 callsite_entry_size;
	guint has_pin_slots : 1;
	guint has_ref_slots : 1;
	guint has_ref_regs : 1;
	guint has_pin_regs : 1;

	/* The offsets below are into an external bitmaps array */

	/* 
	 * A bitmap whose width is equal to bitmap_width, and whose height is equal to ncallsites.
	 * The bitmap contains a 1 if the corresponding stack slot has type SLOT_REF at the
	 * given callsite.
	 */
	guint32 stack_ref_bitmap_offset;
	/*
	 * Same for SLOT_PIN. It is possible that the same bit is set in both bitmaps at
     * different callsites, if the slot starts out as PIN, and later changes to REF.
	 */
	guint32 stack_pin_bitmap_offset;

	/*
	 * Corresponding bitmaps for registers
	 * These have width equal to the number of bits set in reg_ref_mask/reg_pin_mask.
	 * FIXME: Merge these with the normal bitmaps, i.e. reserve the first x slots for them ?
	 */
	guint32 reg_pin_bitmap_offset;
	guint32 reg_ref_bitmap_offset;

	guint32 reg_ref_mask, reg_pin_mask;

	/* The number of bits set in the two masks above */
	guint8 nref_regs, npin_regs;

	/*
	 * A bit array marking slots which contain refs.
	 * This is used only for debugging.
	 */
	//guint8 *ref_slots;

	/* Callsite offsets */
	/* These can take up a lot of space, so encode them compactly */
	union {
		guint8 *offsets8;
		guint16 *offsets16;
		guint32 *offsets32;
	} callsites;
	int ncallsites;
} GCMap;

/*
 * A compressed version of GCMap. This is what gets stored in MonoJitInfo.
 */
typedef struct {
	//guint8 *ref_slots;
	//guint8 encoded_size;

	/*
	 * The arrays below are embedded after the struct.
	 * Their address needs to be computed.
	 */

	/* The fixed fields of the GCMap encoded using LEB128 */
	guint8 encoded [MONO_ZERO_LEN_ARRAY];

	/* An array of ncallsites entries, each entry is callsite_entry_size bytes long */
	guint8 callsites [MONO_ZERO_LEN_ARRAY];

	/* The GC bitmaps */
	guint8 bitmaps [MONO_ZERO_LEN_ARRAY];
} GCEncodedMap;

static int precise_frame_count [2], precise_frame_limit = -1;
static gboolean precise_frame_limit_inited;

/* Stats */
typedef struct {
	int scanned_stacks;
	int scanned;
	int scanned_precisely;
	int scanned_conservatively;
	int scanned_registers;

	int all_slots;
	int noref_slots;
	int ref_slots;
	int pin_slots;

	int gc_maps_size;
	int gc_callsites_size;
	int gc_callsites8_size;
	int gc_callsites16_size;
	int gc_callsites32_size;
	int gc_bitmaps_size;
	int gc_map_struct_size;
	int tlsdata_size;
} JITGCStats;

static JITGCStats stats;

// FIXME: Move these to a shared place

static inline void
encode_uleb128 (guint32 value, guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;

	do {
		guint8 b = value & 0x7f;
		value >>= 7;
		if (value != 0) /* more bytes to come */
			b |= 0x80;
		*p ++ = b;
	} while (value);

	*endbuf = p;
}

static G_GNUC_UNUSED void
encode_sleb128 (gint32 value, guint8 *buf, guint8 **endbuf)
{
	gboolean more = 1;
	gboolean negative = (value < 0);
	guint32 size = 32;
	guint8 byte;
	guint8 *p = buf;

	while (more) {
		byte = value & 0x7f;
		value >>= 7;
		/* the following is unnecessary if the
		 * implementation of >>= uses an arithmetic rather
		 * than logical shift for a signed left operand
		 */
		if (negative)
			/* sign extend */
			value |= - (1 <<(size - 7));
		/* sign bit of byte is second high order bit (0x40) */
		if ((value == 0 && !(byte & 0x40)) ||
			(value == -1 && (byte & 0x40)))
			more = 0;
		else
			byte |= 0x80;
		*p ++= byte;
	}

	*endbuf = p;
}

static inline guint32
decode_uleb128 (guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;
	guint32 res = 0;
	int shift = 0;

	while (TRUE) {
		guint8 b = *p;
		p ++;

		res = res | (((int)(b & 0x7f)) << shift);
		if (!(b & 0x80))
			break;
		shift += 7;
	}

	*endbuf = p;

	return res;
}

static inline gint32
decode_sleb128 (guint8 *buf, guint8 **endbuf)
{
	guint8 *p = buf;
	gint32 res = 0;
	int shift = 0;

	while (TRUE) {
		guint8 b = *p;
		p ++;

		res = res | (((int)(b & 0x7f)) << shift);
		shift += 7;
		if (!(b & 0x80)) {
			if (shift < 32 && (b & 0x40))
				res |= - (1 << shift);
			break;
		}
	}

	*endbuf = p;

	return res;
}

static int
encode_frame_reg (int frame_reg)
{
#ifdef TARGET_AMD64
	if (frame_reg == AMD64_RSP)
		return 0;
	else if (frame_reg == AMD64_RBP)
		return 1;
#else
	NOT_IMPLEMENTED;
#endif
	g_assert_not_reached ();
	return -1;
}

static int
decode_frame_reg (int encoded)
{
#ifdef TARGET_AMD64
	if (encoded == 0)
		return AMD64_RSP;
	else if (encoded == 1)
		return AMD64_RBP;
#else
	NOT_IMPLEMENTED;
#endif
	g_assert_not_reached ();
	return -1;
}

/*
 * encode_gc_map:
 *
 *   Encode the fixed fields of MAP into a buffer pointed to by BUF.
 */
static void
encode_gc_map (GCMap *map, guint8 *buf, guint8 **endbuf)
{
	guint32 flags, freg;

	encode_sleb128 (map->start_offset / sizeof (mgreg_t), buf, &buf);
	encode_sleb128 (map->map_offset / sizeof (mgreg_t), buf, &buf);
	encode_uleb128 (map->nslots, buf, &buf);
	g_assert (map->callsite_entry_size <= 4);
	freg = encode_frame_reg (map->frame_reg);
	g_assert (freg < 2);
	flags = (map->has_ref_slots ? 1 : 0) | (map->has_pin_slots ? 2 : 0) | (map->has_ref_regs ? 4 : 0) | (map->has_pin_regs ? 8 : 0) | ((map->callsite_entry_size - 1) << 4) | (freg << 6);
	encode_uleb128 (flags, buf, &buf);
	if (map->has_ref_regs)
		encode_uleb128 (map->reg_ref_mask, buf, &buf);
	if (map->has_pin_regs)
		encode_uleb128 (map->reg_pin_mask, buf, &buf);
	encode_uleb128 (map->ncallsites, buf, &buf);

	*endbuf = buf;
}	

/*
 * decode_gc_map:
 *
 *   Decode the encoded GC map representation in BUF and store the result into MAP.
 */
static void
decode_gc_map (guint8 *buf, GCMap *map, guint8 **endbuf)
{
	guint32 flags;
	int stack_bitmap_size, reg_ref_bitmap_size, reg_pin_bitmap_size, offset, freg;
	int i, n;

	map->start_offset = decode_sleb128 (buf, &buf) * sizeof (mgreg_t);
	map->map_offset = decode_sleb128 (buf, &buf) * sizeof (mgreg_t);
	map->nslots = decode_uleb128 (buf, &buf);
	flags = decode_uleb128 (buf, &buf);
	map->has_ref_slots = (flags & 1) ? 1 : 0;
	map->has_pin_slots = (flags & 2) ? 1 : 0;
	map->has_ref_regs = (flags & 4) ? 1 : 0;
	map->has_pin_regs = (flags & 8) ? 1 : 0;
	map->callsite_entry_size = ((flags >> 4) & 0x3) + 1;
	freg = flags >> 6;
	map->frame_reg = decode_frame_reg (freg);
	if (map->has_ref_regs) {
		map->reg_ref_mask = decode_uleb128 (buf, &buf);
		n = 0;
		for (i = 0; i < NREGS; ++i)
			if (map->reg_ref_mask & (1 << i))
				n ++;
		map->nref_regs = n;
	}
	if (map->has_pin_regs) {
		map->reg_pin_mask = decode_uleb128 (buf, &buf);
		n = 0;
		for (i = 0; i < NREGS; ++i)
			if (map->reg_pin_mask & (1 << i))
				n ++;
		map->npin_regs = n;
	}
	map->ncallsites = decode_uleb128 (buf, &buf);

	stack_bitmap_size = (ALIGN_TO (map->nslots, 8) / 8) * map->ncallsites;
	reg_ref_bitmap_size = (ALIGN_TO (map->nref_regs, 8) / 8) * map->ncallsites;
	reg_pin_bitmap_size = (ALIGN_TO (map->npin_regs, 8) / 8) * map->ncallsites;
	offset = 0;
	map->stack_ref_bitmap_offset = offset;
	if (map->has_ref_slots)
		offset += stack_bitmap_size;
	map->stack_pin_bitmap_offset = offset;
	if (map->has_pin_slots)
		offset += stack_bitmap_size;
	map->reg_ref_bitmap_offset = offset;
	if (map->has_ref_regs)
		offset += reg_ref_bitmap_size;
	map->reg_pin_bitmap_offset = offset;
	if (map->has_pin_regs)
		offset += reg_pin_bitmap_size;

	*endbuf = buf;
}

static gpointer
thread_attach_func (void)
{
	TlsData *tls;

	tls = g_new0 (TlsData, 1);
	stats.tlsdata_size += sizeof (TlsData);

	return tls;
}

static void
thread_detach_func (gpointer user_data)
{
	TlsData *tls = user_data;

	g_free (tls);
}

static void
thread_suspend_func (gpointer user_data, void *sigctx)
{
	TlsData *tls = user_data;

	if (!tls)
		/* Happens during startup */
		return;

	tls->lmf = mono_get_lmf ();
	if (sigctx) {
		mono_arch_sigctx_to_monoctx (sigctx, &tls->ctx);
		tls->has_context = TRUE;
	} else {
		tls->has_context = FALSE;
	}
	tls->jit_tls = TlsGetValue (mono_jit_tls_id);
}

#define DEAD_REF ((gpointer)(gssize)0x2a2a2a2a2a2a2a2aULL)

static inline void
set_bit (guint8 *bitmap, int width, int y, int x)
{
	bitmap [(width * y) + (x / 8)] |= (1 << (x % 8));
}

static inline void
clear_bit (guint8 *bitmap, int width, int y, int x)
{
	bitmap [(width * y) + (x / 8)] &= ~(1 << (x % 8));
}

static inline int
get_bit (guint8 *bitmap, int width, int y, int x)
{
	return bitmap [(width * y) + (x / 8)] & (1 << (x % 8));
}

static const char*
slot_type_to_string (GCSlotType type)
{
	switch (type) {
	case SLOT_REF:
		return "ref";
	case SLOT_NOREF:
		return "noref";
	case SLOT_PIN:
		return "pin";
	default:
		g_assert_not_reached ();
		return NULL;
	}
}

/*
 * conservatively_pass:
 *
 *   Mark a thread stack conservatively and collect information needed by the precise pass.
 */
static void
conservative_pass (TlsData *tls, guint8 *stack_start, guint8 *stack_end)
{
	MonoJitInfo *ji;
	MonoContext ctx, new_ctx;
	MonoLMF *lmf;
	guint8 *stack_limit;
	gboolean last = TRUE;
	GCMap *map;
	GCMap map_tmp;
	GCEncodedMap *emap;
	guint8* fp, *p, *frame_start, *frame_end;
	int i, pc_offset, cindex, bitmap_width;
	int scanned = 0, scanned_precisely, scanned_conservatively, scanned_registers;
	gboolean res;
	StackFrameInfo frame;
	mgreg_t *reg_locations [MONO_MAX_IREGS];
	mgreg_t *new_reg_locations [MONO_MAX_IREGS];
	guint8 *bitmaps;
	FrameInfo *fi;
	guint32 precise_regmask;

	/* tls == NULL can happen during startup */
	if (mono_thread_internal_current () == NULL || !tls) {
		mono_gc_conservatively_scan_area (stack_start, stack_end);
		stats.scanned_stacks += stack_end - stack_start;
		return;
	}

	lmf = tls->lmf;
	frame.domain = NULL;

	/* Number of bytes scanned based on GC map data */
	scanned = 0;
	/* Number of bytes scanned precisely based on GC map data */
	scanned_precisely = 0;
	/* Number of bytes scanned conservatively based on GC map data */
	scanned_conservatively = 0;
	/* Number of bytes scanned conservatively in register save areas */
	scanned_registers = 0;

	/* This is one past the last address which we have scanned */
	stack_limit = stack_start;

	if (!tls->has_context)
		memset (&new_ctx, 0, sizeof (ctx));
	else
		memcpy (&new_ctx, &tls->ctx, sizeof (MonoContext));

	memset (reg_locations, 0, sizeof (reg_locations));
	memset (new_reg_locations, 0, sizeof (new_reg_locations));

	tls->nframes = 0;

	while (TRUE) {
		memcpy (&ctx, &new_ctx, sizeof (ctx));

		for (i = 0; i < MONO_MAX_IREGS; ++i) {
			if (new_reg_locations [i]) {
				/*
				 * If the current frame saves the register, it means it might modify its
				 * value, thus the old location might not contain the same value, so
				 * we have to mark it conservatively.
				 * FIXME: This happens very often, due to:
				 * - outside the live intervals of the variables allocated to a register,
				 * we have to treat the register as PIN, since we don't know whenever it
				 * has the same value as in the caller, or a new dead value.
				 */
				if (reg_locations [i]) {
					DEBUG (printf ("\tscan saved reg %s location %p.\n", mono_arch_regname (i), reg_locations [i]));
					mono_gc_conservatively_scan_area (reg_locations [i], reg_locations [i] + sizeof (mgreg_t));
					scanned_registers += sizeof (mgreg_t);
				}

				reg_locations [i] = new_reg_locations [i];

				DEBUG (printf ("\treg %s is at location %p.\n", mono_arch_regname (i), reg_locations [i]));
			}
		}

		g_assert ((guint64)stack_limit % sizeof (mgreg_t) == 0);

#ifdef MONO_ARCH_HAVE_FIND_JIT_INFO_EXT
		res = mono_find_jit_info_ext (frame.domain ? frame.domain : mono_domain_get (), tls->jit_tls, NULL, &ctx, &new_ctx, NULL, &lmf, new_reg_locations, &frame);
		if (!res)
			break;
#else
		break;
#endif

		/* The last frame can be in any state so mark conservatively */
		if (last) {
			last = FALSE;
			continue;
		}

		/* These frames are returned by mono_find_jit_info () two times */
		if (!frame.managed)
			continue;

		/* All the other frames are at a call site */

		if (tls->nframes == MAX_FRAMES) {
			/* 
			 * Can't save information since the array is full. So scan the rest of the
			 * stack conservatively.
			 */
			break;
		}

		/* Scan the frame of this method */

		/*
		 * A frame contains the following:
		 * - saved registers
		 * - saved args
		 * - locals
		 * - spill area
		 * - localloc-ed memory
		 */

		ji = frame.ji;
		emap = ji->gc_info;

		if (!emap) {
			DEBUG (char *fname = mono_method_full_name (ji->method, TRUE); printf ("Mark(%d): No GC map for %s\n", precise, fname); g_free (fname));

			continue;
		}

		/*
		 * Debugging aid to control the number of frames scanned precisely
		 */
		if (!precise_frame_limit_inited) {
			if (getenv ("MONO_PRECISE_COUNT"))
				precise_frame_limit = atoi (getenv ("MONO_PRECISE_COUNT"));
			precise_frame_limit_inited = TRUE;
		}
				
		if (precise_frame_limit != -1) {
			if (precise_frame_count [FALSE] == precise_frame_limit)
				printf ("LAST PRECISE FRAME: %s\n", mono_method_full_name (ji->method, TRUE));
			if (precise_frame_count [FALSE] > precise_frame_limit)
				continue;
		}
		precise_frame_count [FALSE] ++;

		/* Decode the encoded GC map */
		map = &map_tmp;
		memset (map, 0, sizeof (GCMap));
		decode_gc_map (&emap->encoded [0], map, &p);
		p = (guint8*)ALIGN_TO (p, map->callsite_entry_size);
		map->callsites.offsets8 = p;
		p += map->callsite_entry_size * map->ncallsites;
		bitmaps = p;

#ifdef __x86_64__
		if (map->frame_reg == AMD64_RSP)
			fp = (guint8*)ctx.rsp;
		else if (map->frame_reg == AMD64_RBP)
			fp = (guint8*)ctx.rbp;
		else
			g_assert_not_reached ();
#else
		fp = NULL;
		g_assert_not_reached ();
#endif

		frame_start = fp + map->start_offset + map->map_offset;
		frame_end = fp + map->end_offset;

		pc_offset = (guint8*)MONO_CONTEXT_GET_IP (&ctx) - (guint8*)ji->code_start;
		g_assert (pc_offset >= 0);

		DEBUG (char *fname = mono_method_full_name (ji->method, TRUE); printf ("Mark(%d): %s+0x%x (%p) limit=%p fp=%p frame=%p-%p (%d)\n", precise, fname, pc_offset, (gpointer)MONO_CONTEXT_GET_IP (&ctx), stack_limit, fp, frame_start, frame_end, (int)(frame_end - frame_start)); g_free (fname));

		/* Find the callsite index */
		if (ji->code_size < 256) {
			for (i = 0; i < map->ncallsites; ++i)
				/* ip points inside the call instruction */
				if (map->callsites.offsets8 [i] == pc_offset + 1)
					break;
		} else if (ji->code_size < 65536) {
			// FIXME: Use a binary search
			for (i = 0; i < map->ncallsites; ++i)
				/* ip points inside the call instruction */
				if (map->callsites.offsets16 [i] == pc_offset + 1)
					break;
		} else {
			// FIXME: Use a binary search
			for (i = 0; i < map->ncallsites; ++i)
				/* ip points inside the call instruction */
				if (map->callsites.offsets32 [i] == pc_offset + 1)
					break;
		}
		if (i == map->ncallsites) {
			printf ("Unable to find ip offset 0x%x in callsite list of %s.\n", pc_offset + 1, mono_method_full_name (ji->method, TRUE));
			g_assert_not_reached ();
		}
		cindex = i;

		g_assert (frame_start >= stack_limit);

		if (frame_start > stack_limit) {
			/* This scans the previously skipped frames as well */
			DEBUG (printf ("\tscan area %p-%p.\n", stack_limit, frame_start));
			mono_gc_conservatively_scan_area (stack_limit, frame_start);
		}

		/* Mark stack slots */
		if (map->has_pin_slots) {
			int bitmap_width = ALIGN_TO (map->nslots, 8) / 8;
			guint8 *pin_bitmap = &bitmaps [map->stack_pin_bitmap_offset + (bitmap_width * cindex)];
			guint8 *p;
			gboolean pinned;

			p = frame_start;
			for (i = 0; i < map->nslots; ++i) {
				pinned = pin_bitmap [i / 8] & (1 << (i % 8));
				if (pinned) {
					DEBUG (printf ("\tscan slot %s0x%x(fp)=%p.\n", (guint8*)p > (guint8*)fp ? "" : "-", ABS ((int)((gssize)p - (gssize)fp)), p));
					mono_gc_conservatively_scan_area (p, p + sizeof (mgreg_t));
					scanned_conservatively += sizeof (mgreg_t);
				} else {
					scanned_precisely += sizeof (mgreg_t);
				}
				p += sizeof (mgreg_t);
			}
		} else {
			scanned_precisely += (map->nslots * sizeof (mgreg_t));
		}

		/* The area outside of start-end is NOREF */
		scanned_precisely += (map->end_offset - map->start_offset) - (map->nslots * sizeof (mgreg_t));

		/* Mark registers */
		precise_regmask = 0;
		if (map->has_pin_regs) {
			int bitmap_width = ALIGN_TO (map->npin_regs, 8) / 8;
			guint8 *pin_bitmap = &bitmaps [map->reg_pin_bitmap_offset + (bitmap_width * cindex)];
			int bindex = 0;
			for (i = 0; i < NREGS; ++i) {
				if (!(map->reg_pin_mask & (1 << i)))
					continue;

				if (reg_locations [i] && !(pin_bitmap [bindex / 8] & (1 << (bindex % 8)))) {
					/*
					 * The method uses this register, and we have precise info for it.
					 * This means the location will be scanned precisely.
					 */
					precise_regmask |= (1 << i);
					DEBUG (printf ("\treg %s at location %p is precise.\n", mono_arch_regname (i), reg_locations [i]));
				} else {
					if (reg_locations [i])
						DEBUG (printf ("\treg %s at location %p is pinning.\n", mono_arch_regname (i), reg_locations [i]));
				}
				bindex ++;
			}
		}

		scanned += map->end_offset - map->start_offset;

		g_assert (scanned == scanned_precisely + scanned_conservatively);

		stack_limit = frame_end;

		/* Save information for the precise pass */
		fi = &tls->frames [tls->nframes];
		fi->nslots = map->nslots;
		bitmap_width = ALIGN_TO (map->nslots, 8) / 8;
		if (map->has_ref_slots)
			fi->bitmap = &bitmaps [map->stack_ref_bitmap_offset + (bitmap_width * cindex)];
		else
			fi->bitmap = NULL;
		fi->frame_start_offset = frame_start - stack_start;
		fi->nreg_locations = 0;

		if (map->has_ref_regs) {
			int bitmap_width = ALIGN_TO (map->nref_regs, 8) / 8;
			guint8 *ref_bitmap = &bitmaps [map->reg_ref_bitmap_offset + (bitmap_width * cindex)];
			int bindex = 0;
			for (i = 0; i < NREGS; ++i) {
				if (!(map->reg_ref_mask & (1 << i)))
					continue;

				if (reg_locations [i] && (ref_bitmap [bindex / 8] & (1 << (bindex % 8)))) {
					DEBUG (fi->regs [fi->nreg_locations] = i);
					fi->reg_locations [fi->nreg_locations] = (guint8*)reg_locations [i] - stack_start;
					fi->nreg_locations ++;
				}
				bindex ++;
			}
		}

		if (precise_regmask) {
			for (i = 0; i < NREGS; ++i) {
				if (precise_regmask & (1 << i))
					/*
					 * Tell the code at the beginning of the loop that this location is
					 * processed.
					 */
					reg_locations [i] = NULL;
			}
		}

		tls->nframes ++;
	}

	/* Scan the remaining register save locations */
	for (i = 0; i < MONO_MAX_IREGS; ++i) {
		if (reg_locations [i]) {
			DEBUG (printf ("\tscan saved reg location %p.\n", reg_locations [i]));
			mono_gc_conservatively_scan_area (reg_locations [i], reg_locations [i] + sizeof (mgreg_t));
			scanned_registers += sizeof (mgreg_t);
		}
		// FIXME: Is this needed ?
		if (new_reg_locations [i]) {
			DEBUG (printf ("\tscan saved reg location %p.\n", new_reg_locations [i]));
			mono_gc_conservatively_scan_area (new_reg_locations [i], new_reg_locations [i] + sizeof (mgreg_t));
			scanned_registers += sizeof (mgreg_t);
		}
	}

	if (stack_limit < stack_end) {
		DEBUG (printf ("\tscan area %p-%p.\n", stack_limit, stack_end));
		mono_gc_conservatively_scan_area (stack_limit, stack_end);
	}

	DEBUG (printf ("Marked %d bytes, p=%d,c=%d out of %d.\n", scanned, scanned_precisely, scanned_conservatively, (int)(stack_end - stack_start)));

	stats.scanned_stacks += stack_end - stack_start;
	stats.scanned += scanned;
	stats.scanned_precisely += scanned_precisely;
	stats.scanned_conservatively += scanned_conservatively;
	stats.scanned_registers += scanned_registers;

	//mono_gc_conservatively_scan_area (stack_start, stack_end);
}

/*
 * precise_pass:
 *
 *   Mark a thread stack precisely based on information saved during the conservative
 * pass.
 */
static void
precise_pass (TlsData *tls, guint8 *stack_start, guint8 *stack_end)
{
	int findex, i;
	FrameInfo *fi;
	guint8 *frame_start;

	if (!tls)
		return;

	for (findex = 0; findex < tls->nframes; findex ++) {
		/* Load information saved by the !precise pass */
		fi = &tls->frames [findex];
		frame_start = stack_start + fi->frame_start_offset;

		/* 
		 * FIXME: Add a function to mark using a bitmap, to avoid doing a 
		 * call for each object.
		 */

		/* Mark stack slots */
		if (fi->bitmap) {
			guint8 *ref_bitmap = fi->bitmap;
			gboolean live;

			for (i = 0; i < fi->nslots; ++i) {
				MonoObject **ptr = (MonoObject**)(frame_start + (i * sizeof (mgreg_t)));

				live = ref_bitmap [i / 8] & (1 << (i % 8));

				if (live) {
					MonoObject *obj = *ptr;
					if (obj) {
						DEBUG (printf ("\tref %s0x%x(fp)=%p: %p ->", (guint8*)ptr >= (guint8*)fp ? "" : "-", ABS ((int)((gssize)ptr - (gssize)fp)), ptr, obj));
						*ptr = mono_gc_scan_object (obj);
						DEBUG (printf (" %p.\n", *ptr));
					} else {
						DEBUG (printf ("\tref %s0x%x(fp)=%p: %p.\n", (guint8*)ptr >= (guint8*)fp ? "" : "-", ABS ((int)((gssize)ptr - (gssize)fp)), ptr, obj));
					}
				} else {
#if 0
					/*
					 * This is disabled because the pointer takes up a lot of space.
					 * Stack slots might be shared between ref and non-ref variables ?
					 */
					if (map->ref_slots [i / 8] & (1 << (i % 8))) {
						DEBUG (printf ("\tref %s0x%x(fp)=%p: dead (%p)\n", (guint8*)ptr >= (guint8*)fp ? "" : "-", ABS ((int)((gssize)ptr - (gssize)fp)), ptr, *ptr));
						/*
						 * Fail fast if the live range is incorrect, and
						 * the JITted code tries to access this object
						 */
						*ptr = DEAD_REF;
					}
#endif
				}
			}
		}

		/* Mark registers */

		/*
		 * Registers are different from stack slots, they have no address where they
		 * are stored. Instead, some frame below this frame in the stack saves them
		 * in its prolog to the stack. We can mark this location precisely.
		 */
		for (i = 0; i < fi->nreg_locations; ++i) {
			/*
			 * reg_locations [i] contains the address of the stack slot where
			 * a reg was last saved, so mark that slot.
			 */
			MonoObject **ptr = (MonoObject**)((guint8*)stack_start + fi->reg_locations [i]);
			MonoObject *obj = *ptr;

			if (obj) {
				DEBUG (printf ("\treg %s saved at %p: %p ->", mono_arch_regname (fi->regs [i]), ptr, obj));
				*ptr = mono_gc_scan_object (obj);
				DEBUG (printf (" %p.\n", *ptr));
			} else {
				DEBUG (printf ("\treg %s saved at %p: %p", mono_arch_regname (fi->regs [i]), ptr, obj));
			}
		}	
	}
}

/*
 * thread_mark_func:
 *
 *   This is called by the GC twice to mark a thread stack. PRECISE is FALSE at the first
 * call, and TRUE at the second. USER_DATA points to a TlsData
 * structure filled up by thread_suspend_func. 
 */
static void
thread_mark_func (gpointer user_data, guint8 *stack_start, guint8 *stack_end, gboolean precise)
{
	TlsData *tls = user_data;

	DEBUG (printf ("*** %s stack marking %p-%p ***\n", precise ? "Precise" : "Conservative", stack_start, stack_end));

	if (!precise)
		conservative_pass (tls, stack_start, stack_end);
	else
		precise_pass (tls, stack_start, stack_end);
}

static void
mini_gc_init_gc_map (MonoCompile *cfg)
{
	if (COMPILE_LLVM (cfg))
		return;

#if 1
	/* Debugging support */
	{
		static int precise_count;

		precise_count ++;
		if (getenv ("MONO_GCMAP_COUNT")) {
			if (precise_count == atoi (getenv ("MONO_GCMAP_COUNT")))
				printf ("LAST: %s\n", mono_method_full_name (cfg->method, TRUE));
			if (precise_count > atoi (getenv ("MONO_GCMAP_COUNT")))
				return;
		}
	}
#endif

	cfg->compute_gc_maps = TRUE;

	cfg->gc_info = mono_mempool_alloc0 (cfg->mempool, sizeof (MonoCompileGC));
}

/*
 * mini_gc_set_slot_type_from_fp:
 *
 *   Set the GC slot type of the stack slot identified by SLOT_OFFSET, which should be
 * relative to the frame pointer. By default, all stack slots are type PIN, so there is no
 * need to call this function for those slots.
 */
void
mini_gc_set_slot_type_from_fp (MonoCompile *cfg, int slot_offset, GCSlotType type)
{
	MonoCompileGC *gcfg = (MonoCompileGC*)cfg->gc_info;

	if (!cfg->compute_gc_maps)
		return;

	g_assert (slot_offset % sizeof (mgreg_t) == 0);

	gcfg->stack_slots_from_fp = g_slist_prepend_mempool (cfg->mempool, gcfg->stack_slots_from_fp, GINT_TO_POINTER (((slot_offset) << 16) | type));
}

/*
 * mini_gc_set_slot_type_from_cfa:
 *
 *   Set the GC slot type of the stack slot identified by SLOT_OFFSET, which should be
 * relative to the DWARF CFA value. This should be called from mono_arch_emit_prolog ().
 * If type is STACK_REF, the slot is assumed to be live from the end of the prolog until
 * the end of the method. By default, all stack slots are type PIN, so there is no need to
 * call this function for those slots.
 */
void
mini_gc_set_slot_type_from_cfa (MonoCompile *cfg, int slot_offset, GCSlotType type)
{
	MonoCompileGC *gcfg = (MonoCompileGC*)cfg->gc_info;
	int slot = - (slot_offset / sizeof (mgreg_t));

	if (!cfg->compute_gc_maps)
		return;

	g_assert (slot_offset <= 0);
	g_assert (slot_offset % sizeof (mgreg_t) == 0);

	gcfg->stack_slots_from_cfa = g_slist_prepend_mempool (cfg->mempool, gcfg->stack_slots_from_cfa, GUINT_TO_POINTER (((slot) << 16) | type));
}

static inline int
fp_offset_to_slot (MonoCompile *cfg, int offset)
{
	MonoCompileGC *gcfg = cfg->gc_info;

	return (offset - gcfg->min_offset) / sizeof (mgreg_t);
}

static inline int
slot_to_fp_offset (MonoCompile *cfg, int slot)
{
	MonoCompileGC *gcfg = cfg->gc_info;

	return (slot * sizeof (mgreg_t)) + gcfg->min_offset;
}

static inline void
set_slot (MonoCompileGC *gcfg, int slot, int callsite_index, GCSlotType type)
{
	g_assert (slot >= 0 && slot < gcfg->nslots);

	if (type == SLOT_PIN) {
		clear_bit (gcfg->ref_bitmap, gcfg->bitmap_width, callsite_index, slot);
		set_bit (gcfg->pin_bitmap, gcfg->bitmap_width, callsite_index, slot);
	} else if (type == SLOT_REF) {
		set_bit (gcfg->ref_bitmap, gcfg->bitmap_width, callsite_index, slot);
		clear_bit (gcfg->pin_bitmap, gcfg->bitmap_width, callsite_index, slot);
	} else if (type == SLOT_NOREF) {
		clear_bit (gcfg->ref_bitmap, gcfg->bitmap_width, callsite_index, slot);
		clear_bit (gcfg->pin_bitmap, gcfg->bitmap_width, callsite_index, slot);
	}
}

static inline void
set_slot_everywhere (MonoCompileGC *gcfg, int slot, GCSlotType type)
{
	int cindex;

	for (cindex = 0; cindex < gcfg->ncallsites; ++cindex)
		set_slot (gcfg, slot, cindex, type);
}

static inline void
set_slot_in_range (MonoCompileGC *gcfg, int slot, int from, int to, GCSlotType type)
{
	int cindex;

	for (cindex = 0; cindex < gcfg->ncallsites; ++cindex) {
		int callsite_offset = gcfg->callsites [cindex]->pc_offset;
		if (callsite_offset >= from && callsite_offset < to)
			set_slot (gcfg, slot, cindex, type);
	}
}

static inline void
set_reg_slot (MonoCompileGC *gcfg, int slot, int callsite_index, GCSlotType type)
{
	g_assert (slot >= 0 && slot < gcfg->nregs);

	if (type == SLOT_PIN) {
		clear_bit (gcfg->reg_ref_bitmap, gcfg->reg_bitmap_width, callsite_index, slot);
		set_bit (gcfg->reg_pin_bitmap, gcfg->reg_bitmap_width, callsite_index, slot);
	} else if (type == SLOT_REF) {
		set_bit (gcfg->reg_ref_bitmap, gcfg->reg_bitmap_width, callsite_index, slot);
		clear_bit (gcfg->reg_pin_bitmap, gcfg->reg_bitmap_width, callsite_index, slot);
	} else if (type == SLOT_NOREF) {
		clear_bit (gcfg->reg_ref_bitmap, gcfg->reg_bitmap_width, callsite_index, slot);
		clear_bit (gcfg->reg_pin_bitmap, gcfg->reg_bitmap_width, callsite_index, slot);
	}
}

static inline void
set_reg_slot_everywhere (MonoCompileGC *gcfg, int slot, GCSlotType type)
{
	int cindex;

	for (cindex = 0; cindex < gcfg->ncallsites; ++cindex)
		set_reg_slot (gcfg, slot, cindex, type);
}

static inline void
set_reg_slot_in_range (MonoCompileGC *gcfg, int slot, int from, int to, GCSlotType type)
{
	int cindex;

	for (cindex = 0; cindex < gcfg->ncallsites; ++cindex) {
		int callsite_offset = gcfg->callsites [cindex]->pc_offset;
		if (callsite_offset >= from && callsite_offset < to)
			set_reg_slot (gcfg, slot, cindex, type);
	}
}

static void
process_spill_slots (MonoCompile *cfg)
{
	MonoCompileGC *gcfg = cfg->gc_info;
	MonoBasicBlock *bb;
	GSList *l;
	int i;

	/* Mark all ref/pin spill slots as NOREF by default outside of their live range */
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		for (l = bb->spill_slot_defs; l; l = l->next) {
			MonoInst *def = l->data;
			int spill_slot = def->inst_c0;
			int bank = def->inst_c1;
			int offset = cfg->spill_info [bank][spill_slot].offset;
			int slot = fp_offset_to_slot (cfg, offset);

			if (bank == MONO_REG_INT_MP || bank == MONO_REG_INT_REF)
				set_slot_everywhere (gcfg, slot, SLOT_NOREF);
		}
	}

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		for (l = bb->spill_slot_defs; l; l = l->next) {
			MonoInst *def = l->data;
			int spill_slot = def->inst_c0;
			int bank = def->inst_c1;
			int offset = cfg->spill_info [bank][spill_slot].offset;
			int slot = fp_offset_to_slot (cfg, offset);
			GCSlotType type;

			if (bank == MONO_REG_INT_MP)
				type = SLOT_PIN;
			else
				type = SLOT_REF;

			/*
			 * Extend the live interval for the GC tracked spill slots
			 * defined in this bblock.
			 * FIXME: This is not needed.
			 */
			set_slot_in_range (gcfg, slot, def->backend.pc_offset, bb->native_offset + bb->native_length, type);

			if (cfg->verbose_level > 1)
				printf ("\t%s spill slot at %s0x%x(fp) (slot = %d)\n", slot_type_to_string (type), offset >= 0 ? "+" : "-", ABS (offset), slot);
		}
	}

	/* Set fp spill slots to NOREF */
	for (i = 0; i < cfg->spill_info_len [MONO_REG_DOUBLE]; ++i) {
		int offset = cfg->spill_info [MONO_REG_DOUBLE][i].offset;
		int slot;

		if (offset == -1)
			continue;

		slot = fp_offset_to_slot (cfg, offset);

		set_slot_everywhere (gcfg, slot, SLOT_NOREF);
		/* FIXME: 32 bit */
		if (cfg->verbose_level > 1)
			printf ("\tfp spill slot at %s0x%x(fp) (slot = %d)\n", offset >= 0 ? "" : "-", ABS (offset), slot);
	}

	/* Set int spill slots to NOREF */
	for (i = 0; i < cfg->spill_info_len [MONO_REG_INT]; ++i) {
		int offset = cfg->spill_info [MONO_REG_INT][i].offset;
		int slot;

		if (offset == -1)
			continue;

		slot = fp_offset_to_slot (cfg, offset);

		set_slot_everywhere (gcfg, slot, SLOT_NOREF);
		if (cfg->verbose_level > 1)
			printf ("\tint spill slot at fp+0x%x (slot = %d)\n", offset, slot);
	}
}

/*
 * process_other_slots:
 *
 *   Process stack slots registered using mini_gc_set_slot_type_... ().
 */
static void
process_other_slots (MonoCompile *cfg)
{
	MonoCompileGC *gcfg = cfg->gc_info;
	GSList *l;

	/* Relative to the CFA */
	for (l = gcfg->stack_slots_from_cfa; l; l = l->next) {
		guint data = GPOINTER_TO_UINT (l->data);
		int cfa_slot = data >> 16;
		GCSlotType type = data & 0xff;
		int slot;
		
		/*
		 * Map the cfa relative slot to an fp relative slot.
		 * slot_addr == cfa - <cfa_slot>*4/8
		 * fp + cfa_offset == cfa
		 * -> slot_addr == fp + (cfa_offset - <cfa_slot>*4/8)
		 */
		slot = (cfg->cfa_offset / sizeof (mgreg_t)) - cfa_slot - (gcfg->min_offset / sizeof (mgreg_t));

		set_slot_everywhere (gcfg, slot, type);

		if (cfg->verbose_level > 1) {
			int fp_offset = slot_to_fp_offset (cfg, slot);
			if (type == SLOT_NOREF)
				printf ("\tnoref slot at %s0x%x(fp) (slot = %d) (cfa - 0x%x)\n", fp_offset >= 0 ? "" : "-", ABS (fp_offset), slot, (int)(cfa_slot * sizeof (mgreg_t)));
		}
	}

	/* Relative to the FP */
	for (l = gcfg->stack_slots_from_fp; l; l = l->next) {
		gint data = GPOINTER_TO_INT (l->data);
		int offset = data >> 16;
		GCSlotType type = data & 0xff;
		int slot;
		
		slot = fp_offset_to_slot (cfg, offset);

		set_slot_everywhere (gcfg, slot, type);

		/* Liveness for these slots is handled by process_spill_slots () */

		if (cfg->verbose_level > 1) {
			if (type == SLOT_REF)
				printf ("\tref slot at fp+0x%x (slot = %d)\n", offset, slot);
		}
	}
}

static void
process_variables (MonoCompile *cfg)
{
	MonoCompileGC *gcfg = cfg->gc_info;
	MonoMethodSignature *sig = mono_method_signature (cfg->method);
	int i, locals_min_slot, locals_max_slot, cindex;
	MonoBasicBlock *bb;
	MonoInst *tmp;
	int *pc_offsets;
	int locals_min_offset = gcfg->locals_min_offset;
	int locals_max_offset = gcfg->locals_max_offset;

	/* Slots for locals are NOREF by default */
	locals_min_slot = (locals_min_offset - gcfg->min_offset) / sizeof (mgreg_t);
	locals_max_slot = (locals_max_offset - gcfg->min_offset) / sizeof (mgreg_t);
	for (i = locals_min_slot; i < locals_max_slot; ++i) {
		set_slot_everywhere (gcfg, i, SLOT_NOREF);
	}

	/*
	 * Compute the offset where variables are initialized in the first bblock, if any.
	 */
	pc_offsets = g_new0 (int, cfg->next_vreg);

	bb = cfg->bb_entry->next_bb;
	MONO_BB_FOR_EACH_INS (bb, tmp) {
		if (tmp->opcode == OP_GC_LIVENESS_DEF) {
			int vreg = tmp->inst_c1;
			if (pc_offsets [vreg] == 0) {
				g_assert (tmp->backend.pc_offset > 0);
				pc_offsets [vreg] = tmp->backend.pc_offset;
			}
		}
	}

	/*
	 * Stack slots holding arguments are initialized in the prolog.
	 * This means we can treat them alive for the whole method.
	 */
	for (i = 0; i < cfg->num_varinfo; i++) {
		MonoInst *ins = cfg->varinfo [i];
		MonoType *t = ins->inst_vtype;
		MonoMethodVar *vmv;
		guint32 pos;
		gboolean byref, is_this = FALSE;
		gboolean is_arg = i < cfg->locals_start;

		if (ins == cfg->ret)
			continue;

		vmv = MONO_VARINFO (cfg, i);

		/* For some reason, 'this' is byref */
		if (sig->hasthis && ins == cfg->args [0] && !cfg->method->klass->valuetype) {
			t = &cfg->method->klass->byval_arg;
			is_this = TRUE;
		}

		byref = t->byref;

		if (ins->opcode == OP_REGVAR) {
			int hreg;
			GCSlotType slot_type;

			t = mini_type_get_underlying_type (NULL, t);

			hreg = ins->dreg;
			g_assert (hreg < MONO_MAX_IREGS);

			// FIXME: Add back this check
#if 0
			if (is_arg && gcfg->reg_live_intervals [hreg]) {
				/* 
				 * FIXME: This argument shares a hreg with a local, we can't add the whole
				 * method as a live interval, since it would overlap with the locals
				 * live interval.
				 */
				continue;
			}
#endif

			if (byref)
				slot_type = SLOT_PIN;
			else
				slot_type = MONO_TYPE_IS_REFERENCE (t) ? SLOT_REF : SLOT_NOREF;

			if (is_arg) {
				/* Live for the whole method */
				set_reg_slot_everywhere (gcfg, hreg, slot_type);
			} else {
				if (slot_type == SLOT_PIN) {
					/* These have no live interval, be conservative */
					set_reg_slot_everywhere (gcfg, hreg, slot_type);
				} else {
					/*
					 * Unlike variables allocated to the stack, we generate liveness info
					 * for noref vars in registers in mono_spill_global_vars (), because
					 * knowing that a register doesn't contain a ref allows us to mark its save
					 * locations precisely.
					 */
					for (cindex = 0; cindex < gcfg->ncallsites; ++cindex)
						if (gcfg->callsites [cindex]->liveness [i / 8] & (1 << (i % 8)))
							set_reg_slot (gcfg, hreg, cindex, slot_type);
				}
			}

			if (cfg->verbose_level > 1) {
				printf ("\t%s %sreg %s(R%d)\n", slot_type_to_string (slot_type), is_arg ? "arg " : "", mono_arch_regname (hreg), vmv->vreg);
			}

			continue;
		}

		if (ins->opcode != OP_REGOFFSET)
			continue;

		if (ins->inst_offset % sizeof (mgreg_t) != 0)
			continue;

		if (is_arg && ins->inst_offset >= gcfg->max_offset)
			/* In parent frame */
			continue;

		pos = fp_offset_to_slot (cfg, ins->inst_offset);

		if (is_arg && ins->flags & MONO_INST_IS_DEAD) {
			/* These do not get stored in the prolog */
			set_slot_everywhere (gcfg, pos, SLOT_NOREF);

			if (cfg->verbose_level > 1) {
				printf ("\tdead arg at fp%s0x%x (slot=%d): %s\n", ins->inst_offset < 0 ? "-" : "+", (ins->inst_offset < 0) ? -(int)ins->inst_offset : (int)ins->inst_offset, pos, mono_type_full_name (ins->inst_vtype));
			}
			continue;
		}

		if (MONO_TYPE_ISSTRUCT (t)) {
			int numbits = 0, j;
			gsize *bitmap = NULL;
			gboolean pin = FALSE;
			int size;
			int size_in_slots;
			
			if (ins->backend.is_pinvoke)
				size = mono_class_native_size (ins->klass, NULL);
			else
				size = mono_class_value_size (ins->klass, NULL);
			size_in_slots = ALIGN_TO (size, sizeof (mgreg_t)) / sizeof (mgreg_t);

			if (!ins->klass->has_references) {
				if (is_arg) {
					for (j = 0; j < size_in_slots; ++j)
						set_slot_everywhere (gcfg, pos + j, SLOT_NOREF);
				}
				continue;
			}

			if (ins->klass->generic_container || mono_class_is_open_constructed_type (t)) {
				/* FIXME: Generic sharing */
				pin = TRUE;
			} else {
				mono_class_compute_gc_descriptor (ins->klass);

				bitmap = mono_gc_get_bitmap_for_descr (ins->klass->gc_descr, &numbits);
				g_assert (bitmap);

				/*
				 * Most vtypes are marked volatile because of the LDADDR instructions,
				 * and they have no liveness information since they are decomposed
				 * before the liveness pass. We emit OP_GC_LIVENESS_DEF instructions for
				 * them during VZERO decomposition.
				 */
				if (!pc_offsets [vmv->vreg])
					pin = TRUE;
			}

			if (ins->backend.is_pinvoke)
				pin = TRUE;

			if (bitmap) {
				if (pc_offsets [vmv->vreg]) {
					for (cindex = 0; cindex < gcfg->ncallsites; ++cindex) {
						if (gcfg->callsites [cindex]->pc_offset > pc_offsets [vmv->vreg]) {
							for (j = 0; j < numbits; ++j) {
								if (bitmap [j / GC_BITS_PER_WORD] & ((gsize)1 << (j % GC_BITS_PER_WORD))) {
									/* The descriptor is for the boxed object */
									set_slot (gcfg, (pos + j - (sizeof (MonoObject) / sizeof (gpointer))), cindex, pin ? SLOT_PIN : SLOT_REF);
								}
							}
						}
					}
				}
			} else if (pin) {
				for (j = 0; j < size_in_slots; ++j) {
					set_slot_everywhere (gcfg, pos + j, SLOT_PIN);
				}
			}

			g_free (bitmap);

			if (cfg->verbose_level > 1) {
				printf ("\tvtype R%d at fp+0x%x-0x%x: %s\n", vmv->vreg, (int)ins->inst_offset, (int)(ins->inst_offset + (size / sizeof (mgreg_t))), mono_type_full_name (ins->inst_vtype));
			}

			continue;
		}

		if (!is_arg && (ins->inst_offset < gcfg->min_offset || ins->inst_offset >= gcfg->max_offset))
			/* Vret addr etc. */
			continue;

		if (t->byref) {
			for (cindex = 0; cindex < gcfg->ncallsites; ++cindex)
				if (gcfg->callsites [cindex]->liveness [i / 8] & (1 << (i % 8)))
					set_slot (gcfg, pos, cindex, SLOT_PIN);
			continue;
		}

		/*
		 * This is currently disabled, but could be enabled to debug crashes.
		 */
#if 0
		if (t->type == MONO_TYPE_I) {
			/*
			 * Variables created in mono_handle_global_vregs have type I, but they
			 * could hold GC refs since the vregs they were created from might not been
			 * marked as holding a GC ref. So be conservative.
			 */
			set_slot_everywhere (gcfg, pos, SLOT_PIN);
			continue;
		}
#endif

		t = mini_type_get_underlying_type (NULL, t);

		if (!MONO_TYPE_IS_REFERENCE (t)) {
			set_slot_everywhere (gcfg, pos, SLOT_NOREF);
			if (cfg->verbose_level > 1)
				printf ("\tnoref at %s0x%x(fp) (R%d, slot=%d): %s\n", ins->inst_offset < 0 ? "-" : "", (ins->inst_offset < 0) ? -(int)ins->inst_offset : (int)ins->inst_offset, vmv->vreg, pos, mono_type_full_name (ins->inst_vtype));
			continue;
		}

		/* 'this' is marked INDIRECT for gshared methods */
		if (ins->flags & (MONO_INST_VOLATILE | MONO_INST_INDIRECT) && !is_this) {
			/*
			 * For volatile variables, treat them alive from the point they are
			 * initialized in the first bblock until the end of the method.
			 */
			if (is_arg) {
				set_slot_everywhere (gcfg, pos, SLOT_REF);
			} else if (pc_offsets [vmv->vreg]) {
				set_slot_in_range (gcfg, pos, 0, pc_offsets [vmv->vreg], SLOT_PIN);
				set_slot_in_range (gcfg, pos, pc_offsets [vmv->vreg], cfg->code_size, SLOT_REF);
			} else {
				set_slot_everywhere (gcfg, pos, SLOT_PIN);
			}
			if (cfg->verbose_level > 1)
				printf ("\tvolatile ref at %s0x%x(fp) (R%d, slot=%d): %s\n", ins->inst_offset < 0 ? "-" : "", (ins->inst_offset < 0) ? -(int)ins->inst_offset : (int)ins->inst_offset, vmv->vreg, pos, mono_type_full_name (ins->inst_vtype));
			continue;
		}

		if (is_arg) {
			/* Live for the whole method */
			set_slot_everywhere (gcfg, pos, SLOT_REF);
		} else {
			for (cindex = 0; cindex < gcfg->ncallsites; ++cindex)
				if (gcfg->callsites [cindex]->liveness [i / 8] & (1 << (i % 8)))
					set_slot (gcfg, pos, cindex, SLOT_REF);
		}

		if (cfg->verbose_level > 1) {
			printf ("\tref at %s0x%x(fp) (R%d, slot=%d): %s\n", ins->inst_offset < 0 ? "-" : "", (ins->inst_offset < 0) ? -(int)ins->inst_offset : (int)ins->inst_offset, vmv->vreg, pos, mono_type_full_name (ins->inst_vtype));
		}
	}

	g_free (pc_offsets);
}

static int
sp_offset_to_fp_offset (MonoCompile *cfg, int sp_offset)
{
	/* 
	 * Convert a sp relative offset to a slot index. This is
	 * platform specific.
	 */
#ifdef TARGET_AMD64
	/* fp = sp + offset */
	g_assert (cfg->frame_reg == AMD64_RBP);
	return (- cfg->arch.sp_fp_offset + sp_offset);
#else
	NOT_IMPLEMENTED;
	return -1;
#endif
}

static GCSlotType
type_to_gc_slot_type (MonoType *t)
{
	if (t->byref)
		return SLOT_PIN;
	t = mini_type_get_underlying_type (NULL, t);
	if (MONO_TYPE_IS_REFERENCE (t))
		return SLOT_REF;
	else {
		if (MONO_TYPE_ISSTRUCT (t)) {
			MonoClass *klass = mono_class_from_mono_type (t);
			if (!klass->has_references) {
				return SLOT_NOREF;
			} else {
				// FIXME:
				return SLOT_PIN;
			}
		}
		return SLOT_NOREF;
	}
}

static void
process_param_area_slots (MonoCompile *cfg)
{
	MonoCompileGC *gcfg = cfg->gc_info;
	int i;
	gboolean *is_param;

	/*
	 * These slots are used for passing parameters during calls. They are sp relative, not
	 * fp relative, so they are harder to handle.
	 */
	if (cfg->flags & MONO_CFG_HAS_ALLOCA)
		/* The distance between fp and sp is not constant */
		return;

	is_param = mono_mempool_alloc0 (cfg->mempool, gcfg->nslots * sizeof (gboolean));

	for (i = 0; i < gcfg->ncallsites; ++i) {
		GCCallSite *callsite = gcfg->callsites [i];
		GSList *l;

		for (l = callsite->param_slots; l; l = l->next) {
			MonoInst *def = l->data;
			int sp_offset = def->inst_offset;
			int fp_offset = sp_offset_to_fp_offset (cfg, sp_offset);
			int slot = fp_offset_to_slot (cfg, fp_offset);

			g_assert (slot >= 0 && slot < gcfg->nslots);
			is_param [slot] = TRUE;
		}
	}

	/* All param area slots are noref by default */
	for (i = 0; i < gcfg->nslots; ++i) {
		if (is_param [i])
			set_slot_everywhere (gcfg, i, SLOT_NOREF);
	}

	for (i = 0; i < gcfg->ncallsites; ++i) {
		GCCallSite *callsite = gcfg->callsites [i];
		GSList *l;

		for (l = callsite->param_slots; l; l = l->next) {
			MonoInst *def = l->data;
			MonoType *t = def->inst_vtype;
			int sp_offset = def->inst_offset;
			int fp_offset = sp_offset_to_fp_offset (cfg, sp_offset);
			int slot = fp_offset_to_slot (cfg, fp_offset);
			GCSlotType type = type_to_gc_slot_type (t);

			/* The slot is live between the def instruction and the call */
			set_slot_in_range (gcfg, slot, def->backend.pc_offset, callsite->pc_offset + 1, type);
			if (cfg->verbose_level > 1)
				printf ("\t%s param area slot at %s0x%x(fp)=0x%x(sp) (slot = %d) [0x%x-0x%x]\n", slot_type_to_string (type), fp_offset >= 0 ? "+" : "-", ABS (fp_offset), sp_offset, slot, def->backend.pc_offset, callsite->pc_offset + 1);
		}
	}
}

static void
compute_frame_size (MonoCompile *cfg)
{
	int i, locals_min_offset, locals_max_offset, cfa_min_offset, cfa_max_offset;
	int min_offset, max_offset;
	MonoCompileGC *gcfg = cfg->gc_info;
	MonoMethodSignature *sig = mono_method_signature (cfg->method);
	GSList *l;

	/* Compute min/max offsets from the fp */

	/* Locals */
#ifdef TARGET_AMD64
	locals_min_offset = ALIGN_TO (cfg->locals_min_stack_offset, sizeof (mgreg_t));
	locals_max_offset = cfg->locals_max_stack_offset;
#else
	/* min/max stack offset needs to be computed in mono_arch_allocate_vars () */
	NOT_IMPLEMENTED;
#endif

	locals_min_offset = ALIGN_TO (locals_min_offset, sizeof (mgreg_t));
	locals_max_offset = ALIGN_TO (locals_max_offset, sizeof (mgreg_t));

	min_offset = locals_min_offset;
	max_offset = locals_max_offset;

	/* Arguments */
	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		MonoInst *ins = cfg->args [i];

		if (ins->opcode == OP_REGOFFSET)
			min_offset = MIN (min_offset, ins->inst_offset);
	}

	/* Cfa slots */
	g_assert (cfg->frame_reg == cfg->cfa_reg);
	g_assert (cfg->cfa_offset > 0);
	cfa_min_offset = 0;
	cfa_max_offset = cfg->cfa_offset;

	min_offset = MIN (min_offset, cfa_min_offset);
	max_offset = MAX (max_offset, cfa_max_offset);

	/* Fp relative slots */
	for (l = gcfg->stack_slots_from_fp; l; l = l->next) {
		gint data = GPOINTER_TO_INT (l->data);
		int offset = data >> 16;

		min_offset = MIN (min_offset, offset);
	}

	/* Spill slots */
	if (!(cfg->flags & MONO_CFG_HAS_SPILLUP)) {
		int stack_offset = ALIGN_TO (cfg->stack_offset, sizeof (mgreg_t));
		min_offset = MIN (min_offset, (-stack_offset));
	}

	/* Param area slots */
#ifdef TARGET_AMD64
	min_offset = MIN (min_offset, -cfg->arch.sp_fp_offset);
#endif

	gcfg->min_offset = min_offset;
	gcfg->max_offset = max_offset;
	gcfg->locals_min_offset = locals_min_offset;
	gcfg->locals_max_offset = locals_max_offset;
}

static void
init_gcfg (MonoCompile *cfg)
{
	int i, nregs, nslots;
	MonoCompileGC *gcfg = cfg->gc_info;
	GCCallSite **callsites;
	int ncallsites;
	MonoBasicBlock *bb;
	GSList *l;

	/*
	 * Collect callsites
	 */
	ncallsites = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		ncallsites += g_slist_length (bb->gc_callsites);
	}
	callsites = mono_mempool_alloc0 (cfg->mempool, ncallsites * sizeof (GCCallSite*));
	i = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		for (l = bb->gc_callsites; l; l = l->next)
			callsites [i++] = l->data;
	}

	/* The callsites should already be ordered by pc offset */
	for (i = 1; i < ncallsites; ++i)
		g_assert (callsites [i - 1]->pc_offset < callsites [i]->pc_offset);

	/*
	 * The stack frame looks like this:
	 *
	 * <fp + max_offset> == cfa ->  <end of previous frame>
	 *                              <other stack slots>
	 *                              <locals>
	 *                              <other stack slots>
	 * fp + min_offset          ->
	 * ...
	 * fp                       ->
	 */

	if (cfg->verbose_level > 1)
		printf ("GC Map for %s: 0x%x-0x%x\n", mono_method_full_name (cfg->method, TRUE), gcfg->min_offset, gcfg->max_offset);

	nslots = (gcfg->max_offset - gcfg->min_offset) / sizeof (mgreg_t);
	nregs = NREGS;

	gcfg->nslots = nslots;
	gcfg->nregs = nregs;
	gcfg->callsites = callsites;
	gcfg->ncallsites = ncallsites;
	gcfg->bitmap_width = ALIGN_TO (nslots, 8) / 8;
	gcfg->reg_bitmap_width = ALIGN_TO (nregs, 8) / 8;
	gcfg->ref_bitmap = mono_mempool_alloc0 (cfg->mempool, gcfg->bitmap_width * ncallsites);
	gcfg->pin_bitmap = mono_mempool_alloc0 (cfg->mempool, gcfg->bitmap_width * ncallsites);
	gcfg->reg_ref_bitmap = mono_mempool_alloc0 (cfg->mempool, gcfg->reg_bitmap_width * ncallsites);
	gcfg->reg_pin_bitmap = mono_mempool_alloc0 (cfg->mempool, gcfg->reg_bitmap_width * ncallsites);

	/* All slots start out as PIN */
	memset (gcfg->pin_bitmap, 0xff, gcfg->bitmap_width * ncallsites);
	for (i = 0; i < nregs; ++i) {
		/*
		 * By default, registers are PIN.
		 * This is because we don't know their type outside their live range, since
		 * they could have the same value as in the caller, or a value set by the
		 * current method etc.
		 */
		if ((cfg->used_int_regs & (1 << i)))
			set_reg_slot_everywhere (gcfg, i, SLOT_PIN);
	}
}


static void
create_map (MonoCompile *cfg)
{
	GCMap *map;
	int i, j, nregs, nslots, nref_regs, npin_regs, alloc_size, bitmaps_size, bitmaps_offset;
	int ntypes [16];
	int stack_bitmap_width, stack_bitmap_size, reg_ref_bitmap_width, reg_ref_bitmap_size;
	int reg_pin_bitmap_width, reg_pin_bitmap_size, bindex;
	int start, end;
	gboolean has_ref_slots, has_pin_slots, has_ref_regs, has_pin_regs;
	MonoCompileGC *gcfg = cfg->gc_info;
	GCCallSite **callsites;
	int ncallsites;
	guint8 *bitmap, *bitmaps;
	guint32 reg_ref_mask, reg_pin_mask;

	ncallsites = gcfg->ncallsites;
	nslots = gcfg->nslots;
	nregs = gcfg->nregs;
	callsites = gcfg->callsites;

	/* 
	 * Compute the real size of the bitmap i.e. ignore NOREF columns at the beginning and at
	 * the end. Also, compute whenever the map needs ref/pin bitmaps, and collect stats.
	 */
	has_ref_slots = FALSE;
	has_pin_slots = FALSE;
	start = -1;
	end = -1;
	memset (ntypes, 0, sizeof (ntypes));
	for (i = 0; i < nslots; ++i) {
		gboolean has_ref = FALSE;
		gboolean has_pin = FALSE;

		for (j = 0; j < ncallsites; ++j) {
			if (get_bit (gcfg->pin_bitmap, gcfg->bitmap_width, j, i))
				has_pin = TRUE;
			if (get_bit (gcfg->ref_bitmap, gcfg->bitmap_width, j, i))
				has_ref = TRUE;
		}

		if (has_ref)
			has_ref_slots = TRUE;
		if (has_pin)
			has_pin_slots = TRUE;

		if (has_ref)
			ntypes [SLOT_REF] ++;
		else if (has_pin)
			ntypes [SLOT_PIN] ++;
		else
			ntypes [SLOT_NOREF] ++;

		if (has_ref || has_pin) {
			if (start == -1)
				start = i;
			end = i + 1;
		}
	}
	if (start == -1) {
		start = end = nslots;
	} else {
		g_assert (start != -1);
		g_assert (start < end);
	}

	has_ref_regs = FALSE;
	has_pin_regs = FALSE;
	reg_ref_mask = 0;
	reg_pin_mask = 0;
	nref_regs = 0;
	npin_regs = 0;
	for (i = 0; i < nregs; ++i) {
		gboolean has_ref = FALSE;
		gboolean has_pin = FALSE;

		for (j = 0; j < ncallsites; ++j) {
			if (get_bit (gcfg->reg_ref_bitmap, gcfg->reg_bitmap_width, j, i)) {
				has_ref = TRUE;
				break;
			}
		}
		for (j = 0; j < ncallsites; ++j) {
			if (get_bit (gcfg->reg_pin_bitmap, gcfg->reg_bitmap_width, j, i)) {
				has_pin = TRUE;
				break;
			}
		}

		if (has_ref) {
			reg_ref_mask |= (1 << i);
			has_ref_regs = TRUE;
			nref_regs ++;
		}
		if (has_pin) {
			reg_pin_mask |= (1 << i);
			has_pin_regs = TRUE;
			npin_regs ++;
		}
	}

	if (cfg->verbose_level > 1)
		printf ("Slots: %d Start: %d End: %d Refs: %d NoRefs: %d Pin: %d Callsites: %d\n", nslots, start, end, ntypes [SLOT_REF], ntypes [SLOT_NOREF], ntypes [SLOT_PIN], ncallsites);

	/* Create the GC Map */

	stack_bitmap_width = ALIGN_TO (end - start, 8) / 8;
	stack_bitmap_size = stack_bitmap_width * ncallsites;
	reg_ref_bitmap_width = ALIGN_TO (nref_regs, 8) / 8;
	reg_ref_bitmap_size = reg_ref_bitmap_width * ncallsites;
	reg_pin_bitmap_width = ALIGN_TO (npin_regs, 8) / 8;
	reg_pin_bitmap_size = reg_pin_bitmap_width * ncallsites;
	bitmaps_size = (has_ref_slots ? stack_bitmap_size : 0) + (has_pin_slots ? stack_bitmap_size : 0) + (has_ref_regs ? reg_ref_bitmap_size : 0) + (has_pin_regs ? reg_pin_bitmap_size : 0);
	
	map = mono_mempool_alloc (cfg->mempool, sizeof (GCMap));

	map->frame_reg = cfg->frame_reg;
	map->start_offset = gcfg->min_offset;
	map->end_offset = gcfg->min_offset + (nslots * sizeof (mgreg_t));
	map->map_offset = start * sizeof (mgreg_t);
	map->nslots = end - start;
	map->has_ref_slots = has_ref_slots;
	map->has_pin_slots = has_pin_slots;
	map->has_ref_regs = has_ref_regs;
	map->has_pin_regs = has_pin_regs;
	g_assert (nregs < 32);
	map->reg_ref_mask = reg_ref_mask;
	map->reg_pin_mask = reg_pin_mask;
	map->nref_regs = nref_regs;
	map->npin_regs = npin_regs;

	bitmaps = mono_mempool_alloc0 (cfg->mempool, bitmaps_size);

	bitmaps_offset = 0;
	if (has_ref_slots) {
		map->stack_ref_bitmap_offset = bitmaps_offset;
		bitmaps_offset += stack_bitmap_size;

		bitmap = &bitmaps [map->stack_ref_bitmap_offset];
		for (i = 0; i < nslots; ++i) {
			for (j = 0; j < ncallsites; ++j) {
				if (get_bit (gcfg->ref_bitmap, gcfg->bitmap_width, j, i))
					set_bit (bitmap, stack_bitmap_width, j, i - start);
			}
		}
	}
	if (has_pin_slots) {
		map->stack_pin_bitmap_offset = bitmaps_offset;
		bitmaps_offset += stack_bitmap_size;

		bitmap = &bitmaps [map->stack_pin_bitmap_offset];
		for (i = 0; i < nslots; ++i) {
			for (j = 0; j < ncallsites; ++j) {
				if (get_bit (gcfg->pin_bitmap, gcfg->bitmap_width, j, i))
					set_bit (bitmap, stack_bitmap_width, j, i - start);
			}
		}
	}
	if (has_ref_regs) {
		map->reg_ref_bitmap_offset = bitmaps_offset;
		bitmaps_offset += reg_ref_bitmap_size;

		bitmap = &bitmaps [map->reg_ref_bitmap_offset];
		bindex = 0;
		for (i = 0; i < nregs; ++i) {
			if (reg_ref_mask & (1 << i)) {
				for (j = 0; j < ncallsites; ++j) {
					if (get_bit (gcfg->reg_ref_bitmap, gcfg->reg_bitmap_width, j, i))
						set_bit (bitmap, reg_ref_bitmap_width, j, bindex);
				}
				bindex ++;
			}
		}
	}
	if (has_pin_regs) {
		map->reg_pin_bitmap_offset = bitmaps_offset;
		bitmaps_offset += reg_pin_bitmap_size;

		bitmap = &bitmaps [map->reg_pin_bitmap_offset];
		bindex = 0;
		for (i = 0; i < nregs; ++i) {
			if (reg_pin_mask & (1 << i)) {
				for (j = 0; j < ncallsites; ++j) {
					if (get_bit (gcfg->reg_pin_bitmap, gcfg->reg_bitmap_width, j, i))
						set_bit (bitmap, reg_pin_bitmap_width, j, bindex);
				}
				bindex ++;
			}
		}
	}

	/* Call sites */
	map->ncallsites = ncallsites;
	if (cfg->code_len < 256)
		map->callsite_entry_size = 1;
	else if (cfg->code_len < 65536)
		map->callsite_entry_size = 2;
	else
		map->callsite_entry_size = 4;

	/* Encode the GC Map */
	{
		guint8 buf [256];
		guint8 *endbuf;
		GCEncodedMap *emap;
		int encoded_size;
		guint8 *p;

		encode_gc_map (map, buf, &endbuf);
		g_assert (endbuf - buf < 256);

		encoded_size = endbuf - buf;
		alloc_size = sizeof (GCEncodedMap) + ALIGN_TO (encoded_size, map->callsite_entry_size) + (map->callsite_entry_size * map->ncallsites) + bitmaps_size;

		emap = mono_domain_alloc0 (cfg->domain, alloc_size);
		//emap->ref_slots = map->ref_slots;

		/* Encoded fixed fields */
		p = &emap->encoded [0];
		//emap->encoded_size = encoded_size;
		memcpy (p, buf, encoded_size);
		p += encoded_size;

		/* Callsite table */
		p = (guint8*)ALIGN_TO ((guint64)p, map->callsite_entry_size);
		if (map->callsite_entry_size == 1) {
			guint8 *offsets = p;
			for (i = 0; i < ncallsites; ++i)
				offsets [i] = callsites [i]->pc_offset;
			stats.gc_callsites8_size += ncallsites * sizeof (guint8);
		} else if (map->callsite_entry_size == 2) {
			guint16 *offsets = (guint16*)p;
			for (i = 0; i < ncallsites; ++i)
				offsets [i] = callsites [i]->pc_offset;
			stats.gc_callsites16_size += ncallsites * sizeof (guint16);
		} else {
			guint32 *offsets = (guint32*)p;
			for (i = 0; i < ncallsites; ++i)
				offsets [i] = callsites [i]->pc_offset;
			stats.gc_callsites32_size += ncallsites * sizeof (guint32);
		}
		p += ncallsites * map->callsite_entry_size;

		/* Bitmaps */
		memcpy (p, bitmaps, bitmaps_size);

		stats.gc_maps_size += alloc_size;
		stats.gc_callsites_size += ncallsites * map->callsite_entry_size;
		stats.gc_bitmaps_size += bitmaps_size;
		stats.gc_map_struct_size += sizeof (GCEncodedMap) + encoded_size;

		cfg->jit_info->gc_info = emap;
	}

	stats.all_slots += nslots;
	stats.ref_slots += ntypes [SLOT_REF];
	stats.noref_slots += ntypes [SLOT_NOREF];
	stats.pin_slots += ntypes [SLOT_PIN];
}

void
mini_gc_create_gc_map (MonoCompile *cfg)
{
	int i;

	if (!cfg->compute_gc_maps)
		return;

	/*
	 * During marking, all frames except the top frame are at a call site, and we mark the
	 * top frame conservatively. This means that we only need to compute and record
	 * GC maps for call sites.
	 */

	if (!(cfg->comp_done & MONO_COMP_LIVENESS))
		/* Without liveness info, the live ranges are not precise enough */
		return;

	for (i = 0; i < cfg->header->num_clauses; ++i) {
		MonoExceptionClause *clause = &cfg->header->clauses [i];

		if (clause->flags == MONO_EXCEPTION_CLAUSE_FINALLY)
			/*
			 * The calls to the finally clauses don't show up in the cfg. See
			 * test_0_liveness_8 ().
			 */
			return;
	}

	mono_analyze_liveness_gc (cfg);

	compute_frame_size (cfg);

	init_gcfg (cfg);

	process_spill_slots (cfg);
	process_other_slots (cfg);
	process_param_area_slots (cfg);
	process_variables (cfg);

	create_map (cfg);
}

void
mini_gc_init (void)
{
	MonoGCCallbacks cb;

	memset (&cb, 0, sizeof (cb));
	cb.thread_attach_func = thread_attach_func;
	cb.thread_detach_func = thread_detach_func;
	cb.thread_suspend_func = thread_suspend_func;
	/* Comment this out to disable precise stack marking */
	cb.thread_mark_func = thread_mark_func;
	mono_gc_set_gc_callbacks (&cb);

	mono_counters_register ("GC Maps size",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.gc_maps_size);
	mono_counters_register ("GC Call Sites size",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.gc_callsites_size);
	mono_counters_register ("GC Bitmaps size",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.gc_bitmaps_size);
	mono_counters_register ("GC Map struct size",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.gc_map_struct_size);
	mono_counters_register ("GC Call Sites encoded using 8 bits",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.gc_callsites8_size);
	mono_counters_register ("GC Call Sites encoded using 16 bits",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.gc_callsites16_size);
	mono_counters_register ("GC Call Sites encoded using 32 bits",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.gc_callsites32_size);

	mono_counters_register ("GC Map slots (all)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.all_slots);
	mono_counters_register ("GC Map slots (ref)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.ref_slots);
	mono_counters_register ("GC Map slots (noref)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.noref_slots);
	mono_counters_register ("GC Map slots (pin)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.pin_slots);

	mono_counters_register ("GC TLS Data size",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.tlsdata_size);

	mono_counters_register ("Stack space scanned (all)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.scanned_stacks);
	mono_counters_register ("Stack space scanned (using GC Maps)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.scanned);
	mono_counters_register ("Stack space scanned (precise)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.scanned_precisely);
	mono_counters_register ("Stack space scanned (pin)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.scanned_conservatively);
	mono_counters_register ("Stack space scanned (pin registers)",
							MONO_COUNTER_GC | MONO_COUNTER_INT, &stats.scanned_registers);
}

#else

void
mini_gc_init (void)
{
}

static void
mini_gc_init_gc_map (MonoCompile *cfg)
{
}

void
mini_gc_create_gc_map (MonoCompile *cfg)
{
}

void
mini_gc_set_slot_type_from_fp (MonoCompile *cfg, int slot_offset, GCSlotType type)
{
}

void
mini_gc_set_slot_type_from_cfa (MonoCompile *cfg, int slot_offset, GCSlotType type)
{
}

#endif

/*
 * mini_gc_init_cfg:
 *
 *   Set GC specific options in CFG.
 */
void
mini_gc_init_cfg (MonoCompile *cfg)
{
	if (mono_gc_is_moving ()) {
		cfg->disable_ref_noref_stack_slot_share = TRUE;
		cfg->gen_write_barriers = TRUE;
	}

	mini_gc_init_gc_map (cfg);
}

/*
 * Problems with the current code:
 * - the stack walk is slow
 * - vtypes/refs used in EH regions are treated conservatively
 * - if the code is finished, less pinning will be done, causing problems because
 *   we promote all surviving objects to old-gen.
 */

/*
 * Ideas for creating smaller GC maps:
 * - remove empty columns from the bitmaps. This requires adding a mask bit array for
 *   each bitmap.
 * - merge reg and stack slot bitmaps, so the unused bits at the end of the reg bitmap are
 *   not wasted.
 * - if the bitmap width is not a multiple of 8, the remaining bits are wasted.
 * - group ref and non-ref stack slots together in mono_allocate_stack_slots ().
 * - add an index for the callsite table so that each entry can be encoded as a 1 byte difference
 *   from an index entry.
 */
