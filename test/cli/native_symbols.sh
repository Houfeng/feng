#!/usr/bin/env bash
# Verify target-gated loading through the real driver and a relocated installation.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$root"
source scripts/host_platform.sh
host=$(feng_detect_host_platform)
extension=so
if [[ $host == macos-* ]]; then extension=dylib; fi
work=$(mktemp -d "$root/build/native-symbols-integration.XXXXXX")
trap 'rm -rf "$work"' EXIT
stage="$work/initial install"
mkdir -p "$stage/bin" "$stage/toolchain"
cp build/bin/test_llvm_c_eh_driver "$stage/bin/driver"
cp build/bin/feng "$stage/bin/feng"
cp -R build/include "$stage/include"
cp -R build/lib "$stage/lib"
ln -s "$root/toolchain/llvm/$host" "$stage/toolchain/llvm"
ln -s "$root/toolchain/sysroot" "$stage/toolchain/sysroot"
cp -R "toolchain/llvm-c-eh/$host" "$stage/toolchain/llvm-c-eh"
cp -R "toolchain/llvm-native-symbols/$host" "$stage/toolchain/llvm-native-symbols"
tar -cf "$work/install.tar" -C "$stage" .
mkdir "$work/relocated install"
tar -xf "$work/install.tar" -C "$work/relocated install"
rm -rf "$stage"
stage="$work/relocated install"
plugin="$stage/toolchain/llvm-native-symbols/lib/llvm_native_symbols.$extension"
eh_plugin="$stage/toolchain/llvm-c-eh/lib/llvm_c_eh.$extension"
compiler=$(command -v "${FENG_CC:-$root/build/toolchain/llvm/bin/clang}")
export NS_COMPILER="$compiler" NS_TRACE="$work/argv.txt" NS_IR="$work/captured.ll"
# Record exact argv and inspect the same library compile with LLVM IR output.
cat > "$work/record compiler" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$NS_TRACE"
"$NS_COMPILER" "$@"
ir_args=()
capture=false
for arg in "$@"; do
    if [[ $arg == -c ]]; then
        capture=true
    else
        ir_args+=("$arg")
    fi
done
if "$capture"; then
    "$NS_COMPILER" "${ir_args[@]}" -S -emit-llvm -o "$NS_IR"
fi
WRAPPER
chmod +x "$work/record compiler"

# Require the expected diagnostic, so unrelated compilation errors cannot pass.
expect_failure() {
    local diagnostic=$1; shift
    if "$@" > "$work/failure.log" 2>&1; then
        echo "expected failure: $*" >&2; exit 1
    fi
    grep -F "$diagnostic" "$work/failure.log" >/dev/null || {
        cat "$work/failure.log" >&2; exit 1;
    }
}

# Verify host plugin selection, target gating and coexistence with native EH.
check_trace() {
    local target=$1
    [[ $(grep -Fxc -- "-fpass-plugin=$eh_plugin" "$NS_TRACE") == 1 ]]
    if [[ $target == macos-* ]]; then
        [[ $(grep -Fxc -- "-fpass-plugin=$plugin" "$NS_TRACE") == 1 ]]
    elif grep -F 'llvm-native-symbols' "$NS_TRACE"; then
        echo 'non-macOS target loaded native symbols plugin' >&2; exit 1
    fi
}

cat > "$work/calls.c" <<'C'
#if defined(__APPLE__)
#define NATIVE(name) __asm__("_" name)
#else
#define NATIVE(name) __asm__(name)
#endif
extern double local_sqrt(double) NATIVE("sqrt");
/* Keep a nonconstant call and its address visible even at release optimization. */
double evaluate(double value) { return local_sqrt(value); }
/* Both names must resolve to the same native function. */
double (*address(void))(double) { return local_sqrt; }
C
cat > "$work/main.c" <<'C'
/* Native execution checks the ordinary driver link path in both build modes. */
int main(void) { return 0; }
C
for release in 0 1; do
    for target in macos-arm64 linux-x64-gnu linux-x64-musl linux-arm64-gnu linux-arm64-musl; do
        FENG_CC_FLAGS=-Werror FENG_CC="$work/record compiler" "$stage/bin/driver" \
            lib "$target" "$release" "$work/calls.c" "$work/calls.a" -
        check_trace "$target"
        if [[ $target == macos-* ]]; then
            grep -F '@sqrt' "$NS_IR" >/dev/null
            if grep -F '\01_sqrt' "$NS_IR"; then exit 1; fi
        fi
    done
    FENG_CC="$work/record compiler" "$stage/bin/driver" bin "$host" "$release" \
        "$work/main.c" "$work/program" -
    check_trace "$host"
    "$work/program"
done

# Real Feng external declarations must benefit without an explicit plugin flag.
cat > "$work/external.ff" <<'FENG'
module native_symbols;
@cdecl("libm")
extern func sqrt(value: f64): f64;
open func evaluate(value: f64): f64 { return sqrt(value); }
FENG
FENG_CC_FLAGS=-Werror FENG_CC="$work/record compiler" "$stage/bin/feng" \
    "$work/external.ff" --target=lib --platform=macos-arm64 --keep-ir --out="$work/feng-lib"
check_trace macos-arm64
grep -F '@sqrt' "$NS_IR" >/dev/null
if grep -F '\01_sqrt' "$NS_IR"; then exit 1; fi

# Missing or broken plugins matter only for targets that load them.
mv "$plugin" "$work/saved-plugin"
expect_failure 'cannot configure LLVM native symbols plugin' env FENG_CC_FLAGS= \
    "$stage/bin/driver" lib macos-arm64 0 "$work/calls.c" "$work/bad.a" -
for target in linux-x64-gnu linux-x64-musl linux-arm64-gnu linux-arm64-musl; do
    FENG_CC_FLAGS=-Werror FENG_CC="$work/record compiler" "$stage/bin/driver" \
        lib "$target" 1 "$work/calls.c" "$work/linux.a" -
    check_trace "$target"
done
printf 'version https://git-lfs.github.com/spec/v1\n' > "$plugin"
expect_failure 'load plugin' env FENG_CC_FLAGS= "$stage/bin/driver" \
    lib macos-arm64 0 "$work/calls.c" "$work/bad.a" -
other=macos-arm64
other_extension=dylib
if [[ $host == macos-* ]]; then other=linux-arm64-gnu; other_extension=so; fi
cp "$root/toolchain/llvm-native-symbols/$other/lib/llvm_native_symbols.$other_extension" "$plugin"
expect_failure 'load plugin' env FENG_CC_FLAGS= "$stage/bin/driver" \
    lib macos-arm64 1 "$work/calls.c" "$work/bad.a" -
# A loadable library still must implement the expected LLVM plugin API.
cat > "$work/unsupported.c" <<'C'
#include <stdint.h>
/* Mirror the public handshake solely to reject an unsupported API version. */
typedef struct PluginInfo {
    uint32_t version;
    const char *name;
    const char *release;
    void (*register_passes)(void *);
    void (*pre_codegen)(void);
} PluginInfo;
/* No LLVM callbacks are reached when version zero is rejected. */
PluginInfo llvmGetPassPluginInfo(void) {
    return (PluginInfo){0, "unsupported-native-symbols-test", "0", 0, 0};
}
C
sdk_flags=()
if [[ $host == macos-* ]]; then sdk_flags+=(-isysroot "$(xcrun --show-sdk-path)"); fi
"$compiler" ${sdk_flags[@]+"${sdk_flags[@]}"} -fPIC -shared -fuse-ld=lld \
    "$work/unsupported.c" -o "$plugin"
expect_failure 'Wrong API version' env FENG_CC_FLAGS= "$stage/bin/driver" \
    lib macos-arm64 0 "$work/calls.c" "$work/bad.a" -
mv "$work/saved-plugin" "$plugin"
if [[ $host == macos-* ]]; then codesign --force --sign - "$plugin" >/dev/null 2>&1; fi
FENG_CC_FLAGS=-Werror FENG_CC="$work/record compiler" "$stage/bin/driver" \
    lib macos-arm64 1 "$work/calls.c" "$work/signed.a" -
check_trace macos-arm64
echo 'PASS: native symbols default loading, host/target matrix, relocation, IR and failures'
