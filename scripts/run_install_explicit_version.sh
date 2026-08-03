#!/usr/bin/env bash
set -euo pipefail

# Serve deterministic release metadata and archives when invoked as curl.
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

  [[ -n "${url}" ]] || exit 3
  printf '%s\n' "${url}" >> "${MOCK_REQUEST_LOG}"
  case "${url}" in
    */releases/latest)
      if [[ "${MOCK_LATEST_STATUS:-200}" == "network-error" ]]; then
        exit 7
      fi
      printf '%s\n%s\n' \
        "${MOCK_LATEST_STATUS:-200}" \
        "${MOCK_LATEST_URL:-https://github.com/Houfeng/feng/releases/tag/v0.1.0}"
      ;;
    https://github.com/Houfeng/feng/releases\?*)
      [[ -n "${output}" ]] || exit 4
      if [[ "${url}" == *"page=1" ]]; then
        cp "${MOCK_RELEASES_PAGE}" "${output}"
      else
        printf '<html></html>\n' > "${output}"
      fi
      ;;
    *)
      [[ "${url}" == "${MOCK_EXPECTED_URL}" ]] || exit 5
      [[ -n "${output}" ]] || exit 6
      cp "${MOCK_ARCHIVE}" "${output}"
      ;;
  esac
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

# Assert that one mocked release-resolution failure does not request an archive.
expect_resolution_failure() {
  local name="$1"
  local expected="$2"
  local latest_status="$3"
  local releases_page="$4"
  local test_home="${WORK_ROOT}/home-${name}"
  local request_log="${WORK_ROOT}/${name}-requests.log"

  mkdir -p "${test_home}"
  : > "${request_log}"
  if HOME="${test_home}" \
     SHELL="/bin/zsh" \
     TMPDIR="${INSTALL_TEMP}" \
     MOCK_LATEST_STATUS="${latest_status}" \
     MOCK_LATEST_URL="https://github.com/Houfeng/feng/releases/latest" \
     MOCK_RELEASES_PAGE="${releases_page}" \
     MOCK_EXPECTED_URL="unused" \
     MOCK_ARCHIVE="unused" \
     MOCK_REQUEST_LOG="${request_log}" \
     FENG_INSTALL_TEST_MOCK_CURL=1 \
     PATH="${MOCK_BIN}:${PATH}" \
       "${INSTALL_SCRIPT}" \
       > "${WORK_ROOT}/${name}.out" \
       2> "${WORK_ROOT}/${name}.err"; then
    die "installer unexpectedly resolved a release for ${name}"
  fi
  grep -Fq -- "${expected}" "${WORK_ROOT}/${name}.err" ||
    die "installer reported an unexpected resolution diagnostic for ${name}"
  if grep -q '/releases/download/' "${request_log}"; then
    die "installer requested an archive after release resolution failed for ${name}"
  fi
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
  "empty-channel" \
  "--channel requires a channel name" \
  "--channel="
expect_failure \
  "duplicate-channel" \
  "--channel may only be specified once" \
  "--channel=rc" \
  "--channel=rc"
expect_failure \
  "unsupported-channel" \
  "unsupported Feng release channel: beta" \
  "--channel=beta"
expect_failure \
  "mutually-exclusive-selection" \
  "--version and --channel are mutually exclusive" \
  "--version=v0.1.0" \
  "--channel=rc"

RC_VERSION="0.10.0-rc.10"
RC_PACKAGE_NAME="feng-${RC_VERSION}-${PLATFORM}"
RC_ARCHIVE_PATH="${WORK_ROOT}/${RC_PACKAGE_NAME}.zip"
RC_DOWNLOAD_URL="https://github.com/Houfeng/feng/releases/download/v${RC_VERSION}/${RC_PACKAGE_NAME}.zip"
RELEASES_PAGE="${WORK_ROOT}/releases.html"
create_release_archive "${RC_VERSION}" "${PLATFORM}"
cat > "${RELEASES_PAGE}" <<'EOF'
<html><body>
<a href="/Houfeng/feng/releases/tag/v0.10.0-rc2">v0.10.0-rc2</a>
<a href="/Houfeng/feng/releases/tag/v0.2.0-rc100">v0.2.0-rc100</a>
<a href="/Houfeng/feng/releases/tag/v0.10.0-rc.10">v0.10.0-rc.10</a>
<a href="/Houfeng/feng/releases/tag/v0.10.0-beta.99">v0.10.0-beta.99</a>
<a href="/Houfeng/feng/releases/tag/v0.9.0">v0.9.0</a>
</body></html>
EOF

RC_HOME="${WORK_ROOT}/home-rc-channel"
RC_REQUEST_LOG="${WORK_ROOT}/rc-channel-requests.log"
mkdir -p "${RC_HOME}"
: > "${RC_REQUEST_LOG}"
HOME="${RC_HOME}" \
SHELL="/bin/zsh" \
TMPDIR="${INSTALL_TEMP}" \
MOCK_RELEASES_PAGE="${RELEASES_PAGE}" \
MOCK_ARCHIVE="${RC_ARCHIVE_PATH}" \
MOCK_EXPECTED_URL="${RC_DOWNLOAD_URL}" \
MOCK_REQUEST_LOG="${RC_REQUEST_LOG}" \
FENG_INSTALL_TEST_MOCK_CURL=1 \
PATH="${MOCK_BIN}:${PATH}" \
  "${INSTALL_SCRIPT}" --channel=rc >/dev/null
[[ "$(sed -n '1p' "${RC_HOME}/.feng/VERSION")" == "${RC_VERSION}" ]] ||
  die "installer did not select the semantically greatest RC release"
[[ "$(tail -n 1 "${RC_REQUEST_LOG}")" == "${RC_DOWNLOAD_URL}" ]] ||
  die "RC channel did not request the selected release asset"

FALLBACK_HOME="${WORK_ROOT}/home-rc-fallback"
FALLBACK_REQUEST_LOG="${WORK_ROOT}/rc-fallback-requests.log"
mkdir -p "${FALLBACK_HOME}"
: > "${FALLBACK_REQUEST_LOG}"
HOME="${FALLBACK_HOME}" \
SHELL="/bin/zsh" \
TMPDIR="${INSTALL_TEMP}" \
MOCK_LATEST_STATUS=200 \
MOCK_LATEST_URL="https://github.com/Houfeng/feng/releases" \
MOCK_RELEASES_PAGE="${RELEASES_PAGE}" \
MOCK_ARCHIVE="${RC_ARCHIVE_PATH}" \
MOCK_EXPECTED_URL="${RC_DOWNLOAD_URL}" \
MOCK_REQUEST_LOG="${FALLBACK_REQUEST_LOG}" \
FENG_INSTALL_TEST_MOCK_CURL=1 \
PATH="${MOCK_BIN}:${PATH}" \
  "${INSTALL_SCRIPT}" >/dev/null
[[ "$(sed -n '1p' "${FALLBACK_HOME}/.feng/VERSION")" == "${RC_VERSION}" ]] ||
  die "installer did not fall back to the greatest RC release"

STABLE_VERSION="0.11.0"
STABLE_PACKAGE_NAME="feng-${STABLE_VERSION}-${PLATFORM}"
STABLE_ARCHIVE_PATH="${WORK_ROOT}/${STABLE_PACKAGE_NAME}.zip"
STABLE_DOWNLOAD_URL="https://github.com/Houfeng/feng/releases/download/v${STABLE_VERSION}/${STABLE_PACKAGE_NAME}.zip"
create_release_archive "${STABLE_VERSION}" "${PLATFORM}"
STABLE_HOME="${WORK_ROOT}/home-stable"
STABLE_REQUEST_LOG="${WORK_ROOT}/stable-requests.log"
mkdir -p "${STABLE_HOME}"
: > "${STABLE_REQUEST_LOG}"
HOME="${STABLE_HOME}" \
SHELL="/bin/zsh" \
TMPDIR="${INSTALL_TEMP}" \
MOCK_LATEST_STATUS=200 \
MOCK_LATEST_URL="https://github.com/Houfeng/feng/releases/tag/v${STABLE_VERSION}" \
MOCK_RELEASES_PAGE="${RELEASES_PAGE}" \
MOCK_ARCHIVE="${STABLE_ARCHIVE_PATH}" \
MOCK_EXPECTED_URL="${STABLE_DOWNLOAD_URL}" \
MOCK_REQUEST_LOG="${STABLE_REQUEST_LOG}" \
FENG_INSTALL_TEST_MOCK_CURL=1 \
PATH="${MOCK_BIN}:${PATH}" \
  "${INSTALL_SCRIPT}" >/dev/null
[[ "$(sed -n '1p' "${STABLE_HOME}/.feng/VERSION")" == "${STABLE_VERSION}" ]] ||
  die "installer did not prefer the latest stable release"
if grep -q 'releases?page=' "${STABLE_REQUEST_LOG}"; then
  die "installer queried RC releases when a stable release was available"
fi

NO_RC_PAGE="${WORK_ROOT}/no-rc-releases.html"
printf '%s\n' \
  '<a href="/Houfeng/feng/releases/tag/v1.0.0">v1.0.0</a>' \
  > "${NO_RC_PAGE}"
expect_resolution_failure \
  "latest-network-error" \
  "failed to resolve the latest Feng release" \
  "network-error" \
  "${RELEASES_PAGE}"
if grep -q 'releases?page=' "${WORK_ROOT}/latest-network-error-requests.log"; then
  die "installer fell back to RC after a stable-release network error"
fi
expect_resolution_failure \
  "latest-http-error" \
  "failed to resolve the latest Feng release: HTTP 503" \
  "503" \
  "${RELEASES_PAGE}"
if grep -q 'releases?page=' "${WORK_ROOT}/latest-http-error-requests.log"; then
  die "installer fell back to RC after a stable-release HTTP error"
fi
expect_resolution_failure \
  "no-rc-release" \
  "no Feng RC release is available" \
  "404" \
  "${NO_RC_PAGE}"

echo "install release selection: explicit version, stable, RC, fallback, and errors passed"
