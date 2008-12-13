/*
 * decompose.c: Functions to decompose complex IR instructions into simpler ones.
 *
 * Author:
 *   Zoltan Varga (vargaz@gmail.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#include "mini.h"
#include "ir-emit.h"

#ifndef DISABLE_JIT

/* FIXME: This conflicts with the definition in mini.c, so it cannot be moved to mini.h */
MonoInst* mono_emit_native_call (MonoCompile *cfg, gconstpointer func, MonoMethodSignature *sig, MonoInst **args);
void mini_emit_stobj (MonoCompile *cfg, MonoInst *dest, MonoInst *src, MonoClass *klass, gboolean native);
void mini_emit_initobj (MonoCompile *cfg, MonoInst *dest, const guchar *ip, MonoClass *klass);

/*
 * mono_decompose_opcode:
 *
 *   Decompose complex opcodes into ones closer to opcodes supported by
 * the given architecture.
 */
void
mono_decompose_opcode (MonoCompile *cfg, MonoInst *ins)
{
	/* FIXME: Instead of = NOP, don't emit the original ins at all */

#ifdef MONO_ARCH_HAVE_DECOMPOSE_OPTS
	mono_arch_decompose_opts (cfg, ins);
#endif

	/*
	 * The code below assumes that we are called immediately after emitting 
	 * ins. This means we can emit code using the normal code generation
	 * macros.
	 */
	switch (ins->opcode) {
	/* this doesn't make sense on ppc and other architectures */
#if !defined(MONO_ARCH_NO_IOV_CHECK)
	case OP_IADD_OVF:
		ins->opcode = OP_IADDCC;
		MONO_EMIT_NEW_COND_EXC (cfg, IOV, "OverflowException");
		break;
	case OP_IADD_OVF_UN:
		ins->opcode = OP_IADDCC;
		MONO_EMIT_NEW_COND_EXC (cfg, IC, "OverflowException");
		break;
	case OP_ISUB_OVF:
		ins->opcode = OP_ISUBCC;
		MONO_EMIT_NEW_COND_EXC (cfg, IOV, "OverflowException");
		break;
	case OP_ISUB_OVF_UN:
		ins->opcode = OP_ISUBCC;
		MONO_EMIT_NEW_COND_EXC (cfg, IC, "OverflowException");
		break;
#endif
	case OP_ICONV_TO_OVF_I1:
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, 127);
		MONO_EMIT_NEW_COND_EXC (cfg, IGT, "OverflowException");
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, -128);
		MONO_EMIT_NEW_COND_EXC (cfg, ILT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I1, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_OVF_I1_UN:
		/* probe values between 0 to 127 */
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, 127);
		MONO_EMIT_NEW_COND_EXC (cfg, IGT_UN, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I1, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_OVF_U1:
	case OP_ICONV_TO_OVF_U1_UN:
		/* probe value to be within 0 to 255 */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 255);
		MONO_EMIT_NEW_COND_EXC (cfg, IGT_UN, "OverflowException");
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IAND_IMM, ins->dreg, ins->sreg1, 0xff);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_OVF_I2:
		/* Probe value to be within -32768 and 32767 */
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, 32767);
		MONO_EMIT_NEW_COND_EXC (cfg, IGT, "OverflowException");
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, -32768);
		MONO_EMIT_NEW_COND_EXC (cfg, ILT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I2, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_OVF_I2_UN:
		/* Convert uint value into short, value within 0 and 32767 */
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, 32767);
		MONO_EMIT_NEW_COND_EXC (cfg, IGT_UN, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I2, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_OVF_U2:
	case OP_ICONV_TO_OVF_U2_UN:
		/* Probe value to be within 0 and 65535 */
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, 0xffff);
		MONO_EMIT_NEW_COND_EXC (cfg, IGT_UN, "OverflowException");
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IAND_IMM, ins->dreg, ins->sreg1, 0xffff);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_OVF_U4:
	case OP_ICONV_TO_OVF_I4_UN:
#if SIZEOF_REGISTER == 4
	case OP_ICONV_TO_OVF_U:
	case OP_ICONV_TO_OVF_I_UN:
#endif
		MONO_EMIT_NEW_ICOMPARE_IMM (cfg, ins->sreg1, 0);
		MONO_EMIT_NEW_COND_EXC (cfg, ILT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_I4:
	case OP_ICONV_TO_U4:
	case OP_ICONV_TO_OVF_I4:
#if SIZEOF_REGISTER == 4
	case OP_ICONV_TO_OVF_I:
	case OP_ICONV_TO_OVF_U_UN:
#endif
		ins->opcode = OP_MOVE;
		break;
	case OP_ICONV_TO_I:
#if SIZEOF_REGISTER == 8
		ins->opcode = OP_SEXT_I4;
#else
		ins->opcode = OP_MOVE;
#endif
		break;
	case OP_ICONV_TO_U:
#if SIZEOF_REGISTER == 8
		ins->opcode = OP_ZEXT_I4;
#else
		ins->opcode = OP_MOVE;
#endif
		break;

	case OP_FCONV_TO_R8:
		ins->opcode = OP_FMOVE;
		break;

		/* Long opcodes on 64 bit machines */
#if SIZEOF_REGISTER == 8
	case OP_LCONV_TO_I4:
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_IMM, ins->dreg, ins->sreg1, 0);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_I8:
	case OP_LCONV_TO_I:
	case OP_LCONV_TO_U8:
	case OP_LCONV_TO_U:
		ins->opcode = OP_MOVE;
		break;
	case OP_ICONV_TO_I8:
		ins->opcode = OP_SEXT_I4;
		break;
	case OP_ICONV_TO_U8:
		ins->opcode = OP_ZEXT_I4;
		break;
	case OP_LCONV_TO_U4:
		/* Clean out the upper word */
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_UN_IMM, ins->dreg, ins->sreg1, 0);
		ins->opcode = OP_NOP;
		break;
#if defined(__mono_ppc__) && !defined(__mono_ppc64__)
	case OP_LADD_OVF:
		/* ADC sets the condition code */
		MONO_EMIT_NEW_BIALU (cfg, OP_ADDCC, ins->dreg + 1, ins->sreg1 + 1, ins->sreg2 + 1);
		MONO_EMIT_NEW_BIALU (cfg, OP_ADD_OVF_CARRY, ins->dreg + 2, ins->sreg1 + 2, ins->sreg2 + 2);
		ins->opcode = OP_NOP;
		g_assert_not_reached ();
		break;
	case OP_LADD_OVF_UN:
		/* ADC sets the condition code */
		MONO_EMIT_NEW_BIALU (cfg, OP_ADDCC, ins->dreg + 1, ins->sreg1 + 1, ins->sreg2 + 1);
		MONO_EMIT_NEW_BIALU (cfg, OP_ADD_OVF_UN_CARRY, ins->dreg + 2, ins->sreg1 + 2, ins->sreg2 + 2);
		ins->opcode = OP_NOP;
		g_assert_not_reached ();
		break;
	case OP_LSUB_OVF:
		/* SBB sets the condition code */
		MONO_EMIT_NEW_BIALU (cfg, OP_SUBCC, ins->dreg + 1, ins->sreg1 + 1, ins->sreg2 + 1);
		MONO_EMIT_NEW_BIALU (cfg, OP_SUB_OVF_CARRY, ins->dreg + 2, ins->sreg1 + 2, ins->sreg2 + 2);
		ins->opcode = OP_NOP;
		g_assert_not_reached ();
		break;
	case OP_LSUB_OVF_UN:
		/* SBB sets the condition code */
		MONO_EMIT_NEW_BIALU (cfg, OP_SUBCC, ins->dreg + 1, ins->sreg1 + 1, ins->sreg2 + 1);
		MONO_EMIT_NEW_BIALU (cfg, OP_SUB_OVF_UN_CARRY, ins->dreg + 2, ins->sreg1 + 2, ins->sreg2 + 2);
		ins->opcode = OP_NOP;
		g_assert_not_reached ();
		break;
#else
	case OP_LADD_OVF:
		MONO_EMIT_NEW_BIALU (cfg, OP_ADDCC, ins->dreg, ins->sreg1, ins->sreg2);
		MONO_EMIT_NEW_COND_EXC (cfg, OV, "OverflowException");
		ins->opcode = OP_NOP;
		break;
	case OP_LADD_OVF_UN:
		MONO_EMIT_NEW_BIALU (cfg, OP_ADDCC, ins->dreg, ins->sreg1, ins->sreg2);
		MONO_EMIT_NEW_COND_EXC (cfg, C, "OverflowException");
		ins->opcode = OP_NOP;
		break;
#ifndef __mono_ppc64__
	case OP_LSUB_OVF:
		MONO_EMIT_NEW_BIALU (cfg, OP_SUBCC, ins->dreg, ins->sreg1, ins->sreg2);
		MONO_EMIT_NEW_COND_EXC (cfg, OV, "OverflowException");
		ins->opcode = OP_NOP;
		break;
	case OP_LSUB_OVF_UN:
		MONO_EMIT_NEW_BIALU (cfg, OP_SUBCC, ins->dreg, ins->sreg1, ins->sreg2);
		MONO_EMIT_NEW_COND_EXC (cfg, C, "OverflowException");
		ins->opcode = OP_NOP;
		break;
#endif
#endif
		
	case OP_ICONV_TO_OVF_I8:
	case OP_ICONV_TO_OVF_I:
		ins->opcode = OP_SEXT_I4;
		break;
	case OP_ICONV_TO_OVF_U8:
	case OP_ICONV_TO_OVF_U:
		MONO_EMIT_NEW_COMPARE_IMM (cfg,ins->sreg1, 0);
		MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_ZEXT_I4, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_ICONV_TO_OVF_I8_UN:
	case OP_ICONV_TO_OVF_U8_UN:
	case OP_ICONV_TO_OVF_I_UN:
	case OP_ICONV_TO_OVF_U_UN:
		/* an unsigned 32 bit num always fits in an (un)signed 64 bit one */
		/* Clean out the upper word */
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_UN_IMM, ins->dreg, ins->sreg1, 0);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_I1:
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 127);
		MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, -128);
		MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_LCONV_TO_I1, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_I1_UN:
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 127);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_LCONV_TO_I1, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_U1:
		/* probe value to be within 0 to 255 */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 255);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, ins->dreg, ins->sreg1, 0xff);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_U1_UN:
		/* probe value to be within 0 to 255 */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 255);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, ins->dreg, ins->sreg1, 0xff);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_I2:
		/* Probe value to be within -32768 and 32767 */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 32767);
		MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, -32768);
		MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_LCONV_TO_I2, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_I2_UN:
		/* Probe value to be within 0 and 32767 */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 32767);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_LCONV_TO_I2, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_U2:
		/* Probe value to be within 0 and 65535 */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 0xffff);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, ins->dreg, ins->sreg1, 0xffff);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_U2_UN:
		/* Probe value to be within 0 and 65535 */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 0xffff);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, ins->dreg, ins->sreg1, 0xffff);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_I4:
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 0x7fffffff);
		MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
		/* The int cast is needed for the VS compiler.  See Compiler Warning (level 2) C4146. */
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, ((int)-2147483648));
		MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_I4_UN:
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 0x7fffffff);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_U4:
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 0xffffffffUL);
		MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, ins->sreg1, 0);
		MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_U4_UN:
		MONO_EMIT_NEW_COMPARE_IMM (cfg, ins->sreg1, 0xffffffff);
		MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_I:
	case OP_LCONV_TO_OVF_U_UN:
	case OP_LCONV_TO_OVF_U8_UN:
		ins->opcode = OP_MOVE;
		break;
	case OP_LCONV_TO_OVF_I_UN:
	case OP_LCONV_TO_OVF_I8_UN:
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, ins->sreg1, 0);
		MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	case OP_LCONV_TO_OVF_U8:
	case OP_LCONV_TO_OVF_U:
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, ins->sreg1, 0);
		MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
#endif

	default: {
		MonoJitICallInfo *info;

		info = mono_find_jit_opcode_emulation (ins->opcode);
		if (info) {
			MonoInst **args;
			MonoInst *call;

			/* Create dummy MonoInst's for the arguments */
			g_assert (!info->sig->hasthis);
			g_assert (info->sig->param_count <= 2);

			args = mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst*) * info->sig->param_count);
			if (info->sig->param_count > 0) {
				MONO_INST_NEW (cfg, args [0], OP_ARG);
				args [0]->dreg = ins->sreg1;
			}
			if (info->sig->param_count > 1) {
				MONO_INST_NEW (cfg, args [1], OP_ARG);
				args [1]->dreg = ins->sreg2;
			}

			call = mono_emit_native_call (cfg, mono_icall_get_wrapper (info), info->sig, args);
			call->dreg = ins->dreg;

			ins->opcode = OP_NOP;
		}
		break;
	}
	}
}

#if SIZEOF_REGISTER == 4
static int lbr_decomp [][2] = {
	{0, 0}, /* BEQ */
	{OP_IBGT, OP_IBGE_UN}, /* BGE */
	{OP_IBGT, OP_IBGT_UN}, /* BGT */
	{OP_IBLT, OP_IBLE_UN}, /* BLE */
	{OP_IBLT, OP_IBLT_UN}, /* BLT */
	{0, 0}, /* BNE_UN */
	{OP_IBGT_UN, OP_IBGE_UN}, /* BGE_UN */
	{OP_IBGT_UN, OP_IBGT_UN}, /* BGT_UN */
	{OP_IBLT_UN, OP_IBLE_UN}, /* BLE_UN */
	{OP_IBLT_UN, OP_IBLT_UN}, /* BLT_UN */
};

static int lcset_decomp [][2] = {
	{0, 0}, /* CEQ */
	{OP_IBLT, OP_IBLE_UN}, /* CGT */
	{OP_IBLT_UN, OP_IBLE_UN}, /* CGT_UN */
	{OP_IBGT, OP_IBGE_UN}, /* CLT */
	{OP_IBGT_UN, OP_IBGE_UN}, /* CLT_UN */
};
#endif

/**
 * mono_decompose_long_opts:
 *
 *  Decompose 64bit opcodes into 32bit opcodes on 32 bit platforms.
 */
void
mono_decompose_long_opts (MonoCompile *cfg)
{
#if SIZEOF_REGISTER == 4
	MonoBasicBlock *bb, *first_bb;

	/*
	 * Some opcodes, like lcall can't be decomposed so the rest of the JIT
	 * needs to be able to handle long vregs.
	 */

	/* reg + 1 contains the ls word, reg + 2 contains the ms word */

	/**
	 * Create a dummy bblock and emit code into it so we can use the normal 
	 * code generation macros.
	 */
	cfg->cbb = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoBasicBlock));
	first_bb = cfg->cbb;

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *tree = bb->code;	
		MonoInst *prev = NULL;

		   /*
		mono_print_bb (bb, "BEFORE LOWER_LONG_OPTS");
		*/

		tree = bb->code;
		cfg->cbb->code = cfg->cbb->last_ins = NULL;

		while (tree) {

#ifdef MONO_ARCH_HAVE_DECOMPOSE_LONG_OPTS
			mono_arch_decompose_long_opts (cfg, tree);
#endif

			switch (tree->opcode) {
			case OP_I8CONST:
				MONO_EMIT_NEW_ICONST (cfg, tree->dreg + 1, tree->inst_ls_word);
				MONO_EMIT_NEW_ICONST (cfg, tree->dreg + 2, tree->inst_ms_word);
				break;
			case OP_LMOVE:
			case OP_LCONV_TO_U8:
			case OP_LCONV_TO_I8:
			case OP_LCONV_TO_OVF_U8_UN:
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1 + 1);
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 2, tree->sreg1 + 2);
				break;
			case OP_STOREI8_MEMBASE_REG:
				MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, tree->inst_destbasereg, tree->inst_offset + MINI_MS_WORD_OFFSET, tree->sreg1 + 2);
				MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, tree->inst_destbasereg, tree->inst_offset + MINI_LS_WORD_OFFSET, tree->sreg1 + 1);
				break;
			case OP_LOADI8_MEMBASE:
				MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, tree->dreg + 2, tree->inst_basereg, tree->inst_offset + MINI_MS_WORD_OFFSET);
				MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, tree->dreg + 1, tree->inst_basereg, tree->inst_offset + MINI_LS_WORD_OFFSET);
				break;

			case OP_ICONV_TO_I8: {
				guint32 tmpreg = alloc_ireg (cfg);

				/* branchless code:
				 * low = reg;
				 * tmp = low > -1 ? 1: 0;
				 * high = tmp - 1; if low is zero or pos high becomes 0, else -1
				 */
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, tree->dreg + 1, -1);
				MONO_EMIT_NEW_BIALU (cfg, OP_ICGT, tmpreg, -1, -1);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISUB_IMM, tree->dreg + 2, tmpreg, 1);
				break;
			}
			case OP_ICONV_TO_U8:
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1);
				MONO_EMIT_NEW_ICONST (cfg, tree->dreg + 2, 0);
				break;
			case OP_ICONV_TO_OVF_I8:
				/* a signed 32 bit num always fits in a signed 64 bit one */
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_SHR_IMM, tree->dreg + 2, tree->sreg1, 31);
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1);
				break;
			case OP_ICONV_TO_OVF_U8:
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
				MONO_EMIT_NEW_ICONST (cfg, tree->dreg + 2, 0);
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1);
				break;
			case OP_ICONV_TO_OVF_I8_UN:
			case OP_ICONV_TO_OVF_U8_UN:
				/* an unsigned 32 bit num always fits in an (un)signed 64 bit one */
				MONO_EMIT_NEW_ICONST (cfg, tree->dreg + 2, 0);
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1);
				break;
			case OP_LCONV_TO_I1:
				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I1, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_U1:
				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_U1, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_I2:
				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I2, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_U2:
				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_U2, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_I4:
			case OP_LCONV_TO_U4:
			case OP_LCONV_TO_I:
			case OP_LCONV_TO_U:
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_R8:
				MONO_EMIT_NEW_BIALU (cfg, OP_LCONV_TO_R8_2, tree->dreg, tree->sreg1 + 1, tree->sreg1 + 2);
				break;
			case OP_LCONV_TO_R4:
				MONO_EMIT_NEW_BIALU (cfg, OP_LCONV_TO_R4_2, tree->dreg, tree->sreg1 + 1, tree->sreg1 + 2);
				break;
			case OP_LCONV_TO_R_UN:
				MONO_EMIT_NEW_BIALU (cfg, OP_LCONV_TO_R_UN_2, tree->dreg, tree->sreg1 + 1, tree->sreg1 + 2);
				break;
			case OP_LCONV_TO_OVF_I1: {
				MonoBasicBlock *is_negative, *end_label;

				NEW_BBLOCK (cfg, is_negative);
				NEW_BBLOCK (cfg, end_label);

				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, -1);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");

				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBLT, is_negative);

				/* Positive */
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, 127);
				MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, end_label);

				/* Negative */
				MONO_START_BB (cfg, is_negative);
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, -128);
				MONO_EMIT_NEW_COND_EXC (cfg, LT_UN, "OverflowException");

				MONO_START_BB (cfg, end_label);

				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I1, tree->dreg, tree->sreg1 + 1);
				break;
			}
			case OP_LCONV_TO_OVF_I1_UN:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "OverflowException");

				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, 127);
				MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, -128);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I1, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_OVF_U1:
			case OP_LCONV_TO_OVF_U1_UN:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "OverflowException");

				/* probe value to be within 0 to 255 */
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, 255);
				MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, tree->dreg, tree->sreg1 + 1, 0xff);
				break;
			case OP_LCONV_TO_OVF_I2: {
				MonoBasicBlock *is_negative, *end_label;

				NEW_BBLOCK (cfg, is_negative);
				NEW_BBLOCK (cfg, end_label);

				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, -1);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");

				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBLT, is_negative);

				/* Positive */
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, 32767);
				MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, end_label);

				/* Negative */
				MONO_START_BB (cfg, is_negative);
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, -32768);
				MONO_EMIT_NEW_COND_EXC (cfg, LT_UN, "OverflowException");
				MONO_START_BB (cfg, end_label);

				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I2, tree->dreg, tree->sreg1 + 1);
				break;
			}
			case OP_LCONV_TO_OVF_I2_UN:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "OverflowException");

				/* Probe value to be within -32768 and 32767 */
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, 32767);
				MONO_EMIT_NEW_COND_EXC (cfg, GT, "OverflowException");
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, -32768);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
				MONO_EMIT_NEW_UNALU (cfg, OP_ICONV_TO_I2, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_OVF_U2:
			case OP_LCONV_TO_OVF_U2_UN:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "OverflowException");

				/* Probe value to be within 0 and 65535 */
				MONO_EMIT_NEW_COMPARE_IMM (cfg, tree->sreg1 + 1, 0xffff);
				MONO_EMIT_NEW_COND_EXC (cfg, GT_UN, "OverflowException");
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, tree->dreg, tree->sreg1 + 1, 0xffff);
				break;
			case OP_LCONV_TO_OVF_I4:
			case OP_LCONV_TO_OVF_I:
				MONO_EMIT_NEW_BIALU (cfg, OP_LCONV_TO_OVF_I4_2, tree->dreg, tree->sreg1 + 1, tree->sreg1 + 2);
				break;
			case OP_LCONV_TO_OVF_U4:
			case OP_LCONV_TO_OVF_U:
			case OP_LCONV_TO_OVF_U4_UN:
			case OP_LCONV_TO_OVF_U_UN:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "OverflowException");
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_OVF_I_UN:
			case OP_LCONV_TO_OVF_I4_UN:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "OverflowException");
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 1, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg, tree->sreg1 + 1);
				break;
			case OP_LCONV_TO_OVF_U8:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");

				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1 + 1);
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 2, tree->sreg1 + 2);
				break;
			case OP_LCONV_TO_OVF_I8_UN:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_COND_EXC (cfg, LT, "OverflowException");

				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1 + 1);
				MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 2, tree->sreg1 + 2);
				break;

			case OP_LADD:
				MONO_EMIT_NEW_BIALU (cfg, OP_IADDCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_IADC, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
			case OP_LSUB:
				MONO_EMIT_NEW_BIALU (cfg, OP_ISUBCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_ISBB, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;

#if defined(__ppc__) || defined(__powerpc__)
			case OP_LADD_OVF:
				/* ADC sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_ADDCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_ADD_OVF_CARRY, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
			case OP_LADD_OVF_UN:
				/* ADC sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_ADDCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_ADD_OVF_UN_CARRY, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
			case OP_LSUB_OVF:
				/* SBB sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_SUBCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_SUB_OVF_CARRY, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
			case OP_LSUB_OVF_UN:
				/* SBB sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_SUBCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_SUB_OVF_UN_CARRY, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
#else
			case OP_LADD_OVF:
				/* ADC sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_IADDCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_IADC, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				MONO_EMIT_NEW_COND_EXC (cfg, OV, "OverflowException");
				break;
			case OP_LADD_OVF_UN:
				/* ADC sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_IADDCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_IADC, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				MONO_EMIT_NEW_COND_EXC (cfg, C, "OverflowException");
				break;
			case OP_LSUB_OVF:
				/* SBB sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_ISUBCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_ISBB, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				MONO_EMIT_NEW_COND_EXC (cfg, OV, "OverflowException");
				break;
			case OP_LSUB_OVF_UN:
				/* SBB sets the condition code */
				MONO_EMIT_NEW_BIALU (cfg, OP_ISUBCC, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_ISBB, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				MONO_EMIT_NEW_COND_EXC (cfg, C, "OverflowException");
				break;
#endif
			case OP_LAND:
				MONO_EMIT_NEW_BIALU (cfg, OP_IAND, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_IAND, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
			case OP_LOR:
				MONO_EMIT_NEW_BIALU (cfg, OP_IOR, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_IOR, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
			case OP_LXOR:
				MONO_EMIT_NEW_BIALU (cfg, OP_IXOR, tree->dreg + 1, tree->sreg1 + 1, tree->sreg2 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_IXOR, tree->dreg + 2, tree->sreg1 + 2, tree->sreg2 + 2);
				break;
			case OP_LNOT:
				MONO_EMIT_NEW_UNALU (cfg, OP_INOT, tree->dreg + 1, tree->sreg1 + 1);
				MONO_EMIT_NEW_UNALU (cfg, OP_INOT, tree->dreg + 2, tree->sreg1 + 2);
				break;
			case OP_LNEG:
				/* 
				 * FIXME: The original version in inssel-long32.brg does not work
				 * on x86, and the x86 version might not work on other archs ?
				 */
				/* FIXME: Move these to mono_arch_decompose_long_opts () */
#if defined(__i386__)
				MONO_EMIT_NEW_UNALU (cfg, OP_INEG, tree->dreg + 1, tree->sreg1 + 1);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ADC_IMM, tree->dreg + 2, tree->sreg1 + 2, 0);
				MONO_EMIT_NEW_UNALU (cfg, OP_INEG, tree->dreg + 2, tree->dreg + 2);
#elif defined(__sparc__)
				MONO_EMIT_NEW_BIALU (cfg, OP_SUBCC, tree->dreg + 1, 0, tree->sreg1 + 1);
				MONO_EMIT_NEW_BIALU (cfg, OP_SBB, tree->dreg + 2, 0, tree->sreg1 + 2);
#elif defined(__arm__)
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ARM_RSBS_IMM, tree->dreg + 1, tree->sreg1 + 1, 0);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ARM_RSC_IMM, tree->dreg + 2, tree->sreg1 + 2, 0);
#elif defined(__ppc__) || defined(__powerpc__)
				/* This is the old version from inssel-long32.brg */
				MONO_EMIT_NEW_UNALU (cfg, OP_INOT, tree->dreg + 1, tree->sreg1 + 1);
				MONO_EMIT_NEW_UNALU (cfg, OP_INOT, tree->dreg + 2, tree->sreg1 + 2);
				/* ADC sets the condition codes */
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ADC_IMM, tree->dreg + 1, tree->dreg + 1, 1);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ADC_IMM, tree->dreg + 2, tree->dreg + 2, 0);
#else
				NOT_IMPLEMENTED;
#endif
				break;
			case OP_LMUL:
				/* Emulated */
				/* FIXME: Add OP_BIGMUL optimization */
				break;

			case OP_LADD_IMM:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ADDCC_IMM, tree->dreg + 1, tree->sreg1 + 1, tree->inst_ls_word);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ADC_IMM, tree->dreg + 2, tree->sreg1 + 2, tree->inst_ms_word);
				break;
			case OP_LSUB_IMM:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_SUBCC_IMM, tree->dreg + 1, tree->sreg1 + 1, tree->inst_ls_word);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_SBB_IMM, tree->dreg + 2, tree->sreg1 + 2, tree->inst_ms_word);
				break;
			case OP_LAND_IMM:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, tree->dreg + 1, tree->sreg1 + 1, tree->inst_ls_word);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, tree->dreg + 2, tree->sreg1 + 2, tree->inst_ms_word);
				break;
			case OP_LOR_IMM:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_OR_IMM, tree->dreg + 1, tree->sreg1 + 1, tree->inst_ls_word);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_OR_IMM, tree->dreg + 2, tree->sreg1 + 2, tree->inst_ms_word);
				break;
			case OP_LXOR_IMM:
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_XOR_IMM, tree->dreg + 1, tree->sreg1 + 1, tree->inst_ls_word);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_XOR_IMM, tree->dreg + 2, tree->sreg1 + 2, tree->inst_ms_word);
				break;
			case OP_LSHR_UN_IMM:
				if (tree->inst_c1 == 32) {

					/* The original code had this comment: */
					/* special case that gives a nice speedup and happens to workaorund a ppc jit but (for the release)
					 * later apply the speedup to the left shift as well
					 * See BUG# 57957.
					 */
					/* FIXME: Move this to the strength reduction pass */
					/* just move the upper half to the lower and zero the high word */
					MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 1, tree->sreg1 + 2);
					MONO_EMIT_NEW_ICONST (cfg, tree->dreg + 2, 0);
				}
				break;
			case OP_LSHL_IMM:
				if (tree->inst_c1 == 32) {
					/* just move the lower half to the upper and zero the lower word */
					MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, tree->dreg + 2, tree->sreg1 + 1);
					MONO_EMIT_NEW_ICONST (cfg, tree->dreg + 1, 0);
				}
				break;

			case OP_LCOMPARE: {
				MonoInst *next = tree->next;

				g_assert (next);

				switch (next->opcode) {
				case OP_LBEQ:
				case OP_LBNE_UN: {
					int d1, d2;

					/* Branchless version based on gcc code */
					d1 = alloc_ireg (cfg);
					d2 = alloc_ireg (cfg);
					MONO_EMIT_NEW_BIALU (cfg, OP_IXOR, d1, tree->sreg1 + 1, tree->sreg2 + 1);
					MONO_EMIT_NEW_BIALU (cfg, OP_IXOR, d2, tree->sreg1 + 2, tree->sreg2 + 2);
					MONO_EMIT_NEW_BIALU (cfg, OP_IOR, d1, d1, d2);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, d1, 0);
					MONO_EMIT_NEW_BRANCH_BLOCK2 (cfg, next->opcode == OP_LBEQ ? OP_IBEQ : OP_IBNE_UN, next->inst_true_bb, next->inst_false_bb);
					next->opcode = OP_NOP;
					break;
				}
				case OP_LBGE:
				case OP_LBGT:
				case OP_LBLE:
				case OP_LBLT:
				case OP_LBGE_UN:
				case OP_LBGT_UN:
				case OP_LBLE_UN:
				case OP_LBLT_UN:
					/* Convert into three comparisons + branches */
					MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, tree->sreg1 + 2, tree->sreg2 + 2);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, lbr_decomp [next->opcode - OP_LBEQ][0], next->inst_true_bb);
					MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, tree->sreg1 + 2, tree->sreg2 + 2);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBNE_UN, next->inst_false_bb);
					MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, tree->sreg1 + 1, tree->sreg2 + 1);
					MONO_EMIT_NEW_BRANCH_BLOCK2 (cfg, lbr_decomp [next->opcode - OP_LBEQ][1], next->inst_true_bb, next->inst_false_bb);
					next->opcode = OP_NOP;
					break;
				case OP_LCEQ: {
					int d1, d2;
	
					/* Branchless version based on gcc code */
					d1 = alloc_ireg (cfg);
					d2 = alloc_ireg (cfg);
					MONO_EMIT_NEW_BIALU (cfg, OP_IXOR, d1, tree->sreg1 + 1, tree->sreg2 + 1);
					MONO_EMIT_NEW_BIALU (cfg, OP_IXOR, d2, tree->sreg1 + 2, tree->sreg2 + 2);
					MONO_EMIT_NEW_BIALU (cfg, OP_IOR, d1, d1, d2);

					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, d1, 0);
					MONO_EMIT_NEW_UNALU (cfg, OP_ICEQ, next->dreg, -1);
					next->opcode = OP_NOP;
					break;
				}
				case OP_LCLT:
				case OP_LCLT_UN:
				case OP_LCGT:
				case OP_LCGT_UN: {
					MonoBasicBlock *set_to_0, *set_to_1;
	
					NEW_BBLOCK (cfg, set_to_0);
					NEW_BBLOCK (cfg, set_to_1);

					MONO_EMIT_NEW_ICONST (cfg, next->dreg, 0);
					MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, tree->sreg1 + 2, tree->sreg2 + 2);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, lcset_decomp [next->opcode - OP_LCEQ][0], set_to_0);
					MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, tree->sreg1 + 2, tree->sreg2 + 2);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBNE_UN, set_to_1);
					MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, tree->sreg1 + 1, tree->sreg2 + 1);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, lcset_decomp [next->opcode - OP_LCEQ][1], set_to_0);
					MONO_START_BB (cfg, set_to_1);
					MONO_EMIT_NEW_ICONST (cfg, next->dreg, 1);
					MONO_START_BB (cfg, set_to_0);
					next->opcode = OP_NOP;
					break;	
				}
				default:
					g_assert_not_reached ();
				}
				break;
			}

			/* Not yet used, since lcompare is decomposed before local cprop */
			case OP_LCOMPARE_IMM: {
				MonoInst *next = tree->next;
				guint32 low_imm = tree->inst_ls_word;
				guint32 high_imm = tree->inst_ms_word;
				int low_reg = tree->sreg1 + 1;
				int high_reg = tree->sreg1 + 2;

				g_assert (next);

				switch (next->opcode) {
				case OP_LBEQ:
				case OP_LBNE_UN: {
					int d1, d2;

					/* Branchless version based on gcc code */
					d1 = alloc_ireg (cfg);
					d2 = alloc_ireg (cfg);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IXOR_IMM, d1, low_reg, low_imm);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IXOR_IMM, d2, high_reg, high_imm);
					MONO_EMIT_NEW_BIALU (cfg, OP_IOR, d1, d1, d2);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, d1, 0);
					MONO_EMIT_NEW_BRANCH_BLOCK2 (cfg, next->opcode == OP_LBEQ ? OP_IBEQ : OP_IBNE_UN, next->inst_true_bb, next->inst_false_bb);
					next->opcode = OP_NOP;
					break;
				}

				case OP_LBGE:
				case OP_LBGT:
				case OP_LBLE:
				case OP_LBLT:
				case OP_LBGE_UN:
				case OP_LBGT_UN:
				case OP_LBLE_UN:
				case OP_LBLT_UN:
					/* Convert into three comparisons + branches */
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, high_reg, high_imm);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, lbr_decomp [next->opcode - OP_LBEQ][0], next->inst_true_bb);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, high_reg, high_imm);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBNE_UN, next->inst_false_bb);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, low_reg, low_imm);
					MONO_EMIT_NEW_BRANCH_BLOCK2 (cfg, lbr_decomp [next->opcode - OP_LBEQ][1], next->inst_true_bb, next->inst_false_bb);
					next->opcode = OP_NOP;
					break;
				case OP_LCEQ: {
					int d1, d2;
	
					/* Branchless version based on gcc code */
					d1 = alloc_ireg (cfg);
					d2 = alloc_ireg (cfg);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IXOR_IMM, d1, low_reg, low_imm);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IXOR_IMM, d2, high_reg, high_imm);
					MONO_EMIT_NEW_BIALU (cfg, OP_IOR, d1, d1, d2);

					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, d1, 0);
					MONO_EMIT_NEW_UNALU (cfg, OP_ICEQ, next->dreg, -1);
					next->opcode = OP_NOP;
					break;
				}
				case OP_LCLT:
				case OP_LCLT_UN:
				case OP_LCGT:
				case OP_LCGT_UN: {
					MonoBasicBlock *set_to_0, *set_to_1;
	
					NEW_BBLOCK (cfg, set_to_0);
					NEW_BBLOCK (cfg, set_to_1);

					MONO_EMIT_NEW_ICONST (cfg, next->dreg, 0);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, high_reg, high_imm);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, lcset_decomp [next->opcode - OP_LCEQ][0], set_to_0);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, high_reg, high_imm);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBNE_UN, set_to_1);
					MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, low_reg, low_imm);
					MONO_EMIT_NEW_BRANCH_BLOCK (cfg, lcset_decomp [next->opcode - OP_LCEQ][1], set_to_0);
					MONO_START_BB (cfg, set_to_1);
					MONO_EMIT_NEW_ICONST (cfg, next->dreg, 1);
					MONO_START_BB (cfg, set_to_0);
					next->opcode = OP_NOP;
					break;	
				}
				default:
					g_assert_not_reached ();
				}
				break;
			}

			default:
				break;
			}

			if (cfg->cbb->code || (cfg->cbb != first_bb)) {
				MonoInst *new_prev;

				/* Replace the original instruction with the new code sequence */

				/* Ignore the new value of prev */
				new_prev = prev;
				mono_replace_ins (cfg, bb, tree, &new_prev, first_bb, cfg->cbb);

				/* Process the newly added ops again since they can be long ops too */
				if (prev)
					tree = prev->next;
				else
					tree = bb->code;

				first_bb->code = first_bb->last_ins = NULL;
				first_bb->in_count = first_bb->out_count = 0;
				cfg->cbb = first_bb;
			}
			else {
				prev = tree;
				tree = tree->next;
			}
		}
	}
#endif

	/*
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		mono_print_bb (bb, "AFTER LOWER-LONG-OPTS");
	*/
}

/**
 * mono_decompose_vtype_opts:
 *
 *  Decompose valuetype opcodes.
 */
void
mono_decompose_vtype_opts (MonoCompile *cfg)
{
	MonoBasicBlock *bb, *first_bb;

	/**
	 * Using OP_V opcodes and decomposing them later have two main benefits:
	 * - it simplifies method_to_ir () since there is no need to special-case vtypes
	 *   everywhere.
	 * - it gets rid of the LDADDR opcodes generated when vtype operations are decomposed,
	 *   enabling optimizations to work on vtypes too.
	 * Unlike decompose_long_opts, this pass does not alter the CFG of the method so it 
	 * can be executed anytime. It should be executed as late as possible so vtype
	 * opcodes can be optimized by the other passes.
	 * The pinvoke wrappers need to manipulate vtypes in their unmanaged representation.
	 * This is indicated by setting the 'backend.is_pinvoke' field of the MonoInst for the 
	 * var to 1.
	 * This is done on demand, ie. by the LDNATIVEOBJ opcode, and propagated by this pass 
	 * when OP_VMOVE opcodes are decomposed.
	 */

	/* 
	 * Vregs have no associated type information, so we store the type of the vregs
	 * in ins->klass.
	 */

	/**
	 * Create a dummy bblock and emit code into it so we can use the normal 
	 * code generation macros.
	 */
	cfg->cbb = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoBasicBlock));
	first_bb = cfg->cbb;

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		MonoInst *prev = NULL;
		MonoInst *src_var, *dest_var, *src, *dest;
		gboolean restart;
		int dreg;

		if (cfg->verbose_level > 2) mono_print_bb (bb, "BEFORE LOWER-VTYPE-OPTS ");

		cfg->cbb->code = cfg->cbb->last_ins = NULL;
		restart = TRUE;

		while (restart) {
			restart = FALSE;

			for (ins = bb->code; ins; ins = ins->next) {
				switch (ins->opcode) {
				case OP_VMOVE: {
					src_var = get_vreg_to_inst (cfg, ins->sreg1);
					dest_var = get_vreg_to_inst (cfg, ins->dreg);

					g_assert (ins->klass);

					if (!src_var)
						src_var = mono_compile_create_var_for_vreg (cfg, &ins->klass->byval_arg, OP_LOCAL, ins->dreg);

					if (!dest_var)
						dest_var = mono_compile_create_var_for_vreg (cfg, &ins->klass->byval_arg, OP_LOCAL, ins->dreg);

					// FIXME:
					if (src_var->backend.is_pinvoke)
						dest_var->backend.is_pinvoke = 1;

					EMIT_NEW_VARLOADA ((cfg), (src), src_var, src_var->inst_vtype);
					EMIT_NEW_VARLOADA ((cfg), (dest), dest_var, dest_var->inst_vtype);

					mini_emit_stobj (cfg, dest, src, src_var->klass, src_var->backend.is_pinvoke);
					break;
				}
				case OP_VZERO:
					g_assert (ins->klass);

					EMIT_NEW_VARLOADA_VREG (cfg, dest, ins->dreg, &ins->klass->byval_arg);
					mini_emit_initobj (cfg, dest, NULL, ins->klass);
					break;
				case OP_STOREV_MEMBASE: {
					src_var = get_vreg_to_inst (cfg, ins->sreg1);

					if (!src_var) {
						g_assert (ins->klass);
						src_var = mono_compile_create_var_for_vreg (cfg, &ins->klass->byval_arg, OP_LOCAL, ins->sreg1);
					}

					EMIT_NEW_VARLOADA_VREG ((cfg), (src), ins->sreg1, &ins->klass->byval_arg);

					dreg = alloc_preg (cfg);
					EMIT_NEW_BIALU_IMM (cfg, dest, OP_ADD_IMM, dreg, ins->inst_destbasereg, ins->inst_offset);
					mini_emit_stobj (cfg, dest, src, src_var->klass, src_var->backend.is_pinvoke);
					break;
				}
				case OP_LOADV_MEMBASE: {
					g_assert (ins->klass);

					dest_var = get_vreg_to_inst (cfg, ins->dreg);
					// FIXME:
					if (!dest_var)
						dest_var = mono_compile_create_var_for_vreg (cfg, &ins->klass->byval_arg, OP_LOCAL, ins->dreg);

					dreg = alloc_preg (cfg);
					EMIT_NEW_BIALU_IMM (cfg, src, OP_ADD_IMM, dreg, ins->inst_basereg, ins->inst_offset);
					EMIT_NEW_VARLOADA (cfg, dest, dest_var, dest_var->inst_vtype);
					mini_emit_stobj (cfg, dest, src, dest_var->klass, dest_var->backend.is_pinvoke);
					break;
				}
				case OP_OUTARG_VT: {
					g_assert (ins->klass);

					src_var = get_vreg_to_inst (cfg, ins->sreg1);
					if (!src_var)
						src_var = mono_compile_create_var_for_vreg (cfg, &ins->klass->byval_arg, OP_LOCAL, ins->sreg1);
					EMIT_NEW_VARLOADA (cfg, src, src_var, src_var->inst_vtype);

					mono_arch_emit_outarg_vt (cfg, ins, src);

					/* This might be decomposed into other vtype opcodes */
					restart = TRUE;
					break;
				}
				case OP_OUTARG_VTRETADDR: {
					MonoCallInst *call = (MonoCallInst*)ins->inst_p1;

					src_var = get_vreg_to_inst (cfg, call->inst.dreg);
					if (!src_var)
						src_var = mono_compile_create_var_for_vreg (cfg, call->signature->ret, OP_LOCAL, call->inst.dreg);
					// FIXME: src_var->backend.is_pinvoke ?

					EMIT_NEW_VARLOADA (cfg, src, src_var, src_var->inst_vtype);
					src->dreg = ins->dreg;
					break;
				}
				case OP_VCALL:
				case OP_VCALL_REG:
				case OP_VCALL_MEMBASE: {
					MonoCallInst *call = (MonoCallInst*)ins;
					int size;

					if (call->vret_in_reg) {
						MonoCallInst *call2;

						/* Replace the vcall with an integer call */
						MONO_INST_NEW_CALL (cfg, call2, OP_NOP);
						memcpy (call2, call, sizeof (MonoCallInst));
						switch (ins->opcode) {
						case OP_VCALL:
							call2->inst.opcode = OP_CALL;
							break;
						case OP_VCALL_REG:
							call2->inst.opcode = OP_CALL_REG;
							break;
						case OP_VCALL_MEMBASE:
							call2->inst.opcode = OP_CALL_MEMBASE;
							break;
						}
						call2->inst.dreg = alloc_preg (cfg);
						MONO_ADD_INS (cfg->cbb, ((MonoInst*)call2));

						/* Compute the vtype location */
						dest_var = get_vreg_to_inst (cfg, call->inst.dreg);
						if (!dest_var)
							dest_var = mono_compile_create_var_for_vreg (cfg, call->signature->ret, OP_LOCAL, call->inst.dreg);
						EMIT_NEW_VARLOADA (cfg, dest, dest_var, dest_var->inst_vtype);

						/* Save the result */
						if (dest_var->backend.is_pinvoke)
							size = mono_class_native_size (dest->inst_vtype->data.klass, NULL);
						else
							size = mono_type_size (dest_var->inst_vtype, NULL);
						switch (size) {
						case 1:
							MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI1_MEMBASE_REG, dest->dreg, 0, call2->inst.dreg);
							break;
						case 2:
							MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI2_MEMBASE_REG, dest->dreg, 0, call2->inst.dreg);
							break;
						case 4:
							MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, dest->dreg, 0, call2->inst.dreg);
							break;
						case 8:
							MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI8_MEMBASE_REG, dest->dreg, 0, call2->inst.dreg);
							break;
						default:
							/* This assumes the vtype is sizeof (gpointer) long */
							MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, dest->dreg, 0, call2->inst.dreg);
							break;
						}
					} else {
						switch (ins->opcode) {
						case OP_VCALL:
							ins->opcode = OP_VCALL2;
							break;
						case OP_VCALL_REG:
							ins->opcode = OP_VCALL2_REG;
							break;
						case OP_VCALL_MEMBASE:
							ins->opcode = OP_VCALL2_MEMBASE;
							break;
						}
						ins->dreg = -1;
					}
					break;
				}
				default:
					break;
				}

				g_assert (cfg->cbb == first_bb);

				if (cfg->cbb->code || (cfg->cbb != first_bb)) {
					/* Replace the original instruction with the new code sequence */

					mono_replace_ins (cfg, bb, ins, &prev, first_bb, cfg->cbb);
					first_bb->code = first_bb->last_ins = NULL;
					first_bb->in_count = first_bb->out_count = 0;
					cfg->cbb = first_bb;
				}
				else
					prev = ins;
			}
		}

		if (cfg->verbose_level > 2) mono_print_bb (bb, "AFTER LOWER-VTYPE-OPTS ");
	}
}

#endif /* DISABLE_JIT */
