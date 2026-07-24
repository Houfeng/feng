#!/usr/bin/env bash
set -euo pipefail

# Download one official LLVM prebuilt and the pinned Linux runtime packages
# used by trim_llvm.sh. This is a local maintenance script, not part of the
# release or end-user build flow. All persistent inputs live under local/llvm/.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LLVM_VERSION="${LLVM_VERSION:-22.1.8}"
LLVM_TAG="llvmorg-${LLVM_VERSION}"
PLATFORM=""
CACHE_DIR="${PROJECT_ROOT}/local/llvm"
PRINT_RUNTIME_MANIFEST=0

# Print the supported command-line interface.
usage() {
  cat <<'EOF'
Usage: scripts/fetch_llvm.sh [options]

Options:
  --platform <platform>  LLVM host platform. Defaults to the current host.
                         Supported: macos-arm64, linux-x64-gnu,
                         linux-arm64-gnu.
  --print-runtime-manifest
                         Print the selected Linux runtime manifest and exit.
  -h, --help             Show this help.

Environment:
  LLVM_VERSION           Upstream LLVM version. Default: 22.1.8.
  LLVM_SRC_URL           Override the official LLVM archive URL.

Examples:
  ./scripts/fetch_llvm.sh
  ./scripts/fetch_llvm.sh --platform linux-x64-gnu
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

# Normalize uname values to a supported complete host platform.
detect_host_platform() {
  local os arch
  case "$(uname -s)" in
    Darwin) os="macos" ;;
    Linux) os="linux" ;;
    *) die "unsupported host OS: $(uname -s)" ;;
  esac
  case "$(uname -m)" in
    arm64|aarch64) arch="arm64" ;;
    x86_64|amd64) arch="x64" ;;
    *) die "unsupported host architecture: $(uname -m)" ;;
  esac
  if [[ "${os}" == "linux" ]]; then
    printf '%s-%s-gnu' "${os}" "${arch}"
  else
    printf '%s-%s' "${os}" "${arch}"
  fi
}

# Map a complete Feng host platform to the official LLVM asset fragment.
platform_to_llvm_asset_fragment() {
  case "$1" in
    macos-arm64) printf 'macOS-ARM64' ;;
    linux-arm64-gnu) printf 'Linux-ARM64' ;;
    linux-x64-gnu) printf 'Linux-X64' ;;
    *) die "no LLVM prebuilt asset mapping for platform: $1" ;;
  esac
}

# Map a Linux host platform to the AlmaLinux RPM architecture.
platform_to_rpm_arch() {
  case "$1" in
    linux-arm64-gnu) printf 'aarch64' ;;
    linux-x64-gnu) printf 'x86_64' ;;
    *) die "no Linux runtime RPM mapping for platform: $1" ;;
  esac
}

# Parse named options; positional platform arguments are rejected.
parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --platform)
        [[ $# -ge 2 ]] || die "--platform requires a value"
        PLATFORM="$2"
        shift 2
        ;;
      --print-runtime-manifest)
        PRINT_RUNTIME_MANIFEST=1
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

# Download one immutable input and verify its pinned SHA-256.
download_verified() {
  local url="$1"
  local output="$2"
  local expected_sha256="$3"
  local actual_sha256

  if [[ -f "${output}" ]]; then
    actual_sha256="$(sha256_file "${output}")"
    if [[ "${actual_sha256}" == "${expected_sha256}" ]]; then
      echo "==> Reusing verified $(basename "${output}")"
      return
    fi
    die "cached file checksum mismatch: ${output}"
  fi

  echo "==> Downloading $(basename "${output}")"
  curl -fsSL --connect-timeout 15 --retry 5 "${url}" -o "${output}"
  actual_sha256="$(sha256_file "${output}")"
  [[ "${actual_sha256}" == "${expected_sha256}" ]] ||
    die "download checksum mismatch for ${url}: expected ${expected_sha256}, got ${actual_sha256}"
}

# Download and extract the selected official LLVM archive.
fetch_llvm_archive() {
  local asset_fragment asset_name archive_file extracted_root source_url
  asset_fragment="$(platform_to_llvm_asset_fragment "${PLATFORM}")"
  asset_name="LLVM-${LLVM_VERSION}-${asset_fragment}.tar.xz"
  archive_file="${CACHE_DIR}/${asset_name}"
  extracted_root="${CACHE_DIR}/${asset_name%.tar.xz}"
  source_url="${LLVM_SRC_URL:-https://github.com/llvm/llvm-project/releases/download/${LLVM_TAG}/${asset_name}}"

  echo "==> LLVM ${LLVM_VERSION} for ${PLATFORM}"
  echo "==> Source: ${source_url}"

  if [[ -d "${extracted_root}" ]]; then
    echo "==> Reusing extracted LLVM at ${extracted_root}"
  else
    if [[ ! -f "${archive_file}" ]]; then
      echo "==> Downloading ${asset_name}"
      curl -fsSL --connect-timeout 15 --retry 5 "${source_url}" -o "${archive_file}"
    else
      echo "==> Reusing cached ${asset_name}"
    fi
    echo "==> Extracting ${asset_name}"
    tar -xf "${archive_file}" -C "${CACHE_DIR}"
  fi

  [[ -d "${extracted_root}" ]] ||
    die "extraction did not produce expected directory: ${extracted_root}"
  for required in clang lld llvm-ar llvm-ranlib lldb lldb-dap; do
    [[ -f "${extracted_root}/bin/${required}" ]] ||
      die "required LLVM input missing: ${extracted_root}/bin/${required}"
  done
}

# Emit the pinned Linux private-runtime package manifest for one architecture.
# Each entry is the complete immutable package URL, file name and SHA-256.
linux_runtime_packages() {
  local arch="$1"
  case "${arch}" in
    x86_64)
      cat <<'EOF'
https://repo.almalinux.org/almalinux/8.10/BaseOS/x86_64/os/Packages/libxml2-2.9.7-21.el8_10.6.x86_64.rpm|libxml2-2.9.7-21.el8_10.6.x86_64.rpm|e90b9a22537a3b9b6bd97697ae764bf5b6991556d2fad061421cba2e4f7096aa
https://repo.almalinux.org/almalinux/8.10/BaseOS/x86_64/os/Packages/xz-libs-5.2.4-4.el8_6.x86_64.rpm|xz-libs-5.2.4-4.el8_6.x86_64.rpm|eab633cd7e81de007792509bfbea6cbd48dec90e4c0f18a4c1c337cb9c06ee51
https://repo.almalinux.org/almalinux/8.10/BaseOS/x86_64/os/Packages/zlib-1.2.11-25.el8.x86_64.rpm|zlib-1.2.11-25.el8.x86_64.rpm|ab40e85f180a6d38ce0291408b7ef5c422250f53cb26c44399ef1a4e8622778e
https://repo.almalinux.org/almalinux/8.10/BaseOS/x86_64/os/Packages/libgcc-8.5.0-28.el8_10.alma.1.x86_64.rpm|libgcc-8.5.0-28.el8_10.alma.1.x86_64.rpm|629a08266f7c1397c00d7c32c0ac6d110fe56e993dcf94024559aa28a570f18d
https://repo.almalinux.org/almalinux/8.10/AppStream/x86_64/os/Packages/python3.11-libs-3.11.9-1.el8_10.x86_64.rpm|python3.11-libs-3.11.9-1.el8_10.x86_64.rpm|a4f260e7a38d7ee990f3feab2a0c21b4d815ff5d2f9bac328c54d52959c29526
https://raw.githubusercontent.com/python/cpython/v3.11.9/LICENSE|cpython-3.11.9-LICENSE.txt|3b2f81fe21d181c499c59a256c8e1968455d6689d269aa85373bfb6af41da3bf
https://security.ubuntu.com/ubuntu/pool/main/n/ncurses/libncurses6_6.3-2ubuntu0.2_amd64.deb|libncurses6_6.3-2ubuntu0.2_amd64.deb|0ab876ebf9b8a9118cd45afe435a95e3ec2e6e0302f44c18e3fdc28f26be75ee
https://security.ubuntu.com/ubuntu/pool/main/n/ncurses/libtinfo6_6.3-2ubuntu0.2_amd64.deb|libtinfo6_6.3-2ubuntu0.2_amd64.deb|47ee28ef9c424ce2010e91101f0c2c98009a432071bc654c4379e2f62b6335d8
https://archive.ubuntu.com/ubuntu/pool/main/g/gcc-12/gcc-12-base_12.3.0-1ubuntu1~22.04.3_amd64.deb|gcc-12-base_12.3.0-1ubuntu1~22.04.3_amd64.deb|7f9253b7e0976f0526fc21346c73ee006e185a7b2f2f865048ea78a2af55bc8d
https://archive.ubuntu.com/ubuntu/pool/main/g/gcc-12/libstdc++6_12.3.0-1ubuntu1~22.04.3_amd64.deb|libstdc++6_12.3.0-1ubuntu1~22.04.3_amd64.deb|29ef3d289b272704b75141906c8f683db0f2bdb0942061366691333bfb289bdc
EOF
      ;;
    aarch64)
      cat <<'EOF'
https://repo.almalinux.org/almalinux/8.10/BaseOS/aarch64/os/Packages/libxml2-2.9.7-21.el8_10.6.aarch64.rpm|libxml2-2.9.7-21.el8_10.6.aarch64.rpm|0bb5d3a5c24b7508f7b8cffedf8fbfddbc7cde23206e994d449b3a8a29dc9fc4
https://repo.almalinux.org/almalinux/8.10/BaseOS/aarch64/os/Packages/xz-libs-5.2.4-4.el8_6.aarch64.rpm|xz-libs-5.2.4-4.el8_6.aarch64.rpm|eb1bf61e0b1635d73c0f8abda8a892d3facb8112fa5dbeb0865a7bdfacb4298a
https://repo.almalinux.org/almalinux/8.10/BaseOS/aarch64/os/Packages/zlib-1.2.11-25.el8.aarch64.rpm|zlib-1.2.11-25.el8.aarch64.rpm|2822e566148dd0d50844f53830f24864bfb0e68a9f2bb7cb741e78da99bfd0ed
https://repo.almalinux.org/almalinux/8.10/BaseOS/aarch64/os/Packages/libgcc-8.5.0-28.el8_10.alma.1.aarch64.rpm|libgcc-8.5.0-28.el8_10.alma.1.aarch64.rpm|6978c6f3dc10c50fcd44d02c398e89a08b9eabb7fbf038f09249fb9368e9b073
https://repo.almalinux.org/almalinux/8.10/AppStream/aarch64/os/Packages/python3.11-libs-3.11.9-1.el8_10.aarch64.rpm|python3.11-libs-3.11.9-1.el8_10.aarch64.rpm|b38245bef65020a5482d70842eb715d4df7aed4b078872cd6399b46a6b6d2629
https://raw.githubusercontent.com/python/cpython/v3.11.9/LICENSE|cpython-3.11.9-LICENSE.txt|3b2f81fe21d181c499c59a256c8e1968455d6689d269aa85373bfb6af41da3bf
https://ports.ubuntu.com/ubuntu-ports/pool/main/n/ncurses/libncurses6_6.3-2ubuntu0.2_arm64.deb|libncurses6_6.3-2ubuntu0.2_arm64.deb|1269a0a90a6b9ab074ada65d8eed43636ec58ac9974e9c600a56469702be0bd4
https://ports.ubuntu.com/ubuntu-ports/pool/main/n/ncurses/libtinfo6_6.3-2ubuntu0.2_arm64.deb|libtinfo6_6.3-2ubuntu0.2_arm64.deb|4884d3f3a3bfd86440e8f2c81c69fc893b43d230719c2a5ff25529f4157650fb
https://ports.ubuntu.com/ubuntu-ports/pool/main/g/gcc-12/gcc-12-base_12.3.0-1ubuntu1~22.04.3_arm64.deb|gcc-12-base_12.3.0-1ubuntu1~22.04.3_arm64.deb|b2bc566681989941dd059d6391b01c41854857f26d83c53358287d620dfb0fd4
https://ports.ubuntu.com/ubuntu-ports/pool/main/g/gcc-12/libstdc++6_12.3.0-1ubuntu1~22.04.3_arm64.deb|libstdc++6_12.3.0-1ubuntu1~22.04.3_arm64.deb|59d244119e6129c7e9c0897491c0e210e925d15881b2dedcf8965ceb4fbf1aa3
EOF
      ;;
    *) die "unsupported AlmaLinux RPM architecture: ${arch}" ;;
  esac
}

# Download the complete pinned input set used to construct Linux private libs.
fetch_linux_runtime_packages() {
  local arch cache_dir entry url filename expected_sha256
  arch="$(platform_to_rpm_arch "${PLATFORM}")"
  cache_dir="${CACHE_DIR}/runtime/${PLATFORM}"
  mkdir -p "${cache_dir}"

  while IFS='|' read -r url filename expected_sha256; do
    [[ -n "${url}" ]] || continue
    entry="${cache_dir}/${filename}"
    download_verified "${url}" "${entry}" "${expected_sha256}"
  done < <(linux_runtime_packages "${arch}")
}

# Coordinate argument parsing and all persistent downloads.
main() {
  parse_args "$@"
  PLATFORM="${PLATFORM:-$(detect_host_platform)}"
  if [[ ${PRINT_RUNTIME_MANIFEST} -eq 1 ]]; then
    [[ "${PLATFORM}" == linux-*-gnu ]] ||
      die "--print-runtime-manifest requires a Linux GNU host platform"
    linux_runtime_packages "$(platform_to_rpm_arch "${PLATFORM}")"
    exit 0
  fi

  require_cmd awk
  require_cmd curl
  require_cmd tar
  if ! command -v sha256sum >/dev/null 2>&1; then
    require_cmd shasum
  fi

  mkdir -p "${CACHE_DIR}"
  fetch_llvm_archive
  if [[ "${PLATFORM}" == linux-*-gnu ]]; then
    fetch_linux_runtime_packages
  fi

  echo "==> Done. Cached inputs for ${PLATFORM} under ${CACHE_DIR}"
  echo "==> Next: scripts/trim_llvm.sh --platform ${PLATFORM}"
}

main "$@"
