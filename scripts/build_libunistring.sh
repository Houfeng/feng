#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/libunistring"
OUTPUT_DIR="${1:-${PROJECT_ROOT}/std/lib}"
MAKE_BIN="${MAKE:-make}"

if [[ ! -d "${TARGET_DIR}" ]]; then
  echo "error: ${TARGET_DIR} does not exist" >&2
  echo "error: run ${PROJECT_ROOT}/scripts/fetch_libunistring.sh first" >&2
  exit 1
fi

if [[ ! -f "${TARGET_DIR}/Makefile" ]]; then
  echo "error: ${TARGET_DIR}/Makefile is missing" >&2
  echo "error: run ${PROJECT_ROOT}/scripts/fetch_libunistring.sh again to regenerate the vendored build files" >&2
  exit 1
fi

echo "==> Building libunistring into ${OUTPUT_DIR}"
"${MAKE_BIN}" -C "${TARGET_DIR}" OUTPUT_DIR="${OUTPUT_DIR}" install
echo "==> Built ${OUTPUT_DIR}/libfeng_std_unistring.a"