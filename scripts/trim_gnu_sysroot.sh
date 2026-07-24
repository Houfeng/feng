#!/usr/bin/env bash
set -euo pipefail

# Construct relocatable GNU/glibc target sysroots from the pinned Debian cross
# packages prepared by fetch_gnu_sysroot.sh. Only target headers, libraries,
# CRT objects, compiler runtime files, licenses and provenance are retained.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CACHE_DIR="${PROJECT_ROOT}/local/sysroot/gnu"
OUTPUT_PARENT="${PROJECT_ROOT}/toolchain/sysroot"
PLATFORM=""
STAGING_DIR=""
SOURCE_DIR=""
BACKUP_DIR=""
OUTPUT_DIR=""

# Print the supported command-line interface.
usage() {
  cat <<'EOF'
Usage: scripts/trim_gnu_sysroot.sh [options]

Options:
  --platform <platform>  Trim one target platform. When omitted, trim both.
                         Supported: linux-x64-gnu, linux-arm64-gnu.
  -h, --help             Show this help.

Examples:
  ./scripts/trim_gnu_sysroot.sh
  ./scripts/trim_gnu_sysroot.sh --platform linux-x64-gnu
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
      -h|--help)
        usage
        exit 0
        ;;
      *) die "unknown argument: $1" ;;
    esac
  done
}

# Return one target's GNU triple.
platform_to_triple() {
  case "$1" in
    linux-x64-gnu) printf 'x86_64-linux-gnu' ;;
    linux-arm64-gnu) printf 'aarch64-linux-gnu' ;;
    *) die "unsupported GNU sysroot platform: $1" ;;
  esac
}

# Return one target's glibc dynamic loader name.
platform_to_loader() {
  case "$1" in
    linux-x64-gnu) printf 'ld-linux-x86-64.so.2' ;;
    linux-arm64-gnu) printf 'ld-linux-aarch64.so.1' ;;
    *) die "unsupported GNU sysroot platform: $1" ;;
  esac
}

# Extract the data member of one Debian binary package into an overlay root.
extract_deb() {
  local package="$1"
  local destination="$2"
  ar p "${package}" data.tar.xz | tar -xJf - -C "${destination}"
}

# Validate and extract exactly the pinned packages for one target.
extract_target_packages() {
  local platform="$1"
  local entry package_platform filename snapshot_hash expected_sha256 package
  local package_count=0

  while IFS='|' read -r package_platform filename snapshot_hash expected_sha256; do
    [[ "${package_platform}" == "${platform}" ]] || continue
    package="${CACHE_DIR}/${platform}/${filename}"
    [[ -f "${package}" ]] ||
      die "GNU sysroot input missing: ${package}; run scripts/fetch_gnu_sysroot.sh first"
    [[ "$(sha256_file "${package}")" == "${expected_sha256}" ]] ||
      die "GNU sysroot input checksum mismatch: ${package}"
    extract_deb "${package}" "${SOURCE_DIR}"
    package_count=$((package_count + 1))
  done < <("${SCRIPT_DIR}/fetch_gnu_sysroot.sh" --print-manifest)

  [[ ${package_count} -eq 5 ]] ||
    die "expected five pinned packages for ${platform}, found ${package_count}"
}

# Copy the public target C environment while excluding package-manager data.
copy_target_environment() {
  local triple="$1"
  local source_target="${SOURCE_DIR}/usr/${triple}"

  [[ -d "${source_target}/include" ]] ||
    die "target include directory missing: ${source_target}/include"
  [[ -d "${source_target}/lib" ]] ||
    die "target library directory missing: ${source_target}/lib"

  mkdir -p "${STAGING_DIR}/usr/${triple}"
  cp -R "${source_target}/include" "${STAGING_DIR}/usr/${triple}/include"
  cp -R "${source_target}/lib" "${STAGING_DIR}/usr/${triple}/lib"
  if [[ -d "${source_target}/lib64" ]]; then
    cp -R "${source_target}/lib64" "${STAGING_DIR}/usr/${triple}/lib64"
  fi

  # These glibc diagnostics and debugger helpers are runtime utilities rather
  # than compile/link inputs and are not part of the target sysroot.
  rm -f "${STAGING_DIR}/usr/${triple}/lib/libSegFault.so"
  rm -f "${STAGING_DIR}/usr/${triple}/lib/libmemusage.so"
  rm -f "${STAGING_DIR}/usr/${triple}/lib/libpcprofile.so"
  rm -f "${STAGING_DIR}/usr/${triple}/lib/"libthread_db*
}

# Copy only the GCC target CRT and runtime archives consumed by Clang.
copy_compiler_runtime() {
  local triple="$1"
  local source_runtime="${SOURCE_DIR}/usr/lib/gcc-cross/${triple}/10"
  local destination="${STAGING_DIR}/lib/gcc/${triple}/10"
  local required
  local runtime_files=(
    crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o
    libgcc.a libgcc_eh.a libgcc_s.so
  )

  [[ -d "${source_runtime}" ]] ||
    die "GCC target runtime directory missing: ${source_runtime}"
  mkdir -p "${destination}"
  for required in "${runtime_files[@]}"; do
    [[ -f "${source_runtime}/${required}" ]] ||
      die "required GCC target runtime missing: ${source_runtime}/${required}"
    cp "${source_runtime}/${required}" "${destination}/${required}"
  done
}

# Create stable views used by Clang, linker scripts and the ELF interpreter.
create_directory_views() {
  local platform="$1"
  local triple="$2"
  local loader="$3"

  ln -s "${triple}/include" "${STAGING_DIR}/usr/include"
  ln -s "${triple}/lib" "${STAGING_DIR}/usr/lib"
  ln -s "usr/${triple}" "${STAGING_DIR}/${triple}"
  ln -s "../usr/${triple}/lib/${loader}" "${STAGING_DIR}/lib/${loader}"

  if [[ "${platform}" == "linux-x64-gnu" ]]; then
    ln -s "usr/${triple}/lib64" "${STAGING_DIR}/lib64"
  fi
}

# Preserve the exact license texts shipped by the selected Debian packages.
copy_licenses() {
  local copyright package_name
  mkdir -p "${STAGING_DIR}/LICENSES"
  while IFS= read -r -d '' copyright; do
    package_name="$(basename "$(dirname "${copyright}")")"
    cp "${copyright}" "${STAGING_DIR}/LICENSES/${package_name}.copyright"
  done < <(find "${SOURCE_DIR}/usr/share/doc" -type f -name copyright -print0)
}

# Write exact binary and source provenance into the generated sysroot.
write_metadata() {
  local platform="$1"
  local triple="$2"
  local entry package_platform filename snapshot_hash expected_sha256

  {
    echo "# Linux GNU sysroot for Feng"
    echo
    echo "Target: ${platform}"
    echo "GNU triple: ${triple}"
    echo "glibc target ABI: 2.31"
    echo "Linux userspace headers: 5.10.13"
    echo "GCC target runtime: 10.2.1"
    echo
    echo "## Binary inputs"
    echo
    while IFS='|' read -r package_platform filename snapshot_hash expected_sha256; do
      [[ "${package_platform}" == "${platform}" ]] || continue
      echo "- ${filename}"
      echo "  - URL: https://snapshot.debian.org/file/${snapshot_hash}"
      echo "  - SHA-256: ${expected_sha256}"
    done < <("${SCRIPT_DIR}/fetch_gnu_sysroot.sh" --print-manifest)
    cat <<'EOF'

## Corresponding source packages

- glibc 2.31-9
  - glibc_2.31-9.dsc: https://snapshot.debian.org/file/b80503b444cc6b04ea66a60551552fab550f3a3e
    SHA-256: 5f4848ef9d3b98e3271ec9a8077b50147d37db93575fa73a9de487b095e2973c
  - glibc_2.31.orig.tar.xz: https://snapshot.debian.org/file/026f14aa2836ff3a56b32d17e71f9ddb29a2b7ec
    SHA-256: 3dc7704b6166839c37d7047626fd199f3d4c09aca0d90e48c51c31c967dce34e
  - glibc_2.31-9.debian.tar.xz: https://snapshot.debian.org/file/c276c405aa11e3a7a4eee88a79a19fa2a7d7a3e5
    SHA-256: 4d1644f39bfbbb2eec8c3e4aceda7472ee435a7a9bf73dc2967ddde0a2e35230
- Linux 5.10.13-1
  - linux_5.10.13-1.dsc: https://snapshot.debian.org/file/c2f824d2358a0e1d48cf26c53db45480159def1c
    SHA-256: 910a5b687678ae2f3655923d411c42dac8a71a477269794154096fb4982a2f35
  - linux_5.10.13.orig.tar.xz: https://snapshot.debian.org/file/758975dac538868546abf102daf883cd7e15ed27
    SHA-256: c04de3551864e52485fd100249df430840c995c9263aaa552b1b527d48f867ca
  - linux_5.10.13-1.debian.tar.xz: https://snapshot.debian.org/file/10b82510f07d799884a23c61504767f7eeb39758
    SHA-256: 6fef4385cf488885f5ce5f9fc5b45b220f0ed426173305a5ce13683c19116ec0
- GCC 10.2.1-6
  - gcc-10_10.2.1-6.dsc: https://snapshot.debian.org/file/6b494d45f4a077c5eae80b30cc6c345973edb9a3
    SHA-256: 24024c1e225ca968f37ce39047ff5f1058219976db9e88a807173c2f07fa6029
  - gcc-10_10.2.1.orig.tar.xz: https://snapshot.debian.org/file/8ad27f9465511b4ae3b1bac4a6e03d54c050df82
    SHA-256: ea3c05faa381486e6b859c047dc14977418bf1ccda4567064e016493fd6fffec
  - gcc-10_10.2.1-6.debian.tar.xz: https://snapshot.debian.org/file/aea05e45278deb4eb189c760ca235bfe1affb34d
    SHA-256: a95d6b9da2be83f9751850b002021281411ff1003d9feb77298b131da47820b3

The complete corresponding sources remain available from the immutable Debian
Snapshot URLs above. Package license texts copied from the binary inputs are in
LICENSES/.

## Included

- usr/<triple>/include: glibc and Linux userspace public headers
- usr/<triple>/lib: glibc libraries, linker scripts, CRT and dynamic loader
- lib/gcc/<triple>/10: GCC CRT, libgcc and libgcc_eh target runtime
- relocatable directory views used by Clang and glibc linker scripts

## Excluded

- GCC, binutils, ld and every other host executable
- GCC language headers and unrelated target runtimes
- package manager metadata, maintainer scripts, changelogs and documentation
- glibc diagnostic and debugger helper shared objects

Feng invokes Clang with:
  --target=<triple>
  --sysroot=<this directory>
  --gcc-toolchain=<this directory>
  -fuse-ld=lld

Re-sync:
  ./scripts/fetch_gnu_sysroot.sh
  ./scripts/trim_gnu_sysroot.sh
EOF
  } >"${STAGING_DIR}/README.md"
}

# Check that one target ELF object or library has the selected CPU architecture.
verify_target_binary() {
  local platform="$1"
  local path="$2"
  local description
  description="$(file -b "${path}")"
  case "${platform}" in
    linux-x64-gnu)
      [[ "${description}" == *"x86-64"* ]] ||
        die "${path} does not match ${platform}: ${description}"
      ;;
    linux-arm64-gnu)
      [[ "${description}" == *"ARM aarch64"* || "${description}" == *"ARM64"* ]] ||
        die "${path} does not match ${platform}: ${description}"
      ;;
  esac
}

# Verify the generated target environment and reject broken or host content.
verify_staged_output() {
  local platform="$1"
  local triple="$2"
  local loader="$3"
  local required link elf description
  local required_files=(
    usr/include/stdio.h
    usr/include/linux/version.h
    "usr/${triple}/lib/crt1.o"
    "usr/${triple}/lib/Scrt1.o"
    "usr/${triple}/lib/crti.o"
    "usr/${triple}/lib/crtn.o"
    "usr/${triple}/lib/libc.so"
    "usr/${triple}/lib/libc.so.6"
    "usr/${triple}/lib/${loader}"
    "usr/${triple}/lib/libgcc_s.so.1"
    "lib/gcc/${triple}/10/crtbegin.o"
    "lib/gcc/${triple}/10/crtend.o"
    "lib/gcc/${triple}/10/libgcc.a"
    "lib/gcc/${triple}/10/libgcc_eh.a"
  )

  for required in "${required_files[@]}"; do
    [[ -f "${STAGING_DIR}/${required}" ]] ||
      die "staged GNU sysroot file missing: ${required}"
  done
  verify_target_binary "${platform}" "${STAGING_DIR}/usr/${triple}/lib/crt1.o"
  verify_target_binary "${platform}" "${STAGING_DIR}/usr/${triple}/lib/libc.so.6"
  verify_target_binary "${platform}" "${STAGING_DIR}/usr/${triple}/lib/libgcc_s.so.1"

  while IFS= read -r -d '' link; do
    [[ -e "${link}" ]] || die "staged GNU sysroot has a broken symlink: ${link}"
  done < <(find "${STAGING_DIR}" -type l -print0)

  while IFS= read -r -d '' elf; do
    description="$(file -b "${elf}")"
    [[ "${description}" == *"ELF"* ]] || continue
    verify_target_binary "${platform}" "${elf}"
  done < <(find "${STAGING_DIR}" -type f -print0)

  for required in bin sbin usr/bin usr/sbin usr/libexec; do
    [[ ! -e "${STAGING_DIR}/${required}" ]] ||
      die "host executable directory unexpectedly retained: ${required}"
  done
}

# Restore the previous output after a failed staged replacement.
cleanup_on_failure() {
  local status=$?
  trap - EXIT
  if [[ -n "${SOURCE_DIR}" && -d "${SOURCE_DIR}" ]]; then
    rm -rf "${SOURCE_DIR}"
  fi
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

# Atomically replace one target output after all staged checks pass.
install_staged_output() {
  local platform="$1"
  OUTPUT_DIR="${OUTPUT_PARENT}/${platform}"
  BACKUP_DIR="${OUTPUT_PARENT}/.${platform}.backup.$$"
  [[ ! -e "${BACKUP_DIR}" ]] || die "temporary backup path exists: ${BACKUP_DIR}"

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

# Construct and atomically install one target sysroot.
trim_one() {
  local platform="$1"
  local triple loader
  triple="$(platform_to_triple "${platform}")"
  loader="$(platform_to_loader "${platform}")"

  echo "==> Trimming GNU sysroot for ${platform}"
  mkdir -p "${OUTPUT_PARENT}"
  SOURCE_DIR="$(mktemp -d "${OUTPUT_PARENT}/.${platform}.source.XXXXXX")"
  STAGING_DIR="$(mktemp -d "${OUTPUT_PARENT}/.${platform}.staging.XXXXXX")"
  trap cleanup_on_failure EXIT
  mkdir -p "${STAGING_DIR}/usr" "${STAGING_DIR}/lib"

  extract_target_packages "${platform}"
  copy_target_environment "${triple}"
  copy_compiler_runtime "${triple}"
  create_directory_views "${platform}" "${triple}" "${loader}"
  copy_licenses
  write_metadata "${platform}" "${triple}"
  verify_staged_output "${platform}" "${triple}" "${loader}"
  rm -rf "${SOURCE_DIR}"
  SOURCE_DIR=""
  install_staged_output "${platform}"
  trap - EXIT

  echo "==> Done: ${OUTPUT_DIR}"
  OUTPUT_DIR=""
}

# Validate prerequisites and construct the selected target set.
main() {
  local target
  parse_args "$@"
  case "${PLATFORM}" in
    ""|linux-x64-gnu|linux-arm64-gnu) ;;
    *) die "unsupported GNU sysroot platform: ${PLATFORM}" ;;
  esac

  require_cmd ar
  require_cmd awk
  require_cmd cp
  require_cmd file
  require_cmd find
  require_cmd ln
  require_cmd mktemp
  require_cmd mv
  require_cmd tar
  if ! command -v sha256sum >/dev/null 2>&1; then
    require_cmd shasum
  fi

  if [[ -n "${PLATFORM}" ]]; then
    trim_one "${PLATFORM}"
  else
    for target in linux-x64-gnu linux-arm64-gnu; do
      trim_one "${target}"
    done
  fi
  echo "==> All selected GNU sysroots trimmed."
}

main "$@"
