#!/usr/bin/env bash
set -euo pipefail

# Download and extract the LLVM official prebuilt into temp/llvm/ for
# consumption by trim_clang.sh / trim_lldb.sh. This is a maintenance
# script — not part of the release build flow.
#
# Cache behaviour: the archive and extracted tree live under
# ${PROJECT_ROOT}/temp/llvm/ (gitignored). Re-runs skip the download
# when the archive is already present, and skip extraction when the
# extracted root is already present. Delete the cache to force a
# re-download.
#
# Resource: https://github.com/llvm/llvm-project/releases
# The official prebuilt only ships macOS-ARM64 / Linux-ARM64 / Linux-X64
# (no macOS-X64 binary). Windows uses a different naming convention and
# is intentionally unsupported here until that target lands.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LLVM_VERSION="${LLVM_VERSION:-22.1.8}"
LLVM_TAG="llvmorg-${LLVM_VERSION}"

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

# Map a Feng target triple (<os>-<arch>) to the LLVM prebuilt asset name
# fragment. Unsupported targets fail loudly — adding a platform requires
# verifying the upstream asset name, not guessing.
target_to_llvm_asset_fragment() {
  case "$1" in
    macos-arm64) printf 'macOS-ARM64' ;;
    linux-arm64) printf 'Linux-ARM64' ;;
    linux-x64)   printf 'Linux-X64' ;;
    *) echo "error: no LLVM prebuilt asset mapping for target: $1" >&2; exit 1 ;;
  esac
}

LLVM_ASSET_FRAGMENT="$(target_to_llvm_asset_fragment "${TARGET}")"
LLVM_ASSET_NAME="LLVM-${LLVM_VERSION}-${LLVM_ASSET_FRAGMENT}.tar.xz"
LLVM_SRC_URL="${LLVM_SRC_URL:-https://github.com/llvm/llvm-project/releases/download/${LLVM_TAG}/${LLVM_ASSET_NAME}}"

CACHE_DIR="${PROJECT_ROOT}/temp/llvm"
ARCHIVE_FILE="${CACHE_DIR}/${LLVM_ASSET_NAME}"
EXTRACTED_ROOT="${CACHE_DIR}/${LLVM_ASSET_NAME%.tar.xz}"

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd curl
require_cmd tar
require_cmd find

echo "==> LLVM toolchain ${LLVM_VERSION} for ${TARGET}"
echo "==> Source: ${LLVM_SRC_URL}"
echo "==> Cache:  ${CACHE_DIR}"

mkdir -p "${CACHE_DIR}"

if [[ -d "${EXTRACTED_ROOT}" ]]; then
  echo "==> Reusing extracted LLVM at ${EXTRACTED_ROOT}"
elif [[ -f "${ARCHIVE_FILE}" ]]; then
  echo "==> Reusing cached archive ${ARCHIVE_FILE}"
  echo "==> Extracting"
  tar -xf "${ARCHIVE_FILE}" -C "${CACHE_DIR}"
else
  echo "==> Downloading ${LLVM_ASSET_NAME}"
  curl -sSLf --connect-timeout 15 --retry 3 "${LLVM_SRC_URL}" -o "${ARCHIVE_FILE}"
  echo "==> Extracting"
  tar -xf "${ARCHIVE_FILE}" -C "${CACHE_DIR}"
fi

if [[ ! -d "${EXTRACTED_ROOT}" ]]; then
  echo "error: extraction did not produce expected dir: ${EXTRACTED_ROOT}" >&2
  exit 1
fi

if [[ ! -f "${EXTRACTED_ROOT}/bin/clang" ]]; then
  echo "error: bin/clang missing at extracted root — content looks wrong" >&2
  exit 1
fi

echo "==> Done. Extracted LLVM ${LLVM_VERSION} for ${TARGET} at:"
echo "    ${EXTRACTED_ROOT}"
echo "==> Next: scripts/trim_clang.sh && scripts/trim_lldb.sh"
