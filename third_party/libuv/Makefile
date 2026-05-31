CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./include -I./src -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_DARWIN_UNLIMITED_SELECT=1 -D_DARWIN_USE_64_BIT_INODE=1

TARGET ?= libfeng_std_uv.a
HOST_OS :=   $(shell sh -c 'os=$$(uname -s); if [ "$$os" = Darwin ]; then printf macos; elif [ "$$os" = Linux ]; then printf linux; else printf %s "$$os" | tr "[:upper:]" "[:lower:]"; fi')
HOST_ARCH := $(if $(filter x86_64 amd64,$(shell uname -m)),x64,$(if $(filter arm64 aarch64,$(shell uname -m)),arm64,$(shell uname -m)))
OUTPUT_DIR ?= ../../std/extlib/$(HOST_OS)-$(HOST_ARCH)
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

SRCS = $(shell find src -type f -name '*.c' | LC_ALL=C sort)

OBJS = $(SRCS:.c=.o)

all: $(OUTPUT_TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

$(OUTPUT_TARGET): $(TARGET) ; @mkdir -p "$(OUTPUT_DIR)" && cp "$(TARGET)" "$(OUTPUT_TARGET)"

install: $(OUTPUT_TARGET)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean: ; rm -f $(OBJS) $(TARGET) $(OUTPUT_TARGET)

.PHONY: all clean install
