# LLVM libunwind minimal source closure

This directory vendors the LLVM libunwind source files needed by Feng's native
exception backend.

Version: 20.1.8

Included:
- public unwind headers under include/
- native Itanium unwind sources used on macOS/Linux
- no tests, docs, CMake project files, or shared-library artifacts

Build:
- `make` builds a static archive and stages it into `../../build/lib` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_libunwind.a`
