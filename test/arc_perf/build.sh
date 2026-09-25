#!/usr/bin/env bash
# Build the exact requested compiler/runtime as both a source and binary package.
set -euo pipefail
if [[ $# != 3 ]]; then echo "usage: $0 FENG LLVM_ROOT OUTPUT_DIR" >&2; exit 2; fi
feng=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
sdk=$(cd -- "$2" && pwd)
mkdir -p -- "$3"
output=$(cd -- "$3" && pwd)
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$source_dir/../.." && pwd)
[[ $("$sdk/bin/llvm-config" --version) == 22.1.8 ]] || exit 2
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) platform=macos-arm64; library=llvm_c_eh.dylib;;
    Linux-aarch64) platform=linux-arm64-gnu; library=llvm_c_eh.so;;
    Linux-x86_64) platform=linux-x64-gnu; library=llvm_c_eh.so;;
    *) echo 'unsupported benchmark host' >&2; exit 2;;
esac
tool_root=$(cd -- "$(dirname -- "$feng")/.." && pwd)
plugin="$tool_root/toolchain/llvm-c-eh/lib/$library"
if [[ $platform == macos-arm64 ]]; then
    mkdir -p "$output/tools"
    ln -sf "$repo/toolchain/llvm/$platform/bin/lld" "$output/tools/ld64.lld"
    export COMPILER_PATH="$output/tools${COMPILER_PATH:+:$COMPILER_PATH}"
fi
cp "$source_dir"/* "$output/"
: > "$output/commands.log"
# Save exact argument boundaries, not a reconstructed command for evaluation.
run() { printf '%q ' "$@" >> "$output/commands.log"; printf '\n' >> "$output/commands.log"; "$@"; }
shasum -a 256 "$feng" "$plugin" "$tool_root/include/feng_runtime.h" \
    "$tool_root/lib/$platform/libfeng_runtime.a" "$sdk/bin/clang" > "$output/tool-identities.txt"
git -C "$repo" rev-parse HEAD > "$output/revision.txt"
git -C "$repo" diff > "$output/changes.patch"
uname -a > "$output/host.txt"
run "$sdk/bin/clang" -std=c11 -O2 -Wall -Wextra -Werror -c "$source_dir/support.c" -o "$output/support.o"
run "$sdk/bin/llvm-ar" rcs "$output/libarc_bench_support.a" "$output/support.o"
for level in 0 2 3; do
    release=(); if ((level > 0)); then release=(--release); fi
    for layout in single split; do
        work="$output/O$level-$layout"
        mkdir -p "$work/provider/src"
        cp "$source_dir/provider.ff" "$work/provider/src/"
        inputs=("$source_dir/kernel.ff")
        if [[ $layout == single ]]; then inputs+=("$source_dir/provider.ff")
        else
            cat > "$work/provider/feng.fm" <<'EOF'
[package]
name: "arc_bench_provider"
version: "0.1.0"
target: "lib"
src: "src/"
out: "build/"
EOF
            run env FENG_CC="$sdk/bin/clang" FENG_CC_FLAGS="-O$level -save-temps=obj -fstack-usage" \
                "$feng" build "$work/provider" --keep-ir ${release[@]+"${release[@]}"} \
                > "$work/provider-build.log" 2>&1 || { cat "$work/provider-build.log" >&2; exit 1; }
            mkdir -p "$work/bundle/lib/$platform"
            cp -R "$work/provider/build/$platform/mod" "$work/bundle/"
            cp "$work/provider/build/$platform/lib/libarc_bench_provider.a" "$work/bundle/lib/$platform/"
            printf '[package]\nname: "arc_bench_provider"\nversion: "0.1.0"\nplatform: "%s"\nabi: "feng"\n' \
                "$platform" > "$work/bundle/feng.fm"
            (cd "$work/bundle" && zip -q -r "$work/provider.fb" feng.fm mod lib)
            inputs+=("--pkg=$work/provider.fb")
        fi
        run env FENG_CC="$sdk/bin/clang" FENG_CC_FLAGS="-O$level -save-temps=obj -fstack-usage" \
            "$feng" "${inputs[@]}" --out="$work/consumer" --name=arc_bench --keep-ir \
            ${release[@]+"${release[@]}"} --lib="$output/libarc_bench_support.a" \
            > "$work/consumer-build.log" 2>&1 || { cat "$work/consumer-build.log" >&2; exit 1; }
        for mode in {0..10}; do
            ARC_COUNT=1001 ARC_MODE="$mode" "$work/consumer/bin/arc_bench" > "$work/check-$mode.tsv"
        done
    done
done
find "$output" -name '*.bc' -print | while IFS= read -r bitcode; do
    run "$sdk/bin/llvm-dis" "$bitcode" -o "${bitcode%.bc}.ll"
    run "$sdk/bin/opt" -load-pass-plugin="$plugin" -passes='c-eh-lowering,verify' \
        -S "$bitcode" -o "${bitcode%.bc}.lowered.ll"
done
echo "ARC O0/O2/O3 source and binary-package kernels verified: $output"
