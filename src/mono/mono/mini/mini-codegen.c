/*
 * mini-codegen.c: Arch independent code generation functionality
 *
 * (C) 2003 Ximian, Inc.
 */

#include <string.h>
#include <math.h>
#include <unistd.h>

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

#if defined(__x86_64__)
const char * const amd64_desc [OP_LAST];
static const char*const * ins_spec = amd64_desc;
#elif defined(__sparc__) || defined(sparc)
const char * const sparc_desc [OP_LAST];
static const char*const * ins_spec = sparc_desc;
#elif defined(__i386__)
extern const char * const pentium_desc [OP_LAST];
static const char*const * ins_spec = pentium_desc;
#elif defined(__ia64__)
const char * const ia64_desc [OP_LAST];
static const char*const * ins_spec = ia64_desc;
#elif defined(__arm__)
const char * const arm_cpu_desc [OP_LAST];
static const char*const * ins_spec = arm_cpu_desc;
#elif defined(__s390x__)
const char * const s390x_cpu_desc [OP_LAST];
static const char*const * ins_spec = s390x_cpu_desc;
#elif defined(__s390__)
const char * const s390_cpu_desc [OP_LAST];
static const char*const * ins_spec = s390_cpu_desc;
#else
#error "Not implemented"
#endif

#define use_fpstack MONO_ARCH_USE_FPSTACK

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
    }
  else
      return new_list;
}

/**
 * Duplicated here from regalloc.c so they can be inlined
 * FIXME: Remove the old one after the new JIT is done
 */

static inline void
mono_regstate2_reset (MonoRegState *rs) {
	rs->next_vireg = MONO_MAX_IREGS;
	rs->next_vfreg = MONO_MAX_FREGS;
}

static inline MonoRegState*
mono_regstate2_new (void)
{
	MonoRegState* rs = g_new0 (MonoRegState, 1);

	mono_regstate2_reset (rs);

	return rs;
}

static inline void
mono_regstate2_free (MonoRegState *rs) {
	g_free (rs->iassign);
	if (rs->iassign != rs->fassign)
		g_free (rs->fassign);
	g_free (rs);
}

static inline void
mono_regstate2_assign (MonoRegState *rs) {
	rs->max_ireg = -1;

	if (rs->next_vireg > rs->iassign_size) {
		g_free (rs->iassign);
		rs->iassign_size = MAX (rs->next_vireg, 256);
		rs->iassign = g_malloc (rs->iassign_size * sizeof (int));
	}

	memset (rs->isymbolic, 0, MONO_MAX_IREGS * sizeof (rs->isymbolic [0]));
	memset (rs->iassign, -1, sizeof (rs->iassign [0]) * rs->next_vireg);

	if (rs->next_vfreg > rs->fassign_size) {
		g_free (rs->fassign);
		rs->fassign = g_malloc (MAX (MONO_MAX_FREGS, rs->next_vfreg) * sizeof (int));
		rs->fassign_size = rs->next_vfreg;
	}

	if (rs->next_vfreg > MONO_MAX_FREGS) {
		memset (rs->fsymbolic, 0, MONO_MAX_IREGS * sizeof (rs->fsymbolic [0]));
		memset (rs->fassign, -1, sizeof (rs->fassign [0]) * rs->next_vfreg);
	}
}

static inline int
mono_regstate2_alloc_int (MonoRegState *rs, regmask_t allow)
{
	int i;
	regmask_t mask = allow & rs->ifree_mask;
	for (i = 0; i < MONO_MAX_IREGS; ++i) {
		if (mask & ((regmask_t)1 << i)) {
			rs->ifree_mask &= ~ ((regmask_t)1 << i);
			rs->max_ireg = MAX (rs->max_ireg, i);
			return i;
		}
	}
	return -1;
}

static inline void
mono_regstate2_free_int (MonoRegState *rs, int reg)
{
	if (reg >= 0) {
		rs->ifree_mask |= (regmask_t)1 << reg;
		rs->isymbolic [reg] = 0;
	}
}

static inline int
mono_regstate2_alloc_float (MonoRegState *rs, regmask_t allow)
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
mono_regstate2_free_float (MonoRegState *rs, int reg)
{
	if (reg >= 0) {
		rs->ffree_mask |= (regmask_t)1 << reg;
		rs->fsymbolic [reg] = 0;
	}
}

static inline int
mono_regstate2_next_long (MonoRegState *rs)
{
	int rval = rs->next_vireg;

	rs->next_vireg += 2;

	return rval;
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
		call->used_fregs |= 1 << hreg;
		call->out_freg_args = g_slist_append_mempool (cfg->mempool, call->out_freg_args, (gpointer)(gssize)(regpair));
	} else {
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
mono_spillvar_offset (MonoCompile *cfg, int spillvar)
{
	MonoSpillInfo *info;

	if (G_UNLIKELY (spillvar >= cfg->spill_info_len)) {
		resize_spill_info (cfg, FALSE);
		g_assert (spillvar < cfg->spill_info_len);
	}

	info = &cfg->spill_info [spillvar];
	if (info->offset == -1) {
		if (cfg->flags & MONO_CFG_HAS_SPILLUP) {
			info->offset = cfg->stack_offset;
			cfg->stack_offset += sizeof (gpointer);
		} else {
			cfg->stack_offset += sizeof (gpointer);
			info->offset = - cfg->stack_offset;
		}
	}

	return info->offset;
}

#if MONO_ARCH_USE_FPSTACK

/*
 * returns the offset used by spillvar. It allocates a new
 * spill float variable if necessary. 
 * (same as mono_spillvar_offset but for float)
 */
static inline int
mono_spillvar_offset_float (MonoCompile *cfg, int spillvar)
{
	MonoSpillInfo *info;

	if (G_UNLIKELY (spillvar >= cfg->spill_info_float_len)) {
		resize_spill_info (cfg, TRUE);
		g_assert (spillvar < cfg->spill_info_float_len);
	}

	info = &cfg->spill_info_float [spillvar];
	if (info->offset == -1) {
		if (cfg->flags & MONO_CFG_HAS_SPILLUP) {
			cfg->stack_offset += 7;
			cfg->stack_offset &= ~7;
			info->offset = cfg->stack_offset;
			cfg->stack_offset += sizeof (double);
		} else {
			/* FIXME: align */
			cfg->stack_offset += sizeof (double);
			info->offset = - cfg->stack_offset;
		}
	}

	return info->offset;
}

/*
 * Creates a store for spilled floating point items
 */
static MonoInst*
create_spilled_store_float (MonoCompile *cfg, int spill, int reg, MonoInst *ins)
{
	MonoInst *store;
	MONO_INST_NEW (cfg, store, OP_STORER8_MEMBASE_REG);
	store->sreg1 = reg;
	store->inst_destbasereg = cfg->frame_reg;
	store->inst_offset = mono_spillvar_offset_float (cfg, spill);

	DEBUG (printf ("SPILLED FLOAT STORE (%d at 0x%08lx(%%sp)) (from %d)\n", spill, (long)store->inst_offset, reg));
	return store;
}

/*
 * Creates a load for spilled floating point items 
 */
static MonoInst*
create_spilled_load_float (MonoCompile *cfg, int spill, int reg, MonoInst *ins)
{
	MonoInst *load;
	MONO_INST_NEW (cfg, load, OP_LOADR8_SPILL_MEMBASE);
	load->dreg = reg;
	load->inst_basereg = cfg->frame_reg;
	load->inst_offset = mono_spillvar_offset_float (cfg, spill);

	DEBUG (printf ("SPILLED FLOAT LOAD (%d at 0x%08lx(%%sp)) (from %d)\n", spill, (long)load->inst_offset, reg));
	return load;
}

#endif /* MONO_ARCH_USE_FPSTACK */

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
#define is_hard_reg(r,fp) ((fp) ? ((r) < MONO_MAX_FREGS) : ((r) < MONO_MAX_IREGS))
#define is_soft_reg(r,fp) (!is_hard_reg((r),(fp)))
#define rassign(cfg,reg,fp) ((fp) ? (cfg)->rs->fassign [(reg)] : (cfg)->rs->iassign [(reg)])

#define sreg1_is_fp(spec) (spec [MONO_INST_SRC1] == 'f')
#define sreg2_is_fp(spec) (spec [MONO_INST_SRC2] == 'f')

#ifdef MONO_ARCH_INST_IS_FLOAT
#define dreg_is_fp(spec)  (MONO_ARCH_INST_IS_FLOAT (spec [MONO_INST_DEST]))
#else
#define dreg_is_fp(spec)  (spec [MONO_INST_DEST] == 'f')
#endif

#define sreg1_is_fp_ins(ins) (sreg1_is_fp (ins_spec [(ins)->opcode]))
#define sreg2_is_fp_ins(ins) (sreg2_is_fp (ins_spec [(ins)->opcode]))
#define dreg_is_fp_ins(ins)  (dreg_is_fp (ins_spec [(ins)->opcode]))

#define regpair_reg2_mask(desc,hreg1) ((MONO_ARCH_INST_REGPAIR_REG2 (desc,hreg1) != -1) ? (regmask (MONO_ARCH_INST_REGPAIR_REG2 (desc,hreg1))) : MONO_ARCH_CALLEE_REGS)

#ifdef MONO_ARCH_IS_GLOBAL_IREG
#undef is_global_ireg
#define is_global_ireg(reg) MONO_ARCH_IS_GLOBAL_IREG ((reg))
#endif

typedef struct {
	int born_in;
	int killed_in;
	int last_use;
	int prev_use;
	int flags;		/* used to track fp spill/load */
	regmask_t preferred_mask; /* the hreg where the register should be allocated, or 0 */
} RegTrack;

#ifndef DISABLE_LOGGING
static void
print_ins (int i, MonoInst *ins)
{
	const char *spec = ins_spec [ins->opcode];
	printf ("\t%-2d %s", i, mono_inst_name (ins->opcode));
	if (!spec)
		g_error ("Unknown opcode: %s\n", mono_inst_name (ins->opcode));

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
		if (is_soft_reg (ins->sreg1, fp))
			printf (" R%d", ins->sreg1);
		else if (spec [MONO_INST_SRC1] == 'b')
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
		printf ("liveness: %s [%d - %d]\n", r, t [i].born_in, t[i].last_use);
	}
}
#endif /* DISABLE_LOGGING */

typedef struct InstList InstList;

struct InstList {
	InstList *prev;
	InstList *next;
	MonoInst *data;
};

static inline InstList*
inst_list_prepend (guint8 *mem, InstList *list, MonoInst *data)
{
	InstList *item = (InstList*)mem;
	item->data = data;
	item->prev = NULL;
	item->next = list;
	if (list)
		list->prev = item;
	return item;
}

static inline void
insert_before_ins (MonoInst *ins, InstList *item, MonoInst* to_insert)
{
	MonoInst *prev;

	/*
	 * If this function is called multiple times, the new instructions are inserted
	 * in the proper order.
	 */

	if (item->next) {
		prev = item->next->data;

		while (prev->next != ins)
			prev = prev->next;
		to_insert->next = ins;
		prev->next = to_insert;
	} else {
		to_insert->next = ins;
	}

	/*
	 * insert_after_ins insert the new instruction before item->data, so
	 * we have to modify it to point to the first of the prepended instructions.
	 */
	if (item->data == ins)
		item->data = to_insert; 
}

static inline void
insert_after_ins (MonoInst *ins, InstList *item, MonoInst* to_insert)
{
	if (item->prev) {
		while (ins->next != item->prev->data)
			ins = ins->next;
	}
	to_insert->next = ins->next;
	ins->next = to_insert;
}

/*
 * Force the spilling of the variable in the symbolic register 'reg'.
 */
static int
get_register_force_spilling (MonoCompile *cfg, InstList *item, MonoInst *ins, int reg, gboolean fp)
{
	MonoInst *load;
	int i, sel, spill;
	int *assign, *symbolic;

	if (fp) {
		assign = cfg->rs->fassign;
		symbolic = cfg->rs->fsymbolic;
	}
	else {
		assign = cfg->rs->iassign;
		symbolic = cfg->rs->isymbolic;
	}	
	
	sel = assign [reg];
	/*i = cfg->rs->isymbolic [sel];
	g_assert (i == reg);*/
	i = reg;
	spill = ++cfg->spill_count;
	assign [i] = -spill - 1;
	if (fp)
		mono_regstate2_free_float (cfg->rs, sel);
	else
		mono_regstate2_free_int (cfg->rs, sel);
	/* we need to create a spill var and insert a load to sel after the current instruction */
	if (fp)
		MONO_INST_NEW (cfg, load, OP_LOADR8_MEMBASE);
	else
		MONO_INST_NEW (cfg, load, OP_LOAD_MEMBASE);
	load->dreg = sel;
	load->inst_basereg = cfg->frame_reg;
	load->inst_offset = mono_spillvar_offset (cfg, spill);
	insert_after_ins (ins, item, load);
	DEBUG (printf ("SPILLED LOAD (%d at 0x%08lx(%%ebp)) R%d (freed %s)\n", spill, (long)load->inst_offset, i, mono_regname_full (sel, fp)));
	if (fp)
		i = mono_regstate2_alloc_float (cfg->rs, regmask (sel));
	else
		i = mono_regstate2_alloc_int (cfg->rs, regmask (sel));
	g_assert (i == sel);

	return sel;
}

/* This isn't defined on older glib versions and on some platforms */
#ifndef G_GUINT64_FORMAT
#define G_GUINT64_FORMAT "ul"
#endif

static int
get_register_spilling (MonoCompile *cfg, InstList *item, MonoInst *ins, regmask_t regmask, int reg, gboolean fp)
{
	MonoInst *load;
	int i, sel, spill;
	int *assign, *symbolic;

	if (fp) {
		assign = cfg->rs->fassign;
		symbolic = cfg->rs->fsymbolic;
	}
	else {
		assign = cfg->rs->iassign;
		symbolic = cfg->rs->isymbolic;
	}

	DEBUG (printf ("\tstart regmask to assign R%d: 0x%08" G_GUINT64_FORMAT " (R%d <- R%d R%d)\n", reg, (guint64)regmask, ins->dreg, ins->sreg1, ins->sreg2));
	/* exclude the registers in the current instruction */
	if ((sreg1_is_fp_ins (ins) == fp) && (reg != ins->sreg1) && (reg_is_freeable (ins->sreg1, fp) || (is_soft_reg (ins->sreg1, fp) && rassign (cfg, ins->sreg1, fp) >= 0))) {
		if (is_soft_reg (ins->sreg1, fp))
			regmask &= ~ (regmask (rassign (cfg, ins->sreg1, fp)));
		else
			regmask &= ~ (regmask (ins->sreg1));
		DEBUG (printf ("\t\texcluding sreg1 %s\n", mono_regname_full (ins->sreg1, fp)));
	}
	if ((sreg2_is_fp_ins (ins) == fp) && (reg != ins->sreg2) && (reg_is_freeable (ins->sreg2, fp) || (is_soft_reg (ins->sreg2, fp) && rassign (cfg, ins->sreg2, fp) >= 0))) {
		if (is_soft_reg (ins->sreg2, fp))
			regmask &= ~ (regmask (rassign (cfg, ins->sreg2, fp)));
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
	sel = -1;
	/* we should track prev_use and spill the register that's farther */
	if (fp) {
		for (i = 0; i < MONO_MAX_FREGS; ++i) {
			if (regmask & (regmask (i))) {
				sel = i;
				DEBUG (printf ("\t\tselected register %s has assignment %d\n", mono_arch_fregname (sel), cfg->rs->fsymbolic [sel]));
				break;
			}
		}

		i = cfg->rs->fsymbolic [sel];
		spill = ++cfg->spill_count;
		cfg->rs->fassign [i] = -spill - 1;
		mono_regstate2_free_float (cfg->rs, sel);
	}
	else {
		for (i = 0; i < MONO_MAX_IREGS; ++i) {
			if (regmask & (regmask (i))) {
				sel = i;
				DEBUG (printf ("\t\tselected register %s has assignment %d\n", mono_arch_regname (sel), cfg->rs->isymbolic [sel]));
				break;
			}
		}

		i = cfg->rs->isymbolic [sel];
		spill = ++cfg->spill_count;
		cfg->rs->iassign [i] = -spill - 1;
		mono_regstate2_free_int (cfg->rs, sel);
	}

	/* we need to create a spill var and insert a load to sel after the current instruction */
	MONO_INST_NEW (cfg, load, fp ? OP_LOADR8_MEMBASE : OP_LOAD_MEMBASE);
	load->dreg = sel;
	load->inst_basereg = cfg->frame_reg;
	load->inst_offset = mono_spillvar_offset (cfg, spill);
	insert_after_ins (ins, item, load);
	DEBUG (printf ("\tSPILLED LOAD (%d at 0x%08lx(%%ebp)) R%d (freed %s)\n", spill, (long)load->inst_offset, i, mono_regname_full (sel, fp)));
	if (fp)
		i = mono_regstate2_alloc_float (cfg->rs, regmask (sel));
	else
		i = mono_regstate2_alloc_int (cfg->rs, regmask (sel));
	g_assert (i == sel);
	
	return sel;
}

static void
free_up_ireg (MonoCompile *cfg, InstList *item, MonoInst *ins, int hreg)
{
	if (!(cfg->rs->ifree_mask & (regmask (hreg)))) {
		DEBUG (printf ("\tforced spill of R%d\n", cfg->rs->isymbolic [hreg]));
		get_register_force_spilling (cfg, item, ins, cfg->rs->isymbolic [hreg], FALSE);
		mono_regstate2_free_int (cfg->rs, hreg);
	}
}

static void
free_up_reg (MonoCompile *cfg, InstList *item, MonoInst *ins, int hreg, gboolean fp)
{
	if (fp) {
		if (!(cfg->rs->ffree_mask & (regmask (hreg)))) {
			DEBUG (printf ("\tforced spill of R%d\n", cfg->rs->isymbolic [hreg]));
			get_register_force_spilling (cfg, item, ins, cfg->rs->isymbolic [hreg], fp);
			mono_regstate2_free_float (cfg->rs, hreg);
		}
	}
	else {
		if (!(cfg->rs->ifree_mask & (regmask (hreg)))) {
			DEBUG (printf ("\tforced spill of R%d\n", cfg->rs->isymbolic [hreg]));
			get_register_force_spilling (cfg, item, ins, cfg->rs->isymbolic [hreg], fp);
			mono_regstate2_free_int (cfg->rs, hreg);
		}
	}
}

static MonoInst*
create_copy_ins (MonoCompile *cfg, int dest, int src, MonoInst *ins, const unsigned char *ip, gboolean fp)
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
		copy->next = ins->next;
		copy->cil_code = ins->cil_code;
		ins->next = copy;
	}
	DEBUG (printf ("\tforced copy from %s to %s\n", mono_regname_full (src, fp), mono_regname_full (dest, fp)));
	return copy;
}

static MonoInst*
create_spilled_store (MonoCompile *cfg, int spill, int reg, int prev_reg, MonoInst *ins, gboolean fp)
{
	MonoInst *store;
	MONO_INST_NEW (cfg, store, fp ? OP_STORER8_MEMBASE_REG : OP_STORE_MEMBASE_REG);
	store->sreg1 = reg;
	store->inst_destbasereg = cfg->frame_reg;
	store->inst_offset = mono_spillvar_offset (cfg, spill);
	if (ins) {
		store->next = ins->next;
		ins->next = store;
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

static int
alloc_int_reg (MonoCompile *cfg, InstList *tmp, MonoInst *ins, regmask_t dest_mask, int sym_reg, RegTrack *info)
{
	int val;

	if (info && info->preferred_mask) {
		val = mono_regstate2_alloc_int (cfg->rs, info->preferred_mask & dest_mask);
		if (val >= 0) {
			DEBUG (printf ("\tallocated preferred reg R%d to %s\n", sym_reg, mono_arch_regname (val)));
			return val;
		}
	}

	val = mono_regstate2_alloc_int (cfg->rs, dest_mask);
	if (val < 0)
		val = get_register_spilling (cfg, tmp, ins, dest_mask, sym_reg, FALSE);

	return val;
}

static int
alloc_float_reg (MonoCompile *cfg, InstList *tmp, MonoInst *ins, regmask_t dest_mask, int sym_reg)
{
	int val;

	val = mono_regstate2_alloc_float (cfg->rs, dest_mask);

	if (val < 0) {
		val = get_register_spilling (cfg, tmp, ins, dest_mask, sym_reg, TRUE);
	}

	return val;
}

static int
alloc_reg (MonoCompile *cfg, InstList *tmp, MonoInst *ins, regmask_t dest_mask, int sym_reg, RegTrack *info, gboolean fp)
{
	if (fp)
		return alloc_float_reg (cfg, tmp, ins, dest_mask, sym_reg);
	else
		return alloc_int_reg (cfg, tmp, ins, dest_mask, sym_reg, info);
}

static inline void
assign_reg (MonoCompile *cfg, MonoRegState *rs, int reg, int hreg, gboolean fp)
{
	if (fp) {
		g_assert (reg >= MONO_MAX_FREGS);
		g_assert (hreg < MONO_MAX_FREGS);
		g_assert (! is_global_freg (hreg));

		rs->fassign [reg] = hreg;
		rs->fsymbolic [hreg] = reg;
		rs->ffree_mask &= ~ (regmask (hreg));
	}
	else {
		g_assert (reg >= MONO_MAX_IREGS);
		g_assert (hreg < MONO_MAX_IREGS);
		g_assert (! is_global_ireg (hreg));

		rs->iassign [reg] = hreg;
		rs->isymbolic [hreg] = reg;
		rs->ifree_mask &= ~ (regmask (hreg));
	}
}

static inline void
assign_ireg (MonoCompile *cfg, MonoRegState *rs, int reg, int hreg)
{
	assign_reg (cfg, rs, reg, hreg, FALSE);
}

/*
 * Local register allocation.
 * We first scan the list of instructions and we save the liveness info of
 * each register (when the register is first used, when it's value is set etc.).
 * We also reverse the list of instructions (in the InstList list) because assigning
 * registers backwards allows for more tricks to be used.
 */
void
mono_local_regalloc (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins;
	MonoRegState *rs = cfg->rs;
	int i, val, fpcount, ins_count;
	RegTrack *reginfo, *reginfof;
	RegTrack *reginfo1, *reginfo2, *reginfod;
	InstList *tmp, *reversed = NULL;
	const char *spec;
	GList *fspill_list = NULL;
	gboolean fp;
	int fspill = 0;
	guint8 *inst_list, *mem;
#if MONO_ARCH_USE_FPSTACK
	gboolean need_fpstack = (use_fpstack && bb->max_freg > MONO_MAX_FREGS);
#endif

	if (!bb->code)
		return;

	rs->next_vireg = bb->max_ireg;
	rs->next_vfreg = bb->max_freg;
	mono_regstate2_assign (rs);

	rs->ifree_mask = MONO_ARCH_CALLEE_REGS;
	rs->ffree_mask = MONO_ARCH_CALLEE_FREGS;

	if (use_fpstack)
		rs->ffree_mask = 0xff & ~(regmask (MONO_ARCH_FPSTACK_SIZE));

	ins = bb->code;

	/*if (cfg->opt & MONO_OPT_COPYPROP)
		local_copy_prop (cfg, ins);*/

	if (cfg->reginfo && cfg->reginfo_len < rs->next_vireg) {
		g_free (cfg->reginfo);
		cfg->reginfo = NULL;
	}
	reginfo = cfg->reginfo;
	if (!reginfo) {
		cfg->reginfo_len = MAX (256, rs->next_vireg * 2);
		reginfo = cfg->reginfo = mono_mempool_alloc (cfg->mempool, sizeof (RegTrack) * cfg->reginfo_len);
	} 
	else
		g_assert (cfg->reginfo_len >= rs->next_vireg);

	if (cfg->reginfof && cfg->reginfof_len < rs->next_vfreg) {
		g_free (cfg->reginfof);
		cfg->reginfof = NULL;
	}
	reginfof = cfg->reginfof;
	if (!reginfof) {
		cfg->reginfof_len = MAX (256, rs->next_vireg * 2);
		reginfof = cfg->reginfof = mono_mempool_alloc (cfg->mempool, sizeof (RegTrack) * cfg->reginfof_len);
	} 
	else
		g_assert (cfg->reginfof_len >= rs->next_vfreg);

	memset (reginfo, 0, rs->next_vireg * sizeof (RegTrack));
	memset (reginfof, 0, rs->next_vfreg * sizeof (RegTrack));

	ins_count = 0;
	for (ins = bb->code; ins; ins = ins->next) {
		spec = ins_spec [ins->opcode];

		ins_count ++;
	}

	if (cfg->reverse_inst_list && (cfg->reverse_inst_list_len < ins_count)) {
		g_free (cfg->reverse_inst_list);
		cfg->reverse_inst_list = NULL;
	}

	inst_list = cfg->reverse_inst_list;
	if (!inst_list) {
		cfg->reverse_inst_list_len = MAX (ins_count, 1024);
		inst_list = cfg->reverse_inst_list = mono_mempool_alloc (cfg->mempool, cfg->reverse_inst_list_len * sizeof (InstList));
	}
	mem = inst_list;

	i = 1;
	fpcount = 0;
	DEBUG (printf ("LOCAL regalloc: basic block: %d\n", bb->block_num));
	/* forward pass on the instructions to collect register liveness info */
	for (ins = bb->code; ins; ins = ins->next) {
		spec = ins_spec [ins->opcode];

		if (G_UNLIKELY (!spec)) {
			g_error ("Opcode '%s' missing from machine description file.", mono_inst_name (ins->opcode));
		}
		
		DEBUG (print_ins (i, ins));

		/*
		 * TRACK FP STACK
		 */
#if MONO_ARCH_USE_FPSTACK
		if (need_fpstack) {
			GList *spill;

			if (spec [MONO_INST_SRC1] == 'f') {
				spill = g_list_first (fspill_list);
				if (spill && fpcount < MONO_ARCH_FPSTACK_SIZE) {
					reginfof [ins->sreg1].flags |= MONO_FP_NEEDS_LOAD;
					fspill_list = g_list_remove (fspill_list, spill->data);
				} else
					fpcount--;
			}

			if (spec [MONO_INST_SRC2] == 'f') {
				spill = g_list_first (fspill_list);
				if (spill) {
					reginfof [ins->sreg2].flags |= MONO_FP_NEEDS_LOAD;
					fspill_list = g_list_remove (fspill_list, spill->data);
					if (fpcount >= MONO_ARCH_FPSTACK_SIZE) {
						fspill++;
						fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
						reginfof [ins->sreg2].flags |= MONO_FP_NEEDS_LOAD_SPILL;
					}
				} else
					fpcount--;
			}

			if (dreg_is_fp (spec)) {
				if (use_fpstack && (spec [MONO_INST_CLOB] != 'm')) {
					if (fpcount >= MONO_ARCH_FPSTACK_SIZE) {
						reginfof [ins->dreg].flags |= MONO_FP_NEEDS_SPILL;
						fspill++;
						fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
						fpcount--;
					}
					fpcount++;
				}
			}
		}
#endif

		if (spec [MONO_INST_SRC1]) {
			if (spec [MONO_INST_SRC1] == 'f')
				reginfo1 = reginfof;
			else
				reginfo1 = reginfo;
			reginfo1 [ins->sreg1].prev_use = reginfo1 [ins->sreg1].last_use;
			reginfo1 [ins->sreg1].last_use = i;
			if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_SRC2])) {
				/* The virtual register is allocated sequentially */
				reginfo1 [ins->sreg1 + 1].prev_use = reginfo1 [ins->sreg1 + 1].last_use;
				reginfo1 [ins->sreg1 + 1].last_use = i;
				if (reginfo1 [ins->sreg1 + 1].born_in == 0 || reginfo1 [ins->sreg1 + 1].born_in > i)
					reginfo1 [ins->sreg1 + 1].born_in = i;
			}
		} else {
			ins->sreg1 = -1;
		}
		if (spec [MONO_INST_SRC2]) {
			if (spec [MONO_INST_SRC2] == 'f')
				reginfo2 = reginfof;
			else
				reginfo2 = reginfo;
			reginfo2 [ins->sreg2].prev_use = reginfo2 [ins->sreg2].last_use;
			reginfo2 [ins->sreg2].last_use = i;
			if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_SRC2])) {
				/* The virtual register is allocated sequentially */
				reginfo2 [ins->sreg2 + 1].prev_use = reginfo2 [ins->sreg2 + 1].last_use;
				reginfo2 [ins->sreg2 + 1].last_use = i;
				if (reginfo2 [ins->sreg2 + 1].born_in == 0 || reginfo2 [ins->sreg2 + 1].born_in > i)
					reginfo2 [ins->sreg2 + 1].born_in = i;
			}
		} else {
			ins->sreg2 = -1;
		}
		if (spec [MONO_INST_DEST]) {
			int dest_dreg;

			if (dreg_is_fp (spec))
				reginfod = reginfof;
			else
				reginfod = reginfo;
			if (spec [MONO_INST_DEST] != 'b') /* it's not just a base register */
				reginfod [ins->dreg].killed_in = i;
			reginfod [ins->dreg].prev_use = reginfod [ins->dreg].last_use;
			reginfod [ins->dreg].last_use = i;
			if (reginfod [ins->dreg].born_in == 0 || reginfod [ins->dreg].born_in > i)
				reginfod [ins->dreg].born_in = i;

			dest_dreg = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_DEST]);
			if (dest_dreg != -1)
				reginfod [ins->dreg].preferred_mask = (regmask (dest_dreg));

			if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_DEST])) {
				/* The virtual register is allocated sequentially */
				reginfod [ins->dreg + 1].prev_use = reginfod [ins->dreg + 1].last_use;
				reginfod [ins->dreg + 1].last_use = i;
				if (reginfod [ins->dreg + 1].born_in == 0 || reginfod [ins->dreg + 1].born_in > i)
					reginfod [ins->dreg + 1].born_in = i;
				if (MONO_ARCH_INST_REGPAIR_REG2 (spec [MONO_INST_DEST], -1) != -1)
					reginfod [ins->dreg + 1].preferred_mask = regpair_reg2_mask (spec [MONO_INST_DEST], -1);
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

					reginfo [reg].prev_use = reginfo [reg].last_use;
					reginfo [reg].last_use = i;

					list = g_slist_next (list);
				}
			}

			list = call->out_freg_args;
			if (!use_fpstack && list) {
				while (list) {
					guint32 regpair;
					int reg, hreg;

					regpair = (guint32)(gssize)(list->data);
					hreg = regpair >> 24;
					reg = regpair & 0xffffff;

					reginfof [reg].prev_use = reginfof [reg].last_use;
					reginfof [reg].last_use = i;

					list = g_slist_next (list);
				}
			}
		}

		reversed = inst_list_prepend (mem, reversed, ins);
		mem += sizeof (InstList);
		++i;
	}

	// todo: check if we have anything left on fp stack, in verify mode?
	fspill = 0;

	DEBUG (print_regtrack (reginfo, rs->next_vireg));
	DEBUG (print_regtrack (reginfof, rs->next_vfreg));
	tmp = reversed;
	while (tmp) {
		int prev_dreg, prev_sreg1, prev_sreg2, clob_dreg;
		int dest_dreg, dest_sreg1, dest_sreg2, clob_reg;
		int dreg_high, sreg1_high;
		regmask_t dreg_mask, sreg1_mask, sreg2_mask, mask;
		const unsigned char *ip;
		--i;
		ins = tmp->data;
		spec = ins_spec [ins->opcode];
		prev_dreg = -1;
		prev_sreg2 = -1;
		clob_dreg = -1;
		clob_reg = -1;
		dest_dreg = -1;
		dest_sreg1 = -1;
		dest_sreg2 = -1;
		dreg_mask = dreg_is_fp (spec) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;
		sreg1_mask = sreg1_is_fp (spec) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;
		sreg2_mask = sreg2_is_fp (spec) ? MONO_ARCH_CALLEE_FREGS : MONO_ARCH_CALLEE_REGS;

		DEBUG (printf ("processing:"));
		DEBUG (print_ins (i, ins));

		ip = ins->cil_code;

		/*
		 * FIXED REGS
		 */
		dest_sreg1 = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_SRC1]);
		dest_sreg2 = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_SRC2]);
		dest_dreg = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_DEST]);
		clob_reg = MONO_ARCH_INST_FIXED_REG (spec [MONO_INST_CLOB]);
		sreg2_mask &= ~ (MONO_ARCH_INST_SREG2_MASK (spec));

		/*
		 * TRACK FP STACK
		 */
#if MONO_ARCH_USE_FPSTACK
		if (need_fpstack && (spec [MONO_INST_CLOB] != 'm')) {
			if (dreg_is_fp (spec)) {
				if (reginfof [ins->dreg].flags & MONO_FP_NEEDS_SPILL) {
					GList *spill_node;
					MonoInst *store;
					spill_node = g_list_first (fspill_list);
					g_assert (spill_node);

					store = create_spilled_store_float (cfg, GPOINTER_TO_INT (spill_node->data), ins->dreg, ins);
					insert_before_ins (ins, tmp, store);
					fspill_list = g_list_remove (fspill_list, spill_node->data);
					fspill--;
				}
			}

			if (spec [MONO_INST_SRC1] == 'f') {
				if (reginfof [ins->sreg1].flags & MONO_FP_NEEDS_LOAD) {
					MonoInst *load;
					MonoInst *store = NULL;

					if (reginfof [ins->sreg1].flags & MONO_FP_NEEDS_LOAD_SPILL) {
						GList *spill_node;
						spill_node = g_list_first (fspill_list);
						g_assert (spill_node);

						store = create_spilled_store_float (cfg, GPOINTER_TO_INT (spill_node->data), ins->sreg1, ins);		
						fspill_list = g_list_remove (fspill_list, spill_node->data);
					}

					fspill++;
					fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
					load = create_spilled_load_float (cfg, fspill, ins->sreg1, ins);
					insert_before_ins (ins, tmp, load);
					if (store) 
						insert_before_ins (load, tmp, store);
				}
			}

			if (spec [MONO_INST_SRC2] == 'f') {
				if (reginfof [ins->sreg2].flags & MONO_FP_NEEDS_LOAD) {
					MonoInst *load;
					MonoInst *store = NULL;

					if (reginfof [ins->sreg2].flags & MONO_FP_NEEDS_LOAD_SPILL) {
						GList *spill_node;

						spill_node = g_list_first (fspill_list);
						g_assert (spill_node);
						if (spec [MONO_INST_SRC1] == 'f' && (reginfof [ins->sreg2].flags & MONO_FP_NEEDS_LOAD_SPILL))
							spill_node = g_list_next (spill_node);
	
						store = create_spilled_store_float (cfg, GPOINTER_TO_INT (spill_node->data), ins->sreg2, ins);
						fspill_list = g_list_remove (fspill_list, spill_node->data);
					}
				
					fspill++;
					fspill_list = g_list_prepend (fspill_list, GINT_TO_POINTER(fspill));
					load = create_spilled_load_float (cfg, fspill, ins->sreg2, ins);
					insert_before_ins (ins, tmp, load);
					if (store) 
						insert_before_ins (load, tmp, store);
				}
			}
		}
#endif

		/*
		 * TRACK FIXED SREG2
		 */
		if (dest_sreg2 != -1) {
			if (rs->ifree_mask & (regmask (dest_sreg2))) {
				if (is_global_ireg (ins->sreg2)) {
					/* Argument already in hard reg, need to copy */
					MonoInst *copy = create_copy_ins (cfg, dest_sreg2, ins->sreg2, NULL, ip, FALSE);
					insert_before_ins (ins, tmp, copy);
				}
				else {
					DEBUG (printf ("\tshortcut assignment of R%d to %s\n", ins->sreg2, mono_arch_regname (dest_sreg2)));
					assign_ireg (cfg, rs, ins->sreg2, dest_sreg2);
				}
			} else {
				int need_spill = TRUE;

				dreg_mask &= ~ (regmask (dest_sreg2));
				sreg1_mask &= ~ (regmask (dest_sreg2));

				/* 
				 * First check if dreg is assigned to dest_sreg2, since we
				 * can't spill a dreg.
				 */
				val = rs->iassign [ins->dreg];
				if (val == dest_sreg2 && ins->dreg != ins->sreg2) {
					/* 
					 * the destination register is already assigned to 
					 * dest_sreg2: we need to allocate another register for it 
					 * and then copy from this to dest_sreg2.
					 */
					int new_dest;
					new_dest = alloc_int_reg (cfg, tmp, ins, dreg_mask, ins->dreg, &reginfo [ins->dreg]);
					g_assert (new_dest >= 0);
					DEBUG (printf ("\tchanging dreg R%d to %s from %s\n", ins->dreg, mono_arch_regname (new_dest), mono_arch_regname (dest_sreg2)));

					prev_dreg = ins->dreg;
					assign_ireg (cfg, rs, ins->dreg, new_dest);
					clob_dreg = ins->dreg;
					create_copy_ins (cfg, dest_sreg2, new_dest, ins, ip, FALSE);
					need_spill = FALSE;
				}

				if (is_global_ireg (ins->sreg2)) {
					MonoInst *copy = create_copy_ins (cfg, dest_sreg2, ins->sreg2, NULL, ip, FALSE);
					insert_before_ins (ins, tmp, copy);
				}
				else {
					val = rs->iassign [ins->sreg2];
					if (val == dest_sreg2) {
						/* sreg2 is already assigned to the correct register */
						need_spill = FALSE;
					}
					else if ((val >= 0) || (val < -1)) {
						/* FIXME: sreg2 already assigned to another register */
						g_assert_not_reached ();
					}
				}

				if (need_spill) {
					DEBUG (printf ("\tforced spill of R%d\n", rs->isymbolic [dest_sreg2]));
					get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [dest_sreg2], FALSE);
					mono_regstate2_free_int (rs, dest_sreg2);
				}

				if (!is_global_ireg (ins->sreg2))
					/* force-set sreg2 */
					assign_ireg (cfg, rs, ins->sreg2, dest_sreg2);
			}
			ins->sreg2 = dest_sreg2;
		}

		/*
		 * TRACK DREG
		 */
		fp = dreg_is_fp (spec);
		if (spec [MONO_INST_DEST] && (!fp || (fp && !use_fpstack)) && is_soft_reg (ins->dreg, fp))
			prev_dreg = ins->dreg;

		if (spec [MONO_INST_DEST] == 'b') {
			/* 
			 * The dest reg is read by the instruction, not written, so
			 * avoid allocating sreg1/sreg2 to the same reg.
			 */
			if (dest_sreg1 != -1)
				dreg_mask &= ~ (regmask (dest_sreg1));
			if (dest_sreg2 != -1)
				dreg_mask &= ~ (regmask (dest_sreg2));
		}

		/*
		 * If dreg is a fixed regpair, free up both of the needed hregs to avoid
		 * various complex situations.
		 */
		if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_DEST])) {
			guint32 dreg2, dest_dreg2;

			g_assert (is_soft_reg (ins->dreg, fp));

			if (dest_dreg != -1) {
				if (rs->iassign [ins->dreg] != dest_dreg)
					free_up_ireg (cfg, tmp, ins, dest_dreg);

				dreg2 = ins->dreg + 1;
				dest_dreg2 = MONO_ARCH_INST_REGPAIR_REG2 (spec [MONO_INST_DEST], dest_dreg);
				if (dest_dreg2 != -1) {
					if (rs->iassign [dreg2] != dest_dreg2)
						free_up_ireg (cfg, tmp, ins, dest_dreg2);
				}
			}
		}

		if ((!fp || (fp && !use_fpstack)) && (is_soft_reg (ins->dreg, fp))) {
			if (dest_dreg != -1)
				dreg_mask = (regmask (dest_dreg));

			val = rassign (cfg, ins->dreg, fp);

			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = alloc_reg (cfg, tmp, ins, dreg_mask, ins->dreg, &reginfo [ins->dreg], fp);
				assign_reg (cfg, rs, ins->dreg, val, fp);
				if (spill)
					create_spilled_store (cfg, spill, val, prev_dreg, ins, fp);
			}
				
			DEBUG (printf ("\tassigned dreg %s to dest R%d\n", mono_regname_full (val, fp), ins->dreg));
			ins->dreg = val;
		}

		/* Handle regpairs */
		if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_DEST])) {
			int reg2 = prev_dreg + 1;

			g_assert (!fp);
			g_assert (prev_dreg > -1);
			g_assert (!is_global_ireg (rs->iassign [prev_dreg]));
			mask = regpair_reg2_mask (spec [MONO_INST_DEST], rs->iassign [prev_dreg]);
			val = rs->iassign [reg2];
			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = mono_regstate2_alloc_int (rs, mask);
				if (val < 0)
					val = get_register_spilling (cfg, tmp, ins, mask, reg2, fp);
				if (spill)
					create_spilled_store (cfg, spill, val, reg2, ins, fp);
			}
			else {
				if (! (mask & (regmask (val)))) {
					val = mono_regstate2_alloc_int (rs, mask);
					if (val < 0)
						val = get_register_spilling (cfg, tmp, ins, mask, reg2, fp);

					/* Reallocate hreg to the correct register */
					create_copy_ins (cfg, rs->iassign [reg2], val, ins, ip, fp);

					mono_regstate2_free_int (rs, rs->iassign [reg2]);
				}
			}					

			DEBUG (printf ("\tassigned dreg-high %s to dest R%d\n", mono_arch_regname (val), reg2));
			assign_reg (cfg, rs, reg2, val, fp);

			dreg_high = val;
			ins->unused = val;

			if (reg_is_freeable (val, fp) && reg2 >= 0 && (reginfo [reg2].born_in >= i)) {
				DEBUG (printf ("\tfreeable %s (R%d)\n", mono_arch_regname (val), reg2));
				mono_regstate2_free_int (rs, val);
			}
		}

		if ((!fp || (fp && !use_fpstack)) && prev_dreg >= 0 && is_soft_reg (prev_dreg, fp) && (fp ? reginfof : reginfo) [prev_dreg].born_in >= i) {
			/* 
			 * In theory, we could free up the hreg even if the vreg is alive,
			 * but branches inside bblocks force us to assign the same hreg
			 * to a vreg every time it is encountered.
			 */
			int dreg = rassign (cfg, prev_dreg, fp);
			g_assert (dreg >= 0);
			DEBUG (printf ("\tfreeable %s (R%d) (born in %d)\n", mono_regname_full (dreg, fp), prev_dreg, (fp ? reginfof : reginfo) [prev_dreg].born_in));
			if (fp)
				mono_regstate2_free_float (rs, dreg);
			else
				mono_regstate2_free_int (rs, dreg);
		}

		if ((dest_dreg != -1) && (ins->dreg != dest_dreg)) {
			/* this instruction only outputs to dest_dreg, need to copy */
			create_copy_ins (cfg, ins->dreg, dest_dreg, ins, ip, fp);
			ins->dreg = dest_dreg;

			if (fp) {
				if (rs->fsymbolic [dest_dreg] >= MONO_MAX_FREGS)
					free_up_reg (cfg, tmp, ins, dest_dreg, fp);
			}
			else {
				if (rs->isymbolic [dest_dreg] >= MONO_MAX_IREGS)
					free_up_reg (cfg, tmp, ins, dest_dreg, fp);
			}
		}

		if (spec [MONO_INST_DEST] == 'b') {
			/* 
			 * The dest reg is read by the instruction, not written, so
			 * avoid allocating sreg1/sreg2 to the same reg.
			 */
			sreg1_mask &= ~ (regmask (ins->dreg));
			sreg2_mask &= ~ (regmask (ins->dreg));
		}

		/*
		 * TRACK CLOBBERING
		 */
		if ((clob_reg != -1) && (!(rs->ifree_mask & (regmask (clob_reg))))) {
			DEBUG (printf ("\tforced spill of clobbered reg R%d\n", rs->isymbolic [clob_reg]));
			get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [clob_reg], FALSE);
			mono_regstate2_free_int (rs, clob_reg);
		}

		if (spec [MONO_INST_CLOB] == 'c') {
			int j, s, dreg, dreg2;
			guint64 clob_mask;

			clob_mask = MONO_ARCH_CALLEE_REGS;

			/*
			 * Need to avoid spilling the dreg since the dreg is not really
			 * clobbered by the call.
			 */
			if ((prev_dreg != -1) && !dreg_is_fp (spec))
				dreg = rassign (cfg, prev_dreg, dreg_is_fp (spec));
			else
				dreg = -1;

			if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_DEST]))
				dreg2 = rassign (cfg, prev_dreg + 1, dreg_is_fp (spec));
			else
				dreg2 = -1;

			for (j = 0; j < MONO_MAX_IREGS; ++j) {
				s = regmask (j);
				if ((clob_mask & s) && !(rs->ifree_mask & s) && (j != ins->sreg1) && (j != dreg) && (j != dreg2)) {
					get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [j], FALSE);
					mono_regstate2_free_int (rs, j);
				}
			}

			if (!use_fpstack) {
				clob_mask = MONO_ARCH_CALLEE_FREGS;
				if ((prev_dreg != -1) && dreg_is_fp (spec))
					dreg = rassign (cfg, prev_dreg, dreg_is_fp (spec));
				else
					dreg = -1;

				for (j = 0; j < MONO_MAX_FREGS; ++j) {
					s = regmask (j);
					if ((clob_mask & s) && !(rs->ffree_mask & s) && (j != ins->sreg1) && (j != dreg)) {
						get_register_force_spilling (cfg, tmp, ins, rs->fsymbolic [j], TRUE);
						mono_regstate2_free_float (rs, j);
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
			if (list && !use_fpstack) {
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
		if ((!fp || (fp && !use_fpstack))) {
			if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_DEST]) && (spec [MONO_INST_CLOB] == '1')) {
				g_assert (is_soft_reg (ins->sreg1, fp));

				/* To simplify things, we allocate the same regpair to sreg1 and dreg */
				if (dest_sreg1 != -1)
					g_assert (dest_sreg1 == ins->dreg);
				val = mono_regstate2_alloc_int (rs, regmask (ins->dreg));
				g_assert (val >= 0);
				assign_reg (cfg, rs, ins->sreg1, val, fp);

				DEBUG (printf ("\tassigned sreg1-low %s to R%d\n", mono_regname_full (val, fp), ins->sreg1));

				g_assert ((regmask (dreg_high)) & regpair_reg2_mask (spec [MONO_INST_SRC1], ins->dreg));
				val = mono_regstate2_alloc_int (rs, regmask (dreg_high));
				g_assert (val >= 0);
				assign_reg (cfg, rs, ins->sreg1 + 1, val, fp);

				DEBUG (printf ("\tassigned sreg1-high %s to R%d\n", mono_regname_full (val, fp), ins->sreg1 + 1));

				/* Skip rest of this section */
				dest_sreg1 = -1;
			}

			if (dest_sreg1 != -1) {
				sreg1_mask = regmask (dest_sreg1);

				if (!(rs->ifree_mask & (regmask (dest_sreg1)))) {
					DEBUG (printf ("\tforced spill of R%d\n", rs->isymbolic [dest_sreg1]));
					get_register_force_spilling (cfg, tmp, ins, rs->isymbolic [dest_sreg1], FALSE);
					mono_regstate2_free_int (rs, dest_sreg1);
				}
				if (is_global_ireg (ins->sreg1)) {
					/* The argument is already in a hard reg, need to copy */
					MonoInst *copy = create_copy_ins (cfg, dest_sreg1, ins->sreg1, NULL, ip, FALSE);
					insert_before_ins (ins, tmp, copy);
					ins->sreg1 = dest_sreg1;
				}
			}

			if (is_soft_reg (ins->sreg1, fp)) {
				val = rassign (cfg, ins->sreg1, fp);
				prev_sreg1 = ins->sreg1;
				if (val < 0) {
					int spill = 0;
					if (val < -1) {
						/* the register gets spilled after this inst */
						spill = -val -1;
					}

					if (((ins->opcode == OP_MOVE) || (ins->opcode == OP_SETREG)) && !spill && !fp && (!is_global_ireg (ins->dreg) && (rs->ifree_mask & (regmask (ins->dreg))))) {
						/* 
						 * Allocate the same hreg to sreg1 as well so the 
						 * peephole can get rid of the move.
						 */
						sreg1_mask = regmask (ins->dreg);
					}

					val = alloc_reg (cfg, tmp, ins, sreg1_mask, ins->sreg1, &reginfo [ins->sreg1], fp);
					assign_reg (cfg, rs, ins->sreg1, val, fp);
					DEBUG (printf ("\tassigned sreg1 %s to R%d\n", mono_regname_full (val, fp), ins->sreg1));

					if (spill) {
						MonoInst *store = create_spilled_store (cfg, spill, val, prev_sreg1, NULL, fp);
						/*
						 * Need to insert before the instruction since it can
						 * overwrite sreg1.
						 */
						insert_before_ins (ins, tmp, store);
					}
				}
				else if ((dest_sreg1 != -1) && (dest_sreg1 != val)) {
					g_assert_not_reached ();
				}
				
				ins->sreg1 = val;
			}
			else {
				prev_sreg1 = -1;
			}
			sreg2_mask &= ~(regmask (ins->sreg1));
		}

		/* Handle the case when sreg1 is a regpair but dreg is not */
		if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_SRC1]) && (spec [MONO_INST_CLOB] != '1')) {
			int reg2 = prev_sreg1 + 1;

			g_assert (!fp);
			g_assert (prev_sreg1 > -1);
			g_assert (!is_global_ireg (rs->iassign [prev_sreg1]));
			mask = regpair_reg2_mask (spec [MONO_INST_SRC1], rs->iassign [prev_sreg1]);
			val = rs->iassign [reg2];
			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = mono_regstate2_alloc_int (rs, mask);
				if (val < 0)
					val = get_register_spilling (cfg, tmp, ins, mask, reg2, fp);
				if (spill)
					g_assert_not_reached ();
			}
			else {
				if (! (mask & (regmask (val)))) {
					/* The vreg is already allocated to a wrong hreg */
					/* FIXME: */
					g_assert_not_reached ();
#if 0
					val = mono_regstate2_alloc_int (rs, mask);
					if (val < 0)
						val = get_register_spilling (cfg, tmp, ins, mask, reg2, fp);

					/* Reallocate hreg to the correct register */
					create_copy_ins (cfg, rs->iassign [reg2], val, ins, ip, fp);

					mono_regstate2_free_int (rs, rs->iassign [reg2]);
#endif
				}
			}					

			sreg1_high = val;
			DEBUG (printf ("\tassigned sreg1 hreg %s to dest R%d\n", mono_arch_regname (val), reg2));
			assign_reg (cfg, rs, reg2, val, fp);
		}

		/* Handle dreg==sreg1 */
		if (((dreg_is_fp (spec) && spec [MONO_INST_SRC1] == 'f' && !use_fpstack) || spec [MONO_INST_CLOB] == '1') && ins->dreg != ins->sreg1) {
			MonoInst *sreg2_copy = NULL;
			MonoInst *copy;
			gboolean fp = (spec [MONO_INST_SRC1] == 'f');

			if (ins->dreg == ins->sreg2) {
				/* 
				 * copying sreg1 to dreg could clobber sreg2, so allocate a new
				 * register for it.
				 */
				int reg2 = alloc_reg (cfg, tmp, ins, dreg_mask, ins->sreg2, NULL, fp);

				DEBUG (printf ("\tneed to copy sreg2 %s to reg %s\n", mono_regname_full (ins->sreg2, fp), mono_regname_full (reg2, fp)));
				sreg2_copy = create_copy_ins (cfg, reg2, ins->sreg2, NULL, ip, fp);
				prev_sreg2 = ins->sreg2 = reg2;

				if (fp)
					mono_regstate2_free_float (rs, reg2);
				else
					mono_regstate2_free_int (rs, reg2);
			}

			if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_SRC1])) {
				/* Copying sreg1_high to dreg could also clobber sreg2 */
				if (rs->iassign [prev_sreg1 + 1] == ins->sreg2)
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
			copy = create_copy_ins (cfg, ins->dreg, ins->sreg1, NULL, ip, fp);
			insert_before_ins (ins, tmp, copy);

			if (sreg2_copy)
				insert_before_ins (copy, tmp, sreg2_copy);

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
		if (MONO_ARCH_INST_IS_REGPAIR (spec [MONO_INST_SRC2]))
			g_assert_not_reached ();
		if ((!fp || (fp && !use_fpstack)) && (is_soft_reg (ins->sreg2, fp))) {
			val = rassign (cfg, ins->sreg2, fp);

			if (val < 0) {
				int spill = 0;
				if (val < -1) {
					/* the register gets spilled after this inst */
					spill = -val -1;
				}
				val = alloc_reg (cfg, tmp, ins, sreg2_mask, ins->sreg2, &reginfo [ins->sreg2], fp);
				assign_reg (cfg, rs, ins->sreg2, val, fp);
				DEBUG (printf ("\tassigned sreg2 %s to R%d\n", mono_regname_full (val, fp), ins->sreg2));
				if (spill)
					create_spilled_store (cfg, spill, val, prev_sreg2, ins, fp);
			}
			ins->sreg2 = val;
		}
		else {
			prev_sreg2 = -1;
		}

		/*if (reg_is_freeable (ins->sreg1) && prev_sreg1 >= 0 && reginfo [prev_sreg1].born_in >= i) {
			DEBUG (printf ("freeable %s\n", mono_arch_regname (ins->sreg1)));
			mono_regstate2_free_int (rs, ins->sreg1);
		}
		if (reg_is_freeable (ins->sreg2) && prev_sreg2 >= 0 && reginfo [prev_sreg2].born_in >= i) {
			DEBUG (printf ("freeable %s\n", mono_arch_regname (ins->sreg2)));
			mono_regstate2_free_int (rs, ins->sreg2);
		}*/
	
		DEBUG (print_ins (i, ins));
		/* this may result from a insert_before call */
		if (!tmp->next)
			bb->code = tmp->data;
		tmp = tmp->next;
	}

	g_list_free (fspill_list);
}
