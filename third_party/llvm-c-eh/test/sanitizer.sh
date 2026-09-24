#!/usr/bin/env bash
# Test real UBSan checks in converted functions, including both fatal and recover modes.
set -euo pipefail
if [[ $# -lt 3 ]]; then
    echo "usage: $0 CLANG RELEASE_PLUGIN OUTPUT_DIR [--link-option=OPTION]" >&2; exit 2
fi
compiler=$1
plugin=$2
output=$3
shift 3
link_options=()
for option in "$@"; do
    case "$option" in
        --link-option=*) link_options+=("${option#*=}");;
        *) echo "unknown option: $option" >&2; exit 2;;
    esac
done
source_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p -- "$output"
flags=(-std=gnu11 -fexceptions -g -Wall -Wextra -Werror -fsanitize=undefined
       "-fpass-plugin=$plugin" "-I$source_dir/include" "-I$source_dir/test")
if [[ $(uname -s) == Darwin ]]; then flags+=(-isysroot "$(xcrun --show-sdk-path)"); fi
sources=("$source_dir/test/runtime.c" "$source_dir/test/sanitizer.c")
modes=(o b f l)
diagnostics=('signed integer overflow' 'index 2 out of bounds' 'incorrect function type' 'signed integer overflow')
for level in 0 2 3; do
    fatal="$output/fatal-O$level"
    "$compiler" "${flags[@]}" -O"$level" -fno-sanitize-recover=all "${sources[@]}" \
        ${link_options[@]+"${link_options[@]}"} -o "$fatal"
    "$fatal" valid > "$output/valid-O$level.log"
    grep -q '^body completed$' "$output/valid-O$level.log"
    for ((i=0; i<${#modes[@]}; ++i)); do
        log="$output/fatal-O$level-${modes[i]}.log"
        if { "$fatal" "${modes[i]}" > "$log" 2>&1; } 2>/dev/null; then
            echo "UBSan did not terminate ${modes[i]} at O$level" >&2; exit 1
        fi
        grep -qF "${diagnostics[i]}" "$log"
        ! grep -Eq 'body completed|handler completed|test failure' "$log"
    done
    recover="$output/recover-O$level"
    "$compiler" "${flags[@]}" -O"$level" -fsanitize-recover=all "${sources[@]}" \
        ${link_options[@]+"${link_options[@]}"} -o "$recover"
    for mode in o l; do
        log="$output/recover-O$level-$mode.log"
        "$recover" "$mode" > "$log" 2>&1
        grep -qF 'signed integer overflow' "$log"
        if [[ $mode == o ]]; then grep -q '^body completed$' "$log"
        else grep -q '^handler completed$' "$log"; fi
    done
done
echo 'llvm-c-eh: UBSan body/handler checks, indirect signatures, bounds, fatal and recovery modes passed'
