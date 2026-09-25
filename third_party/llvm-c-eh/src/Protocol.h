#ifndef LLVM_C_EH_PROTOCOL_H
#define LLVM_C_EH_PROTOCOL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <map>

namespace ceh {

/* Closed vocabulary of compiler-only operations in protocol v1. */
enum class Marker { None, Configure, Region, Activate, Exception, Selector,
                    TypeId, Propagate, Unknown };

/* One function-local exception scope and its validated constant type keys. */
struct Region {
  uint32_t ID = 0;
  uint32_t Parent = 0;
  llvm::BasicBlock *Body = nullptr;
  llvm::CallInst *Declaration = nullptr;
  llvm::SmallVector<llvm::Constant *> Types;
  llvm::BasicBlock *Pad = nullptr;
  llvm::PHINode *State = nullptr;
};

/* Validated protocol inputs, owned only for the duration of one transformation. */
struct FunctionProtocol {
  llvm::Function &Function;
  llvm::Function *Personality = nullptr;
  llvm::CallInst *Configure = nullptr;
  std::map<uint32_t, Region> Regions;

  /* Bind the protocol to its source function without mutating the IR. */
  explicit FunctionProtocol(llvm::Function &F) : Function(F) {}
};

/* Classify an exact reserved symbol, distinguishing unknown reserved names. */
Marker classify(llvm::StringRef Name);

/* Get a direct marker callee without guessing the identity of indirect calls. */
Marker classify(const llvm::CallBase &Call);

/* Emit a normal compiler diagnostic, carrying the function and source location. */
bool error(llvm::Function &F, const llvm::Twine &Message,
           const llvm::Instruction *At = nullptr);

/* Reject malformed declarations and escaped protocol symbol addresses. */
bool validateModule(llvm::Module &M);

/* Collect and validate constant declarations before changing a function. */
bool parse(FunctionProtocol &P);

/* Convert validated regions into native EH; return false after a diagnostic. */
bool lower(FunctionProtocol &P);

/* Prove native no-unwind only for lowered definitions, including implicit calls. */
bool refineNoUnwind(llvm::ArrayRef<llvm::Function *> Functions);

} // namespace ceh
#endif
