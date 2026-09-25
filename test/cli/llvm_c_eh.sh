#!/usr/bin/env bash
# Validate the installed plugin through the production driver and native Feng EH.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
source scripts/host_platform.sh
host=$(feng_detect_host_platform)
extension=so
if [[ $host == macos-* ]]; then extension=dylib; fi
work=$(mktemp -d "$root/build/llvm-c-eh-integration.XXXXXX")
trap 'rm -rf "$work"' EXIT
stage="$work/install with spaces"
mkdir -p "$stage/bin" "$stage/toolchain"
cp build/bin/test_llvm_c_eh_driver "$stage/bin/driver"
cp build/bin/feng "$stage/bin/feng"
cp -R build/include "$stage/include"
cp -R build/lib "$stage/lib"
ln -s "$root/toolchain/llvm/$host" "$stage/toolchain/llvm"
ln -s "$root/toolchain/sysroot" "$stage/toolchain/sysroot"
cp -R "toolchain/llvm-c-eh/$host" "$stage/toolchain/llvm-c-eh"
# Archive extraction and relocation must not preserve an earlier plugin path.
tar -cf "$work/install.tar" -C "$stage" .
mkdir "$work/relocated install"
tar -xf "$work/install.tar" -C "$work/relocated install"
rm -rf "$stage"
stage="$work/relocated install"
plugin="$stage/toolchain/llvm-c-eh/lib/llvm_c_eh.$extension"
include="$stage/toolchain/llvm-c-eh/include"
compiler=${FENG_CC:-"$root/build/toolchain/llvm/bin/clang"}
compiler=$(command -v "$compiler")
flags=(-std=gnu11 -fexceptions "-I$include")
sanitizer_options=()
if [[ ${FENG_CC_FLAGS:-} == *-fsanitize=undefined* ]]; then
    flags+=(-fsanitize=undefined -fno-sanitize-recover=all)
    sanitizer_options+=(--sanitizer)
fi
sdk_flags=()
if [[ $host == macos-* ]]; then sdk_flags+=(-isysroot "$(xcrun --show-sdk-path)"); fi
flags+=(${sdk_flags[@]+"${sdk_flags[@]}"})

# Log argv boundaries, then execute the selected real compiler without a shell split.
cat > "$work/record compiler" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$CEH_TRACE"
exec "$CEH_COMPILER" "$@"
WRAPPER
chmod +x "$work/record compiler"
export CEH_COMPILER="$compiler" CEH_TRACE="$work/argv.txt"

# Require one expected failure and its diagnostic, never just a nonzero exit.
expect_failure() {
    local diagnostic=$1; shift
    if "$@" > "$work/failure.log" 2>&1; then
        echo "expected failure: $*" >&2; exit 1
    fi
    if ! grep -F "$diagnostic" "$work/failure.log" >/dev/null; then
        cat "$work/failure.log" >&2; exit 1
    fi
}

# Compile the independent protocol consumer without depending on Feng's personality.
cat > "$work/support.c" <<EOF
#include "$root/third_party/llvm-c-eh/test/runtime.c"
#include "$root/third_party/llvm-c-eh/test/producer.c"
EOF
"$compiler" "${flags[@]}" -c "$work/support.c" -o "$work/support.o"
build/toolchain/llvm/bin/llvm-ar rcs "$work/support.a" "$work/support.o"
cat > "$work/protocol.c" <<EOF
#include "$root/third_party/llvm-c-eh/test/smoke.c"
EOF
cat > "$work/main.c" <<'EOF'
extern int ceh_test_smoke(int);
/* Both native unwinding and the ordinary return must survive optimization. */
int main(void) { return ceh_test_smoke(-7) != 7 || ceh_test_smoke(4) != 5; }
EOF
cat "$work/protocol.c" "$work/main.c" > "$work/program.c"

for release in 0 1; do
    # Both output kinds load the same host plugin, with linker flags only for bin.
    FENG_CC="$work/record compiler" "$stage/bin/driver" bin "$host" "$release" \
        "$work/program.c" "$work/program" "$work/support.a"
    grep -Fx -- "-fpass-plugin=$plugin" "$CEH_TRACE" >/dev/null
    grep -Fx -- "$include" "$CEH_TRACE" >/dev/null
    grep -Fx -- '-fuse-ld=lld' "$CEH_TRACE" >/dev/null
    "$work/program"
    FENG_CC="$work/record compiler" "$stage/bin/driver" lib "$host" "$release" \
        "$work/protocol.c" "$work/protocol.a" -
    grep -Fx -- "-fpass-plugin=$plugin" "$CEH_TRACE" >/dev/null
    if grep -E -- '^-fuse-ld=|^--ld-path=' "$CEH_TRACE"; then exit 1; fi
    "$compiler" "${flags[@]}" "$work/main.c" "$work/protocol.a" "$work/support.a" \
        -fuse-ld=lld -o "$work/consumer"
    "$work/consumer"
done

# Every target consumes the host plugin, including SDK-free macOS library output.
# Cross-target objects intentionally omit sanitizer runtimes and do not execute.
for target in macos-arm64 linux-x64-gnu linux-x64-musl linux-arm64-gnu linux-arm64-musl; do
    FENG_CC_FLAGS=-Werror FENG_CC="$work/record compiler" "$stage/bin/driver" lib \
        "$target" 1 "$work/protocol.c" "$work/cross.a" -
    grep -Fx -- "-fpass-plugin=$plugin" "$CEH_TRACE" >/dev/null
    if grep -E -- '^-fuse-ld=|^--ld-path=' "$CEH_TRACE"; then exit 1; fi
done

# Missing inputs, unexpanded LFS pointers and wrong-host libraries must fail closed.
mv "$plugin" "$work/saved-plugin"
expect_failure 'cannot configure LLVM C EH plugin' "$stage/bin/driver" lib "$host" 0 \
    "$work/protocol.c" "$work/bad.a" -
printf 'version https://git-lfs.github.com/spec/v1\n' > "$plugin"
expect_failure 'load plugin' "$stage/bin/driver" lib "$host" 0 "$work/protocol.c" "$work/bad.a" -
if [[ $host == macos-* ]]; then other=linux-x64-gnu; else other=macos-arm64; fi
other_extension=so
if [[ $other == macos-* ]]; then other_extension=dylib; fi
cp "toolchain/llvm-c-eh/$other/lib/llvm_c_eh.$other_extension" "$plugin"
expect_failure 'load plugin' "$stage/bin/driver" lib "$host" 0 "$work/protocol.c" "$work/bad.a" -
# A loadable library with an unsupported handshake must retain LLVM's error.
cat > "$work/unsupported.c" <<'EOF'
#include <stdint.h>
/* Mirror only the public plugin handshake, deliberately selecting API version 0. */
typedef struct PluginInfo {
    uint32_t version;
    const char *name;
    const char *release;
    void (*register_passes)(void *);
    void (*pre_codegen)(void);
} PluginInfo;
/* No LLVM dependency or callbacks are needed to test version rejection. */
PluginInfo llvmGetPassPluginInfo(void) {
    return (PluginInfo){0, "unsupported-test-plugin", "0", 0, 0};
}
EOF
"$compiler" ${sdk_flags[@]+"${sdk_flags[@]}"} -fPIC -shared -fuse-ld=lld \
    "$work/unsupported.c" -o "$plugin"
expect_failure 'Wrong API version' "$stage/bin/driver" lib "$host" 0 "$work/protocol.c" "$work/bad.a" -
mv "$work/saved-plugin" "$plugin"
mv "$include/llvm_c_eh.h" "$work/saved-header"
expect_failure 'llvm_c_eh.h' "$stage/bin/driver" lib "$host" 0 "$work/protocol.c" "$work/bad.a" -
mv "$work/saved-header" "$include/llvm_c_eh.h"
expect_failure 'invalid linker name' env FENG_CC_FLAGS=-fuse-ld=missing-feng-test-linker \
    "$stage/bin/driver" bin "$host" 0 "$work/program.c" "$work/bad" "$work/support.a"

# Loading the relocated (and, on macOS, re-signed) plugin must also work by default.
if [[ $host == macos-* ]]; then codesign --force --sign - "$plugin" >/dev/null 2>&1; fi
"$stage/bin/driver" bin "$host" 0 "$work/program.c" "$work/signed" "$work/support.a"
"$work/signed"
if [[ $host == macos-* && ${#sanitizer_options[@]} != 0 ]]; then
    FENG_CC_FLAGS="${FENG_CC_FLAGS} -###" "$stage/bin/driver" bin "$host" 0 \
        "$work/program.c" "$work/link-probe" "$work/support.a" > "$work/link.txt" 2>&1
    grep -F "\"$root/toolchain/test_tools/lld/$host/bin/ld64.lld\"" "$work/link.txt" >/dev/null
fi

# Exercise generated Feng C in the relocated installation, preserving its IR.
printf 'module main; func main(args: string[]): void {}\n' > "$work/main.ff"
"$stage/bin/feng" "$work/main.ff" --target=bin --keep-ir --out="$work/feng-bin"
"$stage/bin/feng" "$work/main.ff" --target=lib --out="$work/feng-lib"
generated=$(find "$work/feng-bin" -name '*.c' -type f)
[[ -n $generated ]]
# Keep the no-protocol equivalence control independent of Feng's EH emission.
cat > "$work/control.c" <<'EOF'
extern int transform(int);
/* A pass with no configured functions must preserve ordinary calls and branches. */
int control(int value) { return value > 0 ? transform(value) : value + 1; }
EOF
for optimization in 0 2; do
    "$compiler" "${flags[@]}" "-I$stage/include" -O"$optimization" -S -emit-llvm \
        "-fpass-plugin=$plugin" "$generated" -o "$work/feng.ll"
    if grep -F '__llvm_c_eh_' "$work/feng.ll"; then exit 1; fi
    grep -F 'invoke ' "$work/feng.ll" >/dev/null
    grep -F 'landingpad ' "$work/feng.ll" >/dev/null
    grep -F 'resume ' "$work/feng.ll" >/dev/null
    for format in ll s; do
        emit=()
        if [[ $format == ll ]]; then emit+=(-emit-llvm); fi
        "$compiler" "${flags[@]}" "-I$stage/include" -O"$optimization" -S \
            ${emit[@]+"${emit[@]}"} "$work/control.c" -o "$work/plain.$format"
        "$compiler" "${flags[@]}" "-I$stage/include" -O"$optimization" -S \
            ${emit[@]+"${emit[@]}"} "-fpass-plugin=$plugin" "$work/control.c" -o "$work/plugin.$format"
        cmp "$work/plain.$format" "$work/plugin.$format"
    done
done

# Reuse independent no-protocol controls and actual UB checks with the same Release plugin.
bash third_party/llvm-c-eh/test/passthrough.sh "$compiler" "$plugin" "$work/passthrough" \
    ${sanitizer_options[@]+"${sanitizer_options[@]}"} --link-option=-fuse-ld=lld
if [[ ${#sanitizer_options[@]} != 0 ]]; then
    bash third_party/llvm-c-eh/test/sanitizer.sh "$compiler" "$plugin" "$work/ubsan" \
        --link-option=-fuse-ld=lld
fi
echo 'LLVM C EH integration: driver bin/lib, five targets, relocation, failures and unchanged non-protocol output verified'
