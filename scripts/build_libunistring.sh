#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/libunistring"
MAKE_BIN="${MAKE:-make}"

detect_host_target() {
    local os arch

    case "$(uname -s)" in
        Darwin)               os="macos" ;;
        Linux)                os="linux" ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *)
            echo "error: unsupported host OS: $(uname -s)" >&2
            exit 1
            ;;
    esac

    case "$(uname -m)" in
        arm64|aarch64) arch="arm64" ;;
        x86_64|amd64)  arch="x64" ;;
        *)
            echo "error: unsupported host architecture: $(uname -m)" >&2
            exit 1
            ;;
    esac

    printf '%s-%s' "$os" "$arch"
}

HOST_TARGET="$(detect_host_target)"
OUTPUT_DIR="${1:-${PROJECT_ROOT}/std/extlib/${HOST_TARGET}}"

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