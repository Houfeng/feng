#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Build every native dependency carried by the Feng standard library.
"${SCRIPT_DIR}/build_libuv.sh"
"${SCRIPT_DIR}/build_libunistring.sh"
"${SCRIPT_DIR}/build_pcre2.sh"
"${SCRIPT_DIR}/build_libsodium.sh"
