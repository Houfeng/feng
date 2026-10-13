#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLATFORM=""
PATH_OUTPUT=""
PATH_OUTPUT_SET=false

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

# Accept supported host patch releases without changing compiler selection.
check_compiler_version() {
  local compiler="$1" compiler_path version
  compiler_path="$(command -v "${compiler}")" ||
    die "missing required command: ${compiler}"
  version="$("${compiler_path}" -dumpversion 2>/dev/null)" ||
    die "cannot read ${compiler} version: ${compiler_path}"
  printf '%s\n' "${version}" | awk -F. '
    /^[0-9]+\.[0-9]+\.[0-9]+$/ &&
    $1 == 23 && ($2 > 1 || ($2 == 1 && $3 >= 3)) { ok = 1 }
    END { exit !(ok && NR == 1) }
  ' || die "${compiler} must be >= 23.1.3 and < 24.0.0; found ${version} (${compiler_path})"
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
  LLVM_PREFIX="$(brew --prefix llvm@23 2>/dev/null)" ||
    die "Homebrew llvm@23 is required"
  [[ -x "${LLVM_PREFIX}/bin/clang" ]] ||
    die "Homebrew llvm@23 clang not found: ${LLVM_PREFIX}/bin/clang"
  PATH="${LLVM_PREFIX}/bin:${PATH}"
  export PATH
fi

for command_name in clang file make node timeout unzip zip; do
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
      die "clang must resolve to Homebrew llvm@23"
    require_cmd cc
    check_compiler_version clang
    check_compiler_version cc
    ;;
  linux-x64-gnu|linux-arm64-gnu)
    require_cmd ar
    require_cmd cc
    require_cmd ld.lld
    [[ -f /etc/os-release ]] || die "/etc/os-release not found"
    # shellcheck source=/etc/os-release
    source /etc/os-release
    [[ "${ID:-}" == "ubuntu" && "${VERSION_ID:-}" == "26.04" ]] ||
      die "Ubuntu 26.04 is required, found ${ID:-unknown} ${VERSION_ID:-unknown}"
    check_compiler_version clang
    check_compiler_version cc
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
