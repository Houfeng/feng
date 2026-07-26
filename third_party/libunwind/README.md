# LLVM libunwind minimal source closure

This directory vendors the LLVM libunwind source files needed by Feng's native
exception backend.

Version: 20.1.8

Included:
- public unwind headers under include/
- native Itanium unwind sources used on macOS/Linux
- the LLVM `libunwind.cpp` implementation unit, compiled as C++ with exceptions
	and RTTI disabled
- no tests, docs, CMake project files, or shared-library artifacts

Build:
- `make` builds a static archive and stages it into `../../build/lib` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_unwind.a`
- Linux builds disable libunwind's optional `dladdr`-based function-name lookup;
  Feng only uses the unwind API, so generated programs do not need a separate
  `libdl` link dependency for an unused introspection path.
- the root Makefile unpacks this archive and merges its object files into
	`build/lib/<platform>/libfeng_runtime.a`; generated programs link only the
	matching complete-platform `libfeng_runtime`.

Note:
- This is LLVM libunwind, not the C-majority `libunwind/libunwind` project.
	The latter provides `_Unwind_*` APIs on ELF-oriented targets but is not a
	direct Darwin/Mach-O replacement in Feng's current backend.
