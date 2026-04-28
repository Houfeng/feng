#!/usr/bin/env bash
# Phase 1A smoke runner.
# For each <name>.ff in test/smoke/phase1a/, compile via `feng compile`,
# build with cc, run, and compare stdout against <name>.expected.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FENG="$ROOT/build/bin/feng"
RT_LIB="$ROOT/build/lib/libfeng_runtime.a"
GEN_DIR="$ROOT/build/gen/smoke"
BIN_DIR="$ROOT/build/bin/smoke"
SMOKE_DIR="$ROOT/test/smoke/phase1a"

mkdir -p "$GEN_DIR" "$BIN_DIR"

if [[ ! -x "$FENG" ]]; then
    echo "missing $FENG (run 'make cli' first)" >&2
    exit 2
fi
if [[ ! -f "$RT_LIB" ]]; then
    echo "missing $RT_LIB (run 'make runtime' first)" >&2
    exit 2
fi

CC="${CC:-cc}"
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -pedantic -I$ROOT/src"

failures=0
total=0

for ff in "$SMOKE_DIR"/*.ff; do
    name="$(basename "$ff" .ff)"
    total=$((total + 1))
    expected="$SMOKE_DIR/$name.expected"
    cgen="$GEN_DIR/$name.c"
    bin="$BIN_DIR/$name"

    if ! "$FENG" compile --emit-c="$cgen" "$ff" 2>"$GEN_DIR/$name.cgen.log"; then
        echo "FAIL[$name] codegen:"
        cat "$GEN_DIR/$name.cgen.log"
        failures=$((failures + 1))
        continue
    fi
    if ! $CC $CFLAGS "$cgen" "$RT_LIB" -lpthread -o "$bin" 2>"$GEN_DIR/$name.cc.log"; then
        echo "FAIL[$name] cc:"
        cat "$GEN_DIR/$name.cc.log"
        failures=$((failures + 1))
        continue
    fi
    actual="$("$bin" 2>&1 || true)"
    if [[ -f "$expected" ]]; then
        want="$(cat "$expected")"
        if [[ "$actual" != "$want" ]]; then
            echo "FAIL[$name] output mismatch"
            echo "  expected: $want"
            echo "  actual:   $actual"
            failures=$((failures + 1))
            continue
        fi
    else
        echo "WARN[$name] no .expected file; output was:"
        echo "$actual"
    fi
    echo "ok   $name"
done

echo ""
if [[ $failures -gt 0 ]]; then
    echo "smoke: $failures of $total failed"
    exit 1
fi
echo "smoke: all $total passed"
