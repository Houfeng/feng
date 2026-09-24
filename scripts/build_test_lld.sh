#!/usr/bin/env bash
# Manual, pinned, standalone LLD build for macOS UBSan; never called by normal CI.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vendor="$repo/third_party/lld"
source "$vendor/source.env"
sdk=
archive=

# Explain the optional SDK and offline source overrides.
usage() { echo 'usage: scripts/build_test_lld.sh [--llvm-root=PATH] [--source-archive=PATH]'; }

# Fail without installing an unverified artifact.
die() { echo "error: $*" >&2; exit 1; }

# Record identities independent of download transport and local timestamps.
sha256() { shasum -a 256 "$1" | cut -d ' ' -f 1; }

# Discover an already installed complete SDK without changing the machine.
find_sdk() {
    local candidate
    for candidate in "$(command -v llvm-config || true)" /opt/homebrew/opt/llvm@22/bin/llvm-config; do
        [[ -x "$candidate" ]] || continue
        if [[ $("$candidate" --version) == "$LLD_VERSION" ]]; then "$candidate" --prefix; return; fi
    done
    return 1
}

# Follow an installed SDK dependency symlink to its license-bearing prefix.
resolve_file() {
    local path=$1 target depth=0
    while [[ -L "$path" ]]; do
        depth=$((depth + 1)); [[ $depth -le 32 ]] || die 'dependency symlink loop'
        target=$(readlink "$path")
        if [[ $target == /* ]]; then path=$target; else path="$(dirname "$path")/$target"; fi
    done
    printf '%s/%s\n' "$(cd "$(dirname "$path")" && pwd -P)" "$(basename "$path")"
}

# Check the final executable before it replaces an existing prebuild.
verify_binary() {
    local binary=$1 dependencies commands
    [[ $(file -b "$binary") == *'Mach-O 64-bit executable arm64'* ]] || die 'not an ARM64 Mach-O executable'
    [[ $("$binary" -flavor darwin --version) == "Feng UBSan test tools patch $LLD_PATCH_REVISION LLD $LLD_VERSION" ]] ||
        die 'executable version does not match the approved patch'
    dependencies=$(otool -L "$binary")
    if printf '%s\n' "$dependencies" | tail -n +2 | grep -Evq '^[[:space:]]+/(usr/lib/|System/Library/)'; then
        die 'linker retains a non-system dynamic dependency'
    fi
    commands=$(otool -l "$binary")
    ! grep -q LC_RPATH <<< "$commands" || die 'linker retains an rpath'
    codesign --verify --strict "$binary" || die 'invalid code signature'
}

# Verify FDE relocation and native exceptions without the Feng EH plugin.
run_regression() {
    local linker=$1 output=$2 arch bytes stem address frame mode level scenario program
    local flags=()
    mkdir -p "$output"
    for arch in arm64 x86_64; do
        for bytes in 0 4 8 16 32; do
            stem="$output/$arch-$bytes"
            "$sdk/bin/llvm-mc" -filetype=obj -triple="$arch-apple-macos14.0" \
                -defsym="PREFIX_BYTES=$bytes" "$vendor/test/prefixed-entry.s" -o "$stem.o"
            "$linker" -arch "$arch" -platform_version macos 14.0 14.0 \
                -syslibroot "$macos_sdk" -lSystem "$stem.o" -o "$stem"
            address=$("$sdk/bin/llvm-nm" "$stem" | awk '$NF == "_prefixed" {print $1}')
            "$sdk/bin/llvm-dwarfdump" --eh-frame "$stem" > "$stem.frames"
            frame=$(sed -n 's/.* FDE .* pc=\([0-9a-fA-F]*\)\.\.\..*/\1/p' "$stem.frames")
            [[ $address =~ ^[0-9a-fA-F]+$ && $frame =~ ^[0-9a-fA-F]+$ ]] || die 'missing or ambiguous FDE'
            ((16#$address == 16#$frame)) || die "wrong FDE entry: $arch prefix $bytes"
        done
    done
    for mode in plain ubsan; do
        flags=()
        if [[ $mode == ubsan ]]; then flags=(-fsanitize=undefined -fno-sanitize-recover=all); fi
        for level in 0 2 3; do
            for scenario in 1 2 3; do
                program="$output/native-$mode-O$level-$scenario"
                "$sdk/bin/clang++" -std=c++17 -g -Wall -Wextra -Werror -Wno-unused-function \
                    -O"$level" -DCASE="$scenario" -isysroot "$macos_sdk" "--ld-path=$linker" \
                    ${flags[@]+"${flags[@]}"} "$vendor/test/native.cpp" -o "$program"
                "$program"
            done
        done
    done
    echo 'test LLD: 10 exact FDE cases and 18 native C++ exception configurations passed'
}

for option in "$@"; do
    case "$option" in
        --llvm-root=*) [[ -z "$sdk" ]] || die 'SDK specified twice'; sdk=${option#*=}; [[ -n "$sdk" ]] || die 'empty SDK';;
        --source-archive=*) [[ -z "$archive" ]] || die 'archive specified twice'; archive=${option#*=}; [[ -n "$archive" ]] || die 'empty archive';;
        --help|-h) usage; exit 0;;
        *) die "unknown option: $option";;
    esac
done
[[ $(uname -s)/$(uname -m) == Darwin/arm64 ]] || die 'maintenance requires macOS ARM64'
if [[ -z "$sdk" ]]; then sdk=$(find_sdk) || die 'complete LLVM 22.1.8 SDK not found; use --llvm-root'; fi
sdk=$(cd "$sdk" && pwd)
for tool in clang clang++ llvm-config FileCheck not llvm-mc llvm-nm llvm-dwarfdump llvm-strip; do
    [[ -x "$sdk/bin/$tool" ]] || die "SDK lacks $tool"
done
[[ $("$sdk/bin/llvm-config" --version) == "$LLD_VERSION" ]] || die 'SDK version must be 22.1.8'
for tool in clang clang++; do
    case $("$sdk/bin/$tool" --version | head -1) in
        *"clang version $LLD_VERSION"|*"clang version $LLD_VERSION "*) ;;
        *) die "$tool version must be 22.1.8";;
    esac
done
for tool in cmake ninja python3 curl patch tar codesign shasum file otool xcrun; do command -v "$tool" >/dev/null || die "missing $tool"; done
original="$repo/toolchain/llvm/macos-arm64/bin/lld"
[[ -x "$original" ]] || die 'original bundled LLD is required to build the test linker'
case $("$original" -flavor darwin --version) in
    "LLD $LLD_VERSION"|"LLD $LLD_VERSION "*) ;;
    *) die 'original LLD version must be 22.1.8';;
esac
work="$repo/build/test-lld/macos-arm64"
mkdir -p "$work"
mkdir "$work/lock" 2>/dev/null || die "another maintenance run holds $work/lock"
trap 'rmdir "$work/lock"' EXIT
if [[ -z "$archive" ]]; then
    mkdir -p "$repo/temp/test-lld"
    archive="$repo/temp/test-lld/llvm-$LLD_VERSION.tar.xz"
    if [[ ! -f "$archive" ]]; then
        curl -fL --retry 3 --connect-timeout 15 --max-time 900 "$LLD_SOURCE_URL" -o "$archive.download"
        [[ $(sha256 "$archive.download") == "$LLD_SOURCE_SHA256" ]] || die 'download checksum mismatch'
        mv "$archive.download" "$archive"
    fi
fi
[[ -f "$archive" && $(sha256 "$archive") == "$LLD_SOURCE_SHA256" ]] || die 'source archive checksum mismatch'

# Re-extract clean source on every explicit maintenance run; never patch an SDK.
rm -rf "$work/source" "$work/lit" "$work/staging"
mkdir -p "$work/source" "$work/lit" "$work/tools" "$work/staging/bin" "$work/staging/LICENSES"
tar -xf "$archive" -C "$work/source" --strip-components=1 \
    "$LLD_SOURCE_PREFIX/lld" "$LLD_SOURCE_PREFIX/cmake" "$LLD_SOURCE_PREFIX/llvm/cmake"
tar -xf "$archive" -C "$work/lit" --strip-components=4 "$LLD_SOURCE_PREFIX/llvm/utils/lit"
patch -d "$work/source" -p1 < "$vendor/patches/0001-fix-macho-fde-relocation.patch"
ln -sf "$original" "$work/tools/ld64.lld"
macos_sdk=$(xcrun --sdk macosx --show-sdk-path)
cmake -S "$work/source/lld" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$sdk/bin/clang" -DCMAKE_CXX_COMPILER="$sdk/bin/clang++" \
    -DLLVM_DIR="$vendor/cmake" -DTEST_LLD_LLVM_CMAKE_DIR="$sdk/lib/cmake/llvm" \
    -DCMAKE_EXE_LINKER_FLAGS="--ld-path=$work/tools/ld64.lld" \
    -DCMAKE_OSX_SYSROOT="$macos_sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_SKIP_RPATH=ON \
    -DLLVM_INCLUDE_TESTS=ON -DLLVM_EXTERNAL_LIT="$work/lit/lit.py" \
    -DLLD_VENDOR="Feng UBSan test tools patch $LLD_PATCH_REVISION"
cmake --build "$work/build" --target lld --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-8}"
python3 "$work/lit/lit.py" -sv "$work/build/test/MachO" 2>&1 | tee "$work/macho-tests.log"
run_regression "$work/build/bin/ld64.lld" "$work/validation"

# Publish only a self-contained executable and its required licenses.
install -m 755 "$work/build/bin/lld" "$work/staging/bin/lld"
"$sdk/bin/llvm-strip" --strip-all "$work/staging/bin/lld"
codesign --force --sign - "$work/staging/bin/lld"
ln -s lld "$work/staging/bin/ld64.lld"
cp "$vendor/LICENSE.TXT" "$work/staging/LICENSES/LLVM.txt"
[[ -f "$work/build/test-lld-zstd.txt" ]] || die 'SDK must provide the static zstd dependency and its license'
zstd_archive=$(resolve_file "$(cat "$work/build/test-lld-zstd.txt")")
zstd_license="$(dirname "$(dirname "$zstd_archive")")/LICENSE"
[[ -f "$zstd_license" ]] || die "dependency license missing: $zstd_license"
cp "$zstd_license" "$work/staging/LICENSES/Zstandard.txt"
# Keep maintenance reports outside the installed tool directory.
{
    printf 'llvm_version=%s\nhost=macos-arm64\npatch_revision=%s\n' "$LLD_VERSION" "$LLD_PATCH_REVISION"
    printf 'source_url=%s\nsource_sha256=%s\n' "$LLD_SOURCE_URL" "$LLD_SOURCE_SHA256"
    printf 'patch_sha256=%s\n' "$(sha256 "$vendor/patches/0001-fix-macho-fde-relocation.patch")"
    printf 'build_script_sha256=%s\n' "$(sha256 "$0")"
    printf 'build_mode=Release\nminimum_macos=26.0\nllvm_linkage=static\n'
    printf 'compiler=%s\n' "$("$sdk/bin/clang" --version | head -1)"
    printf 'built_at_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'validation=upstream Mach-O; arbitrary prefixes ARM64/x64; native C++ UBSan O0/O2/O3; relocated executable\n'
} > "$work/build-info.txt"
(
    cd "$work/staging"
    for path in bin/lld LICENSES/LLVM.txt LICENSES/Zstandard.txt; do
        printf '%s  %s\n' "$(sha256 "$path")" "$path"
    done
) > "$work/SHA256SUMS"
verify_binary "$work/staging/bin/lld"
rm -rf "$work/relocated tool"
cp -R "$work/staging" "$work/relocated tool"
verify_binary "$work/relocated tool/bin/lld"
run_regression "$work/relocated tool/bin/ld64.lld" "$work/relocated-validation"
destination="$repo/toolchain/test_tools/lld/macos-arm64"
mkdir -p "$(dirname "$destination")"
backup=$(mktemp -d "$work/previous.XXXXXX")
rmdir "$backup"
if [[ -e "$destination" ]]; then mv "$destination" "$backup"; fi
if ! mv "$work/staging" "$destination"; then
    if [[ -e "$backup" ]]; then mv "$backup" "$destination"; fi
    die 'failed to install the validated test linker'
fi
rm -rf "$backup"
echo "Validated UBSan test linker installed: $destination"
