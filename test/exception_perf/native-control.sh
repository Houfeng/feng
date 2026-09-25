#!/usr/bin/env bash
# Isolate protocol lowering from a language's resource management and exception runtime.
set -euo pipefail
if [[ $# != 3 ]]; then echo 'usage: native-control.sh LLVM_ROOT PLUGIN OUTPUT_DIR' >&2; exit 2; fi
sdk=$(cd -- "$1" && pwd)
plugin=$(cd -- "$(dirname -- "$2")" && pwd)/$(basename -- "$2")
mkdir -p -- "$3"
output=$(cd -- "$3" && pwd)
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$source_dir/../.." && pwd)
[[ $("$sdk/bin/llvm-config" --version) == 22.1.8 ]] || exit 2
link_flags=()
if [[ $(uname -s) == Darwin ]]; then
    mkdir -p "$output/tools"
    ln -sf "$repo/toolchain/llvm/macos-arm64/bin/lld" "$output/tools/ld64.lld"
    link_flags=("--ld-path=$output/tools/ld64.lld")
fi
cp "$source_dir"/native-control.{c,cpp,sh} "$output/"
: > "$output/commands.log"
# Log compiler arguments without relying on shell evaluation for replay.
run() {
    printf '%q ' "$@" >> "$output/commands.log"
    printf '\n' >> "$output/commands.log"
    "$@"
}
hash=(shasum -a 256)
if command -v sha256sum >/dev/null 2>&1; then hash=(sha256sum); fi
"${hash[@]}" "$sdk/bin/clang" "$plugin" > "$output/tool-identities.txt"
"$sdk/bin/clang" --version > "$output/compiler.txt"
uname -a > "$output/host.txt"
for level in 0 2 3; do
    work="$output/O$level"
    mkdir -p "$work"
    run "$sdk/bin/clang++" -std=c++17 -fexceptions -O"$level" -Wall -Wextra -Werror \
        -c "$source_dir/native-control.cpp" -o "$work/support.o"
    for protected in 0 1; do
        for language in c cpp; do
            flags=(-fexceptions -O"$level" -DPROTECTED="$protected" -Wall -Wextra -Werror)
            if [[ $language == c ]]; then
                compiler="$sdk/bin/clang"
                flags+=(-std=gnu11 "-fpass-plugin=$plugin" "-I$repo/third_party/llvm-c-eh/include")
            else
                compiler="$sdk/bin/clang++"
                flags+=(-std=c++17 -DKERNEL)
            fi
            stem="$work/$language-$protected"
            run "$compiler" "${flags[@]}" -c "$source_dir/native-control.$language" -o "$stem.o"
            run "$compiler" "${flags[@]}" -S "$source_dir/native-control.$language" -o "$stem.s"
            run "$compiler" "${flags[@]}" -S -emit-llvm "$source_dir/native-control.$language" -o "$stem.ll"
            run "$sdk/bin/clang++" "$stem.o" "$work/support.o" \
                ${link_flags[@]+"${link_flags[@]}"} -o "$stem"
            "$stem" 0 1001 > "$stem.check-normal.tsv"
            if ((protected)); then "$stem" 1 1001 > "$stem.check-throw.tsv"; fi
        done
    done
done
find "$output" -type f ! -name artifact-identities.txt ! -name samples.tsv -print0 | \
    xargs -0 "${hash[@]}" > "$output/artifact-identities.txt"
echo 'native EH controls: shared C++ producer/runtime, O0/O2/O3 and both paths passed'
if [[ ${EH_BUILD_ONLY:-0} == 1 ]]; then exit 0; fi
printf 'round\tlevel\tprotected\tlanguage\tthrow\tcount\tnanoseconds\tchecksum\n' > "$output/samples.tsv"
for ((round=0; round<=${EH_ROUNDS:-7}; ++round)); do
    languages=(c cpp)
    if ((round % 2)); then languages=(cpp c); fi
    for level in 0 2 3; do
        for failure in 0 1; do
            count=20000000
            if ((failure)); then count=200000; fi
            for protected in 0 1; do
                if ((failure && !protected)); then continue; fi
                for language in "${languages[@]}"; do
                    line=$("$output/O$level/$language-$protected" "$failure" "$count")
                    if ((round)); then
                        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$round" "$level" \
                            "$protected" "$language" "$failure" "$count" "$line" >> "$output/samples.tsv"
                    fi
                done
            done
        done
    done
done
