#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_ROOT=""
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

# Report one fatal release-script regression failure.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove only the regression workspace created by this invocation.
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

# Print only the SHA-256 digest for one file.
sha256_digest() {
  if [[ "${SHA256_TOOL}" == "sha256sum" ]]; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

# Print the runtime platforms owned by one native component.
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
      die "unsupported test component platform: $1"
      ;;
  esac
}

# Print one component checksum record.
print_sha256_record() {
  local component_root="$1"
  local relative_path="$2"
  local digest

  digest="$(sha256_digest "${component_root}/${relative_path}")"
  printf '%s  %s\n' "${digest}" "${relative_path}"
}

# Create one small cross-format object used as a stand-in host executable.
create_platform_binary() {
  local platform="$1"
  local output_path="$2"
  local target

  case "${platform}" in
    macos-arm64) target="arm64-apple-macosx" ;;
    linux-x64-gnu) target="x86_64-unknown-linux-gnu" ;;
    linux-arm64-gnu) target="aarch64-unknown-linux-gnu" ;;
    *) die "unsupported test binary platform: ${platform}" ;;
  esac
  "${PROJECT_ROOT}/build/toolchain/llvm/bin/clang" \
    "--target=${target}" \
    -c "${WORK_ROOT}/source.c" \
    -o "${output_path}"
  chmod 0755 "${output_path}"
}

# Create one exact component manifest after staging its required files.
create_component_manifest() {
  local host_platform="$1"
  local component_root="$2"
  local manifest="${component_root}/SHA256SUMS"
  local header
  local runtime_platform

  : > "${manifest}"
  print_sha256_record "${component_root}" "bin/feng" >> "${manifest}"
  for header in "${PUBLIC_HEADERS[@]}"; do
    print_sha256_record \
      "${component_root}" "include/${header}" >> "${manifest}"
  done
  while IFS= read -r runtime_platform; do
    print_sha256_record \
      "${component_root}" \
      "lib/${runtime_platform}/libfeng_runtime.a" >> "${manifest}"
  done < <(component_runtime_platforms "${host_platform}")
  LC_ALL=C sort "${manifest}" -o "${manifest}"
}

# Create all three native component directories with real-format small inputs.
create_components() {
  local components_root="$1"
  local host_platform
  local component_root
  local header
  local runtime_platform

  for host_platform in "${HOST_PLATFORMS[@]}"; do
    component_root="${components_root}/${host_platform}"
    mkdir -p "${component_root}/bin" "${component_root}/include"
    create_platform_binary \
      "${host_platform}" "${component_root}/bin/feng"
    for header in "${PUBLIC_HEADERS[@]}"; do
      cp \
        "${PROJECT_ROOT}/build/include/${header}" \
        "${component_root}/include/${header}"
    done
    while IFS= read -r runtime_platform; do
      mkdir -p "${component_root}/lib/${runtime_platform}"
      cp \
        "${PROJECT_ROOT}/extlib/${runtime_platform}/libfeng_unwind.a" \
        "${component_root}/lib/${runtime_platform}/libfeng_runtime.a"
    done < <(component_runtime_platforms "${host_platform}")
    create_component_manifest "${host_platform}" "${component_root}"
  done
}

# Create a minimal but structurally complete repository release-asset root.
create_source_root() {
  local source_root="$1"
  local host_platform
  local platform
  local tool

  printf '%s\n' "0.1.0" > "${source_root}/VERSION"
  for host_platform in "${HOST_PLATFORMS[@]}"; do
    mkdir -p \
      "${source_root}/toolchain/llvm/${host_platform}/bin" \
      "${source_root}/toolchain/llvm/${host_platform}/lib"
    for tool in clang lld ld.lld llvm-ar llvm-ranlib lldb lldb-dap lldb-argdumper; do
      printf '#!/usr/bin/env sh\nexit 0\n' \
        > "${source_root}/toolchain/llvm/${host_platform}/bin/${tool}"
      chmod 0755 "${source_root}/toolchain/llvm/${host_platform}/bin/${tool}"
    done
    rm \
      "${source_root}/toolchain/llvm/${host_platform}/bin/ld.lld" \
      "${source_root}/toolchain/llvm/${host_platform}/bin/llvm-ranlib"
    ln -s lld "${source_root}/toolchain/llvm/${host_platform}/bin/ld.lld"
    ln -s llvm-ar "${source_root}/toolchain/llvm/${host_platform}/bin/llvm-ranlib"
    case "${host_platform}" in
      macos-arm64) tool="debugserver" ;;
      linux-*) tool="lldb-server" ;;
    esac
    printf '#!/usr/bin/env sh\nexit 0\n' \
      > "${source_root}/toolchain/llvm/${host_platform}/bin/${tool}"
    chmod 0755 "${source_root}/toolchain/llvm/${host_platform}/bin/${tool}"
  done
  for platform in "${LINUX_PLATFORMS[@]}"; do
    mkdir -p \
      "${source_root}/toolchain/sysroot/${platform}/usr/include" \
      "${source_root}/toolchain/sysroot/${platform}/usr/lib" \
      "${source_root}/toolchain/sysroot/${platform}/lib/gcc"
    printf '%s\n' "${platform}" \
      > "${source_root}/toolchain/sysroot/${platform}/usr/include/.platform"
    printf '%s\n' "${platform}" \
      > "${source_root}/toolchain/sysroot/${platform}/usr/lib/.platform"
  done
}

# Create a PATH curl replacement that serves one local release archive.
create_mock_curl() {
  local mock_path="$1"

  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'for arg in "$@"; do'
    printf '%s\n' '  if [[ "${arg}" == *"/releases/latest" ]]; then'
    printf '%s\n' '    printf "%s\\n" "https://github.com/Houfeng/feng/releases/tag/v0.1.0"'
    printf '%s\n' '    exit 0'
    printf '%s\n' '  fi'
    printf '%s\n' 'done'
    printf '%s\n' 'output=""'
    printf '%s\n' 'while [[ "$#" -gt 0 ]]; do'
    printf '%s\n' '  if [[ "$1" == "-o" ]]; then'
    printf '%s\n' '    shift'
    printf '%s\n' '    output="$1"'
    printf '%s\n' '  fi'
    printf '%s\n' '  shift'
    printf '%s\n' 'done'
    printf '%s\n' '[[ -n "${output}" ]]'
    printf '%s\n' 'cp "${MOCK_ARCHIVE}" "${output}"'
  } > "${mock_path}"
  chmod 0755 "${mock_path}"
}

# Create a PATH mv replacement that fails one shell-file commit then delegates.
create_single_failure_mv() {
  local mock_path="$1"

  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'destination="${!#}"'
    printf '%s\n' 'if [[ "${destination}" == "${HOME}/.zshrc" && ! -e "${MOCK_MV_MARKER}" ]]; then'
    printf '%s\n' '  : > "${MOCK_MV_MARKER}"'
    printf '%s\n' '  exit 1'
    printf '%s\n' 'fi'
    printf '%s\n' 'exec /bin/mv "$@"'
  } > "${mock_path}"
  chmod 0755 "${mock_path}"
}

# Create a PATH gh replacement for deterministic publication regression cases.
create_mock_gh() {
  local mock_path="$1"

  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'printf "%s\\n" "$*" >> "${MOCK_GH_LOG}"'
    printf '%s\n' 'case "$1" in'
    printf '%s\n' '  api)'
    printf '%s\n' '    case "${MOCK_GH_MODE}" in'
    printf '%s\n' '      missing)'
    printf '%s\n' '        echo "gh: Not Found (HTTP 404)" >&2'
    printf '%s\n' '        exit 1'
    printf '%s\n' '        ;;'
    printf '%s\n' '      auth)'
    printf '%s\n' '        echo "gh: authentication failed (HTTP 401)" >&2'
    printf '%s\n' '        exit 1'
    printf '%s\n' '        ;;'
    printf '%s\n' '      draft)'
    printf '%s\n' '        printf "true\\tfalse\\tfalse\\n"'
    printf '%s\n' '        ;;'
    printf '%s\n' '      *)'
    printf '%s\n' '        exit 2'
    printf '%s\n' '        ;;'
    printf '%s\n' '    esac'
    printf '%s\n' '    ;;'
    printf '%s\n' '  release)'
    printf '%s\n' '    exit 0'
    printf '%s\n' '    ;;'
    printf '%s\n' '  *)'
    printf '%s\n' '    exit 2'
    printf '%s\n' '    ;;'
    printf '%s\n' 'esac'
  } > "${mock_path}"
  chmod 0755 "${mock_path}"
}

trap cleanup EXIT
configure_sha256_tool
mkdir -p "${PROJECT_ROOT}/build"
WORK_ROOT="$(mktemp -d "${PROJECT_ROOT}/build/release-scripts-test.XXXXXX")"
printf '%s\n' 'int feng_release_test_object;' > "${WORK_ROOT}/source.c"

VERSION_TEST_FILE="${WORK_ROOT}/VERSION"
printf '%s\n' '0.1.0' > "${VERSION_TEST_FILE}"
VERSION_OUTPUT="$("${SCRIPT_DIR}/release_version.sh" resolve \
  --event-name=push \
  --ref-type=branch \
  --ref-name=main \
  --version-file="${VERSION_TEST_FILE}")"
[[ "${VERSION_OUTPUT}" == $'version=0.1.0\ntag=\nrelease=false' ]] ||
  die "branch build version resolution returned unexpected values"
VERSION_OUTPUT="$("${SCRIPT_DIR}/release_version.sh" resolve \
  --event-name=create \
  --ref-type=tag \
  --ref-name=v1.2.3-rc.1 \
  --version-file="${VERSION_TEST_FILE}")"
[[ "${VERSION_OUTPUT}" == $'version=1.2.3-rc.1\ntag=v1.2.3-rc.1\nrelease=true' ]] ||
  die "release tag version resolution returned unexpected values"
if "${SCRIPT_DIR}/release_version.sh" resolve \
  --event-name=create \
  --ref-type=tag \
  --ref-name=release-1.2.3 \
  --version-file="${VERSION_TEST_FILE}" \
  >/dev/null 2>&1; then
  die "release version resolution accepted an invalid tag"
fi
"${SCRIPT_DIR}/release_version.sh" set \
  --version=2.0.0 \
  --version-file="${VERSION_TEST_FILE}"
[[ "$(sed -n '1p' "${VERSION_TEST_FILE}")" == "2.0.0" ]] ||
  die "release version configuration did not update the version file"

NATIVE_PLATFORM="$(source "${SCRIPT_DIR}/host_platform.sh"; feng_detect_host_platform)"
[[ "$("${PROJECT_ROOT}/build/bin/feng" --version)" == "feng $(sed -n '1p' "${PROJECT_ROOT}/VERSION")" ]] ||
  die "Makefile-built Feng version does not match VERSION"
"${SCRIPT_DIR}/release_component.sh" \
  --platform="${NATIVE_PLATFORM}" \
  --archive="${WORK_ROOT}/release-component-${NATIVE_PLATFORM}.tar" >/dev/null
tar -tf "${WORK_ROOT}/release-component-${NATIVE_PLATFORM}.tar" |
  grep -qx "${NATIVE_PLATFORM}/SHA256SUMS" ||
  die "native release component archive did not contain SHA256SUMS"

SOURCE_ROOT="${WORK_ROOT}/source-root"
COMPONENTS_ROOT="${WORK_ROOT}/components"
COMPONENT_ARCHIVES_ROOT="${WORK_ROOT}/component-archives"
OUTPUT_ROOT="${WORK_ROOT}/release"
mkdir -p \
  "${SOURCE_ROOT}" \
  "${COMPONENTS_ROOT}" \
  "${COMPONENT_ARCHIVES_ROOT}" \
  "${OUTPUT_ROOT}"
create_source_root "${SOURCE_ROOT}"
create_components "${COMPONENTS_ROOT}"
for host_platform in "${HOST_PLATFORMS[@]}"; do
  tar -cf \
    "${COMPONENT_ARCHIVES_ROOT}/release-component-${host_platform}.tar" \
    -C "${COMPONENTS_ROOT}" "${host_platform}"
done

"${SCRIPT_DIR}/release_assemble.sh" \
  --version=0.1.0 \
  --component-archives="${COMPONENT_ARCHIVES_ROOT}" \
  --output="${OUTPUT_ROOT}" \
  --source-root="${SOURCE_ROOT}" \
  --archive-tool="${PROJECT_ROOT}/build/toolchain/llvm/bin/llvm-ar"
[[ "$(find "${OUTPUT_ROOT}" -type f -name '*.zip' | wc -l | tr -d ' ')" == "3" ]] ||
  die "release assembly did not create exactly three archives"
if "${SCRIPT_DIR}/release_assemble.sh" \
  --version=0.1.0.rc.1 \
  --components="${COMPONENTS_ROOT}" \
  --output="${WORK_ROOT}/invalid-version-output" \
  --source-root="${SOURCE_ROOT}" \
  --archive-tool="${PROJECT_ROOT}/build/toolchain/llvm/bin/llvm-ar" \
  >/dev/null 2>&1; then
  die "release assembly accepted an invalid prerelease version"
fi

PACKAGE_NAME="feng-0.1.0-${NATIVE_PLATFORM}"
ARCHIVE_PATH="${OUTPUT_ROOT}/${PACKAGE_NAME}.zip"
[[ -f "${ARCHIVE_PATH}" ]] ||
  die "native test release archive not found: ${ARCHIVE_PATH}"

BAD_COMPONENTS="${WORK_ROOT}/bad-components"
BAD_OUTPUT="${WORK_ROOT}/bad-output"
cp -R "${COMPONENTS_ROOT}" "${BAD_COMPONENTS}"
printf '%s\n' 'corrupt' >> "${BAD_COMPONENTS}/linux-x64-gnu/include/feng_runtime.h"
if "${SCRIPT_DIR}/release_assemble.sh" \
  --version=0.1.0 \
  --components="${BAD_COMPONENTS}" \
  --output="${BAD_OUTPUT}" \
  --source-root="${SOURCE_ROOT}" \
  --archive-tool="${PROJECT_ROOT}/build/toolchain/llvm/bin/llvm-ar" \
  >/dev/null 2>&1; then
  die "release assembly accepted a stale component manifest"
fi
if find "${BAD_OUTPUT}" -type f -name '*.zip' -print -quit | grep -q .; then
  die "failed release assembly left a partial archive"
fi

MOCK_BIN="${WORK_ROOT}/mock-bin"
INSTALL_HOME="${WORK_ROOT}/home"
INSTALL_TEMP="${WORK_ROOT}/install-temp"
mkdir -p "${MOCK_BIN}" "${INSTALL_HOME}" "${INSTALL_TEMP}"
create_mock_curl "${MOCK_BIN}/curl"
create_mock_gh "${MOCK_BIN}/gh"

GH_LOG="${WORK_ROOT}/gh.log"
: > "${GH_LOG}"
GH_REPO="Houfeng/feng" \
MOCK_GH_LOG="${GH_LOG}" \
MOCK_GH_MODE=draft \
PATH="${MOCK_BIN}:${PATH}" \
  "${SCRIPT_DIR}/release_publish_github.sh" \
    --tag=v0.1.0 \
    --version=0.1.0 \
    --packages="${OUTPUT_ROOT}" >/dev/null
grep -q '^release upload v0.1.0 ' "${GH_LOG}" ||
  die "GitHub publication did not upload to an existing release"
grep -q '^release edit v0.1.0 --draft=false ' "${GH_LOG}" ||
  die "GitHub publication did not publish an existing draft"

RC_PACKAGES="${WORK_ROOT}/rc-packages"
mkdir -p "${RC_PACKAGES}"
for host_platform in "${HOST_PLATFORMS[@]}"; do
  cp \
    "${OUTPUT_ROOT}/feng-0.1.0-${host_platform}.zip" \
    "${RC_PACKAGES}/feng-0.1.0-rc.1-${host_platform}.zip"
done
: > "${GH_LOG}"
GH_REPO="Houfeng/feng" \
MOCK_GH_LOG="${GH_LOG}" \
MOCK_GH_MODE=missing \
PATH="${MOCK_BIN}:${PATH}" \
  "${SCRIPT_DIR}/release_publish_github.sh" \
    --tag=v0.1.0-rc.1 \
    --version=0.1.0-rc.1 \
    --packages="${RC_PACKAGES}" >/dev/null
grep -q '^release create v0.1.0-rc.1 .* --prerelease$' "${GH_LOG}" ||
  die "GitHub publication did not create a prerelease"

: > "${GH_LOG}"
if GH_REPO="Houfeng/feng" \
   MOCK_GH_LOG="${GH_LOG}" \
   MOCK_GH_MODE=auth \
   PATH="${MOCK_BIN}:${PATH}" \
     "${SCRIPT_DIR}/release_publish_github.sh" \
       --tag=v0.1.0 \
       --version=0.1.0 \
       --packages="${OUTPUT_ROOT}" \
       >/dev/null 2>&1; then
  die "GitHub publication treated an authentication error as a missing release"
fi
if grep -q '^release create ' "${GH_LOG}"; then
  die "GitHub publication created a release after an authentication error"
fi

for iteration in 1 2; do
  HOME="${INSTALL_HOME}" \
  SHELL="/bin/zsh" \
  TMPDIR="${INSTALL_TEMP}" \
  MOCK_ARCHIVE="${ARCHIVE_PATH}" \
  PATH="${MOCK_BIN}:${PATH}" \
    "${SCRIPT_DIR}/install.sh" >/dev/null
done
[[ -x "${INSTALL_HOME}/.feng/bin/feng" ]] ||
  die "installer did not create the Feng executable"
[[ "$(grep -Fxc 'export PATH="$HOME/.feng/bin:$PATH"' "${INSTALL_HOME}/.zshrc")" == "1" ]] ||
  die "installer duplicated or omitted the PATH entry"

FENG_BEFORE="$(sha256_digest "${INSTALL_HOME}/.feng/bin/feng")"
SHELL_BEFORE="$(sha256_digest "${INSTALL_HOME}/.zshrc")"
create_single_failure_mv "${MOCK_BIN}/mv"
if HOME="${INSTALL_HOME}" \
   SHELL="/bin/zsh" \
   TMPDIR="${INSTALL_TEMP}" \
   MOCK_ARCHIVE="${ARCHIVE_PATH}" \
   MOCK_MV_MARKER="${WORK_ROOT}/mv-failed" \
   PATH="${MOCK_BIN}:${PATH}" \
     "${SCRIPT_DIR}/install.sh" >/dev/null 2>&1; then
  die "installer succeeded after the injected shell-file commit failure"
fi
[[ "$(sha256_digest "${INSTALL_HOME}/.feng/bin/feng")" == "${FENG_BEFORE}" ]] ||
  die "installer did not restore the previous installation"
[[ "$(sha256_digest "${INSTALL_HOME}/.zshrc")" == "${SHELL_BEFORE}" ]] ||
  die "installer did not restore the previous shell startup file"
if find "${INSTALL_HOME}" -maxdepth 1 \
  \( -name '.feng-install.*' -o -name '.feng-backup.*' -o -name '.feng-shell*' \) \
  -print -quit | grep -q .; then
  die "installer failure left staging or backup paths"
fi

echo "release scripts: version, component, assembly, publication, install, idempotency, and rollback passed"
