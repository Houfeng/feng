#!/usr/bin/env bash
set -e

# ==============================================================================
# 🎯 物理路径自适应定位
# ==============================================================================
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "${SCRIPT_DIR}/.." && pwd )"
TARGET_DIR="${PROJECT_ROOT}/third_party/mimalloc"

echo "========================================================"
echo "🚀 启动 mimalloc 静态库源码提取程序"
echo "⚙️  项目根目录: ${PROJECT_ROOT}"
echo "🎯 目标存储目录: ${TARGET_DIR}"
echo "========================================================"

# 清理并重建目标物理现场
rm -rf "${TARGET_DIR}"
mkdir -p "${TARGET_DIR}/include"
mkdir -p "${TARGET_DIR}/src"

# 将临时工作区显式指定到项目根目录下的 temp/ 目录中
TMP_ROOT="${PROJECT_ROOT}/temp"
TMP_DIR="${TMP_ROOT}/mimalloc_tmp"

rm -rf "${TMP_ROOT}"
mkdir -p "${TMP_DIR}"

# 🎯 下载微软官方目前最稳定的 v2.1.2 源码包
SRC_URL="https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.2.tar.gz"
TAR_FILE="${TMP_ROOT}/mimalloc-2.1.2.tar.gz"

echo "📦 [1/4] 正在从 Microsoft 官方 GitHub 下载 mimalloc 源码归档..."
curl -s -L -f --connect-timeout 15 "${SRC_URL}" -o "${TAR_FILE}"

echo "📦 [2/4] 正在解压源码包..."
tar -xf "${TAR_FILE}" -C "${TMP_DIR}"
EXTRACTED_SRC="${TMP_DIR}/mimalloc-2.1.2"

# ------------------------------------------------------------------------------
# 🚚 [3/4] 提取核心 C 源码与公共头文件
# ------------------------------------------------------------------------------
echo "🚚 正在精准提取核心源文件到 third_party..."

# 1. 搬运公共全局 API 头文件
cp -r "${EXTRACTED_SRC}/include/" "${TARGET_DIR}/include/"

# 2. 搬运 mimalloc 标志性的 static-init 核心实现
# mimalloc 官方支持将所有源码通过 mimalloc-alloc.c 级联引入，极其适合单体编译
cp -r "${EXTRACTED_SRC}/src/" "${TARGET_DIR}/src/"

# ------------------------------------------------------------------------------
# 📝 [4/4] 原地为 third_party/mimalloc 写入一个极致干净的标准 Makefile
# ------------------------------------------------------------------------------
echo "📝 正在生成标准 Makefile 编译脚本..."
cat << 'EOF' > "${TARGET_DIR}/Makefile"
CC ?= gcc
AR ?= ar
# -O3: 开启极致优化
# -DMI_MALLOC_OVERRIDE: 允许作为全局替换默认 malloc
CFLAGS ?= -O3 -Wall -I./include -DMI_MALLOC_OVERRIDE

TARGET = libmimalloc.a

# mimalloc 官方推荐的单文件单体编译模式，能够让编译器做最极致的内联优化（LTO级性能）
SRCS = src/static.c
OBJS = src/static.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

src/static.o: src/static.c
	$(CC) $(CFLAGS) -c $< -o $@

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
echo "👉 请进入目录执行: cd third_party/mimalloc && make"
echo "========================================================"