#!/usr/bin/env bash
# Manual, pinned, native-host LLD build shared by tests and releases.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vendor="$repo/third_party/lld"
source "$vendor/source.env"
sdk=
archive=
platform=

# Explain the optional SDK and offline source overrides.
usage() {
    echo 'usage: scripts/build_lld.sh [--platform=HOST] [--llvm-root=PATH] [--source-archive=PATH]'
    echo 'HOST: macos-arm64, linux-arm64-gnu or linux-x64-gnu; omitted means the current host.'
}

# Fail without installing an unverified artifact.
die() { echo "error: $*" >&2; exit 1; }

# Record identities independent of download transport and local timestamps.
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d ' ' -f 1
    else shasum -a 256 "$1" | cut -d ' ' -f 1; fi
}

# Discover an already installed complete SDK without changing the machine.
find_sdk() {
    local candidate
    for candidate in "$(command -v llvm-config || true)" "$(command -v llvm-config-23 || true)" \
        /opt/homebrew/opt/llvm@23/bin/llvm-config /usr/lib/llvm-23/bin/llvm-config; do
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
    local binary=$1 dependencies commands version library
    case "$host:$(file -b "$binary")" in
        macos-arm64:*'Mach-O 64-bit executable arm64'*) ;;
        linux-arm64-gnu:*'ELF 64-bit'*'ARM aarch64'*) ;;
        linux-x64-gnu:*'ELF 64-bit'*'x86-64'*) ;;
        *) die "linker has the wrong format for $host";;
    esac
    version=$("$binary" -flavor darwin --version)
    case "$version" in
        "Feng patch $LLD_PATCH_REVISION LLD $LLD_VERSION"|"Feng patch $LLD_PATCH_REVISION LLD $LLD_VERSION "*) ;;
        *) die 'executable version does not match the approved patch';;
    esac
    if [[ $host == macos-arm64 ]]; then
        dependencies=$(otool -L "$binary")
        if printf '%s\n' "$dependencies" | tail -n +2 | grep -Evq '^[[:space:]]+/(usr/lib/|System/Library/)'; then
            die 'linker retains a non-system dynamic dependency'
        fi
        commands=$(otool -l "$binary")
        ! grep -q LC_RPATH <<< "$commands" || die 'linker retains an rpath'
        codesign --verify --strict "$binary" || die 'invalid code signature'
    else
        # Keep the existing distribution's glibc floor and private library set.
        while IFS= read -r version; do
            [[ $(printf '%s\n' 2.34 "$version" | sort -V | tail -1) == 2.34 ]] ||
                die "linker requires glibc $version; use an SDK built for glibc 2.34 or older"
        done < <("$sdk/bin/llvm-readelf" --version-info "$binary" | sed -n 's/.*Name: GLIBC_\([0-9.]*\).*/\1/p')
        dependencies=$("$sdk/bin/llvm-readelf" --dynamic "$binary")
        while IFS= read -r library; do
            case "$library" in
                libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|ld-linux-*.so.*) ;;
                *) [[ -f "$bundled/lib/$library" ]] || die "dependency is not in the distribution: $library";;
            esac
        done < <(sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' <<< "$dependencies")
        ! grep -E '(RPATH|RUNPATH)' <<< "$dependencies" | grep -vF '[$ORIGIN/../lib]' ||
            die 'linker contains a non-relocatable library path'
    fi
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
                -syslibroot "$macho_sysroot" -lSystem "$stem.o" -o "$stem"
            address=$("$sdk/bin/llvm-nm" "$stem" | awk '$NF == "_prefixed" {print $1}')
            "$sdk/bin/llvm-dwarfdump" --eh-frame "$stem" > "$stem.frames"
            frame=$(sed -n 's/.* FDE .* pc=\([0-9a-fA-F]*\)\.\.\..*/\1/p' "$stem.frames")
            [[ $address =~ ^[0-9a-fA-F]+$ && $frame =~ ^[0-9a-fA-F]+$ ]] || die 'missing or ambiguous FDE'
            ((16#$address == 16#$frame)) || die "wrong FDE entry: $arch prefix $bytes"
        done
    done
    if [[ $host == macos-arm64 ]]; then
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
    fi
    echo 'LLD: exact FDE relocation and applicable native C++ exception configurations passed'
}

# Exercise archive providers and consumers independently, with real unwinding.
run_lto_regression() {
    local linker=$1 output=$2 mode level provider consumer program
    local flags=() provider_flags=() consumer_flags=()
    mkdir -p "$output"
    for mode in plain ubsan; do
        flags=()
        if [[ $host == macos-arm64 ]]; then
            flags+=(-isysroot "$macos_sdk")
        fi
        if [[ $mode == ubsan ]]; then flags+=(-fsanitize=undefined -fno-sanitize-recover=all); fi
        for level in 0 2 3; do
            for provider in off thin; do
                provider_flags=()
                if [[ $provider == thin ]]; then provider_flags=(-flto=thin); fi
                "$sdk/bin/clang++" -std=c++17 -Wall -Wextra -Werror -O"$level" \
                    ${flags[@]+"${flags[@]}"} ${provider_flags[@]+"${provider_flags[@]}"} \
                    -DPROVIDER -c "$vendor/test/native-lto.cpp" -o "$output/provider.o"
                rm -f "$output/provider.a"
                "$sdk/bin/llvm-ar" rcs "$output/provider.a" "$output/provider.o"
                for consumer in off thin; do
                    consumer_flags=()
                    if [[ $consumer == thin ]]; then consumer_flags=(-flto=thin); fi
                    program="$output/$mode-O$level-provider-$provider-consumer-$consumer"
                    "$sdk/bin/clang++" -std=c++17 -Wall -Wextra -Werror -O"$level" \
                        ${flags[@]+"${flags[@]}"} ${consumer_flags[@]+"${consumer_flags[@]}"} \
                        "--ld-path=$linker" "$vendor/test/native-lto.cpp" "$output/provider.a" -o "$program"
                    "$program"
                done
            done
        done
    done
    echo 'LLD: 24 native/ThinLTO archive, cleanup, catch, rethrow and indirect-call configurations passed'
}

for option in "$@"; do
    case "$option" in
        --platform=*) [[ -z "$platform" ]] || die 'platform specified twice'; platform=${option#*=}; [[ -n "$platform" ]] || die 'empty platform';;
        --llvm-root=*) [[ -z "$sdk" ]] || die 'SDK specified twice'; sdk=${option#*=}; [[ -n "$sdk" ]] || die 'empty SDK';;
        --source-archive=*) [[ -z "$archive" ]] || die 'archive specified twice'; archive=${option#*=}; [[ -n "$archive" ]] || die 'empty archive';;
        --help|-h) usage; exit 0;;
        *) die "unknown option: $option";;
    esac
done
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64) host=macos-arm64;;
    Linux/aarch64|Linux/arm64) host=linux-arm64-gnu;;
    Linux/x86_64|Linux/amd64) host=linux-x64-gnu;;
    *) die 'unsupported build host';;
esac

# Build and validate only the current native host.
platform=${platform:-$host}
case "$platform" in macos-arm64|linux-arm64-gnu|linux-x64-gnu) ;; *) die "unsupported platform: $platform";; esac
[[ $platform == "$host" ]] || die "--platform must match current host: $host"
if [[ -z "$sdk" ]]; then sdk=$(find_sdk) || die 'complete LLVM 23.1.3 SDK not found; use --llvm-root'; fi
sdk=$(cd "$sdk" && pwd)
for tool in clang clang++ llvm-config FileCheck not llvm-mc llvm-nm llvm-ar llvm-dwarfdump llvm-strip llvm-readelf; do
    [[ -x "$sdk/bin/$tool" ]] || die "SDK lacks $tool"
done
[[ $("$sdk/bin/llvm-config" --version) == "$LLD_VERSION" ]] || die 'SDK version must be 23.1.3'
for tool in clang clang++; do
    case $("$sdk/bin/$tool" --version | head -1) in
        *"clang version $LLD_VERSION"|*"clang version $LLD_VERSION "*) ;;
        *) die "$tool version must be 23.1.3";;
    esac
done
for tool in cmake ninja python3 curl patch tar file; do command -v "$tool" >/dev/null || die "missing $tool"; done
bundled="$repo/toolchain/llvm/$host"
original="$bundled/bin/lld"
[[ -x "$original" ]] || die 'bundled LLD is required to bootstrap the patched linker'
bootstrap_version=$("$original" -flavor darwin --version)
# A previous patch revision of the same LLVM may bootstrap its replacement.
bootstrap_version=$(sed -E 's/^Feng patch [0-9]+ //' <<< "$bootstrap_version")
case "$bootstrap_version" in
    "LLD $LLD_VERSION"|"LLD $LLD_VERSION "*) ;;
    *) die 'bootstrap LLD version must match source.env';;
esac
work="$repo/build/lld/$host"
mkdir -p "$work"
mkdir "$work/lock" 2>/dev/null || die "another maintenance run holds $work/lock"
trap 'rmdir "$work/lock"' EXIT
if [[ -z "$archive" ]]; then
    mkdir -p "$repo/local/lld"
    archive="$repo/local/lld/llvm-$LLD_VERSION.tar.xz"
    if [[ ! -f "$archive" ]]; then
        curl -fL --retry 3 --connect-timeout 15 --max-time 900 "$LLD_SOURCE_URL" -o "$archive.download"
        [[ $(sha256 "$archive.download") == "$LLD_SOURCE_SHA256" ]] || die 'download checksum mismatch'
        mv "$archive.download" "$archive"
    fi
fi
[[ -f "$archive" && $(sha256 "$archive") == "$LLD_SOURCE_SHA256" ]] || die 'source archive checksum mismatch'

# Re-extract clean source on every explicit maintenance run; never patch an SDK.
rm -rf "$work/source" "$work/lit" "$work/build" "$work/staging"
mkdir -p "$work/source" "$work/lit" "$work/tools" "$work/staging/bin" "$work/staging/LICENSES"
tar -xf "$archive" -C "$work/source" --strip-components=1 \
    "$LLD_SOURCE_PREFIX/lld" "$LLD_SOURCE_PREFIX/cmake" "$LLD_SOURCE_PREFIX/llvm/cmake" \
    "$LLD_SOURCE_PREFIX/llvm/utils/dtlto"
tar -xf "$archive" -C "$work/lit" --strip-components=4 "$LLD_SOURCE_PREFIX/llvm/utils/lit"
for patch_file in "$vendor"/patches/*.patch; do patch -d "$work/source" -p1 < "$patch_file"; done
cp "$vendor/test/lto-unwind-owner.ll" "$work/source/lld/test/MachO/feng-lto-unwind-owner.ll"
cmake_options=()
if [[ $host == macos-arm64 ]]; then
    for tool in codesign otool xcrun; do command -v "$tool" >/dev/null || die "missing $tool"; done
    ln -sf "$original" "$work/tools/ld64.lld"
    macos_sdk=$(xcrun --sdk macosx --show-sdk-path)
    macho_sysroot="$macos_sdk"
    cmake_options+=("-DCMAKE_EXE_LINKER_FLAGS=--ld-path=$work/tools/ld64.lld -Wl,-dead_strip_dylibs -L$macos_sdk/usr/lib"
        "-DCMAKE_OSX_SYSROOT=$macos_sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_SKIP_RPATH=ON)
else
    ln -sf "$original" "$work/tools/ld.lld"
    macho_sysroot="$work/source/lld/test/MachO/Inputs/MacOSX.sdk"
    cmake_options+=("-DCMAKE_EXE_LINKER_FLAGS=--ld-path=$work/tools/ld.lld -Wl,--as-needed"
        '-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib' -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON)
fi
cmake -S "$work/source/lld" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$sdk/bin/clang" -DCMAKE_CXX_COMPILER="$sdk/bin/clang++" \
    -DLLVM_DIR="$vendor/cmake" -DTEST_LLD_LLVM_CMAKE_DIR="$sdk/lib/cmake/llvm" \
    "${cmake_options[@]}" \
    -DLLVM_INCLUDE_TESTS=ON -DLLVM_EXTERNAL_LIT="$work/lit/lit.py" \
    -DLLD_VENDOR="Feng patch $LLD_PATCH_REVISION"
cmake --build "$work/build" --target lld --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-8}"
python3 "$work/lit/lit.py" -sv "$work/build/test/MachO" 2>&1 | tee "$work/macho-tests.log"
if [[ $host == linux-* ]]; then
    python3 "$work/lit/lit.py" -sv "$work/build/test/ELF" 2>&1 | tee "$work/elf-tests.log"
fi
run_regression "$work/build/bin/ld64.lld" "$work/validation"
native_flavor=ld.lld
if [[ $host == macos-arm64 ]]; then native_flavor=ld64.lld; fi
run_lto_regression "$work/build/bin/$native_flavor" "$work/lto-validation"

# Publish only a self-contained executable and its required licenses.
install -m 755 "$work/build/bin/lld" "$work/staging/bin/lld"
"$sdk/bin/llvm-strip" --strip-all "$work/staging/bin/lld"
if [[ $host == macos-arm64 ]]; then codesign --force --sign - "$work/staging/bin/lld"
else ln -s "$bundled/lib" "$work/staging/lib"; fi
ln -s lld "$work/staging/bin/ld.lld"
ln -s lld "$work/staging/bin/ld64.lld"
cp "$vendor/LICENSE.TXT" "$work/staging/LICENSES/LLVM.txt"
[[ -f "$work/build/test-lld-zstd.txt" ]] || die 'SDK must provide the static zstd dependency and its license'
zstd_archive=$(resolve_file "$(cat "$work/build/test-lld-zstd.txt")")
zstd_license="$(dirname "$(dirname "$zstd_archive")")/LICENSE"
[[ -f "$zstd_license" ]] || die "dependency license missing: $zstd_license"
cp "$zstd_license" "$work/staging/LICENSES/Zstandard.txt"
# Keep maintenance reports outside the installed tool directory.
{
    printf 'llvm_version=%s\nhost=%s\npatch_revision=%s\n' "$LLD_VERSION" "$host" "$LLD_PATCH_REVISION"
    printf 'source_url=%s\nsource_sha256=%s\n' "$LLD_SOURCE_URL" "$LLD_SOURCE_SHA256"
    for patch_file in "$vendor"/patches/*.patch; do printf 'patch=%s sha256=%s\n' "$(basename "$patch_file")" "$(sha256 "$patch_file")"; done
    printf 'build_script_sha256=%s\n' "$(sha256 "$0")"
    printf 'build_mode=Release\nllvm_linkage=static\n'
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
if [[ $host == linux-* ]]; then
    # Relocation must use copied private libraries, not the original symlink.
    rm "$work/relocated tool/lib"
    cp -R "$bundled/lib" "$work/relocated tool/lib"
fi
verify_binary "$work/relocated tool/bin/lld"
run_regression "$work/relocated tool/bin/ld64.lld" "$work/relocated-validation"
run_lto_regression "$work/relocated tool/bin/$native_flavor" "$work/relocated-lto-validation"
# Replace only the linker, preserving the other tools and existing private libraries.
install -m 755 "$work/staging/bin/lld" "$bundled/bin/lld.new"
mv -f "$bundled/bin/lld.new" "$bundled/bin/lld"
ln -sf lld "$bundled/bin/ld.lld"
ln -sf lld "$bundled/bin/ld64.lld"
mkdir -p "$bundled/LICENSES"
cp "$work/staging/LICENSES/Zstandard.txt" "$bundled/LICENSES/Zstandard.txt"
echo "Validated shared linker installed: $bundled/bin/lld"
