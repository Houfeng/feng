#!/usr/bin/env bash
set -euo pipefail

# Download and extract prebuilt musl-based Linux cross toolchains from musl.cc
# into temp/musl/ for consumption by trim_musl.sh. This is a maintenance
# script — not part of the release build flow.
#
# musl.cc ships complete cross toolchains (GCC + binutils + musl). Feng only
# needs the sysroot portion (C library headers + static libs) for clang-based
# cross compilation. This script downloads the full prebuilt and caches it;
# trim_musl.sh extracts only the sysroot component into toolchain/sysroot/.
#
# musl.cc cross package layout:
#   <target>-linux-musl-cross/
#   ├── usr -> .                 # symlink to root (so usr/include == include)
#   ├── include/                 # empty (GCC placeholder, not the C library)
#   ├── lib/                     # GCC support libs (bfd-plugins, gcc/)
#   ├── bin/                     # cross GCC executables
#   ├── libexec/gcc/...          # GCC internal tools
#   ├── <musl-triple>/           # the actual sysroot (e.g. aarch64-linux-musl)
#   │   ├── include/             # musl public headers (stdio.h, stdlib.h, ...)
#   │   └── lib/                 # musl static libs + crt objects (libc.a, crt1.o, ...)
#   └── ...
#
# Cache behaviour: the archive and extracted tree live under
# ${PROJECT_ROOT}/temp/musl/ (gitignored). Re-runs skip the download when
# the archive is already present and valid, and skip extraction when the
# extracted root is already present. Delete the cache to force a re-download.
#
# musl.cc is a community-hosted server with intermittent connection drops.
# The download loop uses curl --continue-at - to resume partial downloads,
# retrying up to 10 times. If the server is unreachable, you can download
# the archives manually (e.g. via browser) and place them in temp/musl/;
# this script will detect the existing archive and skip to extraction.
#
# Resource: https://musl.cc/
# musl is MIT-licensed; the prebuilt toolchains are freely distributable.
# See https://musl.libc.org/ for license details.
#
# Targets fetched: linux-x64, linux-arm64.
# The script runs on macOS and Linux without modification.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MUSL_SRC_URL="${MUSL_SRC_URL:-https://musl.cc/}"

CACHE_DIR="${PROJECT_ROOT}/temp/musl"

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd curl
require_cmd tar

# --- Target matrix ---
# Each entry: <feng-target>|<musl.cc-archive>|<extracted-root-name>|<musl-triple>
# The musl-triple is the sysroot subdirectory name inside the extracted root
# (e.g. aarch64-linux-musl). The C library headers and static libs live under
# <root>/<musl-triple>/{include,lib}, not at the top level.
TARGETS=(
  "linux-arm64|aarch64-linux-musl-cross.tgz|aarch64-linux-musl-cross|aarch64-linux-musl"
  "linux-x64|x86_64-linux-musl-cross.tgz|x86_64-linux-musl-cross|x86_64-linux-musl"
)

# Download and extract one target into cache.
# Args: $1 = Feng target triple (e.g. linux-arm64)
#       $2 = musl.cc archive name (e.g. aarch64-linux-musl-cross.tgz)
#       $3 = extracted root dir name (e.g. aarch64-linux-musl-cross)
#       $4 = musl triple (sysroot subdirectory inside extracted root)
fetch_one() {
  local target="$1"
  local archive_name="$2"
  local extracted_root_name="$3"
  local musl_triple="$4"
  local src_url="${MUSL_SRC_URL}${archive_name}"
  local archive_file="${CACHE_DIR}/${archive_name}"
  local extracted_root="${CACHE_DIR}/${extracted_root_name}"
  local sysroot_src="${extracted_root}/${musl_triple}"

  echo "==> musl Linux prebuilt for ${target}"
  echo "==> Source: ${src_url}"
  echo "==> Cache:  ${CACHE_DIR}"

  mkdir -p "${CACHE_DIR}"

  if [[ -d "${extracted_root}" ]]; then
    echo "==> Reusing extracted musl at ${extracted_root}"
    return 0
  fi

  # --- Download (with cache + resume) ---
  download_ok=0
  for attempt in $(seq 1 10); do
    if [[ -f "${archive_file}" ]]; then
      # Verify existing archive integrity. If valid, skip download.
      if gzip -t "${archive_file}" 2>/dev/null; then
        echo "==> Cached archive is valid, skipping download"
        download_ok=1
        break
      fi
      # Partial/corrupt archive — resume from where it left off.
      local have_bytes
      have_bytes=$(wc -c < "${archive_file}" 2>/dev/null || echo 0)
      echo "==> Resuming download (attempt ${attempt}, have ${have_bytes} bytes)"
      curl -sSLf --connect-timeout 15 --max-time 300 -C - "${src_url}" -o "${archive_file}" || {
        echo "==> Transfer interrupted (attempt ${attempt}), will retry..."
        sleep 2
        continue
      }
    else
      echo "==> Downloading ${archive_name} (attempt ${attempt})"
      curl -sSLf --connect-timeout 15 --max-time 300 "${src_url}" -o "${archive_file}" || {
        echo "==> Transfer failed (attempt ${attempt}), will retry..."
        sleep 2
        continue
      }
    fi
    # Verify the download completed successfully by testing gzip integrity.
    if gzip -t "${archive_file}" 2>/dev/null; then
      download_ok=1
      break
    fi
    echo "==> Download incomplete (gzip integrity check failed), retrying..."
    sleep 2
  done

  if [[ "${download_ok}" -ne 1 ]]; then
    echo "error: failed to download ${archive_name} after 10 attempts" >&2
    echo "       You can download manually from: ${src_url}" >&2
    echo "       Place the file at: ${archive_file}" >&2
    return 1
  fi

  echo "==> Extracting"
  tar -xzf "${archive_file}" -C "${CACHE_DIR}"

  if [[ ! -d "${extracted_root}" ]]; then
    echo "error: extraction did not produce expected dir: ${extracted_root}" >&2
    return 1
  fi

  # Verify the extracted root has the expected sysroot markers.
  # The C library headers and static libs live under <root>/<musl-triple>/.
  if [[ ! -d "${sysroot_src}/include" ]]; then
    echo "error: ${musl_triple}/include/ missing at extracted root — content looks wrong" >&2
    return 1
  fi
  if [[ ! -d "${sysroot_src}/lib" ]]; then
    echo "error: ${musl_triple}/lib/ missing at extracted root — content looks wrong" >&2
    return 1
  fi
}

for entry in "${TARGETS[@]}"; do
  IFS='|' read -r target archive_name extracted_root_name musl_triple <<<"${entry}"
  fetch_one "${target}" "${archive_name}" "${extracted_root_name}" "${musl_triple}"
  echo
done

echo "==> Done. All musl prebuilts cached at ${CACHE_DIR}"
echo "==> Next: scripts/trim_musl.sh"
