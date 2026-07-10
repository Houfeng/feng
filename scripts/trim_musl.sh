#!/usr/bin/env bash
set -euo pipefail

# Slim the extracted musl prebuilt into toolchain/sysroot/<os>-<arch>/.
#
# Consumes the musl prebuilt roots produced by fetch_musl.sh. Re-runs
# overwrite the target directories safely — only the sysroot dirs are
# touched, other tools' trees under toolchain/ are preserved.
#
# musl.cc layout: the C library sysroot lives under <root>/<musl-triple>/
# (e.g. aarch64-linux-musl/), with usr -> . symlink at the top level.
# We re-materialize the standard --sysroot convention: usr/include + usr/lib.
# Only the C library sysroot material is copied; GCC toolchain binaries
# (bin/, libexec/) and GCC private headers are excluded.
#
# Layout produced:
#   toolchain/sysroot/<os>-<arch>/
#     usr/include/              — musl public C library headers
#     usr/lib/                  — musl static libraries + crt objects
#     README.md                 — provenance and re-sync instructions
#
# Deliberately excluded:
# - bin/ (cross GCC executables — Feng uses the bundled clang)
# - libexec/gcc/ (GCC internal tools — not needed by clang)
# - GCC private headers (clang provides its own resource-dir headers)
#
# Feng's codegen produces C code that only needs standard C headers (stdio.h,
# stdlib.h, etc.) and the musl C library static libs / crt objects at link
# time. The full GCC cross-compiler is not needed.
#
# musl is used for cross compilation sysroot only. Native Linux builds use
# the host system's glibc. See dev/feng-release-and-instanll.md §5/§9.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CACHE_DIR="${PROJECT_ROOT}/temp/musl"

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd cp

# --- Target matrix ---
# Each entry: <feng-target>|<musl.cc-archive>|<extracted-root-name>|<musl-triple>
# The musl-triple is the sysroot subdirectory name inside the extracted root
# (e.g. aarch64-linux-musl). The C library headers and static libs live under
# <root>/<musl-triple>/{include,lib}, not at the top level.
TARGETS=(
  "linux-arm64|aarch64-linux-musl-cross.tgz|aarch64-linux-musl-cross|aarch64-linux-musl"
  "linux-x64|x86_64-linux-musl-cross.tgz|x86_64-linux-musl-cross|x86_64-linux-musl"
)

# Trim one target's sysroot from cache into toolchain/.
# Args: $1 = Feng target triple (e.g. linux-arm64)
#       $2 = musl.cc archive name (for README provenance)
#       $3 = extracted root dir name (cache path)
#       $4 = musl triple (sysroot subdirectory inside extracted root)
trim_one() {
  local target="$1"
  local archive_name="$2"
  local extracted_root_name="$3"
  local musl_triple="$4"
  local extracted_root="${CACHE_DIR}/${extracted_root_name}"
  local sysroot_src="${extracted_root}/${musl_triple}"
  local sysroot_target_dir="${PROJECT_ROOT}/toolchain/sysroot/${target}"

  echo "==> Trimming musl sysroot for ${target}"

  if [[ ! -d "${extracted_root}" ]]; then
    echo "error: extracted musl root not found: ${extracted_root}" >&2
    echo "hint:  run scripts/fetch_musl.sh first" >&2
    return 1
  fi

  # Verify the sysroot subdirectory has the expected markers.
  if [[ ! -d "${sysroot_src}/include" ]]; then
    echo "error: ${musl_triple}/include/ missing at extracted root — content looks wrong" >&2
    return 1
  fi
  if [[ ! -d "${sysroot_src}/lib" ]]; then
    echo "error: ${musl_triple}/lib/ missing at extracted root — content looks wrong" >&2
    return 1
  fi

  echo "==> Target: ${sysroot_target_dir}"

  # Only this target's dir is touched — other tools' trees under toolchain/
  # are preserved.
  rm -rf "${sysroot_target_dir}"
  mkdir -p "${sysroot_target_dir}/usr"

  # Copy include/ -> usr/include/
  # musl public headers: stdio.h, stdlib.h, string.h, unistd.h, etc.
  echo "==> Copying musl headers (usr/include/)"
  cp -R "${sysroot_src}/include" "${sysroot_target_dir}/usr/include"

  # Copy lib/ -> usr/lib/
  # musl static libraries + crt objects: libc.a, libc.o, crt1.o, crti.o,
  # crtn.o, rcrt1.o, Scrt1.o, etc. These are needed by clang at link time.
  echo "==> Copying musl static libs (usr/lib/)"
  cp -R "${sysroot_src}/lib" "${sysroot_target_dir}/usr/lib"

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
- \`./scripts/fetch_musl.sh && ./scripts/trim_musl.sh\` to re-fetch and trim.
- Delete \`temp/musl/\` to force a re-download.
EOF

  echo "==> Done. musl sysroot for ${target} at:"
  echo "    ${sysroot_target_dir}"
  echo "==> Verify: ls ${sysroot_target_dir}/usr/include/stdio.h"
  echo "==> Verify: ls ${sysroot_target_dir}/usr/lib/libc.a"
}

for entry in "${TARGETS[@]}"; do
  IFS='|' read -r target archive_name extracted_root_name musl_triple <<<"${entry}"
  trim_one "${target}" "${archive_name}" "${extracted_root_name}" "${musl_triple}"
  echo
done

echo "==> All musl sysroots trimmed."
