#include "llvm/Support/LLVMDriver.h"

// The upstream standalone driver entry point, compiled with the patched library.
int dsymutil_main(int argc, char **argv, const llvm::ToolContext &context);

// Supply the same tool context as LLVM's generated standalone driver.
int main(int argc, char **argv) {
  return dsymutil_main(argc, argv, {argv[0], nullptr, false});
}
