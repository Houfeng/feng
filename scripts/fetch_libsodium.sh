#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${PROJECT_ROOT}/third_party/libsodium"
TMP_ROOT="${PROJECT_ROOT}/temp"
TMP_DIR="${TMP_ROOT}/libsodium_tmp"
ARCHIVE_FILE="${TMP_DIR}/libsodium.tar.gz"

LIBSODIUM_TAG="${LIBSODIUM_TAG:-1.0.22-RELEASE}"
LIBSODIUM_SRC_URL="${LIBSODIUM_SRC_URL:-https://github.com/jedisct1/libsodium/archive/refs/tags/${LIBSODIUM_TAG}.tar.gz}"

SEED_FILES=(
  sodium/core.c
  sodium/runtime.c
  sodium/utils.c
  randombytes/randombytes.c
  randombytes/sysrandom/randombytes_sysrandom.c
  crypto_aead/chacha20poly1305/aead_chacha20poly1305.c
  crypto_stream/chacha20/stream_chacha20.c
  crypto_stream/chacha20/ref/chacha20_ref.c
  crypto_stream/chacha20/dolbeau/chacha20_dolbeau-avx2.c
  crypto_stream/chacha20/dolbeau/chacha20_dolbeau-ssse3.c
  crypto_onetimeauth/crypto_onetimeauth.c
  crypto_onetimeauth/poly1305/onetimeauth_poly1305.c
  crypto_onetimeauth/poly1305/donna/poly1305_donna.c
  crypto_onetimeauth/poly1305/sse2/poly1305_sse2.c
  crypto_scalarmult/crypto_scalarmult.c
  crypto_scalarmult/curve25519/scalarmult_curve25519.c
  crypto_scalarmult/curve25519/ref10/x25519_ref10.c
  crypto_scalarmult/curve25519/sandy2x/consts.S
  crypto_scalarmult/curve25519/sandy2x/curve25519_sandy2x.c
  crypto_scalarmult/curve25519/sandy2x/fe51_invert.c
  crypto_scalarmult/curve25519/sandy2x/fe_frombytes_sandy2x.c
  crypto_scalarmult/curve25519/sandy2x/ladder.S
  crypto_scalarmult/curve25519/sandy2x/sandy2x.S
  crypto_sign/crypto_sign.c
  crypto_sign/ed25519/sign_ed25519.c
  crypto_sign/ed25519/ref10/keypair.c
  crypto_sign/ed25519/ref10/open.c
  crypto_sign/ed25519/ref10/sign.c
  crypto_core/ed25519/ref10/ed25519_ref10.c
  crypto_generichash/crypto_generichash.c
  crypto_generichash/blake2b/generichash_blake2.c
  crypto_generichash/blake2b/ref/blake2b-ref.c
  crypto_generichash/blake2b/ref/blake2b-compress-avx2.c
  crypto_generichash/blake2b/ref/blake2b-compress-ref.c
  crypto_generichash/blake2b/ref/blake2b-compress-sse41.c
  crypto_generichash/blake2b/ref/blake2b-compress-ssse3.c
  crypto_generichash/blake2b/ref/generichash_blake2b.c
  crypto_kdf/crypto_kdf.c
  crypto_kdf/blake2b/kdf_blake2b.c
  crypto_hash/crypto_hash.c
  crypto_hash/sha512/hash_sha512.c
  crypto_hash/sha512/cp/hash_sha512_cp.c
  crypto_verify/verify.c
  crypto_pwhash/crypto_pwhash.c
  crypto_pwhash/argon2/argon2.c
  crypto_pwhash/argon2/argon2-core.c
  crypto_pwhash/argon2/argon2-encoding.c
  crypto_pwhash/argon2/argon2-fill-block-avx2.c
  crypto_pwhash/argon2/argon2-fill-block-avx512f.c
  crypto_pwhash/argon2/argon2-fill-block-neon.c
  crypto_pwhash/argon2/argon2-fill-block-ref.c
  crypto_pwhash/argon2/argon2-fill-block-ssse3.c
  crypto_pwhash/argon2/argon2-fill-block-wasm32.c
  crypto_pwhash/argon2/blake2b-long.c
  crypto_pwhash/argon2/pwhash_argon2i.c
  crypto_pwhash/argon2/pwhash_argon2id.c
)

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

contains() {
  local needle="$1"
  shift
  local entry

  for entry in "$@"; do
    if [[ "${entry}" == "${needle}" ]]; then
      return 0
    fi
  done
  return 1
}

cleanup() {
  rm -rf "${TMP_DIR}"
}

resolve_extracted_root() {
  find "${TMP_DIR}" -mindepth 1 -maxdepth 1 -type d -name 'libsodium-*' | head -n 1
}

target_path_for_rel() {
  local rel_path="$1"

  if [[ "${rel_path}" == include/* ]]; then
    printf '%s/%s\n' "${TARGET_DIR}" "${rel_path}"
  else
    printf '%s/src/libsodium/%s\n' "${TARGET_DIR}" "${rel_path}"
  fi
}

parse_version_value() {
  local pattern="$1"
  local file_path="$2"

  sed -n "s/^${pattern}=\([0-9][0-9]*\)$/\1/p" "${file_path}"
}

generate_version_header() {
  local template_path="$1"
  local output_path="$2"
  local version_string="$3"
  local version_major="$4"
  local version_minor="$5"

  mkdir -p "$(dirname "${output_path}")"
  sed \
    -e "s|@VERSION@|${version_string}|g" \
    -e "s|@SODIUM_LIBRARY_VERSION_MAJOR@|${version_major}|g" \
    -e "s|@SODIUM_LIBRARY_VERSION_MINOR@|${version_minor}|g" \
    -e '/@SODIUM_LIBRARY_MINIMAL_DEF@/d' \
    "${template_path}" > "${output_path}"
}

copy_rel_file() {
  local rel_path="$1"
  local upstream_root="$2"
  local public_include_root="$3"
  local version_string="$4"
  local version_major="$5"
  local version_minor="$6"
  local source_path
  local target_path

  target_path="$(target_path_for_rel "${rel_path}")"

  if [[ "${rel_path}" == include/sodium/version.h ]]; then
    generate_version_header \
      "${public_include_root}/version.h.in" \
      "${target_path}" \
      "${version_string}" \
      "${version_major}" \
      "${version_minor}"
    return 0
  fi

  source_path="${upstream_root}/${rel_path}"
  if [[ ! -f "${source_path}" ]]; then
    echo "error: missing upstream file: ${source_path}" >&2
    exit 1
  fi

  mkdir -p "$(dirname "${target_path}")"
  cp "${source_path}" "${target_path}"
}

parse_rel_includes() {
  local rel_path="$1"
  local upstream_root="$2"
  local public_include_root="$3"
  local parse_source

  if [[ "${rel_path}" == include/sodium/version.h ]]; then
    parse_source="${public_include_root}/version.h.in"
  else
    parse_source="${upstream_root}/${rel_path}"
  fi

  sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "${parse_source}"
}

resolve_include_rel() {
  local from_rel="$1"
  local include_path="$2"
  local upstream_root="$3"
  local current_dir
  local candidate
  local rel_path
  local search_root

  if [[ "${include_path}" == version.h ]]; then
    printf 'include/sodium/version.h\n'
    return 0
  fi

  current_dir="$(dirname "${from_rel}")"
  candidate="${upstream_root}/${current_dir}/${include_path}"
  if [[ -f "${candidate}" ]]; then
    rel_path="${candidate#${upstream_root}/}"
    printf '%s\n' "${rel_path}"
    return 0
  fi

  for search_root in \
    "${upstream_root}/include/sodium" \
    "${upstream_root}/include" \
    "${upstream_root}"; do
    candidate="${search_root}/${include_path}"
    if [[ -f "${candidate}" ]]; then
      rel_path="${candidate#${upstream_root}/}"
      printf '%s\n' "${rel_path}"
      return 0
    fi
  done

  return 1
}

trap cleanup EXIT

require_cmd curl
require_cmd tar
require_cmd sed
require_cmd grep
require_cmd find
require_cmd wc

echo "==> Syncing libsodium ${LIBSODIUM_TAG} into ${TARGET_DIR}"

rm -rf "${TARGET_DIR}" "${TMP_DIR}"
mkdir -p "${TARGET_DIR}/include/sodium" "${TARGET_DIR}/src/libsodium" "${TMP_DIR}"

echo "==> Downloading ${LIBSODIUM_SRC_URL}"
curl -sSLf --connect-timeout 15 "${LIBSODIUM_SRC_URL}" -o "${ARCHIVE_FILE}"

echo "==> Extracting source archive"
tar -xzf "${ARCHIVE_FILE}" -C "${TMP_DIR}"

EXTRACTED_ROOT="$(resolve_extracted_root)"
if [[ -z "${EXTRACTED_ROOT}" ]]; then
  echo "error: could not locate extracted libsodium source tree" >&2
  exit 1
fi

UPSTREAM_ROOT="${EXTRACTED_ROOT}/src/libsodium"
PUBLIC_INCLUDE_ROOT="${UPSTREAM_ROOT}/include/sodium"
CONFIGURE_AC_PATH="${EXTRACTED_ROOT}/configure.ac"

VERSION_STRING="$(sed -n 's/^AC_INIT(\[libsodium\],\[\([^]]*\)\].*/\1/p' "${CONFIGURE_AC_PATH}")"
VERSION_MAJOR="$(parse_version_value SODIUM_LIBRARY_VERSION_MAJOR "${CONFIGURE_AC_PATH}")"
VERSION_MINOR="$(parse_version_value SODIUM_LIBRARY_VERSION_MINOR "${CONFIGURE_AC_PATH}")"

if [[ -z "${VERSION_STRING}" || -z "${VERSION_MAJOR}" || -z "${VERSION_MINOR}" ]]; then
  echo "error: failed to parse version metadata from ${CONFIGURE_AC_PATH}" >&2
  exit 1
fi

queue=("${SEED_FILES[@]}" "include/sodium/version.h")
copied=()

while [[ "${#queue[@]}" -gt 0 ]]; do
  current_rel="${queue[0]}"
  queue=("${queue[@]:1}")

  if contains "${current_rel}" "${copied[@]-}"; then
    continue
  fi

  copy_rel_file \
    "${current_rel}" \
    "${UPSTREAM_ROOT}" \
    "${PUBLIC_INCLUDE_ROOT}" \
    "${VERSION_STRING}" \
    "${VERSION_MAJOR}" \
    "${VERSION_MINOR}"
  copied+=("${current_rel}")

  while IFS= read -r include_rel; do
    if [[ -z "${include_rel}" ]]; then
      continue
    fi
    resolved_rel="$(resolve_include_rel "${current_rel}" "${include_rel}" "${UPSTREAM_ROOT}" || true)"
    if [[ -z "${resolved_rel}" ]]; then
      continue
    fi
    if contains "${resolved_rel}" "${copied[@]-}"; then
      continue
    fi
    if contains "${resolved_rel}" "${queue[@]-}"; then
      continue
    fi
    queue+=("${resolved_rel}")
  done < <(parse_rel_includes "${current_rel}" "${UPSTREAM_ROOT}" "${PUBLIC_INCLUDE_ROOT}")
done

if [[ -f "${EXTRACTED_ROOT}/LICENSE" ]]; then
  cp "${EXTRACTED_ROOT}/LICENSE" "${TARGET_DIR}/LICENSE"
fi

cat > "${TARGET_DIR}/Makefile" <<'EOF'
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?=

TARGET ?= libfeng_std_sodium.a
HOST_OS := $(shell sh -c 'os=$$(uname -s); if [ "$$os" = Darwin ]; then printf macos; elif [ "$$os" = Linux ]; then printf linux; else printf %s "$$os" | tr "[:upper:]" "[:lower:]"; fi')
HOST_ARCH := $(if $(filter x86_64 amd64,$(shell uname -m)),x64,$(if $(filter arm64 aarch64,$(shell uname -m)),arm64,$(shell uname -m)))
OUTPUT_DIR ?= ../../std/extlib/$(HOST_OS)-$(HOST_ARCH)
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

UNAME_M := $(shell uname -m)
HOST_ARCH := $(if $(filter x86_64 amd64,$(UNAME_M)),x64,$(if $(filter arm64 aarch64,$(UNAME_M)),arm64,generic))

CPPFLAGS += -DCONFIGURED=1 -DNATIVE_LITTLE_ENDIAN=1 -I./include -I./include/sodium -I./src/libsodium

ifeq ($(HOST_ARCH),x64)
CPPFLAGS += -DHAVE_TI_MODE=1 -DHAVE_CPUID=1 -DHAVE_AVX_ASM=1 -DHAVE_EMMINTRIN_H=1 -DHAVE_PMMINTRIN_H=1 -DHAVE_TMMINTRIN_H=1 -DHAVE_SMMINTRIN_H=1 -DHAVE_AVXINTRIN_H=1 -DHAVE_AVX2INTRIN_H=1 -DHAVE_WMMINTRIN_H=1
endif

ifeq ($(HOST_ARCH),arm64)
CPPFLAGS += -DHAVE_TI_MODE=1
endif

C_SRCS := $(shell find src/libsodium -type f -name '*.c' | LC_ALL=C sort)
ifeq ($(HOST_ARCH),x64)
ASM_SRCS := $(shell find src/libsodium -type f -name '*.S' | LC_ALL=C sort)
else
ASM_SRCS :=
endif

SRCS := $(C_SRCS) $(ASM_SRCS)
OBJS := $(patsubst %.c,%.o,$(patsubst %.S,%.o,$(SRCS)))

AVX2_SRCS := src/libsodium/crypto_stream/chacha20/dolbeau/chacha20_dolbeau-avx2.o \
   src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-avx2.o \
   src/libsodium/crypto_pwhash/argon2/argon2-fill-block-avx2.o
SSSE3_SRCS := src/libsodium/crypto_stream/chacha20/dolbeau/chacha20_dolbeau-ssse3.o \
   src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-ssse3.o \
   src/libsodium/crypto_pwhash/argon2/argon2-fill-block-ssse3.o
SSE41_SRCS := src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-sse41.o
SANDY2X_OBJS := src/libsodium/crypto_scalarmult/curve25519/sandy2x/consts.o \
   src/libsodium/crypto_scalarmult/curve25519/sandy2x/curve25519_sandy2x.o \
   src/libsodium/crypto_scalarmult/curve25519/sandy2x/fe51_invert.o \
   src/libsodium/crypto_scalarmult/curve25519/sandy2x/fe_frombytes_sandy2x.o \
   src/libsodium/crypto_scalarmult/curve25519/sandy2x/ladder.o \
   src/libsodium/crypto_scalarmult/curve25519/sandy2x/sandy2x.o

ifeq ($(HOST_ARCH),x64)
$(AVX2_SRCS): CFLAGS += -mavx2
$(SSSE3_SRCS): CFLAGS += -mssse3
$(SSE41_SRCS): CFLAGS += -msse4.1
$(SANDY2X_OBJS): CFLAGS += -mavx
endif

all: $(OUTPUT_TARGET)

$(TARGET): $(OBJS) ; $(AR) rcs $@ $^

$(OUTPUT_TARGET): $(TARGET) ; @mkdir -p "$(OUTPUT_DIR)" && cp "$(TARGET)" "$(OUTPUT_TARGET)"

install: $(OUTPUT_TARGET)

%.o: %.c ; $(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.S ; $(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean: ; rm -f $(OBJS) $(TARGET) $(OUTPUT_TARGET)

.PHONY: all clean install
EOF

SOURCE_COUNT="$(find "${TARGET_DIR}/src/libsodium" -type f \( -name '*.c' -o -name '*.S' \) | wc -l | tr -d ' ')"
HEADER_COUNT="$(find "${TARGET_DIR}" -type f \( -name '*.h' -o -name '*.in' \) | wc -l | tr -d ' ')"

echo "==> Synced ${SOURCE_COUNT} source files and ${HEADER_COUNT} headers"
echo "==> libsodium sync complete"