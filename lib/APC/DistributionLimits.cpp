//=== DistributionLimits.cpp - Limitation of Distribution Checker *- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements checkers to determine whether the distribution of arrays
// is possible.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include <apc/Distribution/Array.h>
#include <apc/GraphCall/graph_calls.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-distribution-limits"


using namespace llvm;
using namespace tsar;

namespace {
class APCDistrLimitsChecker : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  APCDistrLimitsChecker() : FunctionPass(ID) {
    initializeAPCDistrLimitsCheckerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

class APCDistrLimitsIPOChecker : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  APCDistrLimitsIPOChecker() : ModulePass(ID) {
    initializeAPCDistrLimitsIPOCheckerPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}

char APCDistrLimitsChecker::ID = 0;
INITIALIZE_PASS_BEGIN(APCDistrLimitsChecker, "apc-distribution-limits",
                      "Distribution Limitation Checker (APC)", true, true)
INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(APCDistrLimitsChecker, "apc-distribution-limits",
                      "Distribution Limitation Checker (APC)", true, true)

ModulePass *llvm::createAPCDistrLimitsIPOChecker() {
  return new APCDistrLimitsIPOChecker;
}

char APCDistrLimitsIPOChecker::ID = 0;
INITIALIZE_PASS_BEGIN(APCDistrLimitsIPOChecker,
                      "apc-ipo-distribution-limits",
                      "IPO Distribution Limitation Checker (APC)", true, true)
INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
INITIALIZE_PASS_END(APCDistrLimitsIPOChecker, "apc-ipo-distribution-limits",
                    "IPO Distribution Limitation Global Checker (APC)", true,
                    true)

FunctionPass *llvm::createAPCDistrLimitsChecker() {
  return new APCDistrLimitsChecker;
}

void APCDistrLimitsChecker::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

bool APCDistrLimitsChecker::runOnFunction(Function& F) {
  // We use Distribution::SPF_PRIV and Distribution::IO_PRIV flags only
  // because only these flags are propagated through actual-to-formal parameters
  // relation in both directions (up and down).
  auto &APCCtx{*getAnalysis<APCContextWrapper>()};
  if (hasFnAttr(F, AttrKind::IndirectCall))
    if (auto *APCFunc{APCCtx.findFunction(F)})
      for (unsigned I = 0, EI = APCFunc->funcParams.countOfPars; I < EI; ++I) {
        if (APCFunc->funcParams.parametersT[I] == ARRAY_T) {
          assert(APCFunc->funcParams.parameters[I] &&
                 "Array must not be null!");
          auto *A{static_cast<apc::Array *>(APCFunc->funcParams.parameters[I])};
          LLVM_DEBUG(dbgs()
                     << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                     << A->GetName()
                     << " (parent function may be called indirectly)\n");
          A->SetDistributeFlag(Distribution::SPF_PRIV);
        }
      }
  auto &DL{F.getParent()->getDataLayout()};
  auto &DT{getAnalysis<DominatorTreeWrapperPass>().getDomTree()};
  auto &AT{getAnalysis<EstimateMemoryPass>().getAliasTree()};
  auto &TLI{getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F)};
  for (auto &I : instructions(F)) {
    if (isa<LoadInst>(I))
      continue;
    if (auto *SI{dyn_cast<StoreInst>(&I)}) {
      // Check whether we remember pointer to an array element for further use.
      if (auto *Op{SI->getValueOperand()}; Op->getType()->isPointerTy()) {
        auto *EM{AT.find(MemoryLocation{Op, LocationSize::precise(1)})};
        assert(EM && "Estimate memory must be "
                     "presented in alias tree!");
        auto *TopEM{EM->getTopLevelParent()};
        auto *RawDIM{getRawDIMemoryIfExists(*TopEM, I.getContext(), DL, DT)};
        if (!RawDIM)
          continue;
        auto APCArray{APCCtx.findArray(RawDIM)};
        if (!APCArray || APCArray->IsNotDistribute())
          continue;
        LLVM_DEBUG(
            dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                   << APCArray->GetName() << " (store an address to memory) ";
            I.print(dbgs()); dbgs() << "\n");
        APCArray->SetDistributeFlag(Distribution::SPF_PRIV);
      }
      continue;
    }
    for_each_memory(
        I, TLI,
        [&APCCtx, &AT, &DL, &DT](Instruction &I, MemoryLocation &&Loc,
                                 unsigned OpIdx, AccessInfo IsRead,
                                 AccessInfo IsWrite) {
          auto *EM{AT.find(Loc)};
          assert(EM && "Estimate memory must be presented in alias tree!");
          auto *TopEM{EM->getTopLevelParent()};
          auto *RawDIM{getRawDIMemoryIfExists(*TopEM, I.getContext(), DL, DT)};
          if (!RawDIM)
            return;
          auto APCArray{APCCtx.findArray(RawDIM)};
          if (!APCArray || APCArray->IsNotDistribute())
            return;
          if (auto *II{dyn_cast<IntrinsicInst>(&I)}) {
            if (isMemoryMarkerIntrinsic(II->getIntrinsicID()))
              return;
            LLVM_DEBUG(
                dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                       << APCArray->GetName() << " (intrinsic) ";
                I.print(dbgs()); dbgs() << "\n");
            APCArray->SetDistributeFlag(Distribution::SPF_PRIV);
            return;
          }
          if (!isa<CallBase>(I) || EM != TopEM) {
            LLVM_DEBUG(
                dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                       << APCArray->GetName()
                       << " (unsupported memory access) ";
                I.print(dbgs()); dbgs() << "\n");
            APCArray->SetDistributeFlag(Distribution::SPF_PRIV);
            return;
          }
          auto *CB{cast<CallBase>(&I)};
          auto *Callee{
              dyn_cast<Function>(CB->getCalledOperand()->stripPointerCasts())};
          if (!Callee || Callee->isDeclaration() ||
              hasFnAttr(*Callee, AttrKind::LibFunc)) {
            LLVM_DEBUG(
                dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                       << APCArray->GetName() << " (unknown function) ";
                I.print(dbgs()); dbgs() << "\n");
            APCArray->SetDistributeFlag(Distribution::IO_PRIV);
            return;
          }
          if (auto *APCCallee{APCCtx.findFunction(*Callee)};
              !APCCallee || APCCallee->funcParams.countOfPars <= OpIdx ||
              APCCallee->funcParams.parametersT[OpIdx] != ARRAY_T) {
            LLVM_DEBUG(
                dbgs() << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                       << APCArray->GetName()
                       << " (function prototype mismatch) ";
                I.print(dbgs()); dbgs() << "\n");
            APCArray->SetDistributeFlag(Distribution::SPF_PRIV);
            return;
          }
        },
        [](Instruction &I, AccessInfo IsRead, AccessInfo IsWrite) {});
  }
  return false;
}

void APCDistrLimitsIPOChecker::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.setPreservesAll();
}

bool APCDistrLimitsIPOChecker::runOnModule(Module& M) {
  auto &APCCtx{getAnalysis<APCContextWrapper>().get()};
  for (auto &F : M) {
    auto *Func{APCCtx.findFunction(F)};
    if (!Func)
      continue;
    for (unsigned CallFromIdx = 0, CallFromIdxE = Func->actualParams.size();
         CallFromIdx < CallFromIdxE; ++CallFromIdx) {
      auto &Actuals{Func->actualParams[CallFromIdx]};
      assert(Func->parentForPointer[CallFromIdx] &&
             "Call statement must not be null!");
      auto *CB{cast<CallBase>(
          static_cast<Instruction *>(Func->parentForPointer[CallFromIdx]))};
      auto Callee{cast<Function>(CB->getCalledOperand()->stripPointerCasts())};
      auto *APCCallee{APCCtx.findFunction(*Callee)};
      assert(APCCallee && "Function must be registered!");
      for (unsigned I = 0, EI = APCCallee->funcParams.countOfPars; I < EI; ++I)
        if (APCCallee->funcParams.parametersT[I] == ARRAY_T &&
            (Actuals.countOfPars <= I || Actuals.parametersT[I] != ARRAY_T)) {
          auto *A{
              static_cast<apc::Array *>(APCCallee->funcParams.parameters[I])};
          A->SetDistributeFlag(Distribution::SPF_PRIV);
          LLVM_DEBUG(dbgs()
                     << "[APC DISTRIBUTION LIMITS]: disable distribution of "
                     << A->GetName()
                     << " (unable to establish correspondence with actual "
                        "parameter of an array type)\n");
        }
    }
  }
  return false;
}
