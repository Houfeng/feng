#!/usr/bin/env bash
set -euo pipefail

# Slim the extracted LLVM prebuilt into toolchain/clang/<os>-<arch>/.
#
# Consumes the LLVM root produced by fetch_llvm.sh. Re-runs overwrite the
# target directory safely — only this tool's dir is touched, other tools'
# trees under toolchain/ are preserved.
#
# Layout produced:
#   toolchain/clang/<os>-<arch>/
#     bin/clang                — the executable
#     lib/clang/<version>/     — builtin headers + target runtime (libclang_rt.osx.a)
#     LICENSE.TXT              — upstream license
#     README.md                — provenance and re-sync instructions
#
# Deliberately excluded:
# - clang++/clang-cl/llvm-*/lld (other LLVM tools)
# - LLVM dev libs/headers (clang invoked as standalone compiler)
# - C++ standard library material (host SDK provides)
# - share/clang/ (editor integration scripts: clang-format, clang-tidy, etc.)
# - libclang_rt.* except libclang_rt.osx.a (macOS) — sanitizers, fuzzer, xray, profile, etc.
# - Non-target builtin headers (x86 intrinsics, CUDA, OpenMP, z/OS, PPC, etc.)
#
# Feng's codegen produces C code that only needs standard C headers (stdint.h,
# stddef.h, etc.) and the basic runtime library. Sanitizer libraries are not
# needed here because Feng uses the system compiler ($CC) for sanitization
# during development, not the bundled clang. On Linux the system crt
# provides the runtime; the bundled clang does not ship a libclang_rt for
# Linux targets.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LLVM_VERSION="${LLVM_VERSION:-22.1.8}"

detect_target() {
  local os arch uname_s uname_m
  uname_s="$(uname -s)"
  uname_m="$(uname -m)"
  case "${uname_s}" in
    Darwin) os=macos ;;
    Linux)  os=linux ;;
    *) echo "error: unsupported OS for host detection: ${uname_s}" >&2; exit 1 ;;
  esac
  case "${uname_m}" in
    arm64|aarch64) arch=arm64 ;;
    x86_64|amd64)  arch=x64 ;;
    *) echo "error: unsupported arch for host detection: ${uname_m}" >&2; exit 1 ;;
  esac
  printf '%s-%s' "${os}" "${arch}"
}

TARGET="${TARGET:-$(detect_target)}"

target_to_llvm_asset_fragment() {
  case "$1" in
    macos-arm64) printf 'macOS-ARM64' ;;
    linux-arm64) printf 'Linux-ARM64' ;;
    linux-x64)   printf 'Linux-X64' ;;
    *) echo "error: no LLVM prebuilt asset mapping for target: $1" >&2; exit 1 ;;
  esac
}

LLVM_ASSET_FRAGMENT="$(target_to_llvm_asset_fragment "${TARGET}")"
LLVM_ASSET_BASENAME="LLVM-${LLVM_VERSION}-${LLVM_ASSET_FRAGMENT}"

DEFAULT_LLVM_ROOT="${PROJECT_ROOT}/temp/llvm/${LLVM_ASSET_BASENAME}"
LLVM_EXTRACTED_ROOT="${LLVM_EXTRACTED_ROOT:-${DEFAULT_LLVM_ROOT}}"

if [[ ! -d "${LLVM_EXTRACTED_ROOT}" ]]; then
  echo "error: LLVM extracted root not found: ${LLVM_EXTRACTED_ROOT}" >&2
  echo "hint:  run scripts/fetch_llvm.sh first, or set LLVM_EXTRACTED_ROOT=<path>" >&2
  exit 1
fi

CLANG_TARGET_DIR="${PROJECT_ROOT}/toolchain/clang/${TARGET}"

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd find
require_cmd cp
require_cmd chmod

echo "==> Slimming clang from ${LLVM_EXTRACTED_ROOT}"
echo "==> Target: ${CLANG_TARGET_DIR}"

if [[ ! -f "${LLVM_EXTRACTED_ROOT}/bin/clang" ]]; then
  echo "error: bin/clang not found in extracted archive" >&2
  exit 1
fi

# Only delete this tool's target dir — leave lldb/ and other tools alone.
rm -rf "${CLANG_TARGET_DIR}"
mkdir -p "${CLANG_TARGET_DIR}/bin"
cp "${LLVM_EXTRACTED_ROOT}/bin/clang" "${CLANG_TARGET_DIR}/bin/clang"
chmod 0755 "${CLANG_TARGET_DIR}/bin/clang"

CLANG_RESOURCE_DIR="$(find "${LLVM_EXTRACTED_ROOT}/lib/clang" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -n 1)"
if [[ -z "${CLANG_RESOURCE_DIR}" ]]; then
  echo "error: clang resource dir not found (lib/clang/<version>/)" >&2
  exit 1
fi
CLANG_RESOURCE_VERSION="$(basename "${CLANG_RESOURCE_DIR}")"

# --- Copy builtin headers (selective) ---
# Feng's codegen produces C code that uses standard C headers (stdint.h,
# stddef.h, float.h, stdarg.h, stdbool.h, etc.) and ARM64 NEON/SVE intrinsics
# on macOS. Non-target headers (x86 intrinsics, CUDA, OpenMP, z/OS, PPC,
# sanitizer/fuzzer/xray/profile/orc headers) are excluded to save ~12 MB.
mkdir -p "${CLANG_TARGET_DIR}/lib/clang/${CLANG_RESOURCE_VERSION}/include"

# Directories to exclude from builtin headers (non-target platforms/tools).
EXCLUDE_HEADER_DIRS=(
  cuda_wrappers
  openmp_wrappers
  llvm_libc_wrappers
  llvm_offload_wrappers
  zos_wrappers
  ppc_wrappers
  orc
  sanitizer
  fuzzer
  xray
  profile
)

# Build find exclusion arguments.
FIND_EXCLUDES=()
for d in "${EXCLUDE_HEADER_DIRS[@]}"; do
  FIND_EXCLUDES+=(-not -name "${d}" -not -path "*/${d}/*")
done

# Copy header files/dirs, excluding non-target content.
(cd "${CLANG_RESOURCE_DIR}/include" && \
  find . -mindepth 1 -maxdepth 1 \
    "${FIND_EXCLUDES[@]}" \
    -print0 | while IFS= read -r -d '' entry; do
    if [[ -d "${entry}" ]]; then
      cp -R "${entry}" "${CLANG_TARGET_DIR}/lib/clang/${CLANG_RESOURCE_VERSION}/include/"
    else
      cp "${entry}" "${CLANG_TARGET_DIR}/lib/clang/${CLANG_RESOURCE_VERSION}/include/"
    fi
  done)

# --- Copy target runtime library (selective) ---
# macOS: Feng only needs libclang_rt.osx.a (basic runtime, ~228 KB).
# Linux: no bundled runtime lib needed — Linux uses system crt; the LLVM
# prebuilt does not ship a libclang_rt for Linux targets.
# Sanitizer/fuzzer/xray/profile/ORC libraries (~28 MB) are excluded because
# Feng uses $CC (system compiler) for sanitization during development,
# not the bundled clang.
TARGET_OS_LIB=""
case "${TARGET}" in
  macos-*)  TARGET_OS_LIB="libclang_rt.osx.a" ;;
  linux-*)  TARGET_OS_LIB="" ;;
  *) echo "warning: unknown target for runtime lib selection: ${TARGET}" >&2 ;;
esac

# Determine the platform-specific sub-directory under lib/ (darwin/ on macOS).
RT_LIB_DIR=""
if [[ -d "${CLANG_RESOURCE_DIR}/lib/darwin" ]]; then
  RT_LIB_DIR="darwin"
elif [[ -d "${CLANG_RESOURCE_DIR}/lib/linux" ]]; then
  RT_LIB_DIR="linux"
fi

if [[ -n "${RT_LIB_DIR}" && -n "${TARGET_OS_LIB}" ]]; then
  mkdir -p "${CLANG_TARGET_DIR}/lib/clang/${CLANG_RESOURCE_VERSION}/lib/${RT_LIB_DIR}"
  SRC_RT="${CLANG_RESOURCE_DIR}/lib/${RT_LIB_DIR}/${TARGET_OS_LIB}"
  if [[ -f "${SRC_RT}" ]]; then
    cp "${SRC_RT}" "${CLANG_TARGET_DIR}/lib/clang/${CLANG_RESOURCE_VERSION}/lib/${RT_LIB_DIR}/"
  else
    echo "warning: ${TARGET_OS_LIB} not found in upstream — bundled clang may fail at link time" >&2
  fi
fi

# share/clang/ deliberately excluded (clang-format, clang-tidy editor
# integration scripts — not needed by Feng's compilation pipeline).

if [[ -f "${LLVM_EXTRACTED_ROOT}/LICENSE.TXT" ]]; then
  cp "${LLVM_EXTRACTED_ROOT}/LICENSE.TXT" "${CLANG_TARGET_DIR}/LICENSE.TXT"
fi

cat > "${CLANG_TARGET_DIR}/README.md" <<EOF
# LLVM clang minimal binary closure

This directory vendors a slimmed LLVM clang binary for Feng's bundled toolchain.

Version: ${LLVM_VERSION}
Target:  ${TARGET}
Source:  https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-${LLVM_ASSET_FRAGMENT}.tar.xz

Included:
- \`bin/clang\` — the executable
- \`lib/clang/${CLANG_RESOURCE_VERSION}/include/\` — standard C builtin headers
  (stdint.h, stddef.h, float.h, stdarg.h, stdbool.h, arm_neon.h, ...)
- \`lib/clang/${CLANG_RESOURCE_VERSION}/lib/${RT_LIB_DIR}/${TARGET_OS_LIB:-<none>}\` — basic
  target runtime library (macOS only; Linux uses system crt)
- \`LICENSE.TXT\` — upstream license

Deliberately excluded:
- other \`bin/\` tools (clang++, clang-cl, llvm-*, lld, ...)
- LLVM dev libraries and headers
- C++ standard library headers and runtime (host SDK provides)
- \`share/clang/\` (clang-format, clang-tidy editor integration scripts)
- Sanitizer libraries (ASan, UBSan, TSan, MSan, ...) — Feng uses \$CC
  (system compiler) for sanitization during development
- Fuzzer libraries (libFuzzer)
- XRay / Profile / Stats / ORC runtime libraries
- Non-target platform headers (x86 intrinsics, CUDA, OpenMP, z/OS,
  PPC wrappers, LLVM libc/offload wrappers)

The feng compiler locates this directory by its own executable position.

Re-sync:
- \`LLVM_VERSION=<ver> ./scripts/fetch_llvm.sh && ./scripts/trim_clang.sh\` to switch upstream version.
- \`TARGET=<os>-<arch> ./scripts/fetch_llvm.sh && ./scripts/trim_clang.sh\` to override host detection.
EOF

echo "==> Done. clang tree at ${CLANG_TARGET_DIR}"
echo "==> Verify: ${CLANG_TARGET_DIR}/bin/clang --version"
