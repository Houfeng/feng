#!/usr/bin/env bash
# Check shared runtime declarations using the actual plugin at every optimization level.
set -euo pipefail
if [[ $# != 3 ]]; then echo "usage: $0 CLANG PLUGIN OUTPUT_DIR" >&2; exit 2; fi
compiler=$1
plugin=$2
output=$3
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$test_dir/../.." && pwd)
mkdir -p -- "$output"
for level in 0 1 2 3; do
    for sanitizer in normal undefined; do
        flags=()
        if [[ "$sanitizer" == undefined ]]; then flags=(-fsanitize=undefined); fi
        ir="$output/O$level-$sanitizer.ll"
        "$compiler" -std=c11 -fexceptions -O"$level" -Wall -Wextra -Werror \
            -I"$repo/src/runtime" -I"$repo/third_party/llvm-c-eh/include" \
            -fpass-plugin="$plugin" ${flags[@]+"${flags[@]}"} \
            -S -emit-llvm "$test_dir/contracts.c" -o "$ir"
        sed -n '/^define.*@registration(/,/^}/p' "$ir" > "$output/registration.ll"
        test -s "$output/registration.ll"
        if grep -Eq '\binvoke\b|\blandingpad\b|\bresume\b' "$output/registration.ll"; then
            echo 'registration unexpectedly requires a native unwind edge' >&2; exit 1
        fi
        for operation in frame_push try_frame_push cleanup_push cleanup_push_aggregate \
                         defer_push cleanup_pop caught_value caught_clause frame_pop; do
            grep -Eq "call .*@feng_$operation\\(" "$output/registration.ll"
        done
        for operation in frame_release_to exception_catch_begin exception_catch_end release; do
            grep -Eq "invoke .*@feng_$operation\\(" "$ir"
        done
        sed -n '/^define.*@dynamic_call(/,/^}/p' "$ir" | grep -Eq 'invoke void %'
        sed -n '/^define.*@throwing(/,/^}/p' "$ir" | grep -Eq 'invoke void @feng_throw\('
        if grep -q '@__llvm_c_eh_' "$ir"; then echo 'unlowered protocol marker' >&2; exit 1; fi
    done
done
echo 'native EH contracts: registration, callbacks, unknown targets and noreturn at O0/O1/O2/O3 passed'
