#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLATFORM=""

# Print the supported CI environment validation invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_ci_environment.sh --platform=<host-platform>
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
    [[ "$(clang -dumpversion)" == 21.* ]] ||
      die "Clang 21 is required, found $(clang -dumpversion)"
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

echo "==> Verified CI environment for ${PLATFORM}"
