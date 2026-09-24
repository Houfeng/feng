; Handwritten IR covers contracts that C cannot express uniformly on every host.
; CHECK-NOT: @__llvm_c_eh_
; CHECK-LABEL: define i32 @contracts(
; CHECK-SAME: personality ptr @test_personality
; CHECK: call void @no_throw()
; CHECK: invoke fastcc noundef i32 @abi(ptr nonnull align 8 %value) [ "deopt"(i32 7) ]
; CHECK-NEXT: to label %{{.*}} unwind label %{{.*}}, !dbg !{{[0-9]+}}, !annotation !{{[0-9]+}}
; CHECK: call i32 @llvm.eh.typeid.for.p0(ptr @key)
; CHECK: resume { ptr, i32 }
; CHECK: landingpad { ptr, i32 }
; CHECK-NEXT: cleanup
; CHECK-NEXT: catch ptr @key
; CHECK-LABEL: define i32 @plain(
; CHECK-NEXT: entry:
; CHECK-NEXT: %result = add i32 %value, 1
; CHECK-NEXT: ret i32 %result
; CHECK-LABEL: define void @asm_goto(
; CHECK: callbr void asm sideeffect "", "!i"()
declare void @__llvm_c_eh_configure(i32, ptr)
declare i1 @__llvm_c_eh_region(i32, i32, ptr, i32, ...)
declare void @__llvm_c_eh_activate(i32)
declare ptr @__llvm_c_eh_exception(i32)
declare i32 @__llvm_c_eh_selector(i32)
declare i32 @__llvm_c_eh_typeid(ptr)
declare void @__llvm_c_eh_propagate(i32) noreturn
declare i32 @test_personality(i32, i32, i64, ptr, ptr)
declare fastcc noundef i32 @abi(ptr)
declare void @no_throw() nounwind
@key = external global i8

define i32 @contracts(ptr %value) !dbg !4 {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @test_personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@contracts, %landing), i32 1, ptr @key)
  br i1 %keep, label %landing, label %normal
normal:
  call void @__llvm_c_eh_activate(i32 1)
  call void @no_throw()
  %result = call fastcc noundef i32 @abi(ptr nonnull align 8 %value) [ "deopt"(i32 7) ], !dbg !7, !annotation !8
  ret i32 %result
landing:
  %selector = call i32 @__llvm_c_eh_selector(i32 1)
  %type = call i32 @__llvm_c_eh_typeid(ptr @key)
  %matches = icmp eq i32 %selector, %type
  br i1 %matches, label %handled, label %unhandled
handled:
  ret i32 7
unhandled:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

define i32 @plain(i32 %value) {
entry:
  %result = add i32 %value, 1
  ret i32 %result
}

; Legal asm-goto is nounwind and must retain its ordinary branch successors.
define void @asm_goto() {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @test_personality)
  %keep = call i1 (i32, i32, ptr, i32, ...) @__llvm_c_eh_region(i32 1, i32 0, ptr blockaddress(@asm_goto, %landing), i32 0)
  br i1 %keep, label %landing, label %normal
normal:
  call void @__llvm_c_eh_activate(i32 1)
  callbr void asm sideeffect "", "!i"() to label %done [label %alternate]
done:
  ret void
alternate:
  ret void
landing:
  call void @__llvm_c_eh_propagate(i32 1)
  unreachable
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "llvm-c-eh test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "contracts.c", directory: ".")
!2 = !{i32 2, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "contracts", scope: !1, file: !1, line: 10, type: !5, scopeLine: 10, spFlags: DISPFlagDefinition, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{}
!7 = !DILocation(line: 14, column: 9, scope: !4)
!8 = !{!"consumer-metadata"}
