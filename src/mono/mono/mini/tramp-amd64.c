/*
 * tramp-x86.c: JIT trampoline code for x86
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/tabledefs.h>
#include <mono/arch/amd64/amd64-codegen.h>
#include <mono/metadata/mono-debug-debugger.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include "mini.h"
#include "mini-amd64.h"

typedef enum {
	MONO_TRAMPOLINE_GENERIC,
	MONO_TRAMPOLINE_JUMP,
	MONO_TRAMPOLINE_CLASS_INIT
} MonoTrampolineType;

/* adapt to mini later... */
#define mono_jit_share_code (1)

/*
 * Address of the trampoline code.  This is used by the debugger to check
 * whether a method is a trampoline.
 */
guint8 *mono_generic_trampoline_code = NULL;

/*
 * get_unbox_trampoline:
 * @m: method pointer
 * @addr: pointer to native code for @m
 *
 * when value type methods are called through the vtable we need to unbox the
 * this argument. This method returns a pointer to a trampoline which does
 * unboxing before calling the method
 */
static gpointer
get_unbox_trampoline (MonoMethod *m, gpointer addr)
{
	guint8 *code, *start;
	int this_reg = AMD64_RDI;

	if (!m->signature->ret->byref && MONO_TYPE_ISSTRUCT (m->signature->ret))
		this_reg = AMD64_RSI;
	    
	start = code = g_malloc (20);

	amd64_alu_reg_imm (code, X86_ADD, this_reg, sizeof (MonoObject));
	/* FIXME: Optimize this */
	amd64_mov_reg_imm (code, AMD64_RAX, addr);
	amd64_jump_reg (code, AMD64_RAX);
	g_assert ((code - start) < 20);

	mono_arch_flush_icache (start, code - start);

	return start;
}

/**
 * amd64_magic_trampoline:
 */
static gpointer
amd64_magic_trampoline (long *regs, guint8 *code, MonoMethod *m)
{
	gpointer addr;
	gpointer *vtable_slot;

	addr = mono_compile_method (m);
	g_assert (addr);

	//printf ("ENTER: %s\n", mono_method_full_name (m, TRUE));

	/* the method was jumped to */
	if (!code)
		return addr;

	vtable_slot = mono_amd64_get_vcall_slot_addr (code, regs);

	if (vtable_slot) {
		if (m->klass->valuetype)
			addr = get_unbox_trampoline (m, addr);

		/* FIXME: Fill in vtable slot */
	}
	else
		/* FIXME: Patch calling code */
		;

	return addr;
}

/**
 * amd64_class_init_trampoline:
 *
 * This method calls mono_runtime_class_init () to run the static constructor
 * for the type, then patches the caller code so it is not called again.
 */
static void
amd64_class_init_trampoline (long *regs, guint8 *code, MonoVTable *vtable)
{
	mono_runtime_class_init (vtable);

	/* FIXME: patch calling code */

#if 0
	code -= 5;
	if (code [0] == 0xe8) {
		if (!mono_running_on_valgrind ()) {
			guint32 ops;
			/*
			 * Thread safe code patching using the algorithm from the paper
			 * 'Practicing JUDO: Java Under Dynamic Optimizations'
			 */
			/* 
			 * First atomically change the the first 2 bytes of the call to a
			 * spinning jump.
			 */
			ops = 0xfeeb;
			InterlockedExchange ((gint32*)code, ops);

			/* Then change the other bytes to a nop */
			code [2] = 0x90;
			code [3] = 0x90;
			code [4] = 0x90;

			/* Then atomically change the first 4 bytes to a nop as well */
			ops = 0x90909090;
			InterlockedExchange ((guint32*)code, ops);

#ifdef HAVE_VALGRIND_MEMCHECK_H
			/* FIXME: the calltree skin trips on the self modifying code above */

			/* Tell valgrind to recompile the patched code */
			//VALGRIND_DISCARD_TRANSLATIONS (code, code + 8);
#endif
		}
	}
	else
		if (code [0] == 0x90 || code [0] == 0xeb)
			/* Already changed by another thread */
			;
		else {
			printf ("Invalid trampoline sequence: %x %x %x %x %x %x %x\n", code [0], code [1], code [2], code [3],
				code [4], code [5], code [6]);
			g_assert_not_reached ();
		}
#endif
}

static guchar*
create_trampoline_code (MonoTrampolineType tramp_type)
{
	guint8 *buf, *code, *tramp;
	int i;
	static guint8* generic_jump_trampoline = NULL;
	static guint8 *generic_class_init_trampoline = NULL;

	switch (tramp_type) {
	case MONO_TRAMPOLINE_GENERIC:
		if (mono_generic_trampoline_code)
			return mono_generic_trampoline_code;
		break;
	case MONO_TRAMPOLINE_JUMP:
		if (generic_jump_trampoline)
			return generic_jump_trampoline;
		break;
	case MONO_TRAMPOLINE_CLASS_INIT:
		if (generic_class_init_trampoline)
			return generic_class_init_trampoline;
		break;
	}

	code = buf = g_malloc (256);

	/* Allocate some stack space where we can save stuff */
	amd64_alu_reg_imm (buf, X86_SUB, X86_ESP, 512);

	/* Save the method/vtable received in RAX */
	amd64_mov_membase_reg (buf, X86_ESP, 512 - 8, AMD64_RAX, 8);

	/* FIXME: save lmf */

	/* FIXME: Save fp regs */

	/* Save argument registers */

	for (i = 0; i < AMD64_NREG; ++i)
		amd64_mov_membase_reg (buf, X86_ESP, (i * 8), i, 8);

	/* Arg1 is the pointer to the saved registers */
	amd64_lea_membase (buf, AMD64_RDI, AMD64_RSP, 0);

	/* Arg2 is the address of the calling code */
	amd64_mov_reg_membase (buf, AMD64_RSI, X86_ESP, 512, 8);

	/* Arg3 is the method/vtable ptr */
	amd64_mov_reg_membase (buf, AMD64_RDX, X86_ESP, 512 - 8, 8);

	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT)
		tramp = amd64_class_init_trampoline;
	else
		tramp = amd64_magic_trampoline;

	amd64_mov_reg_imm (buf, AMD64_RAX, tramp);
	amd64_call_reg (buf, AMD64_RAX);

	/* Restore argument registers */
	for (i = 0; i < AMD64_NREG; ++i)
		if (AMD64_IS_ARGUMENT_REG (i))
			amd64_mov_reg_membase (buf, i, X86_ESP, (i * 8), 8);

	/* Restore stack */
	amd64_alu_reg_imm (buf, X86_ADD, X86_ESP, 512);

	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT)
		amd64_ret (buf);
	else
		/* call the compiled method */
		amd64_jump_reg (buf, X86_EAX);

#if 0
	/* save caller save regs because we need to do a call */ 
	x86_push_reg (buf, X86_EDX);
	x86_push_reg (buf, X86_EAX);
	x86_push_reg (buf, X86_ECX);

	/* save LMF begin */

	/* save the IP (caller ip) */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		x86_push_imm (buf, 0);
	else
		x86_push_membase (buf, X86_ESP, 16);

	x86_push_reg (buf, X86_EBP);
	x86_push_reg (buf, X86_ESI);
	x86_push_reg (buf, X86_EDI);
	x86_push_reg (buf, X86_EBX);

	/* save method info */
	x86_push_membase (buf, X86_ESP, 32);
	/* get the address of lmf for the current thread */
	x86_call_code (buf, mono_get_lmf_addr);
	/* push lmf */
	x86_push_reg (buf, X86_EAX); 
	/* push *lfm (previous_lmf) */
	x86_push_membase (buf, X86_EAX, 0);
	/* *(lmf) = ESP */
	x86_mov_membase_reg (buf, X86_EAX, 0, X86_ESP, 4);
	/* save LFM end */

	/* push the method info */
	x86_push_membase (buf, X86_ESP, 44);
	/* push the return address onto the stack */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		x86_push_imm (buf, 0);
	else
		x86_push_membase (buf, X86_ESP, 52);

	/* save all register values */
	x86_push_reg (buf, X86_EBX);
	x86_push_reg (buf, X86_EDI);
	x86_push_reg (buf, X86_ESI);
	x86_push_membase (buf, X86_ESP, 64); /* EDX */
	x86_push_membase (buf, X86_ESP, 64); /* ECX */
	x86_push_membase (buf, X86_ESP, 64); /* EAX */

	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT)
		x86_call_code (buf, amd64_class_init_trampoline);
	else
		x86_call_code (buf, amd64_magic_trampoline);
	x86_alu_reg_imm (buf, X86_ADD, X86_ESP, 8*4);

	/* restore LMF start */
	/* ebx = previous_lmf */
	x86_pop_reg (buf, X86_EBX);
	/* edi = lmf */
	x86_pop_reg (buf, X86_EDI);
	/* *(lmf) = previous_lmf */
	x86_mov_membase_reg (buf, X86_EDI, 0, X86_EBX, 4);
	/* discard method info */
	x86_pop_reg (buf, X86_ESI);
	/* restore caller saved regs */
	x86_pop_reg (buf, X86_EBX);
	x86_pop_reg (buf, X86_EDI);
	x86_pop_reg (buf, X86_ESI);
	x86_pop_reg (buf, X86_EBP);

	/* discard save IP */
	x86_alu_reg_imm (buf, X86_ADD, X86_ESP, 4);		
	/* restore LMF end */

	x86_alu_reg_imm (buf, X86_ADD, X86_ESP, 16);
#endif

	g_assert ((buf - code) <= 256);

	switch (tramp_type) {
	case MONO_TRAMPOLINE_GENERIC:
		mono_generic_trampoline_code = code;
		break;
	case MONO_TRAMPOLINE_JUMP:
		generic_jump_trampoline = code;
		break;
	case MONO_TRAMPOLINE_CLASS_INIT:
		generic_class_init_trampoline = code;
		break;
	}

	return code;
}

#define TRAMPOLINE_SIZE 24

static MonoJitInfo*
create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain)
{
	MonoJitInfo *ji;
	guint8 *code, *buf, *tramp;

	tramp = create_trampoline_code (tramp_type);

	mono_domain_lock (domain);
	code = buf = mono_code_manager_reserve (domain->code_mp, TRAMPOLINE_SIZE);
	mono_domain_unlock (domain);

	amd64_mov_reg_imm (code, AMD64_RAX, arg1);
	/* FIXME: optimize this */
	amd64_mov_reg_imm (code, AMD64_R11, tramp);
	amd64_jump_reg (code, AMD64_R11);

	g_assert ((code - buf) <= TRAMPOLINE_SIZE);

	ji = g_new0 (MonoJitInfo, 1);
	ji->code_start = buf;
	ji->code_size = (code - buf) * 4;

	mono_jit_stats.method_trampolines++;

	mono_arch_flush_icache (ji->code_start, ji->code_size);

	return ji;
}	

MonoJitInfo*
mono_arch_create_jump_trampoline (MonoMethod *method)
{
	MonoJitInfo *ji = create_specific_trampoline (method, MONO_TRAMPOLINE_JUMP, mono_domain_get ());

	ji->method = method;
	return ji;
}

/**
 * mono_arch_create_jit_trampoline:
 * @method: pointer to the method info
 *
 * Creates a trampoline function for virtual methods. If the created
 * code is called it first starts JIT compilation of method,
 * and then calls the newly created method. I also replaces the
 * corresponding vtable entry (see amd64_magic_trampoline).
 * 
 * Returns: a pointer to the newly created code 
 */
gpointer
mono_arch_create_jit_trampoline (MonoMethod *method)
{
	MonoJitInfo *ji;

	/* previously created trampoline code */
	if (method->info)
		return method->info;

	if (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		return mono_arch_create_jit_trampoline (mono_marshal_get_synchronized_wrapper (method));

	ji = create_specific_trampoline (method, MONO_TRAMPOLINE_GENERIC, mono_domain_get ());
	method->info = ji->code_start;
	g_free (ji);

	return method->info;
}

/**
 * mono_arch_create_class_init_trampoline:
 *  @vtable: the type to initialize
 *
 * Creates a trampoline function to run a type initializer. 
 * If the trampoline is called, it calls mono_runtime_class_init with the
 * given vtable, then patches the caller code so it does not get called any
 * more.
 * 
 * Returns: a pointer to the newly created code 
 */
gpointer
mono_arch_create_class_init_trampoline (MonoVTable *vtable)
{
	MonoJitInfo *ji;
	gpointer code;

	ji = create_specific_trampoline (vtable, MONO_TRAMPOLINE_CLASS_INIT, vtable->domain);
	code = ji->code_start;
	g_free (ji);

	return code;
}

/*
 * This method is only called when running in the Mono Debugger.
 */
gpointer
mono_debugger_create_notification_function (gpointer *notification_address)
{
	guint8 *ptr, *buf;

	ptr = buf = g_malloc0 (16);
	x86_breakpoint (buf);
	if (notification_address)
		*notification_address = buf;
	x86_ret (buf);

	return ptr;
}

