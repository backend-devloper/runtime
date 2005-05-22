#ifndef __MONO_MINI_IA64_H__
#define __MONO_MINI_IA64_H__

#include <glib.h>

#include <mono/arch/ia64/ia64-codegen.h>

/* FIXME: regset -> 128 bits ! */

#define MONO_MAX_IREGS 128
#define MONO_MAX_FREGS 128

/* Parameters used by the register allocator */

#define MONO_ARCH_HAS_XP_LOCAL_REGALLOC

/* r8..r11, r14..r30 */
#define MONO_ARCH_CALLEE_REGS (0x700UL | 0xefffc000UL)

/* f6..f15, f32..f127 */
/* FIXME: Use the upper 64 bits as well */
#define MONO_ARCH_CALLEE_FREGS (0xffffffffffffffff0000UL | (0x3ffUL << 6))

#define MONO_ARCH_CALLEE_SAVED_REGS ~(MONO_ARCH_CALLEE_REGS)

#define MONO_ARCH_CALLEE_SAVED_FREGS 0

#define MONO_ARCH_USE_FPSTACK FALSE
#define MONO_ARCH_FPSTACK_SIZE 0

#define MONO_ARCH_INST_FIXED_REG(desc) -1
#define MONO_ARCH_INST_SREG2_MASK(ins) (0)
#define MONO_ARCH_INST_IS_REGPAIR(desc) FALSE
#define MONO_ARCH_INST_REGPAIR_REG2(desc,hreg1) (-1)

#define MONO_ARCH_FRAME_ALIGNMENT 16

#define MONO_ARCH_CODE_ALIGNMENT 16

/* FIXME: */
#define MONO_ARCH_BASEREG IA64_GP
#define MONO_ARCH_RETREG1 IA64_R8

struct MonoLMF {
	gpointer    previous_lmf;
	gpointer    lmf_addr;
	MonoMethod *method;
	gpointer    ip;
	gpointer    sp;
	gpointer    ebp;
};

typedef struct MonoContext {
	guint8 *ip;
	gpointer *sp;
	gpointer *fp;
} MonoContext;

typedef struct MonoCompileArch {
	gint32 lmf_offset;
	gint32 localloc_offset;
	gint32 reg_in0;
	gint32 reg_local0;
	gint32 reg_out0;
} MonoCompileArch;

#define MONO_CONTEXT_SET_IP(ctx,eip) do { (ctx)->ip = (gpointer)(eip); } while (0); 
#define MONO_CONTEXT_SET_BP(ctx,ebp) do { (ctx)->fp = (gpointer*)(ebp); } while (0); 
#define MONO_CONTEXT_SET_SP(ctx,esp) do { (ctx)->sp = (gpointer*)(esp); } while (0); 

#define MONO_CONTEXT_GET_IP(ctx) ((gpointer)((ctx)->ip))
#define MONO_CONTEXT_GET_BP(ctx) ((gpointer)((ctx)->fp))
#define MONO_CONTEXT_GET_SP(ctx) ((gpointer)((ctx)->sp))

#define MONO_INIT_CONTEXT_FROM_FUNC(ctx,start_func) do {	\
		mono_arch_flush_register_windows ();	\
		MONO_CONTEXT_SET_IP ((ctx), (start_func));	\
		MONO_CONTEXT_SET_BP ((ctx), __builtin_frame_address (0));	\
	} while (0)

#define MONO_ARCH_USE_SIGACTION 1

#ifdef HAVE_WORKING_SIGALTSTACK
/* FIXME: */
//#define MONO_ARCH_SIGSEGV_ON_ALTSTACK
#endif


#define MONO_ARCH_NO_EMULATE_LONG_SHIFT_OPS

#define MONO_ARCH_EMULATE_CONV_R8_UN    1
#define MONO_ARCH_EMULATE_LCONV_TO_R8_UN 1
#define MONO_ARCH_EMULATE_FREM 1
#define MONO_ARCH_HAVE_IS_INT_OVERFLOW 1

#define MONO_ARCH_ENABLE_EMIT_STATE_OPT 1
#define MONO_ARCH_HAVE_INVALIDATE_METHOD 1
#define MONO_ARCH_HAVE_THROW_CORLIB_EXCEPTION 1
#define MONO_ARCH_HAVE_PIC_AOT 1
#define MONO_ARCH_HAVE_CREATE_TRAMPOLINE_FROM_TOKEN 1

/* FIXME: */
#define MONO_ARCH_EMULATE_DIV           1

#endif /* __MONO_MINI_IA64_H__ */  
