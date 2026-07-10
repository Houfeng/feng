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
#     lib/clang/<version>/     — builtin headers + target runtime (libclang_rt.*)
#     share/clang/             — optional helper scripts
#     LICENSE.TXT              — upstream license
#     README.md                — provenance and re-sync instructions
#
# Deliberately excluded: clang++/clang-cl/llvm-*/lld, LLVM dev libs/headers,
# C++ standard library material (host SDK provides).

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
mkdir -p "${CLANG_TARGET_DIR}/lib/clang"
cp -R "${CLANG_RESOURCE_DIR}" "${CLANG_TARGET_DIR}/lib/clang/"

if [[ -d "${LLVM_EXTRACTED_ROOT}/share/clang" ]]; then
  mkdir -p "${CLANG_TARGET_DIR}/share"
  cp -R "${LLVM_EXTRACTED_ROOT}/share/clang" "${CLANG_TARGET_DIR}/share/"
fi

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
- \`lib/clang/${CLANG_RESOURCE_VERSION}/\` — builtin headers (stdint.h, stddef.h, ...)
  and target runtime (libclang_rt.*.a)
- \`share/clang/\` — clang helper scripts (if present)
- \`LICENSE.TXT\` — upstream license

Deliberately excluded:
- other \`bin/\` tools (clang++, clang-cl, llvm-*, lld, ...)
- LLVM dev libraries and headers (clang is invoked as a standalone
  command-line compiler; no \`-lLLVM\` link needed)
- C++ standard library headers and runtime (host SDK provides)

The feng compiler locates this directory by its own executable position.

Re-sync:
- \`LLVM_VERSION=<ver> ./scripts/fetch_llvm.sh && ./scripts/trim_clang.sh\` to switch upstream version.
- \`TARGET=<os>-<arch> ./scripts/fetch_llvm.sh && ./scripts/trim_clang.sh\` to override host detection.
EOF

echo "==> Done. clang tree at ${CLANG_TARGET_DIR}"
echo "==> Verify: ${CLANG_TARGET_DIR}/bin/clang --version"
