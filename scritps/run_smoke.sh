#!/usr/bin/env bash
# Phase 1A smoke runner (P6 — direct compile mode).
#
# Cases live in test/smoke/phase1a/. Two layouts are supported:
#
#   1. Single-file case:  <name>.ff  +  <name>.expected
#   2. Multi-file case:   <name>/    +  <name>.expected
#                         The directory holds one or more *.ff files; they
#                         are passed to the compiler in deterministic
#                         (alphabetical) order.
#
# For each case we invoke `feng <files...> --target=bin --out=<gen-dir>
# --bin-name=<name> --keep-ir`, which produces:
#
#   <gen-dir>/ir/c/feng.c
#   <gen-dir>/bin/<name>
#
# The bin is executed and its stdout compared against <name>.expected.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FENG="$ROOT/build/bin/feng"
RT_LIB="$ROOT/build/lib/libfeng_runtime.a"
SMOKE_DIR="$ROOT/test/smoke/phase1a"
GEN_ROOT="$ROOT/build/gen/smoke"

if [[ ! -x "$FENG" ]]; then
    echo "missing $FENG (run 'make cli' first)" >&2
    exit 2
fi
if [[ ! -f "$RT_LIB" ]]; then
    echo "missing $RT_LIB (run 'make runtime' first)" >&2
    exit 2
fi

failures=0
total=0

run_case() {
    local name="$1"; shift
    local -a inputs=("$@")
    local expected="$SMOKE_DIR/$name.expected"
    local out_dir="$GEN_ROOT/$name"
    local bin="$out_dir/bin/$name"

    total=$((total + 1))
    rm -rf "$out_dir"
    mkdir -p "$out_dir"

    local log="$out_dir/compile.log"
    if ! "$FENG" "${inputs[@]}" --target=bin --out="$out_dir" \
            --bin-name="$name" --keep-ir >"$log" 2>&1; then
        echo "FAIL[$name] compile:"
        sed 's/^/  /' "$log"
        failures=$((failures + 1))
        return
    fi
    if [[ ! -x "$bin" ]]; then
        echo "FAIL[$name] expected binary $bin not produced"
        failures=$((failures + 1))
        return
    fi

    local actual
    actual="$("$bin" 2>&1 || true)"
    if [[ -f "$expected" ]]; then
        local want
        want="$(cat "$expected")"
        if [[ "$actual" != "$want" ]]; then
            echo "FAIL[$name] output mismatch"
            echo "  expected: $want"
            echo "  actual:   $actual"
            failures=$((failures + 1))
            return
        fi
    else
        echo "WARN[$name] no .expected file; output was:"
        echo "$actual"
    fi
    echo "ok   $name"
}

shopt -s nullglob

# Iterate single-file (*.ff) cases in alphabetical order.
for ff in "$SMOKE_DIR"/*.ff; do
    name="$(basename "$ff" .ff)"
    run_case "$name" "$ff"
done

# Iterate directory-based multi-file cases in alphabetical order. The .ff
# files inside are passed to the compiler in sorted order so codegen sees a
# deterministic input ordering.
for dir in "$SMOKE_DIR"/*/; do
    name="$(basename "$dir")"
    files=()
    while IFS= read -r f; do
        files+=("$f")
    done < <(find "$dir" -maxdepth 1 -name '*.ff' | sort)
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "WARN[$name] directory case has no .ff files; skipping"
        continue
    fi
    run_case "$name" "${files[@]}"
done

echo ""
if [[ $failures -gt 0 ]]; then
    echo "smoke: $failures of $total failed"
    exit 1
fi
echo "smoke: all $total passed"
