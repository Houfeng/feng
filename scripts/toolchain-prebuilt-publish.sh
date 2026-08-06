#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN_ROOT="${PROJECT_ROOT}/toolchain"
BUILD_ROOT="${PROJECT_ROOT}/build/toolchain-prebuilt"
TAG="${GITHUB_REF_NAME:-}"
OUTPUT_FILE="${GITHUB_OUTPUT:-}"

# Report one fatal publication error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Validate the GitHub Actions context and publication input directory.
verify_inputs() {
  [[ "$#" -eq 0 ]] || die "this script does not accept arguments"
  [[ "${TAG}" =~ ^toolchain-prebuilt/[0-9A-Za-z][0-9A-Za-z._-]*$ ]] ||
    die "invalid toolchain prebuilt tag: ${TAG:-<empty>}"
  [[ -n "${OUTPUT_FILE}" ]] || die "GITHUB_OUTPUT is required"
  [[ -d "${TOOLCHAIN_ROOT}" ]] ||
    die "toolchain directory not found: ${TOOLCHAIN_ROOT}"
  command -v tar >/dev/null 2>&1 || die "missing required command: tar"
}

# Archive the complete toolchain directory at its fixed publication path.
create_archive() {
  local version="${TAG#toolchain-prebuilt/}"
  local archive="${BUILD_ROOT}/toolchain-prebuilt-${version}.tar.gz"

  mkdir -p "${BUILD_ROOT}"
  echo "==> Creating $(basename "${archive}")"
  tar -czf "${archive}" --directory="${PROJECT_ROOT}" toolchain
  printf 'asset=%s\n' "${archive#${PROJECT_ROOT}/}" >> "${OUTPUT_FILE}"
  echo "==> Created ${archive}"
}

# Validate the invocation and create the toolchain archive.
main() {
  verify_inputs "$@"
  create_archive
}

main "$@"
