#!/usr/bin/env bash
set -euo pipefail

# Fetch and trim prebuilt musl-based Linux sysroots from musl.cc into
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
# Targets fetched: linux-x64, linux-arm64.
# The script runs on macOS and Linux without modification.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# musl.cc prebuilt versions are not versioned by tag — the archive name is
# fixed but the upstream content was last built 2021-11-23. Pin the source
# URL explicitly so re-runs are reproducible. Override via MUSL_SRC_URL when
# a pinned/mirrored copy is desired.
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
require_cmd cp

# Fetch (download + extract with cache) for a single target.
# Args: $1 = Feng target triple (e.g. linux-arm64)
#       $2 = musl.cc archive name (e.g. aarch64-linux-musl-cross.tgz)
#       $3 = extracted root dir name (e.g. aarch64-linux-musl-cross)
fetch_one() {
  local target="$1"
  local archive_name="$2"
  local extracted_root_name="$3"
  local src_url="${MUSL_SRC_URL}${archive_name}"
  local archive_file="${CACHE_DIR}/${archive_name}"
  local extracted_root="${CACHE_DIR}/${extracted_root_name}"

  echo "==> musl Linux sysroot for ${target}"
  echo "==> Source: ${src_url}"
  echo "==> Cache:  ${CACHE_DIR}"

  mkdir -p "${CACHE_DIR}"

  # --- Download (with cache + resume) ---
  # musl.cc is a community-hosted server with intermittent connection drops.
  # Use curl --continue-at - to resume partial downloads. The loop retries
  # up to 10 times, resuming from where the previous attempt left off.
  if [[ -d "${extracted_root}" ]]; then
    echo "==> Reusing extracted musl at ${extracted_root}"
  else
    download_ok=0
    for attempt in $(seq 1 10); do
      if [[ -f "${archive_file}" ]]; then
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
      rm -f "${archive_file}"
      return 1
    fi
    echo "==> Extracting"
    tar -xzf "${archive_file}" -C "${CACHE_DIR}"
  fi

  if [[ ! -d "${extracted_root}" ]]; then
    echo "error: extraction did not produce expected dir: ${extracted_root}" >&2
    return 1
  fi

  # Verify the extracted root has the expected sysroot markers.
  if [[ ! -d "${extracted_root}/include" ]]; then
    echo "error: include/ missing at extracted root — content looks wrong" >&2
    return 1
  fi
  if [[ ! -d "${extracted_root}/lib" ]]; then
    echo "error: lib/ missing at extracted root — content looks wrong" >&2
    return 1
  fi
}

# Trim into toolchain/sysroot/<os>-<arch>/.
# musl.cc layout: <root>/ is itself the sysroot root, with usr -> . symlink.
# We re-materialize the standard --sysroot convention: usr/include + usr/lib.
# Only the C library sysroot material is copied; GCC toolchain binaries
# (bin/, libexec/) and GCC private headers are excluded.
# Only this target's dir is touched — other tools' trees under toolchain/ are
# preserved.
# Args: $1 = Feng target triple (e.g. linux-arm64)
#       $2 = musl.cc archive name (for README provenance)
#       $3 = extracted root dir name (cache path)
trim_one() {
  local target="$1"
  local archive_name="$2"
  local extracted_root_name="$3"
  local extracted_root="${CACHE_DIR}/${extracted_root_name}"
  local sysroot_target_dir="${PROJECT_ROOT}/toolchain/sysroot/${target}"

  echo "==> Trimming sysroot into ${sysroot_target_dir}"

  rm -rf "${sysroot_target_dir}"
  mkdir -p "${sysroot_target_dir}/usr"

  # Copy include/ -> usr/include/
  # musl public headers: stdio.h, stdlib.h, string.h, unistd.h, etc.
  echo "==> Copying musl headers (usr/include/)"
  cp -R "${extracted_root}/include" "${sysroot_target_dir}/usr/include"

  # Copy lib/ -> usr/lib/
  # musl static libraries + crt objects: libc.a, libc.o, crt1.o, crti.o,
  # crtn.o, rcrt1.o, Scrt1.o, etc. These are needed by clang at link time.
  echo "==> Copying musl static libs (usr/lib/)"
  cp -R "${extracted_root}/lib" "${sysroot_target_dir}/usr/lib"

  # musl.cc archives do not ship a top-level LICENSE file; the musl source
  # license (MIT) is documented at https://musl.libc.org/. Record provenance
  # in README.md below.
  cat > "${sysroot_target_dir}/README.md" <<EOF
# musl Linux sysroot (cross compilation)

This directory vendors a prebuilt musl libc sysroot for Feng's cross
compilation support on Linux targets.

Target:  ${target}
Source:  https://musl.cc/${archive_name}
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

The feng compiler passes \`--sysroot=<install>/toolchain/sysroot/${target}/\`
to clang when cross-compiling to this target. No additional \`-I\` or \`-L\`
flags are needed — the standard \`usr/include\` + \`usr/lib\` layout is
recognized by clang automatically.

Note: musl is used for cross compilation sysroot only. Native Linux builds
use the host system's glibc. See dev/feng-release-and-instanll.md §5/§9 for
the rationale.

Re-sync:
- Run \`./scripts/fetch_musl.sh\` to (re)fetch both targets.
- Delete \`temp/musl/\` to force a re-download.
EOF

  echo "==> Done. musl sysroot for ${target} at:"
  echo "    ${sysroot_target_dir}"
  echo "==> Verify: ls ${sysroot_target_dir}/usr/include/stdio.h"
  echo "==> Verify: ls ${sysroot_target_dir}/usr/lib/libc.a"
}

# --- Target matrix ---
# Each entry: <feng-target>|<musl.cc-archive>|<extracted-root-name>
TARGETS=(
  "linux-arm64|aarch64-linux-musl-cross.tgz|aarch64-linux-musl-cross"
  "linux-x64|x86_64-linux-musl-cross.tgz|x86_64-linux-musl-cross"
)

for entry in "${TARGETS[@]}"; do
  IFS='|' read -r target archive_name extracted_root_name <<<"${entry}"
  fetch_one "${target}" "${archive_name}" "${extracted_root_name}"
  trim_one  "${target}" "${archive_name}" "${extracted_root_name}"
  echo
done
