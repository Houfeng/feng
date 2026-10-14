; REQUIRES: x86, aarch64
;
; The CFI escape forces DWARF unwind information, as in upstream eh-frame.s.
; Public LTO definitions must retain their FDE even though the diagnostic
; symbol origin remains a BitcodeFile. Native objects are the control.
;
; RUN: rm -rf %t; mkdir %t
; RUN: opt -mtriple=x86_64-apple-macosx11.0 -passes=verify %s -o %t/x64.bc
; RUN: opt -mtriple=arm64-apple-macosx11.0 -passes=verify %s -o %t/arm64.bc
; RUN: opt -module-summary %t/x64.bc -o %t/x64-thin.bc
; RUN: opt -module-summary %t/arm64.bc -o %t/arm64-thin.bc
; RUN: llc -filetype=obj %t/x64.bc -o %t/x64.o
; RUN: llc -filetype=obj %t/arm64.bc -o %t/arm64.o
; RUN: %lld -dylib -undefined dynamic_lookup %t/x64.o -o %t/x64-native
; RUN: %lld -dylib -undefined dynamic_lookup %t/x64.bc -o %t/x64-full
; RUN: %lld -dylib -undefined dynamic_lookup %t/x64-thin.bc -o %t/x64-thin
; RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup %t/arm64.o -o %t/arm64-native
; RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup %t/arm64.bc -o %t/arm64-full
; RUN: %lld -arch arm64 -dylib -undefined dynamic_lookup %t/arm64-thin.bc -o %t/arm64-thin
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/x64-native | FileCheck %s
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/x64-full | FileCheck %s
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/x64-thin | FileCheck %s
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/arm64-native | FileCheck %s
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/arm64-full | FileCheck %s
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/arm64-thin | FileCheck %s
;
; Exercise coalescing with a native weak definition in both input orders.
; RUN: sed 's/define void/define weak void/' %s > %t/weak.ll
; RUN: opt -mtriple=x86_64-apple-macosx11.0 -module-summary %t/weak.ll -o %t/weak.bc
; RUN: llc -filetype=obj %t/weak.bc -o %t/weak.o
; RUN: %lld -dylib -undefined dynamic_lookup %t/weak.bc %t/weak.o -o %t/weak-first
; RUN: %lld -dylib -undefined dynamic_lookup %t/weak.o %t/weak.bc -o %t/native-first
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/weak-first | FileCheck %s
; RUN: llvm-objdump --macho --syms --dwarf=frames %t/native-first | FileCheck %s
;
; CHECK: [[#%x,ADDRESS:]] {{.*}} __TEXT,__text _unwind_owner
; CHECK: FDE cie={{.*}} pc={{0*}}[[#%x,ADDRESS]]...

declare i32 @custom_personality(...)
declare void @may_throw()
declare void @perform_cleanup()

; Keep the tested definition away from the section's synthetic local symbol.
define void @preceding_function() noinline {
  call void @perform_cleanup()
  ret void
}

; The exported frame remains observable across the dynamic-library boundary.
define void @unwind_owner() noinline uwtable personality ptr @custom_personality {
entry:
  call void asm sideeffect ".cfi_escape 0", ""()
  invoke void @may_throw() to label %done unwind label %cleanup
done:
  ret void
cleanup:
  %exception = landingpad { ptr, i32 } cleanup
  call void @perform_cleanup()
  resume { ptr, i32 } %exception
}
