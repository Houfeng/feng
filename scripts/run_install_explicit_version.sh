#!/usr/bin/env bash
set -euo pipefail

# Serve the expected local release archive when this script is invoked as curl.
mock_curl() {
  local output=""
  local url=""

  while [[ "$#" -gt 0 ]]; do
    case "$1" in
      -o)
        shift
        [[ "$#" -gt 0 ]] || exit 2
        output="$1"
        ;;
      http://*|https://*)
        url="$1"
        ;;
    esac
    shift
  done

  [[ "${url}" == "${MOCK_EXPECTED_URL}" ]] || exit 3
  [[ -n "${output}" ]] || exit 4
  printf '%s\n' "${url}" >> "${MOCK_REQUEST_LOG}"
  cp "${MOCK_ARCHIVE}" "${output}"
}

if [[ "${FENG_INSTALL_TEST_MOCK_CURL:-0}" == "1" ]]; then
  mock_curl "$@"
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
INSTALL_SCRIPT="${SCRIPT_DIR}/install.sh"
TEST_SCRIPT="${SCRIPT_DIR}/run_install_explicit_version.sh"
WORK_ROOT=""

# Report one fatal explicit-version installer regression failure.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove invocation-owned test files.
cleanup() {
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
}

# Create the minimum package layout accepted by the installer.
create_release_archive() {
  local version="$1"
  local platform="$2"
  local package_name="feng-${version}-${platform}"
  local archive_root="${WORK_ROOT}/archive-root"
  local package_root="${archive_root}/${package_name}"
  local target_platform
  local tool

  mkdir -p "${package_root}/bin" "${package_root}/include"
  printf '#!/usr/bin/env sh\nexit 0\n' > "${package_root}/bin/feng"
  chmod 0755 "${package_root}/bin/feng"
  for target_platform in \
    macos-arm64 \
    linux-x64-gnu \
    linux-x64-musl \
    linux-arm64-gnu \
    linux-arm64-musl; do
    mkdir -p "${package_root}/lib/${target_platform}"
    printf '%s\n' "${target_platform}" \
      > "${package_root}/lib/${target_platform}/libfeng_runtime.a"
  done
  for target_platform in \
    linux-x64-gnu \
    linux-x64-musl \
    linux-arm64-gnu \
    linux-arm64-musl; do
    mkdir -p \
      "${package_root}/toolchain/sysroot/${target_platform}/usr/include" \
      "${package_root}/toolchain/sysroot/${target_platform}/usr/lib" \
      "${package_root}/toolchain/sysroot/${target_platform}/lib/gcc"
  done
  for tool in \
    clang \
    lld \
    ld.lld \
    llvm-ar \
    llvm-ranlib \
    lldb \
    lldb-dap \
    lldb-argdumper; do
    mkdir -p "${package_root}/toolchain/llvm/bin"
    printf '#!/usr/bin/env sh\nexit 0\n' \
      > "${package_root}/toolchain/llvm/bin/${tool}"
    chmod 0755 "${package_root}/toolchain/llvm/bin/${tool}"
  done
  for tool in \
    feng_generated.h \
    feng_runtime.h \
    feng_runtime_contract.inc; do
    printf '%s\n' "${tool}" > "${package_root}/include/${tool}"
  done
  printf '%s\n' "${version}" > "${package_root}/VERSION"

  (
    cd "${archive_root}"
    zip -qry "${WORK_ROOT}/${package_name}.zip" "${package_name}"
  )
}

# Assert that one invalid installer invocation fails with the expected diagnostic.
expect_failure() {
  local name="$1"
  local expected="$2"
  shift 2

  if HOME="${INSTALL_HOME}" \
     SHELL="/bin/zsh" \
       "${INSTALL_SCRIPT}" "$@" \
       > "${WORK_ROOT}/${name}.out" \
       2> "${WORK_ROOT}/${name}.err"; then
    die "installer accepted invalid arguments for ${name}"
  fi
  grep -Fq -- "${expected}" "${WORK_ROOT}/${name}.err" ||
    die "installer reported an unexpected diagnostic for ${name}"
}

trap cleanup EXIT
mkdir -p "${PROJECT_ROOT}/build"
WORK_ROOT="$(mktemp -d "${PROJECT_ROOT}/build/install-version-test.XXXXXX")"
INSTALL_HOME="${WORK_ROOT}/home"
INSTALL_TEMP="${WORK_ROOT}/install-temp"
MOCK_BIN="${WORK_ROOT}/mock-bin"
REQUEST_LOG="${WORK_ROOT}/requests.log"
VERSION="0.1.0-rc.1"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) PLATFORM="macos-arm64" ;;
  Linux-x86_64|Linux-amd64) PLATFORM="linux-x64-gnu" ;;
  Linux-arm64|Linux-aarch64) PLATFORM="linux-arm64-gnu" ;;
  *) die "unsupported test host: $(uname -s)-$(uname -m)" ;;
esac

PACKAGE_NAME="feng-${VERSION}-${PLATFORM}"
ARCHIVE_PATH="${WORK_ROOT}/${PACKAGE_NAME}.zip"
DOWNLOAD_URL="https://github.com/Houfeng/feng/releases/download/v${VERSION}/${PACKAGE_NAME}.zip"
mkdir -p "${INSTALL_HOME}" "${INSTALL_TEMP}" "${MOCK_BIN}"
create_release_archive "${VERSION}" "${PLATFORM}"
ln -s "${TEST_SCRIPT}" "${MOCK_BIN}/curl"

HOME="${INSTALL_HOME}" \
SHELL="/bin/zsh" \
TMPDIR="${INSTALL_TEMP}" \
MOCK_ARCHIVE="${ARCHIVE_PATH}" \
MOCK_EXPECTED_URL="${DOWNLOAD_URL}" \
MOCK_REQUEST_LOG="${REQUEST_LOG}" \
FENG_INSTALL_TEST_MOCK_CURL=1 \
PATH="${MOCK_BIN}:${PATH}" \
  "${INSTALL_SCRIPT}" "--version=v${VERSION}" >/dev/null

[[ "$(sed -n '1p' "${INSTALL_HOME}/.feng/VERSION")" == "${VERSION}" ]] ||
  die "installer did not install the explicitly requested prerelease"
[[ "$(wc -l < "${REQUEST_LOG}" | tr -d ' ')" == "1" ]] ||
  die "installer made an unexpected number of explicit-version requests"
[[ "$(sed -n '1p' "${REQUEST_LOG}")" == "${DOWNLOAD_URL}" ]] ||
  die "installer did not request the explicit prerelease asset"

expect_failure \
  "missing-v-prefix" \
  "invalid Feng release tag: 0.1.0-rc.1" \
  "--version=0.1.0-rc.1"
expect_failure \
  "empty-version" \
  "--version requires a release tag" \
  "--version="
expect_failure \
  "duplicate-version" \
  "--version may only be specified once" \
  "--version=v0.1.0" \
  "--version=v0.1.1"
expect_failure \
  "unsupported-argument" \
  "unsupported argument: --channel=rc" \
  "--channel=rc"

echo "install explicit version: prerelease selection and argument validation passed"
