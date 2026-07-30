#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_ROOT="$(mktemp -d "${PROJECT_ROOT}/temp/feng-init-bundled.XXXXXX")"
INSTALL_ROOT="${WORK_ROOT}/install"
PKG_ROOT="${INSTALL_ROOT}/pkg"
FENG="${INSTALL_ROOT}/bin/feng"
BUNDLE_INDEX=0

cleanup() {
  rm -rf "${WORK_ROOT}"
}
trap cleanup EXIT

die() {
  echo "FAIL[init_bundled_packages] $*" >&2
  exit 1
}

reset_pkg() {
  rm -rf "${PKG_ROOT}"
}

create_bundle_from_manifest() {
  local bundle_path="$1"
  local manifest="$2"
  local source_dir

  BUNDLE_INDEX=$((BUNDLE_INDEX + 1))
  source_dir="${WORK_ROOT}/bundle-${BUNDLE_INDEX}"
  mkdir -p "${source_dir}" "$(dirname "${bundle_path}")"
  printf '%s' "${manifest}" > "${source_dir}/feng.fm"
  (
    cd "${source_dir}"
    zip -q -X "${bundle_path}" feng.fm
  )
}

create_bundle() {
  create_bundle_from_manifest \
    "$1" \
    "$(printf '[package]\nname: "%s"\nversion: "%s"\n' "$2" "$3")"
}

run_init_success() {
  local label="$1"
  shift
  local project="${WORK_ROOT}/project-${label}"

  mkdir -p "${project}"
  (
    cd "${project}"
    "${FENG}" init demo "$@"
  )
  printf '%s\n' "${project}/feng.fm"
}

run_init_failure() {
  local label="$1"
  local project="${WORK_ROOT}/project-${label}"
  local error_path="${WORK_ROOT}/${label}.stderr"

  mkdir -p "${project}"
  if (
    cd "${project}"
    "${FENG}" init demo
  ) > /dev/null 2> "${error_path}"; then
    die "init unexpectedly succeeded: ${label}"
  fi
  [[ ! -e "${project}/feng.fm" && ! -e "${project}/src" ]] ||
    die "failed init left project files: ${label}"
  printf '%s\n' "${error_path}"
}

require_no_dependencies() {
  ! grep -Fq '[dependencies]' "$1" ||
    die "manifest unexpectedly contains dependencies: $1"
}

require_dependencies() {
  local actual

  actual="$(sed -n '/^\[dependencies\]$/,$p' "$1")"
  [[ "${actual}" == "$2" ]] ||
    die "unexpected dependency section: ${actual}"
}

[[ -x "${PROJECT_ROOT}/build/bin/feng" ]] || die "build/bin/feng is required"
command -v zip > /dev/null || die "zip is required"
mkdir -p "${INSTALL_ROOT}/bin"
cp "${PROJECT_ROOT}/build/bin/feng" "${FENG}"

# Missing, empty and irrelevant pkg contents do not add dependencies.
reset_pkg
require_no_dependencies "$(run_init_success missing)"
mkdir -p "${PKG_ROOT}"
require_no_dependencies "$(run_init_success empty)"
printf 'ignored\n' > "${PKG_ROOT}/README.txt"
mkdir -p "${PKG_ROOT}/directory.fb" "${PKG_ROOT}/nested"
create_bundle "${PKG_ROOT}/nested/nested.fb" nested 1.0.0
require_no_dependencies "$(run_init_success ignored)"

# Root manifests are authoritative; output retains versions and sorts by name.
reset_pkg
mkdir -p "${PKG_ROOT}"
create_bundle "${PKG_ROOT}/wrong-zeta-name.fb" zeta 2.0.0-beta.2
create_bundle "${PKG_ROOT}/wrong-alpha-name.fb" alpha 1.0.0-rc.1
EXPECTED="$(printf \
  '[dependencies]\nalpha: "1.0.0-rc.1"\nzeta: "2.0.0-beta.2"')"
require_dependencies "$(run_init_success populated-bin)" "${EXPECTED}"
require_dependencies "$(run_init_success populated-lib --target=lib)" "${EXPECTED}"

# Invalid bundles and unrepresentable coordinates fail before writing the project.
reset_pkg
mkdir -p "${PKG_ROOT}"
printf 'not a zip\n' > "${PKG_ROOT}/corrupt.fb"
ERROR_PATH="$(run_init_failure corrupt)"
grep -Fq "${PKG_ROOT}/corrupt.fb" "${ERROR_PATH}" ||
  die "corrupt bundle error omitted its path"

reset_pkg
mkdir -p "${PKG_ROOT}" "${WORK_ROOT}/no-manifest"
printf 'ignored\n' > "${WORK_ROOT}/no-manifest/note.txt"
(
  cd "${WORK_ROOT}/no-manifest"
  zip -q -X "${PKG_ROOT}/no-manifest.fb" note.txt
)
ERROR_PATH="$(run_init_failure no-manifest)"
grep -Fq "${PKG_ROOT}/no-manifest.fb" "${ERROR_PATH}" ||
  die "missing manifest error omitted its path"

reset_pkg
mkdir -p "${PKG_ROOT}"
create_bundle_from_manifest \
  "${PKG_ROOT}/no-version.fb" \
  "$(printf '[package]\nname: "no_version"\n')"
ERROR_PATH="$(run_init_failure no-version)"
grep -Fq '[package].name' "${ERROR_PATH}" ||
  die "missing coordinate error omitted the required fields"

reset_pkg
mkdir -p "${PKG_ROOT}"
create_bundle "${PKG_ROOT}/duplicate-a.fb" duplicate 1.0.0
create_bundle "${PKG_ROOT}/duplicate-b.fb" duplicate 2.0.0
ERROR_PATH="$(run_init_failure duplicate)"
grep -Fq 'multiple bundled packages declare dependency `duplicate`' "${ERROR_PATH}" ||
  die "duplicate package error omitted its package name"

reset_pkg
printf 'not a directory\n' > "${PKG_ROOT}"
ERROR_PATH="$(run_init_failure invalid-pkg)"
grep -Fq "${PKG_ROOT}" "${ERROR_PATH}" ||
  die "pkg directory error omitted its path"

echo "feng init bundled package tests passed"
