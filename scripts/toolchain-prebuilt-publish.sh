#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN_ROOT="${PROJECT_ROOT}/toolchain"
BUILD_ROOT="${PROJECT_ROOT}/build/toolchain-prebuilt"
REPOSITORY="${GH_REPO:-}"
TAG="${GITHUB_REF_NAME:-}"
WORK_ROOT=""

# Report one fatal publication error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove the staging directory created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
}

# Validate the GitHub Actions context and publication input directory.
verify_inputs() {
  [[ "$#" -eq 0 ]] || die "this script does not accept arguments"
  [[ "${REPOSITORY}" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] ||
    die "invalid GitHub repository: ${REPOSITORY:-<empty>}"
  [[ "${TAG}" =~ ^toolchain-prebuilt/[0-9A-Za-z][0-9A-Za-z._-]*$ ]] ||
    die "invalid toolchain prebuilt tag: ${TAG:-<empty>}"
  [[ -d "${TOOLCHAIN_ROOT}" ]] ||
    die "toolchain directory not found: ${TOOLCHAIN_ROOT}"
  command -v gh >/dev/null 2>&1 || die "missing required command: gh"
  command -v tar >/dev/null 2>&1 || die "missing required command: tar"
}

# Archive the complete toolchain directory and return its path.
create_archive() {
  local version="${TAG#toolchain-prebuilt/}"
  local archive="${WORK_ROOT}/toolchain-prebuilt-${version}.tar.gz"

  echo "==> Creating $(basename "${archive}")" >&2
  tar -czf "${archive}" --directory="${PROJECT_ROOT}" toolchain
  printf '%s\n' "${archive}"
}

# Publish the archive as a regular GitHub Release asset.
publish_release() {
  local archive="$1"
  local version="${TAG#toolchain-prebuilt/}"

  gh release create "${TAG}" "${archive}" \
    --repo "${REPOSITORY}" \
    --verify-tag \
    --latest=false \
    --title "Feng toolchain prebuilt ${version}" \
    --notes "Feng toolchain prebuilt ${version}."
}

# Coordinate toolchain archiving and publication.
main() {
  local archive

  verify_inputs "$@"
  mkdir -p "${BUILD_ROOT}"
  WORK_ROOT="$(mktemp -d "${BUILD_ROOT}/.publish.XXXXXX")"
  trap cleanup EXIT

  archive="$(create_archive)"
  publish_release "${archive}"
  echo "==> Published toolchain prebuilt ${TAG}"
}

main "$@"
