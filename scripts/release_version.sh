#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VERSION_FILE="${PROJECT_ROOT}/VERSION"
OUTPUT_FILE=""
EVENT_NAME=""
REF_TYPE=""
REF_NAME=""
VERSION=""
COMMAND=""
TEMP_FILE=""
VERSION_FILE_SET=false

# Print the supported release-version invocations.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_version.sh resolve \
    --event-name=<event-name> \
    --ref-type=<ref-type> \
    --ref-name=<ref-name> \
    [--version-file=<path>] \
    [--output=<github-output-path>]

  scripts/release_version.sh set \
    --version=<version> \
    [--version-file=<path>]
EOF
}

# Report one fatal release-version error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove only the temporary version file created by this invocation.
cleanup() {
  if [[ -n "${TEMP_FILE}" && -f "${TEMP_FILE}" ]]; then
    rm -f "${TEMP_FILE}"
  fi
}

# Return success when one value is a supported Feng release version.
is_valid_version() {
  [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]]
}

# Emit resolved values either to stdout or to a GitHub Actions output file.
emit_resolution() {
  local tag="$1"
  local is_release="$2"

  if [[ -n "${OUTPUT_FILE}" ]]; then
    {
      printf 'version=%s\n' "${VERSION}"
      printf 'tag=%s\n' "${tag}"
      printf 'release=%s\n' "${is_release}"
    } >> "${OUTPUT_FILE}"
  else
    printf 'version=%s\n' "${VERSION}"
    printf 'tag=%s\n' "${tag}"
    printf 'release=%s\n' "${is_release}"
  fi
}

[[ "$#" -gt 0 ]] || {
  usage >&2
  exit 1
}
COMMAND="$1"
shift

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --event-name=*)
      [[ -z "${EVENT_NAME}" ]] || die "--event-name may only be specified once"
      EVENT_NAME="${1#--event-name=}"
      ;;
    --ref-type=*)
      [[ -z "${REF_TYPE}" ]] || die "--ref-type may only be specified once"
      REF_TYPE="${1#--ref-type=}"
      ;;
    --ref-name=*)
      [[ -z "${REF_NAME}" ]] || die "--ref-name may only be specified once"
      REF_NAME="${1#--ref-name=}"
      ;;
    --version=*)
      [[ -z "${VERSION}" ]] || die "--version may only be specified once"
      VERSION="${1#--version=}"
      ;;
    --version-file=*)
      [[ "${VERSION_FILE_SET}" == "false" ]] ||
        die "--version-file may only be specified once"
      VERSION_FILE="${1#--version-file=}"
      VERSION_FILE_SET=true
      ;;
    --output=*)
      [[ -z "${OUTPUT_FILE}" ]] || die "--output may only be specified once"
      OUTPUT_FILE="${1#--output=}"
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

trap cleanup EXIT

case "${COMMAND}" in
  resolve)
    [[ -n "${EVENT_NAME}" ]] || die "--event-name is required for resolve"
    [[ -n "${REF_TYPE}" ]] || die "--ref-type is required for resolve"
    if [[ "${EVENT_NAME}" == "create" ]]; then
      [[ "${REF_TYPE}" == "tag" ]] ||
        die "create event does not reference a tag: ${REF_TYPE}"
      [[ "${REF_NAME}" == v* ]] ||
        die "release tag must start with v: ${REF_NAME:-<empty>}"
      VERSION="${REF_NAME#v}"
      is_valid_version "${VERSION}" ||
        die "invalid release tag: ${REF_NAME}"
      emit_resolution "${REF_NAME}" "true"
    else
      [[ -f "${VERSION_FILE}" ]] ||
        die "version file not found: ${VERSION_FILE}"
      VERSION="$(sed -n '1p' "${VERSION_FILE}")"
      is_valid_version "${VERSION}" ||
        die "invalid build version: ${VERSION:-<empty>}"
      emit_resolution "" "false"
    fi
    ;;
  set)
    [[ -z "${EVENT_NAME}" && -z "${REF_TYPE}" && -z "${REF_NAME}" ]] ||
      die "event arguments are not supported by set"
    [[ -z "${OUTPUT_FILE}" ]] || die "--output is not supported by set"
    is_valid_version "${VERSION}" ||
      die "invalid build version: ${VERSION:-<empty>}"
    VERSION_DIR="$(dirname "${VERSION_FILE}")"
    [[ -d "${VERSION_DIR}" ]] ||
      die "version directory not found: ${VERSION_DIR}"
    TEMP_FILE="$(mktemp "${VERSION_DIR}/.VERSION.XXXXXX")"
    printf '%s\n' "${VERSION}" > "${TEMP_FILE}"
    mv "${TEMP_FILE}" "${VERSION_FILE}"
    TEMP_FILE=""
    ;;
  *)
    die "unknown command: ${COMMAND}"
    ;;
esac
