#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/libsodium"
MAKE_BIN="${MAKE:-make}"

# shellcheck source=std_extlib_build_common.sh
source "${SCRIPT_DIR}/std_extlib_build_common.sh"

feng_std_extlib_parse_args \
  "usage: scripts/build_libsodium.sh [--libc=gnu|musl] [output-archive]" "$@"
feng_std_extlib_configure_target

OUTPUT_DIR=""
if [[ -n "${FENG_STD_EXTLIB_OUTPUT}" ]]; then
  [[ "$(basename "${FENG_STD_EXTLIB_OUTPUT}")" == "libfeng_std_sodium.a" ]] ||
    feng_std_extlib_die \
      "custom libsodium output must end with libfeng_std_sodium.a"
  OUTPUT_DIR="$(dirname "${FENG_STD_EXTLIB_OUTPUT}")"
fi
feng_std_extlib_build_archive \
  "${TARGET_DIR}" "libfeng_std_sodium.a" "${OUTPUT_DIR}"
