#!/usr/bin/env bash
set -euo pipefail

# Trim the musl.cc cross-toolchain archives prepared by fetch_musl.sh into
# toolchain/sysroot/<os>-<arch>-musl/. Feng uses bundled Clang and LLD, so the
# GCC/binutils host executables are excluded. The target musl C environment
# and the GCC runtime closure required by Clang are retained.
#
# Produced layout:
#   toolchain/sysroot/<os>-<arch>-musl/
#     usr/include/                         musl public headers
#     usr/lib/                             musl libraries and CRT objects
#     lib/gcc/<musl-triple>/<gcc-version>/ GCC CRT and runtime archives
#     README.md                            provenance and license summary
#
# The lib/gcc hierarchy is intentional. When Feng passes both
# --sysroot=<this directory> and --gcc-toolchain=<this directory>, Clang's GCC
# installation detector finds crtbegin*, crtend* and libgcc without bespoke
# -B or -L paths.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CACHE_DIR="${PROJECT_ROOT}/local/musl"
OUTPUT_PARENT="${PROJECT_ROOT}/toolchain/sysroot"
STAGING_DIR=""
BACKUP_DIR=""
OUTPUT_DIR=""

# Each entry is <feng-target>|<archive>|<extracted-root>|<musl-triple>.
TARGETS=(
  "linux-arm64-musl|aarch64-linux-musl-cross.tgz|aarch64-linux-musl-cross|aarch64-linux-musl"
  "linux-x64-musl|x86_64-linux-musl-cross.tgz|x86_64-linux-musl-cross|x86_64-linux-musl"
)

# Fail with a consistent diagnostic.
die() {
  echo "error: $*" >&2
  exit 1
}

# Ensure a command required by the trimming implementation is available.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

# Return the single GCC version directory supplied for one musl triple.
find_gcc_runtime_dir() {
  local extracted_root="$1"
  local musl_triple="$2"
  local candidate found=""
  local gcc_root="${extracted_root}/lib/gcc/${musl_triple}"

  [[ -d "${gcc_root}" ]] || die "GCC runtime root not found: ${gcc_root}"
  for candidate in "${gcc_root}"/*; do
    [[ -d "${candidate}" ]] || continue
    if [[ -n "${found}" ]]; then
      die "multiple GCC runtime versions found under ${gcc_root}"
    fi
    found="${candidate}"
  done
  [[ -n "${found}" ]] || die "GCC runtime version not found under ${gcc_root}"
  printf '%s' "${found}"
}

# Check that one target object or shared library matches the selected target.
verify_target_binary() {
  local target="$1"
  local path="$2"
  local description
  description="$(file -b "${path}")"

  case "${target}" in
    linux-arm64-musl)
      [[ "${description}" == *"ARM aarch64"* || "${description}" == *"ARM64"* ]] ||
        die "${path} does not match ${target}: ${description}"
      ;;
    linux-x64-musl)
      [[ "${description}" == *"x86-64"* ]] ||
        die "${path} does not match ${target}: ${description}"
      ;;
    *) die "unsupported sysroot target: ${target}" ;;
  esac
}

# Validate the complete source closure before creating staged output.
validate_source_tree() {
  local extracted_root="$1"
  local musl_triple="$2"
  local sysroot_src="${extracted_root}/${musl_triple}"
  local gcc_runtime_dir required

  [[ -d "${extracted_root}" ]] || die "extracted musl root not found: ${extracted_root}"
  [[ -d "${sysroot_src}/include" ]] || die "musl include directory not found: ${sysroot_src}/include"
  [[ -d "${sysroot_src}/lib" ]] || die "musl library directory not found: ${sysroot_src}/lib"

  for required in stdio.h stdlib.h string.h; do
    [[ -f "${sysroot_src}/include/${required}" ]] ||
      die "required musl header not found: ${sysroot_src}/include/${required}"
  done
  for required in libc.a libc.so crt1.o crti.o crtn.o Scrt1.o rcrt1.o \
                  libgcc_s.so libgcc_s.so.1; do
    [[ -f "${sysroot_src}/lib/${required}" ]] ||
      die "required target library file not found: ${sysroot_src}/lib/${required}"
  done

  gcc_runtime_dir="$(find_gcc_runtime_dir "${extracted_root}" "${musl_triple}")"
  for required in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o \
                  libgcc.a libgcc_eh.a; do
    [[ -f "${gcc_runtime_dir}/${required}" ]] ||
      die "required GCC runtime file not found: ${gcc_runtime_dir}/${required}"
  done
}

# Copy all public musl headers, excluding any accidental C++ subtree.
copy_musl_headers() {
  local sysroot_src="$1"
  local entry

  mkdir -p "${STAGING_DIR}/usr/include"
  while IFS= read -r -d '' entry; do
    cp -R "${entry}" "${STAGING_DIR}/usr/include/"
  done < <(find "${sysroot_src}/include" -mindepth 1 -maxdepth 1 ! -name c++ -print0)
}

# Copy the target musl libraries and startup objects needed for C linking.
copy_musl_libraries() {
  local sysroot_src="$1"
  local required loader loader_name
  local musl_files=(
    libc.a libc.so libm.a librt.a libpthread.a libdl.a libcrypt.a
    libresolv.a libutil.a libxnet.a
    crt1.o crti.o crtn.o Scrt1.o rcrt1.o
    libgcc_s.so libgcc_s.so.1
  )

  mkdir -p "${STAGING_DIR}/usr/lib"
  for required in "${musl_files[@]}"; do
    [[ -f "${sysroot_src}/lib/${required}" ]] || continue
    cp "${sysroot_src}/lib/${required}" "${STAGING_DIR}/usr/lib/"
  done

  loader=""
  while IFS= read -r -d '' required; do
    [[ -z "${loader}" ]] || die "multiple musl dynamic loaders found under ${sysroot_src}/lib"
    loader="${required}"
  done < <(find "${sysroot_src}/lib" -maxdepth 1 -name 'ld-musl-*.so.1' -print0)
  [[ -n "${loader}" ]] || die "musl dynamic loader not found under ${sysroot_src}/lib"

  # The upstream loader is an absolute /lib/libc.so symlink. Materialize an
  # equivalent relative link inside the relocatable sysroot instead of
  # preserving a host-visible broken absolute symlink.
  loader_name="$(basename "${loader}")"
  ln -s libc.so "${STAGING_DIR}/usr/lib/${loader_name}"
}

# Copy the minimal GCC runtime hierarchy consumed by Clang's detector.
copy_gcc_runtime() {
  local extracted_root="$1"
  local musl_triple="$2"
  local gcc_runtime_dir gcc_version target_dir required
  local runtime_files=(
    crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o
    libgcc.a libgcc_eh.a
  )

  gcc_runtime_dir="$(find_gcc_runtime_dir "${extracted_root}" "${musl_triple}")"
  gcc_version="$(basename "${gcc_runtime_dir}")"
  target_dir="${STAGING_DIR}/lib/gcc/${musl_triple}/${gcc_version}"
  mkdir -p "${target_dir}"
  for required in "${runtime_files[@]}"; do
    cp "${gcc_runtime_dir}/${required}" "${target_dir}/"
  done
}

# Extract the musl revision string embedded by the musl.cc build.
detect_musl_version() {
  local libc="$1"
  strings "${libc}" | awk '/^1\.[0-9]+\.[0-9]+(-git-[0-9]+-g[0-9a-f]+)?$/ && !found { print; found=1 }'
}

# Record separate provenance and licenses for musl and the GCC runtime.
write_metadata() {
  local target="$1"
  local archive_name="$2"
  local extracted_root="$3"
  local musl_triple="$4"
  local gcc_runtime_dir gcc_version musl_version

  gcc_runtime_dir="$(find_gcc_runtime_dir "${extracted_root}" "${musl_triple}")"
  gcc_version="$(basename "${gcc_runtime_dir}")"
  musl_version="$(detect_musl_version "${extracted_root}/${musl_triple}/lib/libc.so")"
  [[ -n "${musl_version}" ]] || die "unable to detect musl version from ${musl_triple}/lib/libc.so"

  cat >"${STAGING_DIR}/README.md" <<EOF
# Linux musl sysroot for Feng

Target: ${target}
Source archive: https://musl.cc/${archive_name}
musl version: ${musl_version}
musl license: MIT (https://musl.libc.org/about.html)
GCC runtime version: ${gcc_version}
GCC runtime license: GPL-3.0-or-later WITH GCC-exception-3.1
GCC Runtime Library Exception: https://www.gnu.org/licenses/gcc-exception-3.1.html

Included:
- usr/include/: musl public C and Linux userspace headers
- usr/lib/: musl C libraries, dynamic libc, loader link and startup objects
- usr/lib/libgcc_s.so and libgcc_s.so.1: target GCC shared runtime
- lib/gcc/${musl_triple}/${gcc_version}/: crtbegin*, crtend*, libgcc.a and
  libgcc_eh.a required by Clang's GCC installation detector

Excluded:
- GCC and binutils host executables
- GCC internal executables and plugins
- C++/Fortran/OpenMP/atomic/transactional-memory libraries and headers
- libtool archives, GCC specs, linker scripts and unrelated GCC runtimes

Feng invokes Clang with:
  --target=<target-musl-triple>
  --sysroot=<this directory>
  --gcc-toolchain=<this directory>
  -fuse-ld=lld

Re-sync:
  ./scripts/fetch_musl.sh
  ./scripts/trim_musl.sh
EOF
}

# Verify staged structure, target architecture and symlink integrity.
verify_staged_output() {
  local target="$1"
  local musl_triple="$2"
  local extracted_root="$3"
  local gcc_runtime_dir gcc_version required link

  gcc_runtime_dir="$(find_gcc_runtime_dir "${extracted_root}" "${musl_triple}")"
  gcc_version="$(basename "${gcc_runtime_dir}")"

  for required in usr/include/stdio.h usr/lib/libc.a usr/lib/libc.so \
                  usr/lib/crt1.o usr/lib/crti.o usr/lib/crtn.o \
                  usr/lib/libgcc_s.so usr/lib/libgcc_s.so.1 \
                  "lib/gcc/${musl_triple}/${gcc_version}/crtbegin.o" \
                  "lib/gcc/${musl_triple}/${gcc_version}/crtend.o" \
                  "lib/gcc/${musl_triple}/${gcc_version}/libgcc.a" \
                  "lib/gcc/${musl_triple}/${gcc_version}/libgcc_eh.a"; do
    [[ -f "${STAGING_DIR}/${required}" ]] || die "staged sysroot file missing: ${required}"
  done

  verify_target_binary "${target}" "${STAGING_DIR}/usr/lib/crt1.o"
  verify_target_binary "${target}" "${STAGING_DIR}/usr/lib/libc.so"
  verify_target_binary "${target}" "${STAGING_DIR}/usr/lib/libgcc_s.so.1"

  while IFS= read -r -d '' link; do
    [[ -e "${link}" ]] || die "staged sysroot contains a broken symlink: ${link}"
  done < <(find "${STAGING_DIR}" -type l -print0)
}

# Restore the previous target output if staging or installation fails.
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

# Atomically replace one target sysroot after all staged checks pass.
install_staged_output() {
  local target="$1"
  OUTPUT_DIR="${OUTPUT_PARENT}/${target}"
  BACKUP_DIR="${OUTPUT_PARENT}/.${target}.backup.$$"
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

# Trim, validate and install one target sysroot.
trim_one() {
  local target="$1"
  local archive_name="$2"
  local extracted_root_name="$3"
  local musl_triple="$4"
  local extracted_root="${CACHE_DIR}/${extracted_root_name}"
  local sysroot_src="${extracted_root}/${musl_triple}"

  echo "==> Trimming musl/GCC sysroot for ${target}"
  validate_source_tree "${extracted_root}" "${musl_triple}"

  mkdir -p "${OUTPUT_PARENT}"
  STAGING_DIR="$(mktemp -d "${OUTPUT_PARENT}/.${target}.staging.XXXXXX")"
  trap cleanup_on_failure EXIT

  copy_musl_headers "${sysroot_src}"
  copy_musl_libraries "${sysroot_src}"
  copy_gcc_runtime "${extracted_root}" "${musl_triple}"
  write_metadata "${target}" "${archive_name}" "${extracted_root}" "${musl_triple}"
  verify_staged_output "${target}" "${musl_triple}" "${extracted_root}"
  install_staged_output "${target}"

  trap - EXIT
  echo "==> Done: ${OUTPUT_DIR}"
  OUTPUT_DIR=""
}

# Validate prerequisites and trim both supported Linux targets.
main() {
  local entry target archive_name extracted_root_name musl_triple
  require_cmd awk
  require_cmd basename
  require_cmd cp
  require_cmd file
  require_cmd find
  require_cmd ln
  require_cmd mktemp
  require_cmd mv
  require_cmd strings

  for entry in "${TARGETS[@]}"; do
    IFS='|' read -r target archive_name extracted_root_name musl_triple <<<"${entry}"
    trim_one "${target}" "${archive_name}" "${extracted_root_name}" "${musl_triple}"
  done
  echo "==> All musl/GCC sysroots trimmed."
}

main "$@"
