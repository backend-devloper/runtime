//
// mini-llvm-cpp.cpp: C++ support classes for the mono LLVM integration
//
// (C) 2009-2011 Novell, Inc.
// Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
//

//
// We need to override some stuff in LLVM, but this cannot be done using the C
// interface, so we have to use some C++ code here.
// The things which we override are:
// - the default JIT code manager used by LLVM doesn't allocate memory using
//   MAP_32BIT, we require it.
// - add some callbacks so we can obtain the size of methods and their exception
//   tables.
//

//
// Mono's internal header files are not C++ clean, so avoid including them if 
// possible
//

#include "config.h"

#include <stdint.h>

#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include "mini-llvm-cpp.h"

using namespace llvm;

void
mono_llvm_dump_value (LLVMValueRef value)
{
	/* Same as LLVMDumpValue (), but print to stdout */
	fflush (stdout);
	outs () << (*unwrap<Value> (value));
}

/* Missing overload for building an alloca with an alignment */
LLVMValueRef
mono_llvm_build_alloca (LLVMBuilderRef builder, LLVMTypeRef Ty, 
						LLVMValueRef ArraySize,
						int alignment, const char *Name)
{
	return wrap (unwrap (builder)->Insert (new AllocaInst (unwrap (Ty), unwrap (ArraySize), alignment), Name));
}

LLVMValueRef 
mono_llvm_build_load (LLVMBuilderRef builder, LLVMValueRef PointerVal,
					  const char *Name, gboolean is_volatile, BarrierKind barrier)
{
	LoadInst *ins = unwrap(builder)->CreateLoad(unwrap(PointerVal), is_volatile, Name);

	switch (barrier) {
	case LLVM_BARRIER_NONE:
		break;
	case LLVM_BARRIER_ACQ:
		ins->setOrdering(Acquire);
		break;
	case LLVM_BARRIER_SEQ:
		ins->setOrdering(SequentiallyConsistent);
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	return wrap(ins);
}

LLVMValueRef 
mono_llvm_build_aligned_load (LLVMBuilderRef builder, LLVMValueRef PointerVal,
							  const char *Name, gboolean is_volatile, int alignment)
{
	LoadInst *ins;

	ins = unwrap(builder)->CreateLoad(unwrap(PointerVal), is_volatile, Name);
	ins->setAlignment (alignment);

	return wrap(ins);
}

LLVMValueRef 
mono_llvm_build_store (LLVMBuilderRef builder, LLVMValueRef Val, LLVMValueRef PointerVal,
					  gboolean is_volatile, BarrierKind barrier)
{
	StoreInst *ins = unwrap(builder)->CreateStore(unwrap(Val), unwrap(PointerVal), is_volatile);

	switch (barrier) {
	case LLVM_BARRIER_NONE:
		break;
	case LLVM_BARRIER_REL:
		ins->setOrdering(Release);
		break;
	case LLVM_BARRIER_SEQ:
		ins->setOrdering(SequentiallyConsistent);
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	return wrap(ins);
}

LLVMValueRef 
mono_llvm_build_aligned_store (LLVMBuilderRef builder, LLVMValueRef Val, LLVMValueRef PointerVal,
							   gboolean is_volatile, int alignment)
{
	StoreInst *ins;

	ins = unwrap(builder)->CreateStore(unwrap(Val), unwrap(PointerVal), is_volatile);
	ins->setAlignment (alignment);

	return wrap (ins);
}

LLVMValueRef
mono_llvm_build_cmpxchg (LLVMBuilderRef builder, LLVMValueRef ptr, LLVMValueRef cmp, LLVMValueRef val)
{
	AtomicCmpXchgInst *ins;

	ins = unwrap(builder)->CreateAtomicCmpXchg (unwrap(ptr), unwrap (cmp), unwrap (val), SequentiallyConsistent, SequentiallyConsistent);
	return wrap (ins);
}

LLVMValueRef
mono_llvm_build_atomic_rmw (LLVMBuilderRef builder, AtomicRMWOp op, LLVMValueRef ptr, LLVMValueRef val)
{
	AtomicRMWInst::BinOp aop = AtomicRMWInst::Xchg;
	AtomicRMWInst *ins;

	switch (op) {
	case LLVM_ATOMICRMW_OP_XCHG:
		aop = AtomicRMWInst::Xchg;
		break;
	case LLVM_ATOMICRMW_OP_ADD:
		aop = AtomicRMWInst::Add;
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	ins = unwrap (builder)->CreateAtomicRMW (aop, unwrap (ptr), unwrap (val), SequentiallyConsistent);
	return wrap (ins);
}

LLVMValueRef
mono_llvm_build_fence (LLVMBuilderRef builder, BarrierKind kind)
{
	FenceInst *ins;
	AtomicOrdering ordering;

	g_assert (kind != LLVM_BARRIER_NONE);

	switch (kind) {
	case LLVM_BARRIER_ACQ:
		ordering = Acquire;
		break;
	case LLVM_BARRIER_REL:
		ordering = Release;
		break;
	case LLVM_BARRIER_SEQ:
		ordering = SequentiallyConsistent;
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	ins = unwrap (builder)->CreateFence (ordering);
	return wrap (ins);
}

void
mono_llvm_set_must_tail (LLVMValueRef call_ins)
{
	CallInst *ins = (CallInst*)unwrap (call_ins);

	ins->setTailCallKind (CallInst::TailCallKind::TCK_MustTail);
}

void
mono_llvm_replace_uses_of (LLVMValueRef var, LLVMValueRef v)
{
	Value *V = ConstantExpr::getTruncOrBitCast (unwrap<Constant> (v), unwrap (var)->getType ());
	unwrap (var)->replaceAllUsesWith (V);
}

LLVMValueRef
mono_llvm_create_constant_data_array (const uint8_t *data, int len)
{
	return wrap(ConstantDataArray::get (*unwrap(LLVMGetGlobalContext ()), makeArrayRef(data, len)));
}

void
mono_llvm_set_is_constant (LLVMValueRef global_var)
{
	unwrap<GlobalVariable>(global_var)->setConstant (true);
}

void
mono_llvm_set_preserveall_cc (LLVMValueRef func)
{
	unwrap<Function>(func)->setCallingConv (CallingConv::PreserveAll);
}

void
mono_llvm_set_call_preserveall_cc (LLVMValueRef func)
{
	unwrap<CallInst>(func)->setCallingConv (CallingConv::PreserveAll);
}
