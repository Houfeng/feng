#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_ROOT=""
FIXTURE_ROOT=""
MOCK_BIN=""
MOCK_LOG=""
TEST_VERSION="latest"
TEST_TAG="toolchain-prebuilt/${TEST_VERSION}"
OLDER_TAG="toolchain-prebuilt/older"

# Report one fatal toolchain-prebuilt fetch regression failure.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove only the regression workspace created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" &&
        "${WORK_ROOT}" == "${PROJECT_ROOT}/build/toolchain-prebuilt-fetch-test."* &&
        -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
}

# Create a minimal complete toolchain fixture accepted by the fetch script.
create_toolchain() {
  local root="$1"
  local platform
  local tool

  for platform in macos-arm64 linux-x64-gnu linux-arm64-gnu; do
    mkdir -p "${root}/toolchain/llvm/${platform}/bin"
    for tool in clang llvm-ar; do
      printf '#!/usr/bin/env sh\nexit 0\n' \
        > "${root}/toolchain/llvm/${platform}/bin/${tool}"
      chmod 0755 "${root}/toolchain/llvm/${platform}/bin/${tool}"
    done
    ln -s llvm-ar "${root}/toolchain/llvm/${platform}/bin/llvm-ranlib"
  done
  for platform in \
    linux-x64-gnu linux-x64-musl linux-arm64-gnu linux-arm64-musl; do
    mkdir -p \
      "${root}/toolchain/sysroot/${platform}/usr/include" \
      "${root}/toolchain/sysroot/${platform}/usr/lib"
    printf '%s\n' "${platform}" \
      > "${root}/toolchain/sysroot/${platform}/usr/include/.platform"
  done
}

# Archive one fixture using the production prebuilt top-level layout.
create_archive() {
  local source_root="$1"
  local archive="$2"

  tar -czf "${archive}" --directory="${source_root}" toolchain
}

# Reset the copied project to a checkout-like toolchain placeholder tree.
reset_fixture_project() {
  rm -rf "${FIXTURE_ROOT}/toolchain" "${FIXTURE_ROOT}/build"
  mkdir -p "${FIXTURE_ROOT}/toolchain"
  printf '%s\n' "checkout-lfs-pointer" \
    > "${FIXTURE_ROOT}/toolchain/original-marker"
  : > "${MOCK_LOG}"
}

# Create a deterministic GitHub CLI replacement for API and asset downloads.
create_mock_gh() {
  local mock_path="$1"

  cat > "${mock_path}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "${MOCK_GH_LOG}"
case "${1:-}" in
  api)
    [[ "${2:-}" == \
      "repos/Houfeng/feng/git/matching-refs/tags/toolchain-prebuilt/" ]] ||
      exit 2
    printf '%s\n' "${MOCK_OLDER_TAG:-}" "${MOCK_RELEASE_TAG:-}"
    ;;
  release)
    case "${2:-}" in
      view)
        case "${3:-}" in
          "${MOCK_OLDER_TAG:-}")
            printf '%s\n' "${MOCK_OLDER_PUBLISHED_AT}"
            ;;
          "${MOCK_RELEASE_TAG:-}")
            printf '%s\n' "${MOCK_RELEASE_PUBLISHED_AT}"
            ;;
          *)
            exit 2
            ;;
        esac
        ;;
      download)
        directory=""
        pattern=""
        shift 3
        while [[ "$#" -gt 0 ]]; do
          case "$1" in
            --dir)
              shift
              directory="$1"
              ;;
            --pattern)
              shift
              pattern="$1"
              ;;
          esac
          shift
        done
        [[ -n "${directory}" && -n "${pattern}" ]]
        cp "${MOCK_RELEASE_ARCHIVE}" "${directory}/${pattern}"
        ;;
      *)
        exit 2
        ;;
    esac
    ;;
  *)
    exit 2
    ;;
esac
EOF
  chmod 0755 "${mock_path}"
}

# Invoke the copied fetch script with deterministic GitHub context.
run_fetch() {
  env \
    PATH="${MOCK_BIN}:${PATH}" \
    GH_TOKEN=test-token \
    GITHUB_REPOSITORY=Houfeng/feng \
    MOCK_GH_LOG="${MOCK_LOG}" \
    MOCK_OLDER_TAG="${MOCK_OLDER_TAG-${OLDER_TAG}}" \
    MOCK_OLDER_PUBLISHED_AT=2026-01-01T00:00:00Z \
    MOCK_RELEASE_TAG="${MOCK_RELEASE_TAG-${TEST_TAG}}" \
    MOCK_RELEASE_PUBLISHED_AT=2026-01-02T00:00:00Z \
    MOCK_RELEASE_ARCHIVE="${MOCK_RELEASE_ARCHIVE}" \
    "${FIXTURE_ROOT}/scripts/toolchain-prebuilt-fetch.sh"
}

trap cleanup EXIT
mkdir -p "${PROJECT_ROOT}/build"
WORK_ROOT="$(mktemp -d "${PROJECT_ROOT}/build/toolchain-prebuilt-fetch-test.XXXXXX")"
FIXTURE_ROOT="${WORK_ROOT}/project"
MOCK_BIN="${WORK_ROOT}/mock-bin"
MOCK_LOG="${WORK_ROOT}/gh.log"
mkdir -p "${FIXTURE_ROOT}/scripts" "${MOCK_BIN}"
cp "${SCRIPT_DIR}/toolchain-prebuilt-fetch.sh" "${FIXTURE_ROOT}/scripts/"
create_mock_gh "${MOCK_BIN}/gh"

VALID_SOURCE="${WORK_ROOT}/valid-source"
VALID_ARCHIVE="${WORK_ROOT}/toolchain-prebuilt-${TEST_VERSION}.tar.gz"
mkdir -p "${VALID_SOURCE}"
create_toolchain "${VALID_SOURCE}"
create_archive "${VALID_SOURCE}" "${VALID_ARCHIVE}"
reset_fixture_project
MOCK_RELEASE_ARCHIVE="${VALID_ARCHIVE}"
FETCH_OUTPUT="$(run_fetch)"
[[ "${FETCH_OUTPUT}" == *"Restored ${TEST_TAG}"* ]] ||
  die "fetch script did not report the selected prebuilt tag"
[[ ! -e "${FIXTURE_ROOT}/toolchain/original-marker" ]] ||
  die "fetch script did not replace the checkout toolchain"
[[ -x "${FIXTURE_ROOT}/toolchain/llvm/macos-arm64/bin/clang" ]] ||
  die "fetch script did not restore the host LLVM layout"
[[ "$(readlink "${FIXTURE_ROOT}/toolchain/llvm/linux-x64-gnu/bin/llvm-ranlib")" == "llvm-ar" ]] ||
  die "fetch script did not preserve internal symbolic links"
grep -Fq \
  'api repos/Houfeng/feng/git/matching-refs/tags/toolchain-prebuilt/' \
  "${MOCK_LOG}" ||
  die "fetch script did not query matching toolchain prebuilt tags"
grep -Fq "release view ${OLDER_TAG}" "${MOCK_LOG}" ||
  die "fetch script did not query the older matching Release"
grep -Fq "release view ${TEST_TAG}" "${MOCK_LOG}" ||
  die "fetch script did not query the latest matching Release"
grep -Fq "release download ${TEST_TAG}" "${MOCK_LOG}" ||
  die "fetch script did not download the selected prebuilt release"
grep -Fq -- "--pattern toolchain-prebuilt-${TEST_VERSION}.tar.gz" "${MOCK_LOG}" ||
  die "fetch script did not request the derived asset name"

INCOMPLETE_SOURCE="${WORK_ROOT}/incomplete-source"
INCOMPLETE_ARCHIVE="${WORK_ROOT}/incomplete.tar.gz"
mkdir -p "${INCOMPLETE_SOURCE}"
create_toolchain "${INCOMPLETE_SOURCE}"
rm -rf "${INCOMPLETE_SOURCE}/toolchain/sysroot/linux-arm64-musl"
create_archive "${INCOMPLETE_SOURCE}" "${INCOMPLETE_ARCHIVE}"
reset_fixture_project
MOCK_RELEASE_ARCHIVE="${INCOMPLETE_ARCHIVE}"
if run_fetch >/dev/null 2>&1; then
  die "fetch script accepted an incomplete toolchain"
fi
[[ -f "${FIXTURE_ROOT}/toolchain/original-marker" ]] ||
  die "incomplete archive changed the checkout toolchain"

ESCAPING_SOURCE="${WORK_ROOT}/escaping-source"
ESCAPING_ARCHIVE="${WORK_ROOT}/escaping.tar.gz"
mkdir -p "${ESCAPING_SOURCE}"
create_toolchain "${ESCAPING_SOURCE}"
ln -s ../../outside "${ESCAPING_SOURCE}/toolchain/escape"
create_archive "${ESCAPING_SOURCE}" "${ESCAPING_ARCHIVE}"
reset_fixture_project
MOCK_RELEASE_ARCHIVE="${ESCAPING_ARCHIVE}"
if run_fetch >/dev/null 2>&1; then
  die "fetch script accepted a symbolic link escaping toolchain"
fi
[[ -f "${FIXTURE_ROOT}/toolchain/original-marker" ]] ||
  die "unsafe archive changed the checkout toolchain"

reset_fixture_project
MOCK_RELEASE_ARCHIVE="${VALID_ARCHIVE}"
MOCK_OLDER_TAG=""
MOCK_RELEASE_TAG=""
if run_fetch >/dev/null 2>&1; then
  die "fetch script accepted a missing toolchain prebuilt release"
fi
[[ -f "${FIXTURE_ROOT}/toolchain/original-marker" ]] ||
  die "missing release changed the checkout toolchain"

reset_fixture_project
MOCK_OLDER_TAG="${OLDER_TAG}"
MOCK_RELEASE_TAG="${TEST_TAG}"
MOCK_RELEASE_ARCHIVE="${WORK_ROOT}/missing-asset.tar.gz"
if run_fetch >/dev/null 2>&1; then
  die "fetch script accepted a missing toolchain prebuilt asset"
fi
[[ -f "${FIXTURE_ROOT}/toolchain/original-marker" ]] ||
  die "missing asset changed the checkout toolchain"

WORKFLOW="${PROJECT_ROOT}/.github/workflows/release.yml"
[[ "$(grep -Fc 'scripts/toolchain-prebuilt-fetch.sh' "${WORKFLOW}")" == "4" ]] ||
  die "release workflow must restore toolchain in exactly four jobs"
if grep -Fq 'lfs: true' "${WORKFLOW}"; then
  die "release workflow still enables Git LFS checkout"
fi

echo "toolchain prebuilt fetch: latest selection, validation, restoration, and workflow wiring passed"
