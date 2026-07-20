#!/usr/bin/env bash
set -euo pipefail

# Trim one official LLVM prebuilt into toolchain/llvm/<os>-<arch>/.
# clang, lld, lldb and lldb-dap are emitted together so their version, source
# and host platform cannot drift. The source archive is prepared separately
# by fetch_llvm.sh; this maintenance script performs no network access.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LLVM_VERSION="${LLVM_VERSION:-22.1.8}"
PLATFORM=""
LLVM_ROOT=""
OUTPUT_PARENT="${PROJECT_ROOT}/toolchain/llvm"
STAGING_DIR=""
BACKUP_DIR=""
OUTPUT_DIR=""

# Print the supported command-line interface.
usage() {
  cat <<'EOF'
Usage: scripts/trim_llvm.sh [options]

Options:
  --platform <os-arch>  LLVM executable platform. Defaults to the current host.
                        Supported: macos-arm64, linux-x64, linux-arm64.
  --llvm-root <path>    Extracted LLVM root. Defaults to the matching directory
                        under local/llvm/ for LLVM_VERSION.
  -h, --help            Show this help.

Environment:
  LLVM_VERSION          Upstream LLVM version. Default: 22.1.8.

Examples:
  ./scripts/trim_llvm.sh
  ./scripts/trim_llvm.sh --platform linux-x64
  ./scripts/trim_llvm.sh --platform linux-arm64 \
    --llvm-root /path/to/LLVM-22.1.8-Linux-ARM64
EOF
}

# Fail with a consistent diagnostic.
die() {
  echo "error: $*" >&2
  exit 1
}

# Normalize uname values to a supported Feng platform identifier.
detect_host_platform() {
  local os arch
  case "$(uname -s)" in
    Darwin) os="macos" ;;
    Linux) os="linux" ;;
    *) die "unsupported OS for host detection: $(uname -s)" ;;
  esac

  case "$(uname -m)" in
    arm64|aarch64) arch="arm64" ;;
    x86_64|amd64) arch="x64" ;;
    *) die "unsupported architecture for host detection: $(uname -m)" ;;
  esac

  printf '%s-%s' "${os}" "${arch}"
}

# Validate and map a supported platform to the official LLVM asset fragment.
platform_to_asset_fragment() {
  case "$1" in
    macos-arm64) printf 'macOS-ARM64' ;;
    linux-arm64) printf 'Linux-ARM64' ;;
    linux-x64) printf 'Linux-X64' ;;
    *) die "unsupported LLVM platform: $1" ;;
  esac
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
      --llvm-root)
        [[ $# -ge 2 ]] || die "--llvm-root requires a value"
        LLVM_ROOT="$2"
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

# Ensure a command required by the trimming implementation is available.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

# Return the single clang resource directory from an official LLVM root.
find_clang_resource_dir() {
  local candidate found=""
  [[ -d "${LLVM_ROOT}/lib/clang" ]] || die "clang resource root not found: ${LLVM_ROOT}/lib/clang"

  for candidate in "${LLVM_ROOT}/lib/clang"/*; do
    [[ -d "${candidate}" ]] || continue
    if [[ -n "${found}" ]]; then
      die "multiple clang resource versions found under ${LLVM_ROOT}/lib/clang"
    fi
    found="${candidate}"
  done

  [[ -n "${found}" ]] || die "clang resource directory not found under ${LLVM_ROOT}/lib/clang"
  printf '%s' "${found}"
}

# Check that a binary's format and CPU match the selected LLVM platform.
verify_binary_platform() {
  local binary="$1" description
  description="$(file -b "${binary}")"

  case "${PLATFORM}" in
    macos-arm64)
      [[ "${description}" == *"Mach-O 64-bit"* && "${description}" == *"arm64"* ]] ||
        die "${binary} does not match ${PLATFORM}: ${description}"
      ;;
    linux-x64)
      [[ "${description}" == *"ELF 64-bit"* && "${description}" == *"x86-64"* ]] ||
        die "${binary} does not match ${PLATFORM}: ${description}"
      ;;
    linux-arm64)
      [[ "${description}" == *"ELF 64-bit"* && ( "${description}" == *"ARM aarch64"* || "${description}" == *"ARM64"* ) ]] ||
        die "${binary} does not match ${PLATFORM}: ${description}"
      ;;
  esac
}

# Validate all mandatory upstream inputs before touching the current output.
validate_source_tree() {
  local required resource_dir
  [[ -d "${LLVM_ROOT}" ]] || die "LLVM extracted root not found: ${LLVM_ROOT}"

  for required in clang lld ld.lld lldb lldb-dap lldb-argdumper; do
    [[ -f "${LLVM_ROOT}/bin/${required}" ]] || die "required upstream file not found: bin/${required}"
    verify_binary_platform "${LLVM_ROOT}/bin/${required}"
  done

  case "${PLATFORM}" in
    macos-arm64)
      [[ -f "${LLVM_ROOT}/bin/debugserver" ]] || die "required upstream file not found: bin/debugserver"
      verify_binary_platform "${LLVM_ROOT}/bin/debugserver"
      ;;
    linux-*)
      [[ -f "${LLVM_ROOT}/bin/lldb-server" ]] || die "required upstream file not found: bin/lldb-server"
      verify_binary_platform "${LLVM_ROOT}/bin/lldb-server"
      ;;
  esac

  resource_dir="$(find_clang_resource_dir)"
  [[ -d "${resource_dir}/include" ]] || die "clang builtin headers not found: ${resource_dir}/include"

  case "${PLATFORM}" in
    macos-arm64)
      [[ -f "${LLVM_ROOT}/lib/liblldb.${LLVM_VERSION}.dylib" ]] ||
        die "required LLDB library not found: lib/liblldb.${LLVM_VERSION}.dylib"
      ;;
    linux-*)
      [[ -e "${LLVM_ROOT}/lib/liblldb.so" ]] || die "required LLDB library not found: lib/liblldb.so"
      ;;
  esac
}

# Copy an executable into the staged output. Official command aliases such as
# bin/clang -> clang-22 are dereferenced so the minimal tree stays self-contained.
copy_executable() {
  local name="$1"
  cp -L "${LLVM_ROOT}/bin/${name}" "${STAGING_DIR}/bin/${name}"
  chmod 0755 "${STAGING_DIR}/bin/${name}"
}

# Copy clang and its complete builtin-header resource set.
trim_clang() {
  local resource_dir resource_version runtime_dir runtime_file copied_runtime=0
  resource_dir="$(find_clang_resource_dir)"
  resource_version="$(basename "${resource_dir}")"

  copy_executable clang
  mkdir -p "${STAGING_DIR}/lib/clang/${resource_version}"
  cp -R "${resource_dir}/include" "${STAGING_DIR}/lib/clang/${resource_version}/include"

  # Keep only compiler-rt files required for ordinary linking. Sanitizer,
  # profiler, fuzzer and XRay runtimes are development tools and are excluded.
  case "${PLATFORM}" in
    macos-arm64)
      runtime_dir="${resource_dir}/lib/darwin"
      runtime_file="${runtime_dir}/libclang_rt.osx.a"
      if [[ -f "${runtime_file}" ]]; then
        mkdir -p "${STAGING_DIR}/lib/clang/${resource_version}/lib/darwin"
        cp "${runtime_file}" "${STAGING_DIR}/lib/clang/${resource_version}/lib/darwin/"
        copied_runtime=1
      fi
      [[ ${copied_runtime} -eq 1 ]] || die "required clang runtime not found: ${runtime_file}"
      ;;
    linux-*)
      # Official Linux prebuilts may rely on the host GCC runtime or ship a
      # basic compiler-rt archive. Preserve only builtins and CRT objects when
      # present, retaining their upstream target-directory names.
      if [[ -d "${resource_dir}/lib" ]]; then
        while IFS= read -r -d '' runtime_file; do
          runtime_dir="${STAGING_DIR}/lib/clang/${resource_version}/lib/${runtime_file#"${resource_dir}/lib/"}"
          mkdir -p "$(dirname "${runtime_dir}")"
          cp -P "${runtime_file}" "${runtime_dir}"
          copied_runtime=1
        done < <(find "${resource_dir}/lib" \( -type f -o -type l \) \
          \( -name 'libclang_rt.builtins*.a' -o -name 'clang_rt.crtbegin*.o' -o -name 'clang_rt.crtend*.o' \) \
          -print0)
      fi
      ;;
  esac
}

# Copy the LLD driver and preserve the Unix driver name used by
# clang -fuse-ld=lld. The official package ships bin/ld.lld -> lld.
trim_lld() {
  copy_executable lld
  ln -s lld "${STAGING_DIR}/bin/ld.lld"
}

# Copy LLDB executables, its shared library, and optional upstream support data.
trim_lldb() {
  local library matched=0
  copy_executable lldb
  copy_executable lldb-dap
  copy_executable lldb-argdumper

  case "${PLATFORM}" in
    macos-arm64)
      copy_executable debugserver
      for library in "${LLVM_ROOT}/lib/"liblldb*.dylib; do
        [[ -e "${library}" || -L "${library}" ]] || continue
        cp -P "${library}" "${STAGING_DIR}/lib/"
        matched=1
      done
      ;;
    linux-*)
      copy_executable lldb-server
      for library in "${LLVM_ROOT}/lib/"liblldb.so*; do
        [[ -e "${library}" || -L "${library}" ]] || continue
        cp -P "${library}" "${STAGING_DIR}/lib/"
        matched=1
      done
      ;;
  esac
  [[ ${matched} -eq 1 ]] || die "no LLDB shared library copied for ${PLATFORM}"

  # Preserve Python support only when the official package supplies it. Linux
  # system libpython dependencies remain system prerequisites and are not
  # synthesized by this script.
  if [[ -d "${LLVM_ROOT}/lib/Python3.framework" ]]; then
    cp -R "${LLVM_ROOT}/lib/Python3.framework" "${STAGING_DIR}/lib/"
  fi
  if [[ -d "${LLVM_ROOT}/lib/lldb" ]]; then
    cp -R "${LLVM_ROOT}/lib/lldb" "${STAGING_DIR}/lib/"
  fi
  if [[ -d "${LLVM_ROOT}/share/lldb" ]]; then
    mkdir -p "${STAGING_DIR}/share"
    cp -R "${LLVM_ROOT}/share/lldb" "${STAGING_DIR}/share/"
  fi
}

# Write provenance and a precise high-level inclusion policy into the output.
write_metadata() {
  local asset_fragment resource_dir resource_version
  asset_fragment="$(platform_to_asset_fragment "${PLATFORM}")"
  resource_dir="$(find_clang_resource_dir)"
  resource_version="$(basename "${resource_dir}")"

  if [[ -f "${LLVM_ROOT}/LICENSE.TXT" ]]; then
    cp "${LLVM_ROOT}/LICENSE.TXT" "${STAGING_DIR}/LICENSE.TXT"
  fi

  cat >"${STAGING_DIR}/README.md" <<EOF
# LLVM minimal binary closure

This directory vendors a slimmed official LLVM toolchain for Feng.

Version: ${LLVM_VERSION}
Platform: ${PLATFORM}
Source: https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-${asset_fragment}.tar.xz

Included:
- bin/clang
- bin/lld and bin/ld.lld -> lld
- bin/lldb and bin/lldb-dap
- bin/lldb-argdumper
- bin/debugserver (macOS) or bin/lldb-server (Linux)
- lib/clang/${resource_version}/include
- the basic clang runtime files required for ordinary linking when supplied
- liblldb shared-library files and their symlink chain
- upstream LLDB/Python support directories only when supplied by the package
- LICENSE.TXT when supplied by the package

Excluded:
- unrelated LLVM executables and development headers/libraries
- clang tooling, C++ standard-library material and editor integrations
- sanitizer, profiler, fuzzer and XRay runtimes
- lldb-mi and the LLDB C++ embedding API

Re-sync:
  ./scripts/fetch_llvm.sh --platform ${PLATFORM}
  ./scripts/trim_llvm.sh --platform ${PLATFORM}
EOF

  if [[ "${PLATFORM}" == linux-* ]]; then
    cat >>"${STAGING_DIR}/README.md" <<'EOF'

Linux runtime prerequisites:
- The official LLVM Linux binaries retain their system shared-library
  dependencies. In particular, liblldb directly requires
  libpython3.11.so.1.0; it is not included in the official LLVM archive and
  must be installed by the user together with the other distribution runtime
  libraries required by the official binaries.
EOF
  fi
}

# Reject broken relative or absolute symlinks in the staged tree.
verify_symlinks() {
  local link
  while IFS= read -r -d '' link; do
    [[ -e "${link}" ]] || die "staged output contains a broken symlink: ${link}"
  done < <(find "${STAGING_DIR}" -type l -print0)
}

# Run executable smoke checks only when the selected LLVM package matches host.
verify_native_executables() {
  local host_platform
  host_platform="$(detect_host_platform)"
  if [[ "${host_platform}" != "${PLATFORM}" ]]; then
    echo "==> Static validation only; ${PLATFORM} binaries cannot run on ${host_platform}"
    return
  fi

  "${STAGING_DIR}/bin/clang" --version >/dev/null
  "${STAGING_DIR}/bin/ld.lld" --version >/dev/null
  "${STAGING_DIR}/bin/lldb" --version >/dev/null
  "${STAGING_DIR}/bin/lldb-dap" --version >/dev/null
}

# Remove staging state and restore the previous output after an interrupted swap.
cleanup_on_failure() {
  local status=$?
  trap - EXIT
  if [[ ${status} -ne 0 ]]; then
    if [[ -n "${STAGING_DIR}" && -d "${STAGING_DIR}" ]]; then
      rm -rf "${STAGING_DIR}"
    fi
    if [[ -n "${BACKUP_DIR}" && -d "${BACKUP_DIR}" && ! -e "${OUTPUT_DIR}" ]]; then
      mv "${BACKUP_DIR}" "${OUTPUT_DIR}"
    fi
  fi
  exit "${status}"
}

# Replace one platform directory only after the complete staged tree validates.
install_staged_output() {
  OUTPUT_DIR="${OUTPUT_PARENT}/${PLATFORM}"
  BACKUP_DIR="${OUTPUT_PARENT}/.${PLATFORM}.backup.$$"
  [[ ! -e "${BACKUP_DIR}" ]] || die "temporary backup path already exists: ${BACKUP_DIR}"

  if [[ -e "${OUTPUT_DIR}" ]]; then
    mv "${OUTPUT_DIR}" "${BACKUP_DIR}"
  fi
  mv "${STAGING_DIR}" "${OUTPUT_DIR}"
  STAGING_DIR=""

  if [[ -d "${BACKUP_DIR}" ]]; then
    rm -rf "${BACKUP_DIR}"
  fi
  BACKUP_DIR=""
}

# Coordinate validation, trimming and the final output swap.
main() {
  local asset_fragment default_root
  parse_args "$@"
  PLATFORM="${PLATFORM:-$(detect_host_platform)}"
  asset_fragment="$(platform_to_asset_fragment "${PLATFORM}")"
  default_root="${PROJECT_ROOT}/local/llvm/LLVM-${LLVM_VERSION}-${asset_fragment}"
  LLVM_ROOT="${LLVM_ROOT:-${default_root}}"

  require_cmd basename
  require_cmd cp
  require_cmd dirname
  require_cmd file
  require_cmd find
  require_cmd ln
  require_cmd mktemp
  require_cmd mv

  validate_source_tree

  mkdir -p "${OUTPUT_PARENT}"
  STAGING_DIR="$(mktemp -d "${OUTPUT_PARENT}/.${PLATFORM}.staging.XXXXXX")"
  trap cleanup_on_failure EXIT
  mkdir -p "${STAGING_DIR}/bin" "${STAGING_DIR}/lib"

  echo "==> Trimming LLVM ${LLVM_VERSION} for ${PLATFORM}"
  echo "==> Source: ${LLVM_ROOT}"
  echo "==> Output: ${OUTPUT_PARENT}/${PLATFORM}"

  trim_clang
  trim_lld
  trim_lldb
  write_metadata
  verify_symlinks
  verify_native_executables
  install_staged_output

  trap - EXIT
  echo "==> Done: ${OUTPUT_DIR}"
}

main "$@"
