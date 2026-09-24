#!/usr/bin/env bash
# Compare identical non-protocol input with and without a Release plugin, byte for byte.
set -euo pipefail
if [[ $# -lt 3 ]]; then
    echo "usage: $0 CLANG PLUGIN OUTPUT_DIR [--sanitizer] [--link-option=OPTION]" >&2; exit 2
fi
compiler=$1
plugin=$2
output=$3
shift 3
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$output"
flags=(-std=gnu11 -fexceptions -g -Wall -Wextra -Werror)
if [[ $(uname -s) == Darwin ]]; then flags+=(-isysroot "$(xcrun --show-sdk-path)"); fi
sanitizer=false
link_options=()
for option in "$@"; do
    case "$option" in
        --sanitizer) flags+=(-fsanitize=undefined -fno-sanitize-recover=all); sanitizer=true;;
        --link-option=*) link_options+=("${option#*=}");;
        *) echo "unknown option: $option" >&2; exit 2;;
    esac
done
for level in 0 2 3; do
    for mode in plain plugin; do
        active=("${flags[@]}")
        if [[ $mode == plugin ]]; then active+=("-fpass-plugin=$plugin"); fi
        "$compiler" "${active[@]}" -O"$level" -S -emit-llvm "$source_dir/passthrough.c" -o "$output/$mode-O$level.ll"
        "$compiler" "${active[@]}" -O"$level" -S "$source_dir/passthrough.c" -o "$output/$mode-O$level.s"
        "$compiler" "${active[@]}" -O"$level" "$source_dir/passthrough.c" \
            ${link_options[@]+"${link_options[@]}"} -o "$output/$mode-O$level"
        "$output/$mode-O$level" > "$output/$mode-O$level.out"
        if "$sanitizer"; then
            if { "$output/$mode-O$level" 1 > "$output/$mode-O$level.ubsan" 2>&1; } 2>/dev/null; then
                echo 'sanitizer failed to diagnose overflow' >&2; exit 1
            fi
            grep -q 'runtime error: signed integer overflow' "$output/$mode-O$level.ubsan"
        fi
    done
    for extension in ll s out; do cmp "$output/plain-O$level.$extension" "$output/plugin-O$level.$extension"; done
    if "$sanitizer"; then cmp "$output/plain-O$level.ubsan" "$output/plugin-O$level.ubsan"; fi
done
echo 'llvm-c-eh: non-protocol IR, assembly, results and requested sanitizer diagnostics unchanged'
