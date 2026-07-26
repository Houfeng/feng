#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/PCRE2"
MAKE_BIN="${MAKE:-make}"

# shellcheck source=std_extlib_build_common.sh
source "${SCRIPT_DIR}/std_extlib_build_common.sh"

feng_std_extlib_parse_args \
  "usage: scripts/build_pcre2.sh [--libc=gnu|musl] [output-dir]" "$@"
feng_std_extlib_configure_target
feng_std_extlib_build_archive \
  "${TARGET_DIR}" "libfeng_std_pcre2.a" "${FENG_STD_EXTLIB_OUTPUT}"
