#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/build"
OUTPUT_ROOT=""
ARCHIVE_PATH=""
PLATFORM=""
WORK_ROOT=""
SHA256_TOOL=""
ARCHIVE_TOOL=""
PUBLIC_HEADERS=(
  "feng_generated.h"
  "feng_runtime.h"
  "feng_runtime_contract.inc"
)

# Print the supported component-staging invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_component.sh \
    --platform=<host-platform> \
    [--output=<component-root>] \
    [--archive=<component-tar>] \
    [--build-root=<build-root>]

At least one of --output or --archive is required.
EOF
}

# Report one fatal component-staging error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove only the component staging directory created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
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

# Verify one executable or object has the expected file format and CPU.
verify_platform_file() {
  local file_path="$1"
  local platform="$2"
  local description="$3"
  local format

  [[ -f "${file_path}" ]] || die "${description} not found: ${file_path}"
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

# Verify every member in one staged runtime archive matches its platform.
verify_runtime_archive() {
  local archive_path="$1"
  local platform="$2"
  local object_path="${WORK_ROOT}/runtime-${platform}.o"
  local member
  local member_count=0

  while IFS= read -r member; do
    [[ -n "${member}" ]] || continue
    case "${member}" in
      "__.SYMDEF"|"__.SYMDEF SORTED"|"/"|"//")
        continue
        ;;
    esac
    case "${member}" in
      */*|.|..)
        die "unsafe archive member in staged runtime: ${member}"
        ;;
    esac
    "${ARCHIVE_TOOL}" p "${archive_path}" "${member}" > "${object_path}" ||
      die "failed to read ${member} from staged runtime ${platform}"
    verify_platform_file \
      "${object_path}" "${platform}" \
      "staged runtime archive member ${member}"
    member_count=$((member_count + 1))
  done < <("${ARCHIVE_TOOL}" t "${archive_path}")
  [[ "${member_count}" -gt 0 ]] ||
    die "staged runtime archive contains no members: ${platform}"
}

# Print the runtime platforms produced by one native release host.
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
      die "unsupported release host platform: $1"
      ;;
  esac
}

# Print a portable SHA-256 record relative to the component directory.
print_sha256_record() {
  local component_root="$1"
  local relative_path="$2"
  local digest

  if [[ "${SHA256_TOOL}" == "sha256sum" ]]; then
    digest="$(sha256sum "${component_root}/${relative_path}" | awk '{print $1}')"
  else
    digest="$(shasum -a 256 "${component_root}/${relative_path}" | awk '{print $1}')"
  fi
  printf '%s  %s\n' "${digest}" "${relative_path}"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --platform=*)
      [[ -z "${PLATFORM}" ]] || die "--platform may only be specified once"
      PLATFORM="${1#--platform=}"
      ;;
    --output=*)
      [[ -z "${OUTPUT_ROOT}" ]] || die "--output may only be specified once"
      OUTPUT_ROOT="${1#--output=}"
      ;;
    --archive=*)
      [[ -z "${ARCHIVE_PATH}" ]] || die "--archive may only be specified once"
      ARCHIVE_PATH="${1#--archive=}"
      ;;
    --build-root=*)
      [[ "${BUILD_ROOT}" == "${PROJECT_ROOT}/build" ]] ||
        die "--build-root may only be specified once"
      BUILD_ROOT="${1#--build-root=}"
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

trap cleanup EXIT

[[ -n "${PLATFORM}" ]] || die "--platform is required"
[[ -n "${OUTPUT_ROOT}" || -n "${ARCHIVE_PATH}" ]] ||
  die "at least one of --output or --archive is required"
configure_sha256_tool
command -v file >/dev/null 2>&1 || die "missing required command: file"
if [[ -n "${ARCHIVE_PATH}" ]]; then
  command -v tar >/dev/null 2>&1 || die "missing required command: tar"
fi

# shellcheck source=host_platform.sh
source "${SCRIPT_DIR}/host_platform.sh"
DETECTED_PLATFORM="$(feng_detect_host_platform)"
[[ "${DETECTED_PLATFORM}" == "${PLATFORM}" ]] ||
  die "component platform ${PLATFORM} does not match native host ${DETECTED_PLATFORM}"

VERSION="$(sed -n '1p' "${PROJECT_ROOT}/VERSION")"
[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]] ||
  die "invalid VERSION: ${VERSION:-<empty>}"
[[ -x "${BUILD_ROOT}/bin/feng" ]] ||
  die "Feng executable is missing or not executable: ${BUILD_ROOT}/bin/feng"
[[ "$("${BUILD_ROOT}/bin/feng" --version)" == "feng ${VERSION}" ]] ||
  die "Feng executable version does not match VERSION"
ARCHIVE_TOOL="${BUILD_ROOT}/toolchain/llvm/bin/llvm-ar"
[[ -x "${ARCHIVE_TOOL}" ]] ||
  die "bundled llvm-ar is missing or not executable: ${ARCHIVE_TOOL}"

mkdir -p "${BUILD_ROOT}"
BUILD_ROOT="$(cd "${BUILD_ROOT}" && pwd)"
if [[ -n "${OUTPUT_ROOT}" ]]; then
  mkdir -p "${OUTPUT_ROOT}"
  OUTPUT_ROOT="$(cd "${OUTPUT_ROOT}" && pwd)"
  FINAL_COMPONENT_ROOT="${OUTPUT_ROOT}/${PLATFORM}"
  [[ ! -e "${FINAL_COMPONENT_ROOT}" ]] ||
    die "refusing to overwrite existing release component: ${FINAL_COMPONENT_ROOT}"
fi
if [[ -n "${ARCHIVE_PATH}" ]]; then
  ARCHIVE_DIR="$(dirname "${ARCHIVE_PATH}")"
  mkdir -p "${ARCHIVE_DIR}"
  ARCHIVE_DIR="$(cd "${ARCHIVE_DIR}" && pwd)"
  ARCHIVE_PATH="${ARCHIVE_DIR}/$(basename "${ARCHIVE_PATH}")"
  [[ ! -e "${ARCHIVE_PATH}" ]] ||
    die "refusing to overwrite existing component archive: ${ARCHIVE_PATH}"
fi
WORK_ROOT="$(mktemp -d "${BUILD_ROOT}/.release-component.${PLATFORM}.XXXXXX")"
COMPONENT_ROOT="${WORK_ROOT}/${PLATFORM}"
mkdir -p "${COMPONENT_ROOT}/bin" "${COMPONENT_ROOT}/include"
cp "${BUILD_ROOT}/bin/feng" "${COMPONENT_ROOT}/bin/feng"
chmod 0755 "${COMPONENT_ROOT}/bin/feng"

for header in "${PUBLIC_HEADERS[@]}"; do
  [[ -f "${BUILD_ROOT}/include/${header}" ]] ||
    die "public header not found: ${BUILD_ROOT}/include/${header}"
  cp "${BUILD_ROOT}/include/${header}" "${COMPONENT_ROOT}/include/${header}"
done

while IFS= read -r runtime_platform; do
  runtime_source="${BUILD_ROOT}/lib/${runtime_platform}/libfeng_runtime.a"
  [[ -f "${runtime_source}" ]] ||
    die "runtime archive not found: ${runtime_source}"
  mkdir -p "${COMPONENT_ROOT}/lib/${runtime_platform}"
  cp \
    "${runtime_source}" \
    "${COMPONENT_ROOT}/lib/${runtime_platform}/libfeng_runtime.a"
done < <(component_runtime_platforms "${PLATFORM}")

verify_platform_file \
  "${COMPONENT_ROOT}/bin/feng" "${PLATFORM}" \
  "staged Feng executable"
while IFS= read -r runtime_platform; do
  verify_runtime_archive \
    "${COMPONENT_ROOT}/lib/${runtime_platform}/libfeng_runtime.a" \
    "${runtime_platform}"
done < <(component_runtime_platforms "${PLATFORM}")

MANIFEST_TEMP="${COMPONENT_ROOT}/.SHA256SUMS.tmp"
: > "${MANIFEST_TEMP}"
print_sha256_record "${COMPONENT_ROOT}" "bin/feng" >> "${MANIFEST_TEMP}"
for header in "${PUBLIC_HEADERS[@]}"; do
  print_sha256_record \
    "${COMPONENT_ROOT}" "include/${header}" >> "${MANIFEST_TEMP}"
done
while IFS= read -r runtime_platform; do
  print_sha256_record \
    "${COMPONENT_ROOT}" \
    "lib/${runtime_platform}/libfeng_runtime.a" >> "${MANIFEST_TEMP}"
done < <(component_runtime_platforms "${PLATFORM}")
LC_ALL=C sort "${MANIFEST_TEMP}" > "${COMPONENT_ROOT}/SHA256SUMS"
rm "${MANIFEST_TEMP}"

if [[ -n "${ARCHIVE_PATH}" ]]; then
  tar -cf "${WORK_ROOT}/release-component-${PLATFORM}.tar" \
    -C "${WORK_ROOT}" "${PLATFORM}"
fi
if [[ -n "${OUTPUT_ROOT}" ]]; then
  mv "${COMPONENT_ROOT}" "${FINAL_COMPONENT_ROOT}"
fi
if [[ -n "${ARCHIVE_PATH}" ]]; then
  mv "${WORK_ROOT}/release-component-${PLATFORM}.tar" "${ARCHIVE_PATH}"
fi
rm -rf "${WORK_ROOT}"
WORK_ROOT=""
if [[ -n "${OUTPUT_ROOT}" ]]; then
  echo "==> Created release component ${FINAL_COMPONENT_ROOT}"
fi
if [[ -n "${ARCHIVE_PATH}" ]]; then
  echo "==> Created release component archive ${ARCHIVE_PATH}"
fi
