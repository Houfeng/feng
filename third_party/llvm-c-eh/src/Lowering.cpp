#include "Protocol.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include <set>

using namespace llvm;

namespace ceh {
namespace {

/* Preserve each call's selected region until the CFG has been rebuilt. */
struct ProtectedCall {
  CallInst *Call;
  uint32_t RegionID;
};

/* Decode an operand already checked by the protocol validator. */
uint32_t id(const CallInst &C) {
  return static_cast<uint32_t>(cast<ConstantInt>(C.getArgOperand(0))->getZExtValue());
}

/* Keep ordinary entry and all declared exception roots while deleting synthetic edges. */
bool prepareCFG(FunctionProtocol &P) {
  Function &F = P.Function;
  for (auto &Item : P.Regions) {
    Region &R = Item.second;
    CallInst *C = R.Declaration;
    BasicBlock *B = C->getParent();
    C->replaceAllUsesWith(ConstantInt::getFalse(F.getContext()));
    C->eraseFromParent();
    R.Declaration = nullptr;
    ConstantFoldTerminator(B, true);
  }
  SmallVector<CallInst *> Propagations;
  for (BasicBlock &B : F)
    for (Instruction &I : B)
      if (auto *C = dyn_cast<CallInst>(&I))
        if (classify(*C) == Marker::Propagate) Propagations.push_back(C);
  // A protocol terminator owns all following control flow, including sanitizer
  // instrumentation. Its semantics never depend on a particular inserted call.
  for (CallInst *C : Propagations) {
    BasicBlock *B = C->getParent();
    B->splitBasicBlock(C->getNextNode(), "ceh.dead.continuation");
    B->getTerminator()->eraseFromParent();
    new UnreachableInst(F.getContext(), B);
  }
  SmallPtrSet<BasicBlock *, 32> Live;
  SmallVector<BasicBlock *> Work{&F.getEntryBlock()};
  for (auto &Item : P.Regions) Work.push_back(Item.second.Body);
  while (!Work.empty()) {
    BasicBlock *B = Work.pop_back_val();
    if (!Live.insert(B).second) continue;
    for (BasicBlock *S : successors(B)) Work.push_back(S);
  }
  SmallVector<BasicBlock *> Dead;
  for (BasicBlock &B : F)
    if (!Live.count(&B)) Dead.push_back(&B);
  DeleteDeadBlocks(Dead);
  for (auto &Item : P.Regions) {
    Region &R = Item.second;
    if (!pred_empty(R.Body) || R.Body == &F.getEntryBlock())
      return error(F, "ordinary control flow enters a landing label");
    if (R.Body->hasAddressTaken()) {
      BlockAddress *Address = BlockAddress::get(&F, R.Body);
      if (!Address->use_empty()) return error(F, "landing address escapes its region declaration");
      Address->destroyConstant();
    }
  }
  return true;
}

/* Solve region state on the CFG; kill markers reset sets and joins union them. */
bool analyze(FunctionProtocol &P, SmallVectorImpl<ProtectedCall> &Calls,
             SmallVectorImpl<CallInst *> &Markers) {
  Function &F = P.Function;
  DenseMap<BasicBlock *, std::set<uint32_t>> Inputs, Outputs;
  Inputs[&F.getEntryBlock()].insert(0);
  for (auto &Item : P.Regions) Inputs[Item.second.Body].insert(Item.second.Parent);
  bool Changed;
  do {
    Changed = false;
    for (BasicBlock &B : F) {
      auto State = Inputs[&B];
      for (Instruction &I : B)
        if (auto *C = dyn_cast<CallInst>(&I))
          if (classify(*C) == Marker::Activate) State = {id(*C)};
      if (State != Outputs[&B]) {
        Outputs[&B] = State;
        Changed = true;
      }
      for (BasicBlock *S : successors(&B)) {
        auto &In = Inputs[S];
        size_t Before = In.size();
        In.insert(State.begin(), State.end());
        Changed |= Before != In.size();
      }
    }
  } while (Changed);

  for (BasicBlock &B : F) {
    auto State = Inputs[&B];
    for (Instruction &I : B) {
      auto *C = dyn_cast<CallInst>(&I);
      if (!C) continue;
      Marker Kind = classify(*C);
      if (Kind != Marker::None) {
        Markers.push_back(C);
        if (Kind == Marker::Activate) State = {id(*C)};
        continue;
      }
      if (C->doesNotThrow()) continue;
      if (State.size() != 1) return error(F, "ambiguous region at a potentially unwinding call", C);
      uint32_t RegionID = *State.begin();
      if (!RegionID) continue;
      if (C->isMustTailCall()) return error(F, "musttail cannot carry a local unwind successor", C);
      Calls.push_back({C, RegionID});
    }
  }
  return true;
}

/* Create the native exception pair and retain effective parent handlers for phase one. */
void createLandings(FunctionProtocol &P) {
  Function &F = P.Function;
  LLVMContext &Ctx = F.getContext();
  auto *Pair = StructType::get(PointerType::getUnqual(Ctx), Type::getInt32Ty(Ctx));
  for (auto &Item : P.Regions) {
    Region &R = Item.second;
    R.Pad = BasicBlock::Create(Ctx, "ceh.landing", &F);
    IRBuilder<> Builder(R.Pad);
    auto *Landing = Builder.CreateLandingPad(Pair, 0, "ceh.exception.pair");
    Landing->setCleanup(true);
    for (uint32_t Current = R.ID; Current; Current = P.Regions.at(Current).Parent)
      for (Constant *Key : P.Regions.at(Current).Types) Landing->addClause(Key);
    R.State = PHINode::Create(Pair, 1, "ceh.state", R.Body->begin());
    R.State->addIncoming(Landing, R.Pad);
    Builder.CreateBr(R.Body);
  }
}

/* Continue the original pair through the lexical parent, not a mutable runtime slot. */
void propagate(FunctionProtocol &P, CallInst &C) {
  Region &R = P.Regions.at(id(C));
  BasicBlock *B = C.getParent();
  B->getTerminator()->eraseFromParent();
  IRBuilder<> Builder(B);
  Builder.SetCurrentDebugLocation(C.getDebugLoc());
  if (R.Parent) {
    Region &Parent = P.Regions.at(R.Parent);
    Builder.CreateBr(Parent.Body);
    Parent.State->addIncoming(R.State, B);
  } else {
    Builder.CreateResume(R.State);
  }
}

/* Split only the call's normal continuation and preserve its full call-site contract. */
void invoke(ProtectedCall &Site, FunctionProtocol &P) {
  CallInst *C = Site.Call;
  BasicBlock *B = C->getParent();
  BasicBlock *Normal = B->splitBasicBlock(C->getNextNode(), "ceh.continue");
  B->getTerminator()->eraseFromParent();
  SmallVector<Value *> Args(C->args());
  SmallVector<OperandBundleDef> Bundles;
  C->getOperandBundlesAsDefs(Bundles);
  auto *Invoke = InvokeInst::Create(C->getFunctionType(), C->getCalledOperand(),
      Normal, P.Regions.at(Site.RegionID).Pad, Args, Bundles, "", B);
  Invoke->setAttributes(C->getAttributes());
  Invoke->setCallingConv(C->getCallingConv());
  Invoke->copyIRFlags(C);
  Invoke->setDebugLoc(C->getDebugLoc());
  Invoke->copyMetadata(*C);
  Invoke->adoptDbgRecords(B, C->getIterator(), false);
  Invoke->takeName(C);
  C->replaceAllUsesWith(Invoke);
  C->eraseFromParent();
}

} // namespace

/* Assemble native CFG first, then check dominance before exposing exception values. */
bool lower(FunctionProtocol &P) {
  Function &F = P.Function;
  if (!prepareCFG(P)) return false;
  SmallVector<ProtectedCall> Calls;
  SmallVector<CallInst *> Markers;
  if (!analyze(P, Calls, Markers)) return false;
  F.setPersonalityFn(P.Personality);
  createLandings(P);
  for (CallInst *C : Markers)
    if (classify(*C) == Marker::Propagate) propagate(P, *C);
  for (auto &Site : Calls) invoke(Site, P);

  DominatorTree DT(F);
  for (CallInst *C : Markers) {
    Marker Kind = classify(*C);
    if (Kind == Marker::Exception || Kind == Marker::Selector || Kind == Marker::Propagate) {
      Region &R = P.Regions.at(id(*C));
      if (!DT.dominates(R.State, C))
        return error(F, "exception result is not available on every path to this marker", C);
    }
  }
  for (CallInst *C : Markers) {
    Marker Kind = classify(*C);
    IRBuilder<> Builder(C);
    Builder.SetCurrentDebugLocation(C->getDebugLoc());
    if (Kind == Marker::Exception || Kind == Marker::Selector) {
      Value *Result = Builder.CreateExtractValue(P.Regions.at(id(*C)).State,
                                                 Kind == Marker::Exception ? 0 : 1);
      C->replaceAllUsesWith(Result);
    } else if (Kind == Marker::TypeId) {
      auto *TypeID = Intrinsic::getOrInsertDeclaration(F.getParent(), Intrinsic::eh_typeid_for,
                                                    {C->getArgOperand(0)->getType()});
      C->replaceAllUsesWith(Builder.CreateCall(TypeID, {C->getArgOperand(0)}));
    }
    C->eraseFromParent();
  }
  std::string Details;
  raw_string_ostream Stream(Details);
  if (verifyFunction(F, &Stream)) return error(F, "invalid native EH output: " + Stream.str());
  return true;
}

} // namespace ceh
