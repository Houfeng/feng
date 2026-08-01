#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/libuv"
INCLUDE_DIR="${TARGET_DIR}/include"
SRC_DIR="${TARGET_DIR}/src"
TMP_ROOT="${PROJECT_ROOT}/temp"
TMP_DIR="${TMP_ROOT}/libuv_tmp"

LIBUV_VERSION="${LIBUV_VERSION:-1.49.2}"
LIBUV_TAG="v${LIBUV_VERSION}"
SRC_URL="${LIBUV_SRC_URL:-https://dist.libuv.org/dist/${LIBUV_TAG}/libuv-${LIBUV_TAG}.tar.gz}"
TAR_FILE="${TMP_ROOT}/libuv-${LIBUV_TAG}.tar.gz"
EXTRACTED_SRC="${TMP_DIR}/libuv-${LIBUV_TAG}"

contains() {
  local needle="$1"
  shift
  local value
  for value in "$@"; do
    if [[ "${value}" == "${needle}" ]]; then
      return 0
    fi
  done
  return 1
}

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

  mkdir -p "$(dirname "${target_path}")"
  cp "${source_path}" "${target_path}"
}

canonicalize_rel_under_root() {
  local root="$1"
  local rel_path="$2"
  local abs_path

  abs_path="$(cd "${root}" && cd "$(dirname "${rel_path}")" 2>/dev/null && pwd -P)/$(basename "${rel_path}")"
  if [[ ! -f "${abs_path}" ]]; then
    return 1
  fi

  case "${abs_path}" in
    "${root}"/*)
      printf '%s\n' "${abs_path#"${root}/"}"
      ;;
    *)
      return 1
      ;;
  esac
}

resolve_include_rel() {
  local current_rel="$1"
  local include_rel="$2"
  local root="$3"
  local current_dir
  local candidate
  local resolved

  current_dir="$(dirname "${current_rel}")"
  candidate="${current_dir}/${include_rel}"
  if resolved="$(canonicalize_rel_under_root "${root}" "${candidate}" 2>/dev/null)"; then
    printf '%s\n' "${resolved}"
    return 0
  fi

  candidate="${include_rel}"
  if resolved="$(canonicalize_rel_under_root "${root}" "${candidate}" 2>/dev/null)"; then
    printf '%s\n' "${resolved}"
    return 0
  fi

  return 1
}

parse_rel_includes() {
  local current_rel="$1"
  local root="$2"

  sed -nE 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"([^"]+)".*/\1/p' "${root}/${current_rel}"
}

join_make_list() {
  local output=""
  local item

  for item in "$@"; do
    if [[ -z "${output}" ]]; then
      output="${item}"
    else
      output+=" ${item}"
    fi
  done

  printf '%s' "${output}"
}

cleanup() {
  rm -rf "${TMP_ROOT}"
}

trap cleanup EXIT

require_cmd curl
require_cmd tar

COMMON_HEADERS=(uv.h)

UV_HEADERS=(
  uv/aix.h
  uv/bsd.h
  uv/darwin.h
  uv/errno.h
  uv/linux.h
  uv/os390.h
  uv/posix.h
  uv/sunos.h
  uv/threadpool.h
  uv/tree.h
  uv/unix.h
  uv/version.h
  uv/win.h
)

COMMON_SOURCES=(
  src/fs-poll.c
  src/idna.c
  src/inet.c
  src/random.c
  src/strscpy.c
  src/strtok.c
  src/thread-common.c
  src/threadpool.c
  src/timer.c
  src/uv-common.c
  src/uv-data-getter-setters.c
  src/version.c
)

UNIX_BASE_SOURCES=(
  src/unix/async.c
  src/unix/core.c
  src/unix/dl.c
  src/unix/fs.c
  src/unix/getaddrinfo.c
  src/unix/getnameinfo.c
  src/unix/loop-watcher.c
  src/unix/loop.c
  src/unix/pipe.c
  src/unix/poll.c
  src/unix/process.c
  src/unix/random-devurandom.c
  src/unix/signal.c
  src/unix/stream.c
  src/unix/tcp.c
  src/unix/thread.c
  src/unix/tty.c
  src/unix/udp.c
)

DARWIN_SOURCES=(
  src/unix/proctitle.c
  src/unix/bsd-ifaddrs.c
  src/unix/kqueue.c
  src/unix/random-getentropy.c
  src/unix/darwin-proctitle.c
  src/unix/darwin.c
  src/unix/fsevents.c
)

LINUX_SOURCES=(
  src/unix/proctitle.c
  src/unix/linux.c
  src/unix/procfs-exepath.c
  src/unix/random-getrandom.c
  src/unix/random-sysctl-linux.c
)

WINDOWS_SOURCES=(
  src/win/async.c
  src/win/core.c
  src/win/detect-wakeup.c
  src/win/dl.c
  src/win/error.c
  src/win/fs.c
  src/win/fs-event.c
  src/win/getaddrinfo.c
  src/win/getnameinfo.c
  src/win/handle.c
  src/win/loop-watcher.c
  src/win/pipe.c
  src/win/thread.c
  src/win/poll.c
  src/win/process.c
  src/win/process-stdio.c
  src/win/signal.c
  src/win/snprintf.c
  src/win/stream.c
  src/win/tcp.c
  src/win/tty.c
  src/win/udp.c
  src/win/util.c
  src/win/winapi.c
  src/win/winsock.c
)

echo "==> Syncing libuv ${LIBUV_VERSION} (darwin+linux+windows) into ${TARGET_DIR}"

rm -rf "${TARGET_DIR}" "${TMP_ROOT}"
mkdir -p "${INCLUDE_DIR}" "${SRC_DIR}" "${TMP_DIR}"

echo "==> Downloading ${SRC_URL}"
curl -sSLf --connect-timeout 15 "${SRC_URL}" -o "${TAR_FILE}"

echo "==> Extracting source archive"
tar -xf "${TAR_FILE}" -C "${TMP_DIR}"

if [[ ! -d "${EXTRACTED_SRC}" ]]; then
  echo "error: unexpected archive layout, missing ${EXTRACTED_SRC}" >&2
  exit 1
fi

echo "==> Copying public header subset"
for header_rel in "${COMMON_HEADERS[@]}" "${UV_HEADERS[@]}"; do
  copy_required "${EXTRACTED_SRC}/include/${header_rel}" "${INCLUDE_DIR}/${header_rel}"
done

echo "==> Copying minimal source seed set"
SEED_SOURCES=(
  "${COMMON_SOURCES[@]}"
  "${UNIX_BASE_SOURCES[@]}"
  "${DARWIN_SOURCES[@]}"
  "${LINUX_SOURCES[@]}"
  "${WINDOWS_SOURCES[@]}"
)
for source_rel in "${SEED_SOURCES[@]}"; do
  copy_required "${EXTRACTED_SRC}/${source_rel}" "${TARGET_DIR}/${source_rel}"
done

echo "==> Expanding internal include closure"
queue=("${SEED_SOURCES[@]}")
copied=("${SEED_SOURCES[@]}")

while [[ ${#queue[@]} -gt 0 ]]; do
  current_rel="${queue[0]}"
  queue=("${queue[@]:1}")

  while IFS= read -r include_rel; do
    [[ -z "${include_rel}" ]] && continue

    if ! resolved_rel="$(resolve_include_rel "${current_rel}" "${include_rel}" "${EXTRACTED_SRC}" 2>/dev/null)"; then
      continue
    fi

    # 头文件闭包只保留 src/ 下内容，include/ 公共头由上面的白名单管理。
    case "${resolved_rel}" in
      src/*)
        ;;
      *)
        continue
        ;;
    esac

    if contains "${resolved_rel}" "${copied[@]}"; then
      continue
    fi

    copy_required "${EXTRACTED_SRC}/${resolved_rel}" "${TARGET_DIR}/${resolved_rel}"
    copied+=("${resolved_rel}")

    case "${resolved_rel}" in
      *.h)
        queue+=("${resolved_rel}")
        ;;
      *)
        ;;
    esac
  done < <(parse_rel_includes "${current_rel}" "${EXTRACTED_SRC}")
done

echo "==> Copying license"
copy_required "${EXTRACTED_SRC}/LICENSE" "${TARGET_DIR}/LICENSE"

COMMON_SOURCES_MAKE="$(join_make_list "${COMMON_SOURCES[@]}")"
UNIX_BASE_SOURCES_MAKE="$(join_make_list "${UNIX_BASE_SOURCES[@]}")"
DARWIN_SOURCES_MAKE="$(join_make_list "${DARWIN_SOURCES[@]}")"
LINUX_SOURCES_MAKE="$(join_make_list "${LINUX_SOURCES[@]}")"
WINDOWS_SOURCES_MAKE="$(join_make_list "${WINDOWS_SOURCES[@]}")"

cat > "${TARGET_DIR}/README.md" <<EOF
# libuv minimal subset (darwin + linux + windows)

This directory vendors only the minimal libuv closure required by Feng.

Version: ${LIBUV_VERSION}
Platform closure: darwin + linux + windows

Included:
- public headers (uv.h + include/uv/*.h)
- common core sources (loop/threadpool/timer/inet/idna/random)
- unix base sources (async/poll/stream/tcp/udp/fs/process/thread/dns)
- darwin-specific sources from upstream CMake target graph
- linux-specific sources from upstream CMake target graph
- windows-specific sources from upstream CMake target graph
- recursively discovered internal src/* headers required by the selected sources

Excluded:
- tests/benchmarks/examples/docs/tools
- upstream CMake/Autotools build system

Build:
- \`make\` builds the native static archive and stages it into \`../../std/std/extlib/<host-platform>\` by default.
- \`make OUTPUT_DIR=<path>\` overrides the staging directory.
- \`make install\` is an alias of the staging step.
- default staged library name: \`libfeng_std_uv.a\`
EOF

cat > "${TARGET_DIR}/Makefile" <<EOF
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./include -I./src
UNIX_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
DARWIN_CPPFLAGS := -D_DARWIN_UNLIMITED_SELECT=1 -D_DARWIN_USE_64_BIT_INODE=1
LINUX_CPPFLAGS := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112

TARGET ?= libfeng_std_uv.a
HOST_OS := \
  \$(shell sh -c 'os=\$\$(uname -s); if [ "\$\$os" = Darwin ]; then printf macos; elif [ "\$\$os" = Linux ]; then printf linux; else printf %s "\$\$os" | tr "[:upper:]" "[:lower:]"; fi')
HOST_ARCH := \$(if \$(filter x86_64 amd64,\$(shell uname -m)),x64,\$(if \$(filter arm64 aarch64,\$(shell uname -m)),arm64,\$(shell uname -m)))
HOST_PLATFORM := \$(if \$(filter linux,\$(HOST_OS)),\$(HOST_OS)-\$(HOST_ARCH)-gnu,\$(HOST_OS)-\$(HOST_ARCH))
OUTPUT_DIR ?= ../../std/std/extlib/\$(HOST_PLATFORM)
OUTPUT_NAME ?= \$(TARGET)
OUTPUT_TARGET := \$(OUTPUT_DIR)/\$(OUTPUT_NAME)

COMMON_SRCS = ${COMMON_SOURCES_MAKE}
UNIX_BASE_SRCS = ${UNIX_BASE_SOURCES_MAKE}
DARWIN_SRCS = ${DARWIN_SOURCES_MAKE}
LINUX_SRCS = ${LINUX_SOURCES_MAKE}
WINDOWS_SRCS = ${WINDOWS_SOURCES_MAKE}

ifeq (\$(HOST_OS),macos)
  SRCS = \$(COMMON_SRCS) \$(UNIX_BASE_SRCS) \$(DARWIN_SRCS)
  CPPFLAGS += \$(UNIX_CPPFLAGS) \$(DARWIN_CPPFLAGS)
else ifeq (\$(HOST_OS),linux)
  SRCS = \$(COMMON_SRCS) \$(UNIX_BASE_SRCS) \$(LINUX_SRCS)
  CPPFLAGS += \$(UNIX_CPPFLAGS) \$(LINUX_CPPFLAGS)
else
  SRCS = \$(COMMON_SRCS) \$(WINDOWS_SRCS)
endif

OBJS = \$(SRCS:.c=.o)

all: \$(OUTPUT_TARGET)

\$(TARGET): \$(OBJS)
	\$(AR) rcs \$@ \$^

\$(OUTPUT_TARGET): \$(TARGET) ; @mkdir -p "\$(OUTPUT_DIR)" && cp "\$(TARGET)" "\$(OUTPUT_TARGET)"

install: \$(OUTPUT_TARGET)

%.o: %.c
	\$(CC) \$(CPPFLAGS) \$(CFLAGS) -c \$< -o \$@

clean: ; rm -f \$(OBJS) \$(TARGET) \$(OUTPUT_TARGET)

.PHONY: all clean install
EOF

SOURCE_COUNT="$(find "${TARGET_DIR}/src" -type f -name '*.c' | wc -l | tr -d ' ')"
HEADER_COUNT="$(find "${TARGET_DIR}" -type f -name '*.h' | wc -l | tr -d ' ')"

echo "==> Synced ${SOURCE_COUNT} source files and ${HEADER_COUNT} headers"
echo "==> libuv sync complete"
echo "==> Build with: ${PROJECT_ROOT}/scripts/build_libuv.sh"
