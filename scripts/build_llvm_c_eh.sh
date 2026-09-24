#!/usr/bin/env bash
# Manual maintainer entry: build and validate every host, or one explicitly selected host.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/.." && pwd)
sdk=
platform=
link_options=()
forward_options=()

# Keep ordinary builds out of this manual multi-host maintenance path.
usage() {
    echo 'usage: scripts/build_llvm_c_eh.sh [--platform=HOST] [--llvm-root=PATH]'
    echo 'HOST: macos-arm64, linux-arm64-gnu or linux-x64-gnu; omitted means all three.'
    echo 'LLVM 22.1.8 SDK discovery is automatic; --llvm-root overrides one selected host.'
    echo 'All-host maintenance uses macOS ARM64 and prepared Apple Container Linux environments.'
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
    for candidate in llvm-config llvm-config-22; do
        if prefix=$(command -v "$candidate"); then candidates+=("$prefix"); fi
    done
    candidates+=(/opt/homebrew/opt/llvm@22/bin/llvm-config /usr/lib/llvm-22/bin/llvm-config)
    for candidate in "${candidates[@]}"; do
        [[ -x "$candidate" ]] || continue
        [[ $("$candidate" --version) == 22.1.8 ]] || continue
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
            link_options+=("--link-option=${option#*=}"); forward_options+=("$option");;
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

# Each child has its own errexit context; one failed host cannot hide another result.
if [[ -z "$platform" ]]; then
    [[ -z "$sdk" ]] || die '--llvm-root requires --platform; all-host SDKs are discovered separately'
    [[ "$host" == macos-arm64 ]] || die 'all-host maintenance requires macOS ARM64; select --platform for a native Linux build'
    failed=()
    for selected in macos-arm64 linux-arm64-gnu linux-x64-gnu; do
        echo "==> Building and validating $selected"
        if bash "$script_dir/build_llvm_c_eh.sh" "--platform=$selected" ${forward_options[@]+"${forward_options[@]}"}; then
            echo "==> PASS $selected"
        else
            failed+=("$selected")
            echo "==> FAIL $selected" >&2
        fi
    done
    if [[ ${#failed[@]} != 0 ]]; then die "hosts not delivered: ${failed[*]}"; fi
    exit 0
fi
case "$platform" in macos-arm64|linux-arm64-gnu|linux-x64-gnu) ;; *) die "unsupported platform: $platform";; esac

# Reuse explicit, pre-provisioned Linux containers; never install tools implicitly.
if [[ "$platform" != "$host" ]]; then
    [[ "$host" == macos-arm64 && "$platform" == linux-*-gnu ]] || die "cannot build $platform from $host"
    command -v container >/dev/null || die 'Apple Container is required for Linux hosts'
    case "$platform" in
        linux-arm64-gnu) container_name=llvm-c-eh-arm64; container_arch=arm64;;
        linux-x64-gnu) container_name=llvm-c-eh-x64; container_arch=amd64;;
    esac
    inspection=$(container inspect "$container_name") || die "prepare Apple Container $container_name with LLVM 22.1.8 and this repository mounted at /work"
    # Apple plutil reads the documented JSON inspection output without extra dependencies.
    json_field() { printf '%s' "$inspection" | plutil -extract "$1" raw -o - -; }
    [[ $(json_field 0.configuration.platform.os) == linux &&
       $(json_field 0.configuration.platform.architecture) == "$container_arch" ]] || die "wrong architecture in $container_name"
    mounted=false
    index=0
    while destination=$(json_field "0.configuration.mounts.$index.destination" 2>/dev/null); do
        if [[ "$destination" == /work && $(json_field "0.configuration.mounts.$index.source") == "$repo" ]]; then mounted=true; fi
        index=$((index + 1))
    done
    "$mounted" || die "$container_name must mount $repo at /work"
    state=$(json_field 0.status.state)
    [[ "$state" == running || "$state" == stopped ]] || die "$container_name is not ready: $state"
    dispatch="$repo/build/llvm-c-eh/dispatch-$platform"
    mkdir -p "$(dirname "$dispatch")"
    mkdir "$dispatch" 2>/dev/null || die "another dispatch holds $dispatch"
    started=false
    # Restore only state owned by this invocation, even when compilation fails.
    cleanup_container() {
        if "$started"; then container stop "$container_name" >/dev/null; fi
        rmdir "$dispatch"
    }
    trap cleanup_container EXIT
    if [[ "$state" == stopped ]]; then container start "$container_name"; started=true; fi
    if [[ -n "$sdk" ]]; then forward_options+=("--llvm-root=$sdk"); fi
    container exec "$container_name" bash /work/scripts/build_llvm_c_eh.sh \
        "--platform=$platform" ${forward_options[@]+"${forward_options[@]}"}
    exit 0
fi

if [[ -z "$sdk" ]]; then sdk=$(find_sdk) || die 'complete LLVM 22.1.8 SDK not found; provide --llvm-root'; fi
sdk=$(cd -- "$sdk" && pwd)
for tool in clang clang++ llvm-config opt FileCheck llvm-nm llvm-readelf; do
    [[ -x "$sdk/bin/$tool" ]] || die "complete SDK lacks $tool"
done
[[ $("$sdk/bin/llvm-config" --version) == 22.1.8 ]] || die 'SDK must be LLVM 22.1.8'
[[ -f "$sdk/include/llvm/Plugins/PassPlugin.h" ]] || die 'SDK lacks LLVM development headers'
for tool in make git; do command -v "$tool" >/dev/null || die "missing $tool"; done
source_revision=$(git -c safe.directory="$repo" -C "$repo" rev-parse HEAD)
[[ -n "$source_revision" ]] || die 'missing source revision'
bundled="$repo/toolchain/llvm/$host/bin/clang"
[[ -x "$bundled" ]] || die "missing bundled Clang for $host"
source_dir="$repo/third_party/llvm-c-eh"
work="$repo/build/llvm-c-eh/maintainer-$host"
mkdir -p -- "$work"
mkdir -- "$work/lock" 2>/dev/null || die "another build holds $work/lock"
trap 'rmdir -- "$work/lock"' EXIT
echo "==> $host SDK: $sdk"
make_args=(-C "$source_dir" "LLVM_ROOT=$sdk" "BUILD_DIR=$work/build" 'CXXFLAGS=-O3 -DNDEBUG' LDFLAGS=)
normal_link_options=(${link_options[@]+"${link_options[@]}"})
sanitizer_link_options=(${link_options[@]+"${link_options[@]}"})
# Keep the original linker for Release/ordinary coverage; only UBSan uses I04's fix.
if [[ "$host" == macos-arm64 ]]; then
    source "$repo/third_party/lld/source.env"
    original_linker="$repo/toolchain/llvm/$host/bin/lld"
    sanitizer_linker="$repo/toolchain/test_tools/lld/$host/bin/ld64.lld"
    [[ -x "$original_linker" ]] || die 'missing original bundled LLD'
    [[ -x "$sanitizer_linker" ]] || die 'missing UBSan test LLD; run scripts/build_test_lld.sh manually'
    case $("$original_linker" -flavor darwin --version) in
        "LLD 22.1.8"|"LLD 22.1.8 "*) ;;
        *) die 'original LLD must be 22.1.8';;
    esac
    [[ $("$sanitizer_linker" --version) == "Feng UBSan test tools patch $LLD_PATCH_REVISION LLD 22.1.8" ]] ||
        die 'UBSan test LLD does not match the approved version and patch'
    mkdir -p "$work/tools"
    ln -sf "$original_linker" "$work/tools/ld64.lld"
    normal_linker="$work/tools/ld64.lld"
    make_args+=("LDFLAGS=--ld-path=\"$normal_linker\"")
    normal_link_options=("--link-option=--ld-path=$normal_linker" ${link_options[@]+"${link_options[@]}"})
    sanitizer_link_options=("--link-option=--ld-path=$sanitizer_linker" ${link_options[@]+"${link_options[@]}"})
    echo "==> ordinary linker: $("$normal_linker" --version)"
    echo "==> UBSan linker: $("$sanitizer_linker" --version)"
fi
make "${make_args[@]}" clean
make "${make_args[@]}" all
library="$work/build/llvm_c_eh.$extension"
[[ -f "$library" ]] || die 'build did not produce the plugin'
"$sdk/bin/llvm-nm" --undefined-only "$library" > "$work/imports.txt"
! grep -Eq '__asan_|__ubsan_|__tsan_|__msan_' "$work/imports.txt" || die 'Release plugin contains sanitizer instrumentation'
nm_flags=(--defined-only --extern-only)
if [[ "$extension" == so ]]; then nm_flags+=(--dynamic); fi
"$sdk/bin/llvm-nm" "${nm_flags[@]}" "$library" > "$work/exports.txt"
grep -q 'llvmGetPassPluginInfo' "$work/exports.txt" || die 'plugin entry is not exported'
! grep -E ' [TW] ' "$work/exports.txt" | grep -qv 'llvmGetPassPluginInfo' || die 'plugin exports an internal function'
bash "$source_dir/test/ir.sh" "$sdk/bin" "$library" "$work/ir"
bash "$source_dir/test/passthrough.sh" "$bundled" "$library" "$work/passthrough-bundled" ${normal_link_options[@]+"${normal_link_options[@]}"}
bash "$source_dir/test/passthrough.sh" "$sdk/bin/clang" "$library" "$work/passthrough-host" --sanitizer ${sanitizer_link_options[@]+"${sanitizer_link_options[@]}"}
bash "$source_dir/test/sanitizer.sh" "$sdk/bin/clang" "$library" "$work/sanitizer-checks" ${sanitizer_link_options[@]+"${sanitizer_link_options[@]}"}
bash "$source_dir/test/run.sh" "$bundled" "$library" "$work/bundled" ${normal_link_options[@]+"${normal_link_options[@]}"}
bash "$source_dir/test/run.sh" "$sdk/bin/clang" "$library" "$work/host" ${normal_link_options[@]+"${normal_link_options[@]}"}
bash "$source_dir/test/run.sh" "$sdk/bin/clang" "$library" "$work/ubsan" --sanitizer ${sanitizer_link_options[@]+"${sanitizer_link_options[@]}"}
if [[ "$extension" == so ]]; then
    bash "$source_dir/test/run.sh" "$sdk/bin/clang" "$library" "$work/asan-ubsan" \
        --sanitizer=address,undefined ${link_options[@]+"${link_options[@]}"}
fi
bash "$source_dir/test/benchmark.sh" "$bundled" "$library" "$work/benchmark" ${normal_link_options[@]+"${normal_link_options[@]}"}

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
cp "$library" "$work/relocated plugin/llvm_c_eh.$extension"
bash "$source_dir/test/run.sh" "$bundled" "$work/relocated plugin/llvm_c_eh.$extension" \
    "$work/relocated-test" ${normal_link_options[@]+"${normal_link_options[@]}"}
(
    cd "$source_dir"
    find Makefile LICENSE README.md include src test -type f | LC_ALL=C sort | while IFS= read -r file; do
        printf '%s  %s\n' "$(sha256 "$file")" "$file"
    done
) > "$work/source-files.sha256"
{
    printf 'project=llvm-c-eh\nplugin_version=0.1.0\nplugin_build_mode=Release\nbuild_system=make\nprotocol_version=1\nllvm_version=22.1.8\nhost=%s\n' "$host"
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
    if [[ "$host" == macos-arm64 ]]; then
        printf 'normal_linker_version=%s\nnormal_linker_sha256=%s\n' \
            "$("$normal_linker" --version)" "$(sha256 "$normal_linker")"
        printf 'ubsan_linker_version=%s\nubsan_linker_sha256=%s\n' \
            "$("$sanitizer_linker" --version)" "$(sha256 "$sanitizer_linker")"
    fi
    printf 'validation=IR; unchanged non-protocol input; bundled O0/O2/O3; host O0/O2/O3; UBSan O0/O2/O3; relocated bundled; cost observations\n'
    printf 'ubsan_checks=protected body; handler; overflow; bounds; indirect function type; fatal; recovery\n'
    if [[ "$extension" == so ]]; then printf 'additional_validation=ASan+UBSan O0/O2/O3\n'; fi
} > "$work/build-info.txt"
destination="$repo/toolchain/llvm-c-eh/$host"
mkdir -p "$destination/lib" "$destination/include"
# Replace only project-owned files, preserving any unrelated host metadata.
install -m 644 "$library" "$destination/lib/llvm_c_eh.$extension.new"
mv "$destination/lib/llvm_c_eh.$extension.new" "$destination/lib/llvm_c_eh.$extension"
install -m 644 "$source_dir/include/llvm_c_eh.h" "$destination/include/llvm_c_eh.h"
install -m 644 "$source_dir/LICENSE" "$destination/LICENSE"
install -m 644 "$work/build-info.txt" "$destination/build-info.txt"
install -m 644 "$work/source-files.sha256" "$destination/source-files.sha256"
echo "Validated prebuilt plugin installed: $destination"
