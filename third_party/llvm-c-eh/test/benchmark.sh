#!/usr/bin/env bash
# Record cost and compiler output using identical normal-path computations in separate TUs.
set -euo pipefail
if [[ $# -lt 3 ]]; then echo "usage: $0 CLANG PLUGIN OUTPUT_DIR [--link-option=OPTION]" >&2; exit 2; fi
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
flags=(-std=gnu11 -fexceptions -O2 -g -Wall -Wextra -Werror "-fpass-plugin=$plugin"
       "-I$source_dir/include" "-I$source_dir/test")
if [[ $(uname -s) == Darwin ]]; then flags+=(-isysroot "$(xcrun --show-sdk-path)"); fi
for protected in 0 1; do
    "$compiler" "${flags[@]}" -DPROTECTED="$protected" "$source_dir/test/benchmark-call.c" \
        "$source_dir/test/benchmark.c" "$source_dir/test/producer.c" "$source_dir/test/runtime.c" \
        ${link_options[@]+"${link_options[@]}"} -o "$output/benchmark-$protected"
    "$compiler" "${flags[@]}" -DPROTECTED="$protected" -S "$source_dir/test/benchmark-call.c" \
        -o "$output/call-$protected.s"
    "$compiler" "${flags[@]}" -DPROTECTED="$protected" -c "$source_dir/test/benchmark-call.c" \
        -o "$output/call-$protected.o"
done
: > "$output/timing.txt"
for iteration in 1 2 3 4 5; do
    printf 'sample %s plain ' "$iteration" >> "$output/timing.txt"
    "$output/benchmark-0" >> "$output/timing.txt"
    printf 'sample %s protected ' "$iteration" >> "$output/timing.txt"
    "$output/benchmark-1" >> "$output/timing.txt"
    "$output/benchmark-1" throw >> "$output/timing.txt"
done
cat "$output/timing.txt"
