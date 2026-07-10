#!/usr/bin/env bash
set -euo pipefail

# Slim the extracted LLVM prebuilt into toolchain/lldb/<os>-<arch>/.
#
# Consumes the LLVM root produced by fetch_llvm.sh. Re-runs overwrite the
# target directory safely — only this tool's dir is touched, other tools'
# trees under toolchain/ are preserved.
#
# Layout produced (macOS):
#   toolchain/lldb/<os>-<arch>/
#     bin/lldb, bin/lldb-dap   — the executables
#     bin/lldb-argdumper       — used by lldb `run` for shell arg expansion
#     bin/debugserver          — local debug server spawned by lldb on macOS
#     lib/liblldb.<ver>.dylib  — LLDB shared library (real file, @rpath dep)
#     lib/liblldb.dylib       — unversioned symlink (preserved)
#     LICENSE.TXT             — upstream license (when present)
#     README.md                — provenance and re-sync instructions
#
# On Linux the local debug server is bin/lldb-server instead of bin/debugserver.
#
# Deliberately excluded: lldb-mi (MI interface for IDE integration, not
# needed for DAP / CLI), include/lldb/ (C++ embedding API).
#
# Note: lldb-argdumper is included — lldb's `run` command uses it for
# shell expansion of process args. Without it, `run` fails with
# "could not find the lldb-argdumper tool".
#
# Python: empirical check on LLVM 22.1.8 macOS ARM64 prebuilt shows
# liblldb links only system libs (libcompression, libpanel/libncurses/
# libform, libxml2, libedit, Foundation, CoreFoundation, libobjc,
# CoreServices, Security, libz, libSystem, libc++). No Python3.framework
# hard-link, no libpython dependency. The build disables Python scripting.
# Should a future LLVM prebuilt ship Python3.framework / lib/lldb/ /
# share/lldb/, the conditional blocks below will pick them up.

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

# lldb-argdumper — used by lldb's `run` command for shell expansion of
# process args. Without it, `run` fails.
if [[ -f "${LLVM_EXTRACTED_ROOT}/bin/lldb-argdumper" ]]; then
  cp "${LLVM_EXTRACTED_ROOT}/bin/lldb-argdumper" "${LLDB_TARGET_DIR}/bin/lldb-argdumper"
  chmod 0755 "${LLDB_TARGET_DIR}/bin/lldb-argdumper"
else
  echo "warning: bin/lldb-argdumper not found — lldb 'run' command will fail" >&2
fi

# Local debug server that lldb spawns on the host. macOS uses debugserver;
# Linux uses lldb-server. Include whichever is present.
case "${TARGET}" in
  macos-*)
    if [[ -f "${LLVM_EXTRACTED_ROOT}/bin/debugserver" ]]; then
      cp "${LLVM_EXTRACTED_ROOT}/bin/debugserver" "${LLDB_TARGET_DIR}/bin/debugserver"
      chmod 0755 "${LLDB_TARGET_DIR}/bin/debugserver"
    else
      echo "warning: bin/debugserver not found — lldb will not be able to launch processes" >&2
    fi
    ;;
  linux-*)
    if [[ -f "${LLVM_EXTRACTED_ROOT}/bin/lldb-server" ]]; then
      cp "${LLVM_EXTRACTED_ROOT}/bin/lldb-server" "${LLDB_TARGET_DIR}/bin/lldb-server"
      chmod 0755 "${LLDB_TARGET_DIR}/bin/lldb-server"
    else
      echo "warning: bin/lldb-server not found — lldb will not be able to launch processes" >&2
    fi
    ;;
esac

# liblldb.<version>.dylib — the real file bin/lldb and bin/lldb-dap link
# to via @rpath/liblldb.<version>.dylib. The unversioned liblldb.dylib
# symlink points to it; copy both with cp -PR to preserve the link.
mkdir -p "${LLDB_TARGET_DIR}/lib"
if [[ -f "${LLVM_EXTRACTED_ROOT}/lib/liblldb.${LLVM_VERSION}.dylib" ]]; then
  cp -PR "${LLVM_EXTRACTED_ROOT}/lib/liblldb"*.dylib "${LLDB_TARGET_DIR}/lib/"
else
  echo "warning: lib/liblldb.${LLVM_VERSION}.dylib not found — lldb will fail to start" >&2
fi

# lib/Python3.framework — Python runtime. Empirically absent from the
# LLVM 22.1.8 macOS prebuilt (the build disables Python scripting, so
# liblldb has no Python hard-link). Conditional block kept for forward
# compatibility — if a future LLVM prebuilt ships it, bundle it.
if [[ -d "${LLVM_EXTRACTED_ROOT}/lib/Python3.framework" ]]; then
  cp -R "${LLVM_EXTRACTED_ROOT}/lib/Python3.framework" "${LLDB_TARGET_DIR}/lib/"
fi

# lib/lldb/ — LLDB's own python plug-ins / support modules. Conditional,
# for forward compatibility.
if [[ -d "${LLVM_EXTRACTED_ROOT}/lib/lldb" ]]; then
  cp -R "${LLVM_EXTRACTED_ROOT}/lib/lldb" "${LLDB_TARGET_DIR}/lib/"
fi

# share/lldb/ — python scripts, formatters, settings. Conditional,
# for forward compatibility.
if [[ -d "${LLVM_EXTRACTED_ROOT}/share/lldb" ]]; then
  mkdir -p "${LLDB_TARGET_DIR}/share"
  cp -R "${LLVM_EXTRACTED_ROOT}/share/lldb" "${LLDB_TARGET_DIR}/share/"
fi

if [[ -f "${LLVM_EXTRACTED_ROOT}/LICENSE.TXT" ]]; then
  cp "${LLVM_EXTRACTED_ROOT}/LICENSE.TXT" "${LLDB_TARGET_DIR}/LICENSE.TXT"
fi

cat > "${LLDB_TARGET_DIR}/README.md" <<EOF
# LLVM lldb minimal binary closure

This directory vendors a slimmed LLVM lldb binary for Feng's bundled toolchain.

Version: ${LLVM_VERSION}
Target:  ${TARGET}
Source:  https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-${LLVM_ASSET_FRAGMENT}.tar.xz

Included:
- \`bin/lldb\` — command-line debugger
- \`bin/lldb-dap\` — DAP adapter for \`feng dap\` / VS Code
- \`bin/lldb-argdumper\` — used by lldb \`run\` for shell arg expansion
- \`bin/debugserver\` (macOS) / \`bin/lldb-server\` (Linux) — local debug
  server that lldb spawns on the host
- \`lib/liblldb.${LLVM_VERSION}.dylib\` — LLDB shared library (real file,
  the @rpath dep bin/lldb and bin/lldb-dap load)
- \`lib/liblldb.dylib\` — unversioned symlink (preserved)
- \`LICENSE.TXT\` — upstream license (when present in upstream prebuilt)

Conditional (present in some upstream builds; absent in LLVM 22.1.8 macOS):
- \`lib/Python3.framework/\` — Python runtime, hard-linked by liblldb when
  the upstream build enables Python scripting
- \`lib/lldb/\` — LLDB python plug-ins and dynamic type support
- \`share/lldb/\` — python scripts, formatters, settings

Deliberately excluded:
- \`bin/lldb-mi\` (MI interface for IDE integration, not needed for DAP / CLI)
- \`include/lldb/\` (C++ embedding API)

The feng compiler locates this directory by its own executable position.

Re-sync:
- \`LLVM_VERSION=<ver> ./scripts/fetch_llvm.sh && ./scripts/trim_lldb.sh\` to switch upstream version.
- \`TARGET=<os>-<arch> ./scripts/fetch_llvm.sh && ./scripts/trim_lldb.sh\` to override host detection.
EOF

echo "==> Done. lldb tree at ${LLDB_TARGET_DIR}"
echo "==> Verify: ${LLDB_TARGET_DIR}/bin/lldb --version"
