; Positive and conservative cases share a module to exercise name collisions.
target datalayout = "e-m:o-i64:64-n32:64-S128"
target triple = "arm64-apple-macosx26.0.0"

; CHECK: @data = global i32 7
@data = global i32 7
; CHECK: @alias = alias double (double), ptr @body
@alias = alias double (double), ptr @body
; CHECK: declare double @sqrt(double)
declare double @"\01_sqrt"(double)
; CHECK: declare ptr @ordinary(ptr)
declare ptr @"\01_ordinary"(ptr)
; CHECK: declare i32 @_leading(i32)
declare i32 @"\01__leading"(i32)
; CHECK: declare i32 @printf(ptr, ...)
declare i32 @"\01_printf"(ptr, ...)
; CHECK: declare double @"\01_native"(double)
declare double @"\01_native"(double)
; CHECK: declare double @native(double)
declare double @native(double)
; CHECK: declare i32 @"\01_data"()
declare i32 @"\01_data"()
; CHECK: declare double @"\01_alias"(double)
declare double @"\01_alias"(double)
; CHECK: declare extern_weak double @"\01_weak"(double)
declare extern_weak double @"\01_weak"(double)
; CHECK: declare i64 @"\01_sqrtf"(i64)
declare i64 @"\01_sqrtf"(i64)
; CHECK: declare fastcc double @"\01_cbrt"(double)
declare fastcc double @"\01_cbrt"(double)
; CHECK: declare double @"\01_fabs"(double)
declare double @"\01_fabs"(double)
; CHECK: declare i64 @"\01_mixed"(i64)
declare i64 @"\01_mixed"(i64)
; CHECK: declare void @"\01_convention"()
declare void @"\01_convention"()
; CHECK: declare double @"\01sqrtl"(double)
declare double @"\01sqrtl"(double)
; CHECK: declare void @"\01_"()
declare void @"\01_"()
; CHECK: declare void @"\01_llvm.fake"()
declare void @"\01_llvm.fake"()
; CHECK: declare void @"\01_control\09"()
declare void @"\01_control\09"()
; CHECK: declare i32 @personality(...)
declare i32 @personality(...)

; CHECK: define double @body(double %x)
define double @body(double %x) { ret double %x }
; CHECK: define double @"\01_defined"(double %x)
define double @"\01_defined"(double %x) { ret double %x }
; CHECK: define ptr @address()
; CHECK: ret ptr @sqrt
define ptr @address() { ret ptr @"\01_sqrt" }
; CHECK: define double @invoke_sqrt(double %x) personality ptr @personality
; CHECK: invoke double @sqrt(double %x)
; CHECK: landingpad { ptr, i32 }
; CHECK: resume { ptr, i32 }
define double @invoke_sqrt(double %x) personality ptr @personality {
  %value = invoke double @"\01_sqrt"(double %x) to label %ok unwind label %unwind
ok:
  ret double %value
unwind:
  %exception = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %exception
}
; These deliberately use a different call ABI; normalization must leave them alone.
; CHECK: call float @"\01_fabs"(float %x)
; CHECK: call i32 @"\01_mixed"(i32 1)
; CHECK: call fastcc void @"\01_convention"()
define float @mixed_calls(float %x) {
  %a = call float @"\01_fabs"(float %x)
  %b = call i32 @"\01_mixed"(i32 1)
  call fastcc void @"\01_convention"()
  ret float %a
}
; CHECK: call double @sqrt(double %x) #[[NOBUILTIN:[0-9]+]]
define double @no_builtin(double %x) {
  %r = call double @"\01_sqrt"(double %x) nobuiltin
  ret double %r
}
; CHECK: attributes #[[NOBUILTIN]] = { nobuiltin }
