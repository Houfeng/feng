#!/usr/bin/env bash
set -euo pipefail

# Fetch and trim a prebuilt musl-based Linux sysroot from musl.cc into
# toolchain/sysroot/<os>-<arch>/. This is a maintenance script — not part of
# the release build flow.
#
# Unlike fetch_llvm.sh (which only caches the raw LLVM prebuilt for
# trim_clang.sh / trim_lldb.sh to consume separately), this script combines
# fetch + trim into one step: musl.cc's prebuilt is consumed by a single
# trim target (the sysroot), so there is no need to split the two stages.
#
# musl.cc ships complete cross toolchains (GCC + binutils + musl). Feng only
# needs the sysroot portion (C library headers + static libs) for clang-based
# cross compilation. This script downloads the prebuilt, extracts only the
# sysroot component, and discards the GCC toolchain material.
#
# musl.cc cross package layout:
#   <target>-linux-musl-cross/
#   ├── usr -> .                 # symlink to root (so usr/include == include)
#   ├── include/                 # musl public headers (== usr/include)
#   ├── lib/                     # musl static libs + crt objects (== usr/lib)
#   ├── bin/                     # cross GCC executables      (excluded)
#   ├── libexec/gcc/...          # GCC internal tools          (excluded)
#   └── ...
#
# Cache behaviour: the archive and extracted tree live under
# ${PROJECT_ROOT}/temp/musl/ (gitignored). Re-runs skip the download when
# the archive is already present, and skip extraction when the extracted
# root is already present. Delete the cache to force a re-download.
#
# Resource: https://musl.cc/
# musl is MIT-licensed; the prebuilt toolchains are freely distributable.
# See https://musl.libc.org/ for license details.
#
# Supported targets: linux-x64, linux-arm64.
# macOS is intentionally unsupported here — Apple SDK is not freely
# redistributable and must be obtained by the user separately.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# musl.cc prebuilt versions are not versioned by tag — the archive name is
# fixed but the upstream content was last built 2021-11-23. Pin the source
# URL explicitly so re-runs are reproducible. Override via MUSL_SRC_URL when
# a pinned/mirrored copy is desired.
MUSL_SRC_URL_DEFAULT="https://musl.cc/"
MUSL_SRC_URL="${MUSL_SRC_URL:-${MUSL_SRC_URL_DEFAULT}}"

detect_target() {
  local os arch uname_s uname_m
  uname_s="$(uname -s)"
  uname_m="$(uname -m)"
  case "${uname_s}" in
    Darwin) os=macos ;;
    Linux)  os=linux ;;
    *) echo "error: unsupported OS for host detection: ${uname_s}" >&2; exit 1 ;;
  esac
  case "${uname_m}" in
    arm64|aarch64) arch=arm64 ;;
    x86_64|amd64)  arch=x64 ;;
    *) echo "error: unsupported arch for host detection: ${uname_m}" >&2; exit 1 ;;
  esac
  printf '%s-%s' "${os}" "${arch}"
}

# Only Linux targets make sense for a musl sysroot. macOS/Windows targets
# are rejected — macOS SDK is Apple-restricted, Windows uses mingw-w64
# (a separate fetch path not covered here).
TARGET="${TARGET:-$(detect_target)}"

# Map a Feng target triple (<os>-<arch>) to the musl.cc archive name.
# musl.cc uses the full target triple in the archive basename.
target_to_musl_archive() {
  case "$1" in
    linux-x64)   printf 'x86_64-linux-musl-cross.tgz' ;;
    linux-arm64) printf 'aarch64-linux-musl-cross.tgz' ;;
    macos-*|windows-*)
      echo "error: musl sysroot fetch does not support target: $1" >&2
      echo "       macOS SDK is Apple-restricted; Windows uses mingw-w64 (separate path)." >&2
      exit 1
      ;;
    *) echo "error: no musl prebuilt mapping for target: $1" >&2; exit 1 ;;
  esac
}

# The extracted root dir name inside the archive (musl.cc convention).
target_to_extracted_root() {
  case "$1" in
    linux-x64)   printf 'x86_64-linux-musl-cross' ;;
    linux-arm64) printf 'aarch64-linux-musl-cross' ;;
  esac
}

MUSL_ARCHIVE_NAME="$(target_to_musl_archive "${TARGET}")"
MUSL_EXTRACTED_ROOT_NAME="$(target_to_extracted_root "${TARGET}")"
MUSL_SRC_URL="${MUSL_SRC_URL}${MUSL_ARCHIVE_NAME}"

CACHE_DIR="${PROJECT_ROOT}/temp/musl"
ARCHIVE_FILE="${CACHE_DIR}/${MUSL_ARCHIVE_NAME}"
EXTRACTED_ROOT="${CACHE_DIR}/${MUSL_EXTRACTED_ROOT_NAME}"

SYSROOT_TARGET_DIR="${PROJECT_ROOT}/toolchain/sysroot/${TARGET}"

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd curl
require_cmd tar
require_cmd cp

echo "==> musl Linux sysroot for ${TARGET}"
echo "==> Source: ${MUSL_SRC_URL}"
echo "==> Cache:  ${CACHE_DIR}"

mkdir -p "${CACHE_DIR}"

# --- Download (with cache) ---
if [[ -d "${EXTRACTED_ROOT}" ]]; then
  echo "==> Reusing extracted musl at ${EXTRACTED_ROOT}"
elif [[ -f "${ARCHIVE_FILE}" ]]; then
  echo "==> Reusing cached archive ${ARCHIVE_FILE}"
  echo "==> Extracting"
  tar -xzf "${ARCHIVE_FILE}" -C "${CACHE_DIR}"
else
  echo "==> Downloading ${MUSL_ARCHIVE_NAME}"
  curl -sSLf --connect-timeout 15 --retry 3 "${MUSL_SRC_URL}" -o "${ARCHIVE_FILE}"
  echo "==> Extracting"
  tar -xzf "${ARCHIVE_FILE}" -C "${CACHE_DIR}"
fi

if [[ ! -d "${EXTRACTED_ROOT}" ]]; then
  echo "error: extraction did not produce expected dir: ${EXTRACTED_ROOT}" >&2
  exit 1
fi

# Verify the extracted root has the expected sysroot markers.
if [[ ! -d "${EXTRACTED_ROOT}/include" ]]; then
  echo "error: include/ missing at extracted root — content looks wrong" >&2
  exit 1
fi
if [[ ! -d "${EXTRACTED_ROOT}/lib" ]]; then
  echo "error: lib/ missing at extracted root — content looks wrong" >&2
  exit 1
fi

# --- Trim into toolchain/sysroot/<os>-<arch>/ ---
# musl.cc layout: <root>/ is itself the sysroot root, with usr -> . symlink.
# We re-materialize the standard --sysroot convention: usr/include + usr/lib.
# Only the C library sysroot material is copied; GCC toolchain binaries
# (bin/, libexec/) and GCC private headers are excluded.
#
# Only this target's dir is touched — other tools' trees under toolchain/ are
# preserved.
echo "==> Trimming sysroot into ${SYSROOT_TARGET_DIR}"

rm -rf "${SYSROOT_TARGET_DIR}"
mkdir -p "${SYSROOT_TARGET_DIR}/usr"

# Copy include/ -> usr/include/
# musl public headers: stdio.h, stdlib.h, string.h, unistd.h, etc.
echo "==> Copying musl headers (usr/include/)"
cp -R "${EXTRACTED_ROOT}/include" "${SYSROOT_TARGET_DIR}/usr/include"

# Copy lib/ -> usr/lib/
# musl static libraries + crt objects: libc.a, libc.o, crt1.o, crti.o,
# crtn.o, rcrt1.o, Scrt1.o, etc. These are needed by clang at link time.
echo "==> Copying musl static libs (usr/lib/)"
cp -R "${EXTRACTED_ROOT}/lib" "${SYSROOT_TARGET_DIR}/usr/lib"

# musl.cc archives do not ship a top-level LICENSE file; the musl source
# license (MIT) is documented at https://musl.libc.org/. Record provenance
# in README.md below.

cat > "${SYSROOT_TARGET_DIR}/README.md" <<EOF
# musl Linux sysroot (cross compilation)

This directory vendors a prebuilt musl libc sysroot for Feng's cross
compilation support on Linux targets.

Target:  ${TARGET}
Source:  https://musl.cc/${MUSL_ARCHIVE_NAME}
License: MIT (https://musl.libc.org/)

Included:
- \`usr/include/\` — musl public C library headers (stdio.h, stdlib.h,
  string.h, unistd.h, pthread.h, etc.)
- \`usr/lib/\` — musl static libraries and crt objects (libc.a, crt1.o,
  crti.o, crtn.o, rcrt1.o, Scrt1.o, etc.)

Deliberately excluded:
- GCC cross-compiler binaries (\`bin/\`) — Feng uses the bundled clang
- GCC internal tools (\`libexec/gcc/\`) — not needed by clang
- GCC private headers — clang provides its own resource-dir headers

The feng compiler passes \`--sysroot=<install>/toolchain/sysroot/${TARGET}/\`
to clang when cross-compiling to this target. No additional \`-I\` or \`-L\`
flags are needed — the standard \`usr/include\` + \`usr/lib\` layout is
recognized by clang automatically.

Note: musl is used for cross compilation sysroot only. Native Linux builds
use the host system's glibc. See dev/feng-release-and-instanll.md §5/§9 for
the rationale.

Re-sync:
- \`TARGET=linux-x64 ./scripts/fetch_musl.sh\` to (re)fetch x86_64 sysroot.
- \`TARGET=linux-arm64 ./scripts/fetch_musl.sh\` to (re)fetch aarch64 sysroot.
- Delete \`temp/musl/\` to force a re-download.
EOF

echo "==> Done. musl sysroot for ${TARGET} at:"
echo "    ${SYSROOT_TARGET_DIR}"
echo "==> Verify: ls ${SYSROOT_TARGET_DIR}/usr/include/stdio.h"
echo "==> Verify: ls ${SYSROOT_TARGET_DIR}/usr/lib/libc.a"
