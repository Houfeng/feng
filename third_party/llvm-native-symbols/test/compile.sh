#!/usr/bin/env bash
# Use only compile-time artifacts so cross-target coverage requires no SDK or runner.
set -euo pipefail
if [[ $# != 3 ]]; then echo 'usage: compile.sh CLANG PLUGIN OUTPUT' >&2; exit 2; fi
clang=$1
plugin=$2
output=$3
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output"
for target in arm64-apple-macosx26.0.0 x86_64-apple-macosx14.0.0 aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu; do
    for level in 0 2 3; do
        prefix="$output/$target-O$level"
        "$clang" --target="$target" -O"$level" -fpass-plugin="$plugin" -S -emit-llvm \
            "$test_dir/calls.c" -o "$prefix.ll"
        "$clang" --target="$target" -O"$level" -fpass-plugin="$plugin" -S \
            "$test_dir/calls.c" -o "$prefix.s"
        if [[ $target == *apple* ]]; then
            grep -F '@sqrt' "$prefix.ll" >/dev/null
            if grep -F '\01_sqrt' "$prefix.ll" >/dev/null; then
                echo 'eligible sqrt alias was not normalized' >&2; exit 1
            fi
            grep -F '\01_foreign_mixed' "$prefix.ll" >/dev/null
            grep -F 'declare double @foreign_unknown(double' "$prefix.ll" >/dev/null
            grep -F '_sqrt' "$prefix.s" >/dev/null
        else
            "$clang" --target="$target" -O"$level" -S -emit-llvm \
                "$test_dir/calls.c" -o "$prefix.baseline.ll"
            cmp "$prefix.baseline.ll" "$prefix.ll"
        fi
        "$clang" --target="$target" -O"$level" -S -emit-llvm \
            "$test_dir/ordinary.c" -o "$prefix.ordinary-before.ll"
        "$clang" --target="$target" -O"$level" -fpass-plugin="$plugin" -S -emit-llvm \
            "$test_dir/ordinary.c" -o "$prefix.ordinary-after.ll"
        cmp "$prefix.ordinary-before.ll" "$prefix.ordinary-after.ll"
    done
done
grep -E '[[:space:]]fsqrt[[:space:]]' "$output/arm64-apple-macosx26.0.0-O2.s" >/dev/null
# Explicitly disabled builtin optimizations must remain disabled.
"$clang" --target=arm64-apple-macosx26.0.0 -O2 -fno-builtin -fpass-plugin="$plugin" \
    -S "$test_dir/calls.c" -o "$output/no-builtin.s"
if grep -E '[[:space:]]fsqrt[[:space:]]' "$output/no-builtin.s" >/dev/null; then
    echo 'nobuiltin was not preserved' >&2; exit 1
fi
if grep -E '[[:space:]](b|bl)[[:space:]]+sqrt([[:space:]]|$)' "$output/arm64-apple-macosx26.0.0-O2.s" >/dev/null; then
    echo 'incorrect unprefixed machine symbol' >&2; exit 1
fi
echo 'PASS: C aliases, multiple declarations, target symbols, O0/O2/O3 and nobuiltin'
