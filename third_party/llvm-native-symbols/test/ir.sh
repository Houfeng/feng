#!/usr/bin/env bash
# Verify exact pass scope, ABI preservation and idempotence without backend noise.
set -euo pipefail
if [[ $# != 3 ]]; then echo 'usage: ir.sh LLVM_BIN PLUGIN OUTPUT' >&2; exit 2; fi
llvm_bin=$1
plugin=$2
output=$3
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output"
for target in arm64-apple-macosx26.0.0 x86_64-apple-macosx14.0.0; do
    sed "s/arm64-apple-macosx26.0.0/$target/" "$test_dir/cases.ll" > "$output/$target.in.ll"
    "$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='feng-native-symbols,verify' \
        -S - < "$output/$target.in.ll" > "$output/$target.ll"
    "$llvm_bin/FileCheck" "$test_dir/cases.ll" < "$output/$target.ll"
    "$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='feng-native-symbols,verify' \
        -S - < "$output/$target.ll" > "$output/$target.twice.ll"
    cmp "$output/$target.ll" "$output/$target.twice.ll"
done
for target in aarch64-unknown-linux-gnu x86_64-unknown-linux-gnu arm64-apple-ios18.0.0 x86_64-pc-windows-msvc; do
    sed "s/arm64-apple-macosx26.0.0/$target/" "$test_dir/cases.ll" > "$output/$target.in.ll"
    "$llvm_bin/opt" -passes=verify -S - < "$output/$target.in.ll" > "$output/$target.baseline.ll"
    "$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='feng-native-symbols,verify' \
        -S - < "$output/$target.in.ll" > "$output/$target.ll"
    cmp "$output/$target.baseline.ll" "$output/$target.ll"
done
# A macOS triple without the expected data layout must not be guessed from the host.
sed 's/e-m:o-i64:64-n32:64-S128/e-m:e-i64:64-n32:64-S128/' "$test_dir/cases.ll" > "$output/layout.in.ll"
"$llvm_bin/opt" -passes=verify -S - < "$output/layout.in.ll" > "$output/layout.baseline.ll"
"$llvm_bin/opt" -load-pass-plugin="$plugin" -passes='feng-native-symbols,verify' \
    -S - < "$output/layout.in.ll" > "$output/layout.ll"
cmp "$output/layout.baseline.ll" "$output/layout.ll"
echo 'PASS: IR scope, names, conflicts, ABIs, invoke, addresses and idempotence'
