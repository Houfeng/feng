#!/usr/bin/env bash
# Manual maintainer entry: build and validate the current native host.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/.." && pwd)
sdk=
platform=
link_options=()

# Keep ordinary builds out of this manual host maintenance path.
usage() {
    echo 'usage: scripts/build_llvm_native_symbols.sh [--platform=HOST] [--llvm-root=PATH]'
    echo 'HOST: macos-arm64, linux-arm64-gnu or linux-x64-gnu; omitted means the current host.'
    echo 'LLVM 23.1.3 SDK discovery is automatic; --llvm-root selects a path in the current environment.'
}

# Stop before installing an unvalidated or incompatible artifact.
die() { echo "error: $*" >&2; exit 1; }

# Compute content identities on both supported host families.
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d ' ' -f 1
    else shasum -a 256 "$1" | cut -d ' ' -f 1; fi
}

# Find a complete SDK in this build environment without downloading or changing PATH.
find_sdk() {
    local candidate prefix
    local candidates=()
    for candidate in llvm-config llvm-config-23; do
        if prefix=$(command -v "$candidate"); then candidates+=("$prefix"); fi
    done
    candidates+=(/opt/homebrew/opt/llvm@23/bin/llvm-config /usr/lib/llvm-23/bin/llvm-config)
    for candidate in "${candidates[@]}"; do
        [[ -x "$candidate" ]] || continue
        [[ $("$candidate" --version) == 23.1.3 ]] || continue
        prefix=$("$candidate" --prefix)
        [[ -x "$prefix/bin/clang++" && -f "$prefix/include/llvm/Plugins/PassPlugin.h" ]] || continue
        printf '%s\n' "$prefix"
        return 0
    done
    return 1
}

for option in "$@"; do
    case "$option" in
        --platform=*)
            [[ -z "$platform" ]] || die 'platform specified twice'
            platform=${option#*=}; [[ -n "$platform" ]] || die 'empty platform';;
        --llvm-root=*)
            [[ -z "$sdk" ]] || die 'SDK specified twice'
            sdk=${option#*=}; [[ -n "$sdk" ]] || die 'empty SDK path';;
        --test-link-option=*)
            link_options+=("--link-option=${option#*=}");;
        -h|--help) usage; exit 0;;
        *) die "unknown option: $option";;
    esac
done
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64) host=macos-arm64; extension=dylib;;
    Linux/aarch64|Linux/arm64) host=linux-arm64-gnu; extension=so;;
    Linux/x86_64|Linux/amd64) host=linux-x64-gnu; extension=so;;
    *) die 'unsupported build host';;
esac

# Build and validate only the current native host.
platform=${platform:-$host}
case "$platform" in macos-arm64|linux-arm64-gnu|linux-x64-gnu) ;; *) die "unsupported platform: $platform";; esac
[[ "$platform" == "$host" ]] || die "--platform must match current host: $host"

if [[ -z "$sdk" ]]; then sdk=$(find_sdk) || die 'complete LLVM 23.1.3 SDK not found; provide --llvm-root'; fi
sdk=$(cd -- "$sdk" && pwd)
for tool in clang clang++ llvm-config opt FileCheck llvm-nm llvm-readelf; do
    [[ -x "$sdk/bin/$tool" ]] || die "complete SDK lacks $tool"
done
[[ $("$sdk/bin/llvm-config" --version) == 23.1.3 ]] || die 'SDK must be LLVM 23.1.3'
[[ -f "$sdk/include/llvm/Plugins/PassPlugin.h" ]] || die 'SDK lacks LLVM development headers'
for tool in make git; do command -v "$tool" >/dev/null || die "missing $tool"; done
source_revision=$(git -c safe.directory="$repo" -C "$repo" rev-parse HEAD)
[[ -n "$source_revision" ]] || die 'missing source revision'
bundled="$repo/toolchain/llvm/$host/bin/clang"
[[ -x "$bundled" ]] || die "missing bundled Clang for $host"
source_dir="$repo/third_party/llvm-native-symbols"
work="$repo/build/llvm-native-symbols/maintainer-$host"
mkdir -p -- "$work"
mkdir -- "$work/lock" 2>/dev/null || die "another build holds $work/lock"
trap 'rmdir -- "$work/lock"' EXIT
echo "==> $host SDK: $sdk"
make_args=(-C "$source_dir" "LLVM_ROOT=$sdk" "BUILD_DIR=$work/build" 'CXXFLAGS=-O3 -DNDEBUG' LDFLAGS=)
# Use the same host linker for plugin builds, ordinary tests and sanitizer tests.
source "$repo/third_party/lld/source.env"
normal_linker="$repo/toolchain/llvm/$host/bin/ld.lld"
if [[ "$host" == macos-arm64 ]]; then normal_linker="$repo/toolchain/llvm/$host/bin/ld64.lld"; fi
[[ -x "$normal_linker" ]] || die 'missing bundled LLD; run scripts/build_lld.sh manually'
case $("$normal_linker" --version) in
    "Feng patch $LLD_PATCH_REVISION LLD $LLD_VERSION"|"Feng patch $LLD_PATCH_REVISION LLD $LLD_VERSION "*) ;;
    *) die 'bundled LLD does not match the maintained version and patch';;
esac
sanitizer_linker="$normal_linker"
make_args+=("LDFLAGS=--ld-path=\"$normal_linker\"")
normal_link_options=("--link-option=--ld-path=$normal_linker" ${link_options[@]+"${link_options[@]}"})
sanitizer_link_options=("${normal_link_options[@]}")
echo "==> linker: $("$normal_linker" --version)"

make "${make_args[@]}" clean
make "${make_args[@]}" all
library="$work/build/llvm_native_symbols.$extension"
[[ -f "$library" ]] || die 'build did not produce the plugin'
"$sdk/bin/llvm-nm" --undefined-only "$library" > "$work/imports.txt"
! grep -Eq '__asan_|__ubsan_|__tsan_|__msan_' "$work/imports.txt" || die 'Release plugin contains sanitizer instrumentation'
nm_flags=(--defined-only --extern-only)
if [[ "$extension" == so ]]; then nm_flags+=(--dynamic); fi
"$sdk/bin/llvm-nm" "${nm_flags[@]}" "$library" > "$work/exports.txt"
grep -q 'llvmGetPassPluginInfo' "$work/exports.txt" || die 'plugin entry is not exported'
! grep -E ' [TW] ' "$work/exports.txt" | grep -qv 'llvmGetPassPluginInfo' || die 'plugin exports an internal function'
bash "$source_dir/test/ir.sh" "$sdk/bin" "$library" "$work/ir"
bash "$source_dir/test/compile.sh" "$bundled" "$library" "$work/compile-bundled"
bash "$source_dir/test/compile.sh" "$sdk/bin/clang" "$library" "$work/compile-host"
bash "$source_dir/test/run.sh" "$bundled" "$library" "$work/bundled" ${normal_link_options[@]+"${normal_link_options[@]}"}
bash "$source_dir/test/run.sh" "$sdk/bin/clang" "$library" "$work/host" ${normal_link_options[@]+"${normal_link_options[@]}"}
bash "$source_dir/test/run.sh" "$sdk/bin/clang" "$library" "$work/strict-fp" --strict-fp ${normal_link_options[@]+"${normal_link_options[@]}"}
bash "$source_dir/test/run.sh" "$sdk/bin/clang" "$library" "$work/ubsan" --sanitizer ${sanitizer_link_options[@]+"${sanitizer_link_options[@]}"}
if [[ "$extension" == so ]]; then
    bash "$source_dir/test/run.sh" "$sdk/bin/clang" "$library" "$work/asan-ubsan" \
        --sanitizer=address,undefined ${sanitizer_link_options[@]+"${sanitizer_link_options[@]}"}
fi

# Inspect load-time dependencies without carrying a private LLVM instance or SDK rpath.
if [[ "$extension" == dylib ]]; then
    otool -L "$library" > "$work/dependencies.txt"
    otool -l "$library" > "$work/load-commands.txt"
    if tail -n +2 "$work/dependencies.txt" | grep -Evq '^[[:space:]]+/usr/lib/'; then
        die 'plugin has a non-system dynamic dependency'
    fi
    ! grep -q LC_RPATH "$work/load-commands.txt" || die 'plugin has an rpath'
else
    "$sdk/bin/llvm-readelf" -d "$library" > "$work/dependencies.txt"
    ! grep -Eq 'RPATH|RUNPATH|libLLVM|libclang' "$work/dependencies.txt" || die 'plugin retains an SDK dependency'
    if grep NEEDED "$work/dependencies.txt" | grep -Evq 'lib(stdc\+\+|gcc_s|c|m)\.so'; then
        die 'plugin has an unexpected dynamic dependency'
    fi
fi

# Verify the very same bytes after relocation, before touching the published directory.
mkdir -p "$work/relocated plugin"
cp "$library" "$work/relocated plugin/llvm_native_symbols.$extension"
bash "$source_dir/test/run.sh" "$bundled" "$work/relocated plugin/llvm_native_symbols.$extension" \
    "$work/relocated-test" ${normal_link_options[@]+"${normal_link_options[@]}"}
(
    cd "$source_dir"
    find Makefile LICENSE README.md src test -type f | LC_ALL=C sort | while IFS= read -r file; do
        printf '%s  %s\n' "$(sha256 "$file")" "$file"
    done
) > "$work/source-files.sha256"
{
    printf 'project=llvm-native-symbols\nplugin_version=0.1.0\nplugin_build_mode=Release\nbuild_system=make\nllvm_version=23.1.3\nhost=%s\n' "$host"
    printf 'source_git_revision=%s\n' "$source_revision"
    printf 'source_tree_sha256=%s\n' "$(sha256 "$work/source-files.sha256")"
    printf 'maintenance_script_sha256=%s\n' "$(sha256 "$0")"
    printf 'plugin_sha256=%s\n' "$(sha256 "$library")"
    printf 'built_at_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf 'sdk_build_mode=%s\nsdk_assertions=%s\nsdk_rtti=%s\n' \
        "$("$sdk/bin/llvm-config" --build-mode)" "$("$sdk/bin/llvm-config" --assertion-mode)" "$("$sdk/bin/llvm-config" --has-rtti)"
    printf 'host_compiler=%s\n' "$("$sdk/bin/clang" --version | head -1)"
    printf 'bundled_compiler=%s\n' "$("$bundled" --version | head -1)"
    printf 'test_link_options='; printf '%q ' ${link_options[@]+"${link_options[@]}"}; printf '\n'
    printf 'normal_linker_version=%s\nnormal_linker_sha256=%s\n' \
        "$("$normal_linker" --version)" "$(sha256 "$normal_linker")"
    printf 'ubsan_linker_version=%s\nubsan_linker_sha256=%s\n' \
        "$("$sanitizer_linker" --version)" "$(sha256 "$sanitizer_linker")"
    printf 'validation=IR; unchanged non-macOS IR; bundled O0/O2/O3; host O0/O2/O3; strict FP O0/O2/O3; UBSan O0/O2/O3; relocated bundled\n'
    printf 'behavior_checks=native aliases; callable identity; unknown side effects; floating boundaries; errno\n'
    if [[ "$extension" == so ]]; then printf 'additional_validation=ASan+UBSan O0/O2/O3\n'; fi
} > "$work/build-info.txt"
destination="$repo/toolchain/llvm-native-symbols/$host"
mkdir -p "$destination/lib"
# Replace only project-owned files, preserving any unrelated host metadata.
install -m 644 "$library" "$destination/lib/llvm_native_symbols.$extension.new"
mv "$destination/lib/llvm_native_symbols.$extension.new" "$destination/lib/llvm_native_symbols.$extension"
install -m 644 "$source_dir/LICENSE" "$destination/LICENSE"
# Remove maintenance reports left by older prebuilds from the installed layout.
rm -f "$destination/build-info.txt" "$destination/SHA256SUMS" "$destination/source-files.sha256"
echo "Validated prebuilt plugin installed: $destination"
