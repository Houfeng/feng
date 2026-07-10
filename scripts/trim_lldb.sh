#!/usr/bin/env bash
set -euo pipefail

# Slim the extracted LLVM prebuilt into toolchain/lldb/<os>-<arch>/.
#
# Consumes the LLVM root produced by fetch_llvm.sh. Re-runs overwrite the
# target directory safely — only this tool's dir is touched, other tools'
# trees under toolchain/ are preserved.
#
# Layout produced:
#   toolchain/lldb/<os>-<arch>/
#     bin/lldb, bin/lldb-dap   — the executables
#     lib/liblldb.dylib        — LLDB shared library (hard dep on macOS)
#     lib/Python3.framework/   — Python runtime, hard-linked by liblldb
#     lib/lldb/                — LLDB python plug-ins and support modules
#     share/lldb/             — python scripts, formatters, settings
#     LICENSE.TXT             — upstream license
#     README.md                — provenance and re-sync instructions
#
# Deliberately excluded: lldb-server, lldb-mi, lldb-argdumper (server-side /
# MI helpers not needed for client-side DAP), include/lldb/ (C++ API).

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

LLDB_TARGET_DIR="${PROJECT_ROOT}/toolchain/lldb/${TARGET}"

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd cp
require_cmd chmod

echo "==> Slimming lldb from ${LLVM_EXTRACTED_ROOT}"
echo "==> Target: ${LLDB_TARGET_DIR}"

if [[ ! -f "${LLVM_EXTRACTED_ROOT}/bin/lldb" ]]; then
  echo "error: bin/lldb not found in extracted archive" >&2
  exit 1
fi
if [[ ! -f "${LLVM_EXTRACTED_ROOT}/bin/lldb-dap" ]]; then
  echo "error: bin/lldb-dap not found in extracted archive" >&2
  exit 1
fi

# Only delete this tool's target dir — leave clang/ and other tools alone.
rm -rf "${LLDB_TARGET_DIR}"
mkdir -p "${LLDB_TARGET_DIR}/bin"
cp "${LLVM_EXTRACTED_ROOT}/bin/lldb" "${LLDB_TARGET_DIR}/bin/lldb"
cp "${LLVM_EXTRACTED_ROOT}/bin/lldb-dap" "${LLDB_TARGET_DIR}/bin/lldb-dap"
chmod 0755 "${LLDB_TARGET_DIR}/bin/lldb" "${LLDB_TARGET_DIR}/bin/lldb-dap"

# liblldb.dylib — hard dependency on macOS. bin/lldb and bin/lldb-dap both
# link to it via @rpath; copy the dylib so the link resolves.
if [[ -f "${LLVM_EXTRACTED_ROOT}/lib/liblldb.dylib" ]]; then
  mkdir -p "${LLDB_TARGET_DIR}/lib"
  cp "${LLVM_EXTRACTED_ROOT}/lib/liblldb.dylib" "${LLDB_TARGET_DIR}/lib/liblldb.dylib"
else
  echo "warning: lib/liblldb.dylib not found — lldb may fail to start" >&2
fi

# lib/Python3.framework — Python runtime hard-linked by liblldb. Without
# it, lldb fails to load at startup regardless of whether scripting is
# used. Bundle the whole framework so the hard-link resolves.
if [[ -d "${LLVM_EXTRACTED_ROOT}/lib/Python3.framework" ]]; then
  mkdir -p "${LLDB_TARGET_DIR}/lib"
  cp -R "${LLVM_EXTRACTED_ROOT}/lib/Python3.framework" "${LLDB_TARGET_DIR}/lib/"
else
  echo "warning: lib/Python3.framework not found — see §9 Python dependency decision" >&2
fi

# lib/lldb/ — LLDB's own python plug-ins / support modules. Needed for
# pretty-printers and dynamic type synthesis in the DAP adapter.
if [[ -d "${LLVM_EXTRACTED_ROOT}/lib/lldb" ]]; then
  mkdir -p "${LLDB_TARGET_DIR}/lib"
  cp -R "${LLVM_EXTRACTED_ROOT}/lib/lldb" "${LLDB_TARGET_DIR}/lib/"
fi

if [[ -d "${LLVM_EXTRACTED_ROOT}/share/lldb" ]]; then
  mkdir -p "${LLDB_TARGET_DIR}/share"
  cp -R "${LLVM_EXTRACTED_ROOT}/share/lldb" "${LLDB_TARGET_DIR}/share/"
fi

cp "${LLVM_EXTRACTED_ROOT}/LICENSE.TXT" "${LLDB_TARGET_DIR}/LICENSE.TXT"

cat > "${LLDB_TARGET_DIR}/README.md" <<EOF
# LLVM lldb minimal binary closure

This directory vendors a slimmed LLVM lldb binary for Feng's bundled toolchain.

Version: ${LLVM_VERSION}
Target:  ${TARGET}
Source:  https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-${LLVM_ASSET_FRAGMENT}.tar.xz

Included:
- \`bin/lldb\` — command-line debugger
- \`bin/lldb-dap\` — DAP adapter for \`feng dap\` / VS Code
- \`lib/liblldb.dylib\` — LLDB shared library (hard dependency)
- \`lib/Python3.framework/\` — Python runtime, hard-linked by liblldb
- \`lib/lldb/\` — LLDB python plug-ins and dynamic type support
- \`share/lldb/\` — python scripts, formatters, settings
- \`LICENSE.TXT\` — upstream license

Deliberately excluded:
- \`bin/lldb-server\`, \`bin/lldb-mi\`, \`bin/lldb-argdumper\` (not needed
  for client-side DAP / CLI debugging on the local host)
- \`include/lldb/\` (C++ embedding API)

The feng compiler locates this directory by its own executable position.

Re-sync:
- \`LLVM_VERSION=<ver> ./scripts/fetch_llvm.sh && ./scripts/trim_lldb.sh\` to switch upstream version.
- \`TARGET=<os>-<arch> ./scripts/fetch_llvm.sh && ./scripts/trim_lldb.sh\` to override host detection.
EOF

echo "==> Done. lldb tree at ${LLDB_TARGET_DIR}"
echo "==> Verify: ${LLDB_TARGET_DIR}/bin/lldb --version"
