#!/usr/bin/env bash
set -euo pipefail

# Download and extract the LLVM official prebuilt into local/llvm/ for
# consumption by trim_llvm.sh. This is a maintenance
# script — not part of the release build flow.
#
# Cache behaviour: the archive and extracted tree live under
# ${PROJECT_ROOT}/local/llvm/ (gitignored). Re-runs skip the download
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
PLATFORM=""

# Print the supported command-line interface.
usage() {
  cat <<'EOF'
Usage: scripts/fetch_llvm.sh [options]

Options:
  --platform <os-arch>  LLVM executable platform. Defaults to the current host.
                        Supported: macos-arm64, linux-x64, linux-arm64.
  -h, --help            Show this help.

Environment:
  LLVM_VERSION          Upstream LLVM version. Default: 22.1.8.
  LLVM_SRC_URL          Override the official archive URL.

Examples:
  ./scripts/fetch_llvm.sh
  ./scripts/fetch_llvm.sh --platform linux-x64
EOF
}

# Fail with a consistent diagnostic.
die() {
  echo "error: $*" >&2
  exit 1
}

# Normalize uname values to a supported Feng platform identifier.
detect_host_platform() {
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

# Parse named options; positional platform arguments are intentionally rejected.
parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --platform)
        [[ $# -ge 2 ]] || die "--platform requires a value"
        PLATFORM="$2"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *) die "unknown argument: $1" ;;
    esac
  done
}

parse_args "$@"
PLATFORM="${PLATFORM:-$(detect_host_platform)}"

# Map a Feng platform identifier (<os>-<arch>) to the LLVM prebuilt asset name
# fragment. Unsupported platforms fail loudly — adding a platform requires
# verifying the upstream asset name, not guessing.
platform_to_llvm_asset_fragment() {
  case "$1" in
    macos-arm64) printf 'macOS-ARM64' ;;
    linux-arm64) printf 'Linux-ARM64' ;;
    linux-x64)   printf 'Linux-X64' ;;
    *) die "no LLVM prebuilt asset mapping for platform: $1" ;;
  esac
}

LLVM_ASSET_FRAGMENT="$(platform_to_llvm_asset_fragment "${PLATFORM}")"
LLVM_ASSET_NAME="LLVM-${LLVM_VERSION}-${LLVM_ASSET_FRAGMENT}.tar.xz"
LLVM_SRC_URL="${LLVM_SRC_URL:-https://github.com/llvm/llvm-project/releases/download/${LLVM_TAG}/${LLVM_ASSET_NAME}}"

CACHE_DIR="${PROJECT_ROOT}/local/llvm"
ARCHIVE_FILE="${CACHE_DIR}/${LLVM_ASSET_NAME}"
EXTRACTED_ROOT="${CACHE_DIR}/${LLVM_ASSET_NAME%.tar.xz}"

# Ensure a command required by download or extraction is available.
require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd curl
require_cmd tar

echo "==> LLVM toolchain ${LLVM_VERSION} for ${PLATFORM}"
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

for required in clang lldb lldb-dap; do
  if [[ ! -f "${EXTRACTED_ROOT}/bin/${required}" ]]; then
    die "bin/${required} missing at extracted root — content looks wrong"
  fi
done

echo "==> Done. Extracted LLVM ${LLVM_VERSION} for ${PLATFORM} at:"
echo "    ${EXTRACTED_ROOT}"
echo "==> Next: scripts/trim_llvm.sh --platform ${PLATFORM}"
