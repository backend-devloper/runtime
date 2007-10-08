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
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-debug-debugger.h>
#include <mono/arch/x86/x86-codegen.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include "mini.h"
#include "mini-x86.h"

static guint8* nullified_class_init_trampoline;

/*
 * mono_arch_get_unbox_trampoline:
 * @m: method pointer
 * @addr: pointer to native code for @m
 *
 * when value type methods are called through the vtable we need to unbox the
 * this argument. This method returns a pointer to a trampoline which does
 * unboxing before calling the method
 */
gpointer
mono_arch_get_unbox_trampoline (MonoMethod *m, gpointer addr)
{
	guint8 *code, *start;
	int this_pos = 4;
	MonoDomain *domain = mono_domain_get ();

	if (!mono_method_signature (m)->ret->byref && MONO_TYPE_ISSTRUCT (mono_method_signature (m)->ret))
		this_pos = 8;
	    
	mono_domain_lock (domain);
	start = code = mono_code_manager_reserve (domain->code_mp, 16);
	mono_domain_unlock (domain);

	x86_alu_membase_imm (code, X86_ADD, X86_ESP, this_pos, sizeof (MonoObject));
	x86_jump_code (code, addr);
	g_assert ((code - start) < 16);

	return start;
}

void
mono_arch_patch_callsite (guint8 *code, guint8 *addr)
{
	/* go to the start of the call instruction
	 *
	 * address_byte = (m << 6) | (o << 3) | reg
	 * call opcode: 0xff address_byte displacement
	 * 0xff m=1,o=2 imm8
	 * 0xff m=2,o=2 imm32
	 */
	code -= 6;
	if ((code [1] == 0xe8)) {
		if (!mono_running_on_valgrind ()) {
			InterlockedExchange ((gint32*)(code + 2), (guint)addr - ((guint)code + 1) - 5);

#ifdef HAVE_VALGRIND_MEMCHECK_H
				/* Tell valgrind to recompile the patched code */
				//VALGRIND_DISCARD_TRANSLATIONS (code + 2, code + 6);
#endif
		}
	} else if (code [1] == 0xe9) {
		/* A PLT entry: jmp <DISP> */
		if (!mono_running_on_valgrind ())
			InterlockedExchange ((gint32*)(code + 2), (guint)addr - ((guint)code + 1) - 5);
	} else {
		printf ("Invalid trampoline sequence: %x %x %x %x %x %x %x\n", code [0], code [1], code [2], code [3],
				code [4], code [5], code [6]);
		g_assert_not_reached ();
	}
}

void
mono_arch_patch_plt_entry (guint8 *code, guint8 *addr)
{
	/* A PLT entry: jmp <DISP> */
	g_assert (code [0] == 0xe9);

	if (!mono_running_on_valgrind ())
		InterlockedExchange ((gint32*)(code + 1), (guint)addr - (guint)code - 5);
}

void
mono_arch_nullify_class_init_trampoline (guint8 *code, gssize *regs)
{
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
			InterlockedExchange ((gint32*)code, ops);
#ifdef HAVE_VALGRIND_MEMCHECK_H
			/* FIXME: the calltree skin trips on the self modifying code above */

			/* Tell valgrind to recompile the patched code */
			//VALGRIND_DISCARD_TRANSLATIONS (code, code + 8);
#endif
		}
	} else if (code [0] == 0x90 || code [0] == 0xeb) {
		/* Already changed by another thread */
		;
	} else if ((code [-1] == 0xff) && (x86_modrm_reg (code [0]) == 0x2)) {
		/* call *<OFFSET>(<REG>) -> Call made from AOT code */
		gpointer *vtable_slot;

		vtable_slot = mono_arch_get_vcall_slot_addr (code + 5, (gpointer*)regs);
		g_assert (vtable_slot);

		*vtable_slot = nullified_class_init_trampoline;
	} else {
			printf ("Invalid trampoline sequence: %x %x %x %x %x %x %x\n", code [0], code [1], code [2], code [3],
				code [4], code [5], code [6]);
			g_assert_not_reached ();
		}
}

void
mono_arch_nullify_plt_entry (guint8 *code)
{
	if (!mono_running_on_valgrind ()) {
		guint32 ops;

		ops = 0xfeeb;
		InterlockedExchange ((gint32*)code, ops);

		/* Then change the other bytes to a nop */
		code [2] = 0x90;
		code [3] = 0x90;
		code [4] = 0x90;

		/* Change the first byte to a nop */
		ops = 0xc3;
		InterlockedExchange ((gint32*)code, ops);
	}
}

guchar*
mono_arch_create_trampoline_code (MonoTrampolineType tramp_type)
{
	guint8 *buf, *code;
	int pushed_args;

	code = buf = mono_global_codeman_reserve (256);

	/* Note that there is a single argument to the trampoline
	 * and it is stored at: esp + pushed_args * sizeof (gpointer)
	 * the ret address is at: esp + (pushed_args + 1) * sizeof (gpointer)
	 */
	/* Put all registers into an array on the stack
	 * If this code is changed, make sure to update the offset value in
	 * mono_arch_find_this_argument () in mini-x86.c.
	 */
	x86_push_reg (buf, X86_EDI);
	x86_push_reg (buf, X86_ESI);
	x86_push_reg (buf, X86_EBP);
	x86_push_reg (buf, X86_ESP);
	x86_push_reg (buf, X86_EBX);
	x86_push_reg (buf, X86_EDX);
	x86_push_reg (buf, X86_ECX);
	x86_push_reg (buf, X86_EAX);

	pushed_args = 8;

	/* Align stack on apple */
	x86_alu_reg_imm (buf, X86_SUB, X86_ESP, 4);

	pushed_args ++;

	/* save LMF begin */

	/* save the IP (caller ip) */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		x86_push_imm (buf, 0);
	else
		x86_push_membase (buf, X86_ESP, (pushed_args + 1) * sizeof (gpointer));

	pushed_args++;

	x86_push_reg (buf, X86_EBP);
	x86_push_reg (buf, X86_ESI);
	x86_push_reg (buf, X86_EDI);
	x86_push_reg (buf, X86_EBX);

	pushed_args += 4;

	/* save ESP */
	x86_push_reg (buf, X86_ESP);
	/* Adjust ESP so it points to the previous frame */
	x86_alu_membase_imm (buf, X86_ADD, X86_ESP, 0, (pushed_args + 2) * 4);

	pushed_args ++;

	/* save method info */
	if ((tramp_type == MONO_TRAMPOLINE_GENERIC) || (tramp_type == MONO_TRAMPOLINE_JUMP))
		x86_push_membase (buf, X86_ESP, pushed_args * sizeof (gpointer));
	else
		x86_push_imm (buf, 0);

	pushed_args++;

	/* On apple, the stack is correctly aligned to 16 bytes because pushed_args is
	 * 16 and there is the extra trampoline arg + the return ip pushed by call
	 * FIXME: Note that if an exception happens while some args are pushed
	 * on the stack, the stack will be misaligned.
	 */
	g_assert (pushed_args == 16);

	/* get the address of lmf for the current thread */
	x86_call_code (buf, mono_get_lmf_addr);
	/* push lmf */
	x86_push_reg (buf, X86_EAX); 
	/* push *lfm (previous_lmf) */
	x86_push_membase (buf, X86_EAX, 0);
	/* Signal to mono_arch_find_jit_info () that this is a trampoline frame */
	x86_alu_membase_imm (buf, X86_ADD, X86_ESP, 0, 1);
	/* *(lmf) = ESP */
	x86_mov_membase_reg (buf, X86_EAX, 0, X86_ESP, 4);
	/* save LFM end */

	pushed_args += 2;

	/* starting the call sequence */

	/* FIXME: Push the trampoline address */
	x86_push_imm (buf, 0);

	pushed_args++;

	/* push the method info */
	x86_push_membase (buf, X86_ESP, pushed_args * sizeof (gpointer));

	pushed_args++;

	/* push the return address onto the stack */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		x86_push_imm (buf, 0);
	else
		x86_push_membase (buf, X86_ESP, (pushed_args + 1) * sizeof (gpointer));
	pushed_args++;
	/* push the address of the register array */
	x86_lea_membase (buf, X86_EAX, X86_ESP, (pushed_args - 8) * sizeof (gpointer));
	x86_push_reg (buf, X86_EAX);

	pushed_args++;

#ifdef __APPLE__
	/* check the stack is aligned after the ret ip is pushed */
	/*x86_mov_reg_reg (buf, X86_EDX, X86_ESP, 4);
	x86_alu_reg_imm (buf, X86_AND, X86_EDX, 15);
	x86_alu_reg_imm (buf, X86_CMP, X86_EDX, 0);
	x86_branch_disp (buf, X86_CC_Z, 3, FALSE);
	x86_breakpoint (buf);*/
#endif

	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT)
		x86_call_code (buf, mono_class_init_trampoline);
	else if (tramp_type == MONO_TRAMPOLINE_AOT)
		x86_call_code (buf, mono_aot_trampoline);
	else if (tramp_type == MONO_TRAMPOLINE_AOT_PLT)
		x86_call_code (buf, mono_aot_plt_trampoline);
	else if (tramp_type == MONO_TRAMPOLINE_DELEGATE)
		x86_call_code (buf, mono_delegate_trampoline);
	else
		x86_call_code (buf, mono_magic_trampoline);

	x86_alu_reg_imm (buf, X86_ADD, X86_ESP, 4*4);

	/* restore LMF start */
	/* ebx = previous_lmf */
	x86_pop_reg (buf, X86_EBX);
	x86_alu_reg_imm (buf, X86_SUB, X86_EBX, 1);
	/* edi = lmf */
	x86_pop_reg (buf, X86_EDI);
	/* *(lmf) = previous_lmf */
	x86_mov_membase_reg (buf, X86_EDI, 0, X86_EBX, 4);
	/* discard method info */
	x86_pop_reg (buf, X86_ESI);
	/* discard ESP */
	x86_pop_reg (buf, X86_ESI);
	/* restore caller saved regs */
	x86_pop_reg (buf, X86_EBX);
	x86_pop_reg (buf, X86_EDI);
	x86_pop_reg (buf, X86_ESI);
	x86_pop_reg (buf, X86_EBP);

	/* discard save IP */
	x86_alu_reg_imm (buf, X86_ADD, X86_ESP, 4);		
	/* restore LMF end */

	/* Restore caller saved registers */
	x86_mov_reg_membase (buf, X86_ECX, X86_ESP, 1 * 4, 4);
	x86_mov_reg_membase (buf, X86_EDX, X86_ESP, 2 * 4, 4);

	/* Pop saved reg array + stack align + method ptr */
	x86_alu_reg_imm (buf, X86_ADD, X86_ESP, 10 * 4);

	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT)
		x86_ret (buf);
	else
		/* call the compiled method */
		x86_jump_reg (buf, X86_EAX);

	g_assert ((buf - code) <= 256);

	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT) {
		/* Initialize the nullified class init trampoline used in the AOT case */
		nullified_class_init_trampoline = buf = mono_global_codeman_reserve (16);
		x86_ret (buf);
	}

	return code;
}

#define TRAMPOLINE_SIZE 10

gpointer
mono_arch_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len)
{
	guint8 *code, *buf, *tramp;
	
	tramp = mono_get_trampoline_code (tramp_type);

	mono_domain_lock (domain);
	code = buf = mono_code_manager_reserve_align (domain->code_mp, TRAMPOLINE_SIZE, 4);
	mono_domain_unlock (domain);

	x86_push_imm (buf, arg1);
	x86_jump_code (buf, tramp);
	g_assert ((buf - code) <= TRAMPOLINE_SIZE);

	mono_arch_flush_icache (code, buf - code);

	if (code_len)
		*code_len = buf - code;

	return code;
}

void
mono_arch_invalidate_method (MonoJitInfo *ji, void *func, gpointer func_arg)
{
	/* FIXME: This is not thread safe */
	guint8 *code = ji->code_start;

	x86_push_imm (code, func_arg);
	x86_call_code (code, (guint8*)func);
}

/*
 * This method is only called when running in the Mono Debugger.
 */
gpointer
mono_debugger_create_notification_function (void)
{
	guint8 *buf, *code;

	code = buf = mono_global_codeman_reserve (2);
	x86_breakpoint (buf);
	x86_ret (buf);
	return code;
}
