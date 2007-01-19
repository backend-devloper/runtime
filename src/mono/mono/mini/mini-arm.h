#ifndef __MONO_MINI_ARM_H__
#define __MONO_MINI_ARM_H__

#include <mono/arch/arm/arm-codegen.h>
#include <glib.h>

#ifdef ARM_FPU_NONE
#define MONO_ARCH_SOFT_FLOAT 1
#endif

#define MONO_ARCH_CPU_SPEC arm_cpu_desc

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define ARM_LSW_REG ARMREG_R0
#define ARM_MSW_REG ARMREG_R1
#else
#define ARM_LSW_REG ARMREG_R1
#define ARM_MSW_REG ARMREG_R0
#endif

#define MONO_MAX_IREGS 16
#define MONO_MAX_FREGS 16

#define MONO_SAVED_GREGS 10 /* r4-411, ip, lr */
#define MONO_SAVED_FREGS 8

/* r4-r11, ip, lr: registers saved in the LMF and MonoContext  */
#define MONO_ARM_REGSAVE_MASK 0x5ff0

/* Parameters used by the register allocator */
#define MONO_ARCH_HAS_XP_LOCAL_REGALLOC

#define MONO_ARCH_CALLEE_REGS ((1<<ARMREG_R0) | (1<<ARMREG_R1) | (1<<ARMREG_R2) | (1<<ARMREG_R3) | (1<<ARMREG_IP))
#define MONO_ARCH_CALLEE_SAVED_REGS ((1<<ARMREG_V1) | (1<<ARMREG_V2) | (1<<ARMREG_V3) | (1<<ARMREG_V4) | (1<<ARMREG_V5) | (1<<ARMREG_V6) | (1<<ARMREG_V7))

#define MONO_ARCH_CALLEE_FREGS 0xf
#define MONO_ARCH_CALLEE_SAVED_FREGS 0

#define MONO_ARCH_USE_FPSTACK FALSE
#define MONO_ARCH_FPSTACK_SIZE 0

#define MONO_ARCH_INST_SREG2_MASK(ins) (0)

#ifdef MONO_ARCH_SOFT_FLOAT
#define MONO_ARCH_INST_FIXED_REG(desc) (((desc) == 'l' || (desc == 'f') || (desc == 'g'))? ARM_LSW_REG: -1)
#define MONO_ARCH_INST_IS_REGPAIR(desc) ((desc) == 'l' || (desc) == 'L' || (desc) == 'f' || (desc) == 'g')
#define MONO_ARCH_INST_IS_FLOAT(desc) (FALSE)
#else
#define MONO_ARCH_INST_FIXED_REG(desc) (((desc) == 'l')? ARM_LSW_REG: -1)
#define MONO_ARCH_INST_IS_REGPAIR(desc) (desc == 'l' || desc == 'L')
#define MONO_ARCH_INST_IS_FLOAT(desc) ((desc == 'f') || (desc == 'g'))
#endif
#define MONO_ARCH_INST_REGPAIR_REG2(desc,hreg1) (desc == 'l'  || (desc == 'f') || (desc == 'g')? ARM_MSW_REG : -1)

#define MONO_ARCH_FRAME_ALIGNMENT 8

/* fixme: align to 16byte instead of 32byte (we align to 32byte to get 
 * reproduceable results for benchmarks */
#define MONO_ARCH_CODE_ALIGNMENT 32

void arm_patch (guchar *code, const guchar *target);
guint8* mono_arm_emit_load_imm (guint8 *code, int dreg, guint32 val);

/* keep the size of the structure a multiple of 8 */
struct MonoLMF {
	gpointer    previous_lmf;
	gpointer    lmf_addr;
	MonoMethod *method;
	gulong     esp;
	gulong     ebp;
	gulong     eip;
	gdouble    fregs [MONO_SAVED_FREGS]; /* 8..15 */
	/* all but sp and pc: matches the PUSH instruction layout in the trampolines
	 * 0-4 should be considered undefined (execpt in the magic tramp)
	 */
	gulong     iregs [14];
};

/* we define our own structure and we'll copy the data
 * from sigcontext/ucontext/mach when we need it.
 * This also makes us save stack space and time when copying
 * We might also want to add an additional field to propagate
 * the original context from the signal handler.
 */
typedef struct {
	gulong eip;          // pc 
	gulong ebp;          // fp
	gulong esp;          // sp
	gulong regs [MONO_SAVED_GREGS];
	double fregs [MONO_SAVED_FREGS];
} MonoContext;

typedef struct MonoCompileArch {
	int dummy;
} MonoCompileArch;

#define MONO_ARCH_EMULATE_FCONV_TO_I8 1
#define MONO_ARCH_EMULATE_LCONV_TO_R8 1
#define MONO_ARCH_EMULATE_LCONV_TO_R4 1
#define MONO_ARCH_EMULATE_LCONV_TO_R8_UN 1
#define MONO_ARCH_EMULATE_FREM 1
#define MONO_ARCH_EMULATE_DIV 1
#define MONO_ARCH_EMULATE_CONV_R8_UN 1
//#define MONO_ARCH_BIGMUL_INTRINS 1

#define ARM_FIRST_ARG_REG 0
#define ARM_LAST_ARG_REG 3

#define MONO_ARCH_USE_SIGACTION 1
#define MONO_ARCH_NEED_DIV_CHECK 1

#define ARM_NUM_REG_ARGS (ARM_LAST_ARG_REG-ARM_FIRST_ARG_REG+1)
#define ARM_NUM_REG_FPARGS 0

/* we have the stack pointer, not the base pointer in sigcontext */
#define MONO_CONTEXT_SET_IP(ctx,ip) do { (ctx)->eip = (int)ip; } while (0); 
#define MONO_CONTEXT_SET_BP(ctx,bp) do { (ctx)->ebp = (int)bp; } while (0); 
#define MONO_CONTEXT_SET_SP(ctx,bp) do { (ctx)->esp = (int)bp; } while (0); 

#define MONO_CONTEXT_GET_IP(ctx) ((gpointer)((ctx)->eip))
#define MONO_CONTEXT_GET_BP(ctx) ((gpointer)((ctx)->ebp))
#define MONO_CONTEXT_GET_SP(ctx) ((gpointer)((ctx)->esp))

#define MONO_INIT_CONTEXT_FROM_FUNC(ctx,func) do {	\
		MONO_CONTEXT_SET_BP ((ctx), __builtin_frame_address (0));	\
		MONO_CONTEXT_SET_IP ((ctx), (func));	\
	} while (0)

#endif /* __MONO_MINI_ARM_H__ */

