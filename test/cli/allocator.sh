#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
mkdir -p temp
WORK="$(mktemp -d "${ROOT}/temp/allocator.XXXXXX")"
LLVM="${ROOT}/build/toolchain/llvm/bin"
FIXTURE="${ROOT}/test/cli/fixtures/allocator"

# Remove this test's staging tree, preserving the compiler installation.
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

# Fail with a useful test-specific diagnostic.
die() { echo "allocator test: $*" >&2; exit 1; }

# Ordinary coverage uses bundled tools; the ASan phase below retains its host
# compiler and instrumented runtime and verifies the rejected combination.
feng() { env -u FENG_CC -u FENG_CC_FLAGS "${ROOT}/build/bin/feng" "$@"; }

# Mach-O needs explicit permission for the fixture's optional weak observation.
# Permit only this query to be absent; builtin must still pass real heap checks.
# Recursive library compiles may ignore this link-only argument; diagnostics
# for every other argument remain enabled.
native_feng() {
  if [[ "${HOST}" == macos-* ]]; then
    env -u FENG_CC FENG_CC_FLAGS='--start-no-unused-arguments -Wl,-U,_mi_is_in_heap_region --end-no-unused-arguments' \
      "${ROOT}/build/bin/feng" "$@"
  else
    feng "$@"
  fi
}

# Require a diagnostic and a nonzero exit without ignoring unexpected success.
reject() {
  local expected="$1"; shift
  if feng "$@" >"${WORK}/reject.log" 2>&1; then die "unexpected success: $*"; fi
  grep -F -- "${expected}" "${WORK}/reject.log" >/dev/null || { cat "${WORK}/reject.log"; die "missing diagnostic: ${expected}"; }
}

printf 'module allocator_empty;\nfunc main(args: string[]) {}\n' >"${WORK}/empty.ff"
for flags in '-fsanitize=address' '-fsanitize=undefined,address' \
             '-fsanitize=address -fno-sanitize=undefined' \
             '-fno-sanitize=all -fsanitize=address'; do
  if FENG_CC_FLAGS="${flags}" "${ROOT}/build/bin/feng" "${WORK}/empty.ff" \
       --alloc=builtin "--out=${WORK}/asan-rejected" >"${WORK}/asan.log" 2>&1; then
    die "builtin accepted ASan: ${flags}"
  fi
  grep -F 'incompatible with AddressSanitizer' "${WORK}/asan.log" >/dev/null || { cat "${WORK}/asan.log"; die 'missing ASan diagnostic'; }
done
if [[ "${1:-}" == --asan ]]; then
  "${ROOT}/build/bin/feng" "${WORK}/empty.ff" --alloc=system "--out=${WORK}/asan-system"
  "${WORK}/asan-system/bin/empty"
  echo 'allocator ASan: builtin rejected; instrumented system compile and execution passed'
  exit 0
fi
[[ $# == 0 ]] || die "unexpected arguments: $*"
for flags in '-fsanitize=address -fno-sanitize=address' \
             '-fsanitize=address,undefined -fno-sanitize=all'; do
  FENG_CC_FLAGS="${flags}" "${ROOT}/build/bin/feng" "${WORK}/empty.ff" \
    --alloc=builtin "--out=${WORK}/asan-disabled"
  "${WORK}/asan-disabled/bin/empty"
done

case "$(uname -m)" in arm64|aarch64) ARCH=arm64 ;; x86_64) ARCH=x64 ;; *) die "unsupported host" ;; esac
case "$(uname -s)" in
  Darwin)
    HOST=macos-arm64
    PLATFORMS=("${HOST}")
    SDK="$(xcrun --sdk macosx --show-sdk-path)"
    FLAGS=(-isysroot "${SDK}")
    SHARED="${WORK}/liballocator_shared.dylib"
    "${LLVM}/clang" "${FLAGS[@]}" -fuse-ld=lld -dynamiclib "${FIXTURE}/shared.c" -Wl,-install_name,"${SHARED}" -o "${SHARED}"
    SHARED_FLAGS=("${SHARED}") ;;
  Linux)
    HOST="linux-${ARCH}-gnu"
    PLATFORMS=("${HOST}" "linux-${ARCH}-musl")
    SYSROOT="${ROOT}/build/toolchain/sysroot/${HOST}"
    FLAGS=("--sysroot=${SYSROOT}" "--gcc-toolchain=${SYSROOT}")
    SHARED="${WORK}/liballocator_shared.so"
    "${LLVM}/clang" "${FLAGS[@]}" -shared -fPIC "${FIXTURE}/shared.c" -o "${SHARED}"
    SHARED_FLAGS=("${SHARED}") ;;
esac

mkdir -p "${WORK}/provider/src" "${WORK}/app/src"
cp "${FIXTURE}/provider.ff" "${WORK}/provider/src/main.ff"
cp "${FIXTURE}/main.ff" "${WORK}/app/src/main.ff"
cat >"${WORK}/provider/feng.fm" <<'FM'
[package]
name: "allocator_dep"
version: "0.1.0"
target: "lib"
src: "src/"
out: "build/"
FM
cat >"${WORK}/app/feng.fm" <<'FM'
[package]
name: "allocator_app"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"
[dependencies]
allocator_dep: "../provider"
FM
for command in direct build run; do
  args=("${WORK}/empty.ff")
  if [[ "${command}" != direct ]]; then args=("${command}" "${WORK}/app"); fi
  for option in --alloc --alloc= --alloc=other --malloc=builtin --allocator=builtin; do
    if [[ "${option}" == --malloc=* || "${option}" == --allocator=* ]]; then expected='unknown option'; else expected='invalid allocator option'; fi
    reject "${expected}" "${args[@]}" "${option}"
  done
done
for allocator in system builtin; do
  reject '--alloc requires target=bin' "${WORK}/empty.ff" --target=lib "--alloc=${allocator}"
  reject '--alloc requires target=bin' build "${WORK}/provider" "--alloc=${allocator}"
  reject 'requires a target=bin project' run "${WORK}/provider" "--alloc=${allocator}"
  reject 'unknown option' pack "${WORK}/provider" "--alloc=${allocator}"
done
[[ ! -d "${WORK}/provider/build" ]] || die "invalid allocator started building a library"

for platform in "${PLATFORMS[@]}"; do
  flags=("${FLAGS[@]}")
  shared=("${SHARED_FLAGS[@]}")
  if [[ "${platform}" == *musl ]]; then
    sysroot="${ROOT}/build/toolchain/sysroot/${platform}"
    flags=("--sysroot=${sysroot}" "--gcc-toolchain=${sysroot}" "--target=$( [[ ${ARCH} == arm64 ]] && echo aarch64 || echo x86_64 )-unknown-linux-musl")
    "${LLVM}/clang" "${flags[@]}" -O2 -c "${FIXTURE}/shared.c" -o "${WORK}/shared.o"
    shared=("${WORK}/shared.o")
  fi
  "${LLVM}/clang" "${flags[@]}" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -c "${FIXTURE}/native.c" -o "${WORK}/native.o"
  feng pack "${WORK}/provider" "--platform=${platform}"
  bundle="${WORK}/provider/build/pkg/allocator_dep-0.1.0.fb"
  cp "${bundle}" "${WORK}/provider-${platform}.fb"
  bundle="${WORK}/provider-${platform}.fb"
  for release in debug release; do
    mode=("--platform=${platform}"); if [[ "${release}" == release ]]; then mode+=(--release); fi
    for lto in off thin; do
      for allocator in system builtin; do
        out="${WORK}/${platform}-${release}-${lto}-${allocator}"
        native_feng "${FIXTURE}/main.ff" "--pkg=${bundle}" "--out=${out}" --name=app \
          "${mode[@]}" "--lto=${lto}" "--alloc=${allocator}" \
          "--lib=${WORK}/native.o" "--lib=${shared[0]}"
        FENG_GC_THRESHOLD=1 "${out}/bin/app" "${allocator}"
        if [[ "${platform}" != *musl && "$(uname -s)" == Linux ]]; then
          ! ldd "${out}/bin/app" | grep -i mimalloc || die "mimalloc must not be a shared dependency"
        fi
      done
    done
  done
done

# Project builds must leave recursive lib dependencies allocator-independent.
# A native archive supplied through LIBRARY_PATH belongs only to this fixture.
# Recompile native inputs for the host after the musl matrix.
"${LLVM}/clang" "${FLAGS[@]}" -std=c11 -O2 -c "${FIXTURE}/native.c" -o "${WORK}/host-native.o"
"${LLVM}/clang" "${FLAGS[@]}" -std=c11 -O2 -c "${FIXTURE}/shared.c" -o "${WORK}/host-shared.o"
"${LLVM}/llvm-ar" rcs "${WORK}/liballocator_fixture.a" "${WORK}/host-native.o" "${WORK}/host-shared.o"
# Use the public external-library declaration for project linking.
sed 's/@cdecl("libc", "allocator_native_check")/@cdecl("allocator_fixture", "allocator_native_check")/' \
  "${FIXTURE}/main.ff" >"${WORK}/app/src/main.ff"
for allocator in builtin system builtin; do
  LIBRARY_PATH="${WORK}${LIBRARY_PATH:+:${LIBRARY_PATH}}" native_feng build "${WORK}/app" --release --alloc=builtin "--alloc=${allocator}"
  FENG_GC_THRESHOLD=1 "${WORK}/app/build/${HOST}/bin/allocator_app" "${allocator}"
  FENG_GC_THRESHOLD=1 LIBRARY_PATH="${WORK}${LIBRARY_PATH:+:${LIBRARY_PATH}}" native_feng run "${WORK}/app" "--alloc=${allocator}" -- "${allocator}"
done
# An option-looking program argument after -- must not be parsed by Feng.
FENG_GC_THRESHOLD=1 LIBRARY_PATH="${WORK}${LIBRARY_PATH:+:${LIBRARY_PATH}}" \
  native_feng run "${WORK}/app" --alloc=system -- system --alloc=other

# A moved installation locates the allocator only relative to its executable.
mkdir -p "${WORK}/install/bin" "${WORK}/install/lib/${HOST}"
cp "${ROOT}/build/bin/feng" "${WORK}/install/bin/feng"
cp -R "${ROOT}/build/include" "${WORK}/install/include"
ln -s "${ROOT}/build/toolchain" "${WORK}/install/toolchain"
cp "${ROOT}/build/lib/${HOST}/libfeng_runtime.a" "${WORK}/install/lib/${HOST}/"
cp "${ROOT}/extlib/${HOST}/libmimalloc.a" "${WORK}/install/lib/${HOST}/"
mv "${WORK}/install" "${WORK}/moved install"
compiler="${WORK}/moved install/bin/feng"
library="${WORK}/moved install/lib/${HOST}/libmimalloc.a"
for allocator in system builtin; do
  env -u FENG_CC -u FENG_CC_FLAGS "${compiler}" "${WORK}/empty.ff" --release "--alloc=${allocator}" "--out=${WORK}/moved-${allocator}"
  "${WORK}/moved-${allocator}/bin/empty"
done
rm "${library}"
env -u FENG_CC -u FENG_CC_FLAGS "${compiler}" "${WORK}/empty.ff" --release "--out=${WORK}/without-allocator"
if env -u FENG_CC -u FENG_CC_FLAGS "${compiler}" "${WORK}/empty.ff" --release --alloc=builtin "--out=${WORK}/missing" >"${WORK}/missing.log" 2>&1; then die 'missing builtin accepted'; fi
grep -F 'builtin allocator requires' "${WORK}/missing.log" >/dev/null
printf 'invalid archive\n' >"${library}"
if env -u FENG_CC -u FENG_CC_FLAGS "${compiler}" "${WORK}/empty.ff" --release --alloc=builtin "--out=${WORK}/broken" >"${WORK}/broken.log" 2>&1; then die 'broken builtin accepted'; fi

# Target validation belongs to allocator_release.sh; competing allocator
# definitions must still fail rather than bind libc silently.
"${LLVM}/clang" "${FLAGS[@]}" -O2 -c "${FIXTURE}/conflict.c" -o "${WORK}/conflict.o"
reject 'duplicate symbol' "${WORK}/empty.ff" --release --alloc=builtin "--lib=${WORK}/conflict.o" "--out=${WORK}/conflict"

# C++ uses its normal host ABI; no new/delete replacements are provided by tests.
cpp_link=(-Wl,--whole-archive "${ROOT}/extlib/${HOST}/libmimalloc.a" -Wl,--no-whole-archive -ldl)
cpp_flags=(--driver-mode=g++ -std=c++17 -O2)
if [[ "${HOST}" == macos-* ]]; then
  cpp_link=(-Xlinker -force_load -Xlinker "${ROOT}/extlib/${HOST}/libmimalloc.a")
  cpp_flags+=(-isysroot "${SDK}")
fi
"${FENG_CC:-clang}" "${cpp_flags[@]}" "${FIXTURE}/native.cpp" \
  "${cpp_link[@]}" -pthread -o "${WORK}/cpp"
"${WORK}/cpp"

# The host compiler supplies UBSan; both successful execution and a real native
# overflow diagnostic must remain effective with builtin selected by Feng.
sanitizer_cc="$(command -v clang)"
sanitizer_flags='-fsanitize=undefined -fno-sanitize-recover=all'
"${sanitizer_cc}" "${FLAGS[@]}" -O1 -fsanitize=undefined -fno-sanitize-recover=all \
  -c "${FIXTURE}/ubsan.c" -o "${WORK}/ubsan.o"
cat >"${WORK}/ubsan.ff" <<'FF'
module allocator_ubsan;
@cdecl("libc", "allocator_ubsan_check")
extern func check(fail: i32): i32;
func main(args: string[]) {
    var fail: i32 = 0;
    if args[1] == "fail" { fail = 1; }
    if check(fail) != 1 { throw "allocator/ubsan"; }
}
FF
FENG_CC="${sanitizer_cc}" FENG_CC_FLAGS="${sanitizer_flags}" "${ROOT}/build/bin/feng" \
  "${WORK}/ubsan.ff" --alloc=builtin --release "--lib=${WORK}/ubsan.o" "--out=${WORK}/ubsan"
"${WORK}/ubsan/bin/ubsan" pass
if "${WORK}/ubsan/bin/ubsan" fail >"${WORK}/ubsan.log" 2>&1; then die 'UBSan failed to diagnose signed overflow'; fi
grep -F 'signed integer overflow' "${WORK}/ubsan.log" >/dev/null || { cat "${WORK}/ubsan.log"; die 'missing UBSan diagnostic'; }

# Prove the existing language suite also works with the selected allocator.
feng run fcts/fcts_bin --alloc=builtin >"${WORK}/fcts.log" 2>&1 || { cat "${WORK}/fcts.log"; die 'builtin FCTS failed'; }
echo 'allocator: diagnostics, native ownership, GNU/musl, mode/LTO matrix, dependencies, relocation and FCTS passed'
