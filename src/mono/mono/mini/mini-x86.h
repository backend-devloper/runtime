#ifndef __MONO_MINI_X86_H__
#define __MONO_MINI_X86_H__

#include <mono/arch/x86/x86-codegen.h>

#define MONO_MAX_IREGS 8
#define MONO_MAX_FREGS 6

#define MONO_ARCH_FRAME_ALIGNMENT 4

/* fixme: align to 16byte instead of 32byte (we align to 32byte to get 
 * reproduceable results for benchmarks */
#define MONO_ARCH_CODE_ALIGNMENT 32

#define MONO_ARCH_BASEREG X86_EBP
#define MONO_ARCH_RETREG1 X86_EAX
#define MONO_ARCH_RETREG2 X86_EDX

#define MONO_ARCH_ENCODE_LREG(r1,r2) (r1 | (r2<<3))

#define inst_dreg_low dreg&7 
#define inst_dreg_high dreg>>3
#define inst_sreg1_low sreg1&7 
#define inst_sreg1_high sreg1>>3
#define inst_sreg2_low sreg2&7 
#define inst_sreg2_high sreg2>>3

struct MonoLMF {
	gpointer    previous_lmf;
	gpointer    lmf_addr;
	MonoMethod *method;
	guint32     ebp;
	guint32     esi;
	guint32     edi;
	guint32     ebx;
	guint32     eip;
};

typedef struct MonoCompileArch {
} MonoCompileArch;

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
# define SC_EAX sc_eax
# define SC_EBX sc_ebx
# define SC_ECX sc_ecx
# define SC_EDX sc_edx
# define SC_EBP sc_ebp
# define SC_EIP sc_eip
# define SC_ESP sc_esp
# define SC_EDI sc_edi
# define SC_ESI sc_esi
#else
# define SC_EAX eax
# define SC_EBX ebx
# define SC_ECX ecx
# define SC_EDX edx
# define SC_EBP ebp
# define SC_EIP eip
# define SC_ESP esp
# define SC_EDI edi
# define SC_ESI esi
#endif

typedef struct sigcontext MonoContext;

#define MONO_CONTEXT_SET_IP(ctx,ip) do { (ctx)->SC_EIP = (long)(ip); } while (0); 
#define MONO_CONTEXT_SET_BP(ctx,bp) do { (ctx)->SC_EBP = (long)(bp); } while (0); 
#define MONO_CONTEXT_SET_SP(ctx,esp) do { (ctx)->SC_ESP = (long)(esp); } while (0); 

#define MONO_CONTEXT_GET_IP(ctx) ((gpointer)((ctx)->SC_EIP))
#define MONO_CONTEXT_GET_BP(ctx) ((gpointer)((ctx)->SC_EBP))
#define MONO_CONTEXT_GET_SP(ctx) ((gpointer)((ctx)->SC_ESP))

#ifndef PLATFORM_WIN32

#ifdef HAVE_WORKING_SIGALTSTACK
#define MONO_ARCH_SIGSEGV_ON_ALTSTACK
/* NetBSD doesn't define SA_STACK */
#ifndef SA_STACK
#define SA_STACK SA_ONSTACK
#endif
#endif

#endif

#define MONO_ARCH_BIGMUL_INTRINS 1

#endif /* __MONO_MINI_X86_H__ */  
