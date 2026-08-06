#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN_ROOT="${PROJECT_ROOT}/toolchain"
BUILD_ROOT="${PROJECT_ROOT}/build/toolchain-prebuilt"
REPOSITORY=""
TAG=""
TAG_VERSION=""
WORK_ROOT=""
LOCK_CANDIDATE=""
QUERY_ERROR=""
CREATED_RELEASE=false
RELEASE_ID=""
SOURCE_COMMIT=""
SOURCE_EPOCH=""
LLVM_VERSION=""
MAX_ASSET_BYTES=2147483648
HOST_PLATFORMS=(
  "macos-arm64"
  "linux-x64-gnu"
  "linux-arm64-gnu"
)
SYSROOT_PLATFORMS=(
  "linux-x64-gnu"
  "linux-x64-musl"
  "linux-arm64-gnu"
  "linux-arm64-musl"
)
ARCHIVE_NAMES=(
  "feng-llvm-macos-arm64.tar.gz"
  "feng-llvm-linux-x64-gnu.tar.gz"
  "feng-llvm-linux-arm64-gnu.tar.gz"
  "feng-sysroot-linux-all.tar.gz"
)

# Print the supported prebuilt-toolchain publication invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/toolchain-prebuilt-publish.sh \
    --repository=<owner/repository> \
    --tag=<toolchain-prebuilt/version>
EOF
}

# Report one fatal publication error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Require one command resolved through PATH.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

# Remove invocation-owned local staging and an unpublished or unverified release.
cleanup() {
  local exit_status=$?

  if [[ ${exit_status} -ne 0 && "${CREATED_RELEASE}" == "true" ]]; then
    echo "==> Removing incomplete GitHub Release ${TAG}" >&2
    if [[ "${RELEASE_ID}" =~ ^[0-9]+$ ]]; then
      gh api --method DELETE \
        "repos/${REPOSITORY}/releases/${RELEASE_ID}" >/dev/null 2>&1 || true
    else
      gh release delete "${TAG}" \
        --repo "${REPOSITORY}" --yes >/dev/null 2>&1 || true
    fi
  fi
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
  return "${exit_status}"
}

# Parse named options and reject duplicate or positional arguments.
parse_args() {
  while [[ "$#" -gt 0 ]]; do
    case "$1" in
      --repository=*)
        [[ -z "${REPOSITORY}" ]] ||
          die "--repository may only be specified once"
        REPOSITORY="${1#--repository=}"
        ;;
      --tag=*)
        [[ -z "${TAG}" ]] || die "--tag may only be specified once"
        TAG="${1#--tag=}"
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

  [[ "${REPOSITORY}" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] ||
    die "invalid GitHub repository: ${REPOSITORY:-<empty>}"
  [[ "${TAG}" =~ ^toolchain-prebuilt/[0-9A-Za-z][0-9A-Za-z._-]*$ ]] ||
    die "invalid toolchain prebuilt tag: ${TAG:-<empty>}"
  TAG_VERSION="${TAG#toolchain-prebuilt/}"
}

# Require GNU userland features used to create and verify reproducible archives.
verify_commands() {
  local command_name

  for command_name in \
    awk basename cmp comm cp dirname file find git gh grep gzip jq mkdir \
    mktemp mv readlink realpath rm sed sha256sum sleep sort stat tar; do
    require_cmd "${command_name}"
  done
  [[ "$(tar --version 2>/dev/null)" == *"GNU tar"* ]] ||
    die "GNU tar is required"
  [[ "$(find --version 2>/dev/null)" == *"GNU findutils"* ]] ||
    die "GNU findutils is required"
  git lfs version >/dev/null 2>&1 || die "Git LFS is required"
}

# Verify that the requested tag resolves to the checked-out source commit.
verify_source_revision() {
  local tag_commit

  git -C "${PROJECT_ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
    die "project root is not a Git worktree: ${PROJECT_ROOT}"
  SOURCE_COMMIT="$(git -C "${PROJECT_ROOT}" rev-parse HEAD)"
  tag_commit="$(git -C "${PROJECT_ROOT}" rev-list -n 1 "refs/tags/${TAG}" 2>/dev/null || true)"
  [[ -n "${tag_commit}" ]] || die "tag is not present in the checkout: ${TAG}"
  [[ "${tag_commit}" == "${SOURCE_COMMIT}" ]] ||
    die "tag ${TAG} does not point to the checked-out commit"
  SOURCE_EPOCH="$(git -C "${PROJECT_ROOT}" show -s --format=%ct HEAD)"
  [[ "${SOURCE_EPOCH}" =~ ^[0-9]+$ ]] ||
    die "source commit has an invalid timestamp"
  git -C "${PROJECT_ROOT}" diff --quiet HEAD -- toolchain ||
    die "toolchain contains tracked changes outside the tagged commit"
}

# Verify every Git LFS toolchain input has been materialized in the worktree.
verify_lfs_materialization() {
  local lfs_state="${WORK_ROOT}/lfs-files.json"
  local tracked_files="${WORK_ROOT}/toolchain-files.tracked"
  local lfs_files="${WORK_ROOT}/toolchain-files.lfs"
  local non_lfs_files="${WORK_ROOT}/toolchain-files.non-lfs"
  local unexpected_lfs_files="${WORK_ROOT}/toolchain-files.unexpected-lfs"
  local file_path

  git -C "${PROJECT_ROOT}" lfs ls-files --json > "${lfs_state}"
  : > "${tracked_files}"
  while IFS= read -r -d '' file_path; do
    [[ ! "${file_path}" =~ [[:cntrl:]] ]] ||
      die "toolchain path contains a control character"
    [[ -L "${PROJECT_ROOT}/${file_path}" ]] ||
      printf '%s\n' "${file_path}" >> "${tracked_files}"
  done < <(git -C "${PROJECT_ROOT}" ls-files -z -- toolchain)
  LC_ALL=C sort "${tracked_files}" -o "${tracked_files}"
  jq -r '.files[] | select(.name | startswith("toolchain/")) | .name' \
    "${lfs_state}" | LC_ALL=C sort > "${lfs_files}"
  [[ -s "${lfs_files}" ]] || die "no Git LFS toolchain files were found"
  comm -23 "${tracked_files}" "${lfs_files}" > "${non_lfs_files}"
  comm -13 "${tracked_files}" "${lfs_files}" > "${unexpected_lfs_files}"
  [[ ! -s "${unexpected_lfs_files}" ]] ||
    die "Git LFS reported an untracked toolchain path"
  while IFS= read -r file_path; do
    [[ ! -s "${PROJECT_ROOT}/${file_path}" ]] ||
      die "non-empty tracked toolchain file is not managed by Git LFS: ${file_path}"
  done < "${non_lfs_files}"
  jq -e '
    all(.files[];
      if (.name | startswith("toolchain/")) then
        .checkout == true and .downloaded == true
      else
        true
      end
    )
  ' "${lfs_state}" >/dev/null ||
    die "toolchain contains an unmaterialized Git LFS pointer"
}

# Print the immediate child-directory names under one directory.
directory_set() {
  find "$1" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | LC_ALL=C sort
}

# Verify one directory contains exactly the expected platform directories.
verify_platform_set() {
  local root="$1"
  shift
  local expected="${WORK_ROOT}/platforms.expected"
  local actual="${WORK_ROOT}/platforms.actual"

  printf '%s\n' "$@" | LC_ALL=C sort > "${expected}"
  directory_set "${root}" > "${actual}"
  cmp -s "${expected}" "${actual}" ||
    die "unexpected platform directory set under ${root}"
}

# Verify one native LLVM executable has the expected object format and CPU.
verify_platform_binary() {
  local file_path="$1"
  local platform="$2"
  local format

  [[ -x "${file_path}" ]] ||
    die "required LLVM executable is missing or not executable: ${file_path}"
  format="$(file -b "${file_path}")"
  case "${platform}" in
    macos-arm64)
      [[ "${format}" == *"Mach-O 64-bit"* && "${format}" == *"arm64"* ]] ||
        die "unexpected ${platform} LLVM format: ${format}"
      ;;
    linux-x64-gnu)
      [[ "${format}" == *"ELF 64-bit"* && "${format}" == *"x86-64"* ]] ||
        die "unexpected ${platform} LLVM format: ${format}"
      ;;
    linux-arm64-gnu)
      [[ "${format}" == *"ELF 64-bit"* && "${format}" == *"ARM aarch64"* ]] ||
        die "unexpected ${platform} LLVM format: ${format}"
      ;;
    *)
      die "unsupported LLVM host platform: ${platform}"
      ;;
  esac
}

# Verify one LLVM tree contains its metadata and complete required tool set.
verify_llvm_tree() {
  local platform="$1"
  local llvm_root="${TOOLCHAIN_ROOT}/llvm/${platform}"
  local platform_value version_value tool
  local tools=(clang lld ld.lld llvm-ar llvm-ranlib lldb lldb-dap lldb-argdumper)

  [[ -d "${llvm_root}" ]] || die "LLVM tree not found: ${llvm_root}"
  [[ -f "${llvm_root}/README.md" ]] ||
    die "LLVM metadata not found: ${llvm_root}/README.md"
  version_value="$(sed -n 's/^Version: //p' "${llvm_root}/README.md")"
  platform_value="$(sed -n 's/^Platform: //p' "${llvm_root}/README.md")"
  [[ "${version_value}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    die "invalid LLVM version metadata for ${platform}"
  [[ "${platform_value}" == "${platform}" ]] ||
    die "LLVM platform metadata mismatch for ${platform}"
  if [[ -z "${LLVM_VERSION}" ]]; then
    LLVM_VERSION="${version_value}"
  else
    [[ "${version_value}" == "${LLVM_VERSION}" ]] ||
      die "LLVM versions differ between host platforms"
  fi

  case "${platform}" in
    macos-arm64) tools+=(debugserver) ;;
    linux-*) tools+=(lldb-server) ;;
  esac
  for tool in "${tools[@]}"; do
    [[ -x "${llvm_root}/bin/${tool}" ]] ||
      die "required LLVM tool is missing or not executable: ${llvm_root}/bin/${tool}"
  done
  verify_platform_binary "${llvm_root}/bin/clang" "${platform}"
}

# Verify one Linux sysroot contains its declared platform and required C inputs.
verify_sysroot_tree() {
  local platform="$1"
  local sysroot_root="${TOOLCHAIN_ROOT}/sysroot/${platform}"
  local include_root lib_root triple

  [[ -d "${sysroot_root}" ]] || die "sysroot not found: ${sysroot_root}"
  [[ -f "${sysroot_root}/README.md" ]] ||
    die "sysroot metadata not found: ${sysroot_root}/README.md"
  grep -qx "Target: ${platform}" "${sysroot_root}/README.md" ||
    die "sysroot platform metadata mismatch for ${platform}"
  case "${platform}" in
    linux-x64-gnu) triple="x86_64-linux-gnu" ;;
    linux-arm64-gnu) triple="aarch64-linux-gnu" ;;
    linux-x64-musl|linux-arm64-musl) triple="" ;;
    *) die "unsupported sysroot platform: ${platform}" ;;
  esac
  if [[ -n "${triple}" ]]; then
    include_root="${sysroot_root}/usr/${triple}/include"
    lib_root="${sysroot_root}/usr/${triple}/lib"
  else
    include_root="${sysroot_root}/usr/include"
    lib_root="${sysroot_root}/usr/lib"
  fi
  [[ -f "${include_root}/stdio.h" ]] ||
    die "sysroot C headers are incomplete for ${platform}"
  for required in libc.a libc.so crt1.o; do
    [[ -f "${lib_root}/${required}" ]] ||
      die "sysroot C library input is missing for ${platform}: ${required}"
  done
}

# Reject special files and links that escape the published toolchain root.
verify_tree_safety() {
  local unsafe_path link_path resolved

  unsafe_path="$(find "${TOOLCHAIN_ROOT}/llvm" "${TOOLCHAIN_ROOT}/sysroot" \
    ! -type d ! -type f ! -type l -print -quit)"
  [[ -z "${unsafe_path}" ]] || die "unsupported toolchain file type: ${unsafe_path}"
  while IFS= read -r -d '' link_path; do
    [[ "$(readlink "${link_path}")" != /* ]] ||
      die "absolute toolchain symlink is not allowed: ${link_path}"
    resolved="$(realpath -m "${link_path}")"
    [[ "${resolved}" == "${TOOLCHAIN_ROOT}"/* ]] ||
      die "toolchain symlink escapes its root: ${link_path}"
    [[ -e "${link_path}" ]] || die "dangling toolchain symlink: ${link_path}"
  done < <(find "${TOOLCHAIN_ROOT}/llvm" "${TOOLCHAIN_ROOT}/sysroot" -type l -print0)
}

# Verify the complete fixed toolchain layout before any archive is generated.
verify_toolchain() {
  local platform

  [[ -d "${TOOLCHAIN_ROOT}/llvm" ]] || die "toolchain LLVM root not found"
  [[ -d "${TOOLCHAIN_ROOT}/sysroot" ]] || die "toolchain sysroot root not found"
  verify_platform_set "${TOOLCHAIN_ROOT}/llvm" "${HOST_PLATFORMS[@]}"
  verify_platform_set "${TOOLCHAIN_ROOT}/sysroot" "${SYSROOT_PLATFORMS[@]}"
  for platform in "${HOST_PLATFORMS[@]}"; do
    verify_llvm_tree "${platform}"
  done
  for platform in "${SYSROOT_PLATFORMS[@]}"; do
    verify_sysroot_tree "${platform}"
  done
  verify_tree_safety
}

# Add one tracked path and all archive-visible parent directories to a NUL list.
add_path_and_parents() {
  local path="$1"
  local output="$2"
  local parent="${path}"

  [[ ! "${path}" =~ [[:cntrl:]] ]] ||
    die "toolchain path contains a control character"
  while [[ "${parent}" != "toolchain" && "${parent}" != "." ]]; do
    printf '%s\0' "${parent}" >> "${output}"
    parent="$(dirname "${parent}")"
  done
}

# Build the exact tracked path list for one archive without including ignored files.
build_archive_file_list() {
  local selector="$1"
  local output="$2"
  local unsorted="${output}.unsorted"
  local tracked_path count=0

  : > "${unsorted}"
  while IFS= read -r -d '' tracked_path; do
    [[ -e "${PROJECT_ROOT}/${tracked_path}" || -L "${PROJECT_ROOT}/${tracked_path}" ]] ||
      die "tracked toolchain path is missing: ${tracked_path}"
    add_path_and_parents "${tracked_path}" "${unsorted}"
    count=$((count + 1))
  done < <(git -C "${PROJECT_ROOT}" ls-files -z -- "${selector}")
  [[ ${count} -gt 0 ]] || die "no tracked files matched ${selector}"
  LC_ALL=C sort -zu "${unsorted}" > "${output}"
  rm -f "${unsorted}"
}

# Create one deterministic gzip-compressed GNU tar archive from tracked paths.
create_archive() {
  local selector="$1"
  local archive_path="$2"
  local file_list="${WORK_ROOT}/$(basename "${archive_path}").files"

  echo "==> Creating $(basename "${archive_path}")"
  build_archive_file_list "${selector}" "${file_list}"
  tar \
    --create \
    --format=gnu \
    --no-recursion \
    --null \
    --directory="${PROJECT_ROOT}" \
    --files-from="${file_list}" \
    --transform='s,^toolchain/,,' \
    --mtime="@${SOURCE_EPOCH}" \
    --owner=0 \
    --group=0 \
    --numeric-owner \
    --sort=name \
    --file=- |
    gzip -n > "${archive_path}"
}

# Verify archive members are safe and confined to one required top-level directory.
verify_archive_members() {
  local archive_path="$1"
  local expected_root="$2"
  local member verbose_line type
  local count=0

  gzip -t "${archive_path}" || die "invalid gzip archive: ${archive_path}"
  while IFS= read -r member; do
    [[ -n "${member}" ]] || continue
    [[ "${member}" != /* ]] || die "archive contains an absolute path: ${member}"
    [[ ! "${member}" =~ (^|/)\.\.(/|$) ]] ||
      die "archive contains a parent traversal: ${member}"
    [[ "${member}" == "${expected_root}" || "${member}" == "${expected_root}/"* ]] ||
      die "archive member is outside ${expected_root}/: ${member}"
    count=$((count + 1))
  done < <(tar -tzf "${archive_path}")
  [[ ${count} -gt 0 ]] || die "archive contains no members: ${archive_path}"

  while IFS= read -r verbose_line; do
    type="${verbose_line:0:1}"
    case "${type}" in
      -|d|l|h) ;;
      *) die "archive contains an unsupported member type: ${verbose_line}" ;;
    esac
  done < <(tar -tvzf "${archive_path}")
}

# Verify extracted links remain bounded and required executable permissions survive.
verify_extracted_archive() {
  local archive_path="$1"
  local expected_root="$2"
  local verify_root="${WORK_ROOT}/verify"
  local link_path resolved platform tool

  rm -rf "${verify_root}"
  mkdir -p "${verify_root}"
  tar -xzf "${archive_path}" -C "${verify_root}"
  while IFS= read -r -d '' link_path; do
    resolved="$(realpath -m "${link_path}")"
    [[ "${resolved}" == "${verify_root}"/* ]] ||
      die "extracted symlink escapes its archive: ${link_path}"
    [[ -e "${link_path}" ]] || die "archive contains a dangling symlink: ${link_path}"
  done < <(find "${verify_root}" -type l -print0)

  if [[ "${expected_root}" == "llvm" ]]; then
    platform="$(basename "${archive_path}" .tar.gz)"
    platform="${platform#feng-llvm-}"
    for tool in clang lld llvm-ar lldb lldb-dap; do
      [[ -x "${verify_root}/llvm/${platform}/bin/${tool}" ]] ||
        die "archive lost executable permission: ${platform}/bin/${tool}"
    done
  else
    for platform in "${SYSROOT_PLATFORMS[@]}"; do
      [[ -d "${verify_root}/sysroot/${platform}" ]] ||
        die "sysroot archive omitted platform: ${platform}"
    done
  fi
  rm -rf "${verify_root}"
}

# Return the SHA-256 digest of one file.
sha256_digest() {
  sha256sum "$1" | awk '{print $1}'
}

# Append one file's stable name, size and SHA-256 to a tab-separated manifest.
append_asset_metadata() {
  local file_path="$1"
  local metadata_path="$2"
  local name size digest

  name="$(basename "${file_path}")"
  size="$(stat -c '%s' "${file_path}")"
  [[ "${size}" -lt "${MAX_ASSET_BYTES}" ]] ||
    die "GitHub Release asset must be smaller than 2 GiB: ${name}"
  digest="$(sha256_digest "${file_path}")"
  printf '%s\t%s\t%s\n' "${name}" "${size}" "${digest}" >> "${metadata_path}"
}

# Convert tab-separated asset metadata to the canonical JSON asset array.
metadata_to_json() {
  local metadata_path="$1"
  local output_path="$2"

  jq -Rn '
    [inputs | split("\t") |
      {name: .[0], bytes: (.[1] | tonumber), sha256: .[2]}]
  ' < "${metadata_path}" > "${output_path}"
}

# Generate the release manifest, checksum list and version-lock candidate.
generate_metadata_assets() {
  local archive_metadata="${WORK_ROOT}/archives.tsv"
  local archive_json="${WORK_ROOT}/archives.json"
  local all_metadata="${WORK_ROOT}/all-assets.tsv"
  local all_json="${WORK_ROOT}/all-assets.json"
  local archive_path

  : > "${archive_metadata}"
  for archive_path in "${WORK_ROOT}"/*.tar.gz; do
    append_asset_metadata "${archive_path}" "${archive_metadata}"
  done
  LC_ALL=C sort "${archive_metadata}" -o "${archive_metadata}"
  metadata_to_json "${archive_metadata}" "${archive_json}"

  jq -S -n \
    --arg tag "${TAG}" \
    --arg sourceCommit "${SOURCE_COMMIT}" \
    --arg llvmVersion "${LLVM_VERSION}" \
    --argjson hostPlatforms "$(printf '%s\n' "${HOST_PLATFORMS[@]}" | jq -R . | jq -s .)" \
    --argjson sysrootPlatforms "$(printf '%s\n' "${SYSROOT_PLATFORMS[@]}" | jq -R . | jq -s .)" \
    --slurpfile assets "${archive_json}" \
    '{
      schemaVersion: 1,
      tag: $tag,
      sourceCommit: $sourceCommit,
      llvmVersion: $llvmVersion,
      hostPlatforms: $hostPlatforms,
      sysrootPlatforms: $sysrootPlatforms,
      assets: $assets[0]
    }' > "${WORK_ROOT}/manifest.json"

  cp "${archive_metadata}" "${WORK_ROOT}/SHA256SUMS.metadata"
  append_asset_metadata "${WORK_ROOT}/manifest.json" "${WORK_ROOT}/SHA256SUMS.metadata"
  awk -F '\t' '{print $3 "  " $1}' "${WORK_ROOT}/SHA256SUMS.metadata" |
    LC_ALL=C sort > "${WORK_ROOT}/SHA256SUMS"
  rm -f "${WORK_ROOT}/SHA256SUMS.metadata"

  cp "${archive_metadata}" "${all_metadata}"
  append_asset_metadata "${WORK_ROOT}/manifest.json" "${all_metadata}"
  append_asset_metadata "${WORK_ROOT}/SHA256SUMS" "${all_metadata}"
  LC_ALL=C sort "${all_metadata}" -o "${all_metadata}"
  metadata_to_json "${all_metadata}" "${all_json}"
  jq -S -n \
    --arg repository "${REPOSITORY}" \
    --arg tag "${TAG}" \
    --arg sourceCommit "${SOURCE_COMMIT}" \
    --slurpfile assets "${all_json}" \
    '{
      schemaVersion: 1,
      repository: $repository,
      tag: $tag,
      sourceCommit: $sourceCommit,
      assets: $assets[0]
    }' > "${WORK_ROOT}/toolchain-prebuilt.lock"
}

# Confirm immutable releases are enabled and the requested release does not exist.
verify_remote_preconditions() {
  local immutable_state
  local releases="${WORK_ROOT}/releases.json"
  local existing_count

  immutable_state="$(gh api "repos/${REPOSITORY}/immutable-releases" --jq '.enabled')" ||
    die "immutable releases are not enabled for ${REPOSITORY}"
  [[ "${immutable_state}" == "true" ]] ||
    die "immutable releases are not enabled for ${REPOSITORY}"

  gh api "repos/${REPOSITORY}/releases?per_page=100" --paginate --slurp \
    > "${releases}" 2> "${QUERY_ERROR}" || {
      cat "${QUERY_ERROR}" >&2
      die "failed to list GitHub Releases"
    }
  existing_count="$(jq --arg tag "${TAG}" \
    '[.[][] | select(.tag_name == $tag)] | length' "${releases}")"
  [[ "${existing_count}" == "0" ]] || die "GitHub Release already exists: ${TAG}"
}

# Resolve the unique draft Release created by this invocation.
resolve_draft_release_id() {
  local releases="${WORK_ROOT}/releases.json"
  local attempt

  for attempt in 1 2 3 4 5; do
    gh api "repos/${REPOSITORY}/releases?per_page=100" --paginate --slurp \
      > "${releases}"
    RELEASE_ID="$(jq -r --arg tag "${TAG}" '
      [.[][] | select(.tag_name == $tag and .draft == true)] |
      if length == 1 then .[0].id else empty end
    ' "${releases}")"
    if [[ "${RELEASE_ID}" =~ ^[0-9]+$ ]]; then
      return
    fi
    [[ ${attempt} -eq 5 ]] || sleep 2
  done
  die "could not resolve the unique draft GitHub Release: ${TAG}"
}

# Compare uploaded asset names, sizes, digests and states with local metadata.
verify_remote_assets() {
  local expected="${WORK_ROOT}/remote-assets.expected"
  local actual="${WORK_ROOT}/remote-assets.actual"
  local attempt

  awk -F '\t' '{print $1 "\t" $2 "\tsha256:" $3 "\tuploaded"}' \
    "${WORK_ROOT}/all-assets.tsv" | LC_ALL=C sort > "${expected}"
  for attempt in 1 2 3 4 5; do
    gh api "repos/${REPOSITORY}/releases/${RELEASE_ID}" \
      --jq '.assets[] | [.name, (.size | tostring), .digest, .state] | @tsv' |
      LC_ALL=C sort > "${actual}"
    if cmp -s "${expected}" "${actual}"; then
      return
    fi
    [[ ${attempt} -eq 5 ]] || sleep 2
  done
  die "uploaded GitHub Release assets do not match local metadata"
}

# Create, verify and publish one immutable GitHub Release.
publish_release() {
  local assets=()
  local release_state is_draft is_immutable is_prerelease
  local asset_name attempt

  verify_remote_preconditions
  gh release create "${TAG}" \
    --repo "${REPOSITORY}" \
    --verify-tag \
    --draft \
    --latest=false \
    --title "Feng toolchain prebuilt ${TAG_VERSION}" \
    --notes "Immutable Feng toolchain prebuilt generated from ${SOURCE_COMMIT}."
  CREATED_RELEASE=true
  resolve_draft_release_id

  for asset_name in "${ARCHIVE_NAMES[@]}" manifest.json SHA256SUMS; do
    assets+=("${WORK_ROOT}/${asset_name}")
  done
  gh release upload "${TAG}" "${assets[@]}" --repo "${REPOSITORY}"
  verify_remote_assets
  gh release edit "${TAG}" --repo "${REPOSITORY}" --draft=false --latest=false

  for attempt in 1 2 3 4 5; do
    release_state="$(gh api "repos/${REPOSITORY}/releases/${RELEASE_ID}" \
      --jq '[.draft, (.immutable // false), .prerelease] | @tsv')"
    read -r is_draft is_immutable is_prerelease <<< "${release_state}"
    [[ "${is_draft}" == "false" ]] ||
      die "GitHub Release remained a draft: ${TAG}"
    [[ "${is_prerelease}" == "false" ]] ||
      die "toolchain prebuilt was published as a prerelease: ${TAG}"
    if [[ "${is_immutable}" == "true" ]]; then
      CREATED_RELEASE=false
      return
    fi
    [[ ${attempt} -eq 5 ]] || sleep 2
  done
  die "published GitHub Release is not immutable: ${TAG}"
}

# Coordinate validation, deterministic packaging and immutable publication.
main() {
  local platform archive_path

  parse_args "$@"
  verify_commands
  mkdir -p "${BUILD_ROOT}"
  LOCK_CANDIDATE="${BUILD_ROOT}/toolchain-prebuilt-${TAG_VERSION}.lock"
  [[ ! -e "${LOCK_CANDIDATE}" ]] ||
    die "version lock candidate already exists: ${LOCK_CANDIDATE}"
  WORK_ROOT="$(mktemp -d "${BUILD_ROOT}/.publish.XXXXXX")"
  QUERY_ERROR="${WORK_ROOT}/release-query.error"
  trap cleanup EXIT

  verify_source_revision
  verify_lfs_materialization
  verify_toolchain
  verify_remote_preconditions

  for platform in "${HOST_PLATFORMS[@]}"; do
    archive_path="${WORK_ROOT}/feng-llvm-${platform}.tar.gz"
    create_archive "toolchain/llvm/${platform}" "${archive_path}"
    verify_archive_members "${archive_path}" llvm
    verify_extracted_archive "${archive_path}" llvm
  done
  archive_path="${WORK_ROOT}/feng-sysroot-linux-all.tar.gz"
  create_archive "toolchain/sysroot" "${archive_path}"
  verify_archive_members "${archive_path}" sysroot
  verify_extracted_archive "${archive_path}" sysroot

  generate_metadata_assets
  publish_release

  mv "${WORK_ROOT}/toolchain-prebuilt.lock" "${LOCK_CANDIDATE}"
  echo "==> Published immutable toolchain prebuilt ${TAG}"
  echo "==> Version lock candidate: ${LOCK_CANDIDATE}"
}

main "$@"
