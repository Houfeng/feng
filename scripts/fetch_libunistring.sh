#!/usr/bin/env bash
set -e

# ==============================================================================
# 🎯 物理路径自适应定位
# ==============================================================================
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "${SCRIPT_DIR}/.." && pwd )"
TARGET_DIR="${PROJECT_ROOT}/third_party/libunistring"

echo "========================================================"
echo "🚀 启动 libunistring 静态库源码提取程序"
echo "⚙️  项目根目录: ${PROJECT_ROOT}"
echo "🎯 目标存储目录: ${TARGET_DIR}"
echo "========================================================"

# 清理并重建目标物理现场
rm -rf "${TARGET_DIR}"
mkdir -p "${TARGET_DIR}/include"
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

# ------------------------------------------------------------------------------
# 🚚 [4/4] 提取已经生成完毕的、正统的 C 源码与头文件
# ------------------------------------------------------------------------------
echo "🚚 正在精准提取正统骨骼文件到 third_party..."

# 1. 搬运已经由 configure 正常生成的标准头文件 (不再是 .in.h)
cp "lib/unitypes.h" "${TARGET_DIR}/include/"
cp "lib/unistr.h"   "${TARGET_DIR}/include/"
cp "lib/uninorm.h"  "${TARGET_DIR}/include/"
cp "lib/unicase.h"  "${TARGET_DIR}/include/"

# 2. 搬运正常生成的编译配置环境
cp "config.h"       "${TARGET_DIR}/src/"

# 3. 搬运核心物理实现 (.c 文件)
cp "lib/unistr/u8-mbtowc.c"      "${TARGET_DIR}/src/"
cp "lib/unistr/u8-mbtowc-aux.c"  "${TARGET_DIR}/src/"
cp "lib/unistr/u8-next.c"        "${TARGET_DIR}/src/"
cp "lib/unistr/u8-prev.c"        "${TARGET_DIR}/src/"
cp "lib/unistr/u8-strlen.c"      "${TARGET_DIR}/src/"
cp "lib/unistr/u8-cpy.c"         "${TARGET_DIR}/src/"
cp "lib/uninorm/normalize.c"     "${TARGET_DIR}/src/"
cp "lib/uninorm/u8-normalize.c"  "${TARGET_DIR}/src/"
cp "lib/unicase/u8-tolower.c"    "${TARGET_DIR}/src/"
cp "lib/unicase/u8-toupper.c"    "${TARGET_DIR}/src/"

# ------------------------------------------------------------------------------
# 📝 原地为 third_party/libunistring 写入一个极其干净的标准 Makefile
# ------------------------------------------------------------------------------
echo "📝 正在生成标准 Makefile 编译脚本..."
cat << 'EOF' > "${TARGET_DIR}/Makefile"
CC ?= gcc
AR ?= ar
CFLAGS ?= -O2 -Wall -I./include -I./src

# 核心目标静态库
TARGET = libunistring.a

# 所有的源文件
SRCS = src/u8-mbtowc.c \
       src/u8-mbtowc-aux.c \
       src/u8-next.c \
       src/u8-prev.c \
       src/u8-strlen.c \
       src/u8-cpy.c \
       src/normalize.c \
       src/u8-normalize.c \
       src/u8-tolower.c \
       src/u8-toupper.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -DHAVE_CONFIG_H -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
EOF

# ==============================================================================
# 🧹 物理痕迹擦除
# ==============================================================================
echo "🧹 正在清理 temp/ 目录..."
rm -rf "${TMP_ROOT}"

echo "========================================================"
echo "🎉 提取完美收工！"
echo "👉 请进入目录执行: cd third_party/libunistring && make"
echo "========================================================"