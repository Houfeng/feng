#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBC=""
WORK=""

# Report an invalid maintenance invocation or artifact.
die() { echo "error: $*" >&2; exit 1; }

# Remove only this invocation's temporary staging tree.
cleanup() { if [[ -n "${WORK}" ]]; then rm -rf "${WORK}"; fi; }
trap cleanup EXIT

for arg in "$@"; do
  case "${arg}" in
    --libc=gnu|--libc=musl)
      [[ -z "${LIBC}" ]] || die "--libc may only be specified once"
      LIBC="${arg#--libc=}" ;;
    -h|--help)
      echo "Usage: scripts/build_mimalloc.sh [--libc=gnu|musl]"
      echo "macOS builds macos-arm64; Linux requires --libc and builds the current architecture."
      exit 0 ;;
    *) die "unknown argument: ${arg}" ;;
  esac
done

case "$(uname -m)" in
  arm64|aarch64) ARCH=arm64; CPU='ARM aarch64'; TRIPLE_CPU=aarch64 ;;
  x86_64|amd64) ARCH=x64; CPU=x86-64; TRIPLE_CPU=x86_64 ;;
  *) die "unsupported host architecture: $(uname -m)" ;;
esac

FLAGS=(-std=c11 -O3 -DNDEBUG -DMI_MALLOC_OVERRIDE -fPIC)
case "$(uname -s)" in
  Darwin)
    [[ -z "${LIBC}" && "${ARCH}" == arm64 ]] || die "macOS supports only arm64 without --libc"
    PLATFORM=macos-arm64
    HOST="${PLATFORM}"
    SDK="$(xcrun --sdk macosx --show-sdk-path)"
    [[ -d "${SDK}" ]] || die "macOS SDK not found"
    FLAGS+=(--target=arm64-apple-macosx -isysroot "${SDK}" -mmacosx-version-min=26.0 -DMI_OSX_ZONE=1)
    FORMAT='Mach-O 64-bit object arm64' ;;
  Linux)
    [[ -n "${LIBC}" ]] || die "Linux builds require --libc=gnu or --libc=musl"
    PLATFORM="linux-${ARCH}-${LIBC}"
    HOST="linux-${ARCH}-gnu"
    SYSROOT="${ROOT}/toolchain/sysroot/${PLATFORM}"
    [[ -d "${SYSROOT}/usr/include" ]] || die "target sysroot not found: ${SYSROOT}"
    FLAGS+=("--target=${TRIPLE_CPU}-unknown-linux-${LIBC}" "--sysroot=${SYSROOT}" "--gcc-toolchain=${SYSROOT}")
    FORMAT='ELF 64-bit LSB relocatable' ;;
  *) die "unsupported host OS: $(uname -s)" ;;
esac

LLVM="${ROOT}/toolchain/llvm/${HOST}/bin"
for tool in clang llvm-ar; do
  [[ -x "${LLVM}/${tool}" ]] || die "required executable not found: ${LLVM}/${tool}"
done
SOURCE="${ROOT}/third_party/mimalloc"
command -v file >/dev/null 2>&1 || die "required command not found: file"
[[ -f "${SOURCE}/src/static.c" && -f "${SOURCE}/LICENSE" ]] || die "mimalloc source or license is missing"
mkdir -p "${ROOT}/temp"
WORK="$(mktemp -d "${ROOT}/temp/mimalloc-${PLATFORM}.XXXXXX")"
echo "==> Building native mimalloc for ${PLATFORM}"
"${LLVM}/clang" "${FLAGS[@]}" -I"${SOURCE}/include" -c "${SOURCE}/src/static.c" -o "${WORK}/mimalloc.o"
ACTUAL="$(file -b "${WORK}/mimalloc.o")"
[[ "${ACTUAL}" == *"${FORMAT}"* ]] || die "unexpected object format: ${ACTUAL}"
if [[ "$(uname -s)" == Linux ]]; then
  [[ "${ACTUAL}" == *"${CPU}"* ]] || die "unexpected object architecture: ${ACTUAL}"
fi
"${LLVM}/llvm-ar" rcs "${WORK}/libmimalloc.a" "${WORK}/mimalloc.o"
[[ "$("${LLVM}/llvm-ar" t "${WORK}/libmimalloc.a")" == mimalloc.o ]] || die "unexpected archive members"
mkdir -p "${ROOT}/extlib/${PLATFORM}"
mv "${WORK}/libmimalloc.a" "${ROOT}/extlib/${PLATFORM}/libmimalloc.a"
echo "==> Built and validated extlib/${PLATFORM}/libmimalloc.a"
