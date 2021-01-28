//===--- GenThunk.cpp - IR Generation for Method Dispatch Thunks ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for class and protocol method dispatch
//  thunks, which are used in resilient builds to hide vtable and witness table
//  offsets from clients.
//
//===----------------------------------------------------------------------===//

#include "Callee.h"
#include "CallEmission.h"
#include "ClassMetadataVisitor.h"
#include "ConstantBuilder.h"
#include "Explosion.h"
#include "GenCall.h"
#include "GenClass.h"
#include "GenDecl.h"
#include "GenHeap.h"
#include "GenOpaque.h"
#include "GenPointerAuth.h"
#include "GenProto.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "MetadataLayout.h"
#include "NativeConventionSchema.h"
#include "ProtocolInfo.h"
#include "Signature.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILDeclRef.h"
#include "llvm/IR/Function.h"

using namespace swift;
using namespace irgen;

/// Find the entry point for a method dispatch thunk.
llvm::Function *
IRGenModule::getAddrOfDispatchThunk(SILDeclRef declRef,
                                    ForDefinition_t forDefinition) {
  LinkEntity entity = LinkEntity::forDispatchThunk(declRef);

  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  auto fnType = getSILModule().Types.getConstantFunctionType(
      getMaximalTypeExpansionContext(), declRef);
  Signature signature = getSignature(fnType);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);

  entry = createFunction(*this, link, signature);
  return entry;
}

namespace {

class IRGenThunk {
  IRGenFunction &IGF;
  SILDeclRef declRef;
  TypeExpansionContext expansionContext;
  CanSILFunctionType origTy;
  CanSILFunctionType substTy;
  SubstitutionMap subMap;
  bool isAsync;
  bool isCoroutine;
  bool isWitnessMethod;

  Optional<AsyncContextLayout> asyncLayout;

  // Initialized by prepareArguments()
  llvm::Value *indirectReturnSlot = nullptr;
  llvm::Value *selfValue = nullptr;
  llvm::Value *errorResult = nullptr;
  WitnessMetadata witnessMetadata;
  Explosion params;

  void prepareArguments();
  Callee lookupMethod();

public:
  IRGenThunk(IRGenFunction &IGF, SILDeclRef declRef);
  void emit();
};

} // end namespace

IRGenThunk::IRGenThunk(IRGenFunction &IGF, SILDeclRef declRef)
  : IGF(IGF), declRef(declRef),
    expansionContext(IGF.IGM.getMaximalTypeExpansionContext()) {
  auto &Types = IGF.IGM.getSILModule().Types;
  origTy = Types.getConstantFunctionType(expansionContext, declRef);

  if (auto *genericEnv = Types.getConstantGenericEnvironment(declRef))
    subMap = genericEnv->getForwardingSubstitutionMap();

  substTy = origTy->substGenericArgs(
      IGF.IGM.getSILModule(), subMap, expansionContext);

  isAsync = origTy->isAsync();
  isCoroutine = origTy->isCoroutine();

  auto *decl = cast<AbstractFunctionDecl>(declRef.getDecl());
  isWitnessMethod = isa<ProtocolDecl>(decl->getDeclContext());

  if (isAsync) {
    asyncLayout.emplace(irgen::getAsyncContextLayout(
        IGF.IGM, origTy, substTy, subMap));
  }
}

// FIXME: This duplicates the structure of CallEmission. It should be
// possible to refactor some code and simplify this drastically, since
// conceptually all we're doing is forwarding the arguments verbatim
// using the sync or async calling convention.
void IRGenThunk::prepareArguments() {
  if (isAsync) {
    assert(!isCoroutine);

    assert(asyncLayout->hasLocalContext());
    auto context = asyncLayout->emitCastTo(IGF, IGF.getAsyncContext());
    auto localContextAddr =
        asyncLayout->getLocalContextLayout().project(
            IGF, context, llvm::None);
    selfValue = IGF.Builder.CreateLoad(localContextAddr);

    if (isWitnessMethod) {
      assert(asyncLayout->hasTrailingWitnesses());
      auto context = asyncLayout->emitCastTo(
          IGF, IGF.getAsyncContext());

      auto metadataAddr =
          asyncLayout->getSelfMetadataLayout().project(
              IGF, context, llvm::None);
      witnessMetadata.SelfMetadata = IGF.Builder.CreateLoad(metadataAddr);

      auto wtableAddr =
          asyncLayout->getSelfWitnessTableLayout().project(
              IGF, context, llvm::None);
      witnessMetadata.SelfWitnessTable = IGF.Builder.CreateLoad(wtableAddr);
    }

    if (origTy->hasErrorResult()) {
      Address addr = asyncLayout->getErrorLayout().project(
          IGF, context, llvm::None);
      IGF.setCallerErrorResultSlot(addr.getAddress());
    }

    for (unsigned i = 0, e = asyncLayout->getIndirectReturnCount(); i < e; ++i) {
      Address addr = asyncLayout->getIndirectReturnLayout(i).project(
          IGF, context, llvm::None);
      params.add(IGF.Builder.CreateLoad(addr));
    }

    for (unsigned i = 0, e = asyncLayout->getArgumentCount(); i < e; ++i) {
      Address addr = asyncLayout->getArgumentLayout(i).project(
          IGF, context, llvm::None);
      params.add(IGF.Builder.CreateLoad(addr));
    }

    if (asyncLayout->hasBindings()) {
      Address addr = asyncLayout->getBindingsLayout().project(
          IGF, context, llvm::None);
      asyncLayout->getBindings().save(IGF, addr, params);
    }
  } else {
    Explosion original = IGF.collectParameters();

    if (isWitnessMethod) {
      witnessMetadata.SelfWitnessTable = original.takeLast();
      witnessMetadata.SelfMetadata = original.takeLast();
    }

    if (origTy->hasErrorResult()) {
      errorResult = original.takeLast();
      IGF.setCallerErrorResultSlot(errorResult);
    }

    if (isCoroutine) {
      original.transferInto(params, 1);
    }

    selfValue = original.takeLast();

    // Prepare indirect results, if any.
    SILFunctionConventions conv(origTy, IGF.getSILModule());
    SILType directResultType = conv.getSILResultType(expansionContext);
    auto &directResultTL = IGF.IGM.getTypeInfo(directResultType);
    auto &schema = directResultTL.nativeReturnValueSchema(IGF.IGM);
    if (schema.requiresIndirect()) {
      indirectReturnSlot = original.claimNext();
    }

    original.transferInto(params, conv.getNumIndirectSILResults());

    // Prepare each parameter.
    for (auto param : origTy->getParameters().drop_back()) {
      auto paramType = conv.getSILType(param, expansionContext);

      // If the SIL parameter isn't passed indirectly, we need to map it
      // to an explosion.
      if (paramType.isObject()) {
        auto &paramTI = IGF.getTypeInfo(paramType);
        auto &loadableParamTI = cast<LoadableTypeInfo>(paramTI);
        auto &nativeSchema = loadableParamTI.nativeParameterValueSchema(IGF.IGM);
        unsigned size = nativeSchema.size();

        Explosion nativeParam;
        if (nativeSchema.requiresIndirect()) {
          // If the explosion must be passed indirectly, load the value from the
          // indirect address.
          Address paramAddr =
            loadableParamTI.getAddressForPointer(original.claimNext());
          loadableParamTI.loadAsTake(IGF, paramAddr, nativeParam);
        } else {
          if (!nativeSchema.empty()) {
            // Otherwise, we map from the native convention to the type's explosion
            // schema.
            Explosion paramExplosion;
            original.transferInto(paramExplosion, size);
            nativeParam = nativeSchema.mapFromNative(IGF.IGM, IGF, paramExplosion,
                                                     paramType);
          }
        }

        nativeParam.transferInto(params, nativeParam.size());
      } else {
        params.add(original.claimNext());
      }
    }

    // Anything else, just pass along.  This will include things like
    // generic arguments.
    params.add(original.claimAll());
  }
}

Callee IRGenThunk::lookupMethod() {
  CalleeInfo info(origTy, substTy, subMap);

  // Protocol case.
  if (isWitnessMethod) {
    // Find the witness we're interested in.
    auto *wtable = witnessMetadata.SelfWitnessTable;
    auto witness = emitWitnessMethodValue(IGF, wtable, declRef);

    return Callee(std::move(info), witness, selfValue);
  }

  // Class case.

  // Load the metadata, or use the 'self' value if we have a static method.
  auto selfTy = origTy->getSelfParameter().getSILStorageType(
      IGF.IGM.getSILModule(), origTy, expansionContext);

  // If 'self' is an instance, load the class metadata.
  llvm::Value *metadata;
  if (selfTy.is<MetatypeType>()) {
    metadata = selfValue;
  } else {
    metadata = emitHeapMetadataRefForHeapObject(IGF, selfValue, selfTy,
                                                /*suppress cast*/ true);
  }

  // Find the method we're interested in.
  auto method = emitVirtualMethodValue(IGF, metadata, declRef, origTy);

  return Callee(std::move(info), method, selfValue);
}

void IRGenThunk::emit() {
  GenericContextScope scope(IGF.IGM, origTy->getInvocationGenericSignature());

  if (isAsync) {
    IGF.setupAsync();

    auto entity = LinkEntity::forDispatchThunk(declRef);
    emitAsyncFunctionEntry(IGF, *asyncLayout, entity);
    emitAsyncFunctionPointer(IGF.IGM, IGF.CurFn, entity,
                             asyncLayout->getSize());
  }

  prepareArguments();

  auto callee = lookupMethod();

  std::unique_ptr<CallEmission> emission =
      getCallEmission(IGF, callee.getSwiftContext(), std::move(callee));

  emission->begin();

  emission->setArgs(params, /*isOutlined=*/false, &witnessMetadata);

  if (isCoroutine) {
    assert(!isAsync);

    auto *result = emission->emitCoroutineAsOrdinaryFunction();
    emission->end();

    IGF.Builder.CreateRet(result);
    return;
  }

  Explosion result;

  // Determine if the result is returned indirectly.
  SILFunctionConventions conv(origTy, IGF.getSILModule());
  SILType directResultType = conv.getSILResultType(expansionContext);
  auto &directResultTL = IGF.IGM.getTypeInfo(directResultType);
  auto &schema = directResultTL.nativeReturnValueSchema(IGF.IGM);
  if (schema.requiresIndirect()) {
    Address indirectReturnAddr(indirectReturnSlot,
                               directResultTL.getBestKnownAlignment());
    emission->emitToMemory(indirectReturnAddr,
                           cast<LoadableTypeInfo>(directResultTL), false);
  } else {
    emission->emitToExplosion(result, /*isOutlined=*/false);
  }

  emission->end();

  if (isAsync) {
    emitAsyncReturn(IGF, *asyncLayout, origTy);
    IGF.emitCoroutineOrAsyncExit();
    return;
  }

  // Return the result.
  if (result.empty()) {
    IGF.Builder.CreateRetVoid();
    return;
  }

  auto resultTy = conv.getSILResultType(expansionContext);
  resultTy = resultTy.subst(IGF.getSILModule(), subMap);

  IGF.emitScalarReturn(resultTy, resultTy, result,
                       /*swiftCCReturn=*/false,
                       /*isOutlined=*/false);
}

void IRGenModule::emitDispatchThunk(SILDeclRef declRef) {
  auto *f = getAddrOfDispatchThunk(declRef, ForDefinition);
  if (!f->isDeclaration()) {
    return;
  }

  IRGenFunction IGF(*this, f);
  IRGenThunk(IGF, declRef).emit();
}

llvm::Constant *
IRGenModule::getAddrOfAsyncFunctionPointer(LinkEntity entity) {
  return getAddrOfLLVMVariable(
    LinkEntity::forAsyncFunctionPointer(entity),
    NotForDefinition, DebugTypeInfo());
}

llvm::Constant *
IRGenModule::getAddrOfAsyncFunctionPointer(SILFunction *function) {
  (void)getAddrOfSILFunction(function, NotForDefinition);
  return getAddrOfAsyncFunctionPointer(
      LinkEntity::forSILFunction(function));
}

llvm::Constant *IRGenModule::defineAsyncFunctionPointer(LinkEntity entity,
                                                        ConstantInit init) {
  auto asyncEntity = LinkEntity::forAsyncFunctionPointer(entity);
  auto *var = cast<llvm::GlobalVariable>(
      getAddrOfLLVMVariable(asyncEntity, init, DebugTypeInfo()));
  setTrueConstGlobal(var);
  return var;
}

SILFunction *
IRGenModule::getSILFunctionForAsyncFunctionPointer(llvm::Constant *afp) {
  for (auto &entry : GlobalVars) {
    if (entry.getSecond() == afp) {
      auto entity = entry.getFirst();
      return entity.getSILFunction();
    }
  }
  return nullptr;
}

llvm::GlobalValue *IRGenModule::defineMethodDescriptor(SILDeclRef declRef,
                                                       NominalTypeDecl *nominalDecl,
                                                       llvm::Constant *definition) {
  auto entity = LinkEntity::forMethodDescriptor(declRef);
  return defineAlias(entity, definition);
}

/// Get or create a method descriptor variable.
llvm::Constant *
IRGenModule::getAddrOfMethodDescriptor(SILDeclRef declRef,
                                       ForDefinition_t forDefinition) {
  assert(forDefinition == NotForDefinition);
  assert(declRef.getOverriddenWitnessTableEntry() == declRef &&
         "Overriding protocol requirements do not have method descriptors");
  LinkEntity entity = LinkEntity::forMethodDescriptor(declRef);
  return getAddrOfLLVMVariable(entity, forDefinition, DebugTypeInfo());
}

/// Fetch the method lookup function for a resilient class.
llvm::Function *
IRGenModule::getAddrOfMethodLookupFunction(ClassDecl *classDecl,
                                           ForDefinition_t forDefinition) {
  IRGen.noteUseOfTypeMetadata(classDecl);

  LinkEntity entity = LinkEntity::forMethodLookupFunction(classDecl);
  llvm::Function *&entry = GlobalFuncs[entity];
  if (entry) {
    if (forDefinition) updateLinkageForDefinition(*this, entry, entity);
    return entry;
  }

  llvm::Type *params[] = {
    TypeMetadataPtrTy,
    MethodDescriptorStructTy->getPointerTo()
  };
  auto fnType = llvm::FunctionType::get(Int8PtrTy, params, false);
  Signature signature(fnType, llvm::AttributeList(), SwiftCC);
  LinkInfo link = LinkInfo::get(*this, entity, forDefinition);
  entry = createFunction(*this, link, signature);
  return entry;
}

void IRGenModule::emitMethodLookupFunction(ClassDecl *classDecl) {
  auto *f = getAddrOfMethodLookupFunction(classDecl, ForDefinition);
  if (!f->isDeclaration()) {
    assert(IRGen.isLazilyReemittingNominalTypeDescriptor(classDecl));
    return;
  }

  IRGenFunction IGF(*this, f);

  auto params = IGF.collectParameters();
  auto *metadata = params.claimNext();
  auto *method = params.claimNext();

  auto *description = getAddrOfTypeContextDescriptor(classDecl,
                                                     RequireMetadata);

  // Check for lookups of nonoverridden methods first.
  class LookUpNonoverriddenMethods
    : public ClassMetadataScanner<LookUpNonoverriddenMethods> {
  
    IRGenFunction &IGF;
    llvm::Value *methodArg;
      
  public:
    LookUpNonoverriddenMethods(IRGenFunction &IGF,
                               ClassDecl *classDecl,
                               llvm::Value *methodArg)
      : ClassMetadataScanner(IGF.IGM, classDecl), IGF(IGF),
        methodArg(methodArg) {}
      
    void noteNonoverriddenMethod(SILDeclRef method) {
      // The method lookup function would be used only for `super.` calls
      // from other modules, so we only need to look at public-visibility
      // methods here.
      if (!hasPublicVisibility(method.getLinkage(NotForDefinition))) {
        return;
      }
      
      auto methodDesc = IGM.getAddrOfMethodDescriptor(method, NotForDefinition);
      
      auto isMethod = IGF.Builder.CreateICmpEQ(methodArg, methodDesc);
      
      auto falseBB = IGF.createBasicBlock("");
      auto trueBB = IGF.createBasicBlock("");

      IGF.Builder.CreateCondBr(isMethod, trueBB, falseBB);
      
      IGF.Builder.emitBlock(trueBB);
      // Since this method is nonoverridden, we can produce a static result.
      auto entry = VTable->getEntry(IGM.getSILModule(), method);
      llvm::Value *impl = IGM.getAddrOfSILFunction(entry->getImplementation(),
                                                   NotForDefinition);
      // Sign using the discriminator we would include in the method
      // descriptor.
      if (auto &schema = IGM.getOptions().PointerAuth.SwiftClassMethods) {
        auto discriminator =
          PointerAuthInfo::getOtherDiscriminator(IGM, schema, method);
        
        impl = emitPointerAuthSign(IGF, impl,
                            PointerAuthInfo(schema.getKey(), discriminator));
      }
      impl = IGF.Builder.CreateBitCast(impl, IGM.Int8PtrTy);
      IGF.Builder.CreateRet(impl);
      
      IGF.Builder.emitBlock(falseBB);
      // Continue emission on the false branch.
    }
      
    void noteResilientSuperclass() {}
    void noteStartOfImmediateMembers(ClassDecl *clas) {}
  };
  
  LookUpNonoverriddenMethods(IGF, classDecl, method).layout();
  
  // Use the runtime to look up vtable entries.
  auto *result = IGF.Builder.CreateCall(getLookUpClassMethodFn(),
                                        {metadata, method, description});
  IGF.Builder.CreateRet(result);
}
