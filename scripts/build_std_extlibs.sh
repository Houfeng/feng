#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Build every standard-library dependency for one target libc.
build_target() {
  local build_script

  for build_script in \
    build_libuv.sh \
    build_libunistring.sh \
    build_pcre2.sh \
    build_libsodium.sh; do
    if [[ "$#" -eq 1 ]]; then
      "${SCRIPT_DIR}/${build_script}" "--libc=$1"
    else
      "${SCRIPT_DIR}/${build_script}"
    fi
  done
}

[[ "$#" -eq 0 ]] || {
  echo "error: usage: scripts/build_std_extlibs.sh" >&2
  exit 1
}

case "$(uname -s)" in
  Darwin)
    build_target
    ;;
  Linux)
    build_target gnu
    build_target musl
    ;;
  *)
    echo "error: unsupported host OS: $(uname -s)" >&2
    exit 1
    ;;
esac
