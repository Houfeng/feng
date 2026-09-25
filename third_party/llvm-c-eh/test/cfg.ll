; Structural lowering is mandatory at O0, but ordinary unmarked CFG is untouched.
; CHECK-LABEL: define i64 @nonthrowing(
; CHECK-SAME: #[[SAFE:[0-9]+]]
; CHECK-NEXT: entry:
; CHECK-NEXT: %result = call i64 @safe(i64 %value)
; CHECK-NEXT: ret i64 %result
; CHECK-NEXT: }
; CHECK-LABEL: define i64 @protected(
; CHECK-SAME: #[[DEBUG:[0-9]+]]
; CHECK-NEXT: entry:
; CHECK-NEXT: %result = invoke i64 @unknown(i64 %value)
; CHECK-NEXT: to label %[[NORMAL:[^ ]+]] unwind label %[[LANDING:[^, ]+]]
; CHECK: [[NORMAL]]:
; CHECK-NEXT: ret i64 %result
; CHECK: [[LANDING]]:
; CHECK-NEXT: %[[PAIR:[^ ]+]] = landingpad { ptr, i32 }
; CHECK-NEXT: cleanup
; CHECK-NEXT: resume { ptr, i32 } %[[PAIR]]
; CHECK-NEXT: }
; CHECK-LABEL: define i64 @untouched(
; CHECK-NEXT: entry:
; CHECK-NEXT: br label %middle
; CHECK: middle:
; CHECK-NEXT: br label %body
; CHECK: body:
; CHECK-NEXT: ret i64 %value
; CHECK-NEXT: }
; CHECK-LABEL: define i64 @caller(
; CHECK-SAME: #[[SAFE]]
; CHECK-NEXT: entry:
; CHECK-NEXT: %result = call i64 @nonthrowing(i64 %value)
; CHECK-NEXT: ret i64 %result
; CHECK-NEXT: }
; CHECK-LABEL: define i64 @catch_unknown(
; CHECK-SAME: #[[DEBUG]]
; CHECK: invoke i64 @unknown(i64 %value)
; CHECK: landingpad { ptr, i32 }
; CHECK-NEXT: cleanup
; CHECK-NEXT: catch ptr null
; CHECK-LABEL: define weak i64 @replaceable(
; CHECK-SAME: #[[DEBUG]]
; CHECK: call i64 @safe(i64 %value)
; CHECK-LABEL: define i64 @dynamic(
; CHECK-SAME: #[[DEBUG]]
; CHECK: invoke i64 %target(i64 %value)
; CHECK: resume { ptr, i32 }
; CHECK-LABEL: define i64 @recursive(
; CHECK-SAME: #[[DEBUG]]
; CHECK: invoke i64 @recursive(i64 %value)
; CHECK: resume { ptr, i32 }
; CHECK-LABEL: define i64 @weak_caller(
; CHECK-SAME: #[[DEBUG]]
; CHECK: invoke i64 @replaceable(i64 %value)
; CHECK: resume { ptr, i32 }
; CHECK: attributes #[[SAFE]] = { noinline nounwind optnone }
; CHECK: attributes #[[DEBUG]] = { noinline optnone }

declare void @__llvm_c_eh_configure(i32, ptr)
declare i1 @__llvm_c_eh_region(i32, i32, ptr, i32, ...)
declare void @__llvm_c_eh_activate(i32)
declare void @__llvm_c_eh_propagate(i32) noreturn
declare i32 @personality(i32, i32, i64, ptr, ptr)
declare i64 @safe(i64) nounwind
declare i64 @unknown(i64)

define i64 @nonthrowing(i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@nonthrowing, %landing), i32 0)
  br i1 %keep, label %landing, label %middle
middle:
  br label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 @safe(i64 %value)
  ret i64 %result
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

define i64 @protected(i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@protected, %landing), i32 0)
  br i1 %keep, label %landing, label %middle
middle:
  br label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 @unknown(i64 %value)
  ret i64 %result
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

define i64 @untouched(i64 %value) noinline optnone {
entry:
  br label %middle
middle:
  br label %body
body:
  ret i64 %value
}

; Visit the caller before the callee in the work queue to exercise propagation.
define i64 @caller(i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@caller, %landing), i32 0)
  br i1 %keep, label %landing, label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 @nonthrowing(i64 %value)
  ret i64 %result
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

; A catch-all is interpreted by a consumer personality: do not assume that it
; catches foreign exception classes, or confuse it with a safe callee contract.
define i64 @catch_unknown(i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@catch_unknown, %landing), i32 1, ptr null)
  br i1 %keep, label %landing, label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 @unknown(i64 %value)
  ret i64 %result
landing:
  ret i64 0
}

; A weak definition does not establish the contract of its replacement.
define weak i64 @replaceable(i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@replaceable, %landing), i32 0)
  br i1 %keep, label %landing, label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 @safe(i64 %value)
  ret i64 %result
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

; No body or call-site contract is available for a dynamic target.
define i64 @dynamic(ptr %target, i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@dynamic, %landing), i32 0)
  br i1 %keep, label %landing, label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 %target(i64 %value)
  ret i64 %result
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

; A recursive unknown edge must not bootstrap its own no-unwind proof.
define i64 @recursive(i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@recursive, %landing), i32 0)
  br i1 %keep, label %landing, label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 @recursive(i64 %value)
  ret i64 %result
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

; A call to an interposable definition keeps its native unwind successor.
define i64 @weak_caller(i64 %value) noinline optnone {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@weak_caller, %landing), i32 0)
  br i1 %keep, label %landing, label %body
body:
  call void @__llvm_c_eh_activate(i32 1)
  %result = call i64 @replaceable(i64 %value)
  ret i64 %result
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}
