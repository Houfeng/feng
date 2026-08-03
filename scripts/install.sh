#!/usr/bin/env bash
set -euo pipefail

REPOSITORY="Houfeng/feng"
INSTALL_ROOT="${HOME}/.feng"
INSTALL_PARENT="${HOME}"
PATH_ENTRY='export PATH="$HOME/.feng/bin:$PATH"'
TEMP_ROOT=""
STAGING_ROOT=""
BACKUP_ROOT=""
SHELL_FILE=""
SHELL_FILE_TEMP=""
SHELL_FILE_BACKUP=""
INSTALL_COMMITTED=0
INSTALL_BACKED_UP=0
SHELL_FILE_COMMITTED=0
REQUESTED_TAG=""
REQUESTED_CHANNEL=""

# Report one fatal installation error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Require one command resolved through PATH.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

# Parse the optional explicit GitHub Release tag or release channel.
parse_arguments() {
  while [[ "$#" -gt 0 ]]; do
    case "$1" in
      --version=*)
        [[ -z "${REQUESTED_TAG}" ]] ||
          die "--version may only be specified once"
        REQUESTED_TAG="${1#--version=}"
        [[ -n "${REQUESTED_TAG}" ]] ||
          die "--version requires a release tag"
        ;;
      --channel=*)
        [[ -z "${REQUESTED_CHANNEL}" ]] ||
          die "--channel may only be specified once"
        REQUESTED_CHANNEL="${1#--channel=}"
        [[ -n "${REQUESTED_CHANNEL}" ]] ||
          die "--channel requires a channel name"
        ;;
      *)
        die "unsupported argument: $1"
        ;;
    esac
    shift
  done

  if [[ -n "${REQUESTED_TAG}" ]] &&
     [[ ! "${REQUESTED_TAG}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$ ]]; then
    die "invalid Feng release tag: ${REQUESTED_TAG}"
  fi
  if [[ -n "${REQUESTED_CHANNEL}" && "${REQUESTED_CHANNEL}" != "rc" ]]; then
    die "unsupported Feng release channel: ${REQUESTED_CHANNEL}"
  fi
  if [[ -n "${REQUESTED_TAG}" && -n "${REQUESTED_CHANNEL}" ]]; then
    die "--version and --channel are mutually exclusive"
  fi
}

# Detect the complete Feng host platform supported by the first release.
detect_host_platform() {
  local os
  local arch

  case "$(uname -s)" in
    Darwin) os="macos" ;;
    Linux) os="linux" ;;
    *) die "unsupported host OS: $(uname -s)" ;;
  esac
  case "$(uname -m)" in
    arm64|aarch64) arch="arm64" ;;
    x86_64|amd64) arch="x64" ;;
    *) die "unsupported host architecture: $(uname -m)" ;;
  esac
  case "${os}-${arch}" in
    macos-arm64) printf '%s\n' "macos-arm64" ;;
    linux-x64) printf '%s\n' "linux-x64-gnu" ;;
    linux-arm64) printf '%s\n' "linux-arm64-gnu" ;;
    *) die "unsupported Feng release host: ${os}-${arch}" ;;
  esac
}

# Resolve the latest stable GitHub Release tag.
# Return status 2 only when GitHub explicitly reports that no stable release exists.
resolve_latest_stable_tag() {
  local response_file="${TEMP_ROOT}/latest-release-response"
  local http_status
  local effective_url
  local tag

  if ! curl --location --silent --show-error -o /dev/null \
    -w '%{http_code}\n%{url_effective}\n' \
    "https://github.com/${REPOSITORY}/releases/latest" \
    > "${response_file}"; then
    die "failed to resolve the latest Feng release"
  fi
  http_status="$(sed -n '1p' "${response_file}")"
  effective_url="$(sed -n '2p' "${response_file}")"
  case "${http_status}" in
    200)
      ;;
    404)
      return 2
      ;;
    *)
      die "failed to resolve the latest Feng release: HTTP ${http_status}"
      ;;
  esac
  tag="${effective_url##*/}"
  [[ "${tag}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    die "latest Feng release has an invalid tag: ${tag}"
  printf '%s\n' "${tag}"
}

# Report whether the first non-negative decimal integer is greater than the second.
decimal_is_greater() {
  local left="$1"
  local right="$2"

  while [[ "${#left}" -gt 1 && "${left}" == 0* ]]; do
    left="${left#0}"
  done
  while [[ "${#right}" -gt 1 && "${right}" == 0* ]]; do
    right="${right#0}"
  done
  if [[ "${#left}" -ne "${#right}" ]]; then
    [[ "${#left}" -gt "${#right}" ]]
    return
  fi
  [[ "${left}" > "${right}" ]]
}

# Report whether one valid RC tag has a greater semantic version than another.
rc_tag_is_newer() {
  local candidate="$1"
  local current="$2"
  local candidate_components=()
  local current_components=()
  local component_index

  [[ "${candidate}" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)-rc(\.)?([0-9]+)$ ]] ||
    return 1
  candidate_components=(
    "${BASH_REMATCH[1]}"
    "${BASH_REMATCH[2]}"
    "${BASH_REMATCH[3]}"
    "${BASH_REMATCH[5]}"
  )
  if [[ -z "${current}" ]]; then
    return 0
  fi
  [[ "${current}" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)-rc(\.)?([0-9]+)$ ]] ||
    return 0
  current_components=(
    "${BASH_REMATCH[1]}"
    "${BASH_REMATCH[2]}"
    "${BASH_REMATCH[3]}"
    "${BASH_REMATCH[5]}"
  )
  for component_index in 0 1 2 3; do
    if decimal_is_greater \
      "${candidate_components[component_index]}" \
      "${current_components[component_index]}"; then
      return 0
    fi
    if decimal_is_greater \
      "${current_components[component_index]}" \
      "${candidate_components[component_index]}"; then
      return 1
    fi
  done
  return 1
}

# Resolve the semantically greatest published RC release through GitHub's API.
resolve_latest_rc_tag() {
  local page=1
  local response_file
  local tag_field
  local candidate
  local selected=""

  while true; do
    response_file="${TEMP_ROOT}/github-releases-${page}.json"
    curl --fail --location --silent --show-error \
      -H 'Accept: application/vnd.github+json' \
      "https://api.github.com/repos/${REPOSITORY}/releases?per_page=100&page=${page}" \
      -o "${response_file}" ||
      die "failed to resolve the latest Feng RC release"
    if ! grep -q '"tag_name"' "${response_file}"; then
      break
    fi
    while IFS= read -r tag_field; do
      tag_field="${tag_field%\"}"
      candidate="${tag_field##*\"}"
      if rc_tag_is_newer "${candidate}" "${selected}"; then
        selected="${candidate}"
      fi
    done < <(
      grep -Eo \
        '"tag_name"[[:space:]]*:[[:space:]]*"v[0-9]+\.[0-9]+\.[0-9]+-rc(\.)?[0-9]+"' \
        "${response_file}" || true
    )
    page=$((page + 1))
  done
  [[ -n "${selected}" ]] || die "no Feng RC release is available"
  printf '%s\n' "${selected}"
}

# Resolve an explicit release tag, selected channel, or default stable release.
resolve_release_tag() {
  local tag
  local status

  if [[ -n "${REQUESTED_TAG}" ]]; then
    printf '%s\n' "${REQUESTED_TAG}"
    return
  fi
  if [[ "${REQUESTED_CHANNEL}" == "rc" ]]; then
    resolve_latest_rc_tag
    return
  fi
  if tag="$(resolve_latest_stable_tag)"; then
    printf '%s\n' "${tag}"
    return
  else
    status=$?
  fi
  [[ "${status}" -eq 2 ]] || return "${status}"
  resolve_latest_rc_tag
}

# Download one release archive with terminal-aware progress reporting.
download_release_archive() {
  local download_url="$1"
  local archive_path="$2"

  if [[ -t 2 ]]; then
    curl --fail --location --progress-bar \
      "${download_url}" -o "${archive_path}"
  else
    curl --fail --location --silent --show-error \
      "${download_url}" -o "${archive_path}"
  fi
}

# Select the shell startup file and matching PATH statement.
configure_shell_path() {
  local shell_name="${SHELL##*/}"

  case "${shell_name}" in
    zsh)
      SHELL_FILE="${HOME}/.zshrc"
      PATH_ENTRY='export PATH="$HOME/.feng/bin:$PATH"'
      ;;
    bash)
      if [[ "$(uname -s)" == "Darwin" ]]; then
        SHELL_FILE="${HOME}/.bash_profile"
      else
        SHELL_FILE="${HOME}/.bashrc"
      fi
      PATH_ENTRY='export PATH="$HOME/.feng/bin:$PATH"'
      ;;
    fish)
      SHELL_FILE="${HOME}/.config/fish/config.fish"
      PATH_ENTRY='fish_add_path "$HOME/.feng/bin"'
      ;;
    ksh)
      SHELL_FILE="${HOME}/.kshrc"
      PATH_ENTRY='export PATH="$HOME/.feng/bin:$PATH"'
      ;;
    *)
      SHELL_FILE="${HOME}/.profile"
      PATH_ENTRY='export PATH="$HOME/.feng/bin:$PATH"'
      ;;
  esac
}

# Resolve a symlinked shell startup file without replacing the user's symlink.
resolve_shell_file() {
  local link_target
  local link_directory
  local link_name
  local resolution_count=0

  while [[ -L "${SHELL_FILE}" ]]; do
    resolution_count=$((resolution_count + 1))
    [[ "${resolution_count}" -le 40 ]] ||
      die "shell startup file has too many symbolic-link levels: ${SHELL_FILE}"
    link_target="$(readlink "${SHELL_FILE}")" ||
      die "failed to read shell startup file link: ${SHELL_FILE}"
    case "${link_target}" in
      /*)
        SHELL_FILE="${link_target}"
        ;;
      *)
        link_directory="$(dirname "${SHELL_FILE}")"
        link_name="${link_target##*/}"
        link_directory="${link_directory}/$(dirname "${link_target}")"
        [[ -d "${link_directory}" ]] ||
          die "shell startup file link parent does not exist: ${link_directory}"
        SHELL_FILE="$(cd "${link_directory}" && pwd -P)/${link_name}"
        ;;
    esac
  done
  if [[ -e "${SHELL_FILE}" && ! -f "${SHELL_FILE}" ]]; then
    die "shell startup path is not a regular file: ${SHELL_FILE}"
  fi
}

# Reject archive entries outside the single expected package root.
validate_archive_entries() {
  local archive_path="$1"
  local package_name="$2"
  local entry
  local entry_count=0

  while IFS= read -r entry; do
    [[ -n "${entry}" ]] || continue
    case "${entry}" in
      "${package_name}"|"${package_name}/"|"${package_name}/"*)
        ;;
      *)
        die "release archive contains an unexpected path: ${entry}"
        ;;
    esac
    case "${entry}" in
      *[[:space:]]*)
        die "release archive contains whitespace in a path: ${entry}"
        ;;
    esac
    case "/${entry}/" in
      *"/../"*|*"/./"*)
        die "release archive contains an unsafe path: ${entry}"
        ;;
    esac
    entry_count=$((entry_count + 1))
  done < <(unzip -Z1 "${archive_path}")
  [[ "${entry_count}" -gt 0 ]] ||
    die "release archive is empty: ${archive_path}"
}

# Verify every archived symbolic link resolves inside the package root.
validate_archive_symlinks() {
  local archive_path="$1"
  local package_name="$2"
  local entry
  local target
  local combined
  local component
  local depth
  local components

  while IFS= read -r entry; do
    [[ -n "${entry}" ]] || continue
    target="$(unzip -p "${archive_path}" "${entry}")" ||
      die "failed to read symbolic-link target from archive: ${entry}"
    [[ -n "${target}" && "${target}" != /* ]] ||
      die "release archive contains an unsafe symbolic link: ${entry} -> ${target}"
    combined="${entry%/*}/${target}"
    depth=0
    IFS='/' read -r -a components <<< "${combined}"
    for component in "${components[@]}"; do
      case "${component}" in
        ""|.)
          ;;
        ..)
          depth=$((depth - 1))
          [[ "${depth}" -ge 1 ]] ||
            die "release archive symbolic link escapes package root: ${entry} -> ${target}"
          ;;
        *)
          depth=$((depth + 1))
          ;;
      esac
    done
    [[ "${components[0]:-}" == "${package_name}" ]] ||
      die "release archive symbolic link has an invalid root: ${entry} -> ${target}"
  done < <(unzip -Z -l "${archive_path}" | awk '$1 ~ /^l/ { print $NF }')
}

# Prepare an atomically replaceable shell startup file.
prepare_shell_file() {
  local shell_dir

  shell_dir="$(dirname "${SHELL_FILE}")"
  mkdir -p "${shell_dir}"
  SHELL_FILE_TEMP="$(mktemp "${shell_dir}/.feng-shell.XXXXXX")"
  if [[ -f "${SHELL_FILE}" ]]; then
    cp -p "${SHELL_FILE}" "${SHELL_FILE_TEMP}"
  fi
  if ! grep -Fqx "${PATH_ENTRY}" "${SHELL_FILE_TEMP}"; then
    if [[ -s "${SHELL_FILE_TEMP}" ]] &&
       [[ "$(tail -c 1 "${SHELL_FILE_TEMP}" | wc -l | tr -d ' ')" == "0" ]]; then
      printf '\n' >> "${SHELL_FILE_TEMP}"
    fi
    printf '%s\n' "${PATH_ENTRY}" >> "${SHELL_FILE_TEMP}"
  fi
}

# Verify the extracted tree contains every mandatory distribution component.
validate_package_layout() {
  local package_root="$1"
  local header
  local platform
  local tool

  [[ -x "${package_root}/bin/feng" ]] ||
    die "release package does not contain an executable bin/feng"
  for header in \
    feng_generated.h \
    feng_runtime.h \
    feng_runtime_contract.inc; do
    [[ -f "${package_root}/include/${header}" ]] ||
      die "release package does not contain include/${header}"
  done
  for platform in \
    macos-arm64 \
    linux-x64-gnu \
    linux-x64-musl \
    linux-arm64-gnu \
    linux-arm64-musl; do
    [[ -f "${package_root}/lib/${platform}/libfeng_runtime.a" ]] ||
      die "release package does not contain runtime for ${platform}"
  done
  for platform in \
    linux-x64-gnu \
    linux-x64-musl \
    linux-arm64-gnu \
    linux-arm64-musl; do
    [[ -d "${package_root}/toolchain/sysroot/${platform}/usr/include" ]] ||
      die "release package does not contain sysroot headers for ${platform}"
    [[ -d "${package_root}/toolchain/sysroot/${platform}/usr/lib" ]] ||
      die "release package does not contain sysroot libraries for ${platform}"
    [[ -d "${package_root}/toolchain/sysroot/${platform}/lib/gcc" ]] ||
      die "release package does not contain compiler runtime for ${platform}"
  done
  for tool in \
    clang \
    lld \
    ld.lld \
    llvm-ar \
    llvm-ranlib \
    lldb \
    lldb-dap \
    lldb-argdumper; do
    [[ -x "${package_root}/toolchain/llvm/bin/${tool}" ]] ||
      die "release package does not contain executable LLVM tool ${tool}"
  done
}

# Roll back a partially committed install and remove invocation-owned staging.
cleanup() {
  local exit_status=$?

  if [[ "${exit_status}" -ne 0 ]]; then
    if [[ "${SHELL_FILE_COMMITTED}" -eq 1 ]]; then
      if [[ -n "${SHELL_FILE_BACKUP}" && -f "${SHELL_FILE_BACKUP}" ]]; then
        mv "${SHELL_FILE_BACKUP}" "${SHELL_FILE}"
      else
        rm -f "${SHELL_FILE}"
      fi
    fi
    if [[ "${INSTALL_COMMITTED}" -eq 1 ]]; then
      rm -rf "${INSTALL_ROOT}"
    fi
    if [[ "${INSTALL_BACKED_UP}" -eq 1 &&
          -n "${BACKUP_ROOT}" &&
          ( -e "${BACKUP_ROOT}" || -L "${BACKUP_ROOT}" ) ]]; then
      mv "${BACKUP_ROOT}" "${INSTALL_ROOT}"
    fi
  fi
  if [[ -n "${TEMP_ROOT}" && -d "${TEMP_ROOT}" ]]; then
    rm -rf "${TEMP_ROOT}"
  fi
  if [[ -n "${STAGING_ROOT}" && -d "${STAGING_ROOT}" ]]; then
    rm -rf "${STAGING_ROOT}"
  fi
  if [[ -n "${BACKUP_ROOT}" &&
        ( -e "${BACKUP_ROOT}" || -L "${BACKUP_ROOT}" ) ]]; then
    rm -rf "${BACKUP_ROOT}"
  fi
  if [[ -n "${SHELL_FILE_TEMP}" && -f "${SHELL_FILE_TEMP}" ]]; then
    rm -f "${SHELL_FILE_TEMP}"
  fi
  if [[ -n "${SHELL_FILE_BACKUP}" && -f "${SHELL_FILE_BACKUP}" ]]; then
    rm -f "${SHELL_FILE_BACKUP}"
  fi
  return "${exit_status}"
}

parse_arguments "$@"
[[ -n "${HOME:-}" && -d "${HOME}" ]] || die "HOME is not a directory"
[[ -n "${SHELL:-}" ]] || die "SHELL is not set"
require_cmd curl
require_cmd awk
require_cmd grep
require_cmd mktemp
require_cmd readlink
require_cmd unzip

trap cleanup EXIT

TEMP_BASE="${TMPDIR:-/tmp}"
mkdir -p "${TEMP_BASE}"
TEMP_ROOT="$(mktemp -d "${TEMP_BASE%/}/feng-install.XXXXXX")"
PLATFORM="$(detect_host_platform)"
TAG="$(resolve_release_tag)"
VERSION="${TAG#v}"
PACKAGE_NAME="feng-${VERSION}-${PLATFORM}"
DOWNLOAD_URL="https://github.com/${REPOSITORY}/releases/download/${TAG}/${PACKAGE_NAME}.zip"
ARCHIVE_PATH="${TEMP_ROOT}/${PACKAGE_NAME}.zip"
EXTRACT_ROOT="${TEMP_ROOT}/extract"
mkdir -p "${EXTRACT_ROOT}"

echo "==> Downloading ${PACKAGE_NAME}.zip"
download_release_archive "${DOWNLOAD_URL}" "${ARCHIVE_PATH}" ||
  die "failed to download ${DOWNLOAD_URL}"
validate_archive_entries "${ARCHIVE_PATH}" "${PACKAGE_NAME}"
validate_archive_symlinks "${ARCHIVE_PATH}" "${PACKAGE_NAME}"
unzip -q "${ARCHIVE_PATH}" -d "${EXTRACT_ROOT}" ||
  die "failed to extract ${PACKAGE_NAME}.zip"

PACKAGE_ROOT="${EXTRACT_ROOT}/${PACKAGE_NAME}"
validate_package_layout "${PACKAGE_ROOT}"
[[ -f "${PACKAGE_ROOT}/VERSION" ]] ||
  die "release package does not contain VERSION"
[[ "$(sed -n '1p' "${PACKAGE_ROOT}/VERSION")" == "${VERSION}" ]] ||
  die "release package VERSION does not match ${TAG}"

configure_shell_path
resolve_shell_file
prepare_shell_file
STAGING_ROOT="$(mktemp -d "${INSTALL_PARENT}/.feng-install.XXXXXX")"
cp -R -P "${PACKAGE_ROOT}/." "${STAGING_ROOT}/"

if [[ -e "${INSTALL_ROOT}" || -L "${INSTALL_ROOT}" ]]; then
  BACKUP_ROOT="$(mktemp -d "${INSTALL_PARENT}/.feng-backup.XXXXXX")"
  rmdir "${BACKUP_ROOT}"
  mv "${INSTALL_ROOT}" "${BACKUP_ROOT}"
  INSTALL_BACKED_UP=1
fi
mv "${STAGING_ROOT}" "${INSTALL_ROOT}"
STAGING_ROOT=""
INSTALL_COMMITTED=1

if [[ -f "${SHELL_FILE}" ]]; then
  SHELL_FILE_BACKUP="$(mktemp "$(dirname "${SHELL_FILE}")/.feng-shell-backup.XXXXXX")"
  cp "${SHELL_FILE}" "${SHELL_FILE_BACKUP}"
fi
mv "${SHELL_FILE_TEMP}" "${SHELL_FILE}"
SHELL_FILE_TEMP=""
SHELL_FILE_COMMITTED=1

if [[ -n "${BACKUP_ROOT}" &&
      ( -e "${BACKUP_ROOT}" || -L "${BACKUP_ROOT}" ) ]]; then
  rm -rf "${BACKUP_ROOT}"
  BACKUP_ROOT=""
  INSTALL_BACKED_UP=0
fi
if [[ -n "${SHELL_FILE_BACKUP}" && -f "${SHELL_FILE_BACKUP}" ]]; then
  rm -f "${SHELL_FILE_BACKUP}"
  SHELL_FILE_BACKUP=""
fi

echo "==> Installed Feng ${VERSION} in ${INSTALL_ROOT}"
echo "==> PATH updated in ${SHELL_FILE}"
echo "==> Restart your shell or run: source \"${SHELL_FILE}\""
