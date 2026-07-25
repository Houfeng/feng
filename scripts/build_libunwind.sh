#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/libunwind"
MAKE_BIN="${MAKE:-make}"
LIBC=""
BUILD_ROOT=""

# Print the supported native build invocations.
usage() {
  cat <<'EOF'
Usage:
  scripts/build_libunwind.sh
  scripts/build_libunwind.sh --libc=gnu|musl

macOS builds macos-arm64 and does not accept --libc.
Linux builds the current CPU architecture and requires --libc=gnu|musl.
EOF
}

# Report a fatal build error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Require a command that is resolved through PATH.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

# Require an executable at a fixed repository-relative path.
require_executable() {
  [[ -x "$1" ]] || die "required executable not found: $1"
}

# Remove only the staging directory created by this invocation.
cleanup() {
  if [[ -n "${BUILD_ROOT}" && -d "${BUILD_ROOT}" ]]; then
    rm -rf "${BUILD_ROOT}"
  fi
}

trap cleanup EXIT

for arg in "$@"; do
  case "${arg}" in
    --libc=gnu|--libc=musl)
      [[ -z "${LIBC}" ]] || die "--libc may only be specified once"
      LIBC="${arg#--libc=}"
      ;;
    --libc=*)
      die "unsupported Linux libc: ${arg#--libc=}; expected gnu or musl"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: ${arg}"
      ;;
  esac
done

case "$(uname -m)" in
  arm64|aarch64) ARCH="arm64" ;;
  x86_64|amd64)  ARCH="x64" ;;
  *) die "unsupported host architecture: $(uname -m)" ;;
esac

case "$(uname -s)" in
  Darwin)
    [[ -z "${LIBC}" ]] ||
      die "--libc is only valid on Linux"
    [[ "${ARCH}" == "arm64" ]] ||
      die "unsupported macOS build environment: macos-${ARCH}"

    PLATFORM="macos-arm64"
    HOST_PLATFORM="${PLATFORM}"
    TARGET_TRIPLE="arm64-apple-macosx"
    EXPECTED_OBJECT_FORMAT="Mach-O 64-bit object arm64"

    require_cmd xcrun
    SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)" ||
      die "failed to locate the macOS SDK with xcrun"
    [[ -d "${SDK_PATH}" ]] ||
      die "macOS SDK directory not found: ${SDK_PATH}"
    TARGET_FLAGS="--target=${TARGET_TRIPLE} -isysroot ${SDK_PATH}"
    ;;
  Linux)
    [[ -n "${LIBC}" ]] ||
      die "Linux builds require --libc=gnu or --libc=musl"

    PLATFORM="linux-${ARCH}-${LIBC}"
    HOST_PLATFORM="linux-${ARCH}-gnu"
    EXPECTED_OBJECT_FORMAT="ELF 64-bit LSB relocatable"
    case "${ARCH}" in
      x64)
        TARGET_TRIPLE="x86_64-unknown-linux-${LIBC}"
        EXPECTED_CPU_FORMAT="x86-64"
        ;;
      arm64)
        TARGET_TRIPLE="aarch64-unknown-linux-${LIBC}"
        EXPECTED_CPU_FORMAT="ARM aarch64"
        ;;
    esac

    SYSROOT="${PROJECT_ROOT}/toolchain/sysroot/${PLATFORM}"
    [[ -d "${SYSROOT}/usr/include" ]] ||
      die "target sysroot include directory not found: ${SYSROOT}/usr/include"
    [[ -d "${SYSROOT}/usr/lib" ]] ||
      die "target sysroot library directory not found: ${SYSROOT}/usr/lib"
    TARGET_FLAGS="--target=${TARGET_TRIPLE} --sysroot=${SYSROOT} --gcc-toolchain=${SYSROOT}"
    ;;
  *)
    die "unsupported host OS: $(uname -s)"
    ;;
esac

[[ -d "${TARGET_DIR}" ]] || {
  echo "error: ${TARGET_DIR} does not exist" >&2
  die "run ${PROJECT_ROOT}/scripts/fetch_libunwind.sh first"
}
[[ -f "${TARGET_DIR}/Makefile" ]] || {
  echo "error: ${TARGET_DIR}/Makefile is missing" >&2
  die "run ${PROJECT_ROOT}/scripts/fetch_libunwind.sh again to regenerate the vendored build files"
}

LLVM_BIN="${PROJECT_ROOT}/toolchain/llvm/${HOST_PLATFORM}/bin"
CLANG="${LLVM_BIN}/clang"
LLVM_AR="${LLVM_BIN}/llvm-ar"
require_executable "${CLANG}"
require_executable "${LLVM_AR}"
require_cmd "${MAKE_BIN}"
require_cmd file

mkdir -p "${PROJECT_ROOT}/temp"
BUILD_ROOT="$(mktemp -d "${PROJECT_ROOT}/temp/libunwind-${PLATFORM}.XXXXXX")"
STAGING_OUTPUT="${BUILD_ROOT}/output"
EXTRACTED_OBJECTS="${BUILD_ROOT}/objects"
STAGED_ARCHIVE="${STAGING_OUTPUT}/libfeng_unwind.a"
OUTPUT_DIR="${PROJECT_ROOT}/extlib/${PLATFORM}"
OUTPUT_ARCHIVE="${OUTPUT_DIR}/libfeng_unwind.a"
mkdir -p "${STAGING_OUTPUT}" "${EXTRACTED_OBJECTS}"

MAKE_ARGS=(
  -C "${TARGET_DIR}"
  "CC=${CLANG}"
  "CXX=${CLANG}"
  "AR=${LLVM_AR}"
  "CFLAGS=-O2 -Wall -Wextra -fPIC ${TARGET_FLAGS}"
  "OUTPUT_DIR=${STAGING_OUTPUT}"
)

# The vendored Makefile builds in its source directory. Clean first so a
# previous platform's objects can never be reused for the current archive.
"${MAKE_BIN}" "${MAKE_ARGS[@]}" clean >/dev/null

echo "==> Building libunwind for ${PLATFORM}"
"${MAKE_BIN}" "${MAKE_ARGS[@]}" install
[[ -f "${STAGED_ARCHIVE}" ]] ||
  die "libunwind build did not produce ${STAGED_ARCHIVE}"

ARCHIVE_FORMAT="$(file -b "${STAGED_ARCHIVE}")"
[[ "${ARCHIVE_FORMAT}" == *"archive"* ]] ||
  die "unexpected archive format for ${PLATFORM}: ${ARCHIVE_FORMAT}"

(
  cd "${EXTRACTED_OBJECTS}"
  "${LLVM_AR}" x "${STAGED_ARCHIVE}"
)

OBJECT_COUNT=0
while IFS= read -r -d '' object_path; do
  OBJECT_COUNT=$((OBJECT_COUNT + 1))
  OBJECT_FORMAT="$(file -b "${object_path}")"
  [[ "${OBJECT_FORMAT}" == *"${EXPECTED_OBJECT_FORMAT}"* ]] ||
    die "unexpected object format in ${STAGED_ARCHIVE}: ${OBJECT_FORMAT}"
  if [[ "$(uname -s)" == "Linux" ]]; then
    [[ "${OBJECT_FORMAT}" == *"${EXPECTED_CPU_FORMAT}"* ]] ||
      die "unexpected CPU architecture in ${STAGED_ARCHIVE}: ${OBJECT_FORMAT}"
  fi
done < <(find "${EXTRACTED_OBJECTS}" -type f -name '*.o' -print0)

[[ "${OBJECT_COUNT}" -gt 0 ]] ||
  die "archive contains no object files: ${STAGED_ARCHIVE}"

mkdir -p "${OUTPUT_DIR}"
mv "${STAGED_ARCHIVE}" "${OUTPUT_ARCHIVE}"
echo "==> Built and validated ${OUTPUT_ARCHIVE}"
