#!/usr/bin/env bash

# Shared native toolchain, staging, and archive validation for std extlibs.

FENG_STD_EXTLIB_BUILD_ROOT=""
FENG_STD_EXTLIB_SOURCE_DIR=""
FENG_STD_EXTLIB_MAKE_ARGS=()

# Report a fatal std extlib build error.
feng_std_extlib_die() {
  echo "error: $*" >&2
  exit 1
}

# Require one command resolved through PATH.
feng_std_extlib_require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    feng_std_extlib_die "missing required command: $1"
}

# Require one executable at a fixed path.
feng_std_extlib_require_executable() {
  [[ -x "$1" ]] ||
    feng_std_extlib_die "required executable not found: $1"
}

# Detect the complete native host platform and configure its bundled toolchain.
feng_std_extlib_configure_host() {
  local host_arch target_triple target_flags

  case "$(uname -m)" in
    arm64|aarch64) host_arch="arm64" ;;
    x86_64|amd64) host_arch="x64" ;;
    *) feng_std_extlib_die "unsupported host architecture: $(uname -m)" ;;
  esac

  case "$(uname -s)" in
    Darwin)
      [[ "${host_arch}" == "arm64" ]] ||
        feng_std_extlib_die "unsupported macOS build environment: macos-${host_arch}"
      FENG_STD_EXTLIB_HOST_OS="macos"
      FENG_STD_EXTLIB_PLATFORM="macos-arm64"
      target_triple="arm64-apple-macosx"
      FENG_STD_EXTLIB_EXPECTED_FORMAT="Mach-O 64-bit object arm64"
      feng_std_extlib_require_cmd xcrun
      FENG_STD_EXTLIB_SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)" ||
        feng_std_extlib_die "failed to locate the macOS SDK with xcrun"
      [[ -d "${FENG_STD_EXTLIB_SDK_PATH}" ]] ||
        feng_std_extlib_die "macOS SDK directory not found: ${FENG_STD_EXTLIB_SDK_PATH}"
      target_flags="--target=${target_triple} -isysroot ${FENG_STD_EXTLIB_SDK_PATH}"
      ;;
    Linux)
      FENG_STD_EXTLIB_HOST_OS="linux"
      FENG_STD_EXTLIB_PLATFORM="linux-${host_arch}-gnu"
      case "${host_arch}" in
        x64)
          target_triple="x86_64-unknown-linux-gnu"
          FENG_STD_EXTLIB_EXPECTED_FORMAT="ELF 64-bit LSB relocatable"
          FENG_STD_EXTLIB_EXPECTED_CPU="x86-64"
          ;;
        arm64)
          target_triple="aarch64-unknown-linux-gnu"
          FENG_STD_EXTLIB_EXPECTED_FORMAT="ELF 64-bit LSB relocatable"
          FENG_STD_EXTLIB_EXPECTED_CPU="ARM aarch64"
          ;;
      esac
      FENG_STD_EXTLIB_SYSROOT="${PROJECT_ROOT}/toolchain/sysroot/${FENG_STD_EXTLIB_PLATFORM}"
      [[ -d "${FENG_STD_EXTLIB_SYSROOT}/usr/include" ]] ||
        feng_std_extlib_die \
          "target sysroot include directory not found: ${FENG_STD_EXTLIB_SYSROOT}/usr/include"
      [[ -d "${FENG_STD_EXTLIB_SYSROOT}/usr/lib" ]] ||
        feng_std_extlib_die \
          "target sysroot library directory not found: ${FENG_STD_EXTLIB_SYSROOT}/usr/lib"
      target_flags="--target=${target_triple} --sysroot=${FENG_STD_EXTLIB_SYSROOT} --gcc-toolchain=${FENG_STD_EXTLIB_SYSROOT}"
      ;;
    *)
      feng_std_extlib_die "unsupported host OS: $(uname -s)"
      ;;
  esac

  FENG_STD_EXTLIB_LLVM_BIN="${PROJECT_ROOT}/toolchain/llvm/${FENG_STD_EXTLIB_PLATFORM}/bin"
  FENG_STD_EXTLIB_CLANG="${FENG_STD_EXTLIB_LLVM_BIN}/clang"
  FENG_STD_EXTLIB_AR="${FENG_STD_EXTLIB_LLVM_BIN}/llvm-ar"
  FENG_STD_EXTLIB_CFLAGS="-O2 -Wall -Wextra -fPIC ${target_flags}"

  feng_std_extlib_require_executable "${FENG_STD_EXTLIB_CLANG}"
  feng_std_extlib_require_executable "${FENG_STD_EXTLIB_AR}"
  feng_std_extlib_require_cmd "${MAKE_BIN}"
  feng_std_extlib_require_cmd file
}

# Remove only this invocation's staging tree and vendored intermediate objects.
feng_std_extlib_cleanup() {
  if [[ -n "${FENG_STD_EXTLIB_SOURCE_DIR}" &&
        "${#FENG_STD_EXTLIB_MAKE_ARGS[@]}" -gt 0 ]]; then
    "${MAKE_BIN}" "${FENG_STD_EXTLIB_MAKE_ARGS[@]}" clean >/dev/null 2>&1 || true
  fi
  if [[ -n "${FENG_STD_EXTLIB_BUILD_ROOT}" &&
        -d "${FENG_STD_EXTLIB_BUILD_ROOT}" ]]; then
    rm -rf "${FENG_STD_EXTLIB_BUILD_ROOT}"
  fi
}

# Build, validate, and atomically publish one native std dependency archive.
feng_std_extlib_build_archive() {
  local source_dir="$1"
  local archive_name="$2"
  local output_dir="${3:-${PROJECT_ROOT}/std/extlib/${FENG_STD_EXTLIB_PLATFORM}}"
  local staging_dir extracted_dir staged_archive output_archive
  local archive_format object_path object_format object_count

  [[ -d "${source_dir}" ]] ||
    feng_std_extlib_die "vendored source directory not found: ${source_dir}"
  [[ -f "${source_dir}/Makefile" ]] ||
    feng_std_extlib_die "vendored Makefile not found: ${source_dir}/Makefile"

  mkdir -p "${PROJECT_ROOT}/temp"
  FENG_STD_EXTLIB_BUILD_ROOT="$(
    mktemp -d "${PROJECT_ROOT}/temp/std-extlib-${FENG_STD_EXTLIB_PLATFORM}.XXXXXX"
  )"
  FENG_STD_EXTLIB_SOURCE_DIR="${source_dir}"
  staging_dir="${FENG_STD_EXTLIB_BUILD_ROOT}/output"
  extracted_dir="${FENG_STD_EXTLIB_BUILD_ROOT}/objects"
  staged_archive="${staging_dir}/${archive_name}"
  output_archive="${output_dir}/${archive_name}"
  mkdir -p "${staging_dir}" "${extracted_dir}"

  FENG_STD_EXTLIB_MAKE_ARGS=(
    -C "${source_dir}"
    "CC=${FENG_STD_EXTLIB_CLANG}"
    "AR=${FENG_STD_EXTLIB_AR}"
    "CFLAGS=${FENG_STD_EXTLIB_CFLAGS}"
    "OUTPUT_DIR=${staging_dir}"
    "OUTPUT_NAME=${archive_name}"
  )
  trap feng_std_extlib_cleanup EXIT

  "${MAKE_BIN}" "${FENG_STD_EXTLIB_MAKE_ARGS[@]}" clean >/dev/null
  echo "==> Building ${archive_name} for ${FENG_STD_EXTLIB_PLATFORM}"
  "${MAKE_BIN}" "${FENG_STD_EXTLIB_MAKE_ARGS[@]}" install
  [[ -f "${staged_archive}" ]] ||
    feng_std_extlib_die "build did not produce ${staged_archive}"

  archive_format="$(file -b "${staged_archive}")"
  [[ "${archive_format}" == *"archive"* ]] ||
    feng_std_extlib_die \
      "unexpected archive format for ${FENG_STD_EXTLIB_PLATFORM}: ${archive_format}"

  (
    cd "${extracted_dir}"
    "${FENG_STD_EXTLIB_AR}" x "${staged_archive}"
  )

  object_count=0
  while IFS= read -r -d '' object_path; do
    object_count=$((object_count + 1))
    object_format="$(file -b "${object_path}")"
    [[ "${object_format}" == *"${FENG_STD_EXTLIB_EXPECTED_FORMAT}"* ]] ||
      feng_std_extlib_die "unexpected object format in ${archive_name}: ${object_format}"
    if [[ "${FENG_STD_EXTLIB_HOST_OS}" == "linux" ]]; then
      [[ "${object_format}" == *"${FENG_STD_EXTLIB_EXPECTED_CPU}"* ]] ||
        feng_std_extlib_die "unexpected CPU architecture in ${archive_name}: ${object_format}"
    fi
  done < <(find "${extracted_dir}" -type f -name '*.o' -print0)
  [[ "${object_count}" -gt 0 ]] ||
    feng_std_extlib_die "archive contains no object files: ${staged_archive}"

  mkdir -p "${output_dir}"
  mv "${staged_archive}" "${output_archive}"
  echo "==> Built and validated ${output_archive}"

  feng_std_extlib_cleanup
  trap - EXIT
  FENG_STD_EXTLIB_BUILD_ROOT=""
  FENG_STD_EXTLIB_SOURCE_DIR=""
  FENG_STD_EXTLIB_MAKE_ARGS=()
}
