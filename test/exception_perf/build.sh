#!/usr/bin/env bash
# Build matching real-driver Feng and C++ controls; keep all intermediate artifacts.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "usage: $0 FENG LLVM_ROOT OUTPUT_DIR" >&2; exit 2
fi
feng=$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
sdk=$(cd -- "$2" && pwd)
mkdir -p -- "$3"
output=$(cd -- "$3" && pwd)
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$source_dir/../.." && pwd)
[[ $("$sdk/bin/llvm-config" --version) == 22.1.8 ]] || exit 2
link_flags=()
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) platform=macos-arm64;;
    Linux-aarch64) platform=linux-arm64-gnu;;
    Linux-x86_64) platform=linux-x64-gnu;;
    *) echo 'unsupported benchmark host' >&2; exit 2;;
esac
if [[ $(uname -s) == Darwin ]]; then
    mkdir -p "$output/tools"
    ln -sf "$repo/toolchain/llvm/macos-arm64/bin/lld" "$output/tools/ld64.lld"
    link_flags=("--ld-path=$output/tools/ld64.lld")
    export COMPILER_PATH="$output/tools${COMPILER_PATH:+:$COMPILER_PATH}"
fi
cp "$source_dir"/*.{c,cpp,ff,sh,js} "$source_dir/README.md" "$output/"
: > "$output/commands.log"
# Keep exact argument boundaries without executing a reconstructed shell string.
run() {
    printf '%q ' "$@" >> "$output/commands.log"
    printf '\n' >> "$output/commands.log"
    "$@"
}
tool_root=$(cd -- "$(dirname -- "$feng")/.." && pwd)
plugin="$tool_root/toolchain/llvm-c-eh/lib/llvm_c_eh.so"
if [[ $platform == macos-arm64 ]]; then plugin=${plugin%.so}.dylib; fi
hash=(shasum -a 256)
if command -v sha256sum >/dev/null 2>&1; then hash=(sha256sum); fi
"${hash[@]}" "$feng" "$plugin" "$tool_root/include/feng_runtime.h" \
    "$tool_root/lib/$platform/libfeng_runtime.a" "$sdk/bin/clang" \
    "$repo/toolchain/llvm/$platform/bin/lld" > "$output/tool-identities.txt"
uname -a > "$output/host.txt"
"$sdk/bin/clang" --version > "$output/compiler.txt"
git -C "$repo" rev-parse HEAD > "$output/revision.txt"
git -C "$repo" diff > "$output/changes.patch"
run "$sdk/bin/clang" -std=c11 -O2 -Wall -Wextra -Werror -c "$source_dir/support.c" -o "$output/support.o"
run "$sdk/bin/llvm-ar" rcs "$output/libeh_bench_support.a" "$output/support.o"

for level in 0 2 3; do
    mode_flags=()
    if ((level > 0)); then mode_flags=(--release); fi
    for layout in single split; do
        work="$output/O$level-$layout"
        mkdir -p "$work/provider/src" "$work/consumer/src"
        cp "$source_dir/callee.ff" "$work/provider/src/"
        cp "$source_dir/kernel.ff" "$work/consumer/src/"
        feng_inputs=("$work/consumer/src/kernel.ff")
        if [[ "$layout" == single ]]; then
            feng_inputs+=("$work/provider/src/callee.ff")
        else
            cat > "$work/provider/feng.fm" <<'EOF'
[package]
name: "eh_bench_provider"
version: "0.1.0"
target: "lib"
src: "src/"
out: "build/"
EOF
            run env FENG_CC="$sdk/bin/clang" FENG_CC_FLAGS="-O$level -save-temps=obj" \
                "$feng" build "$work/provider" --keep-ir ${mode_flags[@]+"${mode_flags[@]}"} \
                > "$work/provider-build.log" 2>&1 || {
                    cat "$work/provider-build.log" >&2; exit 1;
                }
            # pack always rebuilds release; package these exact measured objects
            # directly so O0's producer is not silently replaced with release.
            mkdir -p "$work/bundle/lib/$platform"
            cp -R "$work/provider/build/$platform/mod" "$work/bundle/"
            cp "$work/provider/build/$platform/lib/libeh_bench_provider.a" "$work/bundle/lib/$platform/"
            printf '[package]\nname: "eh_bench_provider"\nversion: "0.1.0"\nplatform: "%s"\nabi: "feng"\n' \
                "$platform" > "$work/bundle/feng.fm"
            (cd "$work/bundle" && zip -q -r "$work/provider.fb" feng.fm mod lib)
            feng_inputs+=("--pkg=$work/provider.fb")
        fi
        # The existing override chooses the same complete compiler as the C++ side.
        # The shared support object supplies only inputs, timing and the oracle.
        run env FENG_CC="$sdk/bin/clang" FENG_CC_FLAGS="-O$level -save-temps=obj" \
            "$feng" "${feng_inputs[@]}" --out="$work/consumer/build" --name=eh_bench \
            --keep-ir ${mode_flags[@]+"${mode_flags[@]}"} --lib="$output/libeh_bench_support.a" \
            > "$work/feng-build.log" 2>&1 || {
                cat "$work/feng-build.log" >&2; exit 1;
            }
        cpp_flags=()
        if [[ "$layout" == single ]]; then
            cpp_flags=(-DEH_SINGLE_TU)
        else
            run "$sdk/bin/clang++" -std=c++17 -fexceptions -O"$level" -g -save-temps=obj \
                -c "$source_dir/callee.cpp" -o "$work/callee.o"
            cpp_flags=("$work/callee.o")
        fi
        run "$sdk/bin/clang++" -std=c++17 -fexceptions -O"$level" -g -save-temps=obj \
            "$source_dir/kernel.cpp" "${cpp_flags[@]}" "$output/support.o" \
            ${link_flags[@]+"${link_flags[@]}"} -o "$work/cpp"
        for mode in 0 1 2 3 4 5 6 7 8; do
            EH_COUNT=1001 EH_MODE="$mode" "$work/consumer/build/bin/eh_bench" > "$work/check-$mode-feng.tsv"
            EH_COUNT=1001 EH_MODE="$mode" "$work/cpp" > "$work/check-$mode-cpp.tsv"
        done
    done
done
find "$output" -name '*.bc' -print | while IFS= read -r bitcode; do
    run "$sdk/bin/llvm-dis" "$bitcode" -o "${bitcode%.bc}.ll"
    # Clang's saved bitcode precedes plugin lowering; distinguish this inspection
    # output from both that input and the actual compiler-produced assembly.
    run "$sdk/bin/opt" -load-pass-plugin="$plugin" -passes='c-eh-lowering,verify' \
        -S "$bitcode" -o "${bitcode%.bc}.lowered.ll"
done
find "$output" -type f ! -name artifact-identities.txt -print0 | \
    xargs -0 "${hash[@]}" > "$output/artifact-identities.txt"
echo "Verified Feng/C++ kernels and saved sources, build logs, bitcode, IR and assembly: $output"
