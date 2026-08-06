#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN_ROOT="${PROJECT_ROOT}/toolchain"
BUILD_ROOT="${PROJECT_ROOT}/build"
REPOSITORY="${GITHUB_REPOSITORY:-${GH_REPO:-}}"
WORK_ROOT=""

# Report one fatal prebuilt restoration error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove only the staging directory created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" &&
        "${WORK_ROOT}" == "${BUILD_ROOT}"/toolchain-prebuilt-fetch.* &&
        -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
}

# Validate the GitHub Actions context and required host commands.
verify_inputs() {
  local command

  [[ "$#" -eq 0 ]] || die "this script does not accept arguments"
  [[ "${REPOSITORY}" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] ||
    die "invalid GitHub repository: ${REPOSITORY:-<empty>}"
  [[ -n "${GH_TOKEN:-}" ]] || die "GH_TOKEN is required"
  for command in gh tar find readlink mv; do
    command -v "${command}" >/dev/null 2>&1 ||
      die "missing required command: ${command}"
  done
}

# Resolve the most recently published Release under the prebuilt tag prefix.
resolve_latest_tag() {
  local tags
  local tag
  local published_at
  local latest_tag=""
  local latest_published_at=""

  tags="$(
    gh api \
      "repos/${REPOSITORY}/git/matching-refs/tags/toolchain-prebuilt/" \
      --jq '.[] | .ref | select(startswith("refs/tags/toolchain-prebuilt/")) | ltrimstr("refs/tags/")'
  )" || die "failed to list toolchain prebuilt tags in ${REPOSITORY}"
  while IFS= read -r tag; do
    [[ -n "${tag}" ]] || continue
    [[ "${tag}" =~ ^toolchain-prebuilt/[0-9A-Za-z][0-9A-Za-z._-]*$ ]] ||
      die "invalid toolchain prebuilt tag returned by GitHub: ${tag}"
    published_at="$(
      gh release view "${tag}" \
        --repo "${REPOSITORY}" \
        --json=publishedAt \
        --jq=.publishedAt
    )" || die "failed to query toolchain prebuilt Release: ${tag}"
    if [[ -z "${latest_published_at}" ||
          "${published_at}" > "${latest_published_at}" ]]; then
      latest_published_at="${published_at}"
      latest_tag="${tag}"
    fi
  done <<< "${tags}"
  [[ -n "${latest_tag}" ]] ||
    die "no toolchain prebuilt tag found in ${REPOSITORY}"
  printf '%s\n' "${latest_tag}"
}

# Download the unique asset derived from the selected prebuilt tag.
download_archive() {
  local tag="$1"
  local version="${tag#toolchain-prebuilt/}"
  local asset="toolchain-prebuilt-${version}.tar.gz"
  local archive="${WORK_ROOT}/${asset}"

  echo "==> Downloading ${asset} from ${tag}" >&2
  gh release download "${tag}" \
    --repo "${REPOSITORY}" \
    --pattern "${asset}" \
    --dir "${WORK_ROOT}"
  [[ -f "${archive}" ]] ||
    die "toolchain prebuilt asset not found: ${asset}"
  printf '%s\n' "${archive}"
}

# Reject archive member paths outside the single toolchain top-level directory.
verify_archive_paths() {
  local archive="$1"
  local listing="${WORK_ROOT}/archive-members.txt"
  local member
  local component
  local member_count=0

  tar -tzf "${archive}" > "${listing}" ||
    die "invalid toolchain prebuilt archive: $(basename "${archive}")"
  while IFS= read -r member; do
    [[ -n "${member}" ]] || die "archive contains an empty member path"
    [[ "${member}" != /* ]] ||
      die "archive contains an absolute member path: ${member}"
    [[ "${member}" == "toolchain" || "${member}" == toolchain/* ]] ||
      die "archive contains an unexpected top-level path: ${member}"
    IFS='/' read -r -a components <<< "${member}"
    for component in "${components[@]}"; do
      [[ "${component}" != ".." ]] ||
        die "archive contains a parent path component: ${member}"
    done
    member_count=$((member_count + 1))
  done < "${listing}"
  [[ "${member_count}" -gt 0 ]] || die "toolchain prebuilt archive is empty"
}

# Return success when one relative symbolic-link target stays inside toolchain.
link_target_is_internal() {
  local base="$1"
  local target="$2"
  local part
  local depth=0
  local parts=()

  [[ "${target}" != /* ]] || return 1
  IFS='/' read -r -a parts <<< "${base}/${target}"
  for part in "${parts[@]}"; do
    case "${part}" in
      ""|.) ;;
      ..)
        [[ "${depth}" -gt 0 ]] || return 1
        depth=$((depth - 1))
        ;;
      *) depth=$((depth + 1)) ;;
    esac
  done
}

# Validate extracted file types, symbolic links, host LLVM and Linux sysroots.
verify_toolchain_layout() {
  local root="$1"
  local unexpected
  local link
  local relative
  local base
  local target
  local platform

  [[ -d "${root}" ]] || die "archive does not contain a toolchain directory"
  unexpected="$(find "${root}" ! -type d ! -type f ! -type l -print -quit)"
  [[ -z "${unexpected}" ]] ||
    die "toolchain contains an unsupported file type: ${unexpected#${PROJECT_ROOT}/}"

  while IFS= read -r -d '' link; do
    relative="${link#${root}/}"
    base="$(dirname "${relative}")"
    target="$(readlink "${link}")"
    link_target_is_internal "${base}" "${target}" ||
      die "toolchain symbolic link escapes its root: ${relative} -> ${target}"
    [[ -e "${link}" ]] ||
      die "toolchain contains a dangling symbolic link: ${relative} -> ${target}"
  done < <(find "${root}" -type l -print0)

  for platform in macos-arm64 linux-x64-gnu linux-arm64-gnu; do
    [[ -x "${root}/llvm/${platform}/bin/clang" ]] ||
      die "toolchain is missing executable LLVM clang for ${platform}"
    [[ -x "${root}/llvm/${platform}/bin/llvm-ar" ]] ||
      die "toolchain is missing executable LLVM llvm-ar for ${platform}"
  done
  for platform in \
    linux-x64-gnu linux-x64-musl linux-arm64-gnu linux-arm64-musl; do
    [[ -d "${root}/sysroot/${platform}/usr/include" ]] ||
      die "toolchain is missing sysroot headers for ${platform}"
    [[ -d "${root}/sysroot/${platform}/usr/lib" ]] ||
      die "toolchain is missing sysroot libraries for ${platform}"
  done
}

# Replace the checkout's LFS pointer tree with the validated complete toolchain.
install_toolchain() {
  local source="$1"
  local backup="${WORK_ROOT}/checkout-toolchain"
  local had_original=false

  if [[ -e "${TOOLCHAIN_ROOT}" || -L "${TOOLCHAIN_ROOT}" ]]; then
    mv "${TOOLCHAIN_ROOT}" "${backup}"
    had_original=true
  fi
  if ! mv "${source}" "${TOOLCHAIN_ROOT}"; then
    if [[ "${had_original}" == true ]]; then
      mv "${backup}" "${TOOLCHAIN_ROOT}" ||
        die "failed to install prebuilt toolchain and restore checkout toolchain"
    fi
    die "failed to install prebuilt toolchain"
  fi
  if [[ "${had_original}" == true ]]; then
    rm -rf "${backup}"
  fi
}

# Resolve, download, validate and install the latest toolchain prebuilt.
main() {
  local tag
  local archive
  local extracted

  verify_inputs "$@"
  mkdir -p "${BUILD_ROOT}"
  WORK_ROOT="$(mktemp -d "${BUILD_ROOT}/toolchain-prebuilt-fetch.XXXXXX")"
  trap cleanup EXIT

  tag="$(resolve_latest_tag)"
  archive="$(download_archive "${tag}")"
  verify_archive_paths "${archive}"
  extracted="${WORK_ROOT}/extracted"
  mkdir -p "${extracted}"
  tar -xzf "${archive}" --directory="${extracted}"
  verify_toolchain_layout "${extracted}/toolchain"
  install_toolchain "${extracted}/toolchain"
  echo "==> Restored ${tag}"
}

main "$@"
