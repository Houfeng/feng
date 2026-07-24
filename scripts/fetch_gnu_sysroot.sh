#!/usr/bin/env bash
set -euo pipefail

# Download the pinned Debian cross packages used by trim_gnu_sysroot.sh.
# This is a local maintenance script. It never calls a host package manager,
# and every input is addressed by an immutable Debian Snapshot file hash.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CACHE_DIR="${PROJECT_ROOT}/local/sysroot/gnu"
PLATFORM=""
PRINT_MANIFEST=0

# Print the supported command-line interface.
usage() {
  cat <<'EOF'
Usage: scripts/fetch_gnu_sysroot.sh [options]

Options:
  --platform <platform>  Fetch one target platform. When omitted, fetch both.
                         Supported: linux-x64-gnu, linux-arm64-gnu.
  --print-manifest       Print the pinned binary package manifest and exit.
  -h, --help             Show this help.

Examples:
  ./scripts/fetch_gnu_sysroot.sh
  ./scripts/fetch_gnu_sysroot.sh --platform linux-arm64-gnu
EOF
}

# Fail with a consistent diagnostic.
die() {
  echo "error: $*" >&2
  exit 1
}

# Ensure a required maintenance command exists.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

# Return the SHA-256 digest of one file on macOS or Linux.
sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

# Parse named options and reject ambiguous positional arguments.
parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --platform)
        [[ $# -ge 2 ]] || die "--platform requires a value"
        PLATFORM="$2"
        shift 2
        ;;
      --print-manifest)
        PRINT_MANIFEST=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *) die "unknown argument: $1" ;;
    esac
  done
}

# Emit target, file name, immutable Snapshot hash and SHA-256.
binary_packages() {
  cat <<'EOF'
linux-x64-gnu|libc6-amd64-cross_2.31-9cross4_all.deb|8e336e6f3d10c8cf6c15384cf5c4def3d9ba4a6e|a3ef5dbbfa61f944efaa759feb4b3d05f572be159216545bced7b297fc09bab5
linux-x64-gnu|libc6-dev-amd64-cross_2.31-9cross4_all.deb|d079dc5b1ccd1bdd5a6512c696ed22facd51f5f6|6821bb405a262ab73c0d087d742ce531103b8906f3a5fc0ee25c347295edee30
linux-x64-gnu|linux-libc-dev-amd64-cross_5.10.13-1cross4_all.deb|8f0778971afe558fc96ba50c37351973b06cef5e|50357dfa58ceb7ffd6c85a91fc7e099c3171ae8abc34c328267212f1dd4f2186
linux-x64-gnu|libgcc-s1-amd64-cross_10.2.1-6cross1_all.deb|3b27ffadc67d7b994582d2d9aeb9548c605e4c24|9e1d76c3f638472f7940bfac8f42c10aa534c0a0fce2314641d41a2b492347cd
linux-x64-gnu|libgcc-10-dev-amd64-cross_10.2.1-6cross1_all.deb|3c918454102560cabc4bcc4d4fde9a29fb4b5a9d|347113a3bdd7379846e618efb610f49ce2704032972e1797e8e126925d5d7b19
linux-arm64-gnu|libc6-arm64-cross_2.31-9cross4_all.deb|933531b825d7b1c8e9290ab7ed9f8ea739d7e39d|ccd237814281910e72422a6e86a6ef7570105fc69119ff8fc82f6a4827a8a015
linux-arm64-gnu|libc6-dev-arm64-cross_2.31-9cross4_all.deb|02e7adc8e87ecb82bf23efaa9bf6083638725585|4b88456f6913474587be547a865c6b42756b2031ca11ddf6285ea6ff4275a48d
linux-arm64-gnu|linux-libc-dev-arm64-cross_5.10.13-1cross4_all.deb|6131e8a3c193823fecbb9a01650be56002410d80|3a5c7920a299c88f69b7d64eb60fb6e65631cd57721a30ed60a2f6b7adf931d3
linux-arm64-gnu|libgcc-s1-arm64-cross_10.2.1-6cross1_all.deb|0cf5e51195fbb6f5f210e10f924c4b9f3982402a|2966823a38c100f77786a49f734d240c892fc86c473dbf1c862c23424900fd83
linux-arm64-gnu|libgcc-10-dev-arm64-cross_10.2.1-6cross1_all.deb|e8ba8ad6fc673f3ae723bce0b14caa41f5169d37|eca208337cfdecc8d625cddb6dbc0bffa00b48cfec2d1cc347bfbac3abcea4f5
EOF
}

# Download one immutable package and reject any stale or corrupt cache entry.
fetch_package() {
  local platform="$1"
  local filename="$2"
  local snapshot_hash="$3"
  local expected_sha256="$4"
  local output_dir="${CACHE_DIR}/${platform}"
  local output="${output_dir}/${filename}"
  local url="https://snapshot.debian.org/file/${snapshot_hash}"
  local actual_sha256

  mkdir -p "${output_dir}"
  if [[ -f "${output}" ]]; then
    actual_sha256="$(sha256_file "${output}")"
    [[ "${actual_sha256}" == "${expected_sha256}" ]] ||
      die "cached package checksum mismatch: ${output}"
    echo "==> Reusing verified ${filename}"
    return
  fi

  echo "==> Downloading ${filename}"
  curl -fsSL --connect-timeout 15 --retry 5 "${url}" -o "${output}"
  actual_sha256="$(sha256_file "${output}")"
  [[ "${actual_sha256}" == "${expected_sha256}" ]] ||
    die "package checksum mismatch for ${url}: expected ${expected_sha256}, got ${actual_sha256}"
}

# Download the selected target set, or both targets by default.
main() {
  local entry package_platform filename snapshot_hash expected_sha256 matched=0
  parse_args "$@"
  if [[ ${PRINT_MANIFEST} -eq 1 ]]; then
    binary_packages
    exit 0
  fi
  case "${PLATFORM}" in
    ""|linux-x64-gnu|linux-arm64-gnu) ;;
    *) die "unsupported GNU sysroot platform: ${PLATFORM}" ;;
  esac

  require_cmd awk
  require_cmd curl
  if ! command -v sha256sum >/dev/null 2>&1; then
    require_cmd shasum
  fi

  while IFS='|' read -r package_platform filename snapshot_hash expected_sha256; do
    [[ -z "${PLATFORM}" || "${package_platform}" == "${PLATFORM}" ]] || continue
    fetch_package "${package_platform}" "${filename}" "${snapshot_hash}" "${expected_sha256}"
    matched=1
  done < <(binary_packages)

  [[ ${matched} -eq 1 ]] || die "no package matched platform: ${PLATFORM}"
  echo "==> Done. GNU sysroot inputs are cached under ${CACHE_DIR}"
  echo "==> Next: scripts/trim_gnu_sysroot.sh${PLATFORM:+ --platform ${PLATFORM}}"
}

main "$@"
