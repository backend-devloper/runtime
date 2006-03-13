#ifndef __MONO_MINI_S390_H__
#define __MONO_MINI_S390_H__

#include <mono/arch/s390/s390-codegen.h>
#include <signal.h>

#define MONO_MAX_IREGS 16
#define MONO_MAX_FREGS 16

/*-------------------------------------------*/
/* Parameters used by the register allocator */
/*-------------------------------------------*/
#define MONO_ARCH_HAS_XP_LOCAL_REGALLOC 1

#define MONO_EMIT_NEW_MOVE(cfg,dest,offset,src,imm,size) do { 			\
                MonoInst *inst; 						\
		int tmpr = 0;							\
		int sReg, dReg;							\
										\
		inst = mono_mempool_alloc0 ((cfg)->mempool, sizeof (MonoInst));	\
		if (size > 256) {						\
			tmpr = mono_regstate_next_int (cfg->rs);		\
			MONO_EMIT_NEW_ICONST(cfg,tmpr,size);			\
			inst->dreg	  = dest;				\
			inst->inst_offset = offset;				\
			inst->sreg1	  = src;				\
			inst->inst_imm	  = imm;				\
			inst->sreg2	  = tmpr;				\
		} else {							\
			if (s390_is_uimm12(offset)) {				\
				inst->dreg	  = dest;			\
				inst->inst_offset = offset;			\
			} else {						\
				dReg = mono_regstate_next_int (cfg->rs);	\
				MONO_EMIT_NEW_BIALU_IMM(cfg, OP_ADD_IMM,	\
					dReg, dest, offset);			\
				inst->dreg	  = dReg;			\
				inst->inst_offset = 0;				\
			}							\
			if (s390_is_uimm12(imm)) {  				\
				inst->sreg1	  = src; 			\
				inst->inst_imm    = imm;   			\
			} else {						\
				sReg = mono_regstate_next_int (cfg->rs);	\
				MONO_EMIT_NEW_BIALU_IMM(cfg, OP_ADD_IMM,	\
					sReg, src, imm);   			\
				inst->sreg1	  = sReg;			\
				inst->inst_imm    = 0;				\
			}							\
		}								\
                inst->opcode 	  = OP_S390_MOVE; 				\
		inst->unused	  = size;					\
	        mono_bblock_add_inst (cfg->cbb, inst); 				\
	} while (0)

#define MONO_OUTPUT_VTR(cfg, size, dr, sr, so) do {				\
	int reg = mono_regstate_next_int (cfg->rs);				\
	switch (size) {								\
		case 0: 							\
			MONO_EMIT_NEW_ICONST(cfg, reg, 0);			\
			mono_call_inst_add_outarg_reg(s, call, reg, dr, FALSE);	\
		break;								\
		case 1:								\
			MONO_EMIT_NEW_LOAD_MEMBASE_OP(cfg, OP_LOADU1_MEMBASE,	\
				reg, sr, so);					\
			mono_call_inst_add_outarg_reg(s, call, reg, dr, FALSE);	\
		break;								\
		case 2:								\
			MONO_EMIT_NEW_LOAD_MEMBASE_OP(cfg, OP_LOADU2_MEMBASE,	\
				reg, sr, so);					\
			mono_call_inst_add_outarg_reg(s, call, reg, dr, FALSE);	\
		break;								\
		case 4:								\
			MONO_EMIT_NEW_LOAD_MEMBASE_OP(cfg, OP_LOAD_MEMBASE,	\
				reg, sr, so);					\
			mono_call_inst_add_outarg_reg(s, call, reg, dr, FALSE);	\
		break;								\
		case 8:								\
			MONO_EMIT_NEW_LOAD_MEMBASE_OP(cfg, OP_LOAD_MEMBASE,	\
				reg, sr, so);					\
			mono_call_inst_add_outarg_reg(s, call, reg, dr, FALSE);	\
			dr++; so += sizeof(guint32);				\
			reg = mono_regstate_next_int (cfg->rs);			\
			MONO_EMIT_NEW_LOAD_MEMBASE_OP(cfg, OP_LOAD_MEMBASE,	\
				reg, sr, so);					\
			mono_call_inst_add_outarg_reg(s, call, reg, dr, FALSE);	\
		break;								\
	}									\
} while (0)

#define MONO_OUTPUT_VTS(cfg, size, dr, dx, sr, so) do {				\
	int tmpr;								\
	switch (size) {								\
		case 0: 							\
			tmpr = mono_regstate_next_int (cfg->rs);		\
			MONO_EMIT_NEW_ICONST(cfg, tmpr, 0);			\
			MONO_EMIT_NEW_STORE_MEMBASE(cfg, OP_STORE_MEMBASE_REG,  \
				dr, dx, tmpr);					\
		break;								\
		case 1:								\
			tmpr = mono_regstate_next_int (cfg->rs);		\
			MONO_EMIT_NEW_LOAD_MEMBASE_OP(cfg, OP_LOADU1_MEMBASE,	\
				tmpr, sr, so);					\
			MONO_EMIT_NEW_STORE_MEMBASE(cfg, OP_STORE_MEMBASE_REG,  \
				dr, dx, tmpr);					\
		break;								\
		case 2:								\
			tmpr = mono_regstate_next_int (cfg->rs);		\
			MONO_EMIT_NEW_LOAD_MEMBASE_OP(cfg, OP_LOADU2_MEMBASE,	\
				tmpr, sr, so);					\
			MONO_EMIT_NEW_STORE_MEMBASE(cfg, OP_STORE_MEMBASE_REG,  \
				dr, dx, tmpr);					\
		break;								\
		case 4:								\
		case 8:								\
			MONO_EMIT_NEW_MOVE (cfg, dr, dx, sr, so, size);		\
		break;								\
	}									\
} while (0)

struct MonoLMF {
	gpointer    previous_lmf;
	gpointer    lmf_addr;
	MonoMethod *method;
	gulong      ebp;
	gulong      eip;
	gulong	    pregs[6];
	gulong	    gregs[16];
	gdouble     fregs[16];
};

typedef struct ucontext MonoContext;

typedef struct MonoCompileArch {
} MonoCompileArch;

typedef struct
{
	void *prev;
	void *unused[5];
	void *regs[8];
	void *return_address;
} MonoS390StackFrame;

typedef struct
{
	gint32	size;
	gint32	offset;
	gint32	offPrm;
} MonoS390ArgParm;

#define MONO_ARCH_EMULATE_FCONV_TO_I8 		1
#define MONO_ARCH_EMULATE_LCONV_TO_R8 		1
#define MONO_ARCH_EMULATE_LCONV_TO_R4 		1
#define MONO_ARCH_EMULATE_LCONV_TO_R8_UN 	1
#define MONO_ARCH_EMULATE_LMUL 			1
#define MONO_ARCH_HAVE_IS_INT_OVERFLOW  	1
#define MONO_ARCH_NEED_DIV_CHECK		1
// #define MONO_ARCH_SIGSEGV_ON_ALTSTACK		1
// #define MONO_ARCH_SIGNAL_STACK_SIZE		65536
// #define MONO_ARCH_HAVE_THROW_CORLIB_EXCEPTION	1

#define MONO_ARCH_USE_SIGACTION 	1
// #define CUSTOM_STACK_WALK 		1
// #define CUSTOM_EXCEPTION_HANDLING 	1
// #define mono_find_jit_info 		mono_arch_find_jit_info

#define S390_STACK_ALIGNMENT		 8
#define S390_FIRST_ARG_REG 		s390_r2
#define S390_LAST_ARG_REG 		s390_r6
#define S390_FIRST_FPARG_REG 		s390_f0
#define S390_LAST_FPARG_REG 		s390_f2
#define S390_PASS_STRUCTS_BY_VALUE 	 1
#define S390_SMALL_RET_STRUCT_IN_REG	 1

#define S390_NUM_REG_ARGS (S390_LAST_ARG_REG-S390_FIRST_ARG_REG+1)
#define S390_NUM_REG_FPARGS (S390_LAST_FPARG_REG-S390_FIRST_FPARG_REG)

/*===============================================*/
/* Definitions used by mini-codegen.c            */
/*===============================================*/

/*--------------------------------------------*/
/* use s390_r2-s390_r6 as parm registers      */
/* s390_r0, s390_r1, s390_r13 used internally */
/* s390_r15 is the stack pointer              */
/*--------------------------------------------*/
#define MONO_ARCH_CALLEE_REGS (0x1ffc)

#define MONO_ARCH_CALLEE_SAVED_REGS 0xff80

/*----------------------------------------*/
/* use s390_f1/s390_f3-s390_f15 as temps  */
/*----------------------------------------*/

#define MONO_ARCH_CALLEE_FREGS (0xfffe)

#define MONO_ARCH_CALLEE_SAVED_FREGS 0

#define MONO_ARCH_USE_FPSTACK FALSE
#define MONO_ARCH_FPSTACK_SIZE 0

#define MONO_ARCH_INST_FIXED_REG(desc) ((desc == 'o') ? s390_r2 : 		\
					((desc == 'L') ? s390_r3 :		\
					((desc == 'g') ? s390_f0 : - 1)))

#define MONO_ARCH_INST_IS_FLOAT(desc)  ((desc == 'f') || (desc == 'g'))

#define MONO_ARCH_INST_SREG2_MASK(ins) (0)

#define MONO_ARCH_INST_IS_REGPAIR(desc) ((desc == 'l') || (desc == 'L'))
//#define MONO_ARCH_INST_IS_REGPAIR(desc) (0)
#define MONO_ARCH_INST_REGPAIR_REG2(desc,hr) ((desc == 'l') ? (hr + 1) : 	\
					      ((desc == 'L') ? s390_r2 : -1))

#define MONO_ARCH_IS_GLOBAL_IREG(reg) 0

#define MONO_ARCH_FRAME_ALIGNMENT (sizeof (glong))
#define MONO_ARCH_CODE_ALIGNMENT 32

#define MONO_ARCH_BASEREG s390_r15
#define MONO_ARCH_RETREG1 s390_r2

#define MONO_SPARC_STACK_BIAS 0

/*-----------------------------------------------*/
/* Macros used to generate instructions          */
/*-----------------------------------------------*/
#define S390_OFFSET(b, t)	(guchar *) ((gint32) (b) - (gint32) (t))
#define S390_RELATIVE(b, t)     (guchar *) ((((gint32) (b) - (gint32) (t))) / 2)

#define CODEPTR(c, o) (o) = (short *) ((guint32) c - 2)
#define PTRSLOT(c, o) *(o) = (short) ((guint32) c - (guint32) (o) + 2)/2

#define S390_CC_EQ			8
#define S390_ALIGN(v, a)	(((a) > 0 ? (((v) + ((a) - 1)) & ~((a) - 1)) : (v)))

#define MONO_CONTEXT_SET_IP(ctx,ip) 					\
	do {								\
		(ctx)->uc_mcontext.gregs[14] = (unsigned long)ip;	\
		(ctx)->uc_mcontext.psw.addr = (unsigned long)ip;	\
	} while (0); 

#define MONO_CONTEXT_SET_SP(ctx,bp) MONO_CONTEXT_SET_BP((ctx),(bp))
#define MONO_CONTEXT_SET_BP(ctx,bp) 					\
	do {		 						\
		(ctx)->uc_mcontext.gregs[15] = (unsigned long)bp;	\
		(ctx)->uc_stack.ss_sp	     = (void*)bp;	\
	} while (0); 

#define MONO_CONTEXT_GET_IP(ctx) context_get_ip ((ctx))
#define MONO_CONTEXT_GET_BP(ctx) MONO_CONTEXT_GET_SP((ctx))
#define MONO_CONTEXT_GET_SP(ctx) ((gpointer)((ctx)->uc_mcontext.gregs[15]))

#define MONO_INIT_CONTEXT_FROM_FUNC(ctx,func) do {			\
		MonoS390StackFrame *sframe;				\
		__asm__ volatile("l     %0,0(15)" : "=r" (sframe));	\
		MONO_CONTEXT_SET_BP ((ctx), sframe->prev);		\
		sframe = (MonoS390StackFrame*)sframe->prev;		\
		MONO_CONTEXT_SET_IP ((ctx), sframe->return_address);	\
	} while (0)

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- context_get_ip                                    */
/*                                                                  */
/* Function	- Extract the current instruction address from the  */
/*		  context.                     		 	    */
/*		                               		 	    */
/*------------------------------------------------------------------*/

static inline gpointer 
context_get_ip (MonoContext *ctx) 
{
	gpointer ip;

	ip = (gpointer) ((gint32) (ctx->uc_mcontext.psw.addr) & 0x7fffffff);
	return ip;
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- s390_patch                                        */
/*                                                                  */
/* Function	- Patch the code with a given value.		    */
/*                                                                  */
/*------------------------------------------------------------------*/

static void inline
s390_patch (guchar *code, gint32 target)
{
	gint32 *offset = (gint32 *) code;
	
	if (target != 00) {
		*offset = target;
	}
}

/*========================= End of Function ========================*/

/*------------------------------------------------------------------*/
/*                                                                  */
/* Name		- restoreLMF                                        */
/*                                                                  */
/* Function	- Restore the LMF state prior to exiting a method.  */
/*                                                                  */
/*------------------------------------------------------------------*/

#define restoreLMF(code, frame_reg, stack_usage) do			\
{									\
	int lmfOffset = 0;						\
									\
	s390_lr  (code, s390_r13, frame_reg);				\
									\
	lmfOffset = stack_usage -  sizeof(MonoLMF);			\
									\
	/*-------------------------------------------------*/		\
	/* r13 = my lmf					   */		\
	/*-------------------------------------------------*/		\
	s390_ahi (code, s390_r13, lmfOffset);				\
									\
	/*-------------------------------------------------*/		\
	/* r6 = &jit_tls->lmf				   */		\
	/*-------------------------------------------------*/		\
	s390_l   (code, s390_r6, 0, s390_r13, 				\
		  G_STRUCT_OFFSET(MonoLMF, lmf_addr));			\
									\
	/*-------------------------------------------------*/		\
	/* r0 = lmf.previous_lmf			   */		\
	/*-------------------------------------------------*/		\
	s390_l   (code, s390_r0, 0, s390_r13, 				\
		  G_STRUCT_OFFSET(MonoLMF, previous_lmf));		\
									\
	/*-------------------------------------------------*/		\
	/* jit_tls->lmf = previous_lmf			   */		\
	/*-------------------------------------------------*/		\
	s390_l   (code, s390_r13, 0, s390_r6, 0);			\
	s390_st  (code, s390_r0, 0, s390_r6, 0);			\
} while (0)

/*========================= End of Function ========================*/

#endif /* __MONO_MINI_S390_H__ */  
