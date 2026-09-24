#!/usr/bin/env bash
# Verify lossless native-call conversion and reject malformed IR without Clang filtering it.
set -euo pipefail
if [[ $# != 3 ]]; then echo "usage: $0 LLVM_BIN PLUGIN OUTPUT_DIR" >&2; exit 2; fi
llvm_bin=$1
plugin=$2
output=$3
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$output"
"$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='c-eh-lowering,verify' \
    -S "$test_dir/attributes.ll" -o "$output/attributes.ll"
"$llvm_bin/FileCheck" "$test_dir/attributes.ll" < "$output/attributes.ll"
if grep -q '__llvm_c_eh_' "$output/attributes.ll"; then echo 'IR marker survived' >&2; exit 1; fi
# Running the pass again must be idempotent, including after ordinary optimization.
"$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='c-eh-lowering,default<O2>,c-eh-lowering,verify' \
    "$output/attributes.ll" -disable-output
cat > "$output/declarations.ll" <<'EOF'
declare void @__llvm_c_eh_configure(i32, ptr)
declare i1 @__llvm_c_eh_region(i32, i32, ptr, i32, ...)
declare void @__llvm_c_eh_activate(i32)
declare ptr @__llvm_c_eh_exception(i32)
declare i32 @__llvm_c_eh_selector(i32)
declare i32 @__llvm_c_eh_typeid(ptr)
declare void @__llvm_c_eh_propagate(i32) noreturn
declare i32 @personality(i32, i32, i64, ptr, ptr)
declare void @call()
EOF
# Each supplied module is valid LLVM IR but violates the C EH protocol.
reject() {
    local name=$1 expected=$2
    cat > "$output/$name.ll"
    if "$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='c-eh-lowering,verify' \
        "$output/$name.ll" -disable-output > "$output/$name.log" 2>&1; then
        echo "invalid IR accepted: $name" >&2; exit 1
    fi
    if ! grep -F 'error: llvm-c-eh:' "$output/$name.log" | grep -qF "$expected"; then
        cat "$output/$name.log" >&2; exit 1
    fi
    if grep -Eq 'PLEASE submit|Stack dump|Assertion.*failed|Segmentation fault' "$output/$name.log"; then
        cat "$output/$name.log" >&2; exit 1
    fi
}
reject signature 'invalid protocol declaration' <<'EOF'
declare void @__llvm_c_eh_activate(i64)
EOF
reject definition 'invalid protocol declaration' <<'EOF'
define void @__llvm_c_eh_activate(i32 %id) { ret void }
EOF
reject unknown 'unknown protocol operation' <<'EOF'
declare void @__llvm_c_eh_other()
EOF
reject global 'must declare functions' <<'EOF'
@__llvm_c_eh_activate = global i32 0
EOF
reject alias 'protocol aliases' <<'EOF'
define void @ordinary(i32 %id) { ret void }
@__llvm_c_eh_activate = alias void(i32), ptr @ordinary
EOF
reject escape 'require direct calls' <<'EOF'
declare void @__llvm_c_eh_activate(i32)
@escape = global ptr @__llvm_c_eh_activate
EOF
reject convention 'invalid protocol declaration' <<'EOF'
declare fastcc void @__llvm_c_eh_activate(i32)
EOF
{ cat "$output/declarations.ll"; cat <<'EOF'
define void @bad() {
  call void @__llvm_c_eh_configure(i32 1, ptr @personality) [ "deopt"(i32 0) ]
  ret void
}
EOF
} | reject bundle 'markers cannot have operand bundles'
{ cat "$output/declarations.ll"; cat <<'EOF'
define void @bad() {
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  call void @__llvm_c_eh_activate(i32 poison)
  ret void
}
EOF
} | reject poison_integer 'constant uint32_t'
{ cat "$output/declarations.ll"; cat <<'EOF'
define void @bad() {
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %x = call i32 @__llvm_c_eh_typeid(ptr undef)
  ret void
}
EOF
} | reject undef_key 'constant pointer'
{ cat "$output/declarations.ll"; cat <<'EOF'
define void @bad() {
entry:
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  %r = call i1 (i32,i32,ptr,i32,...) @__llvm_c_eh_region(i32 1,i32 0,ptr blockaddress(@bad,%landing),i32 0)
  br i1 %r,label %landing,label %normal
normal:
  call void @__llvm_c_eh_activate(i32 1)
  musttail call void @call()
  ret void
landing:
  ret void
}
EOF
} | reject musttail 'musttail cannot carry'
{ cat "$output/declarations.ll"; cat <<'EOF'
define void @bad() personality ptr @personality {
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  invoke void @call() to label %ok unwind label %unwind
ok:
  ret void
unwind:
  %e = landingpad {ptr,i32} cleanup
  resume {ptr,i32} %e
}
EOF
} | reject existing_eh 'already contains native'
{ cat "$output/declarations.ll"; cat <<'EOF'
declare i32 @other(i32,i32,i64,ptr,ptr)
define void @bad() personality ptr @other {
  call void @__llvm_c_eh_configure(i32 1, ptr @personality)
  ret void
}
EOF
} | reject conflict 'conflicts with the existing'
reject ifunc 'protocol ifuncs' <<'EOF'
define void @ordinary(i32 %id) { ret void }
define ptr @resolver() { ret ptr @ordinary }
@__llvm_c_eh_activate = ifunc void(i32), ptr @resolver
EOF
echo 'llvm-c-eh: IR call contracts, metadata, debug locations, idempotence and 14 invalid inputs passed'
