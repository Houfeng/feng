#!/usr/bin/env bash
# Build and validate the pinned macOS DWARF linker before replacing the prebuild.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vendor="$repo/third_party/dsymutil"
source "$vendor/source.env"
sdk=
archive=
platform=

# Report maintenance errors without changing the installed tool.
die() { echo "error: $*" >&2; exit 1; }

# Explain native-host and offline maintenance overrides.
usage() {
    echo 'usage: scripts/build_dsymutil.sh [--platform=macos-arm64] [--llvm-root=PATH] [--source-archive=PATH]'
}

# Identify source and output bytes independently of timestamps.
sha256() { shasum -a 256 "$1" | cut -d ' ' -f 1; }

# Discover an existing matching SDK without installing dependencies.
find_sdk() {
    local candidate
    for candidate in "$(command -v llvm-config || true)" \
        /opt/homebrew/opt/llvm@23/bin/llvm-config /opt/homebrew/opt/llvm/bin/llvm-config; do
        [[ -x "$candidate" ]] || continue
        if [[ $("$candidate" --version) == "$DSYMUTIL_VERSION" ]]; then
            "$candidate" --prefix
            return
        fi
    done
    return 1
}

# Resolve dependency symlinks before locating the license in their real prefix.
resolve_file() {
    local path=$1 target depth=0
    while [[ -L "$path" ]]; do
        depth=$((depth + 1)); [[ $depth -le 32 ]] || die 'dependency symlink loop'
        target=$(readlink "$path")
        if [[ $target == /* ]]; then path=$target; else path="$(dirname "$path")/$target"; fi
    done
    printf '%s/%s\n' "$(cd "$(dirname "$path")" && pwd -P)" "$(basename "$path")"
}

# Reject host-specific dependencies or an unexpected patch before installation.
verify_binary() {
    local binary=$1 dependencies commands minimum
    [[ $(file -b "$binary") == *'Mach-O 64-bit executable arm64'* ]] || die 'wrong dsymutil architecture'
    [[ $("$binary" --version | head -1) == "Feng patch $DSYMUTIL_PATCH_REVISION dsymutil $DSYMUTIL_VERSION" ]] ||
        die 'dsymutil version does not match source.env'
    dependencies=$(otool -L "$binary")
    if tail -n +2 <<< "$dependencies" | grep -Evq '^[[:space:]]+/(usr/lib/|System/Library/)'; then
        die 'dsymutil retains a non-system dynamic dependency'
    fi
    commands=$(otool -l "$binary")
    ! grep -q LC_RPATH <<< "$commands" || die 'dsymutil retains an rpath'
    minimum=$(awk '/minos / {print $2}' <<< "$commands")
    [[ $minimum == 26.0 ]] || die "unexpected macOS deployment target: $minimum"
    codesign --verify --strict "$binary" || die 'invalid dsymutil signature'
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
[[ $(uname -s)/$(uname -m) == Darwin/arm64 ]] || die 'dsymutil maintenance requires a macOS ARM64 host'
host=macos-arm64
platform=${platform:-$host}
[[ $platform == "$host" ]] || die "--platform must match current host: $host"
if [[ -z "$sdk" ]]; then sdk=$(find_sdk) || die 'matching LLVM SDK not found; use --llvm-root'; fi
sdk=$(cd "$sdk" && pwd)
for tool in clang clang++ llvm-config llvm-tblgen llvm-ar llvm-dwarfdump llvm-strip; do
    [[ -x "$sdk/bin/$tool" ]] || die "SDK lacks $tool"
done
[[ $("$sdk/bin/llvm-config" --version) == "$DSYMUTIL_VERSION" ]] || die "SDK version must be $DSYMUTIL_VERSION"
for tool in clang clang++; do
    case $("$sdk/bin/$tool" --version | head -1) in
        *"clang version $DSYMUTIL_VERSION"|*"clang version $DSYMUTIL_VERSION "*) ;;
        *) die "$tool version must be $DSYMUTIL_VERSION";;
    esac
done
for tool in cmake ninja curl patch tar file shasum codesign otool xcrun; do
    command -v "$tool" >/dev/null || die "missing $tool"
done
bundled="$repo/toolchain/llvm/$host"
[[ -x "$bundled/bin/ld64.lld" ]] || die 'bundled LLD is required'
work="$repo/build/dsymutil/$host"
mkdir -p "$work"
mkdir "$work/lock" 2>/dev/null || die "another maintenance run holds $work/lock"
trap 'rmdir "$work/lock"' EXIT
if [[ -z "$archive" ]]; then
    mkdir -p "$repo/local/dsymutil"
    archive="$repo/local/dsymutil/llvm-$DSYMUTIL_VERSION.tar.xz"
    if [[ ! -f "$archive" ]]; then
        curl -fL --retry 3 --connect-timeout 15 --max-time 900 "$DSYMUTIL_SOURCE_URL" -o "$archive.download"
        [[ $(sha256 "$archive.download") == "$DSYMUTIL_SOURCE_SHA256" ]] || die 'download checksum mismatch'
        mv "$archive.download" "$archive"
    fi
fi
[[ -f "$archive" && $(sha256 "$archive") == "$DSYMUTIL_SOURCE_SHA256" ]] || die 'source checksum mismatch'

# Start with clean upstream source and build state on every maintenance run.
rm -rf "$work/source" "$work/build" "$work/staging"
mkdir -p "$work/source" "$work/staging/bin" "$work/staging/LICENSES"
tar -xf "$archive" -C "$work/source" --strip-components=1 \
    "$DSYMUTIL_SOURCE_PREFIX/llvm/lib/DWARFLinker/Parallel" \
    "$DSYMUTIL_SOURCE_PREFIX/llvm/tools/dsymutil"
for patch_file in "$vendor"/patches/*.patch; do
    patch -d "$work/source" -p1 < "$patch_file"
done
macos_sdk=$(xcrun --sdk macosx --show-sdk-path)
cmake -S "$vendor" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$sdk/bin/clang" -DCMAKE_CXX_COMPILER="$sdk/bin/clang++" \
    -DLLVM_DIR="$sdk/lib/cmake/llvm" -DDSYMUTIL_SOURCE_ROOT="$work/source/llvm" \
    -DDSYMUTIL_PATCH_REVISION="$DSYMUTIL_PATCH_REVISION" \
    -DCMAKE_OSX_SYSROOT="$macos_sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_SKIP_RPATH=ON \
    "-DCMAKE_EXE_LINKER_FLAGS=--ld-path=$bundled/bin/ld64.lld -Wl,-dead_strip_dylibs -L$macos_sdk/usr/lib"
cmake --build "$work/build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-8}" 2>&1 | tee "$work/compile.log"
if grep -q 'newer than target minimum' "$work/compile.log"; then
    die 'an SDK dependency requires a newer macOS; use an SDK compatible with macOS 26'
fi
install -m 755 "$work/build/dsymutil" "$work/staging/bin/dsymutil"
"$sdk/bin/llvm-strip" --strip-all "$work/staging/bin/dsymutil"
codesign --force --sign - "$work/staging/bin/dsymutil"
cp "$vendor/LICENSE.TXT" "$work/staging/LICENSES/LLVM.txt"
zstd_archive=$(resolve_file "$(cat "$work/build/zstd-library.txt")")
zstd_license="$(dirname "$(dirname "$zstd_archive")")/LICENSE"
[[ -f "$zstd_license" ]] || die "dependency license missing: $zstd_license"
cp "$zstd_license" "$work/staging/LICENSES/Zstandard.txt"
verify_binary "$work/staging/bin/dsymutil"
bash "$vendor/test/retained-types.sh" "$work/staging/bin/dsymutil" "$sdk" \
    "$bundled/bin/ld64.lld" "$work/validation" 2>&1 | tee "$work/validation.log"

# Repeat behavior and dependency checks after moving to a path containing spaces.
rm -rf "$work/relocated tool"
cp -R "$work/staging" "$work/relocated tool"
verify_binary "$work/relocated tool/bin/dsymutil"
bash "$vendor/test/retained-types.sh" "$work/relocated tool/bin/dsymutil" "$sdk" \
    "$bundled/bin/ld64.lld" "$work/relocated validation" 2>&1 | tee "$work/relocated-validation.log"
{
    printf 'llvm_version=%s\nhost=%s\npatch_revision=%s\n' "$DSYMUTIL_VERSION" "$host" "$DSYMUTIL_PATCH_REVISION"
    printf 'source_url=%s\nsource_sha256=%s\n' "$DSYMUTIL_SOURCE_URL" "$DSYMUTIL_SOURCE_SHA256"
    for patch_file in "$vendor"/patches/*.patch; do
        printf 'patch=%s sha256=%s\n' "$(basename "$patch_file")" "$(sha256 "$patch_file")"
    done
    printf 'build_script_sha256=%s\ncmake_sha256=%s\n' "$(sha256 "$0")" "$(sha256 "$vendor/CMakeLists.txt")"
    printf 'built_at_utc=%s\nvalidation=DWARF; off/thin; binary identity; relocation\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$work/build-info.txt"
(
    cd "$work/staging"
    shasum -a 256 bin/dsymutil LICENSES/LLVM.txt LICENSES/Zstandard.txt
) > "$work/SHA256SUMS"
install -m 755 "$work/staging/bin/dsymutil" "$bundled/bin/dsymutil.new"
mv -f "$bundled/bin/dsymutil.new" "$bundled/bin/dsymutil"
mkdir -p "$bundled/LICENSES"
cp "$work/staging/LICENSES/Zstandard.txt" "$bundled/LICENSES/Zstandard.txt"
echo "Validated DWARF linker installed: $bundled/bin/dsymutil"
