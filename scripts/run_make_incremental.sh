#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_ROOT}"

# Capture every regular build file and symbolic link with write-sensitive metadata.
snapshot_build() {
  case "$(uname -s)" in
    Darwin)
      find build \( -type f -o -type l \) \
        -exec stat -f '%N %m %z' {} + | LC_ALL=C sort
      ;;
    Linux)
      find build \( -type f -o -type l \) \
        -exec stat -c '%n %Y %s' {} + | LC_ALL=C sort
      ;;
    *)
      echo "error: unsupported host OS for incremental-build test: $(uname -s)" >&2
      return 1
      ;;
  esac
}

before="$(snapshot_build)"
output="$(LC_ALL=C make all 2>&1)" || {
  printf '%s\n' "${output}" >&2
  exit 1
}
after="$(snapshot_build)"

if ! grep -F "Nothing to be done" >/dev/null <<<"${output}"; then
  echo "error: no-op make all did not report Nothing to be done" >&2
  printf '%s\n' "${output}" >&2
  exit 1
fi
if [[ "${before}" != "${after}" ]]; then
  echo "error: no-op make all changed files under build/" >&2
  exit 1
fi

echo "make incremental: no-op build reported and wrote no files"
