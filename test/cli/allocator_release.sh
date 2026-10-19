#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
mkdir -p temp
WORK="$(mktemp -d "${ROOT}/temp/allocator-release.XXXXXX")"
LLVM="${ROOT}/build/toolchain/llvm/bin"

# Remove only this fixture's isolated filesystem tree.
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT
# Report a failed distribution or maintenance invariant.
die() { echo "allocator release test: $*" >&2; exit 1; }

case "$(uname -m)" in arm64|aarch64) ARCH=arm64 ;; x86_64) ARCH=x64 ;; esac
case "$(uname -s)" in
  Darwin) HOST=macos-arm64; PLATFORMS=(macos-arm64) ;;
  Linux) HOST="linux-${ARCH}-gnu"; PLATFORMS=("${HOST}" "linux-${ARCH}-musl") ;;
esac
INPUT="${WORK}/input"
mkdir -p "${INPUT}/bin" "${INPUT}/include"
cp "${ROOT}/build/bin/feng" "${INPUT}/bin/"
cp "${ROOT}/build/include/"* "${INPUT}/include/"
ln -s "${ROOT}/build/toolchain" "${INPUT}/toolchain"
for platform in "${PLATFORMS[@]}"; do
  mkdir -p "${INPUT}/lib/${platform}"
  cp "${ROOT}/build/lib/${platform}/libfeng_runtime.a" "${INPUT}/lib/${platform}/"
  ln -s "${ROOT}/extlib/${platform}/libmimalloc.a" "${INPUT}/lib/${platform}/libmimalloc.a"
done
"${ROOT}/scripts/release_component.sh" "--platform=${HOST}" "--build-root=${INPUT}" "--output=${WORK}/component" >/dev/null
for platform in "${PLATFORMS[@]}"; do
  artifact="${WORK}/component/${HOST}/lib/${platform}/libmimalloc.a"
  [[ -f "${artifact}" && ! -L "${artifact}" ]] || die 'component leaked a development symlink'
  cmp "${artifact}" "${ROOT}/extlib/${platform}/libmimalloc.a"
  grep -F "lib/${platform}/libmimalloc.a" "${WORK}/component/${HOST}/SHA256SUMS" >/dev/null
  "${LLVM}/llvm-ar" p "${artifact}" mimalloc.o >"${WORK}/member.o"
  [[ "$(file -b "${WORK}/member.o")" != *LLVM*bitcode* ]] || die 'allocator unexpectedly contains bitcode'
done
cmp "${WORK}/component/${HOST}/licenses/mimalloc/LICENSE" "${ROOT}/third_party/mimalloc/LICENSE"
grep -F 'licenses/mimalloc/LICENSE' "${WORK}/component/${HOST}/SHA256SUMS" >/dev/null

# Invalid component inputs fail before publishing any component directory.
reject_component() {
  local name="$1"
  if "${ROOT}/scripts/release_component.sh" "--platform=${HOST}" "--build-root=${INPUT}" "--output=${WORK}/bad-${name}" >"${WORK}/${name}.log" 2>&1; then
    die "invalid ${name} allocator was published"
  fi
  [[ ! -e "${WORK}/bad-${name}/${HOST}" ]] || die 'failed staging published a component'
}
archive="${INPUT}/lib/${HOST}/libmimalloc.a"
rm "${archive}"
reject_component missing
printf 'broken\n' >"${archive}"
reject_component corrupt
rm "${archive}"
"${LLVM}/llvm-ar" rcs "${archive}"
reject_component empty
if [[ "${HOST}" == macos-* ]]; then wrong=linux-arm64-gnu; else wrong=macos-arm64; fi
cp "${ROOT}/extlib/${wrong}/libmimalloc.a" "${archive}"
reject_component wrong-target
printf 'int allocator_fixture(void) { return 1; }\n' >"${WORK}/source.c"
"${LLVM}/clang" -flto=thin -c "${WORK}/source.c" -o "${WORK}/bitcode.o"
rm "${archive}"
"${LLVM}/llvm-ar" rcs "${archive}" "${WORK}/bitcode.o"
reject_component bitcode
grep -F 'allocator archive must contain native objects' "${WORK}/bitcode.log" >/dev/null

# Exercise development links without writing to the suite's live installation.
LAYOUT="${WORK}/layout"
mkdir -p "${LAYOUT}/extlib/${HOST}" "${LAYOUT}/src/cli"
cp "${ROOT}/extlib/${HOST}/libmimalloc.a" "${LAYOUT}/extlib/${HOST}/"
cp "${ROOT}/VERSION" "${LAYOUT}/"
"${MAKE:-make}" -s -C "${LAYOUT}" -f "${ROOT}/Makefile" allocator-layout
link="${LAYOUT}/build/lib/${HOST}/libmimalloc.a"
[[ "$(readlink "${link}")" == "../../../extlib/${HOST}/libmimalloc.a" && -f "${link}" ]] || die 'incorrect relative link'
identity="$(ls -di "${link}")"
"${MAKE:-make}" -s -C "${LAYOUT}" -f "${ROOT}/Makefile" allocator-layout
[[ "$(ls -di "${link}")" == "${identity}" ]] || die 'no-op make rewrote the link'
ln -sfn wrong "${link}"
"${MAKE:-make}" -s -C "${LAYOUT}" -f "${ROOT}/Makefile" allocator-layout
[[ -f "${link}" ]] || die 'make did not repair a stale link'
rm "${link}"
printf 'preserve this regular file\n' >"${link}"
if "${MAKE:-make}" -s -C "${LAYOUT}" -f "${ROOT}/Makefile" allocator-layout >"${WORK}/layout.log" 2>&1; then die 'make overwrote a regular file'; fi
grep -F 'preserve this regular file' "${link}" >/dev/null
"${MAKE:-make}" -s -C "${LAYOUT}" -f "${ROOT}/Makefile" clean
cmp "${LAYOUT}/extlib/${HOST}/libmimalloc.a" "${ROOT}/extlib/${HOST}/libmimalloc.a"

# With all target archives available, each host links only its runtime targets.
check_layout_platforms() {
  local host="$1" kernel="$2" machine="$3"
  shift 3
  local layout="${WORK}/layout-${host}" platform link
  mkdir -p "${layout}/src/cli"
  cp "${ROOT}/VERSION" "${layout}/"
  ln -s "${ROOT}/extlib" "${layout}/extlib"
  for platform in macos-arm64 linux-x64-gnu linux-x64-musl linux-arm64-gnu linux-arm64-musl; do
    [[ -f "${layout}/extlib/${platform}/libmimalloc.a" ]] || die "missing layout fixture: ${platform}"
  done
  "${MAKE:-make}" -s -C "${layout}" -f "${ROOT}/Makefile" \
    "_UNAME_S=${kernel}" "_UNAME_M=${machine}" allocator-layout
  for platform in "$@"; do
    link="${layout}/build/lib/${platform}/libmimalloc.a"
    [[ -L "${link}" && -f "${link}" && "$(readlink "${link}")" == "../../../extlib/${platform}/libmimalloc.a" ]] ||
      die "incorrect ${host} layout for ${platform}"
  done
  [[ "$(find "${layout}/build/lib" -name libmimalloc.a | wc -l | tr -d ' ')" == "$#" ]] ||
    die "${host} layout includes targets outside its runtime platforms"
}
check_layout_platforms macos-arm64 Darwin arm64 macos-arm64
check_layout_platforms linux-x64-gnu Linux x86_64 linux-x64-gnu linux-x64-musl
check_layout_platforms linux-arm64-gnu Linux aarch64 linux-arm64-gnu linux-arm64-musl

# A failing native maintenance build must preserve the previous archive.
mkdir -p "${LAYOUT}/scripts" "${LAYOUT}/third_party/mimalloc/src" "${LAYOUT}/toolchain/llvm/${HOST}/bin"
cp "${ROOT}/scripts/build_mimalloc.sh" "${LAYOUT}/scripts/"
"${LAYOUT}/scripts/build_mimalloc.sh" --help >/dev/null
cp "${ROOT}/third_party/mimalloc/LICENSE" "${LAYOUT}/third_party/mimalloc/"
cp "${ROOT}/third_party/mimalloc/src/static.c" "${LAYOUT}/third_party/mimalloc/src/"
for tool in clang llvm-ar; do
  printf '#!/bin/sh\nexit 42\n' >"${LAYOUT}/toolchain/llvm/${HOST}/bin/${tool}"
  chmod +x "${LAYOUT}/toolchain/llvm/${HOST}/bin/${tool}"
done
# Keep the command array nonempty for macOS Bash 3.2 with nounset enabled.
maintenance_command=("${LAYOUT}/scripts/build_mimalloc.sh")
if [[ "${HOST}" == linux-* ]]; then
  mkdir -p "${LAYOUT}/toolchain/sysroot/${HOST}/usr/include"
  maintenance_command+=(--libc=gnu)
fi
if "${maintenance_command[@]}" >"${WORK}/maintenance.log" 2>&1; then die 'failing maintenance compiler unexpectedly succeeded'; fi
cmp "${LAYOUT}/extlib/${HOST}/libmimalloc.a" "${ROOT}/extlib/${HOST}/libmimalloc.a"
[[ -z "$(find "${LAYOUT}/temp" -mindepth 1 -print -quit)" ]] || die 'maintenance left temporary staging files'
echo 'allocator release: native format, architecture, manifest, license, real files, layout and atomic maintenance passed'
