/*
 * exceptions-x86.c: exception support for x86
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <signal.h>
#include <string.h>

#include <mono/arch/x86/x86-codegen.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/gc-internal.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-debug-debugger.h>

#include "mini.h"
#include "mini-x86.h"

#ifdef PLATFORM_WIN32
static MonoW32ExceptionHandler fpe_handler;
static MonoW32ExceptionHandler ill_handler;
static MonoW32ExceptionHandler segv_handler;

static LPTOP_LEVEL_EXCEPTION_FILTER old_handler;

#define W32_SEH_HANDLE_EX(_ex) \
	if (_ex##_handler) _ex##_handler((int)sctx)

/*
 * Unhandled Exception Filter
 * Top-level per-process exception handler.
 */
LONG CALLBACK seh_handler(EXCEPTION_POINTERS* ep)
{
	EXCEPTION_RECORD* er;
	CONTEXT* ctx;
	struct sigcontext* sctx;
	LONG res;

	res = EXCEPTION_CONTINUE_EXECUTION;

	er = ep->ExceptionRecord;
	ctx = ep->ContextRecord;
	sctx = g_malloc(sizeof(struct sigcontext));

	/* Copy Win32 context to UNIX style context */
	sctx->eax = ctx->Eax;
	sctx->ebx = ctx->Ebx;
	sctx->ecx = ctx->Ecx;
	sctx->edx = ctx->Edx;
	sctx->ebp = ctx->Ebp;
	sctx->esp = ctx->Esp;
	sctx->esi = ctx->Esi;
	sctx->edi = ctx->Edi;
	sctx->eip = ctx->Eip;

	switch (er->ExceptionCode) {
	case EXCEPTION_ACCESS_VIOLATION:
		W32_SEH_HANDLE_EX(segv);
		break;
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		W32_SEH_HANDLE_EX(ill);
		break;
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_INT_OVERFLOW:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_OVERFLOW:
	case EXCEPTION_FLT_UNDERFLOW:
	case EXCEPTION_FLT_INEXACT_RESULT:
		W32_SEH_HANDLE_EX(fpe);
		break;
	default:
		break;
	}

	/* Copy context back */
	ctx->Eax = sctx->eax;
	ctx->Ebx = sctx->ebx;
	ctx->Ecx = sctx->ecx;
	ctx->Edx = sctx->edx;
	ctx->Ebp = sctx->ebp;
	ctx->Esp = sctx->esp;
	ctx->Esi = sctx->esi;
	ctx->Edi = sctx->edi;
	ctx->Eip = sctx->eip;

	return res;
}

void win32_seh_init()
{
	old_handler = SetUnhandledExceptionFilter(seh_handler);
}

void win32_seh_cleanup()
{
	if (old_handler) SetUnhandledExceptionFilter(old_handler);
}

void win32_seh_set_handler(int type, MonoW32ExceptionHandler handler)
{
	switch (type) {
	case SIGFPE:
		fpe_handler = handler;
		break;
	case SIGILL:
		ill_handler = handler;
		break;
	case SIGSEGV:
		segv_handler = handler;
		break;
	default:
		break;
	}
}

#endif /* PLATFORM_WIN32 */

/*
 * mono_arch_get_restore_context:
 *
 * Returns a pointer to a method which restores a previously saved sigcontext.
 */
gpointer
mono_arch_get_restore_context (void)
{
	static guint8 *start = NULL;
	guint8 *code;

	if (start)
		return start;

	/* restore_contect (MonoContext *ctx) */
	/* we do not restore X86_EAX, X86_EDX */

	start = code = mono_global_codeman_reserve (128);
	
	/* load ctx */
	x86_mov_reg_membase (code, X86_EAX, X86_ESP, 4, 4);

	/* get return address, stored in EDX */
	x86_mov_reg_membase (code, X86_EDX, X86_EAX,  G_STRUCT_OFFSET (MonoContext, eip), 4);
	/* restore EBX */
	x86_mov_reg_membase (code, X86_EBX, X86_EAX,  G_STRUCT_OFFSET (MonoContext, ebx), 4);
	/* restore EDI */
	x86_mov_reg_membase (code, X86_EDI, X86_EAX,  G_STRUCT_OFFSET (MonoContext, edi), 4);
	/* restore ESI */
	x86_mov_reg_membase (code, X86_ESI, X86_EAX,  G_STRUCT_OFFSET (MonoContext, esi), 4);
	/* restore ESP */
	x86_mov_reg_membase (code, X86_ESP, X86_EAX,  G_STRUCT_OFFSET (MonoContext, esp), 4);
	/* restore EBP */
	x86_mov_reg_membase (code, X86_EBP, X86_EAX,  G_STRUCT_OFFSET (MonoContext, ebp), 4);

	/* jump to the saved IP */
	x86_jump_reg (code, X86_EDX);

	return start;
}

/*
 * mono_arch_get_call_filter:
 *
 * Returns a pointer to a method which calls an exception filter. We
 * also use this function to call finally handlers (we pass NULL as 
 * @exc object in this case).
 */
gpointer
mono_arch_get_call_filter (void)
{
	static guint8* start;
	static int inited = 0;
	guint8 *code;

	if (inited)
		return start;

	inited = 1;
	/* call_filter (MonoContext *ctx, unsigned long eip) */
	start = code = mono_global_codeman_reserve (64);

	x86_push_reg (code, X86_EBP);
	x86_mov_reg_reg (code, X86_EBP, X86_ESP, 4);
	x86_push_reg (code, X86_EBX);
	x86_push_reg (code, X86_EDI);
	x86_push_reg (code, X86_ESI);

	/* load ctx */
	x86_mov_reg_membase (code, X86_EAX, X86_EBP, 8, 4);
	/* load eip */
	x86_mov_reg_membase (code, X86_ECX, X86_EBP, 12, 4);
	/* save EBP */
	x86_push_reg (code, X86_EBP);

	/* set new EBP */
	x86_mov_reg_membase (code, X86_EBP, X86_EAX,  G_STRUCT_OFFSET (MonoContext, ebp), 4);
	/* restore registers used by global register allocation (EBX & ESI) */
	x86_mov_reg_membase (code, X86_EBX, X86_EAX,  G_STRUCT_OFFSET (MonoContext, ebx), 4);
	x86_mov_reg_membase (code, X86_ESI, X86_EAX,  G_STRUCT_OFFSET (MonoContext, esi), 4);
	x86_mov_reg_membase (code, X86_EDI, X86_EAX,  G_STRUCT_OFFSET (MonoContext, edi), 4);

	/* call the handler */
	x86_call_reg (code, X86_ECX);

	/* restore EBP */
	x86_pop_reg (code, X86_EBP);

	/* restore saved regs */
	x86_pop_reg (code, X86_ESI);
	x86_pop_reg (code, X86_EDI);
	x86_pop_reg (code, X86_EBX);
	x86_leave (code);
	x86_ret (code);

	g_assert ((code - start) < 64);
	return start;
}

static void
throw_exception (unsigned long eax, unsigned long ecx, unsigned long edx, unsigned long ebx,
		 unsigned long esi, unsigned long edi, unsigned long ebp, MonoObject *exc,
		 unsigned long eip,  unsigned long esp, gboolean rethrow)
{
	static void (*restore_context) (MonoContext *);
	MonoContext ctx;

	if (!restore_context)
		restore_context = mono_arch_get_restore_context ();

	/* Pop argument and return address */
	ctx.esp = esp + (2 * sizeof (gpointer));
	ctx.eip = eip;
	ctx.ebp = ebp;
	ctx.edi = edi;
	ctx.esi = esi;
	ctx.ebx = ebx;
	ctx.edx = edx;
	ctx.ecx = ecx;
	ctx.eax = eax;

	if (mono_debugger_throw_exception ((gpointer)(eip - 5), (gpointer)esp, exc)) {
		/*
		 * The debugger wants us to stop on the `throw' instruction.
		 * By the time we get here, it already inserted a breakpoint on
		 * eip - 5 (which is the address of the call).
		 */
		ctx.eip = eip - 5;
		ctx.esp = esp + sizeof (gpointer);
		restore_context (&ctx);
		g_assert_not_reached ();
	}

	/* adjust eip so that it point into the call instruction */
	ctx.eip -= 1;

	if (mono_object_isinst (exc, mono_defaults.exception_class)) {
		MonoException *mono_ex = (MonoException*)exc;
		if (!rethrow)
			mono_ex->stack_trace = NULL;
	}
	mono_handle_exception (&ctx, exc, (gpointer)eip, FALSE);
	restore_context (&ctx);

	g_assert_not_reached ();
}

static guint8*
get_throw_exception (gboolean rethrow)
{
	guint8 *start, *code;

	start = code = mono_global_codeman_reserve (64);

	x86_push_reg (code, X86_ESP);
	x86_push_membase (code, X86_ESP, 4); /* IP */
	x86_push_membase (code, X86_ESP, 12); /* exception */
	x86_push_reg (code, X86_EBP);
	x86_push_reg (code, X86_EDI);
	x86_push_reg (code, X86_ESI);
	x86_push_reg (code, X86_EBX);
	x86_push_reg (code, X86_EDX);
	x86_push_reg (code, X86_ECX);
	x86_push_reg (code, X86_EAX);
	x86_call_code (code, throw_exception);
	/* we should never reach this breakpoint */
	x86_breakpoint (code);

	g_assert ((code - start) < 64);

	return start;
}

/**
 * mono_arch_get_throw_exception:
 *
 * Returns a function pointer which can be used to raise 
 * exceptions. The returned function has the following 
 * signature: void (*func) (MonoException *exc); 
 * For example to raise an arithmetic exception you can use:
 *
 * x86_push_imm (code, mono_get_exception_arithmetic ()); 
 * x86_call_code (code, arch_get_throw_exception ()); 
 *
 */
gpointer 
mono_arch_get_throw_exception (void)
{
	static guint8 *start;
	static int inited = 0;

	if (inited)
		return start;

	start = get_throw_exception (FALSE);

	inited = 1;

	return start;
}

gpointer 
mono_arch_get_rethrow_exception (void)
{
	static guint8 *start;
	static int inited = 0;

	if (inited)
		return start;

	start = get_throw_exception (TRUE);

	inited = 1;

	return start;
}

/**
 * mono_arch_get_throw_exception_by_name:
 *
 * Returns a function pointer which can be used to raise 
 * corlib exceptions. The returned function has the following 
 * signature: void (*func) (gpointer ip, char *exc_name); 
 * For example to raise an arithmetic exception you can use:
 *
 * x86_push_imm (code, "ArithmeticException"); 
 * x86_push_imm (code, <IP>)
 * x86_jump_code (code, arch_get_throw_exception_by_name ()); 
 *
 */
gpointer 
mono_arch_get_throw_exception_by_name (void)
{
	static guint8* start;
	static int inited = 0;
	guint8 *code;

	if (inited)
		return start;

	inited = 1;
	code = start = mono_global_codeman_reserve (32);

	x86_push_membase (code, X86_ESP, 4); /* exception name */
	x86_push_imm (code, "System");
	x86_push_imm (code, mono_defaults.exception_class->image);
	x86_call_code (code, mono_exception_from_name);
	x86_alu_reg_imm (code, X86_ADD, X86_ESP, 12);
	/* save the newly create object (overwrite exception name)*/
	x86_mov_membase_reg (code, X86_ESP, 4, X86_EAX, 4);
	x86_jump_code (code, mono_arch_get_throw_exception ());

	g_assert ((code - start) < 32);

	return start;
}

/**
 * mono_arch_get_throw_corlib_exception:
 *
 * Returns a function pointer which can be used to raise 
 * corlib exceptions. The returned function has the following 
 * signature: void (*func) (guint32 ex_token, guint32 offset); 
 * Here, offset is the offset which needs to be substracted from the caller IP 
 * to get the IP of the throw. Passing the offset has the advantage that it 
 * needs no relocations in the caller.
 */
gpointer 
mono_arch_get_throw_corlib_exception (void)
{
	static guint8* start;
	static int inited = 0;
	guint8 *code;

	if (inited)
		return start;

	inited = 1;
	code = start = mono_global_codeman_reserve (64);

	x86_push_membase (code, X86_ESP, 4); /* token */
	x86_push_imm (code, mono_defaults.exception_class->image);
	x86_call_code (code, mono_exception_from_token);
	x86_alu_reg_imm (code, X86_ADD, X86_ESP, 8);
	/* Compute caller ip */
	x86_pop_reg (code, X86_ECX);
	/* Pop token */
	x86_alu_reg_imm (code, X86_ADD, X86_ESP, 4);
	x86_pop_reg (code, X86_EDX);
	x86_alu_reg_reg (code, X86_SUB, X86_ECX, X86_EDX);
	/* Push exception object */
	x86_push_reg (code, X86_EAX);
	/* Push throw IP */
	x86_push_reg (code, X86_ECX);
	x86_jump_code (code, mono_arch_get_throw_exception ());

	g_assert ((code - start) < 64);

	return start;
}

/* mono_arch_find_jit_info:
 *
 * This function is used to gather information from @ctx. It return the 
 * MonoJitInfo of the corresponding function, unwinds one stack frame and
 * stores the resulting context into @new_ctx. It also stores a string 
 * describing the stack location into @trace (if not NULL), and modifies
 * the @lmf if necessary. @native_offset return the IP offset from the 
 * start of the function or -1 if that info is not available.
 */
MonoJitInfo *
mono_arch_find_jit_info (MonoDomain *domain, MonoJitTlsData *jit_tls, MonoJitInfo *res, MonoJitInfo *prev_ji, MonoContext *ctx, 
			 MonoContext *new_ctx, char **trace, MonoLMF **lmf, int *native_offset,
			 gboolean *managed)
{
	MonoJitInfo *ji;
	gpointer ip = MONO_CONTEXT_GET_IP (ctx);

	/* Avoid costly table lookup during stack overflow */
	if (prev_ji && (ip > prev_ji->code_start && ((guint8*)ip < ((guint8*)prev_ji->code_start) + prev_ji->code_size)))
		ji = prev_ji;
	else
		ji = mono_jit_info_table_find (domain, ip);

	if (managed)
		*managed = FALSE;

	if (ji != NULL) {
		int offset;

		*new_ctx = *ctx;

		if (managed)
			if (!ji->method->wrapper_type)
				*managed = TRUE;

		/*
		 * Some managed methods like pinvoke wrappers might have save_lmf set.
		 * In this case, register save/restore code is not generated by the 
		 * JIT, so we have to restore callee saved registers from the lmf.
		 */
		if (ji->method->save_lmf) {
			/* 
			 * We only need to do this if the exception was raised in managed
			 * code, since otherwise the lmf was already popped of the stack.
			 */
			if (*lmf && (MONO_CONTEXT_GET_BP (ctx) >= (gpointer)(*lmf)->ebp)) {
				new_ctx->esi = (*lmf)->esi;
				new_ctx->edi = (*lmf)->edi;
				new_ctx->ebx = (*lmf)->ebx;
			}
		}
		else {
			offset = -1;
			/* restore caller saved registers */
			if (ji->used_regs & X86_EBX_MASK) {
				new_ctx->ebx = *((int *)ctx->ebp + offset);
				offset--;
			}
			if (ji->used_regs & X86_EDI_MASK) {
				new_ctx->edi = *((int *)ctx->ebp + offset);
				offset--;
			}
			if (ji->used_regs & X86_ESI_MASK) {
				new_ctx->esi = *((int *)ctx->ebp + offset);
			}
		}

		if (*lmf && (MONO_CONTEXT_GET_BP (ctx) >= (gpointer)(*lmf)->ebp)) {
			/* remove any unused lmf */
			*lmf = (*lmf)->previous_lmf;
		}

		/* Pop EBP and the return address */
		new_ctx->esp = ctx->ebp + (2 * sizeof (gpointer));
		/* we substract 1, so that the IP points into the call instruction */
		new_ctx->eip = *((int *)ctx->ebp + 1) - 1;
		new_ctx->ebp = *((int *)ctx->ebp);

		/* Pop arguments off the stack */
		{
			MonoJitArgumentInfo *arg_info = g_newa (MonoJitArgumentInfo, mono_method_signature (ji->method)->param_count + 1);

			guint32 stack_to_pop = mono_arch_get_argument_info (mono_method_signature (ji->method), mono_method_signature (ji->method)->param_count, arg_info);
			new_ctx->esp += stack_to_pop;
		}

		return ji;
	} else if (*lmf) {
		
		*new_ctx = *ctx;

		if (!(*lmf)->method)
			return (gpointer)-1;

		if ((ji = mono_jit_info_table_find (domain, (gpointer)(*lmf)->eip))) {
		} else {
			memset (res, 0, sizeof (MonoJitInfo));
			res->method = (*lmf)->method;
		}

		new_ctx->esi = (*lmf)->esi;
		new_ctx->edi = (*lmf)->edi;
		new_ctx->ebx = (*lmf)->ebx;
		new_ctx->ebp = (*lmf)->ebp;
		new_ctx->eip = (*lmf)->eip;
		/* the lmf is always stored on the stack, so the following
		 * expression points to a stack location which can be used as ESP */
		new_ctx->esp = (unsigned long)&((*lmf)->eip);

		*lmf = (*lmf)->previous_lmf;

		return ji ? ji : res;
	}

	return NULL;
}

void
mono_arch_sigctx_to_monoctx (void *sigctx, MonoContext *mctx)
{
#ifdef MONO_ARCH_USE_SIGACTION
	ucontext_t *ctx = (ucontext_t*)sigctx;
	
	mctx->eax = ctx->uc_mcontext.gregs [REG_EAX];
	mctx->ebx = ctx->uc_mcontext.gregs [REG_EBX];
	mctx->ecx = ctx->uc_mcontext.gregs [REG_ECX];
	mctx->edx = ctx->uc_mcontext.gregs [REG_EDX];
	mctx->ebp = ctx->uc_mcontext.gregs [REG_EBP];
	mctx->esp = ctx->uc_mcontext.gregs [REG_ESP];
	mctx->esi = ctx->uc_mcontext.gregs [REG_ESI];
	mctx->edi = ctx->uc_mcontext.gregs [REG_EDI];
	mctx->eip = ctx->uc_mcontext.gregs [REG_EIP];
#else	
	struct sigcontext *ctx = (struct sigcontext *)sigctx;

	mctx->eax = ctx->SC_EAX;
	mctx->ebx = ctx->SC_EBX;
	mctx->ecx = ctx->SC_ECX;
	mctx->edx = ctx->SC_EDX;
	mctx->ebp = ctx->SC_EBP;
	mctx->esp = ctx->SC_ESP;
	mctx->esi = ctx->SC_ESI;
	mctx->edi = ctx->SC_EDI;
	mctx->eip = ctx->SC_EIP;
#endif
}

void
mono_arch_monoctx_to_sigctx (MonoContext *mctx, void *sigctx)
{
#ifdef MONO_ARCH_USE_SIGACTION
	ucontext_t *ctx = (ucontext_t*)sigctx;

	ctx->uc_mcontext.gregs [REG_EAX] = mctx->eax;
	ctx->uc_mcontext.gregs [REG_EBX] = mctx->ebx;
	ctx->uc_mcontext.gregs [REG_ECX] = mctx->ecx;
	ctx->uc_mcontext.gregs [REG_EDX] = mctx->edx;
	ctx->uc_mcontext.gregs [REG_EBP] = mctx->ebp;
	ctx->uc_mcontext.gregs [REG_ESP] = mctx->esp;
	ctx->uc_mcontext.gregs [REG_ESI] = mctx->esi;
	ctx->uc_mcontext.gregs [REG_EDI] = mctx->edi;
	ctx->uc_mcontext.gregs [REG_EIP] = mctx->eip;
#else
	struct sigcontext *ctx = (struct sigcontext *)sigctx;

	ctx->SC_EAX = mctx->eax;
	ctx->SC_EBX = mctx->ebx;
	ctx->SC_ECX = mctx->ecx;
	ctx->SC_EDX = mctx->edx;
	ctx->SC_EBP = mctx->ebp;
	ctx->SC_ESP = mctx->esp;
	ctx->SC_ESI = mctx->esi;
	ctx->SC_EDI = mctx->edi;
	ctx->SC_EIP = mctx->eip;
#endif
}	

gpointer
mono_arch_ip_from_context (void *sigctx)
{
#ifdef MONO_ARCH_USE_SIGACTION
	ucontext_t *ctx = (ucontext_t*)sigctx;
	return (gpointer)ctx->uc_mcontext.gregs [REG_EIP];
#else
	struct sigcontext *ctx = sigctx;
	return (gpointer)ctx->SC_EIP;
#endif	
}

gboolean
mono_arch_handle_exception (void *sigctx, gpointer obj, gboolean test_only)
{
	MonoContext mctx;

	mono_arch_sigctx_to_monoctx (sigctx, &mctx);

	mono_handle_exception (&mctx, obj, (gpointer)mctx.eip, test_only);

	mono_arch_monoctx_to_sigctx (&mctx, sigctx);

	return TRUE;
}
