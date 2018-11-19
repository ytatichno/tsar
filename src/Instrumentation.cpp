//===- Istrumentation.cpp -- LLVM IR Instrumentation Engine -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methods to perform IR-level instrumentation.
//
//===----------------------------------------------------------------------===//

#include "Instrumentation.h"
#include "DIEstimateMemory.h"
#include "SourceUnparserUtils.h"
#include "DFRegionInfo.h"
#include "CanonicalLoop.h"
#include "Intrinsics.h"
#include "tsar_memory_matcher.h"
#include "MetadataUtils.h"
#include "tsar_pass_provider.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpander.h>
#include "llvm/IR/DiagnosticInfo.h"
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "instr-llvm"

using InstrumentationPassProvider = FunctionPassProvider<
  TransformationEnginePass,
  DFRegionInfoPass,
  LoopInfoWrapperPass,
  CanonicalLoopPass,
  MemoryMatcherImmutableWrapper,
  ScalarEvolutionWrapperPass,
  DominatorTreeWrapperPass>;

STATISTIC(NumFunction, "Number of functions");
STATISTIC(NumFunctionVisited, "Number of processed functions");
STATISTIC(NumLoop, "Number of processed loops");
STATISTIC(NumType, "Number of registered types");
STATISTIC(NumVariable, "Number of registered variables");
STATISTIC(NumScalar, "Number of registered scalar variables");
STATISTIC(NumArray, "Number of registered arrays");
STATISTIC(NumCall, "Number of registered calls");
STATISTIC(NumMemoryAccesses, "Number of registered memory accesses");
STATISTIC(NumLoad, "Number of registered loads from the memory");
STATISTIC(NumLoadScalar, "Number of registered loads from scalars");
STATISTIC(NumLoadArray, "Number of registered loads from arrays");
STATISTIC(NumStore, "Number of registered stores to the memory");
STATISTIC(NumStoreScalar, "Number of registered stores to scalars");
STATISTIC(NumStoreArray, "Number of registered stores to arrays");

INITIALIZE_PROVIDER_BEGIN(InstrumentationPassProvider, "instr-llvm-provider",
  "Instrumentation Provider")
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PROVIDER_END(InstrumentationPassProvider, "instr-llvm-provider",
  "Instrumentation Provider")

char InstrumentationPass::ID = 0;
INITIALIZE_PASS_BEGIN(InstrumentationPass, "instr-llvm",
  "LLVM IR Instrumentation", false, false)
INITIALIZE_PASS_DEPENDENCY(InstrumentationPassProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_END(InstrumentationPass, "instr-llvm",
  "LLVM IR Instrumentation", false, false)

bool InstrumentationPass::runOnModule(Module &M) {
  releaseMemory();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  InstrumentationPassProvider::initialize<TransformationEnginePass>(
    [&M, &TfmCtx](TransformationEnginePass &TEP) {
      TEP.setContext(M, TfmCtx);
  });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  InstrumentationPassProvider::initialize<MemoryMatcherImmutableWrapper>(
    [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
      Wrapper.set(*MMWrapper);
  });
  Instrumentation::visit(M, *this);
  if (auto EntryPoint = M.getFunction("main"))
    visitEntryPoint(*EntryPoint, { &M });
  else
    M.getContext().diagnose(DiagnosticInfoInlineAsm("entry point is not found"));
  return true;
}

void InstrumentationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<InstrumentationPassProvider>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
}

ModulePass * llvm::createInstrumentationPass() {
  return new InstrumentationPass();
}

Function * tsar::createEmptyInitDI(Module &M, Type &IdTy) {
  auto &Ctx = M.getContext();
  auto FuncType = FunctionType::get(Type::getVoidTy(Ctx), { &IdTy }, false);
  auto Func = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.init.di", &M);
  addNameDAMetadata(*Func, "sapfor.da", "sapfor.init.di");
  Func->arg_begin()->setName("startid");
  auto EntryBB = BasicBlock::Create(Ctx, "entry", Func);
  ReturnInst::Create(Ctx, EntryBB);
  return Func;
}

GlobalVariable * tsar::getOrCreateDIPool(Module &M) {
  auto *DIPoolTy = PointerType::getUnqual(Type::getInt8PtrTy(M.getContext()));
  if (auto *DIPool = M.getNamedValue("sapfor.di.pool")) {
    if (isa<GlobalVariable>(DIPool) && DIPool->getValueType() == DIPoolTy &&
        cast<GlobalVariable>(DIPool)->getMetadata("sapfor.da"))
      return cast<GlobalVariable>(DIPool);
    return nullptr;
  }
  auto DIPool = new GlobalVariable(M, DIPoolTy, false,
    GlobalValue::LinkageTypes::ExternalLinkage,
    ConstantPointerNull::get(DIPoolTy), "sapfor.di.pool", nullptr);
  assert(DIPool->getName() == "sapfor.di.pool" &&
    "Unable to crate a metadata pool!");
  DIPool->setAlignment(4);
  DIPool->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  return DIPool;
}

Type * tsar::getInstrIdType(LLVMContext &Ctx) {
  auto InitDIFuncTy = getType(Ctx, IntrinsicId::init_di);
  assert(InitDIFuncTy->getNumParams() > 2 &&
    "Intrinsic 'init_di' must has at least 3 arguments!");
  return InitDIFuncTy->getParamType(2);
}

void Instrumentation::visitModule(Module &M, InstrumentationPass &IP) {
  mInstrPass = &IP;
  mDIStrings.clear(DIStringRegister::numberOfItemTypes());
  mTypes.clear();
  auto &Ctx = M.getContext();
  mDIPool = getOrCreateDIPool(M);
  auto IdTy = getInstrIdType(Ctx);
  assert(IdTy && "Offset type must not be null!");
  mInitDIAll = createEmptyInitDI(M, *IdTy);
  reserveIncompleteDIStrings(M);
  regFunctions(M);
  regGlobals(M);
  visit(M.begin(), M.end());
  regTypes(M);
  auto Int64Ty = Type::getInt64Ty(M.getContext());
  auto PoolSize = ConstantInt::get(IdTy,
    APInt(Int64Ty->getBitWidth(), mDIStrings.numberOfIDs()));
  addNameDAMetadata(*mDIPool, "sapfor.da", "sapfor.di.pool",
    { ConstantAsMetadata::get(PoolSize) });
  NumVariable += NumScalar + NumArray;
  NumLoad += NumLoadScalar + NumLoadArray;
  NumStore += NumStore + NumStoreArray;
  NumMemoryAccesses += NumLoad + NumStore;
}

void Instrumentation::reserveIncompleteDIStrings(llvm::Module &M) {
  auto DbgLocIdx = DIStringRegister::indexOfItemType<DILocation *>();
  createInitDICall(
    Twine("type=") + "file_name" + "*" +
    "file=" + M.getSourceFileName() + "*" + "*", DbgLocIdx);
}

void Instrumentation::visitAllocaInst(llvm::AllocaInst &I) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  auto MD = findMetadata(&I);
  auto Idx = mDIStrings.regItem(&I);
  BasicBlock::iterator InsertBefore(I);
  ++InsertBefore;
  Optional<DIMemoryLocation> DIM;
  if (MD)
    DIM = DIMemoryLocation(MD, DIExpression::get(I.getContext(), {}));
  regValue(&I, I.getAllocatedType(), DIM ? &*DIM : nullptr,
    Idx, *InsertBefore, *I.getModule());
}

void Instrumentation::visitReturnInst(llvm::ReturnInst &I) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  auto Fun = getDeclaration(I.getModule(), IntrinsicId::func_end);
  unsigned Idx = mDIStrings[I.getFunction()];
  auto DIFunc = createPointerToDI(Idx, I);
  auto Call = CallInst::Create(Fun, {DIFunc}, "", &I);
  Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
}

std::tuple<Value *, Value *, Value *, bool>
Instrumentation::computeLoopBounds(Loop &L, IntegerType &IntTy,
    ScalarEvolution &SE, DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS) {
  auto *Region = RI.getRegionFor(&L);
  assert(Region && "Region must not be null!");
  auto CanonItr = CS.find_as(Region);
  if (CanonItr == CS.end())
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  auto *Header = L.getHeader();
  auto *End = (*CanonItr)->getEnd();
  if (!End)
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  auto *EndTy = End->getType();
  if (!EndTy || !EndTy->isIntegerTy() ||
      EndTy->getIntegerBitWidth() > IntTy.getBitWidth())
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  bool Signed = false;
  bool Unsigned = false;
  for (auto *U : End->users())
    if (auto *Cmp = dyn_cast<CmpInst>(U)) {
      Signed |= Cmp->isSigned();
      Unsigned |= Cmp->isUnsigned();
    }
  // Is sign known?
  if (Signed == Unsigned)
    return std::make_tuple(nullptr, nullptr, nullptr, false);
  auto InstrMD = MDNode::get(Header->getContext(), {});
  assert(L.getLoopPreheader() && "For-loop must have a preheader!");
  auto &InsertBefore = L.getLoopPreheader()->back();
  // Compute start if possible.
  auto *Start = (*CanonItr)->getStart();
  if (Start) {
    auto StartTy = Start->getType();
    if (StartTy && StartTy->isIntegerTy()) {
      if (StartTy->getIntegerBitWidth() > IntTy.getBitWidth()) {
        Start = nullptr;
      } else if (StartTy->getIntegerBitWidth() < IntTy.getBitWidth()) {
        Start = CastInst::Create(Signed ? Instruction::SExt : Instruction::ZExt,
          Start, &IntTy, "loop.start", &InsertBefore);
        cast<Instruction>(Start)->setMetadata("sapfor.da", InstrMD);
      }
    }
  }
  // It is unsafe to compute step and end bound if for-loop is not canonical.
  // In this case step and end bound may depend on the loop iteration.
  if (!(*CanonItr)->isCanonical())
    return std::make_tuple(Start, nullptr, nullptr, Signed);
  // Compute end if possible.
  if (isa<Instruction>(End)) {
    SmallVector<Instruction *, 8> EndClone;
    if (!cloneChain(cast<Instruction>(End), EndClone, &InsertBefore, &DT)) {
      End = nullptr;
    } else {
      for (auto I = EndClone.rbegin(), EI = EndClone.rend(); I != EI; ++I) {
        (*I)->insertBefore(&InsertBefore);
        (*I)->setMetadata("sapfor.da", InstrMD);
      }
      if (!EndClone.empty())
        End = EndClone.front();
    }
  }
  if (End && EndTy->getIntegerBitWidth() < IntTy.getBitWidth()) {
    End = CastInst::Create(Signed ? Instruction::SExt : Instruction::ZExt,
      End, &IntTy, "loop.end", &InsertBefore);
    cast<Instruction>(End)->setMetadata("sapfor.da", InstrMD);
  }
  auto Step = computeSCEV(
    (*CanonItr)->getStep(), IntTy, Signed, SE, DT, InsertBefore);
  return std::make_tuple(Start, End, Step, Signed);
}

Value * Instrumentation::computeSCEV(const SCEV *ExprSCEV,
    IntegerType &IntTy, bool Signed, ScalarEvolution &SE, DominatorTree &DT,
    Instruction &InsertBefore) {
  if (!ExprSCEV)
    return nullptr;
  auto *ExprTy = ExprSCEV->getType();
  if (!ExprTy || !ExprTy->isIntegerTy() ||
      ExprTy->getIntegerBitWidth() > IntTy.getBitWidth())
    return nullptr;
  auto InstrMD = MDNode::get(InsertBefore.getContext(), {});
  if (ExprTy->getIntegerBitWidth() < IntTy.getBitWidth())
    ExprSCEV = Signed ? SE.getSignExtendExpr(ExprSCEV, &IntTy) :
      SE.getZeroExtendExpr(ExprSCEV, &IntTy);
  SCEVExpander Exp(SE, InsertBefore.getModule()->getDataLayout(), "");
  auto Expr = Exp.expandCodeFor(ExprSCEV, &IntTy, &InsertBefore);
  SmallVector<Use *, 4> ExprNotDom;
  if (auto ExprInst = dyn_cast<Instruction>(Expr)) {
    if (findNotDom(ExprInst, &InsertBefore, &DT, ExprNotDom)) {
      SmallVector<Instruction *, 8> ExprClone;
      if (!cloneChain(ExprInst, ExprClone, &InsertBefore, &DT))
        return nullptr;
      for (auto I = ExprClone.rbegin(), EI = ExprClone.rend(); I != EI; ++I) {
        (*I)->insertBefore(cast<Instruction>(&InsertBefore));
        (*I)->setMetadata("sapfor.da", InstrMD);
      }
      if (!ExprClone.empty())
        Expr = ExprClone.front();
    } else {
      setMDForDeadInstructions(ExprInst);
      for (auto *Op : ExprNotDom) {
        SmallVector<Instruction *, 8> ExprClone;
        if (!cloneChain(cast<Instruction>(Op), ExprClone, &InsertBefore, &DT)) {
          deleteDeadInstructions(ExprInst);
          return nullptr;
        }
        for (auto I = ExprClone.rbegin(), EI = ExprClone.rend(); I != EI; ++I) {
          (*I)->insertBefore(cast<Instruction>(Op->getUser()));
          (*I)->setMetadata("sapfor.da", InstrMD);
        }
        Op->getUser()->setOperand(Op->getOperandNo(), ExprClone.front());
      }
    }
  }
  return Expr;
}

void Instrumentation::deleteDeadInstructions(Instruction *From) {
  if (!From->use_empty())
    return;
  for (unsigned OpIdx = 0, OpIdxE = From->getNumOperands();
       OpIdx != OpIdxE; ++OpIdx) {
    Value *OpV = From->getOperand(OpIdx);
    From->setOperand(OpIdx, nullptr);
    if (auto *I = dyn_cast<Instruction>(OpV))
      deleteDeadInstructions(I);
  }
  From->eraseFromParent();
}

void Instrumentation::setMDForDeadInstructions(llvm::Instruction *From) {
  if (!From->use_empty())
    return;
  From->setMetadata("sapfor.da", MDNode::get(From->getContext(), {}));
  for (auto &Op : From->operands())
    if (auto I = dyn_cast<Instruction>(&Op))
      setMDForSingleUseInstructions(I);
}

void Instrumentation::setMDForSingleUseInstructions(Instruction *From) {
  if (From->getNumUses() != 1)
    return;
  From->setMetadata("sapfor.da", MDNode::get(From->getContext(), {}));
  for (auto &Op : From->operands())
    if (auto I = dyn_cast<Instruction>(&Op))
      setMDForSingleUseInstructions(I);
}

void Instrumentation::loopBeginInstr(Loop *L, DIStringRegister::IdTy DILoopIdx,
    ScalarEvolution &SE, DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS) {
  auto *Header = L->getHeader();
  auto InstrMD = MDNode::get(Header->getContext(), {});
  Instruction *InsertBefore = nullptr;
  Value *Start = nullptr, *End = nullptr, *Step = nullptr;
  bool Signed = false;
  auto SLBeginFunc = getDeclaration(Header->getModule(), IntrinsicId::sl_begin);
  auto SLBeginFuncTy = SLBeginFunc->getFunctionType();
  assert(SLBeginFuncTy->getNumParams() > 3 && "Too few arguments!");
  auto *SizeTy = dyn_cast<IntegerType>(SLBeginFuncTy->getParamType(1));
  assert(SizeTy && "Bound expression must has an integer type!");
  assert(SLBeginFuncTy->getParamType(2) == SizeTy &&
    "Loop bound expressions have different types!");
  assert(SLBeginFuncTy->getParamType(3) == SizeTy &&
    "Loop bound expressions have different types!");
  if (auto Preheader = L->getLoopPreheader()) {
    InsertBefore = Preheader->getTerminator();
    std::tie(Start, End, Step, Signed) =
      computeLoopBounds(*L, *SizeTy, SE, DT, RI, CS);
  } else {
    auto *NewBB = BasicBlock::Create(Header->getContext(),
      "preheader", Header->getParent(), Header);
    InsertBefore = BranchInst::Create(Header, NewBB);
    InsertBefore->setMetadata("sapfor.da", InstrMD);
    for (auto *PredBB : predecessors(Header)) {
      if (L->contains(PredBB))
        continue;
      auto *PredBranch = PredBB->getTerminator();
      for (unsigned SuccIdx = 0, SuccIdxE = PredBranch->getNumSuccessors();
           SuccIdx < SuccIdxE; ++SuccIdx)
        if (PredBranch->getSuccessor(SuccIdx) ==  Header)
          PredBranch->setSuccessor(SuccIdx, NewBB);
    }
  }
  auto DbgLoc = L->getLocRange();
  std::string StartLoc = DbgLoc.getStart() ?
    ("line1=" + Twine(DbgLoc.getStart().getLine()) + "*" +
      "col1=" + Twine(DbgLoc.getStart().getCol()) + "*").str() :
    std::string("");
  std::string EndLoc = DbgLoc.getEnd() ?
    ("line1=" + Twine(DbgLoc.getEnd().getLine()) + "*" +
      "col1=" + Twine(DbgLoc.getEnd().getCol()) + "*").str() :
    std::string("");
  LoopBoundKind BoundFlag = LoopBoundIsUnknown;
  BoundFlag |= Start ? LoopStartIsKnown : LoopBoundIsUnknown;
  BoundFlag |= End ? LoopEndIsKnown : LoopBoundIsUnknown;
  BoundFlag |= Step ? LoopStepIsKnown : LoopBoundIsUnknown;
  BoundFlag |= !Signed ? LoopBoundUnsigned : LoopBoundIsUnknown;
  auto MDFunc = Header->getParent()->getSubprogram();
  auto Filename = MDFunc ? MDFunc->getFilename() :
    StringRef(Header->getModule()->getSourceFileName());
  createInitDICall(
    Twine("type=") + "seqloop" + "*" +
    "file=" + Filename + "*" +
    "bounds=" + Twine(BoundFlag) + "*" +
    StartLoc + EndLoc + "*", DILoopIdx);
  auto *DILoop = createPointerToDI(DILoopIdx, *InsertBefore);
  Start = Start ? Start : ConstantInt::get(SizeTy, 0);
  End = End ? End : ConstantInt::get(SizeTy, 0);
  Step = Step ? Step : ConstantInt::get(SizeTy, 0);
  auto Call = CallInst::Create(
    SLBeginFunc, {DILoop, Start, End, Step}, "", InsertBefore);
  Call->setMetadata("sapfor.da", InstrMD);
}

void Instrumentation::loopEndInstr(Loop *L, DIStringRegister::IdTy DILoopIdx) {
  assert(L && "Loop must not be null!");
  auto *Header = L->getHeader();
  auto InstrMD = MDNode::get(Header->getContext(), {});
  for (auto *BB : L->blocks())
    for (auto *SuccBB : successors(BB)) {
      if (L->contains(SuccBB))
        continue;
      auto *ExitBB = BasicBlock::Create(Header->getContext(),
        SuccBB->getName(), Header->getParent(), SuccBB);
      auto *InsertBefore = BranchInst::Create(SuccBB, ExitBB);
      InsertBefore->setMetadata("sapfor.da", InstrMD);
      auto *ExitingBranch = BB->getTerminator();
      for (unsigned SuccIdx = 0, SuccIdxE = ExitingBranch->getNumSuccessors();
           SuccIdx < SuccIdxE; ++SuccIdx) {
        if (ExitingBranch->getSuccessor(SuccIdx) == SuccBB)
          ExitingBranch->setSuccessor(SuccIdx, ExitBB);
      }
      auto DILoop = createPointerToDI(DILoopIdx, *InsertBefore);
      auto Fun = getDeclaration(Header->getModule(), IntrinsicId::sl_end);
      auto Call = CallInst::Create(Fun, {DILoop}, "", InsertBefore);
      Call->setMetadata("sapfor.da", InstrMD);
    }
}

void Instrumentation::loopIterInstr(Loop *L, DIStringRegister::IdTy DILoopIdx) {
  assert(L && "Loop must not be null!");
  auto *Header = L->getHeader();
  auto InstrMD = MDNode::get(Header->getContext(), {});
  auto &InsertBefore = *Header->getFirstInsertionPt();
  auto *Int64Ty = Type::getInt64Ty(Header->getContext());
  auto *CountPHI = PHINode::Create(Int64Ty, 0, "loop.count", &Header->front());
  CountPHI->setMetadata("sapfor.da", InstrMD);
  auto *Int1 = ConstantInt::get(Int64Ty, 1);
  assert(L->getLoopPreheader() &&
    "Preheader must be already created if it did not exist!");
  CountPHI->addIncoming(Int1, L->getLoopPreheader());
  auto *Inc = BinaryOperator::CreateNUW(BinaryOperator::Add, CountPHI,
    ConstantInt::get(Int64Ty, 1), "inc", &InsertBefore);
  Inc->setMetadata("sapfor.da", InstrMD);
  SmallVector<BasicBlock *, 4> Latches;
  L->getLoopLatches(Latches);
  for (auto *Latch : Latches)
    CountPHI->addIncoming(Inc, Latch);
  auto *DILoop = createPointerToDI(DILoopIdx, *Inc);
  auto Fun = getDeclaration(Header->getModule(), IntrinsicId::sl_iter);
  auto *Call = CallInst::Create(Fun, {DILoop, CountPHI}, "", Inc);
  Call->setMetadata("sapfor.da", InstrMD);
}

void Instrumentation::regLoops(llvm::Function &F, llvm::LoopInfo &LI,
    llvm::ScalarEvolution &SE, llvm::DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS) {
  for_each_loop(LI, [this, &SE, &DT, &RI, &CS, &F](Loop *L) {
    LLVM_DEBUG(dbgs()<<"[INSTR]: process loop " << L->getHeader()->getName() <<"\n");
    auto Idx = mDIStrings.regItem(LoopUnique(&F, L));
    loopBeginInstr(L, Idx, SE, DT, RI, CS);
    loopEndInstr(L, Idx);
    loopIterInstr(L, Idx);
    ++NumLoop;
  });
}

void Instrumentation::visit(Function &F) {
  // Some functions have not been marked with "sapfor.da" yet. For example,
  // functions which have been created after registration of all functions.
  // So, we set this property here.
  IntrinsicId InstrLibId;
  if (getTsarLibFunc(F.getName(), InstrLibId)) {
    F.setMetadata("sapfor.da", MDNode::get(F.getContext(), {}));
    return;
  }
  if (F.getMetadata("sapfor.da"))
    return;
  ++NumFunction;
  if (F.empty())
    return;
  visitFunction(F);
  visit(F.begin(), F.end());
  mDT = nullptr;
}

void Instrumentation::regFunction(Value &F, Type *ReturnTy, unsigned Rank,
  DISubprogram *MD, DIStringRegister::IdTy Idx, Module &M) {
  LLVM_DEBUG(dbgs() << "[INSTR]: register function ";
    F.printAsOperand(dbgs()); dbgs() << "\n");
  std::string DeclStr = !MD ? F.getName().empty() ? std::string("") :
    (Twine("name1=") + F.getName() + "*").str() :
    ("line1=" + Twine(MD->getLine()) + "*" +
      "name1=" + MD->getName() + "*").str();
  auto Filename = MD ? MD->getFilename() : StringRef(M.getSourceFileName());
  auto ReturnTypeId = mTypes.regItem(ReturnTy);
  createInitDICall(Twine("type=") + "function" + "*" +
    "file=" + Filename + "*" +
    "vtype=" + Twine(ReturnTypeId) + "*" +
    "rank=" + Twine(Rank) + "*" +
    DeclStr + "*", Idx);
}

void Instrumentation::visitFunction(llvm::Function &F) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process function ";
    F.printAsOperand(dbgs()); dbgs() << "\n");
  // Change linkage for inline functions, to avoid merge of a function which
  // should not be instrumented with this function. For example, call of
  // a function which has been instrumented from dynamic analyzer may produce
  // infinite loop. The other example, is call of some system functions before
  // call of main (sprintf... in case of Microsoft implementation of STD). In
  // this case pool of metadata is not allocated yet.
  if(F.getLinkage() == Function::LinkOnceAnyLinkage ||
     F.getLinkage() == Function::LinkOnceODRLinkage)
    F.setLinkage(Function::InternalLinkage);
  auto *M = F.getParent();
  auto *MD = F.getSubprogram();
  auto Idx = mDIStrings[&F];
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto &FirstInst = *inst_begin(F);
  auto DIFunc = createPointerToDI(Idx, FirstInst);
  auto Call = CallInst::Create(Fun, {DIFunc}, "", &FirstInst);
  ++NumFunctionVisited;
  Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
  regArgs(F, DIFunc);
  auto &Provider = mInstrPass->getAnalysis<InstrumentationPassProvider>(F);
  auto &LoopInfo = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  auto &RegionInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
  auto &CanonicalLoop = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto &SE = Provider.get<ScalarEvolutionWrapperPass>().getSE();
  mDT = &Provider.get<DominatorTreeWrapperPass>().getDomTree();
  regLoops(F, LoopInfo, SE, *mDT, RegionInfo, CanonicalLoop);
}

void Instrumentation::regArgs(Function &F, LoadInst *DIFunc) {
  auto InstrMD = MDNode::get(F.getContext(), {});
  auto *BytePtrTy = Type::getInt8PtrTy(F.getContext());
  for (auto &Arg : F.args()) {
    if (Arg.getNumUses() != 1)
      continue;
    auto *U = dyn_cast<StoreInst>(*Arg.user_begin());
    if (!U)
      continue;
    auto *Alloca = dyn_cast<AllocaInst>(U->getPointerOperand());
    if (!Alloca)
      continue;
    auto AllocaMD = findMetadata(Alloca);
    if (!AllocaMD || !AllocaMD->isParameter())
      continue;
    LLVM_DEBUG(dbgs() << "[INSTR]: register "; Alloca->print(dbgs());
      dbgs() << " as argument "; Arg.print(dbgs());
      dbgs() << " with no " << Arg.getArgNo() << "\n");
    auto AllocaAddr =
      new BitCastInst(Alloca, BytePtrTy, Alloca->getName() + ".addr", U);
    AllocaAddr->setMetadata("sapfor.da", InstrMD);
    unsigned Rank;
    uint64_t ArraySize;
    std::tie(Rank, ArraySize) = arraySize(Alloca->getAllocatedType());
    CallInst *Call = nullptr;
    if (Rank != 0) {
      auto Func = getDeclaration(F.getParent(), IntrinsicId::reg_dummy_arr);
      auto FuncTy = Func->getFunctionType();
      assert(FuncTy->getNumParams() > 3 && "Too few arguments!");
      auto Size = ConstantInt::get(FuncTy->getParamType(1), ArraySize);
      auto Pos = ConstantInt::get(FuncTy->getParamType(3), Arg.getArgNo());
      Call = CallInst::Create(Func, { DIFunc, Size, AllocaAddr, Pos }, "");
    } else {
      auto Func = getDeclaration(F.getParent(), IntrinsicId::reg_dummy_var);
      auto FuncTy = Func->getFunctionType();
      assert(FuncTy->getNumParams() > 2 && "Too few arguments!");
      auto Pos = ConstantInt::get(FuncTy->getParamType(2), Arg.getArgNo());
      Call = CallInst::Create(Func, { DIFunc, AllocaAddr, Pos }, "");
    }
    Call->insertBefore(U);
    Call->setMetadata("sapfor.da", InstrMD);
  }
}

void Instrumentation::visitCallSite(llvm::CallSite CS) {
  /// TODO (kaniandr@gmail.com): may be some other intrinsics also should be
  /// ignored, see llvm::AliasSetTracker::addUnknown() for details.
  switch (CS.getIntrinsicID()) {
  case llvm::Intrinsic::dbg_declare: case llvm::Intrinsic::dbg_value:
  case llvm::Intrinsic::assume:
    return;
  }
  DIStringRegister::IdTy FuncIdx = 0;
  if (auto *Callee = llvm::dyn_cast<llvm::Function>(
        CS.getCalledValue()->stripPointerCasts())) {
    IntrinsicId LibId;
    // Do not check for 'sapfor.da' metadata only because it may not be set
    // for some functions of dynamic analyzer yet. However, it is necessary to
    // check for 'sapfor.da' to ignore some internal utility functions which
    // have been created.
    if(Callee->getMetadata("sapfor.da") ||
       getTsarLibFunc(Callee->getName(), LibId))
      return;
    FuncIdx = mDIStrings[Callee];
  } else {
    FuncIdx = mDIStrings.regItem(CS.getCalledValue());
  }
  auto *Inst = CS.getInstruction();
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; Inst->print(dbgs()); dbgs() << "\n");
  auto DbgLocIdx = regDebugLoc(Inst->getDebugLoc());
  auto DILoc = createPointerToDI(DbgLocIdx, *Inst);
  auto DIFunc = createPointerToDI(FuncIdx, *Inst);
  auto *M = Inst->getModule();
  auto Fun = getDeclaration(M, tsar::IntrinsicId::func_call_begin);
  auto CallBegin = llvm::CallInst::Create(Fun, {DILoc, DIFunc}, "", Inst);
  auto InstrMD = MDNode::get(M->getContext(), {});
  CallBegin->setMetadata("sapfor.da", InstrMD);
  Fun = getDeclaration(M, tsar::IntrinsicId::func_call_end);
  auto CallEnd = llvm::CallInst::Create(Fun, {DIFunc}, "");
  CallEnd->insertAfter(Inst);
  CallBegin->setMetadata("sapfor.da", InstrMD);
  ++NumCall;
}

std::tuple<Value *, Value *, Value *, Value *>
Instrumentation::regMemoryAccessArgs(Value *Ptr, const DebugLoc &DbgLoc,
    Instruction &InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto BasePtr = Ptr->stripInBoundsOffsets();
  DIStringRegister::IdTy OpIdx = 0;
  if (auto AI = dyn_cast<AllocaInst>(BasePtr)) {
    OpIdx = mDIStrings[AI];
  } else if (auto GV = dyn_cast<GlobalVariable>(BasePtr)) {
    OpIdx = mDIStrings[GV];
  } else {
    OpIdx = mDIStrings.regItem(BasePtr);
    auto M = InsertBefore.getModule();
    assert(mDT && "Dominator tree must not be null!");
    auto DIM =
      buildDIMemory(MemoryLocation(BasePtr), Ctx, M->getDataLayout(), *mDT);
    regValue(BasePtr, BasePtr->getType(), DIM ? &*DIM : nullptr,
      OpIdx, InsertBefore, *InsertBefore.getModule());
  }
  auto DbgLocIdx = regDebugLoc(DbgLoc);
  auto DILoc = createPointerToDI(DbgLocIdx, InsertBefore);
  auto Addr = new BitCastInst(Ptr,
    Type::getInt8PtrTy(Ctx), "addr", &InsertBefore);
  auto *MD = MDNode::get(Ctx, {});
  Addr->setMetadata("sapfor.da", MD);
  auto DIVar = createPointerToDI(OpIdx, *DILoc);
  auto BasePtrTy = cast_or_null<PointerType>(BasePtr->getType());
  llvm::Instruction *ArrayBase =
    (BasePtrTy && isa<ArrayType>(BasePtrTy->getElementType())) ?
      new BitCastInst(BasePtr, Type::getInt8PtrTy(Ctx),
        BasePtr->getName() + ".arraybase", &InsertBefore) : nullptr;
  if (ArrayBase)
   ArrayBase->setMetadata("sapfor.da", MD);
  return std::make_tuple(DILoc, Addr, DIVar, ArrayBase);
}

void Instrumentation::visitInstruction(Instruction &I) {
  if (I.mayReadOrWriteMemory()) {
    SmallString<64> IStr;
    raw_svector_ostream OS(IStr);
    I.print(OS);
    auto M = I.getModule();
    auto Func = I.getFunction();
    assert(Func && "Function must not be null!");
    auto MD = Func->getSubprogram();
    auto Filename = MD ? MD->getFilename() : StringRef(M->getSourceFileName());
    I.getContext().diagnose(DiagnosticInfoInlineAsm(I,
      Twine("unsupported RW instruction ") + OS.str() + " in " + Filename,
      DS_Warning));
  }
}

void Instrumentation::regReadMemory(Instruction &I, Value &Ptr) {
  if (I.getMetadata("sapfor.da"))
    return;
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  auto *M = I.getModule();
  llvm::Value *DILoc, *Addr, *DIVar, *ArrayBase;
  std::tie(DILoc, Addr, DIVar, ArrayBase) =
    regMemoryAccessArgs(&Ptr, I.getDebugLoc(), I);
  if (ArrayBase) {
    auto *Fun = getDeclaration(M, IntrinsicId::read_arr);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar, ArrayBase}, "", &I);
    Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
    ++NumLoadArray;
  } else {
    auto *Fun = getDeclaration(M, IntrinsicId::read_var);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar}, "", &I);
    Call->setMetadata("sapfor.da", MDNode::get(I.getContext(), {}));
    ++NumLoadScalar;
  }
}

void Instrumentation::regWriteMemory(Instruction &I, Value &Ptr) {
  if (I.getMetadata("sapfor.da"))
    return;
  LLVM_DEBUG(dbgs() << "[INSTR]: process "; I.print(dbgs()); dbgs() << "\n");
  BasicBlock::iterator InsertBefore(I);
  ++InsertBefore;
  auto *M = I.getModule();
  llvm::Value *DILoc, *Addr, *DIVar, *ArrayBase;
  std::tie(DILoc, Addr, DIVar, ArrayBase) =
    regMemoryAccessArgs(&Ptr, I.getDebugLoc(), *InsertBefore);
  if (!Addr)
    return;
  if (ArrayBase) {
    auto *Fun = getDeclaration(M, IntrinsicId::write_arr_end);
    auto Call = CallInst::Create(Fun, { DILoc, Addr, DIVar, ArrayBase }, "");
    Call->insertBefore(&*InsertBefore);
    Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
    ++NumStoreArray;
  } else {
    auto *Fun = getDeclaration(M, IntrinsicId::write_var_end);
    auto Call = CallInst::Create(Fun, {DILoc, Addr, DIVar}, "", &*InsertBefore);
    Call->setMetadata("sapfor.da", MDNode::get(M->getContext(), {}));
    ++NumStoreScalar;
  }
}

void Instrumentation::visitLoadInst(LoadInst &I) {
  regReadMemory(I, *I.getPointerOperand());
}

void Instrumentation::visitStoreInst(StoreInst &I) {
  regWriteMemory(I, *I.getPointerOperand());
}

void Instrumentation::visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) {
  regReadMemory(I, *I.getPointerOperand());
  regWriteMemory(I, *I.getPointerOperand());
}

void Instrumentation::visitAtomicRMWInst(AtomicRMWInst &I) {
  regReadMemory(I, *I.getPointerOperand());
  regWriteMemory(I, *I.getPointerOperand());
}

void Instrumentation::regTypes(Module& M) {
  if (mTypes.numberOfIDs() == 0)
    return;
  auto &Ctx = M.getContext();
  // Get all registered types and fill std::vector<llvm::Constant*>
  // with local indexes and sizes of these types.
  auto &Types = mTypes.getRegister<llvm::Type *>();
  auto *DeclTypeFunc = getDeclaration(&M, IntrinsicId::decl_types);
  auto *SizeTy = DeclTypeFunc->getFunctionType()->getParamType(0);
  auto *Int0 = ConstantInt::get(SizeTy, 0);
  std::vector<Constant* > Ids, Sizes;
  auto &DL = M.getDataLayout();
  for(auto &Pair: Types) {
    auto *TypeId = Constant::getIntegerValue(SizeTy,
      APInt(64, Pair.get<TypeRegister::IdTy>()));
    Ids.push_back(TypeId);
    auto *TypeSize = Pair.get<Type *>()->isSized() ?
      Constant::getIntegerValue(SizeTy,
        APInt(64, DL.getTypeSizeInBits(Pair.get<Type *>()))) : Int0;
    Sizes.push_back(TypeSize);
  }
  // Create global values for IDs and sizes. initialize them with local values.
  auto ArrayTy = ArrayType::get(SizeTy, Types.size());
  auto IdsArray = new GlobalVariable(M, ArrayTy, false,
    GlobalValue::LinkageTypes::InternalLinkage,
    ConstantArray::get(ArrayTy, Ids), "sapfor.type.ids", nullptr);
  IdsArray->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto SizesArray = new GlobalVariable(M, ArrayTy, false,
    GlobalValue::LinkageTypes::InternalLinkage,
    ConstantArray::get(ArrayTy, Sizes), "sapfor.type.sizes", nullptr);
  SizesArray->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  // Create function to update local indexes of types.
  auto FuncType =
    FunctionType::get(Type::getVoidTy(Ctx), { SizeTy }, false);
  auto RegTypeFunc = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.register.type", &M);
  auto *Size = ConstantInt::get(SizeTy, Types.size());
  addNameDAMetadata(*RegTypeFunc, "sapfor.da", "sapfor.register.type",
    {ConstantAsMetadata::get(Size)});
  RegTypeFunc->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto EntryBB = BasicBlock::Create(Ctx, "entry", RegTypeFunc);
  auto *StartId = &*RegTypeFunc->arg_begin();
  StartId->setName("startid");
  // Create loop to update indexes: NewTypeId = StartId + LocalTypId;
  auto *LoopBB = BasicBlock::Create(Ctx, "loop", RegTypeFunc);
  BranchInst::Create(LoopBB, EntryBB);
  auto *Counter = PHINode::Create(SizeTy, 0, "typeidx", LoopBB);
  Counter->addIncoming(Int0, EntryBB);
  auto *GEP = GetElementPtrInst::Create(
    nullptr, IdsArray, { Int0, Counter }, "arrayidx", LoopBB);
  auto *LocalTypeId = new LoadInst(GEP, "typeid", false, 0, LoopBB);
  auto Add = BinaryOperator::CreateNUW(
    BinaryOperator::Add, LocalTypeId, StartId, "add", LoopBB);
  new StoreInst(Add, GEP, false, 0, LoopBB);
  auto Inc = BinaryOperator::CreateNUW(BinaryOperator::Add, Counter,
    ConstantInt::get(SizeTy, 1), "inc", LoopBB);
  Counter->addIncoming(Inc, LoopBB);
  auto *Cmp = new ICmpInst(*LoopBB, CmpInst::ICMP_ULT, Inc, Size, "cmp");
  auto *EndBB = BasicBlock::Create(M.getContext(), "end", RegTypeFunc);
  BranchInst::Create(LoopBB, EndBB, Cmp, LoopBB);
  auto *IdsArg = GetElementPtrInst::Create(nullptr, IdsArray,
    { Int0, Int0 }, "ids", EndBB);
  auto *SizesArg = GetElementPtrInst::Create(nullptr, SizesArray,
    { Int0, Int0 }, "sizes", EndBB);
  CallInst::Create(DeclTypeFunc, { Size, IdsArg, SizesArg }, "", EndBB);
  ReturnInst::Create(Ctx, EndBB);
  NumType += Ids.size();
}

void Instrumentation::createInitDICall(const llvm::Twine &Str,
    DIStringRegister::IdTy Idx) {
  assert(mDIPool && "Pool of metadata strings must not be null!");
  assert(mInitDIAll &&
    "Metadata strings initialization function must not be null!");
  auto &BB = mInitDIAll->getEntryBlock();
  auto *T = BB.getTerminator();
  assert(T && "Terminator must not be null!");
  auto *M = mInitDIAll->getParent();
  auto InitDIFunc = getDeclaration(M, IntrinsicId::init_di);
  auto IdxV = ConstantInt::get(Type::getInt64Ty(M->getContext()), Idx);
  auto DIPoolPtr = new LoadInst(mDIPool, "dipool", T);
  auto GEP =
    GetElementPtrInst::Create(nullptr, DIPoolPtr, { IdxV }, "arrayidx", T);
  SmallString<256> SingleStr;
  auto DIString = createDIStringPtr(Str.toStringRef(SingleStr), *T);
  auto Offset = &*mInitDIAll->arg_begin();
  CallInst::Create(InitDIFunc, {GEP, DIString, Offset}, "", T);
}

GetElementPtrInst* Instrumentation::createDIStringPtr(
    StringRef Str, Instruction &InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto &M = *InsertBefore.getModule();
  auto Data = llvm::ConstantDataArray::getString(Ctx, Str);
  auto Var = new llvm::GlobalVariable(
    M, Data->getType(), true, GlobalValue::InternalLinkage, Data);
  Var->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  auto Int0 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0);
  return GetElementPtrInst::CreateInBounds(
    Var, { Int0,Int0 }, "distring", &InsertBefore);
}

LoadInst* Instrumentation::createPointerToDI(
    DIStringRegister::IdTy Idx, Instruction& InsertBefore) {
  auto &Ctx = InsertBefore.getContext();
  auto *MD = MDNode::get(Ctx, {});
  auto IdxV = ConstantInt::get(Type::getInt64Ty(Ctx), Idx);
  auto DIPoolPtr = new LoadInst(mDIPool, "dipool", &InsertBefore);
  DIPoolPtr->setMetadata("sapfor.da", MD);
  auto GEP = GetElementPtrInst::Create(nullptr, DIPoolPtr, {IdxV}, "arrayidx");
  GEP->setMetadata("sapfor.da", MD);
  GEP->insertAfter(DIPoolPtr);
  GEP->setIsInBounds(true);
  auto DI = new LoadInst(GEP, "di");
  DI->setMetadata("sapfor.da", MD);
  DI->insertAfter(GEP);
  return DI;
}

auto Instrumentation::regDebugLoc(
    const DebugLoc &DbgLoc) -> DIStringRegister::IdTy {
  assert(mDIPool && "Pool of metadata strings must not be null!");
  assert(mInitDIAll &&
    "Metadata strings initialization function must not be null!");
  // We use reserved index if source location is unknown.
  if (!DbgLoc)
    return DIStringRegister::indexOfItemType<DILocation *>();
  auto DbgLocIdx = mDIStrings.regItem(DbgLoc.get());
  auto *Scope = cast<DIScope>(DbgLoc->getScope());
  std::string ColStr = !DbgLoc.getCol() ? std::string("") :
    ("col1=" + Twine(DbgLoc.getCol()) + "*").str();
  createInitDICall(
    Twine("type=") + "file_name" + "*" +
    "file=" + Scope->getFilename() + "*" +
    "line1=" + Twine(DbgLoc.getLine()) + "*" + ColStr + "*", DbgLocIdx);
  return DbgLocIdx;
}

void Instrumentation::regValue(Value *V, Type *T, const DIMemoryLocation *DIM,
    DIStringRegister::IdTy Idx,  Instruction &InsertBefore, Module &M) {
  assert(V && "Variable must not be null!");
  LLVM_DEBUG(dbgs()<<"[INSTR]: register variable "<<(DIM ? "" : "without metadata ");
    V->printAsOperand(dbgs()); dbgs() << "\n");
  auto DeclStr = DIM && DIM->isValid() ?
    (Twine("file=") + DIM->Var->getFilename() + "*" +
      "line1=" + Twine(DIM->Var->getLine()) + "*").str() :
    (Twine("file=") + M.getSourceFileName() + "*").str();
  std::string NameStr;
  if (DIM && DIM->isValid())
    if (auto DWLang = getLanguage(*DIM->Var)) {
      SmallString<16> DIName;
      if (unparseToString(*DWLang, *DIM, DIName)) {
        std::replace(DIName.begin(), DIName.end(), '*', '^');
        NameStr = ("name1=" + DIName + "*").str();
      }
    }
  unsigned TypeId = mTypes.regItem(T);
  unsigned Rank;
  uint64_t ArraySize;
  std::tie(Rank, ArraySize) = arraySize(T);
  auto TypeStr = Rank == 0 ? (Twine("var_name") + "*").str() :
    (Twine("arr_name") + "*" + "rank=" + Twine(Rank) + "*").str();
  createInitDICall(
    Twine("type=") + TypeStr +
    "vtype=" + Twine(TypeId) + "*" + DeclStr + NameStr + "*",
    Idx);
  auto DIVar = createPointerToDI(Idx, InsertBefore);
  auto VarAddr = new BitCastInst(V,
    Type::getInt8PtrTy(M.getContext()), V->getName() + ".addr", &InsertBefore);
  VarAddr->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
  CallInst *Call = nullptr;
  if (Rank != 0) {
    auto Func = getDeclaration(&M, IntrinsicId::reg_arr);
    auto FuncTy = Func->getFunctionType();
    assert(FuncTy->getNumParams() > 2 && "Too few arguments!");
    auto Size = ConstantInt::get(FuncTy->getParamType(1), ArraySize);
    Call = CallInst::Create(Func, { DIVar, Size, VarAddr }, "", &InsertBefore);
    ++NumArray;
  } else {
    auto Func = getDeclaration(&M, IntrinsicId::reg_var);
   Call = CallInst::Create(Func, { DIVar, VarAddr }, "", &InsertBefore);
   ++NumScalar;
  }
  Call->setMetadata("sapfor.da", MDNode::get(M.getContext(), {}));
}

void Instrumentation::regFunctions(Module& M) {
  for (auto &F : M) {
    IntrinsicId LibId;
    if (getTsarLibFunc(F.getName(), LibId)) {
      F.setMetadata("sapfor.da", MDNode::get(F.getContext(), {}));
      continue;
    }
    if (F.getMetadata("sapfor.da"))
      continue;
    /// TODO (kaniandr@gmail.com): may be some other intrinsics also should be
    /// ignored, see llvm::AliasSetTracker::addUnknown() for details.
    switch (F.getIntrinsicID()) {
    case llvm::Intrinsic::dbg_declare: case llvm::Intrinsic::dbg_value:
    case llvm::Intrinsic::assume:
      continue;
    }
    auto Idx = mDIStrings.regItem(&F);
    regFunction(F, F.getReturnType(), F.getFunctionType()->getNumParams(),
      F.getSubprogram(), Idx, M);
  }
}

void Instrumentation::regGlobals(Module& M) {
  auto &Ctx = M.getContext();
  auto FuncType = FunctionType::get(Type::getVoidTy(Ctx), false);
  auto RegGlobalFunc = Function::Create(FuncType,
    GlobalValue::LinkageTypes::InternalLinkage, "sapfor.register.global", &M);
  auto *EntryBB = BasicBlock::Create(Ctx, "entry", RegGlobalFunc);
  auto *RetInst = ReturnInst::Create(mInitDIAll->getContext(), EntryBB);
  DIStringRegister::IdTy RegisteredGLobals = 0;
  for (auto I = M.global_begin(), EI = M.global_end(); I != EI; ++I) {
    if (I->getMetadata("sapfor.da"))
      continue;
    ++RegisteredGLobals;
    auto Idx = mDIStrings.regItem(&(*I));
    auto *MD = findMetadata(&*I);
    Optional<DIMemoryLocation> DIM;
    if (MD)
      DIM = DIMemoryLocation(MD, DIExpression::get(M.getContext(), {}));
    regValue(&*I, I->getValueType(), DIM ? &*DIM : nullptr, Idx, *RetInst, M);
  }
  if (RegisteredGLobals == 0)
    RegGlobalFunc->eraseFromParent();
  else
    addNameDAMetadata(*RegGlobalFunc, "sapfor.da", "sapfor.register.global");
}

/// Find available suffix for a specified name of a global object to resolve
/// conflicts between names in a specified module.
static Optional<unsigned> findAvailableSuffix(Module &M, unsigned MinSuffix,
    StringRef Name) {
  while (M.getNamedValue((Name + Twine(MinSuffix)).str())) {
    ++MinSuffix;
    if (MinSuffix == std::numeric_limits<unsigned>::max())
      return None;
  }
  return MinSuffix;
}

/// Find available suffix for a specified name of a global object to resolve
/// conflicts between names in a specified modules.
static Optional<unsigned> findAvailableSuffix(Module &M, unsigned MinSuffix,
    StringRef Name, ArrayRef<Module *> Modules) {
  auto Suffix = findAvailableSuffix(M, MinSuffix, Name);
  if (!Suffix)
    return None;
  for (auto *OtherM : Modules) {
    if (OtherM == &M)
      continue;
    Suffix = findAvailableSuffix(*OtherM, *Suffix, Name);
    if (!Suffix)
      return None;
  }
  return Suffix;
}

void tsar::visitEntryPoint(Function &Entry, ArrayRef<Module *> Modules) {
  LLVM_DEBUG(dbgs() << "[INSTR]: process entry point ";
    Entry.printAsOperand(dbgs()); dbgs() << "\n");
  // Erase all existent initialization functions from the modules and remember
  // index of metadata operand which points to the removed function.
  DenseMap<Module *, unsigned> InitMDToFuncOp;
  for (auto *M : Modules)
    if (auto OpIdx = eraseFromParent(*M, "sapfor.da", "sapfor.init.module"))
      InitMDToFuncOp.try_emplace(M, *OpIdx);
  Optional<unsigned> Suffix = 0;
  std::vector<unsigned> InitSuffixes;
  auto PoolSizeTy = Type::getInt64Ty(Entry.getContext());
  APInt PoolSize(PoolSizeTy->getBitWidth(), 0);
  for (auto *M: Modules) {
    LLVM_DEBUG(dbgs() << "[INSTR]: initialize module "
      << M->getSourceFileName() << "\n");
    auto NamedMD = M->getNamedMetadata("sapfor.da");
    if (!NamedMD) {
      M->getContext().diagnose(DiagnosticInfoInlineAsm(
        Twine("ignore ") + M->getSourceFileName() + " due to instrumentation "
        "is not available", DS_Warning));
      continue;
    }
    Suffix = findAvailableSuffix(*M, *Suffix, "sapfor.init.module", Modules);
    if (!Suffix)
      report_fatal_error(Twine("unable to initialize instrumentation for ") +
        M->getSourceFileName() + ": can not generate unique name"
        "of external function");
    InitSuffixes.push_back(*Suffix);
    // Now, we create a function to initialize instrumentation.
    auto IdTy = getInstrIdType(M->getContext());
    auto InitFuncTy = FunctionType::get(IdTy, { IdTy }, false);
    auto InitFunc = Function::Create(InitFuncTy, GlobalValue::ExternalLinkage,
      "sapfor.init.module" + Twine(*Suffix), M);
    assert(InitFunc->getName() == ("sapfor.init.module" + Twine(*Suffix)).str()
      && "Unable to initialized instrumentation for a module!");
    InitFunc->arg_begin()->setName("startid");
    auto BB = BasicBlock::Create(M->getContext(), "entry", InitFunc);
    auto NamedOpItr = InitMDToFuncOp.find(M);
    if (NamedOpItr == InitMDToFuncOp.end()) {
      addNameDAMetadata(*InitFunc, "sapfor.da", "sapfor.init.module");
    } else {
      auto InitMD = getMDOfKind(*NamedMD, "sapfor.init.module");
      InitFunc->setMetadata("sapfor.da", InitMD);
      InitMD->replaceOperandWith(NamedOpItr->second,
        ValueAsMetadata::get(InitFunc));
    }
    auto *DIPoolMD = getMDOfKind(*NamedMD, "sapfor.di.pool");
    if (!DIPoolMD || !extractMD<GlobalVariable>(*DIPoolMD).first ||
         !extractMD<ConstantInt>(*DIPoolMD).first)
      report_fatal_error(Twine("'sapfor.di.pool' is not available for ") +
        M->getSourceFileName());
    PoolSize += extractMD<ConstantInt>(*DIPoolMD).first->getValue();
    auto *InitDIMD = getMDOfKind(*NamedMD, "sapfor.init.di");
    if (!InitDIMD || !extractMD<Function>(*InitDIMD).first)
      report_fatal_error(Twine("'sapfor.init.di' is not available for ") +
        M->getSourceFileName());
    auto *InitDIFunc = extractMD<Function>(*InitDIMD).first;
    CallInst::Create(InitDIFunc, { &*InitFunc->arg_begin() }, "", BB);
    auto *RegTyMD = getMDOfKind(*NamedMD, "sapfor.register.type");
    if (!RegTyMD || !extractMD<Function>(*RegTyMD).first ||
        !extractMD<ConstantInt>(*RegTyMD).first)
      report_fatal_error(Twine("'sapfor.register.type' is not available for ") +
        M->getSourceFileName());
    auto *RegTyFunc = extractMD<Function>(*RegTyMD).first;
    CallInst::Create(RegTyFunc, { &*InitFunc->arg_begin() }, "", BB);
    if (auto *RegGlobalMD = getMDOfKind(*NamedMD, "sapfor.register.global"))
      if (auto *RegGlobalFunc = extractMD<Function>(*RegGlobalMD).first)
        CallInst::Create(RegGlobalFunc, {}, "", BB);
      else
        report_fatal_error(
          Twine("'sapfor.register.global' is not available for ") +
          M->getSourceFileName());
    auto FreeId =
      BinaryOperator::CreateNUW(BinaryOperator::Add, &*InitFunc->arg_begin(),
        extractMD<ConstantInt>(*RegTyMD).first, "add", BB);
    ReturnInst::Create(M->getContext(), FreeId, BB);
  }
  auto *EntryM = Entry.getParent();
  assert(EntryM && "Entry point must be in a module!");
  auto *InsertBefore = &Entry.getEntryBlock().front();
  auto AllocatePoolFunc = getDeclaration(EntryM, IntrinsicId::allocate_pool);
  auto PoolSizeV = ConstantInt::get(PoolSizeTy, PoolSize);
  auto *DIPool = getOrCreateDIPool(*EntryM);
  if (!DIPool)
    report_fatal_error(Twine("'sapfor.di.pool' is not available for ") +
      EntryM->getSourceFileName());
  auto CallAPF =
    CallInst::Create(AllocatePoolFunc, { DIPool, PoolSizeV}, "", InsertBefore);
  auto InstrMD = MDNode::get(EntryM->getContext(), {});
  CallAPF->setMetadata("sapfor.da", InstrMD);
  auto IdTy = getInstrIdType(Entry.getContext());
  auto *InitFuncTy = FunctionType::get(IdTy, { IdTy }, false);
  Value *FreeId = llvm::ConstantInt::get(IdTy, 0);
  for (auto Suffix : InitSuffixes) {
    auto *InitFunc = EntryM->getOrInsertFunction(
      ("sapfor.init.module" + Twine(Suffix)).str(), InitFuncTy);
    FreeId = CallInst::Create(InitFunc, {FreeId}, "freeid", InsertBefore);
    cast<CallInst>(FreeId)->setMetadata("sapfor.da", InstrMD);
  }
}
