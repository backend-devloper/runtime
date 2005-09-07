/*
 * mini-arm.c: ARM backend for the Mono code generator
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2003 Ximian, Inc.
 */
#include "mini.h"
#include <string.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>

#include "mini-arm.h"
#include "inssel.h"
#include "cpu-arm.h"
#include "trace.h"
#include "mono/arch/arm/arm-fpa-codegen.h"

/*
 * TODO:
 * floating point support: on ARM it is a mess, there are at least 3
 * different setups, each of which binary incompat with the other.
 * 1) FPA: old and ugly, but unfortunately what current distros use
 *    the double binary format has the two words swapped. 8 double registers.
 *    Implemented usually by kernel emulation.
 * 2) softfloat: the compiler emulates all the fp ops. Usually uses the
 *    ugly swapped double format (I guess a softfloat-vfp exists, too, though).
 * 3) VFP: the new and actually sensible and useful FP support. Implemented
 *    in HW or kernel-emulated, requires new tools. I think this ios what symbian uses.
 *
 * The plan is to write the FPA support first. softfloat can be tested in a chroot.
 */
int mono_exc_esp_offset = 0;

#define arm_is_imm12(v) ((v) > -4096 && (v) < 4096)
#define arm_is_imm8(v) ((v) > -256 && (v) < 256)
#define arm_is_fpimm8(v) ((v) >= -1020 && (v) <= 1020)

const char*
mono_arch_regname (int reg) {
	static const char * rnames[] = {
		"arm_r0", "arm_r1", "arm_r2", "arm_r3", "arm_v1",
		"arm_v2", "arm_v3", "arm_v4", "arm_v5", "arm_v6",
		"arm_v7", "arm_fp", "arm_ip", "arm_sp", "arm_lr",
		"arm_pc"
	};
	if (reg >= 0 && reg < 16)
		return rnames [reg];
	return "unknown";
}

const char*
mono_arch_fregname (int reg) {
	static const char * rnames[] = {
		"arm_f0", "arm_f1", "arm_f2", "arm_f3", "arm_f4",
		"arm_f5", "arm_f6", "arm_f7", "arm_f8", "arm_f9",
		"arm_f10", "arm_f11", "arm_f12", "arm_f13", "arm_f14",
		"arm_f15", "arm_f16", "arm_f17", "arm_f18", "arm_f19",
		"arm_f20", "arm_f21", "arm_f22", "arm_f23", "arm_f24",
		"arm_f25", "arm_f26", "arm_f27", "arm_f28", "arm_f29",
		"arm_f30", "arm_f31"
	};
	if (reg >= 0 && reg < 32)
		return rnames [reg];
	return "unknown";
}

static guint8*
emit_big_add (guint8 *code, int dreg, int sreg, int imm)
{
	int imm8, rot_amount;
	if ((imm8 = mono_arm_is_rotated_imm8 (imm, &rot_amount)) >= 0) {
		ARM_ADD_REG_IMM (code, dreg, sreg, imm8, rot_amount);
		return code;
	}
	g_assert (dreg != sreg);
	code = mono_arm_emit_load_imm (code, dreg, imm);
	ARM_ADD_REG_REG (code, dreg, dreg, sreg);
	return code;
}

static guint8*
emit_memcpy (guint8 *code, int size, int dreg, int doffset, int sreg, int soffset)
{
	/* we can use r0-r3, since this is called only for incoming args on the stack */
	if (size > sizeof (gpointer) * 4) {
		guint8 *start_loop;
		code = emit_big_add (code, ARMREG_R0, sreg, soffset);
		code = emit_big_add (code, ARMREG_R1, dreg, doffset);
		start_loop = code = mono_arm_emit_load_imm (code, ARMREG_R2, size);
		ARM_LDR_IMM (code, ARMREG_R3, ARMREG_R0, 0);
		ARM_STR_IMM (code, ARMREG_R3, ARMREG_R1, 0);
		ARM_ADD_REG_IMM8 (code, ARMREG_R0, ARMREG_R0, 4);
		ARM_ADD_REG_IMM8 (code, ARMREG_R1, ARMREG_R1, 4);
		ARM_SUBS_REG_IMM8 (code, ARMREG_R2, ARMREG_R2, 4);
		ARM_B_COND (code, ARMCOND_NE, 0);
		arm_patch (code - 4, start_loop);
		return code;
	}
	g_assert (arm_is_imm12 (doffset));
	g_assert (arm_is_imm12 (doffset + size));
	g_assert (arm_is_imm12 (soffset));
	g_assert (arm_is_imm12 (soffset + size));
	while (size >= 4) {
		ARM_LDR_IMM (code, ARMREG_LR, sreg, soffset);
		ARM_STR_IMM (code, ARMREG_LR, dreg, doffset);
		doffset += 4;
		soffset += 4;
		size -= 4;
	}
	g_assert (size == 0);
	return code;
}

/*
 * mono_arch_get_argument_info:
 * @csig:  a method signature
 * @param_count: the number of parameters to consider
 * @arg_info: an array to store the result infos
 *
 * Gathers information on parameters such as size, alignment and
 * padding. arg_info should be large enought to hold param_count + 1 entries. 
 *
 * Returns the size of the activation frame.
 */
int
mono_arch_get_argument_info (MonoMethodSignature *csig, int param_count, MonoJitArgumentInfo *arg_info)
{
	int k, frame_size = 0;
	int size, align, pad;
	int offset = 8;

	if (MONO_TYPE_ISSTRUCT (csig->ret)) { 
		frame_size += sizeof (gpointer);
		offset += 4;
	}

	arg_info [0].offset = offset;

	if (csig->hasthis) {
		frame_size += sizeof (gpointer);
		offset += 4;
	}

	arg_info [0].size = frame_size;

	for (k = 0; k < param_count; k++) {
		
		if (csig->pinvoke)
			size = mono_type_native_stack_size (csig->params [k], &align);
		else
			size = mono_type_stack_size (csig->params [k], &align);

		/* ignore alignment for now */
		align = 1;

		frame_size += pad = (align - (frame_size & (align - 1))) & (align - 1);	
		arg_info [k].pad = pad;
		frame_size += size;
		arg_info [k + 1].pad = 0;
		arg_info [k + 1].size = size;
		offset += pad;
		arg_info [k + 1].offset = offset;
		offset += size;
	}

	align = MONO_ARCH_FRAME_ALIGNMENT;
	frame_size += pad = (align - (frame_size & (align - 1))) & (align - 1);
	arg_info [k].pad = pad;

	return frame_size;
}

/*
 * Initialize the cpu to execute managed code.
 */
void
mono_arch_cpu_init (void)
{
}

/*
 * This function returns the optimizations supported on this cpu.
 */
guint32
mono_arch_cpu_optimizazions (guint32 *exclude_mask)
{
	guint32 opts = 0;

	/* no arm-specific optimizations yet */
	*exclude_mask = 0;
	return opts;
}

static gboolean
is_regsize_var (MonoType *t) {
	if (t->byref)
		return TRUE;
	t = mono_type_get_underlying_type (t);
	switch (t->type) {
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		return TRUE;
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
		return TRUE;
	case MONO_TYPE_VALUETYPE:
		return FALSE;
	}
	return FALSE;
}

GList *
mono_arch_get_allocatable_int_vars (MonoCompile *cfg)
{
	GList *vars = NULL;
	int i;

	for (i = 0; i < cfg->num_varinfo; i++) {
		MonoInst *ins = cfg->varinfo [i];
		MonoMethodVar *vmv = MONO_VARINFO (cfg, i);

		/* unused vars */
		if (vmv->range.first_use.abs_pos >= vmv->range.last_use.abs_pos)
			continue;

		if (ins->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT) || (ins->opcode != OP_LOCAL && ins->opcode != OP_ARG))
			continue;

		/* we can only allocate 32 bit values */
		if (is_regsize_var (ins->inst_vtype)) {
			g_assert (MONO_VARINFO (cfg, i)->reg == -1);
			g_assert (i == vmv->idx);
			vars = mono_varlist_insert_sorted (cfg, vars, vmv, FALSE);
		}
	}

	return vars;
}

#define USE_EXTRA_TEMPS 0

GList *
mono_arch_get_global_int_regs (MonoCompile *cfg)
{
	GList *regs = NULL;
	regs = g_list_prepend (regs, GUINT_TO_POINTER (ARMREG_V1));
	regs = g_list_prepend (regs, GUINT_TO_POINTER (ARMREG_V2));
	regs = g_list_prepend (regs, GUINT_TO_POINTER (ARMREG_V3));
	regs = g_list_prepend (regs, GUINT_TO_POINTER (ARMREG_V4));
	regs = g_list_prepend (regs, GUINT_TO_POINTER (ARMREG_V5));
	/*regs = g_list_prepend (regs, GUINT_TO_POINTER (ARMREG_V6));*/
	/*regs = g_list_prepend (regs, GUINT_TO_POINTER (ARMREG_V7));*/

	return regs;
}

/*
 * mono_arch_regalloc_cost:
 *
 *  Return the cost, in number of memory references, of the action of 
 * allocating the variable VMV into a register during global register
 * allocation.
 */
guint32
mono_arch_regalloc_cost (MonoCompile *cfg, MonoMethodVar *vmv)
{
	/* FIXME: */
	return 2;
}

void
mono_arch_flush_icache (guint8 *code, gint size)
{
	__asm __volatile ("mov r0, %0\n"
			"mov r1, %1\n"
			"mov r2, %2\n"
			"swi 0x9f0002       @ sys_cacheflush"
			: /* no outputs */
			: "r" (code), "r" (code + size), "r" (0)
			: "r0", "r1", "r3" );

}

#define NOT_IMPLEMENTED(x) \
                g_error ("FIXME: %s is not yet implemented. (trampoline)", x);

enum {
	RegTypeGeneral,
	RegTypeBase,
	RegTypeFP,
	RegTypeStructByVal,
	RegTypeStructByAddr
};

typedef struct {
	gint32  offset;
	guint16 vtsize; /* in param area */
	guint8  reg;
	guint8  regtype : 4; /* 0 general, 1 basereg, 2 floating point register, see RegType* */
	guint8  size    : 4; /* 1, 2, 4, 8, or regs used by RegTypeStructByVal */
} ArgInfo;

typedef struct {
	int nargs;
	guint32 stack_usage;
	guint32 struct_ret;
	ArgInfo ret;
	ArgInfo sig_cookie;
	ArgInfo args [1];
} CallInfo;

#define DEBUG(a)

static void inline
add_general (guint *gr, guint *stack_size, ArgInfo *ainfo, gboolean simple)
{
	if (simple) {
		if (*gr > ARMREG_R3) {
			ainfo->offset = *stack_size;
			ainfo->reg = ARMREG_SP; /* in the caller */
			ainfo->regtype = RegTypeBase;
			*stack_size += 4;
		} else {
			ainfo->reg = *gr;
		}
	} else {
		if (*gr > ARMREG_R2) {
			*stack_size += 7;
			*stack_size &= ~7;
			ainfo->offset = *stack_size;
			ainfo->reg = ARMREG_SP; /* in the caller */
			ainfo->regtype = RegTypeBase;
			*stack_size += 8;
		} else {
			/*if ((*gr) & 1)
				(*gr) ++;*/
			ainfo->reg = *gr;
		}
		(*gr) ++;
	}
	(*gr) ++;
}

static CallInfo*
calculate_sizes (MonoMethodSignature *sig, gboolean is_pinvoke)
{
	guint i, gr;
	int n = sig->hasthis + sig->param_count;
	guint32 simpletype;
	guint32 stack_size = 0;
	CallInfo *cinfo = g_malloc0 (sizeof (CallInfo) + sizeof (ArgInfo) * n);

	gr = ARMREG_R0;

	/* FIXME: handle returning a struct */
	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		add_general (&gr, &stack_size, &cinfo->ret, TRUE);
		cinfo->struct_ret = ARMREG_R0;
	}

	n = 0;
	if (sig->hasthis) {
		add_general (&gr, &stack_size, cinfo->args + n, TRUE);
		n++;
	}
        DEBUG(printf("params: %d\n", sig->param_count));
	for (i = 0; i < sig->param_count; ++i) {
		if ((sig->call_convention == MONO_CALL_VARARG) && (i == sig->sentinelpos)) {
                        /* Prevent implicit arguments and sig_cookie from
			   being passed in registers */
                        gr = ARMREG_R3 + 1;
                        /* Emit the signature cookie just before the implicit arguments */
                        add_general (&gr, &stack_size, &cinfo->sig_cookie, TRUE);
                }
                DEBUG(printf("param %d: ", i));
		if (sig->params [i]->byref) {
                        DEBUG(printf("byref\n"));
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			continue;
		}
		simpletype = mono_type_get_underlying_type (sig->params [i])->type;
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
			cinfo->args [n].size = 1;
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
			cinfo->args [n].size = 2;
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
			cinfo->args [n].size = 4;
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_R4:
			cinfo->args [n].size = sizeof (gpointer);
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_TYPEDBYREF:
		case MONO_TYPE_VALUETYPE: {
			gint size;
			int align_size;
			int nwords;

			if (simpletype == MONO_TYPE_TYPEDBYREF) {
				size = sizeof (MonoTypedRef);
			} else {
				MonoClass *klass = mono_class_from_mono_type (sig->params [i]);
				if (is_pinvoke)
					size = mono_class_native_size (klass, NULL);
				else
					size = mono_class_value_size (klass, NULL);
			}
			DEBUG(printf ("load %d bytes struct\n",
				      mono_class_native_size (sig->params [i]->data.klass, NULL)));
			align_size = size;
			nwords = 0;
			align_size += (sizeof (gpointer) - 1);
			align_size &= ~(sizeof (gpointer) - 1);
			nwords = (align_size + sizeof (gpointer) -1 ) / sizeof (gpointer);
			cinfo->args [n].regtype = RegTypeStructByVal;
			/* FIXME: align gr and stack_size if needed */
			if (gr > ARMREG_R3) {
				cinfo->args [n].size = 0;
				cinfo->args [n].vtsize = nwords;
			} else {
				int rest = ARMREG_R3 - gr + 1;
				int n_in_regs = rest >= nwords? nwords: rest;
				cinfo->args [n].size = n_in_regs;
				cinfo->args [n].vtsize = nwords - n_in_regs;
				cinfo->args [n].reg = gr;
				gr += n_in_regs;
			}
			cinfo->args [n].offset = stack_size;
			/*g_print ("offset for arg %d at %d\n", n, stack_size);*/
			stack_size += nwords * sizeof (gpointer);
			n++;
			break;
		}
		case MONO_TYPE_U8:
		case MONO_TYPE_I8:
		case MONO_TYPE_R8:
			cinfo->args [n].size = 8;
			add_general (&gr, &stack_size, cinfo->args + n, FALSE);
			n++;
			break;
		default:
			g_error ("Can't trampoline 0x%x", sig->params [i]->type);
		}
	}

	{
		simpletype = mono_type_get_underlying_type (sig->ret)->type;
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_STRING:
			cinfo->ret.reg = ARMREG_R0;
			break;
		case MONO_TYPE_U8:
		case MONO_TYPE_I8:
			cinfo->ret.reg = ARMREG_R0;
			break;
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			cinfo->ret.reg = ARMREG_R0;
			/* FIXME: cinfo->ret.reg = ???;
			cinfo->ret.regtype = RegTypeFP;*/
			break;
		case MONO_TYPE_VALUETYPE:
			break;
		case MONO_TYPE_TYPEDBYREF:
		case MONO_TYPE_VOID:
			break;
		default:
			g_error ("Can't handle as return value 0x%x", sig->ret->type);
		}
	}

	/* align stack size to 8 */
	DEBUG (printf ("      stack size: %d (%d)\n", (stack_size + 15) & ~15, stack_size));
	stack_size = (stack_size + 7) & ~7;

	cinfo->stack_usage = stack_size;
	return cinfo;
}


/*
 * Set var information according to the calling convention. arm version.
 * The locals var stuff should most likely be split in another method.
 */
void
mono_arch_allocate_vars (MonoCompile *m)
{
	MonoMethodSignature *sig;
	MonoMethodHeader *header;
	MonoInst *inst;
	int i, offset, size, align, curinst;
	int frame_reg = ARMREG_FP;

	/* FIXME: this will change when we use FP as gcc does */
	m->flags |= MONO_CFG_HAS_SPILLUP;

	/* allow room for the vararg method args: void* and long/double */
	if (mono_jit_trace_calls != NULL && mono_trace_eval (m->method))
		m->param_area = MAX (m->param_area, sizeof (gpointer)*8);

	header = mono_method_get_header (m->method);

	/* 
	 * We use the frame register also for any method that has
	 * exception clauses. This way, when the handlers are called,
	 * the code will reference local variables using the frame reg instead of
	 * the stack pointer: if we had to restore the stack pointer, we'd
	 * corrupt the method frames that are already on the stack (since
	 * filters get called before stack unwinding happens) when the filter
	 * code would call any method (this also applies to finally etc.).
	 */ 
	if ((m->flags & MONO_CFG_HAS_ALLOCA) || header->num_clauses)
		frame_reg = ARMREG_FP;
	m->frame_reg = frame_reg;
	if (frame_reg != ARMREG_SP) {
		m->used_int_regs |= 1 << frame_reg;
	}

	sig = mono_method_signature (m->method);
	
	offset = 0;
	curinst = 0;
	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		m->ret->opcode = OP_REGVAR;
		m->ret->inst_c0 = ARMREG_R0;
	} else {
		/* FIXME: handle long and FP values */
		switch (mono_type_get_underlying_type (sig->ret)->type) {
		case MONO_TYPE_VOID:
			break;
		default:
			m->ret->opcode = OP_REGVAR;
			m->ret->inst_c0 = ARMREG_R0;
			break;
		}
	}
	/* local vars are at a positive offset from the stack pointer */
	/* 
	 * also note that if the function uses alloca, we use FP
	 * to point at the local variables.
	 */
	offset = 0; /* linkage area */
	/* align the offset to 16 bytes: not sure this is needed here  */
	//offset += 8 - 1;
	//offset &= ~(8 - 1);

	/* add parameter area size for called functions */
	offset += m->param_area;
	offset += 8 - 1;
	offset &= ~(8 - 1);
	if (m->flags & MONO_CFG_HAS_FPOUT)
		offset += 8;

	/* allow room to save the return value */
	if (mono_jit_trace_calls != NULL && mono_trace_eval (m->method))
		offset += 8;

	/* the MonoLMF structure is stored just below the stack pointer */

	if (sig->call_convention == MONO_CALL_VARARG) {
                m->sig_cookie = 0;
        }

	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		inst = m->ret;
		offset += sizeof(gpointer) - 1;
		offset &= ~(sizeof(gpointer) - 1);
		inst->inst_offset = offset;
		inst->opcode = OP_REGOFFSET;
		inst->inst_basereg = frame_reg;
		offset += sizeof(gpointer);
		if (sig->call_convention == MONO_CALL_VARARG)
			m->sig_cookie += sizeof (gpointer);
	}

	curinst = m->locals_start;
	for (i = curinst; i < m->num_varinfo; ++i) {
		inst = m->varinfo [i];
		if ((inst->flags & MONO_INST_IS_DEAD) || inst->opcode == OP_REGVAR)
			continue;

		/* inst->unused indicates native sized value types, this is used by the
		* pinvoke wrappers when they call functions returning structure */
		if (inst->unused && MONO_TYPE_ISSTRUCT (inst->inst_vtype) && inst->inst_vtype->type != MONO_TYPE_TYPEDBYREF)
			size = mono_class_native_size (mono_class_from_mono_type (inst->inst_vtype), &align);
		else
			size = mono_type_size (inst->inst_vtype, &align);

		/* FIXME: if a structure is misaligned, our memcpy doesn't work,
		 * since it loads/stores misaligned words, which don't do the right thing.
		 */
		if (align < 4 && size >= 4)
			align = 4;
		offset += align - 1;
		offset &= ~(align - 1);
		inst->inst_offset = offset;
		inst->opcode = OP_REGOFFSET;
		inst->inst_basereg = frame_reg;
		offset += size;
		//g_print ("allocating local %d to %d\n", i, inst->inst_offset);
	}

	curinst = 0;
	if (sig->hasthis) {
		inst = m->varinfo [curinst];
		if (inst->opcode != OP_REGVAR) {
			inst->opcode = OP_REGOFFSET;
			inst->inst_basereg = frame_reg;
			offset += sizeof (gpointer) - 1;
			offset &= ~(sizeof (gpointer) - 1);
			inst->inst_offset = offset;
			offset += sizeof (gpointer);
			if (sig->call_convention == MONO_CALL_VARARG)
				m->sig_cookie += sizeof (gpointer);
		}
		curinst++;
	}

	for (i = 0; i < sig->param_count; ++i) {
		inst = m->varinfo [curinst];
		if (inst->opcode != OP_REGVAR) {
			inst->opcode = OP_REGOFFSET;
			inst->inst_basereg = frame_reg;
			size = mono_type_size (sig->params [i], &align);
			/* FIXME: if a structure is misaligned, our memcpy doesn't work,
			 * since it loads/stores misaligned words, which don't do the right thing.
			 */
			if (align < 4 && size >= 4)
				align = 4;
			offset += align - 1;
			offset &= ~(align - 1);
			inst->inst_offset = offset;
			offset += size;
			if ((sig->call_convention == MONO_CALL_VARARG) && (i < sig->sentinelpos)) 
				m->sig_cookie += size;
		}
		curinst++;
	}

	/* align the offset to 8 bytes */
	offset += 8 - 1;
	offset &= ~(8 - 1);

	/* change sign? */
	m->stack_offset = offset;

}

/* Fixme: we need an alignment solution for enter_method and mono_arch_call_opcode,
 * currently alignment in mono_arch_call_opcode is computed without arch_get_argument_info 
 */

/* 
 * take the arguments and generate the arch-specific
 * instructions to properly call the function in call.
 * This includes pushing, moving arguments to the right register
 * etc.
 * Issue: who does the spilling if needed, and when?
 */
MonoCallInst*
mono_arch_call_opcode (MonoCompile *cfg, MonoBasicBlock* bb, MonoCallInst *call, int is_virtual) {
	MonoInst *arg, *in;
	MonoMethodSignature *sig;
	int i, n;
	CallInfo *cinfo;
	ArgInfo *ainfo;

	sig = call->signature;
	n = sig->param_count + sig->hasthis;
	
	cinfo = calculate_sizes (sig, sig->pinvoke);
	if (cinfo->struct_ret)
		call->used_iregs |= 1 << cinfo->struct_ret;

	for (i = 0; i < n; ++i) {
		ainfo = cinfo->args + i;
		if ((sig->call_convention == MONO_CALL_VARARG) && (i == sig->sentinelpos)) {
			MonoInst *sig_arg;
			cfg->disable_aot = TRUE;
				
			MONO_INST_NEW (cfg, sig_arg, OP_ICONST);
			sig_arg->inst_p0 = call->signature;
			
			MONO_INST_NEW (cfg, arg, OP_OUTARG);
			arg->inst_imm = cinfo->sig_cookie.offset;
			arg->inst_left = sig_arg;
			
			/* prepend, so they get reversed */
			arg->next = call->out_args;
			call->out_args = arg;
		}
		if (is_virtual && i == 0) {
			/* the argument will be attached to the call instrucion */
			in = call->args [i];
			call->used_iregs |= 1 << ainfo->reg;
		} else {
			MONO_INST_NEW (cfg, arg, OP_OUTARG);
			in = call->args [i];
			arg->cil_code = in->cil_code;
			arg->inst_left = in;
			arg->inst_right = (MonoInst*)call;
			arg->type = in->type;
			/* prepend, we'll need to reverse them later */
			arg->next = call->out_args;
			call->out_args = arg;
			if (ainfo->regtype == RegTypeGeneral) {
				arg->unused = ainfo->reg;
				call->used_iregs |= 1 << ainfo->reg;
				if (arg->type == STACK_I8)
					call->used_iregs |= 1 << (ainfo->reg + 1);
				if (arg->type == STACK_R8) {
					if (ainfo->size == 4) {
						arg->opcode = OP_OUTARG_R4;
					} else {
						call->used_iregs |= 1 << (ainfo->reg + 1);
					}
					cfg->flags |= MONO_CFG_HAS_FPOUT;
				}
			} else if (ainfo->regtype == RegTypeStructByAddr) {
				/* FIXME: where si the data allocated? */
				arg->unused = ainfo->reg;
				call->used_iregs |= 1 << ainfo->reg;
				g_assert_not_reached ();
			} else if (ainfo->regtype == RegTypeStructByVal) {
				int cur_reg;
				/* mark the used regs */
				for (cur_reg = 0; cur_reg < ainfo->size; ++cur_reg) {
					call->used_iregs |= 1 << (ainfo->reg + cur_reg);
				}
				arg->opcode = OP_OUTARG_VT;
				/* vtsize and offset have just 12 bits of encoding in number of words */
				g_assert (((ainfo->vtsize | (ainfo->offset / 4)) & 0xfffff000) == 0);
				arg->unused = ainfo->reg | (ainfo->size << 4) | (ainfo->vtsize << 8) | ((ainfo->offset / 4) << 20);
			} else if (ainfo->regtype == RegTypeBase) {
				arg->opcode = OP_OUTARG_MEMBASE;
				arg->unused = (ainfo->offset << 8) | ainfo->size;
			} else if (ainfo->regtype == RegTypeFP) {
				arg->unused = ainfo->reg;
				/* FPA args are passed in int regs */
				call->used_iregs |= 1 << ainfo->reg;
				if (ainfo->size == 8) {
					arg->opcode = OP_OUTARG_R8;
					call->used_iregs |= 1 << (ainfo->reg + 1);
				} else {
					arg->opcode = OP_OUTARG_R4;
				}
				cfg->flags |= MONO_CFG_HAS_FPOUT;
			} else {
				g_assert_not_reached ();
			}
		}
	}
	/*
	 * Reverse the call->out_args list.
	 */
	{
		MonoInst *prev = NULL, *list = call->out_args, *next;
		while (list) {
			next = list->next;
			list->next = prev;
			prev = list;
			list = next;
		}
		call->out_args = prev;
	}
	call->stack_usage = cinfo->stack_usage;
	cfg->param_area = MAX (cfg->param_area, cinfo->stack_usage);
	cfg->flags |= MONO_CFG_HAS_CALLS;
	/* 
	 * should set more info in call, such as the stack space
	 * used by the args that needs to be added back to esp
	 */

	g_free (cinfo);
	return call;
}

/*
 * Allow tracing to work with this interface (with an optional argument)
 */

void*
mono_arch_instrument_prolog (MonoCompile *cfg, void *func, void *p, gboolean enable_arguments)
{
	guchar *code = p;

	code = mono_arm_emit_load_imm (code, ARMREG_R0, (guint32)cfg->method);
	ARM_MOV_REG_IMM8 (code, ARMREG_R1, 0); /* NULL ebp for now */
	code = mono_arm_emit_load_imm (code, ARMREG_R2, (guint32)func);
	ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
	ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_R2);
	return code;
}

enum {
	SAVE_NONE,
	SAVE_STRUCT,
	SAVE_ONE,
	SAVE_TWO,
	SAVE_FP
};

void*
mono_arch_instrument_epilog (MonoCompile *cfg, void *func, void *p, gboolean enable_arguments)
{
	guchar *code = p;
	int save_mode = SAVE_NONE;
	int offset;
	MonoMethod *method = cfg->method;
	int rtype = mono_type_get_underlying_type (mono_method_signature (method)->ret)->type;
	int save_offset = cfg->param_area;
	save_offset += 7;
	save_offset &= ~7;
	
	offset = code - cfg->native_code;
	/* we need about 16 instructions */
	if (offset > (cfg->code_size - 16 * 4)) {
		cfg->code_size *= 2;
		cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
		code = cfg->native_code + offset;
	}
handle_enum:
	switch (rtype) {
	case MONO_TYPE_VOID:
		/* special case string .ctor icall */
		if (strcmp (".ctor", method->name) && method->klass == mono_defaults.string_class)
			save_mode = SAVE_ONE;
		else
			save_mode = SAVE_NONE;
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		save_mode = SAVE_TWO;
		break;
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		save_mode = SAVE_FP;
		break;
	case MONO_TYPE_VALUETYPE:
		save_mode = SAVE_STRUCT;
		break;
	default:
		save_mode = SAVE_ONE;
		break;
	}

	switch (save_mode) {
	case SAVE_TWO:
		ARM_STR_IMM (code, ARMREG_R0, cfg->frame_reg, save_offset);
		ARM_STR_IMM (code, ARMREG_R1, cfg->frame_reg, save_offset + 4);
		if (enable_arguments) {
			ARM_MOV_REG_REG (code, ARMREG_R2, ARMREG_R1);
			ARM_MOV_REG_REG (code, ARMREG_R1, ARMREG_R0);
		}
		break;
	case SAVE_ONE:
		ARM_STR_IMM (code, ARMREG_R0, cfg->frame_reg, save_offset);
		if (enable_arguments) {
			ARM_MOV_REG_REG (code, ARMREG_R1, ARMREG_R0);
		}
		break;
	case SAVE_FP:
		/* FIXME: what reg?  */
		if (enable_arguments) {
			/* FIXME: what reg?  */
		}
		break;
	case SAVE_STRUCT:
		if (enable_arguments) {
			/* FIXME: get the actual address  */
			ARM_MOV_REG_REG (code, ARMREG_R1, ARMREG_R0);
		}
		break;
	case SAVE_NONE:
	default:
		break;
	}

	code = mono_arm_emit_load_imm (code, ARMREG_R0, (guint32)cfg->method);
	code = mono_arm_emit_load_imm (code, ARMREG_IP, (guint32)func);
	ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
	ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);

	switch (save_mode) {
	case SAVE_TWO:
		ARM_LDR_IMM (code, ARMREG_R0, cfg->frame_reg, save_offset);
		ARM_LDR_IMM (code, ARMREG_R1, cfg->frame_reg, save_offset + 4);
		break;
	case SAVE_ONE:
		ARM_LDR_IMM (code, ARMREG_R0, cfg->frame_reg, save_offset);
		break;
	case SAVE_FP:
		/* FIXME */
		break;
	case SAVE_NONE:
	default:
		break;
	}

	return code;
}

/*
 * The immediate field for cond branches is big enough for all reasonable methods
 */
#define EMIT_COND_BRANCH_FLAGS(ins,condcode) \
if (ins->flags & MONO_INST_BRLABEL) { \
        if (0 && ins->inst_i0->inst_c0) { \
		ARM_B_COND (code, (condcode), (code - cfg->native_code + ins->inst_i0->inst_c0) & 0xffffff);	\
        } else { \
	        mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_LABEL, ins->inst_i0); \
		ARM_B_COND (code, (condcode), 0);	\
        } \
} else { \
        if (0 && ins->inst_true_bb->native_offset) { \
		ARM_B_COND (code, (condcode), (code - cfg->native_code + ins->inst_true_bb->native_offset) & 0xffffff); \
        } else { \
		mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_BB, ins->inst_true_bb); \
		ARM_B_COND (code, (condcode), 0);	\
        } \
}

#define EMIT_COND_BRANCH(ins,cond) EMIT_COND_BRANCH_FLAGS(ins, branch_cc_table [(cond)])

/* emit an exception if condition is fail
 *
 * We assign the extra code used to throw the implicit exceptions
 * to cfg->bb_exit as far as the big branch handling is concerned
 */
#define EMIT_COND_SYSTEM_EXCEPTION_FLAGS(condcode,exc_name)            \
        do {                                                        \
		mono_add_patch_info (cfg, code - cfg->native_code,   \
				    MONO_PATCH_INFO_EXC, exc_name);  \
		ARM_BL_COND (code, (condcode), 0);	\
	} while (0); 

#define EMIT_COND_SYSTEM_EXCEPTION(cond,exc_name) EMIT_COND_SYSTEM_EXCEPTION_FLAGS(branch_cc_table [(cond)], (exc_name))

static void
peephole_pass (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *last_ins = NULL;
	ins = bb->code;

	while (ins) {

		switch (ins->opcode) {
		case OP_MUL_IMM: 
			/* remove unnecessary multiplication with 1 */
			if (ins->inst_imm == 1) {
				if (ins->dreg != ins->sreg1) {
					ins->opcode = OP_MOVE;
				} else {
					last_ins->next = ins->next;				
					ins = ins->next;				
					continue;
				}
			} else {
				int power2 = mono_is_power_of_two (ins->inst_imm);
				if (power2 > 0) {
					ins->opcode = OP_SHL_IMM;
					ins->inst_imm = power2;
				}
			}
			break;
		case OP_LOAD_MEMBASE:
		case OP_LOADI4_MEMBASE:
			/* 
			 * OP_STORE_MEMBASE_REG reg, offset(basereg) 
			 * OP_LOAD_MEMBASE offset(basereg), reg
			 */
			if (last_ins && (last_ins->opcode == OP_STOREI4_MEMBASE_REG 
					 || last_ins->opcode == OP_STORE_MEMBASE_REG) &&
			    ins->inst_basereg == last_ins->inst_destbasereg &&
			    ins->inst_offset == last_ins->inst_offset) {
				if (ins->dreg == last_ins->sreg1) {
					last_ins->next = ins->next;				
					ins = ins->next;				
					continue;
				} else {
					//static int c = 0; printf ("MATCHX %s %d\n", cfg->method->name,c++);
					ins->opcode = OP_MOVE;
					ins->sreg1 = last_ins->sreg1;
				}

			/* 
			 * Note: reg1 must be different from the basereg in the second load
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
					last_ins->next = ins->next;				
					ins = ins->next;				
					continue;
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
				//static int c = 0; printf ("MATCHX %s %d\n", cfg->method->name,c++);
				ins->opcode = OP_ICONST;
				ins->inst_c0 = last_ins->inst_imm;
				g_assert_not_reached (); // check this rule
#endif
			}
			break;
		case OP_LOADU1_MEMBASE:
		case OP_LOADI1_MEMBASE:
			if (last_ins && (last_ins->opcode == OP_STOREI1_MEMBASE_REG) &&
					ins->inst_basereg == last_ins->inst_destbasereg &&
					ins->inst_offset == last_ins->inst_offset) {
				if (ins->dreg == last_ins->sreg1) {
					last_ins->next = ins->next;				
					ins = ins->next;				
					continue;
				} else {
					//static int c = 0; printf ("MATCHX %s %d\n", cfg->method->name,c++);
					ins->opcode = OP_MOVE;
					ins->sreg1 = last_ins->sreg1;
				}
			}
			break;
		case OP_LOADU2_MEMBASE:
		case OP_LOADI2_MEMBASE:
			if (last_ins && (last_ins->opcode == OP_STOREI2_MEMBASE_REG) &&
					ins->inst_basereg == last_ins->inst_destbasereg &&
					ins->inst_offset == last_ins->inst_offset) {
				if (ins->dreg == last_ins->sreg1) {
					last_ins->next = ins->next;				
					ins = ins->next;				
					continue;
				} else {
					//static int c = 0; printf ("MATCHX %s %d\n", cfg->method->name,c++);
					ins->opcode = OP_MOVE;
					ins->sreg1 = last_ins->sreg1;
				}
			}
			break;
		case CEE_CONV_I4:
		case CEE_CONV_U4:
		case OP_MOVE:
		case OP_SETREG:
			ins->opcode = OP_MOVE;
			/* 
			 * OP_MOVE reg, reg 
			 */
			if (ins->dreg == ins->sreg1) {
				if (last_ins)
					last_ins->next = ins->next;				
				ins = ins->next;
				continue;
			}
			/* 
			 * OP_MOVE sreg, dreg 
			 * OP_MOVE dreg, sreg
			 */
			if (last_ins && last_ins->opcode == OP_MOVE &&
			    ins->sreg1 == last_ins->dreg &&
			    ins->dreg == last_ins->sreg1) {
				last_ins->next = ins->next;				
				ins = ins->next;				
				continue;
			}
			break;
		}
		last_ins = ins;
		ins = ins->next;
	}
	bb->last_ins = last_ins;
}

/* 
 * the branch_cc_table should maintain the order of these
 * opcodes.
case CEE_BEQ:
case CEE_BGE:
case CEE_BGT:
case CEE_BLE:
case CEE_BLT:
case CEE_BNE_UN:
case CEE_BGE_UN:
case CEE_BGT_UN:
case CEE_BLE_UN:
case CEE_BLT_UN:
 */
static const guchar 
branch_cc_table [] = {
	ARMCOND_EQ, 
	ARMCOND_GE, 
	ARMCOND_GT, 
	ARMCOND_LE,
	ARMCOND_LT, 
	
	ARMCOND_NE, 
	ARMCOND_HS, 
	ARMCOND_HI, 
	ARMCOND_LS,
	ARMCOND_LO
};


static void
insert_after_ins (MonoBasicBlock *bb, MonoInst *ins, MonoInst *to_insert)
{
	if (ins == NULL) {
		ins = bb->code;
		bb->code = to_insert;
		to_insert->next = ins;
	} else {
		to_insert->next = ins->next;
		ins->next = to_insert;
	}
}

#define NEW_INS(cfg,dest,op) do {       \
		(dest) = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoInst));       \
		(dest)->opcode = (op);  \
		insert_after_ins (bb, last_ins, (dest)); \
	} while (0)

static int
map_to_reg_reg_op (int op)
{
	switch (op) {
	case OP_ADD_IMM:
		return CEE_ADD;
	case OP_SUB_IMM:
		return CEE_SUB;
	case OP_AND_IMM:
		return CEE_AND;
	case OP_COMPARE_IMM:
		return OP_COMPARE;
	case OP_ADDCC_IMM:
		return OP_ADDCC;
	case OP_ADC_IMM:
		return OP_ADC;
	case OP_SUBCC_IMM:
		return OP_SUBCC;
	case OP_SBB_IMM:
		return OP_SBB;
	case OP_OR_IMM:
		return CEE_OR;
	case OP_XOR_IMM:
		return CEE_XOR;
	case OP_LOAD_MEMBASE:
		return OP_LOAD_MEMINDEX;
	case OP_LOADI4_MEMBASE:
		return OP_LOADI4_MEMINDEX;
	case OP_LOADU4_MEMBASE:
		return OP_LOADU4_MEMINDEX;
	case OP_LOADU1_MEMBASE:
		return OP_LOADU1_MEMINDEX;
	case OP_LOADI2_MEMBASE:
		return OP_LOADI2_MEMINDEX;
	case OP_LOADU2_MEMBASE:
		return OP_LOADU2_MEMINDEX;
	case OP_LOADI1_MEMBASE:
		return OP_LOADI1_MEMINDEX;
	case OP_STOREI1_MEMBASE_REG:
		return OP_STOREI1_MEMINDEX;
	case OP_STOREI2_MEMBASE_REG:
		return OP_STOREI2_MEMINDEX;
	case OP_STOREI4_MEMBASE_REG:
		return OP_STOREI4_MEMINDEX;
	case OP_STORE_MEMBASE_REG:
		return OP_STORE_MEMINDEX;
	case OP_STORER4_MEMBASE_REG:
		return OP_STORER4_MEMINDEX;
	case OP_STORER8_MEMBASE_REG:
		return OP_STORER8_MEMINDEX;
	case OP_STORE_MEMBASE_IMM:
		return OP_STORE_MEMBASE_REG;
	case OP_STOREI1_MEMBASE_IMM:
		return OP_STOREI1_MEMBASE_REG;
	case OP_STOREI2_MEMBASE_IMM:
		return OP_STOREI2_MEMBASE_REG;
	case OP_STOREI4_MEMBASE_IMM:
		return OP_STOREI4_MEMBASE_REG;
	}
	g_assert_not_reached ();
}

/*
 * Remove from the instruction list the instructions that can't be
 * represented with very simple instructions with no register
 * requirements.
 */
static void
mono_arch_lowering_pass (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *next, *temp, *last_ins = NULL;
	int rot_amount, imm8, low_imm;

	/* setup the virtual reg allocator */
	if (bb->max_ireg > cfg->rs->next_vireg)
		cfg->rs->next_vireg = bb->max_ireg;

	ins = bb->code;
	while (ins) {
loop_start:
		switch (ins->opcode) {
		case OP_ADD_IMM:
		case OP_SUB_IMM:
		case OP_AND_IMM:
		case OP_COMPARE_IMM:
		case OP_ADDCC_IMM:
		case OP_ADC_IMM:
		case OP_SUBCC_IMM:
		case OP_SBB_IMM:
		case OP_OR_IMM:
		case OP_XOR_IMM:
			if ((imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount)) < 0) {
				NEW_INS (cfg, temp, OP_ICONST);
				temp->inst_c0 = ins->inst_imm;
				temp->dreg = mono_regstate_next_int (cfg->rs);
				ins->sreg2 = temp->dreg;
				ins->opcode = map_to_reg_reg_op (ins->opcode);
			}
			break;
		case OP_MUL_IMM:
			if (ins->inst_imm == 1) {
				ins->opcode = OP_MOVE;
				break;
			}
			if (ins->inst_imm == 0) {
				ins->opcode = OP_ICONST;
				ins->inst_c0 = 0;
				break;
			}
			imm8 = mono_is_power_of_two (ins->inst_imm);
			if (imm8 > 0) {
				ins->opcode = OP_SHL_IMM;
				ins->inst_imm = imm8;
				break;
			}
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_imm;
			temp->dreg = mono_regstate_next_int (cfg->rs);
			ins->sreg2 = temp->dreg;
			ins->opcode = CEE_MUL;
			break;
		case OP_LOAD_MEMBASE:
		case OP_LOADI4_MEMBASE:
		case OP_LOADU4_MEMBASE:
		case OP_LOADU1_MEMBASE:
			/* we can do two things: load the immed in a register
			 * and use an indexed load, or see if the immed can be
			 * represented as an ad_imm + a load with a smaller offset
			 * that fits. We just do the first for now, optimize later.
			 */
			if (arm_is_imm12 (ins->inst_offset))
				break;
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_offset;
			temp->dreg = mono_regstate_next_int (cfg->rs);
			ins->sreg2 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			break;
		case OP_LOADI2_MEMBASE:
		case OP_LOADU2_MEMBASE:
		case OP_LOADI1_MEMBASE:
			if (arm_is_imm8 (ins->inst_offset))
				break;
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_offset;
			temp->dreg = mono_regstate_next_int (cfg->rs);
			ins->sreg2 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			break;
		case OP_LOADR4_MEMBASE:
		case OP_LOADR8_MEMBASE:
			if (arm_is_fpimm8 (ins->inst_offset))
				break;
			low_imm = ins->inst_offset & 0x1ff;
			if ((imm8 = mono_arm_is_rotated_imm8 (ins->inst_offset & ~0x1ff, &rot_amount)) >= 0) {
				NEW_INS (cfg, temp, OP_ADD_IMM);
				temp->inst_imm = ins->inst_offset & ~0x1ff;
				temp->sreg1 = ins->inst_basereg;
				temp->dreg = mono_regstate_next_int (cfg->rs);
				ins->inst_basereg = temp->dreg;
				ins->inst_offset = low_imm;
				break;
			}
			/* FPA doesn't have indexed load instructions */
			g_assert_not_reached ();
			break;
		case OP_STORE_MEMBASE_REG:
		case OP_STOREI4_MEMBASE_REG:
		case OP_STOREI1_MEMBASE_REG:
			if (arm_is_imm12 (ins->inst_offset))
				break;
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_offset;
			temp->dreg = mono_regstate_next_int (cfg->rs);
			ins->sreg2 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			break;
		case OP_STOREI2_MEMBASE_REG:
			if (arm_is_imm8 (ins->inst_offset))
				break;
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_offset;
			temp->dreg = mono_regstate_next_int (cfg->rs);
			ins->sreg2 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			break;
		case OP_STORER4_MEMBASE_REG:
		case OP_STORER8_MEMBASE_REG:
			if (arm_is_fpimm8 (ins->inst_offset))
				break;
			low_imm = ins->inst_offset & 0x1ff;
			if ((imm8 = mono_arm_is_rotated_imm8 (ins->inst_offset & ~ 0x1ff, &rot_amount)) >= 0 && arm_is_fpimm8 (low_imm)) {
				NEW_INS (cfg, temp, OP_ADD_IMM);
				temp->inst_imm = ins->inst_offset & ~0x1ff;
				temp->sreg1 = ins->inst_destbasereg;
				temp->dreg = mono_regstate_next_int (cfg->rs);
				ins->inst_destbasereg = temp->dreg;
				ins->inst_offset = low_imm;
				break;
			}
			/*g_print ("fail with: %d (%d, %d)\n", ins->inst_offset, ins->inst_offset & ~0x1ff, low_imm);*/
			/* FPA doesn't have indexed store instructions */
			g_assert_not_reached ();
			break;
		case OP_STORE_MEMBASE_IMM:
		case OP_STOREI1_MEMBASE_IMM:
		case OP_STOREI2_MEMBASE_IMM:
		case OP_STOREI4_MEMBASE_IMM:
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_imm;
			temp->dreg = mono_regstate_next_int (cfg->rs);
			ins->sreg1 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			last_ins = temp;
			goto loop_start; /* make it handle the possibly big ins->inst_offset */
		}
		last_ins = ins;
		ins = ins->next;
	}
	bb->last_ins = last_ins;
	bb->max_ireg = cfg->rs->next_vireg;

}

void
mono_arch_local_regalloc (MonoCompile *cfg, MonoBasicBlock *bb)
{
	if (!bb->code)
		return;
	mono_arch_lowering_pass (cfg, bb);
	mono_local_regalloc (cfg, bb);
}

static guchar*
emit_float_to_int (MonoCompile *cfg, guchar *code, int dreg, int sreg, int size, gboolean is_signed)
{
	/* sreg is a float, dreg is an integer reg  */
	ARM_FIXZ (code, dreg, sreg);
	if (!is_signed) {
		if (size == 1)
			ARM_AND_REG_IMM8 (code, dreg, dreg, 0xff);
		else if (size == 2) {
			ARM_SHL_IMM (code, dreg, dreg, 16);
			ARM_SHR_IMM (code, dreg, dreg, 16);
		}
	} else {
		if (size == 1) {
			ARM_SHL_IMM (code, dreg, dreg, 24);
			ARM_SAR_IMM (code, dreg, dreg, 24);
		} else if (size == 2) {
			ARM_SHL_IMM (code, dreg, dreg, 16);
			ARM_SAR_IMM (code, dreg, dreg, 16);
		}
	}
	return code;
}

typedef struct {
	guchar *code;
	const guchar *target;
	int absolute;
	int found;
} PatchData;

#define is_call_imm(diff) ((gint)(diff) >= -33554432 && (gint)(diff) <= 33554431)

static int
search_thunk_slot (void *data, int csize, int bsize, void *user_data) {
	PatchData *pdata = (PatchData*)user_data;
	guchar *code = data;
	guint32 *thunks = data;
	guint32 *endthunks = (guint32*)(code + bsize);
	int i, count = 0;
	int difflow, diffhigh;

	/* always ensure a call from pdata->code can reach to the thunks without further thunks */
	difflow = (char*)pdata->code - (char*)thunks;
	diffhigh = (char*)pdata->code - (char*)endthunks;
	if (!((is_call_imm (thunks) && is_call_imm (endthunks)) || (is_call_imm (difflow) && is_call_imm (diffhigh))))
		return 0;

	/*
	 * The thunk is composed of 3 words:
	 * load constant from thunks [2] into ARM_IP
	 * bx to ARM_IP
	 * address constant
	 * Note that the LR register is already setup
	 */
	//g_print ("thunk nentries: %d\n", ((char*)endthunks - (char*)thunks)/16);
	if ((pdata->found == 2) || (pdata->code >= code && pdata->code <= code + csize)) {
		while (thunks < endthunks) {
			//g_print ("looking for target: %p at %p (%08x-%08x)\n", pdata->target, thunks, thunks [0], thunks [1]);
			if (thunks [2] == (guint32)pdata->target) {
				arm_patch (pdata->code, (guchar*)thunks);
				mono_arch_flush_icache (pdata->code, 4);
				pdata->found = 1;
				return 1;
			} else if ((thunks [0] == 0) && (thunks [1] == 0) && (thunks [2] == 0)) {
				/* found a free slot instead: emit thunk */
				code = (guchar*)thunks;
				ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 0);
				ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);
				thunks [2] = (guint32)pdata->target;
				mono_arch_flush_icache ((guchar*)thunks, 12);

				arm_patch (pdata->code, (guchar*)thunks);
				mono_arch_flush_icache (pdata->code, 4);
				pdata->found = 1;
				return 1;
			}
			/* skip 12 bytes, the size of the thunk */
			thunks += 3;
			count++;
		}
		//g_print ("failed thunk lookup for %p from %p at %p (%d entries)\n", pdata->target, pdata->code, data, count);
	}
	return 0;
}

static void
handle_thunk (int absolute, guchar *code, const guchar *target) {
	MonoDomain *domain = mono_domain_get ();
	PatchData pdata;

	pdata.code = code;
	pdata.target = target;
	pdata.absolute = absolute;
	pdata.found = 0;

	mono_domain_lock (domain);
	mono_code_manager_foreach (domain->code_mp, search_thunk_slot, &pdata);

	if (!pdata.found) {
		/* this uses the first available slot */
		pdata.found = 2;
		mono_code_manager_foreach (domain->code_mp, search_thunk_slot, &pdata);
	}
	mono_domain_unlock (domain);

	if (pdata.found != 1)
		g_print ("thunk failed for %p from %p\n", target, code);
	g_assert (pdata.found == 1);
}

void
arm_patch (guchar *code, const guchar *target)
{
	guint32 ins = *(guint32*)code;
	guint32 prim = (ins >> 25) & 7;
	guint32 ovf;

	//g_print ("patching 0x%08x (0x%08x) to point to 0x%08x\n", code, ins, target);
	if (prim == 5) { /* 101b */
		/* the diff starts 8 bytes from the branch opcode */
		gint diff = target - code - 8;
		if (diff >= 0) {
			if (diff <= 33554431) {
				diff >>= 2;
				ins = (ins & 0xff000000) | diff;
				*(guint32*)code = ins;
				return;
			}
		} else {
			/* diff between 0 and -33554432 */
			if (diff >= -33554432) {
				diff >>= 2;
				ins = (ins & 0xff000000) | (diff & ~0xff000000);
				*(guint32*)code = ins;
				return;
			}
		}
		
		handle_thunk (TRUE, code, target);
		return;
	}


	if ((ins & 0x0ffffff0) == 0x12fff10) {
		/* branch and exchange: the address is constructed in a reg */
		g_assert_not_reached ();
	} else {
		g_assert_not_reached ();
	}
//	g_print ("patched with 0x%08x\n", ins);
}

/* 
 * Return the >= 0 uimm8 value if val can be represented with a byte + rotation
 * (with the rotation amount in *rot_amount. rot_amount is already adjusted
 * to be used with the emit macros.
 * Return -1 otherwise.
 */
int
mono_arm_is_rotated_imm8 (guint32 val, gint *rot_amount)
{
	guint32 res, i;
	for (i = 0; i < 31; i+= 2) {
		res = (val << (32 - i)) | (val >> i);
		if (res & ~0xff)
			continue;
		*rot_amount = i? 32 - i: 0;
		return res;
	}
	return -1;
}

/*
 * Emits in code a sequence of instructions that load the value 'val'
 * into the dreg register. Uses at most 4 instructions.
 */
guint8*
mono_arm_emit_load_imm (guint8 *code, int dreg, guint32 val)
{
	int imm8, rot_amount;
#if 0
	ARM_LDR_IMM (code, dreg, ARMREG_PC, 0);
	/* skip the constant pool */
	ARM_B (code, 0);
	*(int*)code = val;
	code += 4;
	return code;
#endif
	if ((imm8 = mono_arm_is_rotated_imm8 (val, &rot_amount)) >= 0) {
		ARM_MOV_REG_IMM (code, dreg, imm8, rot_amount);
	} else if ((imm8 = mono_arm_is_rotated_imm8 (~val, &rot_amount)) >= 0) {
		ARM_MVN_REG_IMM (code, dreg, imm8, rot_amount);
	} else {
		if (val & 0xFF) {
			ARM_MOV_REG_IMM8 (code, dreg, (val & 0xFF));
			if (val & 0xFF00) {
				ARM_ADD_REG_IMM (code, dreg, dreg, (val & 0xFF00) >> 8, 24);
			}
			if (val & 0xFF0000) {
				ARM_ADD_REG_IMM (code, dreg, dreg, (val & 0xFF0000) >> 16, 16);
			}
			if (val & 0xFF000000) {
				ARM_ADD_REG_IMM (code, dreg, dreg, (val & 0xFF000000) >> 24, 8);
			}
		} else if (val & 0xFF00) {
			ARM_MOV_REG_IMM (code, dreg, (val & 0xFF00) >> 8, 24);
			if (val & 0xFF0000) {
				ARM_ADD_REG_IMM (code, dreg, dreg, (val & 0xFF0000) >> 16, 16);
			}
			if (val & 0xFF000000) {
				ARM_ADD_REG_IMM (code, dreg, dreg, (val & 0xFF000000) >> 24, 8);
			}
		} else if (val & 0xFF0000) {
			ARM_MOV_REG_IMM (code, dreg, (val & 0xFF0000) >> 16, 16);
			if (val & 0xFF000000) {
				ARM_ADD_REG_IMM (code, dreg, dreg, (val & 0xFF000000) >> 24, 8);
			}
		}
		//g_assert_not_reached ();
	}
	return code;
}

void
mono_arch_output_basic_block (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins;
	MonoCallInst *call;
	guint offset;
	guint8 *code = cfg->native_code + cfg->code_len;
	MonoInst *last_ins = NULL;
	guint last_offset = 0;
	int max_len, cpos;
	int imm8, rot_amount;

	if (cfg->opt & MONO_OPT_PEEPHOLE)
		peephole_pass (cfg, bb);

	/* we don't align basic blocks of loops on arm */

	if (cfg->verbose_level > 2)
		g_print ("Basic block %d starting at offset 0x%x\n", bb->block_num, bb->native_offset);

	cpos = bb->max_offset;

	if (cfg->prof_options & MONO_PROFILE_COVERAGE) {
		//MonoCoverageInfo *cov = mono_get_coverage_info (cfg->method);
		//g_assert (!mono_compile_aot);
		//cpos += 6;
		//if (bb->cil_code)
		//	cov->data [bb->dfn].iloffset = bb->cil_code - cfg->cil_code;
		/* this is not thread save, but good enough */
		/* fixme: howto handle overflows? */
		//x86_inc_mem (code, &cov->data [bb->dfn].count); 
	}

	ins = bb->code;
	while (ins) {
		offset = code - cfg->native_code;

		max_len = ((guint8 *)arm_cpu_desc [ins->opcode])[MONO_INST_LEN];

		if (offset > (cfg->code_size - max_len - 16)) {
			cfg->code_size *= 2;
			cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
			code = cfg->native_code + offset;
		}
	//	if (ins->cil_code)
	//		g_print ("cil code\n");
		mono_debug_record_line_number (cfg, ins, offset);

		switch (ins->opcode) {
		case OP_TLS_GET:
			g_assert_not_reached ();
			break;
		/*case OP_BIGMUL:
			ppc_mullw (code, ppc_r4, ins->sreg1, ins->sreg2);
			ppc_mulhw (code, ppc_r3, ins->sreg1, ins->sreg2);
			break;
		case OP_BIGMUL_UN:
			ppc_mullw (code, ppc_r4, ins->sreg1, ins->sreg2);
			ppc_mulhwu (code, ppc_r3, ins->sreg1, ins->sreg2);
			break;*/
		case OP_STOREI1_MEMBASE_IMM:
			code = mono_arm_emit_load_imm (code, ARMREG_LR, ins->inst_imm & 0xFF);
			g_assert (arm_is_imm12 (ins->inst_offset));
			ARM_STRB_IMM (code, ARMREG_LR, ins->inst_destbasereg, ins->inst_offset);
			break;
		case OP_STOREI2_MEMBASE_IMM:
			code = mono_arm_emit_load_imm (code, ARMREG_LR, ins->inst_imm & 0xFFFF);
			g_assert (arm_is_imm8 (ins->inst_offset));
			ARM_STRH_IMM (code, ARMREG_LR, ins->inst_destbasereg, ins->inst_offset);
			break;
		case OP_STORE_MEMBASE_IMM:
		case OP_STOREI4_MEMBASE_IMM:
			code = mono_arm_emit_load_imm (code, ARMREG_LR, ins->inst_imm);
			g_assert (arm_is_imm12 (ins->inst_offset));
			ARM_STR_IMM (code, ARMREG_LR, ins->inst_destbasereg, ins->inst_offset);
			break;
		case OP_STOREI1_MEMBASE_REG:
			g_assert (arm_is_imm12 (ins->inst_offset));
			ARM_STRB_IMM (code, ins->sreg1, ins->inst_destbasereg, ins->inst_offset);
			break;
		case OP_STOREI2_MEMBASE_REG:
			g_assert (arm_is_imm8 (ins->inst_offset));
			ARM_STRH_IMM (code, ins->sreg1, ins->inst_destbasereg, ins->inst_offset);
			break;
		case OP_STORE_MEMBASE_REG:
		case OP_STOREI4_MEMBASE_REG:
			/* this case is special, since it happens for spill code after lowering has been called */
			if (arm_is_imm12 (ins->inst_offset)) {
				ARM_STR_IMM (code, ins->sreg1, ins->inst_destbasereg, ins->inst_offset);
			} else {
				code = mono_arm_emit_load_imm (code, ARMREG_LR, ins->inst_offset);
				ARM_STR_REG_REG (code, ins->sreg1, ins->inst_destbasereg, ARMREG_LR);
			}
			break;
		case OP_STOREI1_MEMINDEX:
			ARM_STRB_REG_REG (code, ins->sreg1, ins->inst_destbasereg, ins->sreg2);
			break;
		case OP_STOREI2_MEMINDEX:
			/* note: the args are reversed in the macro */
			ARM_STRH_REG_REG (code, ins->inst_destbasereg, ins->sreg1, ins->sreg2);
			break;
		case OP_STORE_MEMINDEX:
		case OP_STOREI4_MEMINDEX:
			ARM_STR_REG_REG (code, ins->sreg1, ins->inst_destbasereg, ins->sreg2);
			break;
		case CEE_LDIND_I:
		case CEE_LDIND_I4:
		case CEE_LDIND_U4:
			g_assert_not_reached ();
			break;
		case OP_LOADU4_MEM:
			g_assert_not_reached ();
			break;
		case OP_LOAD_MEMINDEX:
		case OP_LOADI4_MEMINDEX:
		case OP_LOADU4_MEMINDEX:
			ARM_LDR_REG_REG (code, ins->dreg, ins->inst_basereg, ins->sreg2);
			break;
		case OP_LOADI1_MEMINDEX:
			ARM_LDRSB_REG_REG (code, ins->dreg, ins->inst_basereg, ins->sreg2);
			break;
		case OP_LOADU1_MEMINDEX:
			ARM_LDRB_REG_REG (code, ins->dreg, ins->inst_basereg, ins->sreg2);
			break;
		case OP_LOADI2_MEMINDEX:
			ARM_LDRSH_REG_REG (code, ins->dreg, ins->inst_basereg, ins->sreg2);
			break;
		case OP_LOADU2_MEMINDEX:
			ARM_LDRH_REG_REG (code, ins->dreg, ins->inst_basereg, ins->sreg2);
			break;
		case OP_LOAD_MEMBASE:
		case OP_LOADI4_MEMBASE:
		case OP_LOADU4_MEMBASE:
			/* this case is special, since it happens for spill code after lowering has been called */
			if (arm_is_imm12 (ins->inst_offset)) {
				ARM_LDR_IMM (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			} else {
				code = mono_arm_emit_load_imm (code, ARMREG_LR, ins->inst_offset);
				ARM_LDR_REG_REG (code, ins->dreg, ins->inst_basereg, ARMREG_LR);
			}
			break;
		case OP_LOADI1_MEMBASE:
			g_assert (arm_is_imm8 (ins->inst_offset));
			ARM_LDRSB_IMM (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case OP_LOADU1_MEMBASE:
			g_assert (arm_is_imm12 (ins->inst_offset));
			ARM_LDRB_IMM (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case OP_LOADU2_MEMBASE:
			g_assert (arm_is_imm8 (ins->inst_offset));
			ARM_LDRH_IMM (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case OP_LOADI2_MEMBASE:
			g_assert (arm_is_imm8 (ins->inst_offset));
			ARM_LDRSH_IMM (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case CEE_CONV_I1:
			ARM_SHL_IMM (code, ins->dreg, ins->sreg1, 24);
			ARM_SAR_IMM (code, ins->dreg, ins->dreg, 24);
			break;
		case CEE_CONV_I2:
			ARM_SHL_IMM (code, ins->dreg, ins->sreg1, 16);
			ARM_SAR_IMM (code, ins->dreg, ins->dreg, 16);
			break;
		case CEE_CONV_U1:
			ARM_AND_REG_IMM8 (code, ins->dreg, ins->sreg1, 0xff);
			break;
		case CEE_CONV_U2:
			ARM_SHL_IMM (code, ins->dreg, ins->sreg1, 16);
			ARM_SHR_IMM (code, ins->dreg, ins->dreg, 16);
			break;
		case OP_COMPARE:
			ARM_CMP_REG_REG (code, ins->sreg1, ins->sreg2);
			break;
		case OP_COMPARE_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_CMP_REG_IMM (code, ins->sreg1, imm8, rot_amount);
			break;
		case OP_X86_TEST_NULL:
			g_assert_not_reached ();
			break;
		case CEE_BREAK:
			*(int*)code = 0xe7f001f0;
			*(int*)code = 0xef9f0001;
			code += 4;
			//ARM_DBRK (code);
			break;
		case OP_ADDCC:
			ARM_ADDS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case CEE_ADD:
			ARM_ADD_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_ADC:
			ARM_ADCS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_ADDCC_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_ADDS_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case OP_ADD_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_ADD_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case OP_ADC_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_ADCS_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case CEE_ADD_OVF:
			ARM_ADD_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case CEE_ADD_OVF_UN:
			ARM_ADD_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case CEE_SUB_OVF:
			ARM_SUB_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case CEE_SUB_OVF_UN:
			ARM_SUB_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_TRUE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_ADD_OVF_CARRY:
			ARM_ADCS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_ADD_OVF_UN_CARRY:
			ARM_ADCS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_SUB_OVF_CARRY:
			ARM_SBCS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_SUB_OVF_UN_CARRY:
			ARM_SBCS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_TRUE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_SUBCC:
			ARM_SUBS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_SUBCC_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_SUBS_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case CEE_SUB:
			ARM_SUB_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_SBB:
			ARM_SBCS_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_SUB_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_SUB_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case OP_SBB_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_SBCS_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case OP_ARM_RSBS_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_RSBS_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case OP_ARM_RSC_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_RSC_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case CEE_AND:
			ARM_AND_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_AND_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_AND_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case CEE_DIV:
		case CEE_DIV_UN:
		case OP_DIV_IMM:
		case CEE_REM:
		case CEE_REM_UN:
		case OP_REM_IMM:
			/* crappy ARM arch doesn't have a DIV instruction */
			g_assert_not_reached ();
		case CEE_OR:
			ARM_ORR_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_OR_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_ORR_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case CEE_XOR:
			ARM_EOR_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_XOR_IMM:
			imm8 = mono_arm_is_rotated_imm8 (ins->inst_imm, &rot_amount);
			g_assert (imm8 >= 0);
			ARM_EOR_REG_IMM (code, ins->dreg, ins->sreg1, imm8, rot_amount);
			break;
		case CEE_SHL:
			ARM_SHL_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_SHL_IMM:
			if (ins->inst_imm)
				ARM_SHL_IMM (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x1f));
			break;
		case CEE_SHR:
			ARM_SAR_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_SHR_IMM:
			if (ins->inst_imm)
				ARM_SAR_IMM (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x1f));
			break;
		case OP_SHR_UN_IMM:
			if (ins->inst_imm)
				ARM_SHR_IMM (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x1f));
			break;
		case CEE_SHR_UN:
			ARM_SHR_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case CEE_NOT:
			ARM_MVN_REG_REG (code, ins->dreg, ins->sreg1);
			break;
		case CEE_NEG:
			ARM_RSB_REG_IMM8 (code, ins->dreg, ins->sreg1, 0);
			break;
		case CEE_MUL:
			if (ins->dreg == ins->sreg2)
				ARM_MUL_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			else
				ARM_MUL_REG_REG (code, ins->dreg, ins->sreg2, ins->sreg1);
			break;
		case OP_MUL_IMM:
			g_assert_not_reached ();
			break;
		case CEE_MUL_OVF:
			/* FIXME: handle ovf/ sreg2 != dreg */
			ARM_MUL_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case CEE_MUL_OVF_UN:
			/* FIXME: handle ovf/ sreg2 != dreg */
			ARM_MUL_REG_REG (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_ICONST:
		case OP_SETREGIMM:
			code = mono_arm_emit_load_imm (code, ins->dreg, ins->inst_c0);
			break;
		case OP_AOTCONST:
			g_assert_not_reached ();
			mono_add_patch_info (cfg, offset, (MonoJumpInfoType)ins->inst_i1, ins->inst_p0);
			break;
		case CEE_CONV_I4:
		case CEE_CONV_U4:
		case OP_MOVE:
		case OP_SETREG:
			if (ins->dreg != ins->sreg1)
				ARM_MOV_REG_REG (code, ins->dreg, ins->sreg1);
			break;
		case OP_SETLRET: {
			int saved = ins->sreg2;
			if (ins->sreg2 == ARM_LSW_REG) {
				ARM_MOV_REG_REG (code, ARMREG_LR, ins->sreg2);
				saved = ARMREG_LR;
			}
			if (ins->sreg1 != ARM_LSW_REG)
				ARM_MOV_REG_REG (code, ARM_LSW_REG, ins->sreg1);
			if (saved != ARM_MSW_REG)
				ARM_MOV_REG_REG (code, ARM_MSW_REG, saved);
			break;
		}
		case OP_SETFREG:
		case OP_FMOVE:
			ARM_MVFD (code, ins->dreg, ins->sreg1);
			break;
		case OP_FCONV_TO_R4:
			ARM_MVFS (code, ins->dreg, ins->sreg1);
			break;
		case CEE_JMP:
			/*
			 * Keep in sync with mono_arch_emit_epilog
			 */
			g_assert (!cfg->method->save_lmf);
			code = emit_big_add (code, ARMREG_SP, cfg->frame_reg, cfg->stack_usage);
			ARM_POP_NWB (code, cfg->used_int_regs | ((1 << ARMREG_SP)) | ((1 << ARMREG_LR)));
			mono_add_patch_info (cfg, (guint8*) code - cfg->native_code, MONO_PATCH_INFO_METHOD_JUMP, ins->inst_p0);
			ARM_B (code, 0);
			break;
		case OP_CHECK_THIS:
			/* ensure ins->sreg1 is not NULL */
			ARM_LDR_IMM (code, ARMREG_LR, ins->sreg1, 0);
			break;
		case OP_ARGLIST: {
#if ARM_PORT
			if (ppc_is_imm16 (cfg->sig_cookie + cfg->stack_usage)) {
				ppc_addi (code, ppc_r11, cfg->frame_reg, cfg->sig_cookie + cfg->stack_usage);
			} else {
				ppc_load (code, ppc_r11, cfg->sig_cookie + cfg->stack_usage);
				ppc_add (code, ppc_r11, cfg->frame_reg, ppc_r11);
			}
			ppc_stw (code, ppc_r11, 0, ins->sreg1);
#endif
			break;
		}
		case OP_FCALL:
		case OP_LCALL:
		case OP_VCALL:
		case OP_VOIDCALL:
		case CEE_CALL:
			call = (MonoCallInst*)ins;
			if (ins->flags & MONO_INST_HAS_METHOD)
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_METHOD, call->method);
			else
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_ABS, call->fptr);
			if (cfg->method->dynamic) {
				ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 0);
				ARM_B (code, 0);
				*(gpointer*)code = NULL;
				code += 4;
				ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
				ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);
			} else {
				ARM_BL (code, 0);
			}
			break;
		case OP_FCALL_REG:
		case OP_LCALL_REG:
		case OP_VCALL_REG:
		case OP_VOIDCALL_REG:
		case OP_CALL_REG:
			ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
			ARM_MOV_REG_REG (code, ARMREG_PC, ins->sreg1);
			break;
		case OP_FCALL_MEMBASE:
		case OP_LCALL_MEMBASE:
		case OP_VCALL_MEMBASE:
		case OP_VOIDCALL_MEMBASE:
		case OP_CALL_MEMBASE:
			g_assert (arm_is_imm12 (ins->inst_offset));
			g_assert (ins->sreg1 != ARMREG_LR);
			ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
			ARM_LDR_IMM (code, ARMREG_PC, ins->sreg1, ins->inst_offset);
			break;
		case OP_OUTARG:
			g_assert_not_reached ();
			break;
		case OP_LOCALLOC: {
			/* keep alignment */
			int alloca_waste = cfg->param_area;
			alloca_waste += 7;
			alloca_waste &= ~7;
			/* round the size to 8 bytes */
			ARM_ADD_REG_IMM8 (code, ins->dreg, ins->sreg1, 7);
			ARM_BIC_REG_IMM8 (code, ins->dreg, ins->sreg1, 7);
			ARM_ADD_REG_IMM8 (code, ins->dreg, ins->dreg, alloca_waste);
			ARM_SUB_REG_REG (code, ARMREG_SP, ARMREG_SP, ins->dreg);
			/* memzero the area: dreg holds the size, sp is the pointer */
			if (ins->flags & MONO_INST_INIT) {
				guint8 *start_loop, *branch_to_cond;
				ARM_MOV_REG_IMM8 (code, ARMREG_LR, 0);
				branch_to_cond = code;
				ARM_B (code, 0);
				start_loop = code;
				ARM_STR_REG_REG (code, ARMREG_LR, ARMREG_SP, ins->dreg);
				arm_patch (branch_to_cond, code);
				/* decrement by 4 and set flags */
				ARM_SUBS_REG_IMM8 (code, ins->dreg, ins->dreg, 4);
				ARM_B_COND (code, ARMCOND_LT, 0);
				arm_patch (code - 4, start_loop);
			}
			ARM_ADD_REG_IMM8 (code, ins->dreg, ARMREG_SP, alloca_waste);
			break;
		}
		case CEE_RET:
			g_assert_not_reached ();
			ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_LR);
			break;
		case CEE_THROW: {
			if (ins->sreg1 != ARMREG_R0)
				ARM_MOV_REG_REG (code, ARMREG_R0, ins->sreg1);
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_INTERNAL_METHOD, 
					     (gpointer)"mono_arch_throw_exception");
			if (cfg->method->dynamic) {
				ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 0);
				ARM_B (code, 0);
				*(gpointer*)code = NULL;
				code += 4;
				ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
				ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);
			} else {
				ARM_BL (code, 0);
			}
			break;
		}
		case OP_RETHROW: {
			if (ins->sreg1 != ARMREG_R0)
				ARM_MOV_REG_REG (code, ARMREG_R0, ins->sreg1);
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_INTERNAL_METHOD, 
					     (gpointer)"mono_arch_rethrow_exception");
			if (cfg->method->dynamic) {
				ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 0);
				ARM_B (code, 0);
				*(gpointer*)code = NULL;
				code += 4;
				ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
				ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);
			} else {
				ARM_BL (code, 0);
			}
			break;
		}
		case OP_START_HANDLER:
			if (arm_is_imm12 (ins->inst_left->inst_offset)) {
				ARM_STR_IMM (code, ARMREG_LR, ins->inst_left->inst_basereg, ins->inst_left->inst_offset);
			} else {
				code = mono_arm_emit_load_imm (code, ARMREG_IP, ins->inst_left->inst_offset);
				ARM_STR_REG_REG (code, ARMREG_LR, ins->inst_left->inst_basereg, ARMREG_IP);
			}
			break;
		case OP_ENDFILTER:
			if (ins->sreg1 != ARMREG_R0)
				ARM_MOV_REG_REG (code, ARMREG_R0, ins->sreg1);
			if (arm_is_imm12 (ins->inst_left->inst_offset)) {
				ARM_LDR_IMM (code, ARMREG_IP, ins->inst_left->inst_basereg, ins->inst_left->inst_offset);
			} else {
				g_assert (ARMREG_IP != ins->inst_left->inst_basereg);
				code = mono_arm_emit_load_imm (code, ARMREG_IP, ins->inst_left->inst_offset);
				ARM_LDR_REG_REG (code, ARMREG_IP, ins->inst_left->inst_basereg, ARMREG_IP);
			}
			ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);
			break;
		case CEE_ENDFINALLY:
			if (arm_is_imm12 (ins->inst_left->inst_offset)) {
				ARM_LDR_IMM (code, ARMREG_IP, ins->inst_left->inst_basereg, ins->inst_left->inst_offset);
			} else {
				g_assert (ARMREG_IP != ins->inst_left->inst_basereg);
				code = mono_arm_emit_load_imm (code, ARMREG_IP, ins->inst_left->inst_offset);
				ARM_LDR_REG_REG (code, ARMREG_IP, ins->inst_left->inst_basereg, ARMREG_IP);
			}
			ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);
			break;
		case OP_CALL_HANDLER: 
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_BB, ins->inst_target_bb);
			ARM_BL (code, 0);
			break;
		case OP_LABEL:
			ins->inst_c0 = code - cfg->native_code;
			break;
		case CEE_BR:
			if (ins->flags & MONO_INST_BRLABEL) {
				/*if (ins->inst_i0->inst_c0) {
					ARM_B (code, 0);
					//x86_jump_code (code, cfg->native_code + ins->inst_i0->inst_c0);
				} else*/ {
					mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_LABEL, ins->inst_i0);
					ARM_B (code, 0);
				}
			} else {
				/*if (ins->inst_target_bb->native_offset) {
					ARM_B (code, 0);
					//x86_jump_code (code, cfg->native_code + ins->inst_target_bb->native_offset); 
				} else*/ {
					mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_BB, ins->inst_target_bb);
					ARM_B (code, 0);
				} 
			}
			break;
		case OP_BR_REG:
			ARM_MOV_REG_REG (code, ARMREG_PC, ins->sreg1);
			break;
		case CEE_SWITCH:
			/* 
			 * In the normal case we have:
			 * 	ldr pc, [pc, ins->sreg1 << 2]
			 * 	nop
			 * If aot, we have:
			 * 	ldr lr, [pc, ins->sreg1 << 2]
			 * 	add pc, pc, lr
			 * After follows the data.
			 * FIXME: add aot support.
			 */
			max_len += 4 * GPOINTER_TO_INT (ins->klass);
			if (offset > (cfg->code_size - max_len - 16)) {
				cfg->code_size += max_len;
				cfg->code_size *= 2;
				cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
				code = cfg->native_code + offset;
			}
			ARM_LDR_REG_REG_SHIFT (code, ARMREG_PC, ARMREG_PC, ins->sreg1, ARMSHIFT_LSL, 2);
			ARM_NOP (code);
			code += 4 * GPOINTER_TO_INT (ins->klass);
			break;
		case OP_CEQ:
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_EQ);
			break;
		case OP_CLT:
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_LT);
			break;
		case OP_CLT_UN:
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_LO);
			break;
		case OP_CGT:
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_GT);
			break;
		case OP_CGT_UN:
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_HI);
			break;
		case OP_COND_EXC_EQ:
		case OP_COND_EXC_NE_UN:
		case OP_COND_EXC_LT:
		case OP_COND_EXC_LT_UN:
		case OP_COND_EXC_GT:
		case OP_COND_EXC_GT_UN:
		case OP_COND_EXC_GE:
		case OP_COND_EXC_GE_UN:
		case OP_COND_EXC_LE:
		case OP_COND_EXC_LE_UN:
			EMIT_COND_SYSTEM_EXCEPTION (ins->opcode - OP_COND_EXC_EQ, ins->inst_p1);
			break;
		case OP_COND_EXC_C:
		case OP_COND_EXC_OV:
		case OP_COND_EXC_NC:
		case OP_COND_EXC_NO:
			g_assert_not_reached ();
			break;
		case CEE_BEQ:
		case CEE_BNE_UN:
		case CEE_BLT:
		case CEE_BLT_UN:
		case CEE_BGT:
		case CEE_BGT_UN:
		case CEE_BGE:
		case CEE_BGE_UN:
		case CEE_BLE:
		case CEE_BLE_UN:
			EMIT_COND_BRANCH (ins, ins->opcode - CEE_BEQ);
			break;

		/* floating point opcodes */
		case OP_R8CONST:
			/* FIXME: we can optimize the imm load by dealing with part of 
			 * the displacement in LDFD (aligning to 512).
			 */
			code = mono_arm_emit_load_imm (code, ARMREG_LR, (guint32)ins->inst_p0);
			ARM_LDFD (code, ins->dreg, ARMREG_LR, 0);
			break;
		case OP_R4CONST:
			code = mono_arm_emit_load_imm (code, ARMREG_LR, (guint32)ins->inst_p0);
			ARM_LDFS (code, ins->dreg, ARMREG_LR, 0);
			break;
		case OP_STORER8_MEMBASE_REG:
			g_assert (arm_is_fpimm8 (ins->inst_offset));
			ARM_STFD (code, ins->sreg1, ins->inst_destbasereg, ins->inst_offset);
			break;
		case OP_LOADR8_MEMBASE:
			g_assert (arm_is_fpimm8 (ins->inst_offset));
			ARM_LDFD (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case OP_STORER4_MEMBASE_REG:
			g_assert (arm_is_fpimm8 (ins->inst_offset));
			ARM_STFS (code, ins->sreg1, ins->inst_destbasereg, ins->inst_offset);
			break;
		case OP_LOADR4_MEMBASE:
			g_assert (arm_is_fpimm8 (ins->inst_offset));
			ARM_LDFS (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case CEE_CONV_R_UN: {
			int tmpreg;
			tmpreg = ins->dreg == 0? 1: 0;
			ARM_CMP_REG_IMM8 (code, ins->sreg1, 0);
			ARM_FLTD (code, ins->dreg, ins->sreg1);
			ARM_B_COND (code, ARMCOND_GE, 8);
			/* save the temp register */
			ARM_SUB_REG_IMM8 (code, ARMREG_SP, ARMREG_SP, 8);
			ARM_STFD (code, tmpreg, ARMREG_SP, 0);
			ARM_LDFD (code, tmpreg, ARMREG_PC, 4);
			ARM_FPA_ADFD (code, ins->dreg, ins->dreg, tmpreg);
			ARM_LDFD (code, tmpreg, ARMREG_SP, 0);
			ARM_ADD_REG_IMM8 (code, ARMREG_SP, ARMREG_SP, 8);
			/* skip the constant pool */
			ARM_B (code, 4);
			*(int*)code = 0x41f00000;
			code += 4;
			*(int*)code = 0;
			code += 4;
			/* FIXME: adjust:
			 * ldfltd  ftemp, [pc, #8] 0x41f00000 0x00000000
			 * adfltd  fdest, fdest, ftemp
			 */
			break;
		}
		case CEE_CONV_R4:
			ARM_FLTS (code, ins->dreg, ins->sreg1);
			break;
		case CEE_CONV_R8:
			ARM_FLTD (code, ins->dreg, ins->sreg1);
			break;
		case OP_X86_FP_LOAD_I8:
			g_assert_not_reached ();
			/*x86_fild_membase (code, ins->inst_basereg, ins->inst_offset, TRUE);*/
			break;
		case OP_X86_FP_LOAD_I4:
			g_assert_not_reached ();
			/*x86_fild_membase (code, ins->inst_basereg, ins->inst_offset, FALSE);*/
			break;
		case OP_FCONV_TO_I1:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 1, TRUE);
			break;
		case OP_FCONV_TO_U1:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 1, FALSE);
			break;
		case OP_FCONV_TO_I2:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 2, TRUE);
			break;
		case OP_FCONV_TO_U2:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 2, FALSE);
			break;
		case OP_FCONV_TO_I4:
		case OP_FCONV_TO_I:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 4, TRUE);
			break;
		case OP_FCONV_TO_U4:
		case OP_FCONV_TO_U:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 4, FALSE);
			break;
		case OP_FCONV_TO_I8:
		case OP_FCONV_TO_U8:
			g_assert_not_reached ();
			/* Implemented as helper calls */
			break;
		case OP_LCONV_TO_R_UN:
			g_assert_not_reached ();
			/* Implemented as helper calls */
			break;
		case OP_LCONV_TO_OVF_I: {
#if ARM_PORT
			guint32 *negative_branch, *msword_positive_branch, *msword_negative_branch, *ovf_ex_target;
			// Check if its negative
			ppc_cmpi (code, 0, 0, ins->sreg1, 0);
			negative_branch = code;
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_LT, 0);
			// Its positive msword == 0
			ppc_cmpi (code, 0, 0, ins->sreg2, 0);
			msword_positive_branch = code;
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);

			ovf_ex_target = code;
			//EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_ALWAYS, 0, "OverflowException");
			// Negative
			ppc_patch (negative_branch, code);
			ppc_cmpi (code, 0, 0, ins->sreg2, -1);
			msword_negative_branch = code;
			ppc_bc (code, PPC_BR_FALSE, PPC_BR_EQ, 0);
			ppc_patch (msword_negative_branch, ovf_ex_target);
			
			ppc_patch (msword_positive_branch, code);
			if (ins->dreg != ins->sreg1)
				ppc_mr (code, ins->dreg, ins->sreg1);
#endif
			if (ins->dreg != ins->sreg1)
				ARM_MOV_REG_REG (code, ins->dreg, ins->sreg1);
			break;
		}
		case OP_FADD:
			ARM_FPA_ADFD (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_FSUB:
			ARM_FPA_SUFD (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;		
		case OP_FMUL:
			ARM_FPA_MUFD (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;		
		case OP_FDIV:
			ARM_FPA_DVFD (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;		
		case OP_FNEG:
			ARM_MNFD (code, ins->dreg, ins->sreg1);
			break;		
		case OP_FREM:
			/* emulated */
			g_assert_not_reached ();
			break;
		case OP_FCOMPARE:
			/* each fp compare op needs to do its own */
			g_assert_not_reached ();
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			break;
		case OP_FCEQ:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_EQ);
			break;
		case OP_FCLT:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_MI);
			break;
		case OP_FCLT_UN:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_MI);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_VS);
			break;
		case OP_FCGT:
			/* swapped */
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg2, ins->sreg1);
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_MI);
			break;
		case OP_FCGT_UN:
			/* swapped */
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg2, ins->sreg1);
			ARM_MOV_REG_IMM8 (code, ins->dreg, 0);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_MI);
			ARM_MOV_REG_IMM8_COND (code, ins->dreg, 1, ARMCOND_VS);
			break;
		/* ARM FPA flags table:
		 * N        Less than               ARMCOND_MI
		 * Z        Equal                   ARMCOND_EQ
		 * C        Greater Than or Equal   ARMCOND_CS
		 * V        Unordered               ARMCOND_VS
		 */
		case OP_FBEQ:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			EMIT_COND_BRANCH (ins, CEE_BEQ - CEE_BEQ);
			break;
		case OP_FBNE_UN:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			EMIT_COND_BRANCH (ins, CEE_BNE_UN - CEE_BEQ);
			break;
		case OP_FBLT:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_MI); /* N set */
			break;
		case OP_FBLT_UN:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_VS); /* V set */
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_MI); /* N set */
			break;
		case OP_FBGT:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg2, ins->sreg1);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_MI); /* N set, swapped args */
			break;
		case OP_FBGT_UN:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg2, ins->sreg1);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_VS); /* V set */
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_MI); /* N set, swapped args */
			break;
		case OP_FBGE:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_CS);
			break;
		case OP_FBGE_UN:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg1, ins->sreg2);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_VS); /* V set */
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_CS);
			break;
		case OP_FBLE:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg2, ins->sreg1);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_CS); /* swapped */
			break;
		case OP_FBLE_UN:
			ARM_FCMP (code, ARM_FPA_CMF, ins->sreg2, ins->sreg1);
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_VS); /* V set */
			EMIT_COND_BRANCH_FLAGS (ins, ARMCOND_CS); /* swapped */
			break;
		case CEE_CKFINITE: {
			/*ppc_stfd (code, ins->sreg1, -8, ppc_sp);
			ppc_lwz (code, ppc_r11, -8, ppc_sp);
			ppc_rlwinm (code, ppc_r11, ppc_r11, 0, 1, 31);
			ppc_addis (code, ppc_r11, ppc_r11, -32752);
			ppc_rlwinmd (code, ppc_r11, ppc_r11, 1, 31, 31);
			EMIT_COND_SYSTEM_EXCEPTION (CEE_BEQ - CEE_BEQ, "ArithmeticException");*/
			g_assert_not_reached ();
			break;
		}
		default:
			g_warning ("unknown opcode %s in %s()\n", mono_inst_name (ins->opcode), __FUNCTION__);
			g_assert_not_reached ();
		}

		if ((cfg->opt & MONO_OPT_BRANCH) && ((code - cfg->native_code - offset) > max_len)) {
			g_warning ("wrong maximal instruction length of instruction %s (expected %d, got %d)",
				   mono_inst_name (ins->opcode), max_len, code - cfg->native_code - offset);
			g_assert_not_reached ();
		}
	       
		cpos += max_len;

		last_ins = ins;
		last_offset = offset;
		
		ins = ins->next;
	}

	cfg->code_len = code - cfg->native_code;
}

void
mono_arch_register_lowlevel_calls (void)
{
}

#define patch_lis_ori(ip,val) do {\
		guint16 *__lis_ori = (guint16*)(ip);	\
		__lis_ori [1] = (((guint32)(val)) >> 16) & 0xffff;	\
		__lis_ori [3] = ((guint32)(val)) & 0xffff;	\
	} while (0)

void
mono_arch_patch_code (MonoMethod *method, MonoDomain *domain, guint8 *code, MonoJumpInfo *ji, gboolean run_cctors)
{
	MonoJumpInfo *patch_info;

	for (patch_info = ji; patch_info; patch_info = patch_info->next) {
		unsigned char *ip = patch_info->ip.i + code;
		const unsigned char *target;

		if (patch_info->type == MONO_PATCH_INFO_SWITCH) {
			gpointer *table = (gpointer *)patch_info->data.table->table;
			gpointer *jt = (gpointer*)(ip + 8);
			int i;
			/* jt is the inlined jump table, 2 instructions after ip
			 * In the normal case we store the absolute addresses,
			 * otherwise the displacements.
			 */
			for (i = 0; i < patch_info->data.table->table_size; i++) { 
				jt [i] = code + (int)patch_info->data.table->table [i];
			}
			continue;
		}
		target = mono_resolve_patch_target (method, domain, code, patch_info, run_cctors);

		switch (patch_info->type) {
		case MONO_PATCH_INFO_IP:
			g_assert_not_reached ();
			patch_lis_ori (ip, ip);
			continue;
		case MONO_PATCH_INFO_METHOD_REL:
			g_assert_not_reached ();
			*((gpointer *)(ip)) = code + patch_info->data.offset;
			continue;
		case MONO_PATCH_INFO_METHODCONST:
		case MONO_PATCH_INFO_CLASS:
		case MONO_PATCH_INFO_IMAGE:
		case MONO_PATCH_INFO_FIELD:
		case MONO_PATCH_INFO_VTABLE:
		case MONO_PATCH_INFO_IID:
		case MONO_PATCH_INFO_SFLDA:
		case MONO_PATCH_INFO_LDSTR:
		case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
		case MONO_PATCH_INFO_LDTOKEN:
			g_assert_not_reached ();
			/* from OP_AOTCONST : lis + ori */
			patch_lis_ori (ip, target);
			continue;
		case MONO_PATCH_INFO_R4:
		case MONO_PATCH_INFO_R8:
			g_assert_not_reached ();
			*((gconstpointer *)(ip + 2)) = patch_info->data.target;
			continue;
		case MONO_PATCH_INFO_EXC_NAME:
			g_assert_not_reached ();
			*((gconstpointer *)(ip + 1)) = patch_info->data.name;
			continue;
		case MONO_PATCH_INFO_NONE:
		case MONO_PATCH_INFO_BB_OVF:
		case MONO_PATCH_INFO_EXC_OVF:
			/* everything is dealt with at epilog output time */
			continue;
		default:
			break;
		}
		arm_patch (ip, target);
	}
}

/*
 * Stack frame layout:
 * 
 *   ------------------- fp
 *   	MonoLMF structure or saved registers
 *   -------------------
 *   	locals
 *   -------------------
 *   	spilled regs
 *   -------------------
 *   	optional 8 bytes for tracing
 *   -------------------
 *   	param area             size is cfg->param_area
 *   ------------------- sp
 */
guint8 *
mono_arch_emit_prolog (MonoCompile *cfg)
{
	MonoMethod *method = cfg->method;
	MonoBasicBlock *bb;
	MonoMethodSignature *sig;
	MonoInst *inst;
	int alloc_size, pos, max_offset, i, rot_amount;
	guint8 *code;
	CallInfo *cinfo;
	int tracing = 0;
	int lmf_offset = 0;
	int prev_sp_offset;

	if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
		tracing = 1;

	sig = mono_method_signature (method);
	cfg->code_size = 256 + sig->param_count * 20;
	code = cfg->native_code = g_malloc (cfg->code_size);

	ARM_MOV_REG_REG (code, ARMREG_IP, ARMREG_SP);

	alloc_size = cfg->stack_offset;
	pos = 0;

	if (!method->save_lmf) {
		ARM_PUSH (code, (cfg->used_int_regs | (1 << ARMREG_IP) | (1 << ARMREG_LR)));
		prev_sp_offset = 8; /* ip and lr */
		for (i = 0; i < 16; ++i) {
			if (cfg->used_int_regs & (1 << i))
				prev_sp_offset += 4;
		}
	} else {
		ARM_PUSH (code, 0x5ff0);
		prev_sp_offset = 4 * 10; /* all but r0-r3, sp and pc */
		pos += sizeof (MonoLMF) - prev_sp_offset;
		lmf_offset = pos;
	}
	alloc_size += pos;
	// align to MONO_ARCH_FRAME_ALIGNMENT bytes
	if (alloc_size & (MONO_ARCH_FRAME_ALIGNMENT - 1)) {
		alloc_size += MONO_ARCH_FRAME_ALIGNMENT - 1;
		alloc_size &= ~(MONO_ARCH_FRAME_ALIGNMENT - 1);
	}

	/* the stack used in the pushed regs */
	if (prev_sp_offset & 4)
		alloc_size += 4;
	cfg->stack_usage = alloc_size;
	if (alloc_size) {
		if ((i = mono_arm_is_rotated_imm8 (alloc_size, &rot_amount)) >= 0) {
			ARM_SUB_REG_IMM (code, ARMREG_SP, ARMREG_SP, i, rot_amount);
		} else {
			code = mono_arm_emit_load_imm (code, ARMREG_IP, alloc_size);
			ARM_SUB_REG_REG (code, ARMREG_SP, ARMREG_SP, ARMREG_IP);
		}
	}
	if (cfg->frame_reg != ARMREG_SP)
		ARM_MOV_REG_REG (code, cfg->frame_reg, ARMREG_SP);
	//g_print ("prev_sp_offset: %d, alloc_size:%d\n", prev_sp_offset, alloc_size);
	prev_sp_offset += alloc_size;

        /* compute max_offset in order to use short forward jumps
	 * we could skip do it on arm because the immediate displacement
	 * for jumps is large enough, it may be useful later for constant pools
	 */
	max_offset = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins = bb->code;
		bb->max_offset = max_offset;

		if (cfg->prof_options & MONO_PROFILE_COVERAGE)
			max_offset += 6; 

		while (ins) {
			max_offset += ((guint8 *)arm_cpu_desc [ins->opcode])[MONO_INST_LEN];
			ins = ins->next;
		}
	}

	/* load arguments allocated to register from the stack */
	pos = 0;

	cinfo = calculate_sizes (sig, sig->pinvoke);

	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		ArgInfo *ainfo = &cinfo->ret;
		inst = cfg->ret;
		g_assert (arm_is_imm12 (inst->inst_offset));
		ARM_STR_IMM (code, ainfo->reg, inst->inst_basereg, inst->inst_offset);
	}
	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ArgInfo *ainfo = cinfo->args + i;
		inst = cfg->varinfo [pos];
		
		if (cfg->verbose_level > 2)
			g_print ("Saving argument %d (type: %d)\n", i, ainfo->regtype);
		if (inst->opcode == OP_REGVAR) {
			if (ainfo->regtype == RegTypeGeneral)
				ARM_MOV_REG_REG (code, inst->dreg, ainfo->reg);
			else if (ainfo->regtype == RegTypeFP) {
				g_assert_not_reached ();
			} else if (ainfo->regtype == RegTypeBase) {
				g_assert (arm_is_imm12 (prev_sp_offset + ainfo->offset));
				ARM_LDR_IMM (code, inst->dreg, ARMREG_SP, (prev_sp_offset + ainfo->offset));
			} else
				g_assert_not_reached ();

			if (cfg->verbose_level > 2)
				g_print ("Argument %d assigned to register %s\n", pos, mono_arch_regname (inst->dreg));
		} else {
			/* the argument should be put on the stack: FIXME handle size != word  */
			if (ainfo->regtype == RegTypeGeneral) {
				switch (ainfo->size) {
				case 1:
					if (arm_is_imm12 (inst->inst_offset))
						ARM_STRB_IMM (code, ainfo->reg, inst->inst_basereg, inst->inst_offset);
					else {
						code = mono_arm_emit_load_imm (code, ARMREG_IP, inst->inst_offset);
						ARM_STRB_REG_REG (code, ainfo->reg, inst->inst_basereg, ARMREG_IP);
					}
					break;
				case 2:
					g_assert (arm_is_imm8 (inst->inst_offset));
					ARM_STRH_IMM (code, ainfo->reg, inst->inst_basereg, inst->inst_offset);
					break;
				case 8:
					g_assert (arm_is_imm12 (inst->inst_offset));
					ARM_STR_IMM (code, ainfo->reg, inst->inst_basereg, inst->inst_offset);
					g_assert (arm_is_imm12 (inst->inst_offset + 4));
					ARM_STR_IMM (code, ainfo->reg + 1, inst->inst_basereg, inst->inst_offset + 4);
					break;
				default:
					if (arm_is_imm12 (inst->inst_offset)) {
						ARM_STR_IMM (code, ainfo->reg, inst->inst_basereg, inst->inst_offset);
					} else {
						code = mono_arm_emit_load_imm (code, ARMREG_IP, inst->inst_offset);
						ARM_STR_REG_REG (code, ainfo->reg, inst->inst_basereg, ARMREG_IP);
					}
					break;
				}
			} else if (ainfo->regtype == RegTypeBase) {
				g_assert (arm_is_imm12 (prev_sp_offset + ainfo->offset));
				switch (ainfo->size) {
				case 1:
					ARM_LDR_IMM (code, ARMREG_LR, ARMREG_SP, (prev_sp_offset + ainfo->offset));
					g_assert (arm_is_imm12 (inst->inst_offset));
					ARM_STRB_IMM (code, ARMREG_LR, inst->inst_basereg, inst->inst_offset);
					break;
				case 2:
					ARM_LDR_IMM (code, ARMREG_LR, ARMREG_SP, (prev_sp_offset + ainfo->offset));
					g_assert (arm_is_imm8 (inst->inst_offset));
					ARM_STRH_IMM (code, ARMREG_LR, inst->inst_basereg, inst->inst_offset);
					break;
				case 8:
					g_assert (arm_is_imm12 (inst->inst_offset));
					ARM_LDR_IMM (code, ARMREG_LR, ARMREG_SP, (prev_sp_offset + ainfo->offset));
					ARM_STR_IMM (code, ARMREG_LR, inst->inst_basereg, inst->inst_offset);
					g_assert (arm_is_imm12 (prev_sp_offset + ainfo->offset + 4));
					g_assert (arm_is_imm12 (inst->inst_offset + 4));
					ARM_LDR_IMM (code, ARMREG_LR, ARMREG_SP, (prev_sp_offset + ainfo->offset + 4));
					ARM_STR_IMM (code, ARMREG_LR, inst->inst_basereg, inst->inst_offset + 4);
					break;
				default:
					g_assert (arm_is_imm12 (inst->inst_offset));
					ARM_LDR_IMM (code, ARMREG_LR, ARMREG_SP, (prev_sp_offset + ainfo->offset));
					ARM_STR_IMM (code, ARMREG_LR, inst->inst_basereg, inst->inst_offset);
					break;
				}
			} else if (ainfo->regtype == RegTypeFP) {
				g_assert_not_reached ();
			} else if (ainfo->regtype == RegTypeStructByVal) {
				int doffset = inst->inst_offset;
				int soffset = 0;
				int cur_reg;
				int size = 0;
				if (mono_class_from_mono_type (inst->inst_vtype))
					size = mono_class_native_size (mono_class_from_mono_type (inst->inst_vtype), NULL);
				for (cur_reg = 0; cur_reg < ainfo->size; ++cur_reg) {
					g_assert (arm_is_imm12 (doffset));
					ARM_STR_IMM (code, ainfo->reg + cur_reg, inst->inst_basereg, doffset);
					soffset += sizeof (gpointer);
					doffset += sizeof (gpointer);
				}
				if (ainfo->vtsize) {
					/* FIXME: handle overrun! with struct sizes not multiple of 4 */
					//g_print ("emit_memcpy (prev_sp_ofs: %d, ainfo->offset: %d, soffset: %d)\n", prev_sp_offset, ainfo->offset, soffset);
					code = emit_memcpy (code, ainfo->vtsize * sizeof (gpointer), inst->inst_basereg, doffset, ARMREG_SP, prev_sp_offset + ainfo->offset);
				}
			} else if (ainfo->regtype == RegTypeStructByAddr) {
				g_assert_not_reached ();
				/* FIXME: handle overrun! with struct sizes not multiple of 4 */
				code = emit_memcpy (code, ainfo->vtsize * sizeof (gpointer), inst->inst_basereg, inst->inst_offset, ainfo->reg, 0);
			} else
				g_assert_not_reached ();
		}
		pos++;
	}

	if (method->save_lmf) {

		mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_INTERNAL_METHOD, 
			     (gpointer)"mono_get_lmf_addr");
		if (cfg->method->dynamic) {
			ARM_LDR_IMM (code, ARMREG_IP, ARMREG_PC, 0);
			ARM_B (code, 0);
			*(gpointer*)code = NULL;
			code += 4;
			ARM_MOV_REG_REG (code, ARMREG_LR, ARMREG_PC);
			ARM_MOV_REG_REG (code, ARMREG_PC, ARMREG_IP);
		} else {
			ARM_BL (code, 0);
		}
		/* we build the MonoLMF structure on the stack - see mini-arm.h */
		/* lmf_offset is the offset from the previous stack pointer,
		 * alloc_size is the total stack space allocated, so the offset
		 * of MonoLMF from the current stack ptr is alloc_size - lmf_offset.
		 * The pointer to the struct is put in r1 (new_lmf).
		 * r2 is used as scratch
		 * The callee-saved registers are already in the MonoLMF structure
		 */
		code = emit_big_add (code, ARMREG_R1, ARMREG_SP, alloc_size - lmf_offset);
		/* r0 is the result from mono_get_lmf_addr () */
		ARM_STR_IMM (code, ARMREG_R0, ARMREG_R1, G_STRUCT_OFFSET (MonoLMF, lmf_addr));
		/* new_lmf->previous_lmf = *lmf_addr */
		ARM_LDR_IMM (code, ARMREG_R2, ARMREG_R0, G_STRUCT_OFFSET (MonoLMF, previous_lmf));
		ARM_STR_IMM (code, ARMREG_R2, ARMREG_R1, G_STRUCT_OFFSET (MonoLMF, previous_lmf));
		/* *(lmf_addr) = r1 */
		ARM_STR_IMM (code, ARMREG_R1, ARMREG_R0, G_STRUCT_OFFSET (MonoLMF, previous_lmf));
		/* save method info */
		code = mono_arm_emit_load_imm (code, ARMREG_R2, method);
		ARM_STR_IMM (code, ARMREG_R2, ARMREG_R1, G_STRUCT_OFFSET (MonoLMF, method));
		ARM_STR_IMM (code, ARMREG_SP, ARMREG_R1, G_STRUCT_OFFSET (MonoLMF, ebp));
		/* save the current IP */
		ARM_MOV_REG_REG (code, ARMREG_R2, ARMREG_PC);
		ARM_STR_IMM (code, ARMREG_R2, ARMREG_R1, G_STRUCT_OFFSET (MonoLMF, eip));
	}

	if (tracing)
		code = mono_arch_instrument_prolog (cfg, mono_trace_enter_method, code, TRUE);

	cfg->code_len = code - cfg->native_code;
	g_assert (cfg->code_len < cfg->code_size);
	g_free (cinfo);

	return code;
}

void
mono_arch_emit_epilog (MonoCompile *cfg)
{
	MonoJumpInfo *patch_info;
	MonoMethod *method = cfg->method;
	int pos, i, rot_amount;
	int max_epilog_size = 16 + 20*4;
	guint8 *code;

	if (cfg->method->save_lmf)
		max_epilog_size += 128;
	
	if (mono_jit_trace_calls != NULL)
		max_epilog_size += 50;

	if (cfg->prof_options & MONO_PROFILE_ENTER_LEAVE)
		max_epilog_size += 50;

	while (cfg->code_len + max_epilog_size > (cfg->code_size - 16)) {
		cfg->code_size *= 2;
		cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
		mono_jit_stats.code_reallocs++;
	}

	/*
	 * Keep in sync with CEE_JMP
	 */
	code = cfg->native_code + cfg->code_len;

	if (mono_jit_trace_calls != NULL && mono_trace_eval (method)) {
		code = mono_arch_instrument_epilog (cfg, mono_trace_leave_method, code, TRUE);
	}
	pos = 0;

	if (method->save_lmf) {
		int lmf_offset;
		/* all but r0-r3, sp and pc */
		pos += sizeof (MonoLMF) - (4 * 10);
		lmf_offset = pos;
		/* r2 contains the pointer to the current LMF */
		code = emit_big_add (code, ARMREG_R2, cfg->frame_reg, cfg->stack_usage - lmf_offset);
		/* ip = previous_lmf */
		ARM_LDR_IMM (code, ARMREG_IP, ARMREG_R2, G_STRUCT_OFFSET (MonoLMF, previous_lmf));
		/* lr = lmf_addr */
		ARM_LDR_IMM (code, ARMREG_LR, ARMREG_R2, G_STRUCT_OFFSET (MonoLMF, lmf_addr));
		/* *(lmf_addr) = previous_lmf */
		ARM_STR_IMM (code, ARMREG_IP, ARMREG_LR, G_STRUCT_OFFSET (MonoLMF, previous_lmf));
		/* FIXME: speedup: there is no actual need to restore the registers if
		 * we didn't actually change them (idea from Zoltan).
		 */
		/* restore iregs */
		/* point sp at the registers to restore: 10 is 14 -4, because we skip r0-r3 */
		ARM_ADD_REG_IMM8 (code, ARMREG_SP, ARMREG_R2, (sizeof (MonoLMF) - 10 * sizeof (gulong)));
		ARM_POP_NWB (code, 0xaff0); /* restore ip to sp and lr to pc */
	} else {
		if ((i = mono_arm_is_rotated_imm8 (cfg->stack_usage, &rot_amount)) >= 0) {
			ARM_ADD_REG_IMM (code, ARMREG_SP, cfg->frame_reg, i, rot_amount);
		} else {
			code = mono_arm_emit_load_imm (code, ARMREG_IP, cfg->stack_usage);
			ARM_ADD_REG_REG (code, ARMREG_SP, ARMREG_SP, ARMREG_IP);
		}
		ARM_POP_NWB (code, cfg->used_int_regs | ((1 << ARMREG_SP) | (1 << ARMREG_PC)));
	}

	cfg->code_len = code - cfg->native_code;

	g_assert (cfg->code_len < cfg->code_size);

}

/* remove once throw_exception_by_name is eliminated */
static int
exception_id_by_name (const char *name)
{
	if (strcmp (name, "IndexOutOfRangeException") == 0)
		return MONO_EXC_INDEX_OUT_OF_RANGE;
	if (strcmp (name, "OverflowException") == 0)
		return MONO_EXC_OVERFLOW;
	if (strcmp (name, "ArithmeticException") == 0)
		return MONO_EXC_ARITHMETIC;
	if (strcmp (name, "DivideByZeroException") == 0)
		return MONO_EXC_DIVIDE_BY_ZERO;
	if (strcmp (name, "InvalidCastException") == 0)
		return MONO_EXC_INVALID_CAST;
	if (strcmp (name, "NullReferenceException") == 0)
		return MONO_EXC_NULL_REF;
	if (strcmp (name, "ArrayTypeMismatchException") == 0)
		return MONO_EXC_ARRAY_TYPE_MISMATCH;
	g_error ("Unknown intrinsic exception %s\n", name);
}

void
mono_arch_emit_exceptions (MonoCompile *cfg)
{
	MonoJumpInfo *patch_info;
	int nthrows, i;
	guint8 *code;
	const guint8* exc_throw_pos [MONO_EXC_INTRINS_NUM] = {NULL};
	guint8 exc_throw_found [MONO_EXC_INTRINS_NUM] = {0};
	guint32 code_size;
	int exc_count = 0;
	int max_epilog_size = 50;

	/* count the number of exception infos */
     
	/* 
	 * make sure we have enough space for exceptions
	 * 12 is the simulated call to throw_exception_by_name
	 */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		if (patch_info->type == MONO_PATCH_INFO_EXC) {
			i = exception_id_by_name (patch_info->data.target);
			if (!exc_throw_found [i]) {
				max_epilog_size += 12;
				exc_throw_found [i] = TRUE;
			}
		}
	}

	while (cfg->code_len + max_epilog_size > (cfg->code_size - 16)) {
		cfg->code_size *= 2;
		cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
		mono_jit_stats.code_reallocs++;
	}

	code = cfg->native_code + cfg->code_len;

	/* add code to raise exceptions */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		switch (patch_info->type) {
		case MONO_PATCH_INFO_EXC: {
			unsigned char *ip = patch_info->ip.i + cfg->native_code;
			const char *ex_name = patch_info->data.target;
			i = exception_id_by_name (patch_info->data.target);
			if (exc_throw_pos [i]) {
				arm_patch (ip, exc_throw_pos [i]);
				patch_info->type = MONO_PATCH_INFO_NONE;
				break;
			} else {
				exc_throw_pos [i] = code;
			}
			arm_patch (ip, code);
			//*(int*)code = 0xef9f0001;
			code += 4;
			/*mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_EXC_NAME, patch_info->data.target);*/
			ARM_LDR_IMM (code, ARMREG_R0, ARMREG_PC, 0);
			/* we got here from a conditional call, so the calling ip is set in lr already */
			patch_info->type = MONO_PATCH_INFO_INTERNAL_METHOD;
			patch_info->data.name = "mono_arch_throw_exception_by_name";
			patch_info->ip.i = code - cfg->native_code;
			ARM_B (code, 0);
			*(gpointer*)code = ex_name;
			code += 4;
			break;
		}
		default:
			/* do nothing */
			break;
		}
	}

	cfg->code_len = code - cfg->native_code;

	g_assert (cfg->code_len < cfg->code_size);

}

void
mono_arch_setup_jit_tls_data (MonoJitTlsData *tls)
{
}

void
mono_arch_free_jit_tls_data (MonoJitTlsData *tls)
{
}

void
mono_arch_emit_this_vret_args (MonoCompile *cfg, MonoCallInst *inst, int this_reg, int this_type, int vt_reg)
{
	
	int this_dreg = ARMREG_R0;
	
	if (vt_reg != -1)
		this_dreg = ARMREG_R1;

	/* add the this argument */
	if (this_reg != -1) {
		MonoInst *this;
		MONO_INST_NEW (cfg, this, OP_SETREG);
		this->type = this_type;
		this->sreg1 = this_reg;
		this->dreg = mono_regstate_next_int (cfg->rs);
		mono_bblock_add_inst (cfg->cbb, this);
		mono_call_inst_add_outarg_reg (inst, this->dreg, this_dreg, FALSE);
	}

	if (vt_reg != -1) {
		MonoInst *vtarg;
		MONO_INST_NEW (cfg, vtarg, OP_SETREG);
		vtarg->type = STACK_MP;
		vtarg->sreg1 = vt_reg;
		vtarg->dreg = mono_regstate_next_int (cfg->rs);
		mono_bblock_add_inst (cfg->cbb, vtarg);
		mono_call_inst_add_outarg_reg (inst, vtarg->dreg, ARMREG_R0, FALSE);
	}
}

MonoInst*
mono_arch_get_inst_for_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	return NULL;
}

gboolean
mono_arch_print_tree (MonoInst *tree, int arity)
{
	return 0;
}

MonoInst* mono_arch_get_domain_intrinsic (MonoCompile* cfg)
{
	return NULL;
}

MonoInst* 
mono_arch_get_thread_intrinsic (MonoCompile* cfg)
{
	return NULL;
}

void
mono_arch_flush_register_windows (void)
{
}

void
mono_arch_fixup_jinfo (MonoCompile *cfg)
{
	/* max encoded stack usage is 64KB * 4 */
	g_assert ((cfg->stack_usage & ~(0xffff << 2)) == 0);
	cfg->jit_info->used_regs |= cfg->stack_usage << 14;
}

