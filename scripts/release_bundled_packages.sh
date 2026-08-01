#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${PROJECT_ROOT}/build"
FENG="${BUILD_ROOT}/bin/feng"
OUTPUT_ROOT=""
OUTPUT_PARENT=""
WORK_ROOT=""
STAGING_ROOT=""
BUNDLED_PROJECTS=(
  "std/std"
)

# Print the supported bundled-package build invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_bundled_packages.sh \
    --output=<bundled-package-directory>

The output directory is created atomically and may be passed directly to
scripts/release_assemble.sh --packages. Existing output is never overwritten.
EOF
}

# Report one fatal bundled-package build error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Require one command resolved through PATH.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

# Remove only temporary directories created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    case "${WORK_ROOT}" in
      "${BUILD_ROOT}"/release-bundled-packages.*)
        rm -rf "${WORK_ROOT}"
        ;;
    esac
  fi
  if [[ -n "${STAGING_ROOT}" && -d "${STAGING_ROOT}" ]]; then
    case "${STAGING_ROOT}" in
      "${OUTPUT_PARENT}"/.feng-bundled-packages.*)
        rm -rf "${STAGING_ROOT}"
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

# Copy one project into an isolated root without its existing build outputs.
stage_project_inputs() {
  local project_relative="$1"
  local source_root="${PROJECT_ROOT}/${project_relative}"
  local destination_root="${WORK_ROOT}/projects/${project_relative}"
  local source_entry
  local entry_name

  [[ -d "${source_root}" ]] ||
    die "bundled project directory not found: ${source_root}"
  [[ -f "${source_root}/feng.fm" ]] ||
    die "bundled project manifest not found: ${source_root}/feng.fm"
  mkdir -p "${destination_root}"
  while IFS= read -r -d '' source_entry; do
    entry_name="${source_entry##*/}"
    [[ "${entry_name}" != "build" ]] || continue
    cp -R -P "${source_entry}" "${destination_root}/"
  done < <(find "${source_root}" -mindepth 1 -maxdepth 1 -print0)
  [[ -f "${destination_root}/feng.fm" ]] ||
    die "failed to stage bundled project manifest: ${project_relative}"
}

# Validate one generated bundle and print its coordinate filename.
validate_bundle() {
  local bundle_path="$1"
  local manifest_path="${WORK_ROOT}/bundle-manifest.feng.fm"
  local manifest_count
  local package_name
  local package_version
  local expected_name

  unzip -tqq "${bundle_path}" >/dev/null ||
    die "bundled package is not a readable .fb archive: ${bundle_path}"
  manifest_count="$(
    unzip -Z1 "${bundle_path}" |
      awk '$0 == "feng.fm" { count += 1 } END { print count + 0 }'
  )"
  [[ "${manifest_count}" == "1" ]] ||
    die "bundled package must contain exactly one top-level feng.fm: ${bundle_path}"
  unzip -p "${bundle_path}" feng.fm > "${manifest_path}" ||
    die "failed to read bundled package manifest: ${bundle_path}"
  package_name="$(package_manifest_value "${manifest_path}" "name")"
  package_version="$(package_manifest_value "${manifest_path}" "version")"
  [[ -n "${package_name}" && "${package_name}" != *$'\n'* ]] ||
    die "bundled package manifest must contain one package name: ${bundle_path}"
  [[ -n "${package_version}" && "${package_version}" != *$'\n'* ]] ||
    die "bundled package manifest must contain one package version: ${bundle_path}"
  expected_name="${package_name}-${package_version}.fb"
  [[ "${bundle_path##*/}" == "${expected_name}" ]] ||
    die "bundled package filename coordinate mismatch: expected ${expected_name}, found ${bundle_path##*/}"
  printf '%s\n' "${expected_name}"
}

# Build and stage the single package produced by one configured project.
build_bundled_project() {
  local project_relative="$1"
  local staged_project="${WORK_ROOT}/projects/${project_relative}"
  local source_manifest="${PROJECT_ROOT}/${project_relative}/feng.fm"
  local package_root="${staged_project}/build/pkg"
  local source_name
  local source_version
  local expected_name
  local package_path=""
  local candidate_path
  local package_count=0
  local unexpected_entry
  local validated_name

  source_name="$(package_manifest_value "${source_manifest}" "name")"
  source_version="$(package_manifest_value "${source_manifest}" "version")"
  [[ -n "${source_name}" && "${source_name}" != *$'\n'* ]] ||
    die "bundled project manifest must contain one package name: ${source_manifest}"
  [[ -n "${source_version}" && "${source_version}" != *$'\n'* ]] ||
    die "bundled project manifest must contain one package version: ${source_manifest}"
  expected_name="${source_name}-${source_version}.fb"

  "${FENG}" pack "${staged_project}"
  [[ -d "${package_root}" ]] ||
    die "bundled project did not produce a package directory: ${project_relative}"
  unexpected_entry="$(
    find "${package_root}" \
      -mindepth 1 -maxdepth 1 \
      \( ! -type f -o ! -name '*.fb' \) \
      -print -quit
  )"
  [[ -z "${unexpected_entry}" ]] ||
    die "bundled project produced an unexpected package entry: ${unexpected_entry}"
  while IFS= read -r candidate_path; do
    [[ -n "${candidate_path}" ]] || continue
    package_path="${candidate_path}"
    package_count=$((package_count + 1))
  done < <(
    find "${package_root}" \
      -mindepth 1 -maxdepth 1 -type f -name '*.fb' -print |
      LC_ALL=C sort
  )
  [[ "${package_count}" == "1" && -n "${package_path}" ]] ||
    die "bundled project must produce exactly one .fb: ${project_relative}"
  [[ "${package_path##*/}" == "${expected_name}" ]] ||
    die "bundled project output coordinate mismatch: expected ${expected_name}, found ${package_path##*/}"
  validated_name="$(validate_bundle "${package_path}")"
  [[ "${validated_name}" == "${expected_name}" ]] ||
    die "bundled project manifest coordinate changed during packaging: ${project_relative}"
  [[ ! -e "${STAGING_ROOT}/${validated_name}" ]] ||
    die "duplicate bundled package coordinate: ${validated_name}"
  cp "${package_path}" "${STAGING_ROOT}/${validated_name}"
}

trap cleanup EXIT

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --output=*)
      [[ -z "${OUTPUT_ROOT}" ]] || die "--output may only be specified once"
      OUTPUT_ROOT="${1#--output=}"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
  shift
done

[[ -n "${OUTPUT_ROOT}" ]] || die "--output is required"
require_cmd awk
require_cmd basename
require_cmd cp
require_cmd dirname
require_cmd find
require_cmd make
require_cmd mkdir
require_cmd mktemp
require_cmd mv
require_cmd sort
require_cmd tr
require_cmd unzip
require_cmd wc

mkdir -p "$(dirname "${OUTPUT_ROOT}")"
OUTPUT_PARENT="$(cd "$(dirname "${OUTPUT_ROOT}")" && pwd)"
OUTPUT_ROOT="${OUTPUT_PARENT}/$(basename "${OUTPUT_ROOT}")"
[[ ! -e "${OUTPUT_ROOT}" ]] ||
  die "refusing to overwrite existing bundled package output: ${OUTPUT_ROOT}"

make -C "${PROJECT_ROOT}" cli
[[ -x "${FENG}" ]] || die "built Feng executable not found: ${FENG}"

mkdir -p "${BUILD_ROOT}"
WORK_ROOT="$(mktemp -d "${BUILD_ROOT}/release-bundled-packages.XXXXXX")"
STAGING_ROOT="$(mktemp -d "${OUTPUT_PARENT}/.feng-bundled-packages.XXXXXX")"
for project_relative in "${BUNDLED_PROJECTS[@]}"; do
  stage_project_inputs "${project_relative}"
  build_bundled_project "${project_relative}"
done

[[ "$(find "${STAGING_ROOT}" -mindepth 1 -maxdepth 1 -type f -name '*.fb' | wc -l | tr -d ' ')" == "${#BUNDLED_PROJECTS[@]}" ]] ||
  die "bundled package output count does not match the configured project set"
[[ -z "$(
  find "${STAGING_ROOT}" \
    -mindepth 1 -maxdepth 1 \
    \( ! -type f -o ! -name '*.fb' \) \
    -print -quit
)" ]] || die "bundled package staging contains an unexpected entry"

mv "${STAGING_ROOT}" "${OUTPUT_ROOT}"
STAGING_ROOT=""
echo "==> Created bundled packages in ${OUTPUT_ROOT}"
