#!/usr/bin/env bash
# Verify the O0 structural contract separately from optimizer-dependent layouts.
set -euo pipefail
if [[ $# != 3 ]]; then echo "usage: $0 LLVM_BIN PLUGIN OUTPUT_DIR" >&2; exit 2; fi
llvm_bin=$1
plugin=$2
output=$3
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$output"
"$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='c-eh-lowering,verify' \
    -S "$test_dir/cfg.ll" -o "$output/once.ll"
"$llvm_bin/FileCheck" "$test_dir/cfg.ll" < "$output/once.ll"
"$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='c-eh-lowering,verify' \
    -S "$output/once.ll" -o "$output/twice.ll"
# ModuleID changes with the input path; all actual IR must be byte-identical.
sed '/^; ModuleID =/d' "$output/once.ll" > "$output/once.body"
sed '/^; ModuleID =/d' "$output/twice.ll" > "$output/twice.body"
cmp "$output/once.body" "$output/twice.body"
echo 'llvm-c-eh: O0 glue removal, real unwind edges, optnone, passthrough and exact idempotence passed'
