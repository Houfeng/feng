#!/usr/bin/env bash
# Exercise production release assembly with small three-host fixtures.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
work=$(mktemp -d "$root/build/native-symbols-release.XXXXXX")
trap 'rm -rf "$work"' EXIT
hosts=(macos-arm64 linux-x64-gnu linux-arm64-gnu)
clang="$root/build/toolchain/llvm/bin/clang"
ar="$root/build/toolchain/llvm/bin/llvm-ar"
source_root="$work/source"
components="$work/components"
mkdir -p "$source_root" "$components"
printf '0.1.0\n' > "$source_root/VERSION"
printf '/* Small cross-format fixture; never executed. */\nint fixture(void) { return 0; }\n' > "$work/input.c"
for host in "${hosts[@]}"; do
    case "$host" in
        macos-arm64) triple=arm64-apple-macosx; extension=dylib; extra=(debugserver ld64.lld);;
        linux-arm64-gnu) triple=aarch64-unknown-linux-gnu; extension=so; extra=(lldb-server);;
        linux-x64-gnu) triple=x86_64-unknown-linux-gnu; extension=so; extra=(lldb-server);;
    esac
    build="$components/$host"
    mkdir -p "$build/bin" "$build/include"
    "$clang" --target="$triple" -c "$work/input.c" -o "$build/bin/feng"
    chmod +x "$build/bin/feng"
    cp build/include/feng_generated.h build/include/feng_runtime.h \
        build/include/feng_runtime_contract.inc "$build/include/"
    platforms=("$host")
    if [[ $host == linux-* ]]; then platforms+=("${host%-gnu}-musl"); fi
    for platform in "${platforms[@]}"; do
        mkdir -p "$build/lib/$platform"
        "$ar" rcs "$build/lib/$platform/libfeng_runtime.a" "$build/bin/feng"
        if [[ $platform == linux-* ]]; then
            mkdir -p "$source_root/toolchain/sysroot/$platform/usr/include" \
                "$source_root/toolchain/sysroot/$platform/usr/lib" \
                "$source_root/toolchain/sysroot/$platform/lib/gcc"
        fi
    done
    # Fixture objects stand in for host-built components; checksum every input.
    (
        cd "$build"
        find bin include lib -type f | LC_ALL=C sort | while IFS= read -r path; do
            if command -v sha256sum >/dev/null 2>&1; then
                sha256sum "$path"
            else
                shasum -a 256 "$path"
            fi
        done
    ) > "$build/SHA256SUMS"
    llvm="$source_root/toolchain/llvm/$host"
    mkdir -p "$llvm/bin" "$llvm/lib"
    for tool in clang lld ld.lld llvm-ar llvm-ranlib lldb lldb-dap lldb-argdumper "${extra[@]}"; do
        printf '#!/bin/sh\nexit 0\n' > "$llvm/bin/$tool"
        chmod +x "$llvm/bin/$tool"
    done
    for plugin in llvm-c-eh llvm-native-symbols; do
        plugin_root="$source_root/toolchain/$plugin/$host"
        mkdir -p "$plugin_root/lib"
        cp "$build/bin/feng" "$plugin_root/lib/${plugin//-/_}.$extension"
        printf '%s %s license\n' "$plugin" "$host" > "$plugin_root/LICENSE"
    done
    mkdir -p "$source_root/toolchain/llvm-c-eh/$host/include"
    cp third_party/llvm-c-eh/include/llvm_c_eh.h \
        "$source_root/toolchain/llvm-c-eh/$host/include/"
done

# Use the same assembly entry point as CI, without running release fixtures.
assemble() {
    bash scripts/release_assemble.sh --version=0.1.0 --components="$components" \
        --source-root="$source_root" --output="$work/packages" --archive-tool="$ar"
}

# Assert failures come from the new input contract, not an unrelated fixture.
expect_failure() {
    local diagnostic=$1; shift
    if "$@" > "$work/failure.log" 2>&1; then
        echo "expected failure: $*" >&2; exit 1
    fi
    grep -F "$diagnostic" "$work/failure.log" >/dev/null || {
        cat "$work/failure.log" >&2; exit 1;
    }
}

assemble > "$work/assemble.log"
for host in "${hosts[@]}"; do
    package="feng-0.1.0-$host"
    unzip -q "$work/packages/$package.zip" -d "$work/extracted"
    for plugin in llvm-c-eh llvm-native-symbols; do
        installed="$work/extracted/$package/toolchain/$plugin"
        expected="$source_root/toolchain/$plugin/$host"
        diff -r "$expected" "$installed"
        [[ $(find "$installed/lib" -type f | wc -l | tr -d ' ') == 1 ]]
        if find "$installed" -type f | grep -E '/(build-info\.txt|SHA256SUMS|source-files\.sha256)$'; then
            echo 'plugin maintenance record included in release' >&2; exit 1
        fi
    done
done

for host in "${hosts[@]}"; do
    extension=so
    if [[ $host == macos-* ]]; then extension=dylib; fi
    plugin_root="$source_root/toolchain/llvm-native-symbols/$host"
    plugin="$plugin_root/lib/llvm_native_symbols.$extension"
    mv "$plugin" "$work/saved-plugin"
    expect_failure "required file not found: $plugin" assemble
    printf 'version https://git-lfs.github.com/spec/v1\n' > "$plugin"
    expect_failure 'LLVM native symbols plugin has unexpected format' assemble
    other=macos-arm64
    if [[ $host == macos-* ]]; then other=linux-arm64-gnu; fi
    cp "$components/$other/bin/feng" "$plugin"
    expect_failure 'LLVM native symbols plugin has unexpected format' assemble
    mv "$work/saved-plugin" "$plugin"
    mv "$plugin_root/LICENSE" "$work/saved-license"
    expect_failure "$plugin_root/LICENSE" assemble
    mv "$work/saved-license" "$plugin_root/LICENSE"
done

# Installation verification must reject missing/wrong-host inputs before executing Feng.
source scripts/host_platform.sh
host=$(feng_detect_host_platform)
installed="$work/extracted/feng-0.1.0-$host"
extension=so
if [[ $host == macos-* ]]; then extension=dylib; fi
plugin="$installed/toolchain/llvm-native-symbols/lib/llvm_native_symbols.$extension"
mv "$plugin" "$work/saved-plugin"
expect_failure 'installed LLVM native symbols plugin not found' \
    bash scripts/release_verify_install.sh --root="$installed" --version=0.1.0
printf 'version https://git-lfs.github.com/spec/v1\n' > "$plugin"
expect_failure 'installed LLVM native symbols plugin has unexpected format' \
    bash scripts/release_verify_install.sh --root="$installed" --version=0.1.0
mv "$work/saved-plugin" "$plugin"
rm "$installed/toolchain/llvm-native-symbols/LICENSE"
expect_failure 'installed LLVM native symbols license not found' \
    bash scripts/release_verify_install.sh --root="$installed" --version=0.1.0
echo 'PASS: both LLVM plugins ship once per host; native symbols release/install inputs validated'
