#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

namespace {

/* Find incompatible direct uses once, independently of declaration ordering. */
llvm::DenseSet<const llvm::Function *> incompatibleCalls(const llvm::Module &M) {
  llvm::DenseSet<const llvm::Function *> Result;
  for (const llvm::Function &F : M)
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call) continue;
        const auto *Callee = llvm::dyn_cast<llvm::Function>(
            Call->getCalledOperand()->stripPointerCasts());
        if (Callee && (Callee->getFunctionType() != Call->getFunctionType() ||
                       Callee->getCallingConv() != Call->getCallingConv()))
          Result.insert(Callee);
      }
  return Result;
}

/* Recover an ordinary IR name only when its machine spelling is identical. */
llvm::StringRef normalizedName(const llvm::Function &F, const llvm::Module &M) {
  llvm::StringRef Name = F.getName();
  if (!F.isDeclaration() || !F.hasExternalLinkage() ||
      !Name.starts_with("\01_"))
    return {};
  llvm::StringRef Candidate = Name.drop_front(2);
  if (Candidate.empty() || Candidate.starts_with("llvm.") ||
      M.getNamedValue(Candidate))
    return {};
  for (unsigned char C : Candidate)
    if (C < 32 || C == 127) return {};

  llvm::SmallString<128> Before, After;
  llvm::Mangler::getNameWithPrefix(Before, Name, M.getDataLayout());
  llvm::Mangler::getNameWithPrefix(After, Candidate, M.getDataLayout());
  return Before == After ? Candidate : llvm::StringRef();
}

/* Restore library recognition without changing native symbols or call ABIs. */
struct NativeSymbolsPass : llvm::PassInfoMixin<NativeSymbolsPass> {
  /* Also validate the same symbol contract when Clang builds at O0. */
  static bool isRequired() { return true; }

  /* Leave unsupported targets and unprovable declarations untouched. */
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
    const auto &Target = M.getTargetTriple();
    if (!Target.isMacOSX() || !Target.isOSBinFormatMachO() ||
        M.getDataLayout().getGlobalPrefix() != '_')
      return llvm::PreservedAnalyses::all();

    const auto Incompatible = incompatibleCalls(M);
    llvm::TargetLibraryInfoImpl LibraryImplementation(Target);
    llvm::TargetLibraryInfo Libraries(LibraryImplementation);
    bool Changed = false;
    for (llvm::Function &F : M) {
      llvm::StringRef Name = normalizedName(F, M);
      if (Name.empty() || Incompatible.contains(&F)) continue;
      llvm::LibFunc LibraryFunction;
      if (Libraries.getLibFunc(Name, LibraryFunction) &&
          (!Libraries.isValidProtoForLibFunc(*F.getFunctionType(),
                                            LibraryFunction, M) ||
           !llvm::TargetLibraryInfoImpl::isCallingConvCCompatible(&F)))
        continue;
      // No uses, types, attributes, linkage or calling conventions are replaced.
      F.setName(Name);
      Changed = true;
    }
    return Changed ? llvm::PreservedAnalyses::none()
                   : llvm::PreservedAnalyses::all();
  }
};

/* Support normal Clang compilation and explicit, independently testable opt use. */
void registerPasses(llvm::PassBuilder &PB) {
  PB.registerPipelineStartEPCallback(
      [](llvm::ModulePassManager &PM, llvm::OptimizationLevel) {
        PM.addPass(NativeSymbolsPass());
      });
  PB.registerPipelineParsingCallback(
      [](llvm::StringRef Name, llvm::ModulePassManager &PM,
         llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
        if (Name != "feng-native-symbols") return false;
        PM.addPass(NativeSymbolsPass());
        return true;
      });
}

} // namespace

/* Export only the versioned LLVM pass-plugin handshake. */
extern "C" LLVM_ATTRIBUTE_VISIBILITY_DEFAULT LLVM_ATTRIBUTE_WEAK
llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "feng-native-symbols", "0.1.0",
          registerPasses, nullptr};
}
