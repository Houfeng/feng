#!/usr/bin/env bash
# Check emitted type graphs, input immutability and both native/LTO object paths.
set -euo pipefail
[[ $# -eq 4 ]] || { echo 'usage: retained-types.sh DSYMUTIL SDK LLD OUTPUT' >&2; exit 1; }
tool=$1
sdk=$2
linker=$3
output=$4
fixture="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/erased-types.c"
sysroot=$(xcrun --sdk macosx --show-sdk-path)
mkdir -p "$output"
for mode in off thin; do
    work="$output/$mode"
    mkdir -p "$work/lto"
    flags=(-g -O0 -fno-eliminate-unused-debug-types -isysroot "$sysroot")
    if [[ $mode == thin ]]; then flags+=(-flto=thin); fi
    "$sdk/bin/clang" "${flags[@]}" -DPROVIDER -c "$fixture" -o "$work/provider.o"
    "$sdk/bin/llvm-ar" rcs "$work/provider.a" "$work/provider.o"
    "$sdk/bin/clang" "${flags[@]}" -c "$fixture" -o "$work/consumer.o"
    "$sdk/bin/clang" "${flags[@]}" "--ld-path=$linker" \
        -Xlinker -object_path_lto -Xlinker "$work/lto" \
        "$work/consumer.o" "$work/provider.a" -o "$work/fixture"
    before=$(shasum -a 256 "$work/fixture")
    "$tool" "$work/fixture" -o "$work/fixture.dSYM"
    [[ $(shasum -a 256 "$work/fixture") == "$before" ]] || {
        echo 'dsymutil changed executable bytes' >&2; exit 1;
    }
    printf '%s\n' "$before" > "$work/binary.sha256"
    "$sdk/bin/llvm-dwarfdump" --verify "$work/fixture.dSYM" > "$work/verify.log"
    "$sdk/bin/llvm-dwarfdump" --debug-info "$work/fixture.dSYM" > "$work/dwarf.txt"
    for type in LocalRecord LocalPayload RemoteRecord RemoteTag RemoteValue; do
        grep -Eq "DW_AT_name[[:space:]]+\(\"$type\"\)" "$work/dwarf.txt" || {
            echo "missing emitted type: $mode $type" >&2; exit 1;
        }
    done
    "$work/fixture"
done
echo 'dsymutil: erased local/archive type graphs, off/thin, valid DWARF and unchanged executable bytes passed'
