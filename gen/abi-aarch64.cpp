//===-- abi-aarch64.cpp ---------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//
//
// The Procedure Call Standard can be found here:
// http://infocenter.arm.com/help/topic/com.arm.doc.ihi0055b/IHI0055B_aapcs64.pdf
//
//===----------------------------------------------------------------------===//

#include "ldcbindings.h"
#include "gen/abi.h"
#include "gen/abi-generic.h"
#include "gen/abi-aarch64.h"

struct AArch64TargetABI : TargetABI {
  HFAToArray hfaToArray;
  CompositeToArray64 compositeToArray64;
  IntegerRewrite integerRewrite;

  bool returnInArg(TypeFunction *tf) override {
    if (tf->isref) {
      return false;
    }

    Type *rt = tf->next->toBasetype();

    // FIXME
    if (tf->linkage == LINKd)
      return rt->ty == Tsarray || rt->ty == Tstruct;

    return rt->ty == Tsarray ||
      (rt->ty == Tstruct && rt->size() > 16 && !isHFA((TypeStruct *)rt));
  }

  bool passByVal(Type *t) override {
    t = t->toBasetype();
    return t->ty == Tsarray ||
           (t->ty == Tstruct && t->size() > 16 && !isHFA((TypeStruct *)t));
  }

  void rewriteFunctionType(TypeFunction *tf, IrFuncTy &fty) override {
    Type *retTy = fty.ret->type->toBasetype();
    if (!fty.ret->byref && retTy->ty == Tstruct) {
      // Rewrite HFAs only because union HFAs are turned into IR types that are
      // non-HFA and messes up register selection
      if (isHFA((TypeStruct *)retTy, &fty.ret->ltype)) {
        fty.ret->rewrite = &hfaToArray;
        fty.ret->ltype = hfaToArray.type(fty.ret->type, fty.ret->ltype);
      }
      else {
        fty.ret->rewrite = &integerRewrite;
        fty.ret->ltype = integerRewrite.type(fty.ret->type, fty.ret->ltype);
      }
    }

    for (auto arg : fty.args) {
      if (!arg->byref)
        rewriteArgument(fty, *arg);
      else if (passByVal(arg->type))
        arg->attrs.remove(LLAttribute::ByVal);
    }
  }

  void rewriteArgument(IrFuncTy &fty, IrFuncTyArg &arg) override {
    // FIXME
    Type *ty = arg.type->toBasetype();

    if (ty->ty == Tstruct || ty->ty == Tsarray) {
      // Rewrite HFAs only because union HFAs are turned into IR types that are
      // non-HFA and messes up register selection
      if (ty->ty == Tstruct && isHFA((TypeStruct *)ty, &arg.ltype)) {
        arg.rewrite = &hfaToArray;
        arg.ltype = hfaToArray.type(arg.type, arg.ltype);
      }
      else {
        arg.rewrite = &compositeToArray64;
        arg.ltype = compositeToArray64.type(arg.type, arg.ltype);
      }
    }
  }

  /**
  * The AACPS64 uses a special native va_list type:
  *
  * typedef struct __va_list {
  *     void *__stack; // next stack param
  *     void *__gr_top; // end of GP arg reg save area
  *     void *__vr_top; // end of FP/SIMD arg reg save area
  *     int __gr_offs; // offset from __gr_top to next GP register arg
  *     int __vr_offs; // offset from __vr_top to next FP/SIMD register arg
  * } va_list;
  *
  * In druntime, the struct is defined as core.stdc.stdarg.__va_list; the
  * actually used core.stdc.stdarg.va_list type is a raw char* pointer though to
  * achieve byref semantics.
  * This requires a little bit of compiler magic in the following
  * implementations.
  */

  LLType *getValistType() {
    LLType *intType = LLType::getInt32Ty(gIR->context());
    LLType *voidPointerType = getVoidPtrType();

    std::vector<LLType *> parts;      // struct __va_list {
    parts.push_back(voidPointerType); //   void *__stack;
    parts.push_back(voidPointerType); //   void *__gr_top;
    parts.push_back(voidPointerType); //   void *__vr_top;
    parts.push_back(intType);         //   int __gr_offs;
    parts.push_back(intType);         //   int __vr_offs; };

    return LLStructType::get(gIR->context(), parts);
  }

  LLValue *prepareVaStart(LLValue *pAp) override {
    // Since the user only created a char* pointer (ap) on the stack before
    // invoking va_start, we first need to allocate the actual __va_list struct
    // and set 'ap' to its address.
    LLValue *valistmem = DtoRawAlloca(getValistType(), 0, "__va_list_mem");
    valistmem = DtoBitCast(valistmem, getVoidPtrType());
    DtoStore(valistmem, DtoBitCast(pAp, getPtrToType(getVoidPtrType())));

    // pass a void* pointer to the actual struct to LLVM's va_start intrinsic
    return valistmem;
  }

  void vaCopy(LLValue *pDest, LLValue *src) override {
    // Analog to va_start, we need to allocate a new __va_list struct on the
    // stack, fill it with a bitcopy of the source struct...
    src = DtoLoad(
        DtoBitCast(src, getValistType()->getPointerTo())); // *(__va_list*)src
    LLValue *valistmem = DtoAllocaDump(src, 0, "__va_list_mem");
    // ... and finally set the passed 'dest' char* pointer to the new struct's
    // address.
    DtoStore(DtoBitCast(valistmem, getVoidPtrType()),
             DtoBitCast(pDest, getPtrToType(getVoidPtrType())));
  }

  LLValue *prepareVaArg(LLValue *pAp) override {
    // pass a void* pointer to the actual __va_list struct to LLVM's va_arg
    // intrinsic
    return DtoLoad(pAp);
  }

  Type *vaListType() override {
    // We need to pass the actual va_list type for correct mangling. Simply
    // using TypeIdentifier here is a bit wonky but works, as long as the name
    // is actually available in the scope (this is what DMD does, so if a better
    // solution is found there, this should be adapted).
    static const llvm::StringRef ident = "__va_list";
    return (createTypeIdentifier(Loc(), Identifier::idPool(ident.data(), ident.size())));
  }

  const char *objcMsgSendFunc(Type *ret, IrFuncTy &fty) override {
    // see objc/message.h for objc_msgSend selection rules
    return "objc_msgSend";
  }
};

// The public getter for abi.cpp
TargetABI *getAArch64TargetABI() { return new AArch64TargetABI(); }
