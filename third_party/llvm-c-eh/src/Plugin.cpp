#include "Protocol.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

namespace {

/* Mandatory semantic lowering, including unoptimized/optnone functions. */
struct CEHLoweringPass : llvm::PassInfoMixin<CEHLoweringPass> {
  /* Exception semantics cannot be disabled by an optimization preference. */
  static bool isRequired() { return true; }

  /* Transform marked functions and erase declarations only after successful lowering. */
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
    if (!ceh::validateModule(M)) return llvm::PreservedAnalyses::all();
    bool Changed = false;
    for (llvm::Function &F : M) {
      if (F.isDeclaration()) continue;
      ceh::FunctionProtocol P(F);
      if (!ceh::parse(P)) return llvm::PreservedAnalyses::none();
      if (!P.Configure) continue;
      if (!ceh::lower(P)) return llvm::PreservedAnalyses::none();
      Changed = true;
    }
    llvm::SmallVector<llvm::Function *> Dead;
    for (llvm::Function &F : M)
      if (ceh::classify(F.getName()) != ceh::Marker::None) {
        if (!F.use_empty()) {
          ceh::error(F, "protocol symbol remains after lowering");
          return llvm::PreservedAnalyses::none();
        }
        Dead.push_back(&F);
      }
    for (llvm::Function *F : Dead) F->eraseFromParent();
    return Changed || !Dead.empty() ? llvm::PreservedAnalyses::none()
                                   : llvm::PreservedAnalyses::all();
  }
};

/* Register one automatic conversion and an explicit opt pipeline for standalone tests. */
void registerPasses(llvm::PassBuilder &PB) {
  PB.registerPipelineStartEPCallback(
      [](llvm::ModulePassManager &PM, llvm::OptimizationLevel) {
        PM.addPass(CEHLoweringPass());
      });
  PB.registerPipelineParsingCallback(
      [](llvm::StringRef Name, llvm::ModulePassManager &PM,
         llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
        if (Name != "c-eh-lowering") return false;
        PM.addPass(CEHLoweringPass());
        return true;
      });
}

} // namespace

/* Standard LLVM plugin handshake; no consuming-language symbol is referenced. */
extern "C" LLVM_ATTRIBUTE_VISIBILITY_DEFAULT LLVM_ATTRIBUTE_WEAK
llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "llvm-c-eh", CEH_PLUGIN_VERSION, registerPasses,
          nullptr};
}
