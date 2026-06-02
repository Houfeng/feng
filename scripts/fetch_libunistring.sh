#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# 🎯 物理路径自适应定位
# ==============================================================================
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "${SCRIPT_DIR}/.." && pwd )"
TARGET_DIR="${PROJECT_ROOT}/third_party/libunistring"
PUBLIC_INCLUDE_DIR="${TARGET_DIR}/include"
INTERNAL_SRC_DIR="${TARGET_DIR}/src/lib"

echo "========================================================"
echo "🚀 启动 libunistring 静态库源码提取程序"
echo "⚙️  项目根目录: ${PROJECT_ROOT}"
echo "🎯 目标存储目录: ${TARGET_DIR}"
echo "========================================================"

# 清理并重建目标物理现场
rm -rf "${TARGET_DIR}"
mkdir -p "${PUBLIC_INCLUDE_DIR}/unistring"
mkdir -p "${INTERNAL_SRC_DIR}/unistr"
mkdir -p "${INTERNAL_SRC_DIR}/unigbrk"
mkdir -p "${INTERNAL_SRC_DIR}/unictype"
mkdir -p "${INTERNAL_SRC_DIR}/unicase"
mkdir -p "${INTERNAL_SRC_DIR}/unistring"
mkdir -p "${TARGET_DIR}/src"

# 将临时工作区显式指定到项目根目录下的 temp/ 目录中
TMP_ROOT="${PROJECT_ROOT}/temp"
TMP_DIR="${TMP_ROOT}/libunistring_tmp"

rm -rf "${TMP_ROOT}"
mkdir -p "${TMP_DIR}"

# 🎯 下载官方 1.4.2 源码包
SRC_URL="https://ftp.gnu.org/gnu/libunistring/libunistring-1.4.2.tar.gz"
TAR_FILE="${TMP_ROOT}/libunistring-1.4.2.tar.gz"

echo "📦 [1/4] 正在从 GNU 官方 FTP 下载源码归档..."
curl -s -L -f --connect-timeout 15 "${SRC_URL}" -o "${TAR_FILE}"

echo "📦 [2/4] 正在解压源码包..."
tar -xf "${TAR_FILE}" -C "${TMP_DIR}"
EXTRACTED_SRC="${TMP_DIR}/libunistring-1.4.2"

# ------------------------------------------------------------------------------
# 🔥 正统步骤：在 temp 里运行一次 configure，让 GNU 正常生成 unitypes.h 和 config.h
# ------------------------------------------------------------------------------
echo "⚙️  [3/4] 正在运行正规配置以生成标准头文件..."
cd "${EXTRACTED_SRC}"
# 禁止一切不需要的模块，只做纯净的本地配置生成
env PATH="${PATH}" HOME="${HOME}" LC_ALL=C LANG=C CC=cc \
       ./configure --disable-shared --disable-rpath --without-libiconv-prefix > /dev/null

# 本仓库保留 UTF-8 rune / grapheme / unicase 所需的最小子集。
GENERATED_HEADERS=(
       unitypes.h
       unistr.h
       unigbrk.h
       unictype.h
       unicase.h
)

env PATH="${PATH}" HOME="${HOME}" LC_ALL=C LANG=C CC=cc make -C "lib" \
       "${GENERATED_HEADERS[@]}" \
       unistring/stdint.h \
       unistring/woe32dll.h > /dev/null

# ------------------------------------------------------------------------------
# 🚚 [4/4] 提取 rune / grapheme 最小公开头与源码闭包
# ------------------------------------------------------------------------------
echo "🚚 正在导出 rune / grapheme / unicase 最小闭包到 third_party..."

# 1. 公开头：只暴露 rune / grapheme 所需接口
cp "lib/unitypes.h" "${PUBLIC_INCLUDE_DIR}/"
cp "lib/unistring/stdint.h" "${PUBLIC_INCLUDE_DIR}/unistring/"

cat << 'EOF' > "${PUBLIC_INCLUDE_DIR}/feng_u8_rune.h"
#ifndef FENG_U8_RUNE_H
#define FENG_U8_RUNE_H

#include "unitypes.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t u8_mbsnlen(const uint8_t *s, size_t n);
const uint8_t *u8_next(ucs4_t *puc, const uint8_t *s);
const uint8_t *u8_prev(ucs4_t *puc, const uint8_t *s, const uint8_t *start);

#ifdef __cplusplus
}
#endif

#endif
EOF

cat << 'EOF' > "${PUBLIC_INCLUDE_DIR}/feng_u8_case.h"
#ifndef FENG_U8_CASE_H
#define FENG_U8_CASE_H

#include "unitypes.h"

#ifdef __cplusplus
extern "C" {
#endif

ucs4_t uc_tolower(ucs4_t uc);
ucs4_t uc_toupper(ucs4_t uc);

#ifdef __cplusplus
}
#endif

#endif
EOF

cat << 'EOF' > "${PUBLIC_INCLUDE_DIR}/feng_u8_grapheme.h"
#ifndef FENG_U8_GRAPHEME_H
#define FENG_U8_GRAPHEME_H

#include "unitypes.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void u8_grapheme_breaks(const uint8_t *s, size_t n, char *p);
const uint8_t *u8_grapheme_next(const uint8_t *s, const uint8_t *end);
const uint8_t *u8_grapheme_prev(const uint8_t *s, const uint8_t *start);

#ifdef __cplusplus
}
#endif

#endif
EOF

cat << 'EOF' > "${TARGET_DIR}/README.md"
# libunistring minimal subset

This directory vendors the UTF-8 rune, grapheme, and case-mapping subset needed by Feng.

Public headers:
- include/unitypes.h
- include/feng_u8_rune.h
- include/feng_u8_grapheme.h
- include/feng_u8_case.h

Supported operations:
- UTF-8 rune count: u8_mbsnlen
- UTF-8 rune traversal: u8_next, u8_prev
- UTF-8 grapheme traversal: u8_grapheme_next, u8_grapheme_prev
- UTF-8 grapheme boundary map: u8_grapheme_breaks
- Unicode code point case mapping: uc_tolower, uc_toupper

Build:
- `make` builds the static library and stages it into `../../std/extlib/<os>-<arch>` by default.
- `make OUTPUT_DIR=<path>` overrides the staging directory.
- `make install` is an alias of the staging step.
- default staged library name: `libfeng_std_unistring.a`
EOF

# 2. 内部配置、完整内部头与最小源码闭包
cp "config.h" "${TARGET_DIR}/src/"

cp "lib/unitypes.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unistr.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unigbrk.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unictype.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unicase.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unistring-notinline.h" "${INTERNAL_SRC_DIR}/"

cp "lib/unistring/cdefs.h" "${INTERNAL_SRC_DIR}/unistring/"
cp "lib/unistring/inline.h" "${INTERNAL_SRC_DIR}/unistring/"
cp "lib/unistring/stdint.h" "${INTERNAL_SRC_DIR}/unistring/"
cp "lib/unistring/woe32dll.h" "${INTERNAL_SRC_DIR}/unistring/"

UNISTR_SOURCES=(
       u8-mbtouc-aux.c
       u8-mbtoucr.c
       u8-mbsnlen.c
       u8-strmbtouc.c
       u8-next.c
       u8-prev.c
       u8-uctomb-aux.c
)

for source in "${UNISTR_SOURCES[@]}"; do
       cp "lib/unistr/${source}" "${INTERNAL_SRC_DIR}/unistr/"
done

UNIGBRK_FILES=(
       u8-grapheme-breaks.c
       u8-grapheme-next.c
       u8-grapheme-prev.c
       uc-gbrk-prop.c
       u-grapheme-breaks.h
       u-grapheme-next.h
       u-grapheme-prev.h
       gbrkprop.h
)

for file in "${UNIGBRK_FILES[@]}"; do
       cp "lib/unigbrk/${file}" "${INTERNAL_SRC_DIR}/unigbrk/"
done

UNICTYPE_FILES=(
       bitmap.h
       pr_extended_pictographic.c
       pr_extended_pictographic.h
       incb_of.c
       incb_of.h
)

for file in "${UNICTYPE_FILES[@]}"; do
       cp "lib/unictype/${file}" "${INTERNAL_SRC_DIR}/unictype/"
done

UNICASE_FILES=(
       simple-mapping.h
       tolower.h
       toupper.h
       tolower.c
       toupper.c
)

for file in "${UNICASE_FILES[@]}"; do
       cp "lib/unicase/${file}" "${INTERNAL_SRC_DIR}/unicase/"
done

# attribute.h is needed by u8-uctomb-aux.c for the FALLTHROUGH macro
cp "lib/attribute.h" "${INTERNAL_SRC_DIR}/"

# uninorm.h stub: unicase.h includes it for uninorm_t type used by full case
# mapping functions (u8_tolower etc.) which we don't compile. Only uc_tolower/
# uc_toupper (code-point level) are needed. Provide a minimal typedef stub.
cat << 'EOF' > "${INTERNAL_SRC_DIR}/uninorm.h"
#ifndef _UNINORM_H
#define _UNINORM_H
/* Minimal stub: only the typedef needed by unicase.h declarations. */
typedef const void *uninorm_t;
#endif
EOF

# 3. 极简构建脚本：仅编译 rune / grapheme 最小子集
cat << 'EOF' > "${TARGET_DIR}/Makefile"
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./src -I./src/lib

TARGET ?= libfeng_std_unistring.a
HOST_OS :=   $(shell sh -c 'os=$$(uname -s); if [ "$$os" = Darwin ]; then printf macos; elif [ "$$os" = Linux ]; then printf linux; else printf %s "$$os" | tr "[:upper:]" "[:lower:]"; fi')
HOST_ARCH := $(if $(filter x86_64 amd64,$(shell uname -m)),x64,$(if $(filter arm64 aarch64,$(shell uname -m)),arm64,$(shell uname -m)))
OUTPUT_DIR ?= ../../std/extlib/$(HOST_OS)-$(HOST_ARCH)
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

SRCS = src/lib/unistr/u8-mbtouc-aux.c \
                      src/lib/unistr/u8-mbtoucr.c \
                      src/lib/unistr/u8-mbsnlen.c \
                      src/lib/unistr/u8-strmbtouc.c \
                      src/lib/unistr/u8-next.c \
                      src/lib/unistr/u8-prev.c \
                      src/lib/unistr/u8-uctomb-aux.c \
                      src/lib/unigbrk/u8-grapheme-breaks.c \
                      src/lib/unigbrk/u8-grapheme-next.c \
                      src/lib/unigbrk/u8-grapheme-prev.c \
                      src/lib/unigbrk/uc-gbrk-prop.c \
                      src/lib/unictype/pr_extended_pictographic.c \
                      src/lib/unictype/incb_of.c \
                      src/lib/unicase/tolower.c \
                      src/lib/unicase/toupper.c

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

cp "COPYING" "${TARGET_DIR}/"
cp "COPYING.LIB" "${TARGET_DIR}/"

# ==============================================================================
# 🧹 物理痕迹擦除
# ==============================================================================
echo "🧹 正在清理 temp/ 目录..."
rm -rf "${TMP_ROOT}"

echo "========================================================"
echo "🎉 提取完美收工！"
echo "👉 已导出 UTF-8 rune / grapheme / unicase 最小头文件与源码闭包"
echo "👉 可执行: ${PROJECT_ROOT}/scripts/build_libunistring.sh"
echo "========================================================"