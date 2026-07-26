#!/usr/bin/env bash
set -euo pipefail

TAG=""
VERSION=""
PACKAGES_ROOT=""
REPOSITORY="${GH_REPO:-}"
QUERY_ERROR=""
REPOSITORY_SET=false
HOST_PLATFORMS=(
  "macos-arm64"
  "linux-x64-gnu"
  "linux-arm64-gnu"
)

# Print the supported GitHub Release publication invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/release_publish_github.sh \
    --tag=<v-version> \
    --version=<version> \
    --packages=<package-dir> \
    [--repository=<owner/repository>]
EOF
}

# Report one fatal GitHub Release publication error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Remove only the temporary query error file created by this invocation.
cleanup() {
  if [[ -n "${QUERY_ERROR}" && -f "${QUERY_ERROR}" ]]; then
    rm -f "${QUERY_ERROR}"
  fi
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --tag=*)
      [[ -z "${TAG}" ]] || die "--tag may only be specified once"
      TAG="${1#--tag=}"
      ;;
    --version=*)
      [[ -z "${VERSION}" ]] || die "--version may only be specified once"
      VERSION="${1#--version=}"
      ;;
    --packages=*)
      [[ -z "${PACKAGES_ROOT}" ]] || die "--packages may only be specified once"
      PACKAGES_ROOT="${1#--packages=}"
      ;;
    --repository=*)
      [[ "${REPOSITORY_SET}" == "false" ]] ||
        die "--repository may only be specified once"
      REPOSITORY="${1#--repository=}"
      REPOSITORY_SET=true
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
[[ "${TAG}" == "v${VERSION}" ]] ||
  die "release tag does not match version: ${TAG:-<empty>}"
[[ "${REPOSITORY}" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] ||
  die "invalid GitHub repository: ${REPOSITORY:-<empty>}"
[[ -d "${PACKAGES_ROOT}" ]] ||
  die "package directory not found: ${PACKAGES_ROOT}"
command -v gh >/dev/null 2>&1 || die "missing required command: gh"

PACKAGES_ROOT="$(cd "${PACKAGES_ROOT}" && pwd)"
ARCHIVES=()
for host_platform in "${HOST_PLATFORMS[@]}"; do
  archive="${PACKAGES_ROOT}/feng-${VERSION}-${host_platform}.zip"
  [[ -f "${archive}" ]] || die "release archive not found: ${archive}"
  ARCHIVES+=("${archive}")
done
[[ "$(find "${PACKAGES_ROOT}" -maxdepth 1 -type f -name '*.zip' | wc -l | tr -d ' ')" == "3" ]] ||
  die "package directory must contain exactly three release archives"

EXPECTED_PRERELEASE="false"
RELEASE_FLAGS=(
  --verify-tag
  --generate-notes
  --title "Feng ${VERSION}"
  --repo "${REPOSITORY}"
)
if [[ "${VERSION}" == *-* ]]; then
  EXPECTED_PRERELEASE="true"
  RELEASE_FLAGS+=(--prerelease)
fi

QUERY_ERROR="$(mktemp "${PACKAGES_ROOT}/.release-query.XXXXXX")"
trap cleanup EXIT
if RELEASE_STATE="$(gh api \
  "repos/${REPOSITORY}/releases/tags/${TAG}" \
  --jq '[.draft, (.immutable // false), .prerelease] | @tsv' \
  2>"${QUERY_ERROR}")"; then
  read -r IS_DRAFT IS_IMMUTABLE IS_PRERELEASE <<< "${RELEASE_STATE}"
  [[ "${IS_DRAFT}" == "true" || "${IS_DRAFT}" == "false" ]] ||
    die "GitHub Release returned an invalid draft state for ${TAG}"
  [[ "${IS_IMMUTABLE}" == "false" ]] ||
    die "existing GitHub Release is immutable: ${TAG}"
  [[ "${IS_PRERELEASE}" == "${EXPECTED_PRERELEASE}" ]] ||
    die "existing GitHub Release prerelease state does not match ${TAG}"
  gh release upload \
    "${TAG}" "${ARCHIVES[@]}" \
    --repo "${REPOSITORY}"
  if [[ "${IS_DRAFT}" == "true" ]]; then
    gh release edit "${TAG}" --draft=false --repo "${REPOSITORY}"
  fi
elif grep -Eq 'HTTP[[:space:]]+404|HTTP 404|status code 404' "${QUERY_ERROR}"; then
  gh release create \
    "${TAG}" "${ARCHIVES[@]}" \
    "${RELEASE_FLAGS[@]}"
else
  cat "${QUERY_ERROR}" >&2
  die "failed to query existing GitHub Release: ${TAG}"
fi

echo "==> Published Feng ${VERSION} release assets for ${TAG}"
