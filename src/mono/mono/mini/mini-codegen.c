/*
 * mini-codegen.c: Arch independent code generation functionality
 *
 * (C) 2003 Ximian, Inc.
 */

#include <string.h>
#include <math.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/profiler-private.h>
#include <mono/utils/mono-math.h>

#include "mini.h"
#include "trace.h"
#include "inssel.h"
#include "mini-arch.h"

#define DEBUG(a) MINI_DEBUG(cfg->verbose_level, 2, a;)

static inline GSList*
g_slist_append_mempool (MonoMemPool *mp, GSList *list, gpointer data)
{
	GSList *new_list;
	GSList *last;
	
	new_list = mono_mempool_alloc (mp, sizeof (GSList));
	new_list->data = data;
	new_list->next = NULL;
	
	if (list) {
		last = list;
		while (last->next)
			last = last->next;
		last->next = new_list;
		
		return list;
	} else
		return new_list;
}

static inline void
mono_regstate_assign (MonoRegState *rs)
{
	if (rs->next_vreg > rs->vassign_size) {
		g_free (rs->vassign);
		rs->vassign_size = MAX (rs->next_vreg, 256);
		rs->vassign = g_malloc (rs->vassign_size * sizeof (int));
	}

	memset (rs->isymbolic, 0, MONO_MAX_IREGS * sizeof (rs->isymbolic [0]));
	memset (rs->fsymbolic, 0, MONO_MAX_FREGS * sizeof (rs->fsymbolic [0]));
}

static inline int
mono_regstate_alloc_int (MonoRegState *rs, regmask_t allow)
{
	regmask_t mask = allow & rs->ifree_mask;

#if defined(__x86_64__) && defined(__GNUC__)
 {
	guint64 i;

	if (mask == 0)
		return -1;

	__asm__("bsfq %1,%0\n\t"
			: "=r" (i) : "rm" (mask));

	rs->ifree_mask &= ~ ((regmask_t)1 << i);
	return i;
 }
#else
	int i;

	for (i = 0; i < MONO_MAX_IREGS; ++i) {
		if (mask & ((regmask_t)1 << i)) {
			rs->ifree_mask &= ~ ((regmask_t)1 << i);
			return i;
		}
	}
	return -1;
#endif
}

static inline void
mono_regstate_free_int (MonoRegState *rs, int reg)
{
	if (reg >= 0) {
		rs->ifree_mask |= (regmask_t)1 << reg;
		rs->isymbolic [reg] = 0;
	}
}

static inline int
mono_regstate_alloc_float (MonoRegState *rs, regmask_t allow)
{
	int i;
	regmask_t mask = allow & rs->ffree_mask;
	for (i = 0; i < MONO_MAX_FREGS; ++i) {
		if (mask & ((regmask_t)1 << i)) {
			rs->ffree_mask &= ~ ((regmask_t)1 << i);
			return i;
		}
	}
	return -1;
}

static inline void
mono_regstate_free_float (MonoRegState *rs, int reg)
{
	if (reg >= 0) {
		rs->ffree_mask |= (regmask_t)1 << reg;
		rs->fsymbolic [reg] = 0;
	}
}

const char*
mono_regname_full (int reg, gboolean fp)
{
	if (fp)
		return mono_arch_fregname (reg);
	else
		return mono_arch_regname (reg);
}

void
mono_call_inst_add_outarg_reg (MonoCompile *cfg, MonoCallInst *call, int vreg, int hreg, gboolean fp)
{
	guint32 regpair;

	regpair = (((guint32)hreg) << 24) + vreg;
	if (fp) {
		g_assert (vreg >= MONO_MAX_FREGS);
		g_assert (hreg < MONO_MAX_FREGS);
		call->used_fregs |= 1 << hreg;
		call->out_freg_args = g_slist_append_mempool (cfg->mempool, call->out_freg_args, (gpointer)(gssize)(regpair));
	} else {
		g_assert (vreg >= MONO_MAX_IREGS);
		g_assert (hreg < MONO_MAX_IREGS);
		call->used_iregs |= 1 << hreg;
		call->out_ireg_args = g_slist_append_mempool (cfg->mempool, call->out_ireg_args, (gpointer)(gssize)(regpair));
	}
}

static void
resize_spill_info (MonoCompile *cfg, gboolean fp)
{
	MonoSpillInfo *orig_info = fp ? cfg->spill_info_float : cfg->spill_info;
	int orig_len = fp ? cfg->spill_info_float_len : cfg->spill_info_len;
	int new_len = orig_len ? orig_len * 2 : 16;
	MonoSpillInfo *new_info;
	int i;

	new_info = mono_mempool_alloc (cfg->mempool, sizeof (MonoSpillInfo) * new_len);
	if (orig_info)
		memcpy (new_info, orig_info, sizeof (MonoSpillInfo) * orig_len);
	for (i = orig_len; i < new_len; ++i)
		new_info [i].offset = -1;

	if (!fp) {
		cfg->spill_info = new_info;
		cfg->spill_info_len = new_len;
	} else {
		cfg->spill_info_float = new_info;
		cfg->spill_info_float_len = new_len;
	}
}

/*
 * returns the offset used by spillvar. It allocates a new
 * spill variable if necessary. 
 */
static inline int
mono_spillvar_offset (MonoCompile *cfg, int spillvar, gboolean fp)
{
	MonoSpillInfo *info;

#if defined (__mips__)
	g_assert_not_reached();
#endif
	if (G_UNLIKELY (spillvar >= (fp ? cfg->spill_info_float_len : cfg->spill_info_len))) {
		while (spillvar >= (fp ? cfg->spill_info_float_len : cfg->spill_info_len))
			resize_spill_info (cfg, fp);
	}

	/*
	 * Allocate separate spill slots for fp/non-fp variables since most processors prefer it.
	 */
	info = fp ? &cfg->spill_info_float [spillvar] : &cfg->spill_info [spillvar];
	if (info->offset == -1) {
		cfg->stack_offset += sizeof (gpointer) - 1;
		cfg->stack_offset &= ~(sizeof (gpointer) - 1);

		if (cfg->flags & MONO_CFG_HAS_SPILLUP) {
			if (fp) {
				cfg->stack_offset += 7;
				cfg->stack_offset &= ~7;
				info->offset = cfg->stack_offset;
				cfg->stack_offset += sizeof (double);
			} else {
				cfg->stack_offset += sizeof (gpointer) - 1;
				cfg->stack_offset &= ~(sizeof (gpointer) - 1);
				info->offset = cfg->stack_offset;
				cfg->stack_offset += sizeof (gpointer);
			}
		} else {
			if (fp) {
				cfg->stack_offset += sizeof (double) - 1;
				cfg->stack_offset &= ~(sizeof (double) - 1);
				cfg->stack_offset += sizeof (double);
				info->offset = - cfg->stack_offset;
			} else {
				cfg->stack_offset += sizeof (gpointer) - 1;
				cfg->stack_offset &= ~(sizeof (gpointer) - 1);
				cfg->stack_offset += sizeof (gpointer);
				info->offset = - cfg->stack_offset;
			}
		}
	}

	return info->offset;
}

#define regmask(reg) (((regmask_t)1) << (reg))

#define is_hard_ireg(r) ((r) >= 0 && (r) < MONO_MAX_IREGS)
#define is_hard_freg(r) ((r) >= 0 && (r) < MONO_MAX_FREGS)
#define is_global_ireg(r) (is_hard_ireg ((r)) && (MONO_ARCH_CALLEE_SAVED_REGS & (regmask (r))))
#define is_local_ireg(r) (is_hard_ireg ((r)) && (MONO_ARCH_CALLEE_REGS & (regmask (r))))
#define is_global_freg(r) (is_hard_freg ((r)) && (MONO_ARCH_CALLEE_SAVED_FREGS & (regmask (r))))
#define is_local_freg(r) (is_hard_ireg ((r)) && (MONO_ARCH_CALLEE_FREGS & (regmask (r))))
#define ireg_is_freeable(r) is_local_ireg ((r))
#define freg_is_freeable(r) is_hard_freg ((r))

#define reg_is_freeable(r,fp) ((fp) ? freg_is_freeable ((r)) : ireg_is_freeable ((r)))
#define is_hard_reg(r,fp) (G_UNLIKELY (fp) ? ((r) < MONO_MAX_FREGS) : ((r) < MONO_MAX_IREGS))
#define is_soft_reg(r,fp) (!is_hard_reg((r),(fp)))

#ifdef MONO_ARCH_INST_IS_FLOAT
#define reg_is_fp(desc) (MONO_ARCH_INST_IS_FLOAT (desc))
#define dreg_is_fp(spec)  (MONO_ARCH_INST_IS_FLOAT (spec [MONO_INST_DEST]))
#define sreg1_is_fp(spec) (MONO_ARCH_INST_IS_FLOAT (spec [MONO_INST_SRC1]))
#define sreg2_is_fp(spec) (MONO_ARCH_INST_IS_FLOAT (spec [MONO_INST_SRC2]))
#else
#define reg_is_fp(desc) ((desc) == 'f')
#define sreg1_is_fp(spec) (G_UNLIKELY (spec [MONO_INST_SRC1] == 'f'))
#define sreg2_is_fp(spec) (G_UNLIKELY (spec [MONO_INST_SRC2] == 'f'))
#define dreg_is_fp(spec)  (G_UNLIKELY (spec [MONO_INST_DEST] == 'f'))
#endif

#define sreg1_is_fp_ins(ins) (sreg1_is_fp (ins_get_spec ((ins)->opcode)))
#define sreg2_is_fp_ins(ins) (sreg2_is_fp (ins_get_spec ((ins)->opcode)))
#define dreg_is_fp_ins(ins)  (dreg_is_fp (ins_get_spec ((ins)->opcode)))

#define regpair_reg2_mask(desc,hreg1) ((MONO_ARCH_INST_REGPAIR_REG2 (desc,hreg1) != -1) ? (regmask (MONO_ARCH_INST_REGPAIR_REG2 (desc,hreg1))) : MONO_ARCH_CALLEE_REGS)

#ifdef MONO_ARCH_IS_GLOBAL_IREG
#undef is_global_ireg
#define is_global_ireg(reg) MONO_ARCH_IS_GLOBAL_IREG ((reg))
#endif

typedef struct {
	int born_in;
	int killed_in;
	/* Not (yet) used */
	//int last_use;
	//int prev_use;
	regmask_t preferred_mask; /* the hreg where the register should be allocated, or 0 */
} RegTrack;

#ifndef DISABLE_LOGGING
void
mono_print_ins_index (int i, MonoInst *ins)
{
	const char *spec = ins_get_spec (ins->opcode);
	if (i != -1)
		printf ("\t%-2d %s", i, mono_inst_name (ins->opcode));
	else
		printf (" %s", mono_inst_name (ins->opcode));
	if (spec == MONO_ARCH_CPU_SPEC) {
		/* This is a lowered opcode */
		if (ins->dreg != -1)
			printf (" R%d <-", ins->dreg);
		if (ins->sreg1 != -1)
			printf (" R%d", ins->sreg1);
		if (ins->sreg2 != -1)
			printf (" R%d", ins->sreg2);

		switch (ins->opcode) {
		case OP_LBNE_UN:
		case OP_LBEQ:
		case OP_LBLT:
		case OP_LBLT_UN:
		case OP_LBGT:
		case OP_LBGT_UN:
		case OP_LBGE:
		case OP_LBGE_UN:
		case OP_LBLE:
		case OP_LBLE_UN:
			if (!(ins->flags & MONO_INST_BRLABEL)) {
				if (!ins->inst_false_bb)
					printf (" [B%d]", ins->inst_true_bb->block_num);
				else
					printf (" [B%dB%d]", ins->inst_true_bb->block_num, ins->inst_false_bb->block_num);
			}
			break;
		case OP_PHI:
		case OP_FPHI: {
			int i;
			printf (" [%d (", (int)ins->inst_c0);
			for (i = 0; i < ins->inst_phi_args [0]; i++) {
				if (i)
					printf (", ");
				printf ("R%d", ins->inst_phi_args [i + 1]);
			}
			printf (")]");
			break;
		}
		case OP_LDADDR:
		case OP_OUTARG_VTRETADDR:
			printf (" R%d", ((MonoInst*)ins->inst_p0)->dreg);
			break;
		case OP_REGOFFSET:
			printf (" + 0x%lx", (long)ins->inst_offset);
		default:
			break;
		}

		printf ("\n");
		//g_error ("Unknown opcode: %s\n", mono_inst_name (ins->opcode));
		return;
	}

	if (spec [MONO_INST_DEST]) {
		gboolean fp = dreg_is_fp_ins (ins);
		if (is_soft_reg (ins->dreg, fp)) {
			if (spec [MONO_INST_DEST] == 'b') {
				if (ins->inst_offset == 0)
					printf (" [R%d] <-", ins->dreg);
				else
					printf (" [R%d + 0x%lx] <-", ins->dreg, (long)ins->inst_offset);
			}
			else
				printf (" R%d <-", ins->dreg);
		} else if (spec [MONO_INST_DEST] == 'b') {
			if (ins->inst_offset == 0)
				printf (" [%s] <-", mono_arch_regname (ins->dreg));
			else
				printf (" [%s + 0x%lx] <-", mono_arch_regname (ins->dreg), (long)ins->inst_offset);
		} else
			printf (" %s <-", mono_regname_full (ins->dreg, fp));
	}
	if (spec [MONO_INST_SRC1]) {
		gboolean fp = (spec [MONO_INST_SRC1] == 'f');
		if (is_soft_reg (ins->sreg1, fp)) {
			if (spec [MONO_INST_SRC1] == 'b')
				printf (" [R%d + 0x%lx]", ins->sreg1, (long)ins->inst_offset);
			else
				printf (" R%d", ins->sreg1);
		} else if (spec [MONO_INST_SRC1] == 'b')
			printf (" [%s + 0x%lx]", mono_arch_regname (ins->sreg1), (long)ins->inst_offset);
		else
			printf (" %s", mono_regname_full (ins->sreg1, fp));
	}
	if (spec [MONO_INST_SRC2]) {
		gboolean fp = (spec [MONO_INST_SRC2] == 'f');
		if (is_soft_reg (ins->sreg2, fp))
			printf (" R%d", ins->sreg2);
		else
			printf (" %s", mono_regname_full (ins->sreg2, fp));
	}

	switch (ins->opcode) {
	case OP_ICONST:
		printf (" [%d]", (int)ins->inst_c0);
		break;
#if defined(__i386__) || defined(__x86_64__)
	case OP_X86_PUSH_IMM:
#endif
	case OP_ICOMPARE_IMM:
	case OP_COMPARE_IMM:
	case OP_IADD_IMM:
	case OP_ISUB_IMM:
	case OP_IAND_IMM:
	case OP_IOR_IMM:
	case OP_IXOR_IMM:
		printf (" [%d]", (int)ins->inst_imm);
		break;
	case OP_ADD_IMM:
	case OP_LADD_IMM:
		printf (" [%d]", (int)(gssize)ins->inst_p1);
		break;
	case OP_I8CONST:
		printf (" [%lld]", (long long)ins->inst_l);
		break;
	case OP_R8CONST:
		printf (" [%f]", *(double*)ins->inst_p0);
		break;
	case OP_R4CONST:
		printf (" [%f]", *(float*)ins->inst_p0);
		break;
	case CEE_CALL:
	case CEE_CALLVIRT:
	case OP_CALL:
	case OP_CALL_MEMBASE:
	case OP_CALL_REG:
	case OP_FCALL:
	case OP_FCALLVIRT:
	case OP_LCALL:
	case OP_LCALLVIRT:
	case OP_VCALL:
	case OP_VCALLVIRT:
	case OP_VCALL_REG:
	case OP_VCALL_MEMBASE:
	case OP_VCALL2:
	case OP_VCALL2_REG:
	case OP_VCALL2_MEMBASE:
	case OP_VOIDCALL:
	case OP_VOIDCALLVIRT: {
		MonoCallInst *call = (MonoCallInst*)ins;
		GSList *list;

		if (ins->opcode == OP_VCALL || ins->opcode == OP_VCALL_REG || ins->opcode == OP_VCALL_MEMBASE) {
			/*
			 * These are lowered opcodes, but they are in the .md files since the old 
			 * JIT passes them to backends.
			 */
			if (ins->dreg != -1)
				printf (" R%d <-", ins->dreg);
		}

		if (call->method) {
			char *full_name = mono_method_full_name (call->method, TRUE);
			printf (" [%s]", full_name);
			g_free (full_name);
		} else if (call->fptr) {
			MonoJitICallInfo *info = mono_find_jit_icall_by_addr (call->fptr);
			if (info)
				printf (" [%s]", info->name);
		}

		list = call->out_ireg_args;
		while (list) {
			guint32 regpair;
			int reg, hreg;

			regpair = (guint32)(gssize)(list->data);
			hreg = regpair >> 24;
			reg = regpair & 0xffffff;

			printf (" [%s <- R%d]", mono_arch_regname (hreg), reg);

			list = g_slist_next (list);
		}
		break;
	}
	case OP_BR:
	case OP_CALL_HANDLER:
		printf (" [B%d]", ins->inst_target_bb->block_num);
		break;
	case CEE_BNE_UN:
	case CEE_BEQ:
	case CEE_BLT:
	case CEE_BLT_UN:
	case CEE_BGT:
	case CEE_BGT_UN:
	case CEE_BGE:
	case CEE_BGE_UN:
	case CEE_BLE:
	case CEE_BLE_UN:
	case OP_IBNE_UN:
	case OP_IBEQ:
	case OP_IBLT:
	case OP_IBLT_UN:
	case OP_IBGT:
	case OP_IBGT_UN:
	case OP_IBGE:
	case OP_IBGE_UN:
	case OP_IBLE:
	case OP_IBLE_UN:
	case OP_LBNE_UN:
	case OP_LBEQ:
	case OP_LBLT:
	case OP_LBLT_UN:
	case OP_LBGT:
	case OP_LBGT_UN:
	case OP_LBGE:
	case OP_LBGE_UN:
	case OP_LBLE:
	case OP_LBLE_UN:
		if (!(ins->flags & MONO_INST_BRLABEL)) {
			if (!ins->inst_false_bb)
				printf (" [B%d]", ins->inst_true_bb->block_num);
			else
				printf (" [B%dB%d]", ins->inst_true_bb->block_num, ins->inst_false_bb->block_num);
		}
		break;
	default:
		break;
	}

	if (spec [MONO_INST_CLOB])
		printf (" clobbers: %c", spec [MONO_INST_CLOB]);
	printf ("\n");
}

static void
print_regtrack (RegTrack *t, int num)
{
	int i;
	char buf [32];
	const char *r;
	
	for (i = 0; i < num; ++i) {
		if (!t [i].born_in)
			continue;
		if (i >= MONO_MAX_IREGS) {
			g_snprintf (buf, sizeof(buf), "R%d", i);
			r = buf;
		} else
			r = mono_arch_regname (i);
		printf ("liveness: %s [%d - %d]\n", r, t [i].born_in, t[i].killed_in);
	}
}
#else
void
mono_print_ins_index (int i, MonoInst *ins)
{
}
#endif /* DISABLE_LOGGING */

void
mono_print_ins (MonoInst *ins)
{
	mono_print_ins_index (-1, ins);
}

static inline void
insert_before_ins (MonoBasicBlock *bb, MonoInst *ins, MonoInst* to_insert)
{
	/*
	 * If this function is called multiple times, the new instructions are inserted
	 * in the proper order.
	 */
	mono_bblock_insert_before_ins (bb, ins, to_insert);
}

static inline void
insert_after_ins (MonoBasicBlock *bb, MonoInst *ins, MonoInst **last, MonoInst* to_insert)
{
	/*
	 * If this function is called multiple times, the new instructions are inserted in
	 * proper order.
	 */
	mono_bblock_insert_after_ins (bb, *last, to_insert);

	*last = to_insert;
}

/*
 * Force the spilling of the variable in the symbolic register 'reg'.
 */
static int
get_register_force_spilling (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, MonoInst *ins, int reg, gboolean fp)
{
	MonoInst *load;
	int i, sel, spill;
	int *symbolic;
	MonoRegState *rs = cfg->rs;

	if (fp)
		symbolic = rs->fsymbolic;
	else
		symbolic = rs->isymbolic;

	sel = rs->vassign [reg];

	/*i = rs->isymbolic [sel];
	g_assert (i == reg);*/
	i = reg;
	spill = ++cfg->spill_count;
	rs->vassign [i] = -spill - 1;
	if (fp)
		mono_regstate_free_float (rs, sel);
	else
		mono_regstate_free_int (rs, sel);
	/* we need to create a spill var and insert a load to sel after the current instruction */
	if (fp)
		MONO_INST_NEW (cfg, load, OP_LOADR8_MEMBASE);
	else
		MONO_INST_NEW (cfg, load, OP_LOAD_MEMBASE);
	load->dreg = sel;
	load->inst_basereg = cfg->frame_reg;
	load->inst_offset = mono_spillvar_offset (cfg, spill, fp);
	insert_after_ins (bb, ins, last, load);
	DEBUG (printf ("SPILLED LOAD (%d at 0x%08lx(%%ebp)) R%d (freed %s)\n", spill, (long)load->inst_offset, i, mono_regname_full (sel, fp)));
	if (fp)
		i = mono_regstate_alloc_float (rs, regmask (sel));
	else
		i = mono_regstate_alloc_int (rs, regmask (sel));
	g_assert (i == sel);

	return sel;
}

/* This isn't defined on older glib versions and on some platforms */
#ifndef G_GUINT64_FORMAT
#define G_GUINT64_FORMAT "ul"
#endif

static int
get_register_spilling (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, MonoInst *ins, regmask_t regmask, int reg, gboolean fp)
{
	MonoInst *load;
	int i, sel, spill;
	int *symbolic;
	MonoRegState *rs = cfg->rs;

	if (fp)
		symbolic = rs->fsymbolic;
	else
		symbolic = rs->isymbolic;

	DEBUG (printf ("\tstart regmask to assign R%d: 0x%08" G_GUINT64_FORMAT " (R%d <- R%d R%d)\n", reg, (guint64)regmask, ins->dreg, ins->sreg1, ins->sreg2));
	/* exclude the registers in the current instruction */
	if ((sreg1_is_fp_ins (ins) == fp) && (reg != ins->sreg1) && (reg_is_freeable (ins->sreg1, fp) || (is_soft_reg (ins->sreg1, fp) && rs->vassign [ins->sreg1] >= 0))) {
		if (is_soft_reg (ins->sreg1, fp))
			regmask &= ~ (regmask (rs->vassign [ins->sreg1]));
		else
			regmask &= ~ (regmask (ins->sreg1));
		DEBUG (printf ("\t\texcluding sreg1 %s\n", mono_regname_full (ins->sreg1, fp)));
	}
	if ((sreg2_is_fp_ins (ins) == fp) && (reg != ins->sreg2) && (reg_is_freeable (ins->sreg2, fp) || (is_soft_reg (ins->sreg2, fp) && rs->vassign [ins->sreg2] >= 0))) {
		if (is_soft_reg (ins->sreg2, fp))
			regmask &= ~ (regmask (rs->vassign [ins->sreg2]));
		else
			regmask &= ~ (regmask (ins->sreg2));
		DEBUG (printf ("\t\texcluding sreg2 %s %d\n", mono_regname_full (ins->sreg2, fp), ins->sreg2));
	}
	if ((dreg_is_fp_ins (ins) == fp) && (reg != ins->dreg) && reg_is_freeable (ins->dreg, fp)) {
		regmask &= ~ (regmask (ins->dreg));
		DEBUG (printf ("\t\texcluding dreg %s\n", mono_regname_full (ins->dreg, fp)));
	}

	DEBUG (printf ("\t\tavailable regmask: 0x%08" G_GUINT64_FORMAT "\n", (guint64)regmask));
	g_assert (regmask); /* need at least a register we can free */
	sel = 0;
	/* we should track prev_use and spill the register that's farther */
	if (fp) {
		for (i = 0; i < MONO_MAX_FREGS; ++i) {
			if (regmask & (regmask (i))) {
				sel = i;
				DEBUG (printf ("\t\tselected register %s has assignment %d\n", mono_arch_fregname (sel), rs->fsymbolic [sel]));
				break;
			}
		}

		i = rs->fsymbolic [sel];
		spill = ++cfg->spill_count;
		rs->vassign [i] = -spill - 1;
		mono_regstate_free_float (rs, sel);
	}
	else {
		for (i = 0; i < MONO_MAX_IREGS; ++i) {
			if (regmask & (regmask (i))) {
				sel = i;
				DEBUG (printf ("\t\tselected register %s has assignment %d\n", mono_arch_regname (sel), rs->isymbolic [sel]));
				break;
			}
		}

		i = rs->isymbolic [sel];
		spill = ++cfg->spill_count;
		rs->vassign [i] = -spill - 1;
		mono_regstate_free_int (rs, sel);
	}

	/* we need to create a spill var and insert a load to sel after the current instruction */
	MONO_INST_NEW (cfg, load, fp ? OP_LOADR8_MEMBASE : OP_LOAD_MEMBASE);
	load->dreg = sel;
	load->inst_basereg = cfg->frame_reg;
	load->inst_offset = mono_spillvar_offset (cfg, spill, fp);
	insert_after_ins (bb, ins, last, load);
	DEBUG (printf ("\tSPILLED LOAD (%d at 0x%08lx(%%ebp)) R%d (freed %s)\n", spill, (long)load->inst_offset, i, mono_regname_full (sel, fp)));
	if (fp)
		i = mono_regstate_alloc_float (rs, regmask (sel));
	else
		i = mono_regstate_alloc_int (rs, regmask (sel));
	g_assert (i == sel);
	
	return sel;
}

static void
free_up_ireg (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, MonoInst *ins, int hreg)
{
	if (!(cfg->rs->ifree_mask & (regmask (hreg)))) {
		DEBUG (printf ("\tforced spill of R%d\n", cfg->rs->isymbolic [hreg]));
		get_register_force_spilling (cfg, bb, last, ins, cfg->rs->isymbolic [hreg], FALSE);
		mono_regstate_free_int (cfg->rs, hreg);
	}
}

static void
free_up_reg (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, MonoInst *ins, int hreg, gboolean fp)
{
	if (fp) {
		if (!(cfg->rs->ffree_mask & (regmask (hreg)))) {
			DEBUG (printf ("\tforced spill of R%d\n", cfg->rs->isymbolic [hreg]));
			get_register_force_spilling (cfg, bb, last, ins, cfg->rs->isymbolic [hreg], fp);
			mono_regstate_free_float (cfg->rs, hreg);
		}
	}
	else {
		if (!(cfg->rs->ifree_mask & (regmask (hreg)))) {
			DEBUG (printf ("\tforced spill of R%d\n", cfg->rs->isymbolic [hreg]));
			get_register_force_spilling (cfg, bb, last, ins, cfg->rs->isymbolic [hreg], fp);
			mono_regstate_free_int (cfg->rs, hreg);
		}
	}
}

static MonoInst*
create_copy_ins (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, int dest, int src, MonoInst *ins, const unsigned char *ip, gboolean fp)
{
	MonoInst *copy;

	if (fp)
		MONO_INST_NEW (cfg, copy, OP_FMOVE);
	else
		MONO_INST_NEW (cfg, copy, OP_MOVE);

	copy->dreg = dest;
	copy->sreg1 = src;
	copy->cil_code = ip;
	if (ins) {
		mono_bblock_insert_after_ins (bb, ins, copy);
		*last = copy;
	}
	DEBUG (printf ("\tforced copy from %s to %s\n", mono_regname_full (src, fp), mono_regname_full (dest, fp)));
	return copy;
}

static MonoInst*
create_spilled_store (MonoCompile *cfg, MonoBasicBlock *bb, int spill, int reg, int prev_reg, MonoInst **last, MonoInst *ins, gboolean fp)
{
	MonoInst *store;
	MONO_INST_NEW (cfg, store, fp ? OP_STORER8_MEMBASE_REG : OP_STORE_MEMBASE_REG);
	store->sreg1 = reg;
	store->inst_destbasereg = cfg->frame_reg;
	store->inst_offset = mono_spillvar_offset (cfg, spill, fp);
	if (ins) {
		mono_bblock_insert_after_ins (bb, ins, store);
		*last = store;
	}
	DEBUG (printf ("\tSPILLED STORE (%d at 0x%08lx(%%ebp)) R%d (from %s)\n", spill, (long)store->inst_offset, prev_reg, mono_regname_full (reg, fp)));
	return store;
}

/* flags used in reginfo->flags */
enum {
	MONO_FP_NEEDS_LOAD_SPILL	= regmask (0),
	MONO_FP_NEEDS_SPILL			= regmask (1),
	MONO_FP_NEEDS_LOAD			= regmask (2)
};

static inline int
alloc_int_reg (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, MonoInst *ins, regmask_t dest_mask, int sym_reg, RegTrack *info)
{
	int val;

	if (info && info->preferred_mask) {
		val = mono_regstate_alloc_int (cfg->rs, info->preferred_mask & dest_mask);
		if (val >= 0) {
			DEBUG (printf ("\tallocated preferred reg R%d to %s\n", sym_reg, mono_arch_regname (val)));
			return val;
		}
	}

	val = mono_regstate_alloc_int (cfg->rs, dest_mask);
	if (val < 0)
		val = get_register_spilling (cfg, bb, last, ins, dest_mask, sym_reg, FALSE);

	return val;
}

static inline int
alloc_float_reg (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, MonoInst *ins, regmask_t dest_mask, int sym_reg)
{
	int val;

	val = mono_regstate_alloc_float (cfg->rs, dest_mask);

	if (val < 0) {
		val = get_register_spilling (cfg, bb, last, ins, dest_mask, sym_reg, TRUE);
	}

	return val;
}

static inline int
alloc_reg (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst **last, MonoInst *ins, regmask_t dest_mask, int sym_reg, RegTrack *info, gboolean fp)
{
	if (G_UNLIKELY (fp))
		return alloc_float_reg (cfg, bb, last, ins, dest_mask, sym_reg);
	else
		return alloc_int_reg (cfg, bb, last, ins, dest_mask, sym_reg, info);
}

static inline void
assign_reg (MonoCompile *cfg, MonoRegState *rs, int reg, int hreg, gboolean fp)
{
	if (G_UNLIKELY (fp)) {
		g_assert (reg >= MONO_MAX_FREGS);
		g_assert (hreg < MONO_MAX_FREGS);
		g_assert (! is_global_freg (hreg));

		rs->vassign [reg] = hreg;
		rs->fsymbolic [hreg] = reg;
		rs->ffree_mask &= ~ (regmask (hreg));
	}
	else {
		g_assert (reg >= MONO_MAX_IREGS);
		g_assert (hreg < MONO_MAX_IREGS);
#ifndef __arm__
		/* this seems to trigger a gcc compilation bug sometime (hreg is 0) */
		g_assert (! is_global_ireg (hreg));
#endif

		rs->vassign [reg] = hreg;
		rs->isymbolic [hreg] = reg;
		rs->ifree_mask &= ~ (regmask (hreg));
	}
}

static gint8 desc_to_fixed_reg [256];
static gboolean desc_to_fixed_reg_inited = FALSE;

/*
 * Local register allocation.
 * We first scan the list of instructions and we save the liveness info of
 * each register (when the register is first used, when it's value is set etc.).
 * We also reverse the list of instructions because assigning registers backwards allows 
 * for more tricks to be used.
 */
void
mono_local_regalloc (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *prev, *last;
	MonoInst **tmp;
	MonoRegState *rs = cfg->rs;
	int i, val, max;
	RegTrack *reginfo;
	const char *spec;
	unsigned char spec_src1, spec_src2, spec_dest;
	gboolean fp;
#if MONO_ARCH_USE_FPSTACK
	gboolean has_fp = FALSE;
	int fpstack [8];
	int sp = 0;
#endif

	if (!bb->code)
		return;

	if (!desc_to_fixed_reg_inited) {
		for (i = 0; i < 256; ++i)
			desc_to_fixed_reg [i] = MONO_ARCH_INST_FIXED_REG (i);
		desc_to_fixed_reg_inited = TRUE;
	}

	rs->next_vreg = bb->max_vreg;
	mono_regstate_assign (rs);

	rs->ifree_mask = MONO_ARCH_CALLEE_REGS;
	rs->ffree_mask = MONO_ARCH_CALLEE_FREGS;

	max = rs->next_vreg;

	if (cfg->reginfo && cfg->reginfo_len < max)
		cfg->reginfo = NULL;

	reginfo = cfg->reginfo;
	if (!reginfo) {
		cfg->reginfo_len = MAX (1024, max * 2);
		reginfo = cfg->reginfo = mono_mempool_alloc (cfg->mempool, sizeof (RegTrack) * cfg->reginfo_len);
	} 
	else
		g_assert (cfg->reginfo_len >= rs->next_vreg);

	if (cfg->verbose_level > 1) {
		/* print_regtrack reads the info of all variables */
		memset (cfg->reginfo, 0, cfg->reginfo_len * sizeof (RegTrack));
	}

	if (cfg->new_ir) {
		/* 
		 * For large methods, next_vreg can be very large, so g_malloc0 time can
		 * be prohibitive. So we manually init the reginfo entries used by the 
		 * bblock.
		 */
		for (ins = bb->code; ins; ins = ins->next) {
			spec = ins_get_spec (ins->opcode);

			if ((ins->dreg != -1) && (ins->dreg < max)) {
				memset (&reginfo [ins->dreg], 0, sizeof (RegTrack));
#if SIZEOF_VOID_P == 4
				if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_DEST])) {
					/**
					 * In the new IR, the two vregs of the regpair do not alias the
					 * original long vreg. shift the vreg here so the rest of the 
					 * allocator doesn't have to care about it.
					 */
					if (cfg->new_ir)
						ins->dreg ++;
					memset (&reginfo [ins->dreg + 1], 0, sizeof (RegTrack));
				}
#endif
			}
			if ((ins->sreg1 != -1) && (ins->sreg1 < max)) {
				memset (&reginfo [ins->sreg1], 0, sizeof (RegTrack));
#if SIZEOF_VOID_P == 4
				if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_SRC1])) {
					if (cfg->new_ir)
						ins->sreg1 ++;
					memset (&reginfo [ins->sreg1 + 1], 0, sizeof (RegTrack));
				}
#endif
			}
			if ((ins->sreg2 != -1) && (ins->sreg2 < max)) {
				memset (&reginfo [ins->sreg2], 0, sizeof (RegTrack));
#if SIZEOF_VOID_P == 4
				if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_SRC2])) {
					if (cfg->new_ir)
						ins->sreg2 ++;
					memset (&reginfo [ins->sreg2 + 1], 0, sizeof (RegTrack));
				}
#endif
			}
		}
	}
	else {
		memset (reginfo, 0, max * sizeof (RegTrack));
	}

	/*if (cfg->opt & MONO_OPT_COPYPROP)
		local_copy_prop (cfg, ins);*/

	i = 1;
	DEBUG (printf ("\nLOCAL REGALLOC: BASIC BLOCK %d:\n", bb->block_num));
	/* forward pass on the instructions to collect register liveness info */
	MONO_BB_FOR_EACH_INS (bb, ins) {
		spec = ins_get_spec (ins->opcode);
		spec_src1 = spec [MONO_INST_SRC1];
		spec_src2 = spec [MONO_INST_SRC2];
		spec_dest = spec [MONO_INST_DEST];

		if (G_UNLIKELY (spec == MONO_ARCH_CPU_SPEC)) {
			g_error ("Opcode '%s' missing from machine description file.", mono_inst_name (ins->opcode));
		}
		
		DEBUG (mono_print_ins_index (i, ins));

#if MONO_ARCH_USE_FPSTACK
		if (sreg1_is_fp (spec) || sreg2_is_fp (spec) || dreg_is_fp (spec))
			has_fp = TRUE;
#endif

		if (spec_src1) {
			fp = sreg1_is_fp (spec);
			g_assert (ins->sreg1 != -1);
			if (cfg->new_ir && is_soft_reg (ins->sreg1, fp))
				/* This means the vreg is not local to this bb */
				g_assert (reginfo [ins->sreg1].born_in > 0);
			rs->vassign [ins->sreg1] = -1;
			//reginfo [ins->sreg1].prev_use = reginfo [ins->sreg1].last_use;
			//reginfo [ins->sreg1].last_use = i;
			if (MONO_ARCH_INST_IS_REGPAIR (spec_src2)) {
				/* The virtual register is allocated sequentially */
				rs->vassign [ins->sreg1 + 1] = -1;
				//reginfo [ins->sreg1 + 1].prev_use = reginfo [ins->sreg1 + 1].last_use;
				//reginfo [ins->sreg1 + 1].last_use = i;
				if (reginfo [ins->sreg1 + 1].born_in == 0 || reginfo [ins->sreg1 + 1].born_in > i)
					reginfo [ins->sreg1 + 1].born_in = i;
			}
		} else {
			ins->sreg1 = -1;
		}
		if (spec_src2) {
			fp = sreg2_is_fp (spec);
			g_assert (ins->sreg2 != -1);
			if (cfg->new_ir && is_soft_reg (ins->sreg2, fp))
				/* This means the vreg is not local to this bb */
				g_assert (reginfo [ins->sreg2].born_in > 0);
			rs->vassign [ins->sreg2] = -1;
			//reginfo [ins->sreg2].prev_use = reginfo [ins->sreg2].last_use;
			//reginfo [ins->sreg2].last_use = i;
			if (MONO_ARCH_INST_IS_REGPAIR (spec_src2)) {
				/* The virtual register is allocated sequentially */
				rs->vassign [ins->sreg2 + 1] = -1;
				//reginfo [ins->sreg2 + 1].prev_use = reginfo [ins->sreg2 + 1].last_use;
				//reginfo [ins->sreg2 + 1].last_use = i;
				if (reginfo [ins->sreg2 + 1].born_in == 0 || reginfo [ins->sreg2 + 1].born_in > i)
					reginfo [ins->sreg2 + 1].born_in = i;
			}
		} else {
			ins->sreg2 = -1;
		}
		if (spec_dest) {
			int dest_dreg;

			fp = dreg_is_fp (spec);
			if (spec_dest != 'b') /* it's not just a base register */
				reginfo [ins->dreg].killed_in = i;
			g_assert (ins->dreg != -1);
			rs->vassign [ins->dreg] = -1;
			//reginfo [ins->dreg].prev_use = reginfo [ins->dreg].last_use;
			//reginfo [ins->dreg].last_use = i;
			if (reginfo [ins->dreg].born_in == 0 || reginfo [ins->dreg].born_in > i)
				reginfo [ins->dreg].born_in = i;

			dest_dreg = desc_to_fixed_reg [spec_dest];
			if (dest_dreg != -1)
				reginfo [ins->dreg].preferred_mask = (regmask (dest_dreg));

#ifdef MONO_ARCH_INST_FIXED_MASK
			reginfo [ins->dreg].preferred_mask |= MONO_ARCH_INST_FIXED_MASK (spec_dest);
#endif

			if (MONO_ARCH_INST_IS_REGPAIR (spec_dest)) {
				/* The virtual register is allocated sequentially */
				rs->vassign [ins->dreg + 1] = -1;
				//reginfo [ins->dreg + 1].prev_use = reginfo [ins->dreg + 1].last_use;
				//reginfo [ins->dreg + 1].last_use = i;
				if (reginfo [ins->dreg + 1].born_in == 0 || reginfo [ins->dreg + 1].born_in > i)
					reginfo [ins->dreg + 1].born_in = i;
				if (MONO_ARCH_INST_REGPAIR_REG2 (spec_dest, -1) != -1)
					reginfo [ins->dreg + 1].preferred_mask = regpair_reg2_mask (spec_dest, -1);
			}
		} else {
			ins->dreg = -1;
		}

		if (spec [MONO_INST_CLOB] == 'c') {
			/* A call instruction implicitly uses all registers in call->out_ireg_args */

			MonoCallInst *call = (MonoCallInst*)ins;
			GSList *list;

			list = call->out_ireg_args;
			if (list) {
				while (list) {
					guint32 regpair;
					int reg, hreg;

					regpair = (guint32)(gssize)(list->data);
					hreg = regpair >> 24;
					reg = regpair & 0xffffff;

					//reginfo [reg].prev_use = reginfo [reg].last_use;
					//reginfo [reg].last_use = i;

					list = g_slist_next (list);
				}
			}

			list = call->out_freg_args;
			if (list) {
				while (list) {
					guint32 regpair;
					int reg, hreg;

					regpair = (guint32)(gssize)(list->data);
					hreg = regpair >> 24;
					reg = regpair & 0xffffff;

					list = g_slist_next (list);
				}
			}
		}

		++i;
	}

	tmp = &last;

	DEBUG (print_regtrack (reginfo, rs->next_vreg));
	MONO_BB_FOR_EACH_INS_REVERSE_SAFE (bb, prev, ins) {
		int prev_dreg, prev_sreg1, prev_sreg2, clob_dreg;
		int dest_dreg, dest_sreg1, dest_sreg2, clob_reg;
		int dreg_high, sreg1_high;
		regmask_t dreg_mask, sreg1_mask, sreg2_mask, mask;
		regmask_t dreg_fixed_mask, sreg1_fixed_mask, sreg2_fixed_mask;
		const unsigned char *ip;
		--i;
		spec = ins_get_spec (ins->opcode);
		spec_src1 = spec [MONO_INST_SRC1];
		spec_src2 = spec [MONO_INST_SRC2];
		spec_dest = spec [MONO_INST_DEST];
		prev_dreg = -1;
		prev_sreg2 = -1;
		clob_dreg = -1;
		clob_reg = -1;
		dest_dreg = -1;
		dest_sreg1 = -1;
		dest_sreg2 = -1;
		prev_sreg1 = -1;
		dreg_high = -1;
		sreg1_high = -1;
		dreg_mask = dreg_is_fp (spec) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;
		sreg1_mask = sreg1_is_fp (spec) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;
		sreg2_mask = sreg2_is_fp (spec) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;

		DEBUG (printf ("processing:"));
		DEBUG (mono_print_ins_index (i, ins));

		ip = ins->cil_code;

		last = ins;

		/*
		 * FIXED REGS
		 */
		dest_sreg1 = desc_to_fixed_reg [spec_src1];
		dest_sreg2 = desc_to_fixed_reg [spec_src2];
		dest_dreg = desc_to_fixed_reg [spec_dest];
		clob_reg = desc_to_fixed_reg [(int)spec [MONO_INST_CLOB]];
		sreg2_mask &= ~ (MONO_ARCH_INST_SREG2_MASK (spec));

#ifdef MONO_ARCH_INST_FIXED_MASK
		sreg1_fixed_mask = MONO_ARCH_INST_FIXED_MASK (spec_src1);
		sreg2_fixed_mask = MONO_ARCH_INST_FIXED_MASK (spec_src2);
		dreg_fixed_mask = MONO_ARCH_INST_FIXED_MASK (spec_dest);
#else
		sreg1_fixed_mask = sreg2_fixed_mask = dreg_fixed_mask = 0;
#endif

		/*
		 * TRACK FIXED SREG2
		 */
		if (dest_sreg2 != -1) {
			if (rs->ifree_mask & (regmask (dest_sreg2))) {
				if (is_global_ireg (ins->sreg2)) {
					/* Argument already in hard reg, need to copy */
					MonoInst *copy = create_copy_ins (cfg, bb, tmp, dest_sreg2, ins->sreg2, NULL, ip, FALSE);
					insert_before_ins (bb, ins, copy);
				}
				else {
					val = rs->vassign [ins->sreg2];
					if (val == -1) {
						DEBUG (printf ("\tshortcut assignment of R%d to %s\n", ins->sreg2, mono_arch_regname (dest_sreg2)));
						assign_reg (cfg, rs, ins->sreg2, dest_sreg2, FALSE);
					} else if (val < -1) {
						/* FIXME: */
						g_assert_not_reached ();
					} else {
						/* Argument already in hard reg, need to copy */
						MonoInst *copy = create_copy_ins (cfg, bb, tmp, dest_sreg2, val, NULL, ip, FALSE);
						insert_before_ins (bb, ins, copy);
					}
				}
			} else {
				gboolean need_spill = TRUE;
				gboolean need_assign = TRUE;

				dreg_mask &= ~ (regmask (dest_sreg2));
				sreg1_mask &= ~ (regmask (dest_sreg2));

				/* 
				 * First check if dreg is assigned to dest_sreg2, since we
				 * can't spill a dreg.
				 */
				val = rs->vassign [ins->dreg];
				if (val == dest_sreg2 && ins->dreg != ins->sreg2) {
					/* 
					 * the destination register is already assigned to 
					 * dest_sreg2: we need to allocate another register for it 
					 * and then copy from this to dest_sreg2.
					 */
					int new_dest;
					new_dest = alloc_int_reg (cfg, bb, tmp, ins, dreg_mask, ins->dreg, &reginfo [ins->dreg]);
					g_assert (new_dest >= 0);
					DEBUG (printf ("\tchanging dreg R%d to %s from %s\n", ins->dreg, mono_arch_regname (new_dest), mono_arch_regname (dest_sreg2)));

					prev_dreg = ins->dreg;
					assign_reg (cfg, rs, ins->dreg, new_dest, FALSE);
					clob_dreg = ins->dreg;
					create_copy_ins (cfg, bb, tmp, dest_sreg2, new_dest, ins, ip, FALSE);
					mono_regstate_free_int (rs, dest_sreg2);
					need_spill = FALSE;
				}

				if (is_global_ireg (ins->sreg2)) {
					MonoInst *copy = create_copy_ins (cfg, bb, tmp, dest_sreg2, ins->sreg2, NULL, ip, FALSE);
					insert_before_ins (bb, ins, copy);
					need_assign = FALSE;
				}
				else {
					val = rs->vassign [ins->sreg2];
					if (val == dest_sreg2) {
						/* sreg2 is already assigned to the correct register */
						need_spill = FALSE;
					} else if (val < -1) {
						/* sreg2 is spilled, it can be assigned to dest_sreg2 */
					} else if (val >= 0) {
						/* sreg2 already assigned to another register */
						/*
						 * We couldn't emit a copy from val to dest_sreg2, because
						 * val might be spilled later while processing this 
						 * instruction. So we spill sreg2 so it can be allocated to
						 * dest_sreg2.
						 */
						DEBUG (printf ("\tforced spill of R%d\n", ins->sreg2));
						free_up_reg (cfg, bb, tmp, ins, val, FALSE);
					}
				}

				if (need_spill) {
					DEBUG (printf ("\tforced spill of R%d\n", rs->isymbolic [dest_sreg2]));
					free_up_reg (cfg, bb, tmp, ins, dest_sreg2, FALSE);
				}

				if (need_assign) {
					if (rs->vassign [ins->sreg2] < -1) {
						MonoInst *store;
						int spill;

						/* Need to emit a spill store */
						spill = - rs->vassign [ins->sreg2] - 1;
						store = create_spilled_store (cfg, bb, spill, dest_sreg2, ins->sreg2, tmp, NULL, fp);						
						insert_before_ins (bb, ins, store);
					}
					/* force-set sreg2 */
					assign_reg (cfg, rs, ins->sreg2, dest_sreg2, FALSE);
				}
			}
			ins->sreg2 = dest_sreg2;
		}

		/*
		 * TRACK DREG
		 */
		fp = dreg_is_fp (spec);
		if (spec_dest && is_soft_reg (ins->dreg, fp)) {
			prev_dreg = ins->dreg;
		}

		if (spec_dest == 'b') {
			/* 
			 * The dest reg is read by the instruction, not written, so
			 * avoid allocating sreg1/sreg2 to the same reg.
			 */
			if (!dest_sreg1 != -1)
				dreg_mask &= ~ (regmask (dest_sreg1));
			if (dest_sreg2 != -1)
				dreg_mask &= ~ (regmask (dest_sreg2));

			val = rs->vassign [ins->dreg];
			if (is_soft_reg (ins->dreg, fp) && (val >= 0) && (!(regmask (val) & dreg_mask))) {
				/* DREG is already allocated to a register needed for sreg1 */
				get_register_force_spilling (cfg, bb, tmp, ins, ins->dreg, FALSE);
				mono_regstate_free_int (rs, val);
			}
		}

		/*
		 * If dreg is a fixed regpair, free up both of the needed hregs to avoid
		 * various complex situations.
		 */
		if (MONO_ARCH_INST_IS_REGPAIR (spec_dest)) {
			guint32 dreg2, dest_dreg2;

			g_assert (is_soft_reg (ins->dreg, fp));

			if (dest_dreg != -1) {
				if (rs->vassign [ins->dreg] != dest_dreg)
					free_up_ireg (cfg, bb, tmp, ins, dest_dreg);

				dreg2 = ins->dreg + 1;
				dest_dreg2 = MONO_ARCH_INST_REGPAIR_REG2 (spec_dest, dest_dreg);
				if (dest_dreg2 != -1) {
					if (rs->vassign [dreg2] != dest_dreg2)
						free_up_ireg (cfg, bb, tmp, ins, dest_dreg2);
				}
			}
		}

		if (dreg_fixed_mask) {
			g_assert (!fp);
			if (is_global_ireg (ins->dreg)) {
				/* 
				 * The argument is already in a hard reg, but that reg is
				 * not usable by this instruction, so allocate a new one.
				 */
				val = mono_regstate_alloc_int (rs, dreg_fixed_mask);
				if (val < 0)
					val = get_register_spilling (cfg, bb, tmp, ins, dreg_fixed_mask, -1, fp);
				mono_regstate_free_int (rs, val);
				dest_dreg = val;

				/* Fall through */
			}
			else
				dreg_mask &= dreg_fixed_mask;
		}

		if (is_soft_reg (ins->dreg, fp)) {
			val = rs->vassign [ins->dreg];

			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = alloc_reg (cfg, bb, tmp, ins, dreg_mask, ins->dreg, &reginfo [ins->dreg], fp);
				assign_reg (cfg, rs, ins->dreg, val, fp);
				if (spill)
					create_spilled_store (cfg, bb, spill, val, prev_dreg, tmp, ins, fp);
			}

			DEBUG (printf ("\tassigned dreg %s to dest R%d\n", mono_regname_full (val, fp), ins->dreg));
			ins->dreg = val;
		}

		/* Handle regpairs */
		if (MONO_ARCH_INST_IS_REGPAIR (spec_dest)) {
			int reg2 = prev_dreg + 1;

			g_assert (!fp);
			g_assert (prev_dreg > -1);
			g_assert (!is_global_ireg (rs->vassign [prev_dreg]));
			mask = regpair_reg2_mask (spec_dest, rs->vassign [prev_dreg]);
#ifdef __i386__
			/* bug #80489 */
			mask &= ~regmask (X86_ECX);
#endif
			val = rs->vassign [reg2];
			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = mono_regstate_alloc_int (rs, mask);
				if (val < 0)
					val = get_register_spilling (cfg, bb, tmp, ins, mask, reg2, fp);
				if (spill)
					create_spilled_store (cfg, bb, spill, val, reg2, tmp, ins, fp);
			}
			else {
				if (! (mask & (regmask (val)))) {
					val = mono_regstate_alloc_int (rs, mask);
					if (val < 0)
						val = get_register_spilling (cfg, bb, tmp, ins, mask, reg2, fp);

					/* Reallocate hreg to the correct register */
					create_copy_ins (cfg, bb, tmp, rs->vassign [reg2], val, ins, ip, fp);

					mono_regstate_free_int (rs, rs->vassign [reg2]);
				}
			}					

			DEBUG (printf ("\tassigned dreg-high %s to dest R%d\n", mono_arch_regname (val), reg2));
			assign_reg (cfg, rs, reg2, val, fp);

			dreg_high = val;
			ins->backend.reg3 = val;

			if (reg_is_freeable (val, fp) && reg2 >= 0 && (reginfo [reg2].born_in >= i)) {
				DEBUG (printf ("\tfreeable %s (R%d)\n", mono_arch_regname (val), reg2));
				mono_regstate_free_int (rs, val);
			}
		}

		if (prev_dreg >= 0 && is_soft_reg (prev_dreg, fp) && (spec_dest != 'b') && (cfg->new_ir || reginfo [prev_dreg].born_in >= i)) {
			/* 
			 * In theory, we could free up the hreg even if the vreg is alive,
			 * but branches inside bblocks force us to assign the same hreg
			 * to a vreg every time it is encountered.
			 */
			int dreg = rs->vassign [prev_dreg];
			g_assert (dreg >= 0);
			DEBUG (printf ("\tfreeable %s (R%d) (born in %d)\n", mono_regname_full (dreg, fp), prev_dreg, reginfo [prev_dreg].born_in));
			if (fp)
				mono_regstate_free_float (rs, dreg);
			else
				mono_regstate_free_int (rs, dreg);
			if (cfg->new_ir)
				rs->vassign [prev_dreg] = -1;
		}

		if ((dest_dreg != -1) && (ins->dreg != dest_dreg)) {
			/* this instruction only outputs to dest_dreg, need to copy */
			create_copy_ins (cfg, bb, tmp, ins->dreg, dest_dreg, ins, ip, fp);
			ins->dreg = dest_dreg;

			if (fp) {
				if (rs->fsymbolic [dest_dreg] >= MONO_MAX_FREGS)
					free_up_reg (cfg, bb, tmp, ins, dest_dreg, fp);
			}
			else {
				if (rs->isymbolic [dest_dreg] >= MONO_MAX_IREGS)
					free_up_reg (cfg, bb, tmp, ins, dest_dreg, fp);
			}
		}

		if (spec_dest == 'b') {
			/* 
			 * The dest reg is read by the instruction, not written, so
			 * avoid allocating sreg1/sreg2 to the same reg.
			 */
			if (!sreg1_is_fp (spec))
				sreg1_mask &= ~ (regmask (ins->dreg));
			if (!sreg2_is_fp (spec))
				sreg2_mask &= ~ (regmask (ins->dreg));
		}

		/*
		 * TRACK CLOBBERING
		 */
		if ((clob_reg != -1) && (!(rs->ifree_mask & (regmask (clob_reg))))) {
			DEBUG (printf ("\tforced spill of clobbered reg R%d\n", rs->isymbolic [clob_reg]));
			get_register_force_spilling (cfg, bb, tmp, ins, rs->isymbolic [clob_reg], FALSE);
			mono_regstate_free_int (rs, clob_reg);
		}

		if (spec [MONO_INST_CLOB] == 'c') {
			int j, s, dreg, dreg2;
			guint64 clob_mask;

			clob_mask = MONO_ARCH_CALLEE_REGS;

			if (rs->ifree_mask != MONO_ARCH_CALLEE_REGS) {
				/*
				 * Need to avoid spilling the dreg since the dreg is not really
				 * clobbered by the call.
				 */
				if ((prev_dreg != -1) && !reg_is_fp (spec_dest))
					dreg = rs->vassign [prev_dreg];
				else
					dreg = -1;

				if (MONO_ARCH_INST_IS_REGPAIR (spec_dest))
					dreg2 = rs->vassign [prev_dreg + 1];
				else
					dreg2 = -1;

				for (j = 0; j < MONO_MAX_IREGS; ++j) {
					s = regmask (j);
					if ((clob_mask & s) && !(rs->ifree_mask & s) && (j != ins->sreg1)) {
						if ((j != dreg) && (j != dreg2))
							get_register_force_spilling (cfg, bb, tmp, ins, rs->isymbolic [j], FALSE);
						else if (rs->isymbolic [j])
							/* The hreg is assigned to the dreg of this instruction */
							rs->vassign [rs->isymbolic [j]] = -1;
						mono_regstate_free_int (rs, j);
					}
				}
			}

			if (rs->ffree_mask != MONO_ARCH_CALLEE_FREGS) {
				clob_mask = MONO_ARCH_CALLEE_FREGS;
				if ((prev_dreg != -1) && reg_is_fp (spec_dest))
					dreg = rs->vassign [prev_dreg];
				else
					dreg = -1;

				for (j = 0; j < MONO_MAX_FREGS; ++j) {
					s = regmask (j);
					if ((clob_mask & s) && !(rs->ffree_mask & s) && (j != ins->sreg1)) {
						if (j != dreg)
							get_register_force_spilling (cfg, bb, tmp, ins, rs->fsymbolic [j], TRUE);
						else if (rs->fsymbolic [j])
							/* The hreg is assigned to the dreg of this instruction */
							rs->vassign [rs->fsymbolic [j]] = -1;
						mono_regstate_free_float (rs, j);
					}
				}
			}
		}

		/*
		 * TRACK ARGUMENT REGS
		 */
		if (spec [MONO_INST_CLOB] == 'c') {
			MonoCallInst *call = (MonoCallInst*)ins;
			GSList *list;

			/* 
			 * This needs to be done before assigning sreg1, so sreg1 will
			 * not be assigned one of the argument regs.
			 */

			/* 
			 * Assign all registers in call->out_reg_args to the proper 
			 * argument registers.
			 */

			list = call->out_ireg_args;
			if (list) {
				while (list) {
					guint32 regpair;
					int reg, hreg;

					regpair = (guint32)(gssize)(list->data);
					hreg = regpair >> 24;
					reg = regpair & 0xffffff;

					assign_reg (cfg, rs, reg, hreg, FALSE);

					sreg1_mask &= ~(regmask (hreg));

					DEBUG (printf ("\tassigned arg reg %s to R%d\n", mono_arch_regname (hreg), reg));

					list = g_slist_next (list);
				}
			}

			list = call->out_freg_args;
			if (list) {
				while (list) {
					guint32 regpair;
					int reg, hreg;

					regpair = (guint32)(gssize)(list->data);
					hreg = regpair >> 24;
					reg = regpair & 0xffffff;

					assign_reg (cfg, rs, reg, hreg, TRUE);

					DEBUG (printf ("\tassigned arg reg %s to R%d\n", mono_arch_fregname (hreg), reg));

					list = g_slist_next (list);
				}
			}
		}

		/*
		 * TRACK SREG1
		 */
		fp = sreg1_is_fp (spec);
		if (MONO_ARCH_INST_IS_REGPAIR (spec_dest) && (spec [MONO_INST_CLOB] == '1')) {
			g_assert (is_soft_reg (ins->sreg1, fp));

			/* To simplify things, we allocate the same regpair to sreg1 and dreg */
			if (dest_sreg1 != -1)
				g_assert (dest_sreg1 == ins->dreg);
			val = mono_regstate_alloc_int (rs, regmask (ins->dreg));
			g_assert (val >= 0);

			if (rs->vassign [ins->sreg1] >= 0 && rs->vassign [ins->sreg1] != val)
				// FIXME:
				g_assert_not_reached ();

			assign_reg (cfg, rs, ins->sreg1, val, fp);

			DEBUG (printf ("\tassigned sreg1-low %s to R%d\n", mono_regname_full (val, fp), ins->sreg1));

			g_assert ((regmask (dreg_high)) & regpair_reg2_mask (spec_src1, ins->dreg));
			val = mono_regstate_alloc_int (rs, regmask (dreg_high));
			g_assert (val >= 0);

			if (rs->vassign [ins->sreg1 + 1] >= 0 && rs->vassign [ins->sreg1 + 1] != val)
				// FIXME:
				g_assert_not_reached ();

			assign_reg (cfg, rs, ins->sreg1 + 1, val, fp);

			DEBUG (printf ("\tassigned sreg1-high %s to R%d\n", mono_regname_full (val, fp), ins->sreg1 + 1));

			/* Skip rest of this section */
			dest_sreg1 = -1;
		}

		if (sreg1_fixed_mask) {
			g_assert (!fp);
			if (is_global_ireg (ins->sreg1)) {
				/* 
				 * The argument is already in a hard reg, but that reg is
				 * not usable by this instruction, so allocate a new one.
				 */
				val = mono_regstate_alloc_int (rs, sreg1_fixed_mask);
				if (val < 0)
					val = get_register_spilling (cfg, bb, tmp, ins, sreg1_fixed_mask, -1, fp);
				mono_regstate_free_int (rs, val);
				dest_sreg1 = val;

				/* Fall through to the dest_sreg1 != -1 case */
			}
			else
				sreg1_mask &= sreg1_fixed_mask;
		}

		if (dest_sreg1 != -1) {
			sreg1_mask = regmask (dest_sreg1);

			if ((rs->vassign [ins->sreg1] != dest_sreg1) && !(rs->ifree_mask & (regmask (dest_sreg1)))) {
				DEBUG (printf ("\tforced spill of R%d\n", rs->isymbolic [dest_sreg1]));
				get_register_force_spilling (cfg, bb, tmp, ins, rs->isymbolic [dest_sreg1], FALSE);
				mono_regstate_free_int (rs, dest_sreg1);
			}
			if (is_global_ireg (ins->sreg1)) {
				/* The argument is already in a hard reg, need to copy */
				MonoInst *copy = create_copy_ins (cfg, bb, tmp, dest_sreg1, ins->sreg1, NULL, ip, FALSE);
				insert_before_ins (bb, ins, copy);
				ins->sreg1 = dest_sreg1;
			}
		}

		if (is_soft_reg (ins->sreg1, fp)) {
			val = rs->vassign [ins->sreg1];
			prev_sreg1 = ins->sreg1;
			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}

				if ((ins->opcode == OP_MOVE) && !spill && !fp && is_local_ireg (ins->dreg) && (rs->ifree_mask & (regmask (ins->dreg)))) {
					/* 
					 * Allocate the same hreg to sreg1 as well so the 
					 * peephole can get rid of the move.
					 */
					sreg1_mask = regmask (ins->dreg);
				}

				if (spec [MONO_INST_CLOB] == '1' && !dreg_is_fp (spec) && (rs->ifree_mask & (regmask (ins->dreg))))
					/* Allocate the same reg to sreg1 to avoid a copy later */
					sreg1_mask = regmask (ins->dreg);

				val = alloc_reg (cfg, bb, tmp, ins, sreg1_mask, ins->sreg1, &reginfo [ins->sreg1], fp);
				assign_reg (cfg, rs, ins->sreg1, val, fp);
				DEBUG (printf ("\tassigned sreg1 %s to R%d\n", mono_regname_full (val, fp), ins->sreg1));

				if (spill) {
					MonoInst *store = create_spilled_store (cfg, bb, spill, val, prev_sreg1, tmp, NULL, fp);
					/*
					 * Need to insert before the instruction since it can
					 * overwrite sreg1.
					 */
					insert_before_ins (bb, ins, store);
				}
			}
			else if ((dest_sreg1 != -1) && (dest_sreg1 != val)) {
				MonoInst *copy = create_copy_ins (cfg, bb, tmp, dest_sreg1, val, NULL, ip, fp);
				insert_before_ins (bb, ins, copy);
				sreg2_mask &= ~(regmask (dest_sreg1));
				val = dest_sreg1;
			}
				
			ins->sreg1 = val;
		}
		else {
			prev_sreg1 = -1;
		}
		sreg2_mask &= ~(regmask (ins->sreg1));

		/* Handle the case when sreg1 is a regpair but dreg is not */
		if (MONO_ARCH_INST_IS_REGPAIR (spec_src1) && (spec [MONO_INST_CLOB] != '1')) {
			int reg2 = prev_sreg1 + 1;

			g_assert (!fp);
			g_assert (prev_sreg1 > -1);
			g_assert (!is_global_ireg (rs->vassign [prev_sreg1]));
			mask = regpair_reg2_mask (spec_src1, rs->vassign [prev_sreg1]);
			val = rs->vassign [reg2];
			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = mono_regstate_alloc_int (rs, mask);
				if (val < 0)
					val = get_register_spilling (cfg, bb, tmp, ins, mask, reg2, fp);
				if (spill)
					g_assert_not_reached ();
			}
			else {
				if (! (mask & (regmask (val)))) {
					/* The vreg is already allocated to a wrong hreg */
					/* FIXME: */
					g_assert_not_reached ();
#if 0
					val = mono_regstate_alloc_int (rs, mask);
					if (val < 0)
						val = get_register_spilling (cfg, bb, tmp, ins, mask, reg2, fp);

					/* Reallocate hreg to the correct register */
					create_copy_ins (cfg, bb, tmp, rs->vassign [reg2], val, ins, ip, fp);

					mono_regstate_free_int (rs, rs->vassign [reg2]);
#endif
				}
			}					

			sreg1_high = val;
			DEBUG (printf ("\tassigned sreg1 hreg %s to dest R%d\n", mono_arch_regname (val), reg2));
			assign_reg (cfg, rs, reg2, val, fp);
		}

		/* Handle dreg==sreg1 */
		if (((dreg_is_fp (spec) && spec_src1 == 'f') || spec [MONO_INST_CLOB] == '1') && ins->dreg != ins->sreg1) {
			MonoInst *sreg2_copy = NULL;
			MonoInst *copy;
			gboolean fp = (spec_src1 == 'f');

			if (ins->dreg == ins->sreg2) {
				/* 
				 * copying sreg1 to dreg could clobber sreg2, so allocate a new
				 * register for it.
				 */
				int reg2 = alloc_reg (cfg, bb, tmp, ins, dreg_mask, ins->sreg2, NULL, fp);

				DEBUG (printf ("\tneed to copy sreg2 %s to reg %s\n", mono_regname_full (ins->sreg2, fp), mono_regname_full (reg2, fp)));
				sreg2_copy = create_copy_ins (cfg, bb, tmp, reg2, ins->sreg2, NULL, ip, fp);
				prev_sreg2 = ins->sreg2 = reg2;

				if (fp)
					mono_regstate_free_float (rs, reg2);
				else
					mono_regstate_free_int (rs, reg2);
			}

			if (MONO_ARCH_INST_IS_REGPAIR (spec_src1)) {
				/* Copying sreg1_high to dreg could also clobber sreg2 */
				if (rs->vassign [prev_sreg1 + 1] == ins->sreg2)
					/* FIXME: */
					g_assert_not_reached ();

				/* 
				 * sreg1 and dest are already allocated to the same regpair by the
				 * SREG1 allocation code.
				 */
				g_assert (ins->sreg1 == ins->dreg);
				g_assert (dreg_high == sreg1_high);
			}

			DEBUG (printf ("\tneed to copy sreg1 %s to dreg %s\n", mono_regname_full (ins->sreg1, fp), mono_regname_full (ins->dreg, fp)));
			copy = create_copy_ins (cfg, bb, tmp, ins->dreg, ins->sreg1, NULL, ip, fp);
			insert_before_ins (bb, ins, copy);

			if (sreg2_copy)
				insert_before_ins (bb, copy, sreg2_copy);

			/*
			 * Need to prevent sreg2 to be allocated to sreg1, since that
			 * would screw up the previous copy.
			 */
			sreg2_mask &= ~ (regmask (ins->sreg1));
			/* we set sreg1 to dest as well */
			prev_sreg1 = ins->sreg1 = ins->dreg;
			sreg2_mask &= ~ (regmask (ins->dreg));
		}

		/*
		 * TRACK SREG2
		 */
		fp = sreg2_is_fp (spec);
		if (MONO_ARCH_INST_IS_REGPAIR (spec_src2))
			g_assert_not_reached ();
		if (is_soft_reg (ins->sreg2, fp)) {
			val = rs->vassign [ins->sreg2];

			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = alloc_reg (cfg, bb, tmp, ins, sreg2_mask, ins->sreg2, &reginfo [ins->sreg2], fp);
				assign_reg (cfg, rs, ins->sreg2, val, fp);
				DEBUG (printf ("\tassigned sreg2 %s to R%d\n", mono_regname_full (val, fp), ins->sreg2));
				if (spill) {
					MonoInst *store = create_spilled_store (cfg, bb, spill, val, prev_sreg2, tmp, NULL, fp);
					/*
					 * Need to insert before the instruction since it can
					 * overwrite sreg2.
					 */
					insert_before_ins (bb, ins, store);
				}
			}
			ins->sreg2 = val;
		}
		else {
			prev_sreg2 = -1;
		}

		/*if (reg_is_freeable (ins->sreg1) && prev_sreg1 >= 0 && reginfo [prev_sreg1].born_in >= i) {
			DEBUG (printf ("freeable %s\n", mono_arch_regname (ins->sreg1)));
			mono_regstate_free_int (rs, ins->sreg1);
		}
		if (reg_is_freeable (ins->sreg2) && prev_sreg2 >= 0 && reginfo [prev_sreg2].born_in >= i) {
			DEBUG (printf ("freeable %s\n", mono_arch_regname (ins->sreg2)));
			mono_regstate_free_int (rs, ins->sreg2);
		}*/
	
		DEBUG (mono_print_ins_index (i, ins));
	}

	// FIXME: Set MAX_FREGS to 8
	// FIXME: Optimize generated code
#if MONO_ARCH_USE_FPSTACK
	/*
	 * Make a forward pass over the code, simulating the fp stack, making sure the
	 * arguments required by the fp opcodes are at the top of the stack.
	 */
	if (has_fp) {
		MonoInst *prev = NULL;
		MonoInst *fxch;
		int tmp;

		for (ins = bb->code; ins; ins = ins->next) {
			spec = ins_get_spec (ins->opcode);

			DEBUG (printf ("processing:"));
			DEBUG (mono_print_ins_index (0, ins));

			if (ins->opcode == OP_FMOVE) {
				/* Do it by renaming the source to the destination on the stack */
				// FIXME: Is this correct ?
				for (i = 0; i < sp; ++i)
					if (fpstack [i] == ins->sreg1)
						fpstack [i] = ins->dreg;
				prev = ins;
				continue;
			}

			if (sreg1_is_fp (spec) && sreg2_is_fp (spec) && (fpstack [sp - 2] != ins->sreg1)) {
				/* Arg1 must be in %st(1) */
				g_assert (prev);

				i = 0;
				while ((i < sp) && (fpstack [i] != ins->sreg1))
					i ++;
				g_assert (i < sp);

				if (sp - 1 - i > 0) {
					/* First move it to %st(0) */
					DEBUG (printf ("\tswap %%st(0) and %%st(%d)\n", sp - 1 - i));
						
					MONO_INST_NEW (cfg, fxch, OP_X86_FXCH);
					fxch->inst_imm = sp - 1 - i;

					prev->next = fxch;
					fxch->next = ins;
					prev = fxch;

					tmp = fpstack [sp - 1];
					fpstack [sp - 1] = fpstack [i];
					fpstack [i] = tmp;
				}
					
				/* Then move it to %st(1) */
				DEBUG (printf ("\tswap %%st(0) and %%st(1)\n"));
				
				MONO_INST_NEW (cfg, fxch, OP_X86_FXCH);
				fxch->inst_imm = 1;

				prev->next = fxch;
				fxch->next = ins;
				prev = fxch;

				tmp = fpstack [sp - 1];
				fpstack [sp - 1] = fpstack [sp - 2];
				fpstack [sp - 2] = tmp;
			}

			if (sreg2_is_fp (spec)) {
				g_assert (sp > 0);

				if (fpstack [sp - 1] != ins->sreg2) {
					g_assert (prev);

					i = 0;
					while ((i < sp) && (fpstack [i] != ins->sreg2))
						i ++;
					g_assert (i < sp);

					DEBUG (printf ("\tswap %%st(0) and %%st(%d)\n", sp - 1 - i));

					MONO_INST_NEW (cfg, fxch, OP_X86_FXCH);
					fxch->inst_imm = sp - 1 - i;

					prev->next = fxch;
					fxch->next = ins;
					prev = fxch;

					tmp = fpstack [sp - 1];
					fpstack [sp - 1] = fpstack [i];
					fpstack [i] = tmp;
				}

				sp --;
			}

			if (sreg1_is_fp (spec)) {
				g_assert (sp > 0);

				if (fpstack [sp - 1] != ins->sreg1) {
					g_assert (prev);

					i = 0;
					while ((i < sp) && (fpstack [i] != ins->sreg1))
						i ++;
					g_assert (i < sp);

					DEBUG (printf ("\tswap %%st(0) and %%st(%d)\n", sp - 1 - i));

					MONO_INST_NEW (cfg, fxch, OP_X86_FXCH);
					fxch->inst_imm = sp - 1 - i;

					prev->next = fxch;
					fxch->next = ins;
					prev = fxch;

					tmp = fpstack [sp - 1];
					fpstack [sp - 1] = fpstack [i];
					fpstack [i] = tmp;
				}

				sp --;
			}

			if (dreg_is_fp (spec)) {
				g_assert (sp < 8);
				fpstack [sp ++] = ins->dreg;
			}

			if (G_UNLIKELY (cfg->verbose_level >= 2)) {
				printf ("\t[");
				for (i = 0; i < sp; ++i)
					printf ("%s%%fr%d", (i > 0) ? ", " : "", fpstack [i]);
				printf ("]\n");
			}

			prev = ins;
		}

		if (sp && bb != cfg->bb_exit && !(bb->out_count == 1 && bb->out_bb [0] == cfg->bb_exit)) {
			/* Remove remaining items from the fp stack */
			/* 
			 * These can remain for example as a result of a dead fmove like in
			 * System.Collections.Generic.EqualityComparer<double>.Equals ().
			 */
			while (sp) {
				MONO_INST_NEW (cfg, ins, OP_X86_FPOP);
				mono_add_ins_to_end (bb, ins);
				sp --;
			}
		}
	}
#endif
}

CompRelation
mono_opcode_to_cond (int opcode)
{
	switch (opcode) {
	case CEE_BEQ:
	case OP_CEQ:
	case OP_IBEQ:
	case OP_ICEQ:
	case OP_LBEQ:
	case OP_LCEQ:
	case OP_FBEQ:
	case OP_FCEQ:
	case OP_COND_EXC_EQ:
	case OP_COND_EXC_IEQ:
	case OP_CMOV_IEQ:
	case OP_CMOV_LEQ:
		return CMP_EQ;
	case CEE_BNE_UN:
	case OP_IBNE_UN:
	case OP_LBNE_UN:
	case OP_FBNE_UN:
	case OP_COND_EXC_NE_UN:
	case OP_COND_EXC_INE_UN:
	case OP_CMOV_INE_UN:
	case OP_CMOV_LNE_UN:
		return CMP_NE;
	case CEE_BLE:
	case OP_IBLE:
	case OP_LBLE:
	case OP_FBLE:
	case OP_CMOV_ILE:
	case OP_CMOV_LLE:
		return CMP_LE;
	case CEE_BGE:
	case OP_IBGE:
	case OP_LBGE:
	case OP_FBGE:
	case OP_CMOV_IGE:
	case OP_CMOV_LGE:
		return CMP_GE;
	case CEE_BLT:
	case OP_CLT:
	case OP_IBLT:
	case OP_ICLT:
	case OP_LBLT:
	case OP_LCLT:
	case OP_FBLT:
	case OP_FCLT:
	case OP_COND_EXC_LT:
	case OP_COND_EXC_ILT:
	case OP_CMOV_ILT:
	case OP_CMOV_LLT:
		return CMP_LT;
	case CEE_BGT:
	case OP_CGT:
	case OP_IBGT:
	case OP_ICGT:
	case OP_LBGT:
	case OP_LCGT:
	case OP_FBGT:
	case OP_FCGT:
	case OP_COND_EXC_GT:
	case OP_COND_EXC_IGT:
	case OP_CMOV_IGT:
	case OP_CMOV_LGT:
		return CMP_GT;

	case CEE_BLE_UN:
	case OP_IBLE_UN:
	case OP_LBLE_UN:
	case OP_FBLE_UN:
	case OP_COND_EXC_LE_UN:
	case OP_COND_EXC_ILE_UN:
	case OP_CMOV_ILE_UN:
	case OP_CMOV_LLE_UN:
		return CMP_LE_UN;
	case CEE_BGE_UN:
	case OP_IBGE_UN:
	case OP_LBGE_UN:
	case OP_FBGE_UN:
	case OP_CMOV_IGE_UN:
	case OP_CMOV_LGE_UN:
		return CMP_GE_UN;
	case CEE_BLT_UN:
	case OP_CLT_UN:
	case OP_IBLT_UN:
	case OP_ICLT_UN:
	case OP_LBLT_UN:
	case OP_LCLT_UN:
	case OP_FBLT_UN:
	case OP_FCLT_UN:
	case OP_COND_EXC_LT_UN:
	case OP_COND_EXC_ILT_UN:
	case OP_CMOV_ILT_UN:
	case OP_CMOV_LLT_UN:
		return CMP_LT_UN;
	case CEE_BGT_UN:
	case OP_CGT_UN:
	case OP_IBGT_UN:
	case OP_ICGT_UN:
	case OP_LBGT_UN:
	case OP_LCGT_UN:
	case OP_FCGT_UN:
	case OP_FBGT_UN:
	case OP_COND_EXC_GT_UN:
	case OP_COND_EXC_IGT_UN:
	case OP_CMOV_IGT_UN:
	case OP_CMOV_LGT_UN:
		return CMP_GT_UN;
	default:
		printf ("%s\n", mono_inst_name (opcode));
		g_assert_not_reached ();
		return 0;
	}
}

CompRelation
mono_negate_cond (CompRelation cond)
{
	switch (cond) {
	case CMP_EQ:
		return CMP_NE;
	case CMP_NE:
		return CMP_EQ;
	case CMP_LE:
		return CMP_GT;
	case CMP_GE:
		return CMP_LT;
	case CMP_LT:
		return CMP_GE;
	case CMP_GT:
		return CMP_LE;
	case CMP_LE_UN:
		return CMP_GT_UN;
	case CMP_GE_UN:
		return CMP_LT_UN;
	case CMP_LT_UN:
		return CMP_GE_UN;
	case CMP_GT_UN:
		return CMP_LE_UN;
	default:
		g_assert_not_reached ();
	}
}

CompType
mono_opcode_to_type (int opcode, int cmp_opcode)
{
	if ((opcode >= CEE_BEQ) && (opcode <= CEE_BLT_UN))
		return CMP_TYPE_L;
	else if ((opcode >= OP_CEQ) && (opcode <= OP_CLT_UN))
		return CMP_TYPE_L;
	else if ((opcode >= OP_IBEQ) && (opcode <= OP_IBLT_UN))
		return CMP_TYPE_I;
	else if ((opcode >= OP_ICEQ) && (opcode <= OP_ICLT_UN))
		return CMP_TYPE_I;
	else if ((opcode >= OP_LBEQ) && (opcode <= OP_LBLT_UN))
		return CMP_TYPE_L;
	else if ((opcode >= OP_LCEQ) && (opcode <= OP_LCLT_UN))
		return CMP_TYPE_L;
	else if ((opcode >= OP_FBEQ) && (opcode <= OP_FBLT_UN))
		return CMP_TYPE_F;
	else if ((opcode >= OP_FCEQ) && (opcode <= OP_FCLT_UN))
		return CMP_TYPE_F;
	else if ((opcode >= OP_COND_EXC_IEQ) && (opcode <= OP_COND_EXC_ILT_UN))
		return CMP_TYPE_I;
	else if ((opcode >= OP_COND_EXC_EQ) && (opcode <= OP_COND_EXC_LT_UN)) {
		switch (cmp_opcode) {
		case OP_ICOMPARE:
		case OP_ICOMPARE_IMM:
		case OP_LCOMPARE_IMM:
			return CMP_TYPE_I;
		default:
			return CMP_TYPE_L;
		}
	} else {
		g_error ("Unknown opcode '%s' in opcode_to_type", mono_inst_name (opcode));
		return 0;
	}
}

gboolean
mono_is_regsize_var (MonoType *t)
{
	if (t->byref)
		return TRUE;
	t = mono_type_get_underlying_type (t);
	switch (t->type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
#if SIZEOF_VOID_P == 8
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
#endif
		return TRUE;
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
		return TRUE;
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (t))
			return TRUE;
		return FALSE;
	case MONO_TYPE_VALUETYPE:
		return FALSE;
	}
	return FALSE;
}

/*
 * mono_peephole_ins:
 *
 *   Perform some architecture independent peephole optimizations.
 */
void
mono_peephole_ins (MonoBasicBlock *bb, MonoInst *ins)
{
	MonoInst *last_ins = ins->prev;

	switch (ins->opcode) {
	case OP_MUL_IMM: 
		/* remove unnecessary multiplication with 1 */
		if (ins->inst_imm == 1) {
			if (ins->dreg != ins->sreg1)
				ins->opcode = OP_MOVE;
			else
				MONO_DELETE_INS (bb, ins);
		}
		break;
	case OP_LOAD_MEMBASE:
	case OP_LOADI4_MEMBASE:
		/* 
		 * Note: if reg1 = reg2 the load op is removed
		 *
		 * OP_STORE_MEMBASE_REG reg1, offset(basereg) 
		 * OP_LOAD_MEMBASE offset(basereg), reg2
		 * -->
		 * OP_STORE_MEMBASE_REG reg1, offset(basereg)
		 * OP_MOVE reg1, reg2
		 */
		if (last_ins &&
			(((ins->opcode == OP_LOADI4_MEMBASE) && (last_ins->opcode == OP_STOREI4_MEMBASE_REG)) ||
			 ((ins->opcode == OP_LOAD_MEMBASE) && (last_ins->opcode == OP_STORE_MEMBASE_REG))) &&
			ins->inst_basereg == last_ins->inst_destbasereg &&
			ins->inst_offset == last_ins->inst_offset) {
			if (ins->dreg == last_ins->sreg1) {
				MONO_DELETE_INS (bb, ins);
				break;
			} else {
				ins->opcode = OP_MOVE;
				ins->sreg1 = last_ins->sreg1;
			}
			
			/* 
			 * Note: reg1 must be different from the basereg in the second load
			 * Note: if reg1 = reg2 is equal then second load is removed
			 *
			 * OP_LOAD_MEMBASE offset(basereg), reg1
			 * OP_LOAD_MEMBASE offset(basereg), reg2
			 * -->
			 * OP_LOAD_MEMBASE offset(basereg), reg1
			 * OP_MOVE reg1, reg2
			 */
		} if (last_ins && (last_ins->opcode == OP_LOADI4_MEMBASE
						   || last_ins->opcode == OP_LOAD_MEMBASE) &&
			  ins->inst_basereg != last_ins->dreg &&
			  ins->inst_basereg == last_ins->inst_basereg &&
			  ins->inst_offset == last_ins->inst_offset) {

			if (ins->dreg == last_ins->dreg) {
				MONO_DELETE_INS (bb, ins);
			} else {
				ins->opcode = OP_MOVE;
				ins->sreg1 = last_ins->dreg;
			}

			//g_assert_not_reached ();

#if 0
			/* 
			 * OP_STORE_MEMBASE_IMM imm, offset(basereg) 
			 * OP_LOAD_MEMBASE offset(basereg), reg
			 * -->
			 * OP_STORE_MEMBASE_IMM imm, offset(basereg) 
			 * OP_ICONST reg, imm
			 */
		} else if (last_ins && (last_ins->opcode == OP_STOREI4_MEMBASE_IMM
						|| last_ins->opcode == OP_STORE_MEMBASE_IMM) &&
				   ins->inst_basereg == last_ins->inst_destbasereg &&
				   ins->inst_offset == last_ins->inst_offset) {
			ins->opcode = OP_ICONST;
			ins->inst_c0 = last_ins->inst_imm;
			g_assert_not_reached (); // check this rule
#endif
		}
		break;
	case OP_LOADI1_MEMBASE:
	case OP_LOADU1_MEMBASE:
		/* 
		 * Note: if reg1 = reg2 the load op is removed
		 *
		 * OP_STORE_MEMBASE_REG reg1, offset(basereg) 
		 * OP_LOAD_MEMBASE offset(basereg), reg2
		 * -->
		 * OP_STORE_MEMBASE_REG reg1, offset(basereg)
		 * OP_MOVE reg1, reg2
		 */
		if (last_ins && (last_ins->opcode == OP_STOREI1_MEMBASE_REG) &&
			ins->inst_basereg == last_ins->inst_destbasereg &&
			ins->inst_offset == last_ins->inst_offset) {
			ins->opcode = (ins->opcode == OP_LOADI1_MEMBASE) ? OP_PCONV_TO_I1 : OP_PCONV_TO_U1;
			ins->sreg1 = last_ins->sreg1;
		}
		break;
	case OP_LOADI2_MEMBASE:
	case OP_LOADU2_MEMBASE:
		/* 
		 * Note: if reg1 = reg2 the load op is removed
		 *
		 * OP_STORE_MEMBASE_REG reg1, offset(basereg) 
		 * OP_LOAD_MEMBASE offset(basereg), reg2
		 * -->
		 * OP_STORE_MEMBASE_REG reg1, offset(basereg)
		 * OP_MOVE reg1, reg2
		 */
		if (last_ins && (last_ins->opcode == OP_STOREI2_MEMBASE_REG) &&
			ins->inst_basereg == last_ins->inst_destbasereg &&
			ins->inst_offset == last_ins->inst_offset) {
#if SIZEOF_VOID_P == 8
			ins->opcode = (ins->opcode == OP_LOADI2_MEMBASE) ? OP_PCONV_TO_I2 : OP_PCONV_TO_U2;
#else
			/* The definition of OP_PCONV_TO_U2 is wrong */
			ins->opcode = (ins->opcode == OP_LOADI2_MEMBASE) ? OP_PCONV_TO_I2 : OP_ICONV_TO_U2;
#endif
			ins->sreg1 = last_ins->sreg1;
		}
		break;
	case OP_MOVE:
	case OP_FMOVE:
		/*
		 * Removes:
		 *
		 * OP_MOVE reg, reg 
		 */
		if (ins->dreg == ins->sreg1) {
			MONO_DELETE_INS (bb, ins);
			break;
		}
		/* 
		 * Removes:
		 *
		 * OP_MOVE sreg, dreg 
		 * OP_MOVE dreg, sreg
		 */
		if (last_ins && last_ins->opcode == OP_MOVE &&
			ins->sreg1 == last_ins->dreg &&
			ins->dreg == last_ins->sreg1) {
			MONO_DELETE_INS (bb, ins);
		}
		break;
	case OP_NOP:
		MONO_DELETE_INS (bb, ins);
		break;
	}
}

