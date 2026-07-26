#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLATFORM=""
PATH_OUTPUT=""
PATH_OUTPUT_SET=false
MACOS_HOST_CLANG_VERSION="21.1.8"

# Print the supported CI environment validation invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_ci_environment.sh \
    --platform=<host-platform> \
    [--path-output=<github-path-file>]
EOF
}

# Report one fatal CI environment mismatch.
die() {
  echo "error: $*" >&2
  exit 1
}

# Require one command resolved through PATH.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --platform=*)
      [[ -z "${PLATFORM}" ]] || die "--platform may only be specified once"
      PLATFORM="${1#--platform=}"
      ;;
    --path-output=*)
      [[ "${PATH_OUTPUT_SET}" == "false" ]] ||
        die "--path-output may only be specified once"
      PATH_OUTPUT="${1#--path-output=}"
      PATH_OUTPUT_SET=true
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
  shift
done

[[ -n "${PLATFORM}" ]] || die "--platform is required"

# shellcheck source=host_platform.sh
source "${SCRIPT_DIR}/host_platform.sh"
DETECTED_PLATFORM="$(feng_detect_host_platform)"
[[ "${DETECTED_PLATFORM}" == "${PLATFORM}" ]] ||
  die "declared platform ${PLATFORM} does not match native host ${DETECTED_PLATFORM}"

if [[ "${PLATFORM}" == "macos-arm64" ]]; then
  require_cmd brew
  LLVM_PREFIX="$(brew --prefix llvm@21 2>/dev/null)" ||
    die "Homebrew llvm@21 is required"
  [[ -x "${LLVM_PREFIX}/bin/clang" ]] ||
    die "Homebrew llvm@21 clang not found: ${LLVM_PREFIX}/bin/clang"
  PATH="${LLVM_PREFIX}/bin:${PATH}"
  export PATH
fi

for command_name in clang file make node unzip zip; do
  require_cmd "${command_name}"
done

case "${PLATFORM}" in
  macos-arm64)
    for command_name in sw_vers xcodebuild xcrun; do
      require_cmd "${command_name}"
    done
    [[ "$(sw_vers -productVersion)" == 26.* ]] ||
      die "macOS 26 is required, found $(sw_vers -productVersion)"
    [[ "$(xcodebuild -version | sed -n '1p')" == "Xcode 26.3" ]] ||
      die "Xcode 26.3 is required"
    [[ "$(xcrun --sdk macosx --show-sdk-version)" == "26.2" ]] ||
      die "macOS 26.2 SDK is required"
    [[ "$(command -v clang)" -ef "${LLVM_PREFIX}/bin/clang" ]] ||
      die "clang must resolve to Homebrew llvm@21"
    [[ "$(clang -dumpversion)" == "${MACOS_HOST_CLANG_VERSION}" ]] ||
      die "Homebrew Clang ${MACOS_HOST_CLANG_VERSION} is required, found $(clang -dumpversion)"
    ;;
  linux-x64-gnu|linux-arm64-gnu)
    require_cmd ar
    require_cmd ld.lld
    [[ -f /etc/os-release ]] || die "/etc/os-release not found"
    # shellcheck source=/etc/os-release
    source /etc/os-release
    [[ "${ID:-}" == "ubuntu" && "${VERSION_ID:-}" == "26.04" ]] ||
      die "Ubuntu 26.04 is required, found ${ID:-unknown} ${VERSION_ID:-unknown}"
    [[ "$(clang -dumpversion)" == 21.* ]] ||
      die "Clang 21 is required, found $(clang -dumpversion)"
    ;;
  *)
    die "unsupported CI platform: ${PLATFORM}"
    ;;
esac

if [[ -n "${PATH_OUTPUT}" ]]; then
  PATH_OUTPUT_DIR="$(dirname "${PATH_OUTPUT}")"
  [[ -d "${PATH_OUTPUT_DIR}" ]] ||
    die "PATH output directory not found: ${PATH_OUTPUT_DIR}"
  printf '%s\n' "$(dirname "$(command -v clang)")" >> "${PATH_OUTPUT}"
fi

echo "==> Verified CI environment for ${PLATFORM}"
