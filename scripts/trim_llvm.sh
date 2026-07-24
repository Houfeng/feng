#!/usr/bin/env bash
set -euo pipefail

# Trim one official LLVM prebuilt into toolchain/llvm/<host-platform>/.
# Linux private libraries are resolved recursively from DT_NEEDED against the
# pinned packages prepared by fetch_llvm.sh. This script performs no downloads.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LLVM_VERSION="${LLVM_VERSION:-22.1.8}"
PLATFORM=""
LLVM_ROOT=""
OUTPUT_PARENT="${PROJECT_ROOT}/toolchain/llvm"
RUNTIME_CACHE="${PROJECT_ROOT}/local/llvm/runtime"
STAGING_DIR=""
RUNTIME_SOURCE=""
BACKUP_DIR=""
OUTPUT_DIR=""
READELF=""

# Print the supported command-line interface.
usage() {
  cat <<'EOF'
Usage: scripts/trim_llvm.sh [options]

Options:
  --platform <platform>  LLVM host platform. Defaults to the current host.
                         Supported: macos-arm64, linux-x64-gnu,
                         linux-arm64-gnu.
  --llvm-root <path>     Extracted LLVM root. Defaults to the matching
                         local/llvm/LLVM-<version>-<asset> directory.
  -h, --help             Show this help.

Environment:
  LLVM_VERSION           Upstream LLVM version. Default: 22.1.8.

Linux maintenance dependencies:
  ar, bsdtar, file, patchelf, readelf or llvm-readelf, tar, zstd

Examples:
  ./scripts/trim_llvm.sh
  ./scripts/trim_llvm.sh --platform linux-x64-gnu
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
platform_to_asset_fragment() {
  case "$1" in
    macos-arm64) printf 'macOS-ARM64' ;;
    linux-arm64-gnu) printf 'Linux-ARM64' ;;
    linux-x64-gnu) printf 'Linux-X64' ;;
    *) die "unsupported LLVM host platform: $1" ;;
  esac
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

# Select the host ELF inspection command.
select_readelf() {
  if command -v llvm-readelf >/dev/null 2>&1; then
    READELF="$(command -v llvm-readelf)"
  elif command -v readelf >/dev/null 2>&1; then
    READELF="$(command -v readelf)"
  else
    die "missing required command: readelf or llvm-readelf"
  fi
}

# Return true when a regular file is ELF.
is_elf() {
  [[ -f "$1" && "$(file -Lb "$1")" == *"ELF"* ]]
}

# Return true when one ELF file has a dynamic section and can carry RUNPATH.
is_dynamic_elf() {
  is_elf "$1" && "${READELF}" -d "$1" 2>/dev/null | grep -q 'Dynamic section'
}

# Return the DT_NEEDED sonames of one ELF file.
needed_libraries() {
  "${READELF}" -d "$1" 2>/dev/null |
    awk '/Shared library:/ { value=$5; gsub(/[][]/, "", value); print value }'
}

# Return the DT_RPATH or DT_RUNPATH value of one ELF file.
elf_runpath() {
  "${READELF}" -d "$1" 2>/dev/null |
    awk '/Library r(un)?path:/ { value=$NF; gsub(/[][]/, "", value); print value }'
}

# Return true for the glibc and dynamic-loader boundary supplied by the host.
is_system_runtime_library() {
  case "$1" in
    libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|\
    libutil.so.1|libresolv.so.2|ld-linux-x86-64.so.2|\
    ld-linux-aarch64.so.1)
      return 0
      ;;
    *) return 1 ;;
  esac
}

# Return the single Clang resource directory in the official package.
find_clang_resource_dir() {
  local candidate found=""
  [[ -d "${LLVM_ROOT}/lib/clang" ]] ||
    die "Clang resource root missing: ${LLVM_ROOT}/lib/clang"
  for candidate in "${LLVM_ROOT}/lib/clang"/*; do
    [[ -d "${candidate}" ]] || continue
    [[ -z "${found}" ]] ||
      die "multiple Clang resource versions under ${LLVM_ROOT}/lib/clang"
    found="${candidate}"
  done
  [[ -n "${found}" ]] ||
    die "Clang resource directory missing under ${LLVM_ROOT}/lib/clang"
  printf '%s' "${found}"
}

# Verify one executable's file format and CPU against the selected host.
verify_binary_platform() {
  local binary="$1"
  local description
  description="$(file -Lb "${binary}")"
  case "${PLATFORM}" in
    macos-arm64)
      [[ "${description}" == *"Mach-O 64-bit"* && "${description}" == *"arm64"* ]] ||
        die "${binary} does not match ${PLATFORM}: ${description}"
      ;;
    linux-x64-gnu)
      [[ "${description}" == *"ELF 64-bit"* && "${description}" == *"x86-64"* ]] ||
        die "${binary} does not match ${PLATFORM}: ${description}"
      ;;
    linux-arm64-gnu)
      [[ "${description}" == *"ELF 64-bit"* &&
         ( "${description}" == *"ARM aarch64"* || "${description}" == *"ARM64"* ) ]] ||
        die "${binary} does not match ${PLATFORM}: ${description}"
      ;;
  esac
}

# Validate all official inputs before creating a staged output.
validate_source_tree() {
  local required resource_dir
  [[ -d "${LLVM_ROOT}" ]] || die "LLVM root not found: ${LLVM_ROOT}"
  for required in clang lld ld.lld llvm-ar llvm-ranlib lldb lldb-dap lldb-argdumper; do
    [[ -f "${LLVM_ROOT}/bin/${required}" ]] ||
      die "required LLVM input missing: bin/${required}"
    verify_binary_platform "${LLVM_ROOT}/bin/${required}"
  done
  case "${PLATFORM}" in
    macos-arm64)
      [[ -f "${LLVM_ROOT}/bin/debugserver" ]] ||
        die "required LLVM input missing: bin/debugserver"
      verify_binary_platform "${LLVM_ROOT}/bin/debugserver"
      [[ -f "${LLVM_ROOT}/lib/liblldb.${LLVM_VERSION}.dylib" ]] ||
        die "required LLDB library missing: lib/liblldb.${LLVM_VERSION}.dylib"
      ;;
    linux-*)
      [[ -f "${LLVM_ROOT}/bin/lldb-server" ]] ||
        die "required LLVM input missing: bin/lldb-server"
      verify_binary_platform "${LLVM_ROOT}/bin/lldb-server"
      [[ -e "${LLVM_ROOT}/lib/liblldb.so" ]] ||
        die "required LLDB library missing: lib/liblldb.so"
      ;;
  esac
  resource_dir="$(find_clang_resource_dir)"
  [[ -d "${resource_dir}/include" ]] ||
    die "Clang builtin headers missing: ${resource_dir}/include"
}

# Copy one executable, dereferencing official command aliases.
copy_executable() {
  local name="$1"
  cp -L "${LLVM_ROOT}/bin/${name}" "${STAGING_DIR}/bin/${name}"
  chmod 0755 "${STAGING_DIR}/bin/${name}"
}

# Copy one executable under an internal destination name.
copy_executable_as() {
  local source_name="$1"
  local destination_name="$2"
  cp -L "${LLVM_ROOT}/bin/${source_name}" "${STAGING_DIR}/bin/${destination_name}"
  chmod 0755 "${STAGING_DIR}/bin/${destination_name}"
}

# Copy Clang and its builtin headers plus ordinary-link compiler runtime files.
trim_clang() {
  local resource_dir resource_version runtime_dir runtime_file copied_runtime=0
  resource_dir="$(find_clang_resource_dir)"
  resource_version="$(basename "${resource_dir}")"
  copy_executable clang
  mkdir -p "${STAGING_DIR}/lib/clang/${resource_version}"
  cp -R "${resource_dir}/include" "${STAGING_DIR}/lib/clang/${resource_version}/include"

  case "${PLATFORM}" in
    macos-arm64)
      runtime_dir="${resource_dir}/lib/darwin"
      runtime_file="${runtime_dir}/libclang_rt.osx.a"
      [[ -f "${runtime_file}" ]] ||
        die "required Clang runtime missing: ${runtime_file}"
      mkdir -p "${STAGING_DIR}/lib/clang/${resource_version}/lib/darwin"
      cp "${runtime_file}" "${STAGING_DIR}/lib/clang/${resource_version}/lib/darwin/"
      ;;
    linux-*)
      if [[ -d "${resource_dir}/lib" ]]; then
        while IFS= read -r -d '' runtime_file; do
          runtime_dir="${STAGING_DIR}/lib/clang/${resource_version}/lib/${runtime_file#"${resource_dir}/lib/"}"
          mkdir -p "$(dirname "${runtime_dir}")"
          cp -P "${runtime_file}" "${runtime_dir}"
          copied_runtime=1
        done < <(find "${resource_dir}/lib" \( -type f -o -type l \) \
          \( -name 'libclang_rt.builtins*.a' -o -name 'clang_rt.crtbegin*.o' \
             -o -name 'clang_rt.crtend*.o' \) -print0)
      fi
      ;;
  esac
  : "${copied_runtime}"
}

# Copy LLD and the driver entry resolved by clang -fuse-ld=lld.
trim_lld() {
  copy_executable lld
  ln -s lld "${STAGING_DIR}/bin/ld.lld"
}

# Copy LLVM's target-independent archive tool and ranlib entry.
trim_archive_tools() {
  copy_executable llvm-ar
  ln -s llvm-ar "${STAGING_DIR}/bin/llvm-ranlib"
}

# Write one Linux LLDB launcher that selects the private Python bootstrap home
# relative to the launcher itself. The environment is internal to the launched
# debugger and does not require an end-user setting.
write_linux_lldb_launcher() {
  local name="$1"
  local real_name="$2"
  cat >"${STAGING_DIR}/bin/${name}" <<EOF
#!/bin/sh
set -eu

launcher_path=\$0
case "\${launcher_path}" in
  */*) ;;
  *) launcher_path=\$(command -v -- "\${launcher_path}") ;;
esac
launcher_dir=\$(CDPATH= cd -- "\${launcher_path%/*}" && pwd)
PYTHONHOME="\${launcher_dir}/../lib/python-home"
PYTHONDONTWRITEBYTECODE=1
export PYTHONHOME PYTHONDONTWRITEBYTECODE
exec "\${launcher_dir}/${real_name}" "\$@"
EOF
  chmod 0755 "${STAGING_DIR}/bin/${name}"
}

# Copy LLDB executables, remote stub and shared-library soname chain.
trim_lldb() {
  local library matched=0
  copy_executable lldb-argdumper
  case "${PLATFORM}" in
    macos-arm64)
      copy_executable lldb
      copy_executable lldb-dap
      copy_executable debugserver
      for library in "${LLVM_ROOT}/lib/"liblldb*.dylib; do
        [[ -e "${library}" || -L "${library}" ]] || continue
        cp -P "${library}" "${STAGING_DIR}/lib/"
        matched=1
      done
      ;;
    linux-*)
      copy_executable_as lldb .lldb.real
      copy_executable_as lldb-dap .lldb-dap.real
      write_linux_lldb_launcher lldb .lldb.real
      write_linux_lldb_launcher lldb-dap .lldb-dap.real
      copy_executable lldb-server
      for library in "${LLVM_ROOT}/lib/"liblldb.so*; do
        [[ -e "${library}" || -L "${library}" ]] || continue
        cp -P "${library}" "${STAGING_DIR}/lib/"
        matched=1
      done
      ;;
  esac
  [[ ${matched} -eq 1 ]] || die "no LLDB shared library copied for ${PLATFORM}"
}

# Extract one Debian package data archive into the Linux runtime source overlay.
extract_deb() {
  local package="$1"
  local data_member
  data_member="$(ar t "${package}" | awk '/^data\.tar/ { print; exit }')"
  [[ -n "${data_member}" ]] || die "Debian data archive missing: ${package}"
  case "${data_member}" in
    *.zst)
      ar p "${package}" "${data_member}" | zstd -dc | tar -xf - -C "${RUNTIME_SOURCE}"
      ;;
    *.xz)
      ar p "${package}" "${data_member}" | tar -xJf - -C "${RUNTIME_SOURCE}"
      ;;
    *.gz)
      ar p "${package}" "${data_member}" | tar -xzf - -C "${RUNTIME_SOURCE}"
      ;;
    *) die "unsupported Debian data archive ${data_member} in ${package}" ;;
  esac
}

# Extract only the packages listed by the pinned runtime manifest.
extract_linux_runtime_packages() {
  local url filename expected_sha256 package
  RUNTIME_SOURCE="$(mktemp -d "${OUTPUT_PARENT}/.${PLATFORM}.runtime.XXXXXX")"
  while IFS='|' read -r url filename expected_sha256; do
    [[ -n "${url}" ]] || continue
    package="${RUNTIME_CACHE}/${PLATFORM}/${filename}"
    [[ -f "${package}" ]] ||
      die "Linux runtime input missing: ${package}; run scripts/fetch_llvm.sh first"
    case "${filename}" in
      *.rpm) bsdtar -xf "${package}" -C "${RUNTIME_SOURCE}" ;;
      *.deb) extract_deb "${package}" ;;
      cpython-*-LICENSE.txt)
        mkdir -p "${RUNTIME_SOURCE}/usr/share/licenses/python3.11"
        cp "${package}" "${RUNTIME_SOURCE}/usr/share/licenses/python3.11/LICENSE"
        ;;
      *) die "unsupported Linux runtime package: ${filename}" ;;
    esac
  done < <("${SCRIPT_DIR}/fetch_llvm.sh" --platform "${PLATFORM}" --print-runtime-manifest)
}

# Copy only the Python bootstrap package required when liblldb initializes its
# filesystem codec. No interpreter, general standard library or LLDB Python
# bindings are retained.
copy_python_bootstrap_runtime() {
  local source_dir="${RUNTIME_SOURCE}/usr/lib64/python3.11/encodings"
  local destination_dir="${STAGING_DIR}/lib/python-home/lib64/python3.11"
  [[ -d "${source_dir}" ]] ||
    die "Python 3.11 encodings input missing: ${source_dir}"
  mkdir -p "${destination_dir}/encodings"
  # Source modules are sufficient for embedded-Python imports. Package-built
  # __pycache__ files are redundant, optimization-level-specific artifacts.
  find "${source_dir}" -maxdepth 1 -type f -name '*.py' \
    -exec cp {} "${destination_dir}/encodings/" \;
  [[ -f "${destination_dir}/encodings/__init__.py" ]] ||
    die "Python 3.11 encodings package is incomplete"
}

# Return the single extracted candidate for one unresolved soname.
find_runtime_candidate() {
  local soname="$1"
  local candidate found=""
  while IFS= read -r -d '' candidate; do
    [[ -z "${found}" ]] ||
      die "multiple runtime candidates for ${soname}: ${found}, ${candidate}"
    found="${candidate}"
  done < <(find "${RUNTIME_SOURCE}" \( -type f -o -type l \) -name "${soname}" -print0)
  [[ -n "${found}" ]] || return 1
  printf '%s' "${found}"
}

# Copy one soname and its complete relative symlink chain from package inputs.
copy_private_library() {
  local soname="$1"
  local candidate name link_target next
  candidate="$(find_runtime_candidate "${soname}")" ||
    die "unresolved non-system DT_NEEDED library: ${soname}"

  while true; do
    name="$(basename "${candidate}")"
    if [[ ! -e "${STAGING_DIR}/lib/${name}" && ! -L "${STAGING_DIR}/lib/${name}" ]]; then
      cp -P "${candidate}" "${STAGING_DIR}/lib/${name}"
    fi
    [[ -L "${candidate}" ]] || break
    link_target="$(readlink "${candidate}")"
    if [[ "${link_target}" == /* ]]; then
      next="${RUNTIME_SOURCE}${link_target}"
    else
      next="$(dirname "${candidate}")/${link_target}"
    fi
    [[ -e "${next}" || -L "${next}" ]] ||
      die "runtime package contains broken library link: ${candidate} -> ${link_target}"
    candidate="${next}"
  done
}

# Resolve the staged Linux ELF dependency closure from the extracted packages.
resolve_linux_dependency_closure() {
  local changed elf needed
  changed=1
  while [[ ${changed} -eq 1 ]]; do
    changed=0
    while IFS= read -r -d '' elf; do
      is_elf "${elf}" || continue
      while IFS= read -r needed; do
        [[ -n "${needed}" ]] || continue
        is_system_runtime_library "${needed}" && continue
        if [[ -e "${STAGING_DIR}/lib/${needed}" || -L "${STAGING_DIR}/lib/${needed}" ]]; then
          continue
        fi
        copy_private_library "${needed}"
        changed=1
      done < <(needed_libraries "${elf}")
    done < <(find "${STAGING_DIR}/bin" "${STAGING_DIR}/lib" -type f -print0)
  done
}

# Set explicit relative lookup paths on every staged Linux ELF.
set_linux_runpaths() {
  local elf
  while IFS= read -r -d '' elf; do
    is_dynamic_elf "${elf}" || continue
    case "${elf}" in
      "${STAGING_DIR}/bin/"*) patchelf --set-rpath '$ORIGIN/../lib' "${elf}" ;;
      "${STAGING_DIR}/lib/"*) patchelf --set-rpath '$ORIGIN' "${elf}" ;;
    esac
  done < <(find "${STAGING_DIR}/bin" "${STAGING_DIR}/lib" -type f -print0)
}

# Copy license texts from package inputs without retaining package metadata.
copy_linux_licenses() {
  local license parent output_name
  mkdir -p "${STAGING_DIR}/LICENSES"
  while IFS= read -r -d '' license; do
    parent="$(basename "$(dirname "${license}")")"
    output_name="${parent}-$(basename "${license}")"
    cp "${license}" "${STAGING_DIR}/LICENSES/${output_name}"
  done < <(find "${RUNTIME_SOURCE}/usr/share" -type f \
    \( -path '*/licenses/*' -o -iname copyright -o -iname 'copying*' \
       -o -iname 'license*' \) -print0)
}

# Write exact high-level provenance and inclusion policy.
write_metadata() {
  local asset_fragment resource_dir resource_version url filename expected_sha256
  asset_fragment="$(platform_to_asset_fragment "${PLATFORM}")"
  resource_dir="$(find_clang_resource_dir)"
  resource_version="$(basename "${resource_dir}")"

  if [[ -f "${LLVM_ROOT}/LICENSE.TXT" ]]; then
    cp "${LLVM_ROOT}/LICENSE.TXT" "${STAGING_DIR}/LICENSE.TXT"
  fi

  {
    echo "# LLVM minimal binary closure"
    echo
    echo "Version: ${LLVM_VERSION}"
    echo "Platform: ${PLATFORM}"
    echo "LLVM source: https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-${asset_fragment}.tar.xz"
    cat <<EOF

Included:
- bin/clang
- bin/lld and bin/ld.lld -> lld
- bin/llvm-ar and bin/llvm-ranlib -> llvm-ar
- bin/lldb and bin/lldb-dap
- bin/lldb-argdumper
- bin/debugserver (macOS) or bin/lldb-server (Linux)
- lib/clang/${resource_version}/include
- ordinary-link Clang runtime files supplied by the official package
- liblldb shared-library files and their soname chain
- Linux non-glibc DT_NEEDED closure resolved from the pinned packages below
- Linux Python 3.11 encodings bootstrap package and internal LLDB launchers
- upstream and package license texts

Excluded:
- unrelated LLVM executables and development headers/libraries
- clang tooling, C++ development material and editor integrations
- sanitizer, profiler, fuzzer and XRay runtimes
- lldb-mi and the LLDB C++ embedding API
- Python executable, standard library outside encodings, LLDB Python bindings,
  third-party modules and general scripting support
- GDB auto-load Python scripts and package-manager metadata
EOF
    if [[ "${PLATFORM}" == linux-*-gnu ]]; then
      echo
      echo "Linux private-runtime binary inputs:"
      while IFS='|' read -r url filename expected_sha256; do
        echo "- ${filename}"
        echo "  - URL: ${url}"
        echo "  - SHA-256: ${expected_sha256}"
      done < <("${SCRIPT_DIR}/fetch_llvm.sh" --platform "${PLATFORM}" --print-runtime-manifest)
      cat <<'EOF'

Linux runtime policy:
- Only the Linux kernel, dynamic loader and glibc foundation remain host
  prerequisites.
- libpython3.11.so.1.0 and encodings exist only for liblldb initialization.
  Feng never uses or supports Python scripts.
- bin/lldb and bin/lldb-dap select lib/python-home through a private relative
  PYTHONHOME before executing their hidden original ELF files. Users do not
  configure this environment.
- Every bundled executable uses $ORIGIN/../lib and every bundled shared
  library uses $ORIGIN. LD_LIBRARY_PATH and target sysroots are not used for
  host-tool startup.
- Package license texts are retained under LICENSES/.
EOF
    fi
    echo
    echo "Re-sync:"
    echo "  ./scripts/fetch_llvm.sh --platform ${PLATFORM}"
    echo "  ./scripts/trim_llvm.sh --platform ${PLATFORM}"
  } >"${STAGING_DIR}/README.md"
}

# Reject broken symlinks in the complete staged tree.
verify_symlinks() {
  local link
  while IFS= read -r -d '' link; do
    [[ -e "${link}" ]] || die "staged output contains broken symlink: ${link}"
  done < <(find "${STAGING_DIR}" -type l -print0)
}

# Verify Linux runpaths and the complete DT_NEEDED closure.
verify_linux_dependency_closure() {
  local elf needed expected_runpath actual_runpath
  while IFS= read -r -d '' elf; do
    is_dynamic_elf "${elf}" || continue
    case "${elf}" in
      "${STAGING_DIR}/bin/"*) expected_runpath='$ORIGIN/../lib' ;;
      "${STAGING_DIR}/lib/"*) expected_runpath='$ORIGIN' ;;
      *) continue ;;
    esac
    actual_runpath="$(elf_runpath "${elf}")"
    [[ "${actual_runpath}" == "${expected_runpath}" ]] ||
      die "unexpected runpath for ${elf}: ${actual_runpath:-<none>}"
    while IFS= read -r needed; do
      [[ -n "${needed}" ]] || continue
      is_system_runtime_library "${needed}" && continue
      [[ -e "${STAGING_DIR}/lib/${needed}" || -L "${STAGING_DIR}/lib/${needed}" ]] ||
        die "incomplete staged DT_NEEDED closure: ${elf} -> ${needed}"
    done < <(needed_libraries "${elf}")
  done < <(find "${STAGING_DIR}/bin" "${STAGING_DIR}/lib" -type f -print0)
}

# Verify the Linux ABI and CPU baseline defined by the release specification.
verify_linux_abi_baseline() {
  local elf max_glibc max_glibcxx version
  local versions_file="${STAGING_DIR}/.symbol-versions"
  : >"${versions_file}"
  while IFS= read -r -d '' elf; do
    is_elf "${elf}" || continue
    "${READELF}" --version-info "${elf}" 2>/dev/null |
      grep -Eo 'GLIBC(X{2})?_[0-9]+(\.[0-9]+)+' >>"${versions_file}" || true
    if [[ "${PLATFORM}" == "linux-x64-gnu" ]] &&
       "${READELF}" -n "${elf}" 2>/dev/null |
         grep -Eq 'x86 ISA needed:.*x86-64-v[2-4]'; then
      die "x86-64-v2 or newer CPU baseline detected: ${elf}"
    fi
  done < <(find "${STAGING_DIR}/bin" "${STAGING_DIR}/lib" -type f -print0)

  max_glibc="$(grep -E '^GLIBC_[0-9]' "${versions_file}" |
    sed 's/^GLIBC_//' | sort -Vu | tail -n 1)"
  max_glibcxx="$(grep -E '^GLIBCXX_[0-9]' "${versions_file}" |
    sed 's/^GLIBCXX_//' | sort -Vu | tail -n 1)"
  rm -f "${versions_file}"

  version="$(printf '%s\n%s\n' "${max_glibc}" "2.34" | sort -Vu | tail -n 1)"
  [[ "${version}" == "2.34" ]] ||
    die "Linux LLVM closure requires GLIBC_${max_glibc}, above GLIBC_2.34"
  version="$(printf '%s\n%s\n' "${max_glibcxx}" "3.4.30" | sort -Vu | tail -n 1)"
  [[ "${version}" == "3.4.30" ]] ||
    die "Linux LLVM closure requires GLIBCXX_${max_glibcxx}, above GLIBCXX_3.4.30"
}

# Verify the versioned ncurses ABI required by the official Ubuntu-built
# liblldb. Unversioned AlmaLinux ncurses libraries are not compatible here.
verify_linux_ncurses_abi() {
  local library version_info
  for library in libncurses.so.6 libpanel.so.6 libform.so.6; do
    [[ -e "${STAGING_DIR}/lib/${library}" ]] ||
      die "required private ncurses library missing: ${library}"
    version_info="$("${READELF}" --version-info \
      "${STAGING_DIR}/lib/${library}" 2>/dev/null)"
    grep -q 'Name: NCURSES6_5\.0\.19991023' <<<"${version_info}" ||
      die "${library} does not provide the required NCURSES6 symbol ABI"
  done
  library="libtinfo.so.6"
  [[ -e "${STAGING_DIR}/lib/${library}" ]] ||
    die "required private ncurses library missing: ${library}"
  version_info="$("${READELF}" --version-info \
    "${STAGING_DIR}/lib/${library}" 2>/dev/null)"
  grep -q 'Name: NCURSES6_TINFO_5\.0\.19991023' <<<"${version_info}" ||
    die "${library} does not provide the required NCURSES6_TINFO symbol ABI"
}

# Ensure the Linux output contains exactly the approved Python bootstrap
# subset: libpython plus the encodings package selected by the private home.
verify_minimal_python_runtime() {
  local path relative
  [[ -f "${STAGING_DIR}/lib/libpython3.11.so.1.0" ]] ||
    die "private libpython3.11.so.1.0 is missing"
  [[ -f "${STAGING_DIR}/lib/python-home/lib64/python3.11/encodings/__init__.py" ]] ||
    die "private Python 3.11 encodings package is missing"
  while IFS= read -r -d '' path; do
    relative="${path#"${STAGING_DIR}/lib/python-home/lib64/python3.11/"}"
    case "${relative}" in
      encodings|encodings/*) ;;
      *) die "unexpected Python standard-library content retained: ${relative}" ;;
    esac
  done < <(find "${STAGING_DIR}/lib/python-home/lib64/python3.11" \
    -mindepth 1 -print0)
  while IFS= read -r -d '' path; do
    relative="${path#"${STAGING_DIR}/"}"
    case "${relative}" in
      lib/libpython3.11.so.1.0|lib/python-home|\
      lib/python-home/lib64/python3.11|\
      lib/python-home/lib64/python3.11/encodings/*|LICENSES/*) ;;
      *) die "unexpected Python runtime content retained: ${relative}" ;;
    esac
  done < <(find "${STAGING_DIR}" -iname '*python*' -print0)
}

# Run startup checks only when the selected package matches the current host.
verify_native_executables() {
  local host_platform
  host_platform="$(detect_host_platform)"
  if [[ "${host_platform}" != "${PLATFORM}" ]]; then
    echo "==> Static validation only; ${PLATFORM} cannot run on ${host_platform}"
    return
  fi
  "${STAGING_DIR}/bin/clang" --version >/dev/null
  "${STAGING_DIR}/bin/ld.lld" --version >/dev/null
  "${STAGING_DIR}/bin/llvm-ar" --version >/dev/null
  "${STAGING_DIR}/bin/llvm-ranlib" --version >/dev/null
  "${STAGING_DIR}/bin/lldb" --version >/dev/null
  "${STAGING_DIR}/bin/lldb-dap" --version >/dev/null
}

# Restore the previous output after a failed staged replacement.
cleanup_on_failure() {
  local status=$?
  trap - EXIT
  if [[ -n "${RUNTIME_SOURCE}" && -d "${RUNTIME_SOURCE}" ]]; then
    rm -rf "${RUNTIME_SOURCE}"
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

# Atomically replace one platform directory after staged validation.
install_staged_output() {
  OUTPUT_DIR="${OUTPUT_PARENT}/${PLATFORM}"
  BACKUP_DIR="${OUTPUT_PARENT}/.${PLATFORM}.backup.$$"
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

# Coordinate validation, trimming and the final atomic replacement.
main() {
  local asset_fragment default_root
  parse_args "$@"
  PLATFORM="${PLATFORM:-$(detect_host_platform)}"
  asset_fragment="$(platform_to_asset_fragment "${PLATFORM}")"
  default_root="${PROJECT_ROOT}/local/llvm/LLVM-${LLVM_VERSION}-${asset_fragment}"
  LLVM_ROOT="${LLVM_ROOT:-${default_root}}"

  require_cmd awk
  require_cmd basename
  require_cmd cp
  require_cmd dirname
  require_cmd file
  require_cmd find
  require_cmd grep
  require_cmd ln
  require_cmd mktemp
  require_cmd mv
  require_cmd readlink
  require_cmd sed
  require_cmd sort
  select_readelf
  if [[ "${PLATFORM}" == linux-*-gnu ]]; then
    require_cmd ar
    require_cmd bsdtar
    require_cmd patchelf
    require_cmd tar
    require_cmd zstd
  fi

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
  trim_archive_tools
  trim_lldb
  if [[ "${PLATFORM}" == linux-*-gnu ]]; then
    extract_linux_runtime_packages
    copy_python_bootstrap_runtime
    resolve_linux_dependency_closure
    set_linux_runpaths
    copy_linux_licenses
  fi
  write_metadata
  verify_symlinks
  if [[ "${PLATFORM}" == linux-*-gnu ]]; then
    verify_linux_dependency_closure
    verify_linux_abi_baseline
    verify_linux_ncurses_abi
    verify_minimal_python_runtime
    rm -rf "${RUNTIME_SOURCE}"
    RUNTIME_SOURCE=""
  fi
  verify_native_executables
  install_staged_output
  trap - EXIT
  echo "==> Done: ${OUTPUT_DIR}"
}

main "$@"
