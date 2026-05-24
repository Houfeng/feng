#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/libunwind"
TMP_ROOT="${PROJECT_ROOT}/temp"
TMP_DIR="${TMP_ROOT}/libunwind_tmp"

LIBUNWIND_VERSION="${LIBUNWIND_VERSION:-20.1.8}"
LIBUNWIND_TAG="llvmorg-${LIBUNWIND_VERSION}"
SRC_URL="${LIBUNWIND_SRC_URL:-https://github.com/llvm/llvm-project/releases/download/${LIBUNWIND_TAG}/libunwind-${LIBUNWIND_VERSION}.src.tar.xz}"
TAR_FILE="${TMP_ROOT}/libunwind-${LIBUNWIND_VERSION}.src.tar.xz"
EXTRACTED_SRC="${TMP_DIR}/libunwind-${LIBUNWIND_VERSION}.src"

SOURCE_FILES=(
  Unwind-sjlj.c
  UnwindLevel1.c
  UnwindLevel1-gcc-ext.c
  UnwindRegistersRestore.S
  UnwindRegistersSave.S
  libunwind.cpp
)

HEADER_FILES=(
  AddressSpace.hpp
  CompactUnwinder.hpp
  DwarfInstructions.hpp
  DwarfParser.hpp
  EHHeaderParser.hpp
  FrameHeaderCache.hpp
  RWMutex.hpp
  Registers.hpp
  Unwind-EHABI.h
  UnwindCursor.hpp
  assembly.h
  cet_unwind.h
  config.h
  dwarf2.h
  libunwind_ext.h
)

PUBLIC_HEADERS=(
  __libunwind_config.h
  libunwind.h
  unwind.h
  unwind_arm_ehabi.h
  unwind_itanium.h
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

echo "==> Syncing LLVM libunwind ${LIBUNWIND_VERSION} into ${TARGET_DIR}"

rm -rf "${TARGET_DIR}" "${TMP_ROOT}"
mkdir -p "${TARGET_DIR}/include" "${TARGET_DIR}/src" "${TMP_DIR}"

echo "==> Downloading ${SRC_URL}"
curl -sSLf --connect-timeout 15 "${SRC_URL}" -o "${TAR_FILE}"

echo "==> Extracting source archive"
tar -xf "${TAR_FILE}" -C "${TMP_DIR}"

echo "==> Copying public headers"
for file_name in "${PUBLIC_HEADERS[@]}"; do
  copy_required "${EXTRACTED_SRC}/include/${file_name}" "${TARGET_DIR}/include/"
done
if [[ -d "${EXTRACTED_SRC}/include/mach-o" ]]; then
  mkdir -p "${TARGET_DIR}/include/mach-o"
  copy_required "${EXTRACTED_SRC}/include/mach-o/compact_unwind_encoding.h" \
                "${TARGET_DIR}/include/mach-o/"
fi

echo "==> Copying runtime source closure"
for file_name in "${SOURCE_FILES[@]}"; do
  copy_required "${EXTRACTED_SRC}/src/${file_name}" "${TARGET_DIR}/src/"
done
for file_name in "${HEADER_FILES[@]}"; do
  copy_required "${EXTRACTED_SRC}/src/${file_name}" "${TARGET_DIR}/src/"
done

copy_required "${EXTRACTED_SRC}/LICENSE.TXT" "${TARGET_DIR}/LICENSE.TXT"

cat > "${TARGET_DIR}/README.md" <<EOF
# LLVM libunwind minimal source closure

This directory vendors the LLVM libunwind source files needed by Feng's native
exception backend.

Version: ${LIBUNWIND_VERSION}

Included:
- public unwind headers under include/
- native Itanium unwind sources used on macOS/Linux
- no tests, docs, CMake project files, or shared-library artifacts

Build:
- \`make\` builds a static archive and stages it into \`../../build/lib\` by default.
- \`make OUTPUT_DIR=<path>\` overrides the staging directory.
- \`make install\` is an alias of the staging step.
- default staged library name: \`libfeng_libunwind.a\`
EOF

cat > "${TARGET_DIR}/Makefile" <<'EOF'
CC ?= cc
CXX ?= c++
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra -fPIC
CXXFLAGS ?= $(CFLAGS) -std=c++11 -fno-exceptions -fno-rtti
CPPFLAGS ?= -I./include -I./src -DNDEBUG -D_LIBUNWIND_IS_NATIVE_ONLY -D_LIBUNWIND_DISABLE_VISIBILITY_ANNOTATIONS

TARGET ?= libfeng_libunwind.a
OUTPUT_DIR ?= ../../build/lib
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

CSRC = src/Unwind-sjlj.c \
       src/UnwindLevel1.c \
       src/UnwindLevel1-gcc-ext.c
CXXSRC = src/libunwind.cpp
ASMSRC = src/UnwindRegistersRestore.S \
         src/UnwindRegistersSave.S

OBJS = $(CSRC:.c=.o) $(CXXSRC:.cpp=.o) $(ASMSRC:.S=.o)

all: $(OUTPUT_TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

$(OUTPUT_TARGET): $(TARGET)
	mkdir -p $(OUTPUT_DIR)
	cp $(TARGET) $(OUTPUT_TARGET)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

install: $(OUTPUT_TARGET)

clean:
	$(RM) $(OBJS) $(TARGET) $(OUTPUT_TARGET)

.PHONY: all install clean
EOF

echo "==> Done. Build with: ${PROJECT_ROOT}/scripts/build_libunwind.sh"