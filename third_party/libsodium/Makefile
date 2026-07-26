CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?=

TARGET ?= libfeng_std_sodium.a
HOST_OS := $(shell sh -c 'os=$$(uname -s); if [ "$$os" = Darwin ]; then printf macos; elif [ "$$os" = Linux ]; then printf linux; else printf %s "$$os" | tr "[:upper:]" "[:lower:]"; fi')
UNAME_M := $(shell uname -m)
HOST_ARCH := $(if $(filter x86_64 amd64,$(UNAME_M)),x64,$(if $(filter arm64 aarch64,$(UNAME_M)),arm64,generic))
HOST_PLATFORM := $(if $(filter linux,$(HOST_OS)),$(HOST_OS)-$(HOST_ARCH)-gnu,$(HOST_OS)-$(HOST_ARCH))
OUTPUT_DIR ?= ../../std/extlib/$(HOST_PLATFORM)
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

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
