#!/usr/bin/env bash
# Cross-compilation is recorded separately from target execution; no runtime is linked here.
set -euo pipefail
if [[ $# -lt 4 ]]; then
    echo "usage: $0 CLANG PLUGIN LLVM_BIN OUTPUT_DIR [TARGET_FLAGS...]" >&2; exit 2
fi
compiler=$1
plugin=$2
llvm_bin=$3
output=$4
shift 4
source_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p -- "$output"
flags=(-std=gnu11 -fexceptions -g -Wall -Wextra -Werror "-fpass-plugin=$plugin"
       "-I$source_dir/include" "-I$source_dir/test" "$@")
for level in 0 2 3; do
    for name in runtime producer smoke behavior; do
        "$compiler" "${flags[@]}" -O"$level" -c "$source_dir/test/$name.c" -o "$output/$name-O$level.o"
        "$llvm_bin/llvm-readobj" --file-headers --sections --relocations "$output/$name-O$level.o" \
            > "$output/$name-O$level.object.txt"
        "$llvm_bin/llvm-nm" --undefined-only "$output/$name-O$level.o" > "$output/$name-O$level.symbols.txt"
        if grep -q '__llvm_c_eh_' "$output/$name-O$level.symbols.txt"; then
            echo 'protocol symbol survived in target object' >&2; exit 1
        fi
    done
    grep -q 'gcc_except_tab' "$output/behavior-O$level.object.txt"
    grep -q 'ceh_test_personality' "$output/behavior-O$level.symbols.txt"
done
echo 'llvm-c-eh: target objects, exception tables and protocol elimination verified; execution is a separate check'
