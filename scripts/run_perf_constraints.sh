#!/usr/bin/env bash
# Performance-constraint regression checks for fit call paths.
# Verifies codegen shape constraints for:
# 1) direct-call
# 2) spec-call
# 3) generic Set<int> direct-call
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FENG="$ROOT/build/bin/feng"
RT_LIB="$ROOT/build/lib/libfeng_runtime.a"
OUT_ROOT="$ROOT/build/gen/perf_constraints"
SMOKE_DIR="$ROOT/test/smoke/phase1a"

if [[ ! -x "$FENG" ]]; then
    echo "missing $FENG (run 'make cli' first)" >&2
    exit 2
fi
if [[ ! -f "$RT_LIB" ]]; then
    echo "missing $RT_LIB (run 'make runtime' first)" >&2
    exit 2
fi

failures=0

die_check() {
    local name="$1"
    local msg="$2"
    echo "FAIL[$name] $msg"
    failures=$((failures + 1))
}

compile_single_case() {
    local name="$1"
    local src="$2"
    local out_dir="$OUT_ROOT/$name"
    mkdir -p "$out_dir"
    if ! "$FENG" "$src" --target=lib --out="$out_dir" --name="$name" --keep-ir >"$out_dir/compile.log" 2>&1; then
        echo "FAIL[$name] compile"
        sed 's/^/  /' "$out_dir/compile.log"
        failures=$((failures + 1))
        return 1
    fi
}

compile_inline_case() {
    local name="$1"
    local src_text="$2"
    local out_dir="$OUT_ROOT/$name"
    mkdir -p "$out_dir"
    local src="$out_dir/$name.ff"
    printf "%s" "$src_text" > "$src"
    if ! "$FENG" "$src" --target=lib --out="$out_dir" --name="$name" --keep-ir >"$out_dir/compile.log" 2>&1; then
        echo "FAIL[$name] compile"
        sed 's/^/  /' "$out_dir/compile.log"
        failures=$((failures + 1))
        return 1
    fi
}

assert_contains() {
    local name="$1"
    local file="$2"
    local needle="$3"
    if ! grep -F -q "$needle" "$file"; then
        die_check "$name" "expected to contain: $needle"
    fi
}

assert_not_contains() {
    local name="$1"
    local file="$2"
    local needle="$3"
    if grep -F -q "$needle" "$file"; then
        die_check "$name" "unexpected pattern: $needle"
    fi
}

rm -rf "$OUT_ROOT"
mkdir -p "$OUT_ROOT"

# 1) direct-call path should not involve spec runtime dispatch.
compile_single_case "direct_call_builtin" "$SMOKE_DIR/fit_builtin_direct_call.ff" || true
if [[ -f "$OUT_ROOT/direct_call_builtin/ir/c/feng.c" ]]; then
    cfile="$OUT_ROOT/direct_call_builtin/ir/c/feng.c"
    assert_contains "direct_call_builtin" "$cfile" "FengFitBuiltin_"
    assert_not_contains "direct_call_builtin" "$cfile" "witness->"
    assert_not_contains "direct_call_builtin" "$cfile" "FengSpecThunk__"
    assert_not_contains "direct_call_builtin" "$cfile" "feng_scalar_box_new_"
fi

# 2) spec-call path keeps one-level witness dispatch shape.
compile_single_case "spec_call_witness" "$SMOKE_DIR/spec_fit_witness.ff" || true
if [[ -f "$OUT_ROOT/spec_call_witness/ir/c/feng.c" ]]; then
    cfile="$OUT_ROOT/spec_call_witness/ir/c/feng.c"
    assert_contains "spec_call_witness" "$cfile" "FengSpecThunk__"
    assert_contains "spec_call_witness" "$cfile" ".witness->"
    assert_not_contains "spec_call_witness" "$cfile" "witness->witness->"
fi

# 3) generic Set<int> direct-call path must stay monomorphized and no boxing/lookup.
GENERIC_SRC='mod feng.perf.g16;
type Set<T> {
    var value: T;
    fn put(next: T) {
        self.value = next;
    }
    fn get(): T {
        return self.value;
    }
}
fn use_it(): int {
    let set: Set<int> = Set:<int>();
    set.put(7);
    return set.get();
}
'
compile_inline_case "generic_set_int_direct" "$GENERIC_SRC" || true
if [[ -f "$OUT_ROOT/generic_set_int_direct/ir/c/feng.c" ]]; then
    cfile="$OUT_ROOT/generic_set_int_direct/ir/c/feng.c"
    assert_contains "generic_set_int_direct" "$cfile" "Set__G__int__put__from__i32"
    assert_contains "generic_set_int_direct" "$cfile" "Set__G__int__get__from__void"
    assert_not_contains "generic_set_int_direct" "$cfile" "feng_scalar_box_new_"
    assert_not_contains "generic_set_int_direct" "$cfile" "FengSpecThunk__"
    assert_not_contains "generic_set_int_direct" "$cfile" "FengSpecSlotWitness__"
    assert_not_contains "generic_set_int_direct" "$cfile" "witness->"
fi

if [[ $failures -gt 0 ]]; then
    echo "perf-constraints: $failures failed"
    exit 1
fi

echo "perf-constraints: all checks passed"
