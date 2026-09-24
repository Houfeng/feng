#!/usr/bin/env bash
# Exercise a prebuilt plugin with one explicit compiler; never build the plugin here.
set -euo pipefail
if [[ $# -lt 3 ]]; then
    echo "usage: $0 CLANG PLUGIN OUTPUT_DIR [--sanitizer] [--link-option=OPTION]" >&2
    exit 2
fi
compiler=$1
plugin=$2
output=$3
shift 3
sanitizer=
link_options=()
for option in "$@"; do
    case "$option" in
        --sanitizer) sanitizer=undefined;;
        --sanitizer=*) sanitizer=${option#*=};;
        --link-option=*) link_options+=("${option#*=}");;
        *) echo "unknown option: $option" >&2; exit 2;;
    esac
done
source_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p -- "$output"
output=$(cd -- "$output" && pwd)
plugin=$(cd -- "$(dirname -- "$plugin")" && pwd)/$(basename -- "$plugin")
"$compiler" --version > "$output/compiler.txt"
if ! head -1 "$output/compiler.txt" | grep -Eq 'clang version 22\.1\.8([[:space:]]|$)'; then
    echo 'llvm-c-eh tests require Clang 22.1.8' >&2
    exit 1
fi
flags=(-std=gnu11 -fexceptions -g -Wall -Wextra -Werror "-fpass-plugin=$plugin"
       "-I$source_dir/include" "-I$source_dir/test")
if [[ $(uname -s) == Darwin ]]; then flags+=(-isysroot "$(xcrun --show-sdk-path)"); fi
if [[ -n "$sanitizer" ]]; then
    flags+=("-fsanitize=$sanitizer" -fno-sanitize-recover=all)
fi
sources=("$source_dir/test/runtime.c" "$source_dir/test/producer.c"
         "$source_dir/test/smoke.c" "$source_dir/test/behavior.c")
for level in 0 2 3; do
    "$compiler" "${flags[@]}" -O"$level" "${sources[@]}" -pthread ${link_options[@]+"${link_options[@]}"} -o "$output/behavior-O$level"
    "$output/behavior-O$level"
    "$compiler" "${flags[@]}" -O"$level" -S -emit-llvm "$source_dir/test/behavior.c" -o "$output/behavior-O$level.ll"
    if grep -Eq '(call|invoke|declare).*@__llvm_c_eh_|blockaddress' "$output/behavior-O$level.ll"; then
        echo "protocol remained at O$level" >&2; exit 1
    fi
    grep -q 'landingpad' "$output/behavior-O$level.ll"
    grep -q 'invoke' "$output/behavior-O$level.ll"
done
# The actual inline_throw body must disappear without disabling optimization.
if grep -Eq '^define.*@inline_throw\(' "$output/behavior-O2.ll"; then
    echo 'required inline fixture did not inline' >&2; exit 1
fi
# Independently named personality and keys prove there are no language-owned symbols.
"$compiler" "${flags[@]}" -O2 -DCEH_TEST_PERSONALITY=alternate_consumer_personality \
    -Dceh_test_key_a=alternate_identity_a -Dceh_test_key_b=alternate_identity_b \
    "${sources[@]}" -pthread ${link_options[@]+"${link_options[@]}"} -o "$output/alternate"
"$output/alternate"
expected=(
    'missing configure' 'unsupported protocol version' 'constant uint32_t'
    'exactly once in the entry block' 'exactly once in the entry block' 'must precede'
    'native unwind signature' 'invalid or duplicate region' 'invalid or duplicate region'
    'unknown parent' 'cyclic region parent' 'label in the same function' 'catch count'
    'constant pointer' 'catch-all must be' 'constant uint32_t' 'multiple regions'
    'retention branch' 'retention branch' 'ordinary control flow' 'address escapes'
    'unknown region' 'constant uint32_t' 'ambiguous region' 'not available'
    'not available' 'not available' 'constant pointer' 'unknown region' 'constant pointer'
    'constant pointer' 'constant pointer' 'constant pointer'
)
for ((i=1; i<=${#expected[@]}; ++i)); do
    log="$output/invalid-$i.log"
    if "$compiler" "${flags[@]}" -Wno-unused-label -Wno-unused-parameter -O0 -DCASE="$i" \
        -c "$source_dir/test/invalid.c" -o "$output/invalid.o" > "$log" 2>&1; then
        echo "invalid protocol case $i compiled" >&2; exit 1
    fi
    if ! grep -F 'error: llvm-c-eh:' "$log" | grep -qF "${expected[i-1]}"; then
        cat "$log" >&2; echo "wrong diagnostic for case $i" >&2; exit 1
    fi
    if grep -Eq 'PLEASE submit|Stack dump|Assertion.*failed|Segmentation fault' "$log"; then
        cat "$log" >&2; echo "compiler crashed for case $i" >&2; exit 1
    fi
done
echo 'llvm-c-eh: 33 malformed protocol inputs rejected without crashes'
