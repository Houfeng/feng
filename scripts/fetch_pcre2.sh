#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/PCRE2"
INCLUDE_DIR="${TARGET_DIR}/include"
SRC_DIR="${TARGET_DIR}/src"
TMP_ROOT="${PROJECT_ROOT}/temp"
TMP_DIR="${TMP_ROOT}/pcre2_tmp"
BUILD_DIR="${TMP_DIR}/build"

PCRE2_VERSION="${PCRE2_VERSION:-10.45}"
PCRE2_TAG="pcre2-${PCRE2_VERSION}"
SRC_URL="${PCRE2_SRC_URL:-https://github.com/PCRE2Project/pcre2/releases/download/${PCRE2_TAG}/${PCRE2_TAG}.tar.gz}"
TAR_FILE="${TMP_ROOT}/${PCRE2_TAG}.tar.gz"
EXTRACTED_SRC="${TMP_DIR}/${PCRE2_TAG}"

SOURCE_FILES=(
  pcre2_auto_possess.c
  pcre2_chkdint.c
  pcre2_compile.c
  pcre2_compile_class.c
  pcre2_config.c
  pcre2_context.c
  pcre2_convert.c
  pcre2_dfa_match.c
  pcre2_error.c
  pcre2_extuni.c
  pcre2_find_bracket.c
  pcre2_jit_compile.c
  pcre2_jit_match.c
  pcre2_jit_misc.c
  pcre2_maketables.c
  pcre2_match.c
  pcre2_match_data.c
  pcre2_newline.c
  pcre2_ord2utf.c
  pcre2_pattern_info.c
  pcre2_printint.c
  pcre2_script_run.c
  pcre2_serialize.c
  pcre2_string_utils.c
  pcre2_study.c
  pcre2_substitute.c
  pcre2_substring.c
  pcre2_tables.c
  pcre2_ucd.c
  pcre2_ucptables.c
  pcre2_valid_utf.c
  pcre2_xclass.c
)

HEADER_FILES=(
  pcre2_compile.h
  pcre2_internal.h
  pcre2_intmodedep.h
  pcre2_ucp.h
  pcre2_util.h
)

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

copy_required() {
  local source_path="$1"
  local target_path="$2"

  if [[ ! -f "${source_path}" ]]; then
    echo "error: required file not found: ${source_path}" >&2
    exit 1
  fi

  cp "${source_path}" "${target_path}"
}

cleanup() {
  rm -rf "${TMP_ROOT}"
}

trap cleanup EXIT

require_cmd curl
require_cmd tar
require_cmd cmake

echo "==> Syncing PCRE2 ${PCRE2_VERSION} into ${TARGET_DIR}"

rm -rf "${TARGET_DIR}" "${TMP_ROOT}"
mkdir -p "${INCLUDE_DIR}" "${SRC_DIR}" "${TMP_DIR}"

echo "==> Downloading ${SRC_URL}"
curl -sSLf --connect-timeout 15 "${SRC_URL}" -o "${TAR_FILE}"

echo "==> Extracting source archive"
tar -xf "${TAR_FILE}" -C "${TMP_DIR}"

echo "==> Generating minimal 8-bit Unicode configuration"
cmake \
  -S "${EXTRACTED_SRC}" \
  -B "${BUILD_DIR}" \
  -DBUILD_SHARED_LIBS=OFF \
  -DPCRE2_BUILD_PCRE2_8=ON \
  -DPCRE2_BUILD_PCRE2_16=OFF \
  -DPCRE2_BUILD_PCRE2_32=OFF \
  -DPCRE2_SUPPORT_UNICODE=ON \
  -DPCRE2_SUPPORT_JIT=OFF \
  -DPCRE2_BUILD_TESTS=OFF \
  -DPCRE2_BUILD_PCRE2GREP=OFF \
  -DPCRE2_BUILD_PCRE2TEST=OFF \
  -DPCRE2_SUPPORT_LIBBZ2=OFF \
  -DPCRE2_SUPPORT_LIBZ=OFF \
  -DPCRE2_SUPPORT_LIBEDIT=OFF \
  -DPCRE2_SUPPORT_LIBREADLINE=OFF \
  >/dev/null

echo "==> Copying generated headers and tables"
copy_required "${BUILD_DIR}/pcre2.h" "${INCLUDE_DIR}/"
copy_required "${BUILD_DIR}/config.h" "${SRC_DIR}/"
copy_required "${BUILD_DIR}/pcre2_chartables.c" "${SRC_DIR}/"

echo "==> Copying required internal headers"
for file_name in "${HEADER_FILES[@]}"; do
  copy_required "${EXTRACTED_SRC}/src/${file_name}" "${SRC_DIR}/"
done

echo "==> Copying required source files"
for file_name in "${SOURCE_FILES[@]}"; do
  copy_required "${EXTRACTED_SRC}/src/${file_name}" "${SRC_DIR}/"
done

echo "==> Copying license"
license_copied=0
for license_name in LICENCE LICENCE.md LICENSE LICENSE.md COPYING COPYING.md; do
  if [[ -f "${EXTRACTED_SRC}/${license_name}" ]]; then
    cp "${EXTRACTED_SRC}/${license_name}" "${TARGET_DIR}/${license_name}"
    license_copied=1
    break
  fi
done

if [[ "${license_copied}" -ne 1 ]]; then
  echo "error: could not find an upstream license file" >&2
  exit 1
fi

cat > "${TARGET_DIR}/README.md" <<'EOF'
# PCRE2 minimal subset

This directory vendors the minimal 8-bit PCRE2 static library subset needed by Feng.

Included:
- generated public header: include/pcre2.h
- generated build configuration: src/config.h
- generated chartables source: src/pcre2_chartables.c
- 8-bit core sources with Unicode/UTF enabled
- no 16/32-bit libraries
- no POSIX wrapper
- no tools, tests, or shared-library artifacts
- no SLJIT JIT dependency tree

Build:
- `make` builds the static library and stages it into `../../std/std/extlib/<host-platform>` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_std_pcre2.a`
EOF

cat > "${TARGET_DIR}/Makefile" <<'EOF'
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./include -I./src -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8

TARGET ?= libfeng_std_pcre2.a
HOST_OS :=   $(shell sh -c 'os=$$(uname -s); if [ "$$os" = Darwin ]; then printf macos; elif [ "$$os" = Linux ]; then printf linux; else printf %s "$$os" | tr "[:upper:]" "[:lower:]"; fi')
HOST_ARCH := $(if $(filter x86_64 amd64,$(shell uname -m)),x64,$(if $(filter arm64 aarch64,$(shell uname -m)),arm64,$(shell uname -m)))
HOST_PLATFORM := $(if $(filter linux,$(HOST_OS)),$(HOST_OS)-$(HOST_ARCH)-gnu,$(HOST_OS)-$(HOST_ARCH))
OUTPUT_DIR ?= ../../std/std/extlib/$(HOST_PLATFORM)
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

SRCS = src/pcre2_chartables.c \
	   src/pcre2_auto_possess.c \
	   src/pcre2_chkdint.c \
	   src/pcre2_compile.c \
	   src/pcre2_compile_class.c \
	   src/pcre2_config.c \
	   src/pcre2_context.c \
	   src/pcre2_convert.c \
	   src/pcre2_dfa_match.c \
	   src/pcre2_error.c \
	   src/pcre2_extuni.c \
	   src/pcre2_find_bracket.c \
	   src/pcre2_jit_compile.c \
	   src/pcre2_maketables.c \
	   src/pcre2_match.c \
	   src/pcre2_match_data.c \
	   src/pcre2_newline.c \
	   src/pcre2_ord2utf.c \
	   src/pcre2_pattern_info.c \
	   src/pcre2_script_run.c \
	   src/pcre2_serialize.c \
	   src/pcre2_string_utils.c \
	   src/pcre2_study.c \
	   src/pcre2_substitute.c \
	   src/pcre2_substring.c \
	   src/pcre2_tables.c \
	   src/pcre2_ucd.c \
	   src/pcre2_valid_utf.c \
	   src/pcre2_xclass.c

OBJS = $(SRCS:.c=.o)

all: $(OUTPUT_TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

$(OUTPUT_TARGET): $(TARGET) ; @mkdir -p "$(OUTPUT_DIR)" && cp "$(TARGET)" "$(OUTPUT_TARGET)"

install: $(OUTPUT_TARGET)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean: ; rm -f $(OBJS) $(TARGET) $(OUTPUT_TARGET)

.PHONY: all clean install
EOF

echo "==> PCRE2 sync complete"
echo "==> Build with: ${PROJECT_ROOT}/scripts/build_pcre2.sh"
