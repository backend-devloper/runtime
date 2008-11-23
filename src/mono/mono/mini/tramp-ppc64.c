/*
 * tramp-ppc.c: JIT trampoline code for PowerPC
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Paolo Molaro (lupus@ximian.com)
 *   Carlos Valiente <yo@virutass.net>
 *   Andreas Faerber <andreas.faerber@web.de>
 *
 * (C) 2001 Ximian, Inc.
 * (C) 2007-2008 Andreas Faerber
 */

#include <config.h>
#include <glib.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/tabledefs.h>
#include <mono/arch/ppc/ppc-codegen.h>

#include "mini.h"
#include "mini-ppc64.h"

/*
 * Return the instruction to jump from code to target, 0 if not
 * reachable with a single instruction
 */
static guint32
branch_for_target_reachable (guint8 *branch, guint8 *target)
{
	gint diff = target - branch;
	g_assert ((diff & 3) == 0);
	if (diff >= 0) {
		if (diff <= 33554431)
			return (18 << 26) | (diff);
	} else {
		/* diff between 0 and -33554432 */
		if (diff >= -33554432)
			return (18 << 26) | (diff & ~0xfc000000);
	}
	return 0;
}

/*
 * get_unbox_trampoline:
 * @gsctx: the generic sharing context
 * @m: method pointer
 * @addr: pointer to native code for @m
 *
 * when value type methods are called through the vtable we need to unbox the
 * this argument. This method returns a pointer to a trampoline which does
 * unboxing before calling the method
 */
gpointer
mono_arch_get_unbox_trampoline (MonoGenericSharingContext *gsctx, MonoMethod *m, gpointer addr)
{
	guint8 *code, *start;
	int this_pos = 3;
	guint32 short_branch;
	MonoDomain *domain = mono_domain_get ();

	addr = mono_get_addr_from_ftnptr (addr);

	if (MONO_TYPE_ISSTRUCT (mono_method_signature (m)->ret))
		this_pos = 4;
	    
	mono_domain_lock (domain);
	start = code = mono_code_manager_reserve (domain->code_mp, 20);
	short_branch = branch_for_target_reachable (code + 4, addr);
	if (short_branch)
		mono_code_manager_commit (domain->code_mp, code, 20, 8);
	mono_domain_unlock (domain);

	if (short_branch) {
		ppc_addi (code, this_pos, this_pos, sizeof (MonoObject));
		ppc_emit32 (code, short_branch);
	} else {
		ppc_load (code, ppc_r0, addr);
		ppc_mtctr (code, ppc_r0);
		ppc_addi (code, this_pos, this_pos, sizeof (MonoObject));
		ppc_bcctr (code, 20, 0);
	}
	mono_arch_flush_icache (start, code - start);
	mono_ppc_emitted (start, code - start, "unbox trampoline");
	g_assert ((code - start) <= 20);
	/*g_print ("unbox trampoline at %d for %s:%s\n", this_pos, m->klass->name, m->name);
	g_print ("unbox code is at %p for method at %p\n", start, addr);*/

	return mono_create_ftnptr (domain, start);
}

void
mono_arch_patch_callsite (guint8 *method_start, guint8 *code_ptr, guint8 *addr)
{
	guint32 *code = (guint32*)code_ptr;

	addr = mono_get_addr_from_ftnptr (addr);

	/* This is the 'blrl' instruction */
	--code;

	/*
	 * Note that methods are called also with the bl opcode.
	 */
	if (ppc_opcode(*code) == 18) {
		/*g_print ("direct patching\n");*/
		ppc_patch ((guint8*)code, addr);
		mono_arch_flush_icache ((guint8*)code, 4);
		return;
	}

	/* Sanity check: instruction must be 'blrl' */
	g_assert (mono_ppc_is_direct_call_sequence (code));

	ppc_patch ((guint8*)code, addr);
}

void
mono_arch_patch_plt_entry (guint8 *code, guint8 *addr)
{
	g_assert_not_reached ();
}

void
mono_arch_nullify_class_init_trampoline (guint8 *code, gssize *regs)
{
	return;
}

void
mono_arch_nullify_plt_entry (guint8 *code)
{
	g_assert_not_reached ();
}

/* Stack size for trampoline function 
 * PPC_MINIMAL_STACK_SIZE + 16 (args + alignment to ppc_magic_trampoline)
 * + MonoLMF + 14 fp regs + 13 gregs + alignment
 * #define STACK (PPC_MINIMAL_STACK_SIZE + 4 * sizeof (gulong) + sizeof (MonoLMF) + 14 * sizeof (double) + 13 * (sizeof (gulong)))
 * STACK would be 444 for 32 bit darwin
 */
#define STACK (PPC_MINIMAL_STACK_SIZE + 4 * sizeof (gulong) + sizeof (MonoLMF) + MONO_FIRST_SAVED_FREG * sizeof (double) + MONO_FIRST_SAVED_GREG * sizeof (gulong))

/* Method-specific trampoline code fragment size */
#define METHOD_TRAMPOLINE_SIZE 64

/* Jump-specific trampoline code fragment size */
#define JUMP_TRAMPOLINE_SIZE   64

/*
 * Stack frame description when the generic trampoline is called.
 * caller frame
 * --------------------
 *  MonoLMF
 *  -------------------
 *  Saved FP registers 0-13
 *  -------------------
 *  Saved general registers 0-12
 *  -------------------
 *  param area for 3 args to ppc_magic_trampoline
 *  -------------------
 *  linkage area
 *  -------------------
 */
guchar*
mono_arch_create_trampoline_code (MonoTrampolineType tramp_type)
{
	guint8 *buf, *code = NULL;
	int i, offset;
	gconstpointer tramp_handler;

	/* Now we'll create in 'buf' the PowerPC trampoline code. This
	   is the trampoline code common to all methods  */

	code = buf = mono_global_codeman_reserve (688);

	ppc_store_reg_update (buf, ppc_r1, -STACK, ppc_r1);

	/* start building the MonoLMF on the stack */
	offset = STACK - sizeof (double) * MONO_SAVED_FREGS;
	for (i = MONO_FIRST_SAVED_FREG; i <= MONO_LAST_SAVED_FREG; i++) {
		ppc_stfd (buf, i, offset, ppc_r1);
		offset += sizeof (double);
	}
	/* 
	 * now the integer registers.
	 */
	offset = STACK - sizeof (MonoLMF) + G_STRUCT_OFFSET (MonoLMF, iregs);
	for (i = MONO_FIRST_SAVED_GREG; i <= MONO_LAST_SAVED_GREG; i++) {
		ppc_store_reg (buf, i, offset, ppc_sp);
		offset += sizeof (gulong);
	}

	/* Now save the rest of the registers below the MonoLMF struct, first 14
	 * fp regs and then the 13 gregs.
	 */
	offset = STACK - sizeof (MonoLMF) - (MONO_FIRST_SAVED_FREG * sizeof (double));
	for (i = 0; i < MONO_FIRST_SAVED_FREG; i++) {
		ppc_stfd (buf, i, offset, ppc_r1);
		offset += sizeof (double);
	}
#define GREGS_OFFSET (STACK - sizeof (MonoLMF) - (MONO_FIRST_SAVED_FREG * sizeof (double)) - (MONO_FIRST_SAVED_GREG * sizeof (gulong)))
	offset = GREGS_OFFSET;
	for (i = 0; i < MONO_FIRST_SAVED_GREG; i++) {
		ppc_store_reg (buf, i, offset, ppc_r1);
		offset += sizeof (gulong);
	}
	/* we got here through a jump to the ctr reg, we must save the lr
	 * in the parent frame (we do it here to reduce the size of the
	 * method-specific trampoline)
	 */
	ppc_mflr (buf, ppc_r0);
	ppc_store_reg (buf, ppc_r0, STACK + PPC_RET_ADDR_OFFSET, ppc_r1);

	/* ok, now we can continue with the MonoLMF setup, mostly untouched
	 * from emit_prolog in mini-ppc.c
	 */
	ppc_load_func (buf, ppc_r0, mono_get_lmf_addr);
	ppc_mtlr (buf, ppc_r0);
	ppc_blrl (buf);
	/* we build the MonoLMF structure on the stack - see mini-ppc.h
	 * The pointer to the struct is put in ppc_r11.
	 */
	ppc_addi (buf, ppc_r11, ppc_sp, STACK - sizeof (MonoLMF));
	ppc_store_reg (buf, ppc_r3, G_STRUCT_OFFSET(MonoLMF, lmf_addr), ppc_r11);
	/* new_lmf->previous_lmf = *lmf_addr */
	ppc_load_reg (buf, ppc_r0, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r3);
	ppc_store_reg (buf, ppc_r0, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r11);
	/* *(lmf_addr) = r11 */
	ppc_store_reg (buf, ppc_r11, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r3);
	/* save method info (it's stored on the stack, so get it first and put it
	 * in r5 as it's the third argument to the function)
	 */
	if (tramp_type == MONO_TRAMPOLINE_GENERIC_CLASS_INIT)
		ppc_load_reg (buf, ppc_r5, GREGS_OFFSET + PPC_FIRST_ARG_REG * sizeof (gpointer), ppc_r1);
	else
		ppc_load_reg (buf, ppc_r5, GREGS_OFFSET, ppc_r1);
	if ((tramp_type == MONO_TRAMPOLINE_JIT) || (tramp_type == MONO_TRAMPOLINE_JUMP))
		ppc_store_reg (buf, ppc_r5, G_STRUCT_OFFSET(MonoLMF, method), ppc_r11);
	ppc_store_reg (buf, ppc_sp, G_STRUCT_OFFSET(MonoLMF, ebp), ppc_r11);
	/* save the IP (caller ip) */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		ppc_li (buf, ppc_r0, 0);
	else
		ppc_load_reg (buf, ppc_r0, STACK + PPC_RET_ADDR_OFFSET, ppc_r1);
	ppc_store_reg (buf, ppc_r0, G_STRUCT_OFFSET(MonoLMF, eip), ppc_r11);

	/*
	 * Now we're ready to call trampoline (gssize *regs, guint8 *code, gpointer value, guint8 *tramp)
	 * Note that the last argument is unused.
	 */
	/* Arg 1: a pointer to the registers */
	ppc_addi (buf, ppc_r3, ppc_r1, GREGS_OFFSET);
		
	/* Arg 2: code (next address to the instruction that called us) */
	if (tramp_type == MONO_TRAMPOLINE_JUMP)
		ppc_li (buf, ppc_r4, 0);
	else
		ppc_load_reg  (buf, ppc_r4, STACK + PPC_RET_ADDR_OFFSET, ppc_r1);

	/* Arg 3: MonoMethod *method. It was put in r5 already above */
	/*ppc_mr  (buf, ppc_r5, ppc_r5);*/

	tramp_handler = mono_get_trampoline_func (tramp_type);
	ppc_load_func (buf, ppc_r0, tramp_handler);
	ppc_mtlr (buf, ppc_r0);
	ppc_blrl (buf);
		
	/* OK, code address is now on r3. Move it to the counter reg
	 * so it will be ready for the final jump: this is safe since we
	 * won't do any more calls.
	 */
	if (tramp_type != MONO_TRAMPOLINE_RGCTX_LAZY_FETCH)
		if (tramp_type != MONO_TRAMPOLINE_DELEGATE)
			ppc_load_reg (buf, ppc_r3, 0, ppc_r3);
		ppc_mtctr (buf, ppc_r3);

	/*
	 * Now we restore the MonoLMF (see emit_epilogue in mini-ppc.c)
	 * and the rest of the registers, so the method called will see
	 * the same state as before we executed.
	 * The pointer to MonoLMF is in ppc_r11.
	 */
	ppc_addi (buf, ppc_r11, ppc_r1, STACK - sizeof (MonoLMF));
	/* r5 = previous_lmf */
	ppc_load_reg (buf, ppc_r5, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r11);
	/* r6 = lmf_addr */
	ppc_load_reg (buf, ppc_r6, G_STRUCT_OFFSET(MonoLMF, lmf_addr), ppc_r11);
	/* *(lmf_addr) = previous_lmf */
	ppc_store_reg (buf, ppc_r5, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r6);
	/* restore iregs */
	for (i = MONO_FIRST_SAVED_GREG; i <= MONO_LAST_SAVED_GREG; i++) {
		ppc_load_reg (buf, i, G_STRUCT_OFFSET(MonoLMF, iregs) +
			((i-MONO_FIRST_SAVED_GREG) * sizeof (gulong)), ppc_r11);
	}
	/* restore fregs */
	for (i = MONO_FIRST_SAVED_FREG; i <= MONO_LAST_SAVED_FREG; i++) {
		ppc_lfd (buf, i, G_STRUCT_OFFSET(MonoLMF, fregs) +
			((i-MONO_FIRST_SAVED_FREG) * sizeof (gdouble)), ppc_r11);
	}

	/* restore the volatile registers, we skip r1, of course */
	offset = STACK - sizeof (MonoLMF) - (MONO_FIRST_SAVED_FREG * sizeof (double));
	for (i = 0; i < MONO_FIRST_SAVED_FREG; i++) {
		ppc_lfd (buf, i, offset, ppc_r1);
		offset += sizeof (double);
	}
	offset = STACK - sizeof (MonoLMF) - (MONO_FIRST_SAVED_FREG * sizeof (double)) - (MONO_FIRST_SAVED_GREG * sizeof (gulong));
	ppc_load_reg (buf, ppc_r0, offset, ppc_r1);
	offset += 2 * sizeof (gulong);
	for (i = 2; i < MONO_FIRST_SAVED_GREG; i++) {
		if (i != 3 || tramp_type != MONO_TRAMPOLINE_RGCTX_LAZY_FETCH)
			ppc_load_reg (buf, i, offset, ppc_r1);
		offset += sizeof (gulong);
	}

	/* Non-standard function epilogue. Instead of doing a proper
	 * return, we just jump to the compiled code.
	 */
	/* Restore stack pointer and LR and jump to the code */
	ppc_load_reg  (buf, ppc_r1,  0, ppc_r1);
	ppc_load_reg  (buf, ppc_r11, PPC_RET_ADDR_OFFSET, ppc_r1);
	ppc_mtlr (buf, ppc_r11);
	if (tramp_type == MONO_TRAMPOLINE_CLASS_INIT ||
			tramp_type == MONO_TRAMPOLINE_GENERIC_CLASS_INIT ||
			tramp_type == MONO_TRAMPOLINE_RGCTX_LAZY_FETCH) {
		ppc_blr (buf);
	} else {
		ppc_bcctr (buf, 20, 0);
	}

	/* Flush instruction cache, since we've generated code */
	mono_arch_flush_icache (code, buf - code);
	mono_ppc_emitted (code, buf - code, "trampoline %d", tramp_type);

	/* Sanity check */
	g_assert ((buf - code) < 688);

	return code;
}

#define TRAMPOLINE_SIZE ((5+5+1+1)*4)
gpointer
mono_arch_create_specific_trampoline (gpointer arg1, MonoTrampolineType tramp_type, MonoDomain *domain, guint32 *code_len)
{
	guint8 *code, *buf, *tramp;
	guint32 short_branch;

	tramp = mono_get_trampoline_code (tramp_type);

	mono_domain_lock (domain);
	code = buf = mono_code_manager_reserve_align (domain->code_mp, TRAMPOLINE_SIZE, 4);
	short_branch = branch_for_target_reachable (code + 5 * 4, tramp);
	/* FIXME: make shorter if possible */
	mono_domain_unlock (domain);

	if (short_branch) {
		ppc_load_sequence (buf, ppc_r0, (gulong) arg1);
		ppc_emit32 (buf, short_branch);
	} else {
		/* Prepare the jump to the generic trampoline code.*/
		ppc_load (buf, ppc_r0, (gulong) tramp);
		ppc_mtctr (buf, ppc_r0);

		/* And finally put 'arg1' in r0 and fly! */
		ppc_load (buf, ppc_r0, (gulong) arg1);
		ppc_bcctr (buf, 20, 0);
	}
	
	/* Flush instruction cache, since we've generated code */
	mono_arch_flush_icache (code, buf - code);
	mono_ppc_emitted (code, buf - code, "specific trampoline %d arg %p", tramp_type, arg1);

	g_assert ((buf - code) <= TRAMPOLINE_SIZE);
	if (code_len)
		*code_len = buf - code;

	return code;
}

static guint8*
emit_trampoline_jump (guint8 *code, guint8 *tramp)
{
	guint32 short_branch = branch_for_target_reachable (code, tramp);

	/* FIXME: we can save a few bytes here by committing if the
	   short branch is possible */
	if (short_branch) {
		ppc_emit32 (code, short_branch);
	} else {
		ppc_load (code, ppc_r0, tramp);
		ppc_mtctr (code, ppc_r0);
		ppc_bcctr (code, 20, 0);
	}

	return code;
}

gpointer
mono_arch_create_rgctx_lazy_fetch_trampoline (guint32 slot)
{
#ifdef MONO_ARCH_VTABLE_REG
	guint8 *tramp;
	guint8 *code, *buf;
	guint8 **rgctx_null_jumps;
	int tramp_size;
	int depth, index;
	int i;
	gboolean mrgctx;

	mrgctx = MONO_RGCTX_SLOT_IS_MRGCTX (slot);
	index = MONO_RGCTX_SLOT_INDEX (slot);
	if (mrgctx)
		index += sizeof (MonoMethodRuntimeGenericContext) / sizeof (gpointer);
	for (depth = 0; ; ++depth) {
		int size = mono_class_rgctx_get_array_size (depth, mrgctx);

		if (index < size - 1)
			break;
		index -= size - 1;
	}

	tramp_size = 40 + 12 * depth;
	if (mrgctx)
		tramp_size += 4;
	else
		tramp_size += 12;

	code = buf = mono_global_codeman_reserve (tramp_size);

	rgctx_null_jumps = g_malloc (sizeof (guint8*) * (depth + 2));

	if (mrgctx) {
		/* get mrgctx ptr */
		ppc_mr (code, ppc_r4, PPC_FIRST_ARG_REG);
	} else {
		/* load rgctx ptr from vtable */
		ppc_load_reg (code, ppc_r4, G_STRUCT_OFFSET (MonoVTable, runtime_generic_context), PPC_FIRST_ARG_REG);
		/* is the rgctx ptr null? */
		ppc_cmpi (code, 0, 1, ppc_r4, 0);
		/* if yes, jump to actual trampoline */
		rgctx_null_jumps [0] = code;
		ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);
	}

	for (i = 0; i < depth; ++i) {
		/* load ptr to next array */
		if (mrgctx && i == 0)
			ppc_load_reg (code, ppc_r4, sizeof (MonoMethodRuntimeGenericContext), ppc_r4);
		else
			ppc_load_reg (code, ppc_r4, 0, ppc_r4);
		/* is the ptr null? */
		ppc_cmpi (code, 0, 1, ppc_r4, 0);
		/* if yes, jump to actual trampoline */
		rgctx_null_jumps [i + 1] = code;
		ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);
	}

	/* fetch slot */
	ppc_load_reg (code, ppc_r4, sizeof (gpointer) * (index  + 1), ppc_r4);
	/* is the slot null? */
	ppc_cmpi (code, 0, 1, ppc_r4, 0);
	/* if yes, jump to actual trampoline */
	rgctx_null_jumps [depth + 1] = code;
	ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);
	/* otherwise return r4 */
	/* FIXME: if we use r3 as the work register we can avoid this copy */
	ppc_mr (code, ppc_r3, ppc_r4);
	ppc_blr (code);

	for (i = mrgctx ? 1 : 0; i <= depth + 1; ++i)
		ppc_patch (rgctx_null_jumps [i], code);

	g_free (rgctx_null_jumps);

	/* move the rgctx pointer to the VTABLE register */
	ppc_mr (code, MONO_ARCH_VTABLE_REG, ppc_r3);

	tramp = mono_arch_create_specific_trampoline (GUINT_TO_POINTER (slot),
			MONO_TRAMPOLINE_RGCTX_LAZY_FETCH, mono_get_root_domain (), NULL);

	/* jump to the actual trampoline */
	code = emit_trampoline_jump (code, tramp);

	mono_arch_flush_icache (buf, code - buf);
	mono_ppc_emitted (buf, code - buf, "rgctx lazy fetch trampoline slot %d", slot);

	g_assert (code - buf <= tramp_size);

	return buf;
#else
	g_assert_not_reached ();
#endif
}

gpointer
mono_arch_create_generic_class_init_trampoline (void)
{
	guint8 *tramp;
	guint8 *code, *buf;
	static int byte_offset = -1;
	static guint8 bitmask;
	guint8 *jump;
	int tramp_size;

	tramp_size = 32;

	code = buf = mono_global_codeman_reserve (tramp_size);

	if (byte_offset < 0)
		mono_marshal_find_bitfield_offset (MonoVTable, initialized, &byte_offset, &bitmask);

	ppc_lbz (code, ppc_r4, byte_offset, PPC_FIRST_ARG_REG);
	ppc_andid (code, ppc_r4, ppc_r4, bitmask);
	jump = code;
	ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);

	ppc_blr (code);

	ppc_patch (jump, code);

	tramp = mono_arch_create_specific_trampoline (NULL, MONO_TRAMPOLINE_GENERIC_CLASS_INIT,
			mono_get_root_domain (), NULL);

	/* jump to the actual trampoline */
	code = emit_trampoline_jump (code, tramp);

	mono_arch_flush_icache (buf, code - buf);
	mono_ppc_emitted (buf, code - buf, "generic class init trampoline");

	g_assert (code - buf <= tramp_size);

	return buf;
}
