#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK_ROOT=""

# Report one fatal macOS release-finalization regression failure.
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

# Create a codesign replacement that records signing and exposes fixed metadata.
create_mock_codesign() {
  local mock_path="$1"

  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'joined=" $* "'
    printf '%s\n' 'file_path="${!#}"'
    printf '%s\n' 'if [[ "${joined}" == *" --verify "* ]]; then'
    printf '%s\n' '  printf "verify|%s\\n" "$*" >> "${MOCK_CODESIGN_LOG}"'
    printf '%s\n' '  exit 0'
    printf '%s\n' 'fi'
    printf '%s\n' 'if [[ "${joined}" == *" --display "* ]]; then'
    printf '%s\n' '  if [[ "${joined}" == *" --entitlements "* ]]; then'
    printf '%s\n' '    if [[ "$(basename "${file_path}")" == "debugserver" ]]; then'
    printf '%s\n' '      printf "%s" "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>com.apple.security.cs.debugger</key><true/></dict></plist>"'
    printf '%s\n' '    fi'
    printf '%s\n' '    exit 0'
    printf '%s\n' '  fi'
    printf '%s\n' '  if [[ "${joined}" == *" --verbose=4 "* ]]; then'
    printf '%s\n' '    printf "%s\\n" \'
    printf '%s\n' '      "Identifier=$(basename "${file_path}")" \'
    printf '%s\n' '      "CodeDirectory v=20500 flags=0x10000(runtime)" \'
    printf '%s\n' '      "Authority=Developer ID Application: Release Test (ABCDEFGHIJ)" \'
    printf '%s\n' '      "Authority=Developer ID Certification Authority" \'
    printf '%s\n' '      "Authority=Apple Root CA" \'
    printf '%s\n' '      "Timestamp=Jul 31, 2026 at 12:00:00 AM" \'
    printf '%s\n' '      "TeamIdentifier=ABCDEFGHIJ"'
    printf '%s\n' '  fi'
    printf '%s\n' '  exit 0'
    printf '%s\n' 'fi'
    printf '%s\n' 'printf "sign|%s\\n" "$*" >> "${MOCK_CODESIGN_LOG}"'
  } > "${mock_path}"
  chmod 0755 "${mock_path}"
}

# Create a file replacement that exposes deterministic Mach-O fixture formats.
create_mock_file() {
  local mock_path="$1"

  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'file_path="${!#}"'
    printf '%s\n' 'case "$(basename "${file_path}")" in'
    printf '%s\n' '  feng|debugserver)'
    printf '%s\n' '    echo "Mach-O 64-bit executable arm64"'
    printf '%s\n' '    ;;'
    printf '%s\n' '  librelease.dylib)'
    printf '%s\n' '    echo "Mach-O 64-bit dynamically linked shared library arm64"'
    printf '%s\n' '    ;;'
    printf '%s\n' '  release.bundle)'
    printf '%s\n' '    echo "Mach-O 64-bit bundle arm64"'
    printf '%s\n' '    ;;'
    printf '%s\n' '  libclang_rt.osx.a)'
    printf '%s\n' '    echo "Mach-O universal binary with 1 architecture: [arm64:current ar archive]"'
    printf '%s\n' '    ;;'
    printf '%s\n' '  *)'
    printf '%s\n' '    echo "ASCII text"'
    printf '%s\n' '    ;;'
    printf '%s\n' 'esac'
  } > "${mock_path}"
  chmod 0755 "${mock_path}"
}

# Create a notarytool replacement with a selectable terminal status.
create_mock_notarytool() {
  local mock_path="$1"

  {
    printf '%s\n' '#!/usr/bin/env bash'
    printf '%s\n' 'set -euo pipefail'
    printf '%s\n' 'printf "%s\\n" "$*" >> "${MOCK_NOTARY_LOG}"'
    printf '%s\n' 'printf "{\"id\":\"release-test\",\"status\":\"%s\"}\\n" "${MOCK_NOTARY_STATUS:-Accepted}"'
  } > "${mock_path}"
  chmod 0755 "${mock_path}"
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
  ' "${PROJECT_ROOT}/.github/workflows/release.yml" > "${output_path}"
  [[ -s "${output_path}" ]] ||
    die "workflow job not found: ${job_name}"
}

trap cleanup EXIT
mkdir -p "${PROJECT_ROOT}/build"
WORK_ROOT="$(
  mktemp -d "${PROJECT_ROOT}/build/release-finalize-macos-test.XXXXXX"
)"

MOCK_BIN="${WORK_ROOT}/mock-bin"
MOCK_CODESIGN_LOG="${WORK_ROOT}/codesign.log"
MOCK_NOTARY_LOG="${WORK_ROOT}/notary.log"
mkdir -p "${MOCK_BIN}"
create_mock_codesign "${MOCK_BIN}/codesign"
create_mock_file "${MOCK_BIN}/file"
create_mock_notarytool "${MOCK_BIN}/notarytool"

PACKAGE_NAME="feng-0.1.0-macos-arm64"
FIXTURE_PARENT="${WORK_ROOT}/fixture"
FIXTURE_ROOT="${FIXTURE_PARENT}/${PACKAGE_NAME}"
UNSIGNED_ARCHIVE="${WORK_ROOT}/${PACKAGE_NAME}.zip"
FINAL_ARCHIVE="${WORK_ROOT}/finalized/${PACKAGE_NAME}.zip"
NOTARY_KEY="${WORK_ROOT}/AuthKey_RELEASE.p8"
mkdir -p \
  "${FIXTURE_ROOT}/bin" \
  "${FIXTURE_ROOT}/toolchain/llvm/bin" \
  "${FIXTURE_ROOT}/toolchain/llvm/lib/clang/22/lib/darwin" \
  "${FIXTURE_ROOT}/toolchain/llvm/lib/plugins"
printf '%s\n' "Mach-O executable fixture" > "${FIXTURE_ROOT}/bin/feng"
printf '%s\n' "Mach-O debugger fixture" \
  > "${FIXTURE_ROOT}/toolchain/llvm/bin/debugserver"
printf '%s\n' "Mach-O dylib fixture" \
  > "${FIXTURE_ROOT}/toolchain/llvm/lib/librelease.dylib"
printf '%s\n' "Mach-O bundle fixture" \
  > "${FIXTURE_ROOT}/toolchain/llvm/lib/plugins/release.bundle"
printf '%s\n' "Mach-O static archive fixture" \
  > "${FIXTURE_ROOT}/toolchain/llvm/lib/clang/22/lib/darwin/libclang_rt.osx.a"
ln -s "../../../bin/feng" "${FIXTURE_ROOT}/toolchain/llvm/bin/clang-link"
printf '%s\n' "0.1.0" > "${FIXTURE_ROOT}/VERSION"
(
  cd "${FIXTURE_PARENT}"
  zip -q -X -y -r "${UNSIGNED_ARCHIVE}" "${PACKAGE_NAME}"
)
printf '%s\n' "release notary fixture" > "${NOTARY_KEY}"
: > "${MOCK_CODESIGN_LOG}"
: > "${MOCK_NOTARY_LOG}"

MOCK_CODESIGN_LOG="${MOCK_CODESIGN_LOG}" \
MOCK_NOTARY_LOG="${MOCK_NOTARY_LOG}" \
MOCK_NOTARY_STATUS=Accepted \
  "${SCRIPT_DIR}/release_finalize_macos.sh" \
    --archive="${UNSIGNED_ARCHIVE}" \
    --output="${FINAL_ARCHIVE}" \
    --version=0.1.0 \
    --identity="Developer ID Application: Release Test (ABCDEFGHIJ)" \
    --team-id=ABCDEFGHIJ \
    --notary-key="${NOTARY_KEY}" \
    --notary-key-id=RELEASEKEY \
    --notary-issuer-id=00000000-0000-0000-0000-000000000000 \
    --codesign-tool="${MOCK_BIN}/codesign" \
    --file-tool="${MOCK_BIN}/file" \
    --notarytool="${MOCK_BIN}/notarytool" >/dev/null
[[ -f "${FINAL_ARCHIVE}" ]] ||
  die "macOS finalization did not publish its accepted archive"
unzip -tqq "${FINAL_ARCHIVE}" >/dev/null ||
  die "macOS finalization published an invalid zip"
[[ "$(grep -c '^sign|' "${MOCK_CODESIGN_LOG}")" == "4" ]] ||
  die "macOS finalization did not sign every distributable Mach-O file"
[[ "$(grep -Fc -- '--preserve-metadata=identifier,entitlements' "${MOCK_CODESIGN_LOG}")" == "4" ]] ||
  die "macOS finalization did not preserve existing signing metadata"
grep -Fq 'submit ' "${MOCK_NOTARY_LOG}" ||
  die "macOS finalization did not submit the final zip for notarization"

FINAL_LISTING="${WORK_ROOT}/final.list"
unzip -Z1 "${FINAL_ARCHIVE}" > "${FINAL_LISTING}"
grep -Fqx \
  "${PACKAGE_NAME}/toolchain/llvm/bin/clang-link" \
  "${FINAL_LISTING}" ||
  die "macOS finalization did not preserve package symbolic links"
grep -Fqx \
  "${PACKAGE_NAME}/toolchain/llvm/lib/clang/22/lib/darwin/libclang_rt.osx.a" \
  "${FINAL_LISTING}" ||
  die "macOS finalization omitted the unsigned static runtime archive"

REJECTED_ARCHIVE="${WORK_ROOT}/rejected/${PACKAGE_NAME}.zip"
if MOCK_CODESIGN_LOG="${MOCK_CODESIGN_LOG}" \
   MOCK_NOTARY_LOG="${MOCK_NOTARY_LOG}" \
   MOCK_NOTARY_STATUS=Invalid \
     "${SCRIPT_DIR}/release_finalize_macos.sh" \
       --archive="${UNSIGNED_ARCHIVE}" \
       --output="${REJECTED_ARCHIVE}" \
       --version=0.1.0 \
       --identity="Developer ID Application: Release Test (ABCDEFGHIJ)" \
       --team-id=ABCDEFGHIJ \
       --notary-key="${NOTARY_KEY}" \
       --notary-key-id=RELEASEKEY \
       --notary-issuer-id=00000000-0000-0000-0000-000000000000 \
       --codesign-tool="${MOCK_BIN}/codesign" \
       --file-tool="${MOCK_BIN}/file" \
       --notarytool="${MOCK_BIN}/notarytool" >/dev/null 2>&1; then
  die "macOS finalization accepted a rejected notarization"
fi
[[ ! -e "${REJECTED_ARCHIVE}" ]] ||
  die "rejected notarization left a final macOS archive"

UNSAFE_PARENT="${WORK_ROOT}/unsafe-fixture"
UNSAFE_ARCHIVE="${WORK_ROOT}/unsafe-input/${PACKAGE_NAME}.zip"
UNSAFE_OUTPUT="${WORK_ROOT}/unsafe-output/${PACKAGE_NAME}.zip"
mkdir -p "${UNSAFE_PARENT}" "$(dirname "${UNSAFE_ARCHIVE}")"
cp -R "${FIXTURE_ROOT}" "${UNSAFE_PARENT}/${PACKAGE_NAME}"
printf '%s\n' "unexpected" > "${UNSAFE_PARENT}/unexpected.txt"
(
  cd "${UNSAFE_PARENT}"
  zip -q -X -y -r \
    "${UNSAFE_ARCHIVE}" \
    "${PACKAGE_NAME}" \
    unexpected.txt
)
if MOCK_CODESIGN_LOG="${MOCK_CODESIGN_LOG}" \
   MOCK_NOTARY_LOG="${MOCK_NOTARY_LOG}" \
   MOCK_NOTARY_STATUS=Accepted \
     "${SCRIPT_DIR}/release_finalize_macos.sh" \
       --archive="${UNSAFE_ARCHIVE}" \
       --output="${UNSAFE_OUTPUT}" \
       --version=0.1.0 \
       --identity="Developer ID Application: Release Test (ABCDEFGHIJ)" \
       --team-id=ABCDEFGHIJ \
       --notary-key="${NOTARY_KEY}" \
       --notary-key-id=RELEASEKEY \
       --notary-issuer-id=00000000-0000-0000-0000-000000000000 \
       --codesign-tool="${MOCK_BIN}/codesign" \
       --file-tool="${MOCK_BIN}/file" \
       --notarytool="${MOCK_BIN}/notarytool" >/dev/null 2>&1; then
  die "macOS finalization accepted an unexpected archive root"
fi
[[ ! -e "${UNSAFE_OUTPUT}" ]] ||
  die "unsafe macOS archive validation left a final output"
[[ -z "$(find "${WORK_ROOT}" -type d -name '.feng-macos-finalize.*' -print -quit)" ]] ||
  die "macOS finalization left a staging directory"

ASSEMBLE_JOB="${WORK_ROOT}/assemble.job"
FINALIZE_JOB="${WORK_ROOT}/finalize-macos.job"
VERIFY_MACOS_JOB="${WORK_ROOT}/verify-macos.job"
VERIFY_LINUX_JOB="${WORK_ROOT}/verify-linux.job"
extract_workflow_job "assemble" "${ASSEMBLE_JOB}"
extract_workflow_job "finalize_macos" "${FINALIZE_JOB}"
extract_workflow_job "verify_macos" "${VERIFY_MACOS_JOB}"
extract_workflow_job "verify_linux" "${VERIFY_LINUX_JOB}"
grep -Fq "name: release-packages-unsigned" "${ASSEMBLE_JOB}" ||
  die "assemble does not publish the internal unsigned artifact"
grep -Fq "      - assemble" "${FINALIZE_JOB}" ||
  die "finalize_macos does not depend on assemble"
grep -Fq "    environment: release-signing" "${FINALIZE_JOB}" ||
  die "finalize_macos is not protected by the release-signing environment"
grep -Fq "scripts/release_finalize_macos.sh" "${FINALIZE_JOB}" ||
  die "finalize_macos does not invoke the standalone finalization script"
grep -Fq "unzip -tqq" "${FINALIZE_JOB}" ||
  die "finalize_macos does not validate every unsigned release archive"
grep -Fq "name: release-packages" "${FINALIZE_JOB}" ||
  die "finalize_macos does not publish the final release artifact"
[[ "$(grep -Fc "scripts/release_finalize_macos.sh" "${PROJECT_ROOT}/.github/workflows/release.yml")" == "1" ]] ||
  die "workflow must invoke macOS finalization exactly once"
grep -Fq "      - finalize_macos" "${VERIFY_MACOS_JOB}" ||
  die "verify_macos does not depend on finalized release packages"
grep -Fq "      - finalize_macos" "${VERIFY_LINUX_JOB}" ||
  die "verify_linux does not depend on finalized release packages"

echo "release macOS finalization: signing, notarization, safety, and workflow passed"
