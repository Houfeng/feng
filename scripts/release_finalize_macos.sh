#!/usr/bin/env bash
set -euo pipefail

ARCHIVE_PATH=""
OUTPUT_ARCHIVE=""
VERSION=""
SIGNING_IDENTITY=""
TEAM_ID=""
NOTARY_KEY_PATH=""
NOTARY_KEY_ID=""
NOTARY_ISSUER_ID=""
CODESIGN_TOOL="codesign"
FILE_TOOL="file"
NOTARYTOOL=""
WORK_ROOT=""
PACKAGE_ROOT=""
SIGNABLE_LIST=""

# Print the supported macOS release-finalization invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_finalize_macos.sh \
    --archive=<unsigned-macos-zip> \
    --output=<final-macos-zip> \
    --version=<version> \
    --identity=<developer-id-application-identity> \
    --team-id=<apple-team-id> \
    --notary-key=<app-store-connect-api-key-p8> \
    --notary-key-id=<api-key-id> \
    --notary-issuer-id=<api-issuer-id> \
    [--codesign-tool=<path>] \
    [--file-tool=<path>] \
    [--notarytool=<path>]

The default notary command is: xcrun notarytool
Tool overrides are intended for release-script regression tests.
EOF
}

# Report one fatal macOS release-finalization error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove only the finalization workspace created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
}

# Require one command resolved through PATH or supplied as an executable path.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

# Return success when one file is distributable Mach-O code that must be signed.
is_signable_macho() {
  local file_path="$1"
  local format

  format="$("${FILE_TOOL}" -b "${file_path}")"
  [[ "${format}" == *"Mach-O"* ]] || return 1
  case "${format}" in
    *"executable"*|*"dynamically linked shared library"*|*"bundle"*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

# Validate archive members before extraction.
validate_archive_members() {
  local listing_path="${WORK_ROOT}/archive-members"
  local duplicate_path="${WORK_ROOT}/duplicate-members"
  local expected_root="feng-${VERSION}-macos-arm64"
  local member
  local member_count=0

  unzip -tqq "${ARCHIVE_PATH}" >/dev/null ||
    die "invalid macOS release zip: ${ARCHIVE_PATH}"
  unzip -Z1 "${ARCHIVE_PATH}" > "${listing_path}" ||
    die "failed to list macOS release zip: ${ARCHIVE_PATH}"
  LC_ALL=C sort "${listing_path}" | uniq -d > "${duplicate_path}"
  [[ ! -s "${duplicate_path}" ]] ||
    die "macOS release zip contains duplicate members"
  while IFS= read -r member; do
    [[ -n "${member}" ]] || continue
    [[ "${member}" != /* ]] ||
      die "macOS release zip contains an absolute path: ${member}"
    case "/${member}/" in
      *"/../"*|*"/./"*)
        die "macOS release zip contains an unsafe path: ${member}"
        ;;
    esac
    case "${member}" in
      "${expected_root}"|"${expected_root}/"|"${expected_root}/"*)
        ;;
      *)
        die "macOS release zip contains an unexpected path: ${member}"
        ;;
    esac
    member_count=$((member_count + 1))
  done < "${listing_path}"
  [[ "${member_count}" -gt 0 ]] ||
    die "macOS release zip is empty: ${ARCHIVE_PATH}"
}

# Ensure extracted symbolic links remain inside the distribution root.
validate_extracted_links() {
  local link_path
  local link_target
  local resolved_path

  while IFS= read -r -d '' link_path; do
    link_target="$(readlink "${link_path}")"
    [[ "${link_target}" != /* ]] ||
      die "macOS release contains an absolute symbolic link: ${link_path}"
    resolved_path="$(realpath "${link_path}")" ||
      die "macOS release contains a broken symbolic link: ${link_path}"
    case "${resolved_path}" in
      "${PACKAGE_ROOT}/"*)
        ;;
      *)
        die "macOS release symbolic link escapes its package root: ${link_path}"
        ;;
    esac
  done < <(find "${PACKAGE_ROOT}" -type l -print0)
}

# Discover every Mach-O executable, dynamic library and bundle in the package.
discover_signable_code() {
  local file_path
  local relative_path

  : > "${SIGNABLE_LIST}"
  while IFS= read -r -d '' file_path; do
    if is_signable_macho "${file_path}"; then
      relative_path="${file_path#"${PACKAGE_ROOT}/"}"
      printf '%s\n' "${relative_path}" >> "${SIGNABLE_LIST}"
    fi
  done < <(find "${PACKAGE_ROOT}" -type f -print0)
  LC_ALL=C sort -o "${SIGNABLE_LIST}" "${SIGNABLE_LIST}"
  [[ -s "${SIGNABLE_LIST}" ]] ||
    die "macOS release contains no signable Mach-O code"
}

# Extract XML entitlements from one signed code object, or an empty file.
extract_entitlements() {
  local file_path="$1"
  local output_path="$2"

  : > "${output_path}"
  "${CODESIGN_TOOL}" \
    --display --entitlements - --xml "${file_path}" \
    > "${output_path}" 2>/dev/null || :
}

# Sign and strictly verify every discovered code object.
sign_and_verify_code() {
  local relative_path
  local file_path
  local metadata_path
  local before_metadata
  local before_entitlements
  local after_entitlements
  local before_identifier
  local after_identifier
  local had_signature
  local index=0
  local -a sign_args

  mkdir -p "${WORK_ROOT}/entitlements"
  while IFS= read -r relative_path; do
    [[ -n "${relative_path}" ]] || continue
    file_path="${PACKAGE_ROOT}/${relative_path}"
    before_metadata="${WORK_ROOT}/codesign-${index}.before.txt"
    before_entitlements="${WORK_ROOT}/entitlements/${index}.before.plist"
    after_entitlements="${WORK_ROOT}/entitlements/${index}.after.plist"
    extract_entitlements "${file_path}" "${before_entitlements}"
    had_signature=false
    before_identifier=""
    if "${CODESIGN_TOOL}" --display --verbose=4 "${file_path}" \
      > "${before_metadata}" 2>&1; then
      had_signature=true
      before_identifier="$(
        sed -n 's/^Identifier=//p' "${before_metadata}" |
          head -n 1
      )"
      [[ -n "${before_identifier}" ]] ||
        die "existing code signature has no identifier: ${relative_path}"
    fi

    sign_args=(
      --force
      --sign "${SIGNING_IDENTITY}"
      --options runtime
      --timestamp
    )
    if [[ "${had_signature}" == "true" ]]; then
      sign_args+=(--preserve-metadata=identifier,entitlements)
    fi
    "${CODESIGN_TOOL}" "${sign_args[@]}" "${file_path}" ||
      die "failed to sign macOS release code: ${relative_path}"

    "${CODESIGN_TOOL}" --verify --strict --verbose=2 "${file_path}" ||
      die "invalid Developer ID signature: ${relative_path}"
    metadata_path="${WORK_ROOT}/codesign-${index}.txt"
    "${CODESIGN_TOOL}" --display --verbose=4 "${file_path}" \
      > "${metadata_path}" 2>&1 ||
      die "failed to inspect Developer ID signature: ${relative_path}"
    grep -Fq "Authority=Developer ID Application:" "${metadata_path}" ||
      die "code is not signed by a Developer ID Application identity: ${relative_path}"
    grep -Fq \
      "Authority=Developer ID Certification Authority" \
      "${metadata_path}" ||
      die "code signature has no Developer ID intermediate CA: ${relative_path}"
    grep -Fq "Authority=Apple Root CA" "${metadata_path}" ||
      die "code signature has no Apple root CA: ${relative_path}"
    grep -Fqx "TeamIdentifier=${TEAM_ID}" "${metadata_path}" ||
      die "code signature has an unexpected Team ID: ${relative_path}"
    grep -Eq '^CodeDirectory .*flags=.*\(.*runtime.*\)' "${metadata_path}" ||
      die "code signature does not enable Hardened Runtime: ${relative_path}"
    grep -Fq "Timestamp=" "${metadata_path}" ||
      die "code signature has no secure timestamp: ${relative_path}"
    after_identifier="$(
      sed -n 's/^Identifier=//p' "${metadata_path}" |
        head -n 1
    )"
    [[ -n "${after_identifier}" ]] ||
      die "Developer ID signature has no identifier: ${relative_path}"
    if [[ "${had_signature}" == "true" &&
          "${after_identifier}" != "${before_identifier}" ]]; then
      die "code-signing identifier changed: ${relative_path}"
    fi

    extract_entitlements "${file_path}" "${after_entitlements}"
    cmp -s "${before_entitlements}" "${after_entitlements}" ||
      die "code-signing entitlements changed: ${relative_path}"
    index=$((index + 1))
  done < "${SIGNABLE_LIST}"
  echo "==> Signed and verified ${index} macOS code objects"
}

# Create the final zip without modifying its fixed top-level layout.
create_final_archive() {
  local package_name="feng-${VERSION}-macos-arm64"
  local staged_archive="${WORK_ROOT}/${package_name}.zip"

  (
    cd "${WORK_ROOT}/extracted"
    zip -q -X -y -r "${staged_archive}" "${package_name}"
  )
  unzip -tqq "${staged_archive}" >/dev/null ||
    die "failed to create valid signed macOS release zip"
}

# Submit the final zip and require an explicit Accepted result.
notarize_final_archive() {
  local package_name="feng-${VERSION}-macos-arm64"
  local staged_archive="${WORK_ROOT}/${package_name}.zip"
  local notary_output="${WORK_ROOT}/notary-result.json"
  local notary_status
  local notary_exit=0

  if [[ -n "${NOTARYTOOL}" ]]; then
    "${NOTARYTOOL}" submit "${staged_archive}" \
      --key "${NOTARY_KEY_PATH}" \
      --key-id "${NOTARY_KEY_ID}" \
      --issuer "${NOTARY_ISSUER_ID}" \
      --wait \
      --output-format json \
      > "${notary_output}" 2>&1 || notary_exit=$?
  else
    xcrun notarytool submit "${staged_archive}" \
      --key "${NOTARY_KEY_PATH}" \
      --key-id "${NOTARY_KEY_ID}" \
      --issuer "${NOTARY_ISSUER_ID}" \
      --wait \
      --output-format json \
      > "${notary_output}" 2>&1 || notary_exit=$?
  fi
  if [[ "${notary_exit}" -ne 0 ]]; then
    cat "${notary_output}" >&2
    die "Apple notarization command failed"
  fi
  notary_status="$(
    sed -n \
      's/.*"status"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
      "${notary_output}" |
      head -n 1
  )"
  [[ "${notary_status}" == "Accepted" ]] || {
    cat "${notary_output}" >&2
    die "Apple notarization did not return Accepted"
  }
  echo "==> Apple notarization accepted ${package_name}.zip"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --archive=*)
      [[ -z "${ARCHIVE_PATH}" ]] || die "--archive may only be specified once"
      ARCHIVE_PATH="${1#--archive=}"
      ;;
    --output=*)
      [[ -z "${OUTPUT_ARCHIVE}" ]] || die "--output may only be specified once"
      OUTPUT_ARCHIVE="${1#--output=}"
      ;;
    --version=*)
      [[ -z "${VERSION}" ]] || die "--version may only be specified once"
      VERSION="${1#--version=}"
      ;;
    --identity=*)
      [[ -z "${SIGNING_IDENTITY}" ]] || die "--identity may only be specified once"
      SIGNING_IDENTITY="${1#--identity=}"
      ;;
    --team-id=*)
      [[ -z "${TEAM_ID}" ]] || die "--team-id may only be specified once"
      TEAM_ID="${1#--team-id=}"
      ;;
    --notary-key=*)
      [[ -z "${NOTARY_KEY_PATH}" ]] || die "--notary-key may only be specified once"
      NOTARY_KEY_PATH="${1#--notary-key=}"
      ;;
    --notary-key-id=*)
      [[ -z "${NOTARY_KEY_ID}" ]] || die "--notary-key-id may only be specified once"
      NOTARY_KEY_ID="${1#--notary-key-id=}"
      ;;
    --notary-issuer-id=*)
      [[ -z "${NOTARY_ISSUER_ID}" ]] ||
        die "--notary-issuer-id may only be specified once"
      NOTARY_ISSUER_ID="${1#--notary-issuer-id=}"
      ;;
    --codesign-tool=*)
      [[ "${CODESIGN_TOOL}" == "codesign" ]] ||
        die "--codesign-tool may only be specified once"
      CODESIGN_TOOL="${1#--codesign-tool=}"
      ;;
    --file-tool=*)
      [[ "${FILE_TOOL}" == "file" ]] ||
        die "--file-tool may only be specified once"
      FILE_TOOL="${1#--file-tool=}"
      ;;
    --notarytool=*)
      [[ -z "${NOTARYTOOL}" ]] || die "--notarytool may only be specified once"
      NOTARYTOOL="${1#--notarytool=}"
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

[[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]] ||
  die "invalid release version: ${VERSION:-<empty>}"
[[ -n "${ARCHIVE_PATH}" ]] || die "--archive is required"
[[ -n "${OUTPUT_ARCHIVE}" ]] || die "--output is required"
[[ -n "${SIGNING_IDENTITY}" ]] || die "--identity is required"
[[ "${TEAM_ID}" =~ ^[A-Z0-9]{10}$ ]] ||
  die "invalid Apple Team ID: ${TEAM_ID:-<empty>}"
[[ -n "${NOTARY_KEY_PATH}" ]] || die "--notary-key is required"
[[ -n "${NOTARY_KEY_ID}" ]] || die "--notary-key-id is required"
[[ -n "${NOTARY_ISSUER_ID}" ]] || die "--notary-issuer-id is required"

require_cmd "${CODESIGN_TOOL}"
require_cmd "${FILE_TOOL}"
if [[ -n "${NOTARYTOOL}" ]]; then
  require_cmd "${NOTARYTOOL}"
else
  require_cmd xcrun
fi
require_cmd cmp
require_cmd find
require_cmd grep
require_cmd readlink
require_cmd realpath
require_cmd sed
require_cmd sort
require_cmd uniq
require_cmd unzip
require_cmd zip

[[ -f "${ARCHIVE_PATH}" ]] ||
  die "unsigned macOS release zip not found: ${ARCHIVE_PATH}"
[[ -f "${NOTARY_KEY_PATH}" ]] ||
  die "App Store Connect API key not found: ${NOTARY_KEY_PATH}"
ARCHIVE_PATH="$(cd "$(dirname "${ARCHIVE_PATH}")" && pwd)/$(basename "${ARCHIVE_PATH}")"
NOTARY_KEY_PATH="$(
  cd "$(dirname "${NOTARY_KEY_PATH}")" &&
  pwd
)/$(basename "${NOTARY_KEY_PATH}")"

EXPECTED_NAME="feng-${VERSION}-macos-arm64.zip"
[[ "$(basename "${ARCHIVE_PATH}")" == "${EXPECTED_NAME}" ]] ||
  die "unexpected unsigned macOS release filename: ${ARCHIVE_PATH}"
[[ "$(basename "${OUTPUT_ARCHIVE}")" == "${EXPECTED_NAME}" ]] ||
  die "unexpected final macOS release filename: ${OUTPUT_ARCHIVE}"
OUTPUT_DIR="$(dirname "${OUTPUT_ARCHIVE}")"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
OUTPUT_ARCHIVE="${OUTPUT_DIR}/${EXPECTED_NAME}"
[[ ! -e "${OUTPUT_ARCHIVE}" ]] ||
  die "refusing to overwrite final macOS release zip: ${OUTPUT_ARCHIVE}"

WORK_ROOT="$(mktemp -d "${OUTPUT_DIR}/.feng-macos-finalize.XXXXXX")"
trap cleanup EXIT
mkdir -p "${WORK_ROOT}/extracted"
SIGNABLE_LIST="${WORK_ROOT}/signable-code"

validate_archive_members
unzip -q "${ARCHIVE_PATH}" -d "${WORK_ROOT}/extracted"
PACKAGE_ROOT="${WORK_ROOT}/extracted/feng-${VERSION}-macos-arm64"
[[ -d "${PACKAGE_ROOT}" && ! -L "${PACKAGE_ROOT}" ]] ||
  die "macOS release package root is missing or unsafe"
validate_extracted_links
discover_signable_code
sign_and_verify_code
create_final_archive
notarize_final_archive

mv "${WORK_ROOT}/${EXPECTED_NAME}" "${OUTPUT_ARCHIVE}"
echo "==> Created signed and notarized macOS release ${OUTPUT_ARCHIVE}"
