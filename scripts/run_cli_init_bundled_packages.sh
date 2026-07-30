#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_FENG="${PROJECT_ROOT}/build/bin/feng"
WORK_ROOT="$(mktemp -d "${PROJECT_ROOT}/temp/feng-init-bundled-packages.XXXXXX")"
BUNDLE_SOURCE_INDEX=0

cleanup() {
  rm -rf "${WORK_ROOT}"
}
trap cleanup EXIT

die() {
  echo "FAIL[init_bundled_packages] $*" >&2
  exit 1
}

require_file() {
  [[ -f "$1" ]] || die "missing file: $1"
}

create_install_layout() {
  local layout="$1"

  mkdir -p "${layout}/bin"
  cp "${SOURCE_FENG}" "${layout}/bin/feng"
  chmod +x "${layout}/bin/feng"
}

create_bundle_from_manifest() {
  local bundle_path="$1"
  local manifest_text="$2"
  local source_dir

  BUNDLE_SOURCE_INDEX=$((BUNDLE_SOURCE_INDEX + 1))
  source_dir="${WORK_ROOT}/bundle-source-${BUNDLE_SOURCE_INDEX}"
  mkdir -p "${source_dir}" "$(dirname "${bundle_path}")"
  printf '%s' "${manifest_text}" > "${source_dir}/feng.fm"
  (
    cd "${source_dir}"
    zip -q -X "${bundle_path}" feng.fm
  )
}

create_bundle() {
  local bundle_path="$1"
  local package_name="$2"
  local package_version="$3"

  create_bundle_from_manifest \
    "${bundle_path}" \
    "$(printf '[package]\nname: "%s"\nversion: "%s"\n' \
      "${package_name}" \
      "${package_version}")"
}

create_bundle_without_manifest() {
  local bundle_path="$1"
  local source_dir

  BUNDLE_SOURCE_INDEX=$((BUNDLE_SOURCE_INDEX + 1))
  source_dir="${WORK_ROOT}/bundle-source-${BUNDLE_SOURCE_INDEX}"
  mkdir -p "${source_dir}" "$(dirname "${bundle_path}")"
  printf 'not a manifest\n' > "${source_dir}/note.txt"
  (
    cd "${source_dir}"
    zip -q -X "${bundle_path}" note.txt
  )
}

run_init_success() {
  local feng="$1"
  local project_dir="$2"
  shift 2

  mkdir -p "${project_dir}"
  (
    cd "${project_dir}"
    "${feng}" init "$@"
  )
}

run_init_failure() {
  local feng="$1"
  local project_dir="$2"
  local error_path="$3"

  mkdir -p "${project_dir}"
  if (
    cd "${project_dir}"
    "${feng}" init demo
  ) > /dev/null 2> "${error_path}"; then
    die "init unexpectedly succeeded: ${project_dir}"
  fi
  [[ ! -e "${project_dir}/feng.fm" ]] ||
    die "failed init left feng.fm: ${project_dir}"
  [[ ! -e "${project_dir}/src" ]] ||
    die "failed init left src/: ${project_dir}"
}

dependency_section() {
  sed -n '/^\[dependencies\]$/,$p' "$1"
}

require_no_dependencies() {
  local manifest_path="$1"

  require_file "${manifest_path}"
  if grep -Fq '[dependencies]' "${manifest_path}"; then
    die "manifest unexpectedly contains dependencies: ${manifest_path}"
  fi
}

require_dependency_section() {
  local manifest_path="$1"
  local expected="$2"
  local actual

  require_file "${manifest_path}"
  actual="$(dependency_section "${manifest_path}")"
  [[ "${actual}" == "${expected}" ]] ||
    die "unexpected dependency section in ${manifest_path}: ${actual}"
}

require_file "${SOURCE_FENG}"
command -v zip > /dev/null || die "zip is required"

# A missing pkg/ directory keeps the previous init output.
MISSING_LAYOUT="${WORK_ROOT}/missing-layout"
create_install_layout "${MISSING_LAYOUT}"
run_init_success \
  "${MISSING_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/missing-project" \
  demo
require_no_dependencies "${WORK_ROOT}/missing-project/feng.fm"

# An empty pkg/ and one containing only ignored entries both omit the section.
EMPTY_LAYOUT="${WORK_ROOT}/empty-layout"
create_install_layout "${EMPTY_LAYOUT}"
mkdir -p "${EMPTY_LAYOUT}/pkg"
run_init_success \
  "${EMPTY_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/empty-project" \
  demo
require_no_dependencies "${WORK_ROOT}/empty-project/feng.fm"

printf 'ignored\n' > "${EMPTY_LAYOUT}/pkg/README.txt"
mkdir -p "${EMPTY_LAYOUT}/pkg/directory.fb" "${EMPTY_LAYOUT}/pkg/nested"
create_bundle \
  "${EMPTY_LAYOUT}/pkg/nested/nested-1.0.0.fb" \
  nested \
  1.0.0
run_init_success \
  "${EMPTY_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/ignored-project" \
  demo
require_no_dependencies "${WORK_ROOT}/ignored-project/feng.fm"

# Coordinates come from each root manifest, retain prerelease suffixes, and sort by name.
POPULATED_LAYOUT="${WORK_ROOT}/populated-layout"
create_install_layout "${POPULATED_LAYOUT}"
mkdir -p "${POPULATED_LAYOUT}/pkg"
create_bundle \
  "${POPULATED_LAYOUT}/pkg/filename-does-not-match.fb" \
  zeta \
  2.0.0-beta.2
create_bundle \
  "${POPULATED_LAYOUT}/pkg/another-unrelated-name.fb" \
  alpha \
  1.0.0-rc.1

EXPECTED_DEPENDENCIES="$(printf \
  '[dependencies]\nalpha: "1.0.0-rc.1"\nzeta: "2.0.0-beta.2"')"
run_init_success \
  "${POPULATED_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/populated-bin-project" \
  demo
require_dependency_section \
  "${WORK_ROOT}/populated-bin-project/feng.fm" \
  "${EXPECTED_DEPENDENCIES}"

run_init_success \
  "${POPULATED_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/populated-lib-project" \
  demo \
  --target=lib
require_dependency_section \
  "${WORK_ROOT}/populated-lib-project/feng.fm" \
  "${EXPECTED_DEPENDENCIES}"

# Invalid archives, missing manifests and missing coordinates fail without project residue.
CORRUPT_LAYOUT="${WORK_ROOT}/corrupt-layout"
create_install_layout "${CORRUPT_LAYOUT}"
mkdir -p "${CORRUPT_LAYOUT}/pkg"
printf 'not a zip archive\n' > "${CORRUPT_LAYOUT}/pkg/corrupt.fb"
run_init_failure \
  "${CORRUPT_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/corrupt-project" \
  "${WORK_ROOT}/corrupt.stderr"
grep -Fq "${CORRUPT_LAYOUT}/pkg/corrupt.fb" "${WORK_ROOT}/corrupt.stderr" ||
  die "corrupt bundle error omitted its path"

NO_MANIFEST_LAYOUT="${WORK_ROOT}/no-manifest-layout"
create_install_layout "${NO_MANIFEST_LAYOUT}"
mkdir -p "${NO_MANIFEST_LAYOUT}/pkg"
create_bundle_without_manifest \
  "${NO_MANIFEST_LAYOUT}/pkg/no-manifest.fb"
run_init_failure \
  "${NO_MANIFEST_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/no-manifest-project" \
  "${WORK_ROOT}/no-manifest.stderr"
grep -Fq "${NO_MANIFEST_LAYOUT}/pkg/no-manifest.fb" "${WORK_ROOT}/no-manifest.stderr" ||
  die "missing manifest error omitted its bundle path"

NO_COORDINATE_LAYOUT="${WORK_ROOT}/no-coordinate-layout"
create_install_layout "${NO_COORDINATE_LAYOUT}"
mkdir -p "${NO_COORDINATE_LAYOUT}/pkg"
create_bundle_from_manifest \
  "${NO_COORDINATE_LAYOUT}/pkg/no-version.fb" \
  "$(printf '[package]\nname: "no_version"\n')"
run_init_failure \
  "${NO_COORDINATE_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/no-coordinate-project" \
  "${WORK_ROOT}/no-coordinate.stderr"
grep -Fq '[package].name' "${WORK_ROOT}/no-coordinate.stderr" ||
  die "missing coordinate error omitted the required fields"

# The single-key dependency model rejects multiple bundles with the same name.
DUPLICATE_LAYOUT="${WORK_ROOT}/duplicate-layout"
create_install_layout "${DUPLICATE_LAYOUT}"
mkdir -p "${DUPLICATE_LAYOUT}/pkg"
create_bundle \
  "${DUPLICATE_LAYOUT}/pkg/duplicate-a.fb" \
  duplicate \
  1.0.0
create_bundle \
  "${DUPLICATE_LAYOUT}/pkg/duplicate-b.fb" \
  duplicate \
  2.0.0
run_init_failure \
  "${DUPLICATE_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/duplicate-project" \
  "${WORK_ROOT}/duplicate.stderr"
grep -Fq 'multiple bundled packages declare dependency `duplicate`' \
  "${WORK_ROOT}/duplicate.stderr" ||
  die "duplicate package error omitted its package name"

# An existing non-directory pkg path is a discovery error, not an absent source.
INVALID_PKG_LAYOUT="${WORK_ROOT}/invalid-pkg-layout"
create_install_layout "${INVALID_PKG_LAYOUT}"
printf 'not a directory\n' > "${INVALID_PKG_LAYOUT}/pkg"
run_init_failure \
  "${INVALID_PKG_LAYOUT}/bin/feng" \
  "${WORK_ROOT}/invalid-pkg-project" \
  "${WORK_ROOT}/invalid-pkg.stderr"
grep -Fq "${INVALID_PKG_LAYOUT}/pkg" "${WORK_ROOT}/invalid-pkg.stderr" ||
  die "pkg directory error omitted its path"

echo "feng init bundled package tests passed"
