#!/usr/bin/env bash
set -e

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
./configure --disable-shared --disable-rpath --without-libiconv-prefix > /dev/null

# 本仓库当前只保留 UTF-8 rune / grapheme 统计与遍历所需的最小子集。
GENERATED_HEADERS=(
       unitypes.h
       unistr.h
       unigbrk.h
       unictype.h
)

make -C "lib" \
       "${GENERATED_HEADERS[@]}" \
       unistring/stdint.h \
       unistring/woe32dll.h > /dev/null

# ------------------------------------------------------------------------------
# 🚚 [4/4] 提取 rune / grapheme 最小公开头与源码闭包
# ------------------------------------------------------------------------------
echo "🚚 正在导出 rune / grapheme 最小闭包到 third_party..."

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

This directory vendors only the UTF-8 rune and grapheme subset needed by Feng.

Public headers:
- include/unitypes.h
- include/feng_u8_rune.h
- include/feng_u8_grapheme.h

Supported operations:
- UTF-8 rune count: u8_mbsnlen
- UTF-8 rune traversal: u8_next, u8_prev
- UTF-8 grapheme traversal: u8_grapheme_next, u8_grapheme_prev
- UTF-8 grapheme boundary map: u8_grapheme_breaks

Build:
- make
EOF

# 2. 内部配置、完整内部头与最小源码闭包
cp "config.h" "${TARGET_DIR}/src/"

cp "lib/unitypes.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unistr.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unigbrk.h" "${INTERNAL_SRC_DIR}/"
cp "lib/unictype.h" "${INTERNAL_SRC_DIR}/"
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

# 3. 极简构建脚本：仅编译 rune / grapheme 最小子集
cat << 'EOF' > "${TARGET_DIR}/Makefile"
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./src -I./src/lib

TARGET = libfeng_u8_text.a

SRCS = src/lib/unistr/u8-mbtouc-aux.c \
                      src/lib/unistr/u8-mbtoucr.c \
                      src/lib/unistr/u8-mbsnlen.c \
                      src/lib/unistr/u8-strmbtouc.c \
                      src/lib/unistr/u8-next.c \
                      src/lib/unistr/u8-prev.c \
                      src/lib/unigbrk/u8-grapheme-breaks.c \
                      src/lib/unigbrk/u8-grapheme-next.c \
                      src/lib/unigbrk/u8-grapheme-prev.c \
                      src/lib/unigbrk/uc-gbrk-prop.c \
                      src/lib/unictype/pr_extended_pictographic.c \
                      src/lib/unictype/incb_of.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
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
echo "👉 已导出 UTF-8 rune / grapheme 最小头文件与源码闭包"
echo "👉 可进入目录执行: cd third_party/libunistring && make"
echo "========================================================"