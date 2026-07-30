#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_SOURCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SOURCE_ROOT="${DEFAULT_SOURCE_ROOT}"
COMPONENTS_ROOT=""
COMPONENT_ARCHIVES_ROOT=""
BUNDLED_PACKAGES_ROOT=""
OUTPUT_ROOT=""
VERSION=""
WORK_ROOT=""
ARCHIVE_TOOL="ar"
SHA256_TOOL=""

HOST_PLATFORMS=(
  "macos-arm64"
  "linux-x64-gnu"
  "linux-arm64-gnu"
)
RUNTIME_PLATFORMS=(
  "macos-arm64"
  "linux-x64-gnu"
  "linux-x64-musl"
  "linux-arm64-gnu"
  "linux-arm64-musl"
)
LINUX_PLATFORMS=(
  "linux-x64-gnu"
  "linux-x64-musl"
  "linux-arm64-gnu"
  "linux-arm64-musl"
)
PUBLIC_HEADERS=(
  "feng_generated.h"
  "feng_runtime.h"
  "feng_runtime_contract.inc"
)

# Print the supported pure-assembly invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_assemble.sh \
    --version=<version> \
    (--components=<component-root> | --component-archives=<archive-root>) \
    --output=<output-dir> \
    [--packages=<bundled-package-dir>] \
    [--source-root=<repository-root>] \
    [--archive-tool=<ar-or-llvm-ar>]

The component root must contain macos-arm64/, linux-x64-gnu/, and
linux-arm64-gnu/ in the layout defined by the release specification.
The archive root must contain one release-component-<platform>.tar for
each of those platforms; this script validates and extracts the archives.
The optional package directory may contain only top-level
<name>-<version>.fb files. Every distribution receives the same validated
package set, and omitting the option produces an empty top-level pkg/.
EOF
}

# Report one fatal release assembly error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Require one command resolved through PATH.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

# Select the native SHA-256 command available on the current host.
configure_sha256_tool() {
  if command -v sha256sum >/dev/null 2>&1; then
    SHA256_TOOL="sha256sum"
  elif command -v shasum >/dev/null 2>&1; then
    SHA256_TOOL="shasum"
  else
    die "missing required SHA-256 command: sha256sum or shasum"
  fi
}

# Require one regular file.
require_file() {
  [[ -f "$1" ]] || die "required file not found: $1"
}

# Require one directory.
require_dir() {
  [[ -d "$1" ]] || die "required directory not found: $1"
}

# Remove only the staging directory created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
}

# Print a portable SHA-256 record for one file and relative manifest path.
print_sha256_record() {
  local file_path="$1"
  local manifest_path="$2"
  local digest

  if [[ "${SHA256_TOOL}" == "sha256sum" ]]; then
    digest="$(sha256sum "${file_path}" | awk '{print $1}')"
  else
    digest="$(shasum -a 256 "${file_path}" | awk '{print $1}')"
  fi
  printf '%s  %s\n' "${digest}" "${manifest_path}"
}

# Print the runtime platforms supplied by one native component.
component_runtime_platforms() {
  case "$1" in
    macos-arm64)
      printf '%s\n' "macos-arm64"
      ;;
    linux-x64-gnu)
      printf '%s\n' "linux-x64-gnu" "linux-x64-musl"
      ;;
    linux-arm64-gnu)
      printf '%s\n' "linux-arm64-gnu" "linux-arm64-musl"
      ;;
    *)
      die "unsupported release component platform: $1"
      ;;
  esac
}

# Print the native component that owns one runtime platform.
runtime_component_platform() {
  case "$1" in
    macos-arm64)
      printf '%s\n' "macos-arm64"
      ;;
    linux-x64-gnu|linux-x64-musl)
      printf '%s\n' "linux-x64-gnu"
      ;;
    linux-arm64-gnu|linux-arm64-musl)
      printf '%s\n' "linux-arm64-gnu"
      ;;
    *)
      die "unsupported runtime platform: $1"
      ;;
  esac
}

# Verify one binary or object has the expected host file format and CPU.
verify_platform_file() {
  local file_path="$1"
  local platform="$2"
  local description="$3"
  local format

  require_file "${file_path}"
  format="$(file -b "${file_path}")"
  case "${platform}" in
    macos-arm64)
      [[ "${format}" == *"Mach-O 64-bit"* && "${format}" == *"arm64"* ]] ||
        die "${description} has unexpected format for ${platform}: ${format}"
      ;;
    linux-x64-gnu|linux-x64-musl)
      [[ "${format}" == *"ELF 64-bit"* && "${format}" == *"x86-64"* ]] ||
        die "${description} has unexpected format for ${platform}: ${format}"
      ;;
    linux-arm64-gnu|linux-arm64-musl)
      [[ "${format}" == *"ELF 64-bit"* && "${format}" == *"ARM aarch64"* ]] ||
        die "${description} has unexpected format for ${platform}: ${format}"
      ;;
    *)
      die "unsupported platform for format validation: ${platform}"
      ;;
  esac
}

# Verify every member in one runtime archive matches its target platform.
verify_runtime_archive() {
  local archive_path="$1"
  local platform="$2"
  local extract_root="$3"
  local member
  local member_count=0
  local object_path="${extract_root}/runtime-member.o"

  require_file "${archive_path}"
  mkdir -p "${extract_root}"
  while IFS= read -r member; do
    [[ -n "${member}" ]] || continue
    case "${member}" in
      "__.SYMDEF"|"__.SYMDEF SORTED"|"/"|"//")
        continue
        ;;
    esac
    case "${member}" in
      */*|.|..)
        die "unsafe archive member in ${archive_path}: ${member}"
        ;;
    esac
    "${ARCHIVE_TOOL}" p "${archive_path}" "${member}" > "${object_path}" ||
      die "failed to read ${member} from ${archive_path}"
    verify_platform_file \
      "${object_path}" "${platform}" \
      "runtime archive member ${member}"
    member_count=$((member_count + 1))
  done < <("${ARCHIVE_TOOL}" t "${archive_path}")
  [[ "${member_count}" -gt 0 ]] ||
    die "runtime archive contains no members: ${archive_path}"
}

# Verify one component manifest contains exactly its required files and hashes.
verify_component_manifest() {
  local host_platform="$1"
  local component_root="${COMPONENTS_ROOT}/${host_platform}"
  local expected_manifest="${WORK_ROOT}/manifest-${host_platform}.expected"
  local actual_manifest="${WORK_ROOT}/manifest-${host_platform}.actual"
  local header
  local runtime_platform

  : > "${expected_manifest}"
  print_sha256_record \
    "${component_root}/bin/feng" "bin/feng" >> "${expected_manifest}"
  for header in "${PUBLIC_HEADERS[@]}"; do
    print_sha256_record \
      "${component_root}/include/${header}" \
      "include/${header}" >> "${expected_manifest}"
  done
  while IFS= read -r runtime_platform; do
    print_sha256_record \
      "${component_root}/lib/${runtime_platform}/libfeng_runtime.a" \
      "lib/${runtime_platform}/libfeng_runtime.a" >> "${expected_manifest}"
  done < <(component_runtime_platforms "${host_platform}")

  LC_ALL=C sort "${expected_manifest}" -o "${expected_manifest}"
  LC_ALL=C sort "${component_root}/SHA256SUMS" > "${actual_manifest}"
  cmp -s "${expected_manifest}" "${actual_manifest}" ||
    die "component checksum manifest is missing, stale, or contains unexpected entries: ${host_platform}/SHA256SUMS"
}

# Verify one native component and all runtime archives assigned to it.
verify_component() {
  local host_platform="$1"
  local component_root="${COMPONENTS_ROOT}/${host_platform}"
  local runtime_platform

  require_dir "${component_root}"
  require_file "${component_root}/SHA256SUMS"
  [[ -x "${component_root}/bin/feng" ]] ||
    die "Feng executable is missing or not executable: ${component_root}/bin/feng"
  verify_platform_file \
    "${component_root}/bin/feng" "${host_platform}" \
    "Feng executable"
  while IFS= read -r runtime_platform; do
    verify_runtime_archive \
      "${component_root}/lib/${runtime_platform}/libfeng_runtime.a" \
      "${runtime_platform}" \
      "${WORK_ROOT}/archive-check/${runtime_platform}"
  done < <(component_runtime_platforms "${host_platform}")
  verify_component_manifest "${host_platform}"
}

# Verify all native components contain byte-identical public headers.
verify_public_headers() {
  local reference_root="${COMPONENTS_ROOT}/macos-arm64/include"
  local host_platform
  local header

  for header in "${PUBLIC_HEADERS[@]}"; do
    require_file "${reference_root}/${header}"
    for host_platform in "${HOST_PLATFORMS[@]}"; do
      require_file "${COMPONENTS_ROOT}/${host_platform}/include/${header}"
      cmp -s \
        "${reference_root}/${header}" \
        "${COMPONENTS_ROOT}/${host_platform}/include/${header}" ||
        die "public header differs between release components: ${header}"
    done
  done
}

# Verify one host LLVM tree contains every required executable entry.
verify_llvm_toolchain() {
  local host_platform="$1"
  local llvm_root="${SOURCE_ROOT}/toolchain/llvm/${host_platform}"
  local tool
  local tools=(
    "clang"
    "lld"
    "ld.lld"
    "llvm-ar"
    "llvm-ranlib"
    "lldb"
    "lldb-dap"
    "lldb-argdumper"
  )

  case "${host_platform}" in
    macos-arm64) tools+=("debugserver") ;;
    linux-*) tools+=("lldb-server") ;;
  esac
  require_dir "${llvm_root}/bin"
  require_dir "${llvm_root}/lib"
  for tool in "${tools[@]}"; do
    [[ -x "${llvm_root}/bin/${tool}" ]] ||
      die "required LLVM tool is missing or not executable: ${llvm_root}/bin/${tool}"
  done
}

# Verify all four Linux sysroots have the required compile/link directory roots.
verify_linux_sysroots() {
  local platform
  local sysroot

  for platform in "${LINUX_PLATFORMS[@]}"; do
    sysroot="${SOURCE_ROOT}/toolchain/sysroot/${platform}"
    require_dir "${sysroot}/usr/include"
    require_dir "${sysroot}/usr/lib"
    require_dir "${sysroot}/lib/gcc"
  done
}

# Read one quoted field from the package section of a bundle manifest.
package_manifest_value() {
  local manifest_path="$1"
  local field_name="$2"

  awk -v wanted="${field_name}" '
    function trim(value) {
      sub(/^[[:space:]]+/, "", value)
      sub(/[[:space:]]+$/, "", value)
      return value
    }
    /^[[:space:]]*\[package\][[:space:]]*$/ {
      in_package = 1
      next
    }
    /^[[:space:]]*\[/ {
      if (in_package) {
        exit
      }
    }
    in_package {
      separator = index($0, ":")
      if (separator == 0) {
        next
      }
      key = trim(substr($0, 1, separator - 1))
      value = trim(substr($0, separator + 1))
      if (key == wanted && value ~ /^"[^"]+"$/) {
        print substr(value, 2, length(value) - 2)
      }
    }
  ' "${manifest_path}"
}

# Validate every optional bundled package and its filename coordinate.
verify_bundled_packages() {
  local unexpected_entry
  local package_path
  local package_name
  local manifest_path="${WORK_ROOT}/bundled-package.feng.fm"
  local manifest_name
  local manifest_version
  local expected_name

  [[ -n "${BUNDLED_PACKAGES_ROOT}" ]] || return 0
  require_dir "${BUNDLED_PACKAGES_ROOT}"
  unexpected_entry="$(
    find "${BUNDLED_PACKAGES_ROOT}" \
      -mindepth 1 -maxdepth 1 ! -type f -print -quit
  )"
  [[ -z "${unexpected_entry}" ]] ||
    die "bundled package directory contains a non-file entry: ${unexpected_entry}"

  while IFS= read -r package_path; do
    [[ -n "${package_path}" ]] || continue
    case "${package_path}" in
      *.fb) ;;
      *) die "bundled package directory contains a non-.fb file: ${package_path}" ;;
    esac
    unzip -tqq "${package_path}" >/dev/null ||
      die "bundled package is not a readable .fb archive: ${package_path}"
    [[ "$(unzip -Z1 "${package_path}" | grep -c '^feng\.fm$')" == "1" ]] ||
      die "bundled package must contain exactly one top-level feng.fm: ${package_path}"
    unzip -p "${package_path}" feng.fm > "${manifest_path}" ||
      die "failed to read bundled package manifest: ${package_path}"
    manifest_name="$(package_manifest_value "${manifest_path}" "name")"
    manifest_version="$(package_manifest_value "${manifest_path}" "version")"
    [[ -n "${manifest_name}" && "${manifest_name}" != *$'\n'* ]] ||
      die "bundled package manifest must contain one package name: ${package_path}"
    [[ -n "${manifest_version}" && "${manifest_version}" != *$'\n'* ]] ||
      die "bundled package manifest must contain one package version: ${package_path}"
    package_name="${package_path##*/}"
    expected_name="${manifest_name}-${manifest_version}.fb"
    [[ "${package_name}" == "${expected_name}" ]] ||
      die "bundled package filename coordinate mismatch: expected ${expected_name}, found ${package_name}"
  done < <(
    find "${BUNDLED_PACKAGES_ROOT}" \
      -mindepth 1 -maxdepth 1 -type f -print | LC_ALL=C sort
  )
}

# Copy one directory tree while preserving symbolic links and file modes.
copy_tree() {
  local source_dir="$1"
  local destination_dir="$2"

  mkdir -p "${destination_dir}"
  cp -R -P "${source_dir}/." "${destination_dir}/"
}

# Validate and extract the three native component archives.
extract_component_archives() {
  local host_platform
  local archive_path
  local member
  local member_count

  mkdir -p "${COMPONENTS_ROOT}"
  for host_platform in "${HOST_PLATFORMS[@]}"; do
    archive_path="${COMPONENT_ARCHIVES_ROOT}/release-component-${host_platform}.tar"
    require_file "${archive_path}"
    member_count=0
    while IFS= read -r member; do
      [[ -n "${member}" ]] || continue
      [[ "${member}" != /* ]] ||
        die "component archive contains an absolute path: ${archive_path}"
      case "/${member}/" in
        *"/../"*|*"/./"*)
          die "component archive contains an unsafe path: ${member}"
          ;;
      esac
      case "${member}" in
        "${host_platform}"|"${host_platform}/"|"${host_platform}/"*)
          ;;
        *)
          die "component archive contains an unexpected path: ${member}"
          ;;
      esac
      member_count=$((member_count + 1))
    done < <(tar -tf "${archive_path}")
    [[ "${member_count}" -gt 0 ]] ||
      die "component archive is empty: ${archive_path}"
    tar -xf "${archive_path}" -C "${COMPONENTS_ROOT}"
  done
  if [[ -n "$(find "${COMPONENTS_ROOT}" -type l -print -quit)" ]]; then
    die "component archives must not contain symbolic links"
  fi
}

# Assemble and validate one complete host distribution tree.
assemble_distribution() {
  local host_platform="$1"
  local package_name="feng-${VERSION}-${host_platform}"
  local package_root="${WORK_ROOT}/packages/${package_name}"
  local component_root="${COMPONENTS_ROOT}/${host_platform}"
  local runtime_platform
  local runtime_owner
  local header

  mkdir -p \
    "${package_root}/bin" \
    "${package_root}/include" \
    "${package_root}/lib" \
    "${package_root}/pkg" \
    "${package_root}/toolchain"
  cp "${component_root}/bin/feng" "${package_root}/bin/feng"
  chmod 0755 "${package_root}/bin/feng"
  for header in "${PUBLIC_HEADERS[@]}"; do
    cp \
      "${COMPONENTS_ROOT}/macos-arm64/include/${header}" \
      "${package_root}/include/${header}"
  done
  for runtime_platform in "${RUNTIME_PLATFORMS[@]}"; do
    runtime_owner="$(runtime_component_platform "${runtime_platform}")"
    mkdir -p "${package_root}/lib/${runtime_platform}"
    cp \
      "${COMPONENTS_ROOT}/${runtime_owner}/lib/${runtime_platform}/libfeng_runtime.a" \
      "${package_root}/lib/${runtime_platform}/libfeng_runtime.a"
  done
  copy_tree \
    "${SOURCE_ROOT}/toolchain/llvm/${host_platform}" \
    "${package_root}/toolchain/llvm"
  copy_tree \
    "${SOURCE_ROOT}/toolchain/sysroot" \
    "${package_root}/toolchain/sysroot"
  if [[ -n "${BUNDLED_PACKAGES_ROOT}" ]]; then
    copy_tree "${BUNDLED_PACKAGES_ROOT}" "${package_root}/pkg"
  fi
  printf '%s\n' "${VERSION}" > "${package_root}/VERSION"

  [[ "$(find "${package_root}/lib" -type f -name 'libfeng_runtime.a' | wc -l | tr -d ' ')" == "5" ]] ||
    die "assembled package does not contain exactly five runtime archives: ${package_name}"
  [[ "$(find "${package_root}/toolchain/sysroot" -mindepth 1 -maxdepth 1 -type d -name 'linux-*' | wc -l | tr -d ' ')" == "4" ]] ||
    die "assembled package does not contain exactly four Linux sysroots: ${package_name}"
}

# Create one zip with its required top-level distribution directory.
archive_distribution() {
  local host_platform="$1"
  local package_name="feng-${VERSION}-${host_platform}"
  local archive_path="${WORK_ROOT}/archives/${package_name}.zip"

  (
    cd "${WORK_ROOT}/packages"
    zip -q -X -y -r "${archive_path}" "${package_name}"
  )
  require_file "${archive_path}"
}

trap cleanup EXIT

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --version=*)
      [[ -z "${VERSION}" ]] || die "--version may only be specified once"
      VERSION="${1#--version=}"
      ;;
    --components=*)
      [[ -z "${COMPONENTS_ROOT}" ]] ||
        die "--components may only be specified once"
      COMPONENTS_ROOT="${1#--components=}"
      ;;
    --component-archives=*)
      [[ -z "${COMPONENT_ARCHIVES_ROOT}" ]] ||
        die "--component-archives may only be specified once"
      COMPONENT_ARCHIVES_ROOT="${1#--component-archives=}"
      ;;
    --output=*)
      [[ -z "${OUTPUT_ROOT}" ]] || die "--output may only be specified once"
      OUTPUT_ROOT="${1#--output=}"
      ;;
    --packages=*)
      [[ -z "${BUNDLED_PACKAGES_ROOT}" ]] ||
        die "--packages may only be specified once"
      BUNDLED_PACKAGES_ROOT="${1#--packages=}"
      ;;
    --source-root=*)
      [[ "${SOURCE_ROOT}" == "${DEFAULT_SOURCE_ROOT}" ]] ||
        die "--source-root may only be specified once"
      SOURCE_ROOT="${1#--source-root=}"
      ;;
    --archive-tool=*)
      [[ "${ARCHIVE_TOOL}" == "ar" ]] ||
        die "--archive-tool may only be specified once"
      ARCHIVE_TOOL="${1#--archive-tool=}"
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

[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]] ||
  die "invalid release version: ${VERSION:-<empty>}"
[[ -z "${COMPONENTS_ROOT}" || -z "${COMPONENT_ARCHIVES_ROOT}" ]] ||
  die "--components and --component-archives are mutually exclusive"
[[ -n "${COMPONENTS_ROOT}" || -n "${COMPONENT_ARCHIVES_ROOT}" ]] ||
  die "one of --components or --component-archives is required"
[[ -n "${OUTPUT_ROOT}" ]] || die "--output is required"

require_cmd "${ARCHIVE_TOOL}"
require_cmd awk
require_cmd cmp
require_cmd file
require_cmd find
require_cmd grep
require_cmd unzip
require_cmd zip
if [[ -n "${COMPONENT_ARCHIVES_ROOT}" ]]; then
  require_cmd tar
fi
configure_sha256_tool
require_dir "${SOURCE_ROOT}"
require_file "${SOURCE_ROOT}/VERSION"
[[ "$(sed -n '1p' "${SOURCE_ROOT}/VERSION")" == "${VERSION}" ]] ||
  die "requested version does not match ${SOURCE_ROOT}/VERSION"

mkdir -p "${OUTPUT_ROOT}"
SOURCE_ROOT="$(cd "${SOURCE_ROOT}" && pwd)"
OUTPUT_ROOT="$(cd "${OUTPUT_ROOT}" && pwd)"
if [[ -n "${BUNDLED_PACKAGES_ROOT}" ]]; then
  require_dir "${BUNDLED_PACKAGES_ROOT}"
  BUNDLED_PACKAGES_ROOT="$(cd "${BUNDLED_PACKAGES_ROOT}" && pwd)"
fi
WORK_ROOT="$(mktemp -d "${OUTPUT_ROOT}/.feng-release.XXXXXX")"
mkdir -p "${WORK_ROOT}/archives" "${WORK_ROOT}/packages"
if [[ -n "${COMPONENT_ARCHIVES_ROOT}" ]]; then
  require_dir "${COMPONENT_ARCHIVES_ROOT}"
  COMPONENT_ARCHIVES_ROOT="$(cd "${COMPONENT_ARCHIVES_ROOT}" && pwd)"
  [[ "$(find "${COMPONENT_ARCHIVES_ROOT}" -maxdepth 1 -type f -name 'release-component-*.tar' | wc -l | tr -d ' ')" == "3" ]] ||
    die "component archive directory must contain exactly three component archives"
  COMPONENTS_ROOT="${WORK_ROOT}/components"
  extract_component_archives
else
  require_dir "${COMPONENTS_ROOT}"
  COMPONENTS_ROOT="$(cd "${COMPONENTS_ROOT}" && pwd)"
fi

for host_platform in "${HOST_PLATFORMS[@]}"; do
  verify_component "${host_platform}"
  verify_llvm_toolchain "${host_platform}"
done
verify_public_headers
verify_linux_sysroots
verify_bundled_packages

for host_platform in "${HOST_PLATFORMS[@]}"; do
  final_archive="${OUTPUT_ROOT}/feng-${VERSION}-${host_platform}.zip"
  [[ ! -e "${final_archive}" ]] ||
    die "refusing to overwrite existing release archive: ${final_archive}"
  assemble_distribution "${host_platform}"
  archive_distribution "${host_platform}"
done

for host_platform in "${HOST_PLATFORMS[@]}"; do
  package_name="feng-${VERSION}-${host_platform}"
  mv \
    "${WORK_ROOT}/archives/${package_name}.zip" \
    "${OUTPUT_ROOT}/${package_name}.zip"
done

echo "==> Created Feng ${VERSION} release archives in ${OUTPUT_ROOT}"
