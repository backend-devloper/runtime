/*
 * unwind.c: Stack Unwinding Interface
 *
 * Authors:
 *   Zoltan Varga (vargaz@gmail.com)
 *
 * (C) 2008 Novell, Inc.
 */

#include "mini.h"
#include "mini-unwind.h"

#include <mono/utils/mono-counters.h>
#include <mono/metadata/threads-types.h>

typedef enum {
	LOC_SAME,
	LOC_OFFSET
} LocType;

typedef struct {
	LocType loc_type;
	int offset;
} Loc;

typedef struct {
	guint32 len;
	guint8 info [MONO_ZERO_LEN_ARRAY];
} MonoUnwindInfo;

static CRITICAL_SECTION unwind_mutex;

static MonoUnwindInfo **cached_info;
static int cached_info_next, cached_info_size;
/* Statistics */
static int unwind_info_size;

#define unwind_lock() EnterCriticalSection (&unwind_mutex)
#define unwind_unlock() LeaveCriticalSection (&unwind_mutex)

#ifdef TARGET_AMD64
static int map_hw_reg_to_dwarf_reg [] = { 0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
#define NUM_REGS AMD64_NREG
#define DWARF_DATA_ALIGN (-8)
#define DWARF_PC_REG (mono_hw_reg_to_dwarf_reg (AMD64_RIP))
#elif defined(TARGET_ARM)
// http://infocenter.arm.com/help/topic/com.arm.doc.ihi0040a/IHI0040A_aadwarf.pdf
static int map_hw_reg_to_dwarf_reg [] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
#define NUM_REGS 16
#define DWARF_DATA_ALIGN (-4)
#define DWARF_PC_REG (mono_hw_reg_to_dwarf_reg (ARMREG_LR))
#elif defined (TARGET_X86)
static int map_hw_reg_to_dwarf_reg [] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
/* + 1 is for IP */
#define NUM_REGS X86_NREG + 1
#define DWARF_DATA_ALIGN (-4)
#define DWARF_PC_REG (mono_hw_reg_to_dwarf_reg (X86_NREG))
#elif defined (TARGET_POWERPC)
// http://refspecs.linuxfoundation.org/ELF/ppc64/PPC-elf64abi-1.9.html
static int map_hw_reg_to_dwarf_reg [] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 
										  9, 10, 11, 12, 13, 14, 15, 16,
										  17, 18, 19, 20, 21, 22, 23, 24,
										  25, 26, 27, 28, 29, 30, 31 };
#define NUM_REGS 110
#define DWARF_DATA_ALIGN (-(gint32)sizeof (mgreg_t))
#define DWARF_PC_REG 108
#else
static int map_hw_reg_to_dwarf_reg [16];
#define NUM_REGS 16
#define DWARF_DATA_ALIGN 0
#define DWARF_PC_REG -1
#endif

static gboolean dwarf_reg_to_hw_reg_inited;

static int map_dwarf_reg_to_hw_reg [NUM_REGS];

/*
 * mono_hw_reg_to_dwarf_reg:
 *
 *   Map the hardware register number REG to the register number used by DWARF.
 */
int
mono_hw_reg_to_dwarf_reg (int reg)
{
#ifdef TARGET_POWERPC
	if (reg == ppc_lr)
		return 108;
	else
		g_assert (reg < NUM_REGS);
#endif

	if (NUM_REGS == 0) {
		g_assert_not_reached ();
		return -1;
	} else {
		return map_hw_reg_to_dwarf_reg [reg];
	}
}

static void
init_reg_map (void)
{
	int i;

	g_assert (NUM_REGS > 0);
	for (i = 0; i < sizeof (map_hw_reg_to_dwarf_reg) / sizeof (int); ++i) {
		map_dwarf_reg_to_hw_reg [mono_hw_reg_to_dwarf_reg (i)] = i;
	}

#ifdef TARGET_POWERPC
	map_dwarf_reg_to_hw_reg [DWARF_PC_REG] = ppc_lr;
#endif

	mono_memory_barrier ();
	dwarf_reg_to_hw_reg_inited = TRUE;
}

static inline int
mono_dwarf_reg_to_hw_reg (int reg)
{
	if (!dwarf_reg_to_hw_reg_inited)
		init_reg_map ();

	return map_dwarf_reg_to_hw_reg [reg];
}

static G_GNUC_UNUSED void
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

/*
 * mono_unwind_ops_encode:
 *
 *   Encode the unwind ops in UNWIND_OPS into the compact DWARF encoding.
 * Return a pointer to malloc'ed memory.
 */
guint8*
mono_unwind_ops_encode (GSList *unwind_ops, guint32 *out_len)
{
	GSList *l;
	MonoUnwindOp *op;
	int loc;
	guint8 *buf, *p, *res;

	p = buf = g_malloc0 (4096);

	loc = 0;
	l = unwind_ops;
	for (; l; l = l->next) {
		int reg;

		op = l->data;

		/* Convert the register from the hw encoding to the dwarf encoding */
		reg = mono_hw_reg_to_dwarf_reg (op->reg);

		/* Emit an advance_loc if neccesary */
		while (op->when > loc) {
			if (op->when - loc < 32) {
				*p ++ = DW_CFA_advance_loc | (op->when - loc);
				loc = op->when;
			} else {
				*p ++ = DW_CFA_advance_loc | (30);
				loc += 30;
			}
		}			

		switch (op->op) {
		case DW_CFA_def_cfa:
			*p ++ = op->op;
			encode_uleb128 (reg, p, &p);
			encode_uleb128 (op->val, p, &p);
			break;
		case DW_CFA_def_cfa_offset:
			*p ++ = op->op;
			encode_uleb128 (op->val, p, &p);
			break;
		case DW_CFA_def_cfa_register:
			*p ++ = op->op;
			encode_uleb128 (reg, p, &p);
			break;
		case DW_CFA_offset:
			if (reg > 63) {
				*p ++ = DW_CFA_offset_extended_sf;
				encode_uleb128 (reg, p, &p);
				encode_sleb128 (op->val / DWARF_DATA_ALIGN, p, &p);
			} else {
				*p ++ = DW_CFA_offset | reg;
				encode_uleb128 (op->val / DWARF_DATA_ALIGN, p, &p);
			}
			break;
		default:
			g_assert_not_reached ();
			break;
		}
	}
	
	g_assert (p - buf < 4096);
	*out_len = p - buf;
	res = g_malloc (p - buf);
	memcpy (res, buf, p - buf);
	g_free (buf);
	return res;
}

#if 0
#define UNW_DEBUG(stmt) do { stmt; } while (0)
#else
#define UNW_DEBUG(stmt) do { } while (0)
#endif

static G_GNUC_UNUSED void
print_dwarf_state (int cfa_reg, int cfa_offset, int ip, int nregs, Loc *locations)
{
	int i;

	printf ("\t%x: cfa=r%d+%d ", ip, cfa_reg, cfa_offset);

	for (i = 0; i < nregs; ++i)
		if (locations [i].loc_type == LOC_OFFSET)
			printf ("r%d@%d(cfa) ", i, locations [i].offset);
	printf ("\n");
}

/*
 * Given the state of the current frame as stored in REGS, execute the unwind 
 * operations in unwind_info until the location counter reaches POS. The result is 
 * stored back into REGS. OUT_CFA will receive the value of the CFA.
 * This function is signal safe.
 */
void
mono_unwind_frame (guint8 *unwind_info, guint32 unwind_info_len, 
				   guint8 *start_ip, guint8 *end_ip, guint8 *ip, mgreg_t *regs, 
				   int nregs, guint8 **out_cfa)
{
	Loc locations [NUM_REGS];
	int i, pos, reg, cfa_reg, cfa_offset;
	guint8 *p;
	guint8 *cfa_val;

	for (i = 0; i < NUM_REGS; ++i)
		locations [i].loc_type = LOC_SAME;

	p = unwind_info;
	pos = 0;
	cfa_reg = -1;
	cfa_offset = -1;
	while (pos <= ip - start_ip && p < unwind_info + unwind_info_len) {
		int op = *p & 0xc0;

		switch (op) {
		case DW_CFA_advance_loc:
			UNW_DEBUG (print_dwarf_state (cfa_reg, cfa_offset, pos, nregs, locations));
			pos += *p & 0x3f;
			p ++;
			break;
		case DW_CFA_offset:
			reg = *p & 0x3f;
			p ++;
			locations [reg].loc_type = LOC_OFFSET;
			locations [reg].offset = decode_uleb128 (p, &p) * DWARF_DATA_ALIGN;
			break;
		case 0: {
			int ext_op = *p;
			p ++;
			switch (ext_op) {
			case DW_CFA_def_cfa:
				cfa_reg = decode_uleb128 (p, &p);
				cfa_offset = decode_uleb128 (p, &p);
				break;
			case DW_CFA_def_cfa_offset:
				cfa_offset = decode_uleb128 (p, &p);
				break;
			case DW_CFA_def_cfa_register:
				cfa_reg = decode_uleb128 (p, &p);
				break;
			case DW_CFA_offset_extended_sf:
				reg = decode_uleb128 (p, &p);
				locations [reg].loc_type = LOC_OFFSET;
				locations [reg].offset = decode_sleb128 (p, &p) * DWARF_DATA_ALIGN;
				break;
			case DW_CFA_advance_loc4:
				pos += *(guint32*)p;
				p += 4;
				break;
			default:
				g_assert_not_reached ();
			}
			break;
		}
		default:
			g_assert_not_reached ();
		}
	}

	cfa_val = (guint8*)regs [mono_dwarf_reg_to_hw_reg (cfa_reg)] + cfa_offset;
	for (i = 0; i < NUM_REGS; ++i) {
		if (locations [i].loc_type == LOC_OFFSET) {
			int hreg = mono_dwarf_reg_to_hw_reg (i);
			g_assert (hreg < nregs);
			regs [hreg] = *(gssize*)(cfa_val + locations [i].offset);
		}
	}

	*out_cfa = cfa_val;
}

void
mono_unwind_init (void)
{
	InitializeCriticalSection (&unwind_mutex);

	mono_counters_register ("Unwind info size", MONO_COUNTER_JIT | MONO_COUNTER_INT, &unwind_info_size);
}

void
mono_unwind_cleanup (void)
{
	int i;

	DeleteCriticalSection (&unwind_mutex);

	if (!cached_info)
		return;

	for (i = 0; i < cached_info_next; ++i) {
		MonoUnwindInfo *cached = cached_info [i];

		g_free (cached);
	}

	g_free (cached_info);
}

/*
 * mono_cache_unwind_info
 *
 *   Save UNWIND_INFO in the unwind info cache and return an id which can be passed
 * to mono_get_cached_unwind_info to get a cached copy of the info.
 * A copy is made of the unwind info.
 * This function is useful for two reasons:
 * - many methods have the same unwind info
 * - MonoJitInfo->used_regs is an int so it can't store the pointer to the unwind info
 */
guint32
mono_cache_unwind_info (guint8 *unwind_info, guint32 unwind_info_len)
{
	int i;
	MonoUnwindInfo *info;

	unwind_lock ();

	if (cached_info == NULL) {
		cached_info_size = 16;
		cached_info = g_new0 (MonoUnwindInfo*, cached_info_size);
	}

	for (i = 0; i < cached_info_next; ++i) {
		MonoUnwindInfo *cached = cached_info [i];

		if (cached->len == unwind_info_len && memcmp (cached->info, unwind_info, unwind_info_len) == 0) {
			unwind_unlock ();
			return i;
		}
	}

	info = g_malloc (sizeof (MonoUnwindInfo) + unwind_info_len);
	info->len = unwind_info_len;
	memcpy (&info->info, unwind_info, unwind_info_len);

	i = cached_info_next;
	
	if (cached_info_next >= cached_info_size) {
		MonoUnwindInfo **old_table, **new_table;

		/*
		 * Have to resize the table, while synchronizing with 
		 * mono_get_cached_unwind_info () using hazard pointers.
		 */

		old_table = cached_info;
		new_table = g_new0 (MonoUnwindInfo*, cached_info_size * 2);

		memcpy (new_table, cached_info, cached_info_size * sizeof (MonoUnwindInfo*));

		mono_memory_barrier ();

		cached_info = new_table;

		mono_memory_barrier ();

		mono_thread_hazardous_free_or_queue (old_table, g_free);

		cached_info_size *= 2;
	}

	cached_info [cached_info_next ++] = info;

	unwind_info_size += sizeof (MonoUnwindInfo) + unwind_info_len;

	unwind_unlock ();
	return i;
}

static gpointer
get_hazardous_pointer (gpointer volatile *pp, MonoThreadHazardPointers *hp, int hazard_index)
{
	gpointer p;

	for (;;) {
		/* Get the pointer */
		p = *pp;
		/* If we don't have hazard pointers just return the
		   pointer. */
		if (!hp)
			return p;
		/* Make it hazardous */
		mono_hazard_pointer_set (hp, hazard_index, p);
		/* Check that it's still the same.  If not, try
		   again. */
		if (*pp != p) {
			mono_hazard_pointer_clear (hp, hazard_index);
			continue;
		}
		break;
	}

	return p;
}

/*
 * This function is signal safe.
 */
guint8*
mono_get_cached_unwind_info (guint32 index, guint32 *unwind_info_len)
{
	MonoUnwindInfo **table;
	MonoUnwindInfo *info;
	guint8 *data;
	MonoThreadHazardPointers *hp = mono_hazard_pointer_get ();

	table = get_hazardous_pointer ((gpointer volatile*)&cached_info, hp, 0);

	info = table [index];

	*unwind_info_len = info->len;
	data = info->info;

	mono_hazard_pointer_clear (hp, 0);

	return data;
}

/*
 * mono_unwind_get_dwarf_data_align:
 *
 *   Return the data alignment used by the encoded unwind information.
 */
int
mono_unwind_get_dwarf_data_align (void)
{
	return DWARF_DATA_ALIGN;
}

/*
 * mono_unwind_get_dwarf_pc_reg:
 *
 *   Return the dwarf register number of the register holding the ip of the
 * previous frame.
 */
int
mono_unwind_get_dwarf_pc_reg (void)
{
	return DWARF_PC_REG;
}

static void
decode_cie_op (guint8 *p, guint8 **endp)
{
	int op = *p & 0xc0;

	switch (op) {
	case DW_CFA_advance_loc:
		p ++;
		break;
	case DW_CFA_offset:
		p ++;
		decode_uleb128 (p, &p);
		break;
	case 0: {
		int ext_op = *p;
		p ++;
		switch (ext_op) {
		case DW_CFA_def_cfa:
			decode_uleb128 (p, &p);
			decode_uleb128 (p, &p);
			break;
		case DW_CFA_def_cfa_offset:
			decode_uleb128 (p, &p);
			break;
		case DW_CFA_def_cfa_register:
			decode_uleb128 (p, &p);
			break;
		case DW_CFA_advance_loc4:
			p += 4;
			break;
		default:
			g_assert_not_reached ();
		}
		break;
	}
	default:
		g_assert_not_reached ();
	}

	*endp = p;
}

/* Pointer Encoding in the .eh_frame */
enum {
    DW_EH_PE_absptr = 0x00,
    DW_EH_PE_omit = 0xff,

    DW_EH_PE_udata4 = 0x03,
	DW_EH_PE_sdata4 = 0x0b,
	DW_EH_PE_sdata8 = 0x0c,
	DW_EH_PE_pcrel = 0x10
};

/*
 * mono_unwind_decode_fde:
 *
 *   Decode a DWARF FDE entry, returning the unwind opcodes.
 * If not NULL, EX_INFO is set to a malloc-ed array of MonoJitExceptionInfo structures,
 * only try_start, try_end and handler_start is set.
 */
guint8*
mono_unwind_decode_fde (guint8 *fde, guint32 *out_len, guint32 *code_len, MonoJitExceptionInfo **ex_info, guint32 *ex_info_len)
{
	guint8 *p, *cie, *fde_current, *fde_aug, *code, *fde_cfi, *cie_cfi;
	gint32 fde_len, cie_offset, pc_begin, pc_range, aug_len, fde_data_len;
	gint32 cie_len, cie_id, cie_version, code_align, data_align, return_reg;
	gint32 i, cie_aug_len, buf_len;
	char *cie_aug_str;
	guint8 *buf;

	/* 
	 * http://refspecs.freestandards.org/LSB_3.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html
	 */

	/* Decode FDE */

	p = fde;
	// FIXME: Endianess ?
	fde_len = *(guint32*)p;
	g_assert (fde_len != 0xffffffff && fde_len != 0);
	p += 4;
	cie_offset = *(guint32*)p;
	cie = p - cie_offset;
	p += 4;
	fde_current = p;

	/* Decode CIE */
	p = cie;
	cie_len = *(guint32*)p;
	p += 4;
	cie_id = *(guint32*)p;
	g_assert (cie_id == 0);
	p += 4;
	cie_version = *p;
	g_assert (cie_version == 1);
	p += 1;
	cie_aug_str = (char*)p;
	p += strlen (cie_aug_str) + 1;
	code_align = decode_uleb128 (p, &p);
	data_align = decode_sleb128 (p, &p);
	return_reg = decode_uleb128 (p, &p);
	if (strstr (cie_aug_str, "z")) {
		cie_aug_len = decode_uleb128 (p, &p);

		g_assert (!strcmp (cie_aug_str, "zR") || !strcmp (cie_aug_str, "zPLR"));

		/* Check that the augmention is what we expect */
		if (!strcmp (cie_aug_str, "zPLR")) {
			guint8 *cie_aug = p;

			g_assert (cie_aug_len == sizeof (gpointer) + 3);
			/* P */
			/* FIXME: 32 bit */
			g_assert (cie_aug [0] == DW_EH_PE_sdata8);
			/* L */
			g_assert (cie_aug [1 + sizeof (gpointer)] == (DW_EH_PE_sdata4|DW_EH_PE_pcrel));
			/* R */
			g_assert (cie_aug [1 + sizeof (gpointer) + 1] == (DW_EH_PE_sdata4|DW_EH_PE_pcrel));
		}
		p += cie_aug_len;
	}
	cie_cfi = p;

	/* Continue decoding FDE */
	p = fde_current;
	/* DW_EH_PE_sdata4|DW_EH_PE_pcrel encoding */
	pc_begin = *(gint32*)p;
	code = p + pc_begin;
	p += 4;
	pc_range = *(guint32*)p;
	p += 4;
	aug_len = decode_uleb128 (p, &p);
	fde_aug = p;
	p += aug_len;
	fde_cfi = p;
	fde_data_len = fde + 4 + fde_len - p;

	if (code_len)
		*code_len = pc_range;

	if (ex_info) {
		*ex_info = NULL;
		*ex_info_len = 0;
	}

	/* Decode FDE augmention */
	if (aug_len) {
		gint32 lsda_offset, cio_offset, call_site_length;
		gint32 call_site_encoding;
		guint8 *lsda, *class_info, *action_table, *call_site, *p;
		int i, ncall_sites;

		/* 
		 * For some reason, this is 8 bytes long, even through it only includes
		 * the offset to the LSDA which is encoded as an sdata4.
		 */
		g_assert (aug_len == 8);
		/* sdata|pcrel encoding */
		lsda_offset = *(gint32*)fde_aug;
		if (lsda_offset != 0) {
			lsda = fde_aug + *(gint32*)fde_aug;

			/*
			 * LLVM generates a c++ style LSDA, which can be decoded by looking at
			 * eh_personality.cc in gcc.
			 */
			p = lsda;

			/* Read @LPStart */
			g_assert (*p == DW_EH_PE_omit);
			p ++;

			/* Read @TType */
			g_assert (*p == DW_EH_PE_absptr);
			p ++;
			cio_offset = decode_uleb128 (p, &p);
			class_info = lsda + cio_offset;

			/* Read call-site table */
			call_site_encoding = *p;
			g_assert (call_site_encoding == DW_EH_PE_udata4);
			p ++;
			call_site_length = decode_uleb128 (p, &p);
			call_site = p;
			p += call_site_length;
			action_table = p;

			/* Calculate the size of our table */
			ncall_sites = 0;
			p = call_site;
			while (p < action_table) {
				int block_start_offset, block_size, landing_pad, action_offset;

				block_start_offset = ((guint32*)p) [0];
				block_size = ((guint32*)p) [1];
				landing_pad = ((guint32*)p) [2];
				p += 3 * sizeof (guint32);
				action_offset = decode_uleb128 (p, &p);

				/* landing_pad == 0 means the region has no landing pad */
				if (landing_pad)
					ncall_sites ++;
			}

			if (ex_info) {
				*ex_info = g_malloc0 (ncall_sites * sizeof (MonoJitExceptionInfo));
				*ex_info_len = ncall_sites;
			}

			p = call_site;
			i = 0;
			while (p < action_table) {
				int block_start_offset, block_size, landing_pad, action_offset;

				block_start_offset = ((guint32*)p) [0];
				block_size = ((guint32*)p) [1];
				landing_pad = ((guint32*)p) [2];
				p += 3 * sizeof (guint32);
				action_offset = decode_uleb128 (p, &p);

				if (landing_pad) {
					//printf ("BLOCK: %p-%p %p, %d\n", code + block_start_offset, code + block_start_offset + block_size, code + landing_pad, action_offset);

					if (ex_info) {
						(*ex_info)[i].try_start = code + block_start_offset;
						(*ex_info)[i].try_end = code + block_start_offset + block_size;
						(*ex_info)[i].handler_start = code + landing_pad;
					}
					i ++;
				}
			}
		}
	}


	/* Make sure the FDE uses the same constants as we do */
	g_assert (code_align == 1);
	g_assert (data_align == DWARF_DATA_ALIGN);
	g_assert (return_reg == DWARF_PC_REG);

	buf_len = (cie + cie_len + 4 - cie_cfi) + (fde + fde_len + 4 - fde_cfi);
	buf = g_malloc0 (buf_len);

	i = 0;
	p = cie_cfi;
	while (p < cie + cie_len + 4) {
		if (*p == DW_CFA_nop)
			break;
		else
			decode_cie_op (p, &p);
	}
	memcpy (buf + i, cie_cfi, p - cie_cfi);
	i += p - cie_cfi;

	p = fde_cfi;
	while (p < fde + fde_len + 4) {
		if (*p == DW_CFA_nop)
			break;
		else
			decode_cie_op (p, &p);
	}
	memcpy (buf + i, fde_cfi, p - fde_cfi);
	i += p - fde_cfi;
	g_assert (i <= buf_len);

	*out_len = i;

	return g_realloc (buf, i);
}
