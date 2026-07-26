#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
INSTALL_ROOT=""
VERSION=""
WORK_ROOT=""
PUBLIC_HEADERS=(
  "feng_generated.h"
  "feng_runtime.h"
  "feng_runtime_contract.inc"
)
RUNTIME_PLATFORMS=(
  "macos-arm64"
  "linux-x64-gnu"
  "linux-x64-musl"
  "linux-arm64-gnu"
  "linux-arm64-musl"
)
LINUX_PLATFORMS=(
  "linux-x64-gnu"
  "linux-x64-musl"
  "linux-arm64-gnu"
  "linux-arm64-musl"
)

# Print the supported clean-install verification invocation.
usage() {
  cat <<'EOF'
Usage:
  scripts/verify_release_install.sh \
    --root=<installed-package-root> \
    --version=<version>
EOF
}

# Report one fatal clean-install verification error.
die() {
  echo "error: $*" >&2
  exit 1
}

# Require one command resolved through PATH.
require_cmd() {
  command -v "$1" >/dev/null 2>&1 ||
    die "missing required command: $1"
}

# Remove only the verification workspace created by this invocation.
cleanup() {
  if [[ -n "${WORK_ROOT}" && -d "${WORK_ROOT}" ]]; then
    rm -rf "${WORK_ROOT}"
  fi
}

# Verify one executable or object has the expected file format and CPU.
verify_platform_file() {
  local file_path="$1"
  local platform="$2"
  local description="$3"
  local format

  [[ -f "${file_path}" ]] || die "${description} not found: ${file_path}"
  format="$(file -b "${file_path}")"
  case "${platform}" in
    macos-arm64)
      [[ "${format}" == *"Mach-O 64-bit"* && "${format}" == *"arm64"* ]] ||
        die "${description} has unexpected format for ${platform}: ${format}"
      ;;
    linux-x64-gnu|linux-x64-musl)
      [[ "${format}" == *"ELF 64-bit"* && "${format}" == *"x86-64"* ]] ||
        die "${description} has unexpected format for ${platform}: ${format}"
      ;;
    linux-arm64-gnu|linux-arm64-musl)
      [[ "${format}" == *"ELF 64-bit"* && "${format}" == *"ARM aarch64"* ]] ||
        die "${description} has unexpected format for ${platform}: ${format}"
      ;;
    *)
      die "unsupported platform for format validation: ${platform}"
      ;;
  esac
}

# Verify every member in one installed runtime archive matches its platform.
verify_runtime_archive() {
  local archive_path="$1"
  local platform="$2"
  local archive_tool="$3"
  local object_path="${WORK_ROOT}/runtime-${platform}.o"
  local member
  local member_count=0

  [[ -f "${archive_path}" ]] ||
    die "installed runtime not found: ${platform}"
  while IFS= read -r member; do
    [[ -n "${member}" ]] || continue
    case "${member}" in
      "__.SYMDEF"|"__.SYMDEF SORTED"|"/"|"//")
        continue
        ;;
    esac
    case "${member}" in
      */*|.|..)
        die "unsafe archive member in installed runtime: ${member}"
        ;;
    esac
    "${archive_tool}" p "${archive_path}" "${member}" > "${object_path}" ||
      die "failed to read ${member} from installed runtime ${platform}"
    verify_platform_file \
      "${object_path}" "${platform}" \
      "installed runtime archive member ${member}"
    member_count=$((member_count + 1))
  done < <("${archive_tool}" t "${archive_path}")
  [[ "${member_count}" -gt 0 ]] ||
    die "installed runtime archive contains no members: ${platform}"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --root=*)
      [[ -z "${INSTALL_ROOT}" ]] || die "--root may only be specified once"
      INSTALL_ROOT="${1#--root=}"
      ;;
    --version=*)
      [[ -z "${VERSION}" ]] || die "--version may only be specified once"
      VERSION="${1#--version=}"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
  shift
done

[[ -n "${INSTALL_ROOT}" ]] || die "--root is required"
[[ -n "${VERSION}" ]] || die "--version is required"
[[ -d "${INSTALL_ROOT}" ]] || die "installed package root not found: ${INSTALL_ROOT}"
INSTALL_ROOT="$(cd "${INSTALL_ROOT}" && pwd)"
require_cmd file
mkdir -p "${PROJECT_ROOT}/build"
WORK_ROOT="$(mktemp -d "${PROJECT_ROOT}/build/release-install-verify.XXXXXX")"
trap cleanup EXIT

# shellcheck source=host_platform.sh
source "${SCRIPT_DIR}/host_platform.sh"
HOST_PLATFORM="$(feng_detect_host_platform)"
FENG="${INSTALL_ROOT}/bin/feng"
LLVM_BIN="${INSTALL_ROOT}/toolchain/llvm/bin"

[[ -x "${FENG}" ]] || die "installed Feng executable not found: ${FENG}"
[[ "$("${FENG}" --version)" == "feng ${VERSION}" ]] ||
  die "installed Feng version does not match ${VERSION}"
verify_platform_file "${FENG}" "${HOST_PLATFORM}" "installed Feng executable"
[[ "$(sed -n '1p' "${INSTALL_ROOT}/VERSION")" == "${VERSION}" ]] ||
  die "installed VERSION does not match ${VERSION}"

for header in "${PUBLIC_HEADERS[@]}"; do
  [[ -f "${INSTALL_ROOT}/include/${header}" ]] ||
    die "installed public header not found: ${header}"
done
for platform in "${RUNTIME_PLATFORMS[@]}"; do
  [[ -f "${INSTALL_ROOT}/lib/${platform}/libfeng_runtime.a" ]] ||
    die "installed runtime not found: ${platform}"
done
for platform in "${LINUX_PLATFORMS[@]}"; do
  [[ -d "${INSTALL_ROOT}/toolchain/sysroot/${platform}/usr/include" ]] ||
    die "installed sysroot include root not found: ${platform}"
  [[ -d "${INSTALL_ROOT}/toolchain/sysroot/${platform}/usr/lib" ]] ||
    die "installed sysroot library root not found: ${platform}"
  [[ -d "${INSTALL_ROOT}/toolchain/sysroot/${platform}/lib/gcc" ]] ||
    die "installed compiler runtime root not found: ${platform}"
done

for tool in clang lld ld.lld llvm-ar llvm-ranlib lldb lldb-dap lldb-argdumper; do
  [[ -x "${LLVM_BIN}/${tool}" ]] ||
    die "installed LLVM tool is missing or not executable: ${tool}"
done
case "${HOST_PLATFORM}" in
  macos-arm64)
    [[ -x "${LLVM_BIN}/debugserver" ]] ||
      die "installed debugserver is missing or not executable"
    ;;
  linux-*)
    [[ -x "${LLVM_BIN}/lldb-server" ]] ||
      die "installed lldb-server is missing or not executable"
    ;;
esac

for platform in "${RUNTIME_PLATFORMS[@]}"; do
  verify_runtime_archive \
    "${INSTALL_ROOT}/lib/${platform}/libfeng_runtime.a" \
    "${platform}" \
    "${LLVM_BIN}/llvm-ar"
done

"${LLVM_BIN}/clang" --version >/dev/null
"${LLVM_BIN}/ld.lld" --version >/dev/null
"${LLVM_BIN}/llvm-ar" --version >/dev/null
"${LLVM_BIN}/llvm-ranlib" --version >/dev/null
"${LLVM_BIN}/lldb" --version >/dev/null

PROJECT_DIR="${WORK_ROOT}/project"
mkdir -p "${PROJECT_DIR}/src"

cat > "${PROJECT_DIR}/feng.fm" <<'EOF'
[package]
name: "release_verify"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"
EOF
cat > "${PROJECT_DIR}/src/main.ff" <<'EOF'
module release.verify;

@cdecl("libc")
extern func puts(message: string*): int;

func main(args: string[]) {
    puts(&"release install verification");
}
EOF

"${FENG}" build "${PROJECT_DIR}"
[[ -x "${PROJECT_DIR}/build/${HOST_PLATFORM}/bin/release_verify" ]] ||
  die "installed Feng build did not produce the host executable"
RUN_OUTPUT="$("${FENG}" run "${PROJECT_DIR}")"
[[ "${RUN_OUTPUT}" == *"release install verification"* ]] ||
  die "installed Feng run produced unexpected output"

echo "==> Verified Feng ${VERSION} clean install for ${HOST_PLATFORM}"
