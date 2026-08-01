#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/build"
BUILDER="${SCRIPT_DIR}/release_bundled_packages.sh"
WORKFLOW="${PROJECT_ROOT}/.github/workflows/release.yml"
WORK_ROOT=""
STALE_MARKER="${PROJECT_ROOT}/std/std/build/pkg/release-bundled-packages-stale-9.9.9.fb"
STALE_MARKER_CREATED=false

# Report one bundled-package regression failure.
die() {
  echo "FAIL[release-bundled-packages] $*" >&2
  exit 1
}

# Remove only fixtures created by this regression invocation.
cleanup() {
  if [[ "${STALE_MARKER_CREATED}" == "true" && -f "${STALE_MARKER}" ]]; then
    rm -f "${STALE_MARKER}"
    rmdir "${PROJECT_ROOT}/std/std/build/pkg" 2>/dev/null || true
    rmdir "${PROJECT_ROOT}/std/std/build" 2>/dev/null || true
  fi
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    case "${WORK_ROOT}" in
      "${BUILD_ROOT}"/release-bundled-packages-test.*)
        rm -rf "${WORK_ROOT}"
        ;;
    esac
  fi
}

# Read one quoted field from the package section of a manifest.
package_manifest_value() {
  local manifest_path="$1"
  local field_name="$2"

  awk -v wanted="${field_name}" '
    function trim(value) {
      sub(/^[[:space:]]+/, "", value)
      sub(/[[:space:]]+$/, "", value)
      return value
    }
    /^[[:space:]]*\[package\][[:space:]]*$/ {
      in_package = 1
      next
    }
    /^[[:space:]]*\[/ {
      if (in_package) {
        exit
      }
    }
    in_package {
      separator = index($0, ":")
      if (separator == 0) {
        next
      }
      key = trim(substr($0, 1, separator - 1))
      value = trim(substr($0, separator + 1))
      if (key == wanted && value ~ /^"[^"]+"$/) {
        print substr(value, 2, length(value) - 2)
      }
    }
  ' "${manifest_path}"
}

# Require one exact archive entry in the generated package listing.
require_archive_entry() {
  local listing_path="$1"
  local expected_entry="$2"

  grep -Fqx "${expected_entry}" "${listing_path}" ||
    die "generated std package is missing ${expected_entry}"
}

# Extract one top-level GitHub Actions job block for structural checks.
extract_workflow_job() {
  local job_name="$1"
  local output_path="$2"

  awk -v header="  ${job_name}:" '
    $0 == header {
      in_job = 1
    }
    in_job && $0 != header && $0 ~ /^  [A-Za-z0-9_]+:$/ {
      exit
    }
    in_job {
      print
    }
  ' "${WORKFLOW}" > "${output_path}"
  [[ -s "${output_path}" ]] ||
    die "workflow job not found: ${job_name}"
}

trap cleanup EXIT

[[ -x "${BUILDER}" ]] || die "bundled-package builder is not executable"
mkdir -p "${BUILD_ROOT}"
WORK_ROOT="$(mktemp -d "${BUILD_ROOT}/release-bundled-packages-test.XXXXXX")"

mkdir -p "$(dirname "${STALE_MARKER}")"
[[ ! -e "${STALE_MARKER}" ]] ||
  die "stale-output fixture path already exists: ${STALE_MARKER}"
printf '%s\n' "must not be published" > "${STALE_MARKER}"
STALE_MARKER_CREATED=true

OUTPUT_ROOT="${WORK_ROOT}/output"
"${BUILDER}" --output="${OUTPUT_ROOT}"

SOURCE_MANIFEST="${PROJECT_ROOT}/std/std/feng.fm"
PACKAGE_NAME="$(package_manifest_value "${SOURCE_MANIFEST}" "name")"
PACKAGE_VERSION="$(package_manifest_value "${SOURCE_MANIFEST}" "version")"
SOURCE_PLATFORMS="$(package_manifest_value "${SOURCE_MANIFEST}" "platform")"
[[ -n "${PACKAGE_NAME}" && -n "${PACKAGE_VERSION}" && -n "${SOURCE_PLATFORMS}" ]] ||
  die "std source manifest is missing package coordinates or platforms"
EXPECTED_NAME="${PACKAGE_NAME}-${PACKAGE_VERSION}.fb"
PACKAGE_PATH="${OUTPUT_ROOT}/${EXPECTED_NAME}"
[[ -f "${PACKAGE_PATH}" ]] ||
  die "expected std package was not published: ${PACKAGE_PATH}"
[[ ! -e "${OUTPUT_ROOT}/${STALE_MARKER##*/}" ]] ||
  die "builder copied stale std/std/build/pkg content"
[[ "$(find "${OUTPUT_ROOT}" -mindepth 1 -maxdepth 1 -type f -name '*.fb' | wc -l | tr -d ' ')" == "1" ]] ||
  die "bundled package output must contain exactly one .fb"
[[ -z "$(find "${OUTPUT_ROOT}" -mindepth 1 -maxdepth 1 ! -type f -print -quit)" ]] ||
  die "bundled package output contains a non-file entry"

ARCHIVE_LISTING="${WORK_ROOT}/std-package.list"
ARCHIVE_MANIFEST="${WORK_ROOT}/std-package.feng.fm"
unzip -tqq "${PACKAGE_PATH}" >/dev/null ||
  die "generated std package is not a readable archive"
unzip -Z1 "${PACKAGE_PATH}" > "${ARCHIVE_LISTING}"
unzip -p "${PACKAGE_PATH}" feng.fm > "${ARCHIVE_MANIFEST}"
[[ "$(package_manifest_value "${ARCHIVE_MANIFEST}" "name")" == "${PACKAGE_NAME}" ]] ||
  die "generated std package name differs from std/std/feng.fm"
[[ "$(package_manifest_value "${ARCHIVE_MANIFEST}" "version")" == "${PACKAGE_VERSION}" ]] ||
  die "generated std package version differs from std/std/feng.fm"
[[ "$(package_manifest_value "${ARCHIVE_MANIFEST}" "platform")" == "${SOURCE_PLATFORMS}" ]] ||
  die "generated std package platform set differs from std/std/feng.fm"

IFS=',' read -r -a PLATFORM_LIST <<< "${SOURCE_PLATFORMS}"
for platform in "${PLATFORM_LIST[@]}"; do
  platform="$(printf '%s' "${platform}" | tr -d '[:space:]')"
  [[ -n "${platform}" ]] || die "std/std/feng.fm contains an empty platform"
  require_archive_entry \
    "${ARCHIVE_LISTING}" \
    "lib/${platform}/lib${PACKAGE_NAME}.a"
  grep -Eq "^extlib/${platform}/[^/]+$" "${ARCHIVE_LISTING}" ||
    die "generated std package has no extlib artifact for ${platform}"
done

PACKAGE_SNAPSHOT="${WORK_ROOT}/std-package.snapshot.fb"
cp "${PACKAGE_PATH}" "${PACKAGE_SNAPSHOT}"
if "${BUILDER}" --output="${OUTPUT_ROOT}" >/dev/null 2>&1; then
  die "builder overwrote an existing output directory"
fi
cmp -s "${PACKAGE_PATH}" "${PACKAGE_SNAPSHOT}" ||
  die "existing output changed after overwrite rejection"

FAILED_OUTPUT="${WORK_ROOT}/failed-output"
if PATH="${WORK_ROOT}/missing-command-path" \
  /bin/bash "${BUILDER}" --output="${FAILED_OUTPUT}" >/dev/null 2>&1; then
  die "builder succeeded without its required commands"
fi
[[ ! -e "${FAILED_OUTPUT}" ]] ||
  die "failed builder invocation published a final output directory"

[[ -z "$(find "${WORK_ROOT}" -mindepth 1 -maxdepth 1 -type d -name '.feng-bundled-packages.*' -print -quit)" ]] ||
  die "builder left a package staging directory"
[[ -z "$(find "${BUILD_ROOT}" -mindepth 1 -maxdepth 1 -type d -name 'release-bundled-packages.*' -print -quit)" ]] ||
  die "builder left an isolated project directory"

BUILDER_JOB="${WORK_ROOT}/bundled-packages.job"
ASSEMBLE_JOB="${WORK_ROOT}/assemble.job"
extract_workflow_job "bundled_packages" "${BUILDER_JOB}"
extract_workflow_job "assemble" "${ASSEMBLE_JOB}"
grep -Fq "    needs: prepare" "${BUILDER_JOB}" ||
  die "bundled_packages must depend only on prepare"
grep -Fq "needs.prepare.outputs.release == 'true' || github.event_name == 'workflow_dispatch'" "${BUILDER_JOB}" ||
  die "bundled_packages must retain the release/manual execution condition"
grep -Fq "scripts/release_bundled_packages.sh" "${BUILDER_JOB}" ||
  die "bundled_packages does not invoke the standalone builder"
grep -Fq "name: release-bundled-packages" "${BUILDER_JOB}" ||
  die "bundled_packages does not upload the expected artifact"
[[ "$(grep -Fc "scripts/release_bundled_packages.sh" "${WORKFLOW}")" == "1" ]] ||
  die "workflow must invoke the standalone builder exactly once"
[[ "$(grep -Ec '(^|[[:space:]])(make cli|feng pack)([[:space:]]|$)' "${WORKFLOW}" || true)" == "0" ]] ||
  die "workflow expands bundled-package build logic inline"
grep -Fq "      - bundled_packages" "${ASSEMBLE_JOB}" ||
  die "assemble does not depend on bundled_packages"
grep -Fq "name: release-bundled-packages" "${ASSEMBLE_JOB}" ||
  die "assemble does not download the bundled package artifact"
grep -Fq -- "--packages=build/release-bundled-packages" "${ASSEMBLE_JOB}" ||
  die "assemble does not pass the bundled package directory"

echo "release bundled packages: isolated std build and publication passed"
