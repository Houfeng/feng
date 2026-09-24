#include "Protocol.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/LLVMContext.h"

using namespace llvm;

namespace ceh {

/* Keep recognition exact: a reserved but unknown operation is an input error. */
Marker classify(StringRef Name) {
  if (!Name.starts_with("__llvm_c_eh_")) return Marker::None;
  return StringSwitch<Marker>(Name)
      .Case("__llvm_c_eh_configure", Marker::Configure)
      .Case("__llvm_c_eh_region", Marker::Region)
      .Case("__llvm_c_eh_activate", Marker::Activate)
      .Case("__llvm_c_eh_exception", Marker::Exception)
      .Case("__llvm_c_eh_selector", Marker::Selector)
      .Case("__llvm_c_eh_typeid", Marker::TypeId)
      .Case("__llvm_c_eh_propagate", Marker::Propagate)
      .Default(Marker::Unknown);
}

/* Only direct symbol references carry protocol meaning. */
Marker classify(const CallBase &Call) {
  const auto *F = dyn_cast<Function>(Call.getCalledOperand()->stripPointerCasts());
  return F ? classify(F->getName()) : Marker::None;
}

/* Use the compiler diagnostic channel instead of assertions or process crashes. */
bool error(Function &F, const Twine &Message, const Instruction *At) {
  const std::string Text = ("llvm-c-eh: " + F.getName() + ": " + Message).str();
  if (At) F.getContext().emitError(At, Text);
  else F.getContext().emitError(Text);
  return false;
}

/* Build the exact opaque-pointer IR signature corresponding to the public C header. */
static FunctionType *signature(LLVMContext &C, Marker Kind) {
  Type *Void = Type::getVoidTy(C), *I32 = Type::getInt32Ty(C);
  Type *Ptr = PointerType::getUnqual(C);
  switch (Kind) {
  case Marker::Configure: return FunctionType::get(Void, {I32, Ptr}, false);
  case Marker::Region:
    return FunctionType::get(Type::getInt1Ty(C), {I32, I32, Ptr, I32}, true);
  case Marker::Activate:
  case Marker::Propagate: return FunctionType::get(Void, {I32}, false);
  case Marker::Exception: return FunctionType::get(Ptr, {I32}, false);
  case Marker::Selector: return FunctionType::get(I32, {I32}, false);
  case Marker::TypeId: return FunctionType::get(I32, {Ptr}, false);
  default: return nullptr;
  }
}

/* Reject all escaped marker references, including incompatible cast/alias calls. */
bool validateModule(Module &M) {
  for (GlobalIFunc &I : M.ifuncs())
    if (classify(I.getName()) != Marker::None) {
      M.getContext().emitError("llvm-c-eh: protocol ifuncs are not allowed");
      return false;
    }
  for (GlobalAlias &A : M.aliases())
    if (classify(A.getName()) != Marker::None) {
      M.getContext().emitError("llvm-c-eh: protocol aliases are not allowed");
      return false;
    }
  for (GlobalVariable &G : M.globals())
    if (classify(G.getName()) != Marker::None) {
      M.getContext().emitError("llvm-c-eh: protocol names must declare functions");
      return false;
    }
  for (Function &F : M) {
    Marker Kind = classify(F.getName());
    if (Kind == Marker::None) continue;
    if (Kind == Marker::Unknown) return error(F, "unknown protocol operation");
    if (!F.isDeclaration() || F.getCallingConv() != CallingConv::C ||
        F.getFunctionType() != signature(M.getContext(), Kind))
      return error(F, "invalid protocol declaration or definition");
    for (Use &U : F.uses()) {
      auto *Call = dyn_cast<CallInst>(U.getUser());
      if (!Call || &U != &Call->getCalledOperandUse() ||
          Call->getFunctionType() != F.getFunctionType() ||
          Call->getCallingConv() != CallingConv::C)
        return error(F, "protocol symbols require direct calls with their declared signature");
    }
  }
  return true;
}

/* Decode one unsigned protocol constant without truncation or undef acceptance. */
static bool number(Function &F, CallInst &C, unsigned Arg, uint32_t &Out) {
  const auto *N = dyn_cast<ConstantInt>(C.getArgOperand(Arg));
  if (!N || N->getBitWidth() != 32)
    return error(F, "protocol integers must be constant uint32_t values", &C);
  Out = static_cast<uint32_t>(N->getZExtValue());
  return true;
}

/* Native type tables retain global symbol identities or null, never arbitrary constants. */
static bool typeKey(const Value *V) {
  const auto *C = dyn_cast<Constant>(V);
  return C && C->getType()->isPointerTy() &&
         (C->isNullValue() || isa<GlobalValue>(C->stripPointerCasts()));
}

/* A retention edge may cross only compiler-created, unconditional goto blocks. */
static bool retentionEdge(CallInst &C, BasicBlock *Landing) {
  if (!C.hasOneUse()) return false;
  auto *Branch = dyn_cast<BranchInst>(*C.user_begin());
  if (!Branch || !Branch->isConditional() || Branch->getCondition() != &C ||
      Branch->getParent() != C.getParent()) return false;
  SmallPtrSet<BasicBlock *, 8> Seen;
  BasicBlock *B = Branch->getSuccessor(0);
  while (B != Landing) {
    if (!Seen.insert(B).second) return false;
    auto *Next = dyn_cast<BranchInst>(B->getTerminator());
    if (!Next || !Next->isUnconditional()) return false;
    for (Instruction &I : *B)
      if (&I != Next && !I.isDebugOrPseudoInst()) return false;
    B = Next->getSuccessor(0);
  }
  return true;
}

/* Validate the native personality ABI, keeping its symbol entirely consumer-owned. */
static bool configuration(FunctionProtocol &P, CallInst &C) {
  Function &F = P.Function;
  if (P.Configure || C.getParent() != &F.getEntryBlock())
    return error(F, "configure must occur exactly once in the entry block", &C);
  uint32_t Version;
  if (!number(F, C, 0, Version)) return false;
  if (Version != 1) return error(F, "unsupported protocol version", &C);
  auto *Personality = dyn_cast<Function>(C.getArgOperand(1)->stripPointerCasts());
  LLVMContext &Ctx = F.getContext();
  Type *I32 = Type::getInt32Ty(Ctx), *Ptr = PointerType::getUnqual(Ctx);
  auto *Expected = FunctionType::get(I32, {I32, I32, Type::getInt64Ty(Ctx), Ptr, Ptr}, false);
  if (!Personality || Personality->getFunctionType() != Expected ||
      Personality->getCallingConv() != CallingConv::C ||
      classify(Personality->getName()) != Marker::None)
    return error(F, "personality must reference a function with the native unwind signature", &C);
  if (F.hasPersonalityFn() && F.getPersonalityFn()->stripPointerCasts() != Personality)
    return error(F, "configured personality conflicts with the existing personality", &C);
  P.Configure = &C;
  P.Personality = Personality;
  return true;
}

/* Read a region declaration and verify its unique label and canonical retention edge. */
static bool declaration(FunctionProtocol &P, CallInst &C) {
  Function &F = P.Function;
  Region R;
  uint32_t Count;
  if (!number(F, C, 0, R.ID) || !number(F, C, 1, R.Parent) ||
      !number(F, C, 3, Count)) return false;
  auto *Address = dyn_cast<BlockAddress>(C.getArgOperand(2)->stripPointerCasts());
  if (!R.ID || P.Regions.count(R.ID)) return error(F, "invalid or duplicate region id", &C);
  if (!Address || Address->getFunction() != &F)
    return error(F, "landing must be a label in the same function", &C);
  if (C.arg_size() - 4 != Count) return error(F, "catch count does not match type arguments", &C);
  R.Body = Address->getBasicBlock();
  R.Declaration = &C;
  for (const auto &Item : P.Regions)
    if (Item.second.Body == R.Body) return error(F, "landing label belongs to multiple regions", &C);
  if (!retentionEdge(C, R.Body)) return error(F, "region must be used only by its landing retention branch", &C);
  for (uint32_t N = 0; N < Count; ++N) {
    Value *Key = C.getArgOperand(N + 4);
    if (!typeKey(Key)) return error(F, "catch type key must be a constant pointer to a global symbol or null", &C);
    auto *K = cast<Constant>(Key);
    if (K->isNullValue() && N + 1 != Count)
      return error(F, "catch-all must be the final type argument", &C);
    R.Types.push_back(K);
  }
  P.Regions.emplace(R.ID, std::move(R));
  return true;
}

/* Validate the complete region forest and every marker operand before mutation. */
bool parse(FunctionProtocol &P) {
  Function &F = P.Function;
  SmallVector<CallInst *> Markers;
  for (BasicBlock &B : F)
    for (Instruction &I : B)
      if (auto *C = dyn_cast<CallInst>(&I)) {
        Marker Kind = classify(*C);
        if (Kind == Marker::None) continue;
        if (C->hasOperandBundles() || C->isMustTailCall())
          return error(F, "protocol markers cannot have operand bundles or musttail", C);
        Markers.push_back(C);
        if (Kind == Marker::Configure && !configuration(P, *C)) return false;
        if (Kind == Marker::Region && !declaration(P, *C)) return false;
      }
  if (Markers.empty()) return true;
  if (!P.Configure) return error(F, "missing configure marker", Markers.front());
  for (BasicBlock &B : F)
    for (Instruction &I : B)
      if (I.isEHPad() || isa<InvokeInst, ResumeInst>(I))
        return error(F, "marked input already contains native exception control flow", &I);
  for (auto &Item : P.Regions) {
    Region *R = &Item.second;
    SmallPtrSet<Region *, 8> Seen;
    while (R->Parent) {
      if (!Seen.insert(R).second) return error(F, "cyclic region parent relationship", R->Declaration);
      auto Parent = P.Regions.find(R->Parent);
      if (Parent == P.Regions.end()) return error(F, "unknown parent region", R->Declaration);
      R = &Parent->second;
    }
  }
  for (CallInst *C : Markers) {
    Marker Kind = classify(*C);
    if (C != P.Configure && C->getParent() == P.Configure->getParent() &&
        C->comesBefore(P.Configure))
      return error(F, "configure must precede other protocol markers", C);
    if (Kind == Marker::TypeId) {
      if (!typeKey(C->getArgOperand(0))) return error(F, "typeid requires a constant pointer to a global symbol or null", C);
    } else if (Kind == Marker::Activate || Kind == Marker::Exception ||
               Kind == Marker::Selector || Kind == Marker::Propagate) {
      uint32_t ID;
      if (!number(F, *C, 0, ID)) return false;
      if ((!ID && Kind != Marker::Activate) || (ID && !P.Regions.count(ID)))
        return error(F, "unknown region id", C);
    }
  }
  return true;
}

} // namespace ceh
