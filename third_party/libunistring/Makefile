CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./src -I./src/lib

TARGET ?= libfeng_std_unistring.a
HOST_OS :=   $(shell sh -c 'os=$$(uname -s); if [ "$$os" = Darwin ]; then printf macos; elif [ "$$os" = Linux ]; then printf linux; else printf %s "$$os" | tr "[:upper:]" "[:lower:]"; fi')
HOST_ARCH := $(if $(filter x86_64 amd64,$(shell uname -m)),x64,$(if $(filter arm64 aarch64,$(shell uname -m)),arm64,$(shell uname -m)))
HOST_PLATFORM := $(if $(filter linux,$(HOST_OS)),$(HOST_OS)-$(HOST_ARCH)-gnu,$(HOST_OS)-$(HOST_ARCH))
OUTPUT_DIR ?= ../../std/std/extlib/$(HOST_PLATFORM)
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

SRCS = src/lib/unistr/u8-mbtouc-aux.c \
                      src/lib/unistr/u8-mbtoucr.c \
                      src/lib/unistr/u8-mbsnlen.c \
                      src/lib/unistr/u8-strmbtouc.c \
                      src/lib/unistr/u8-next.c \
                      src/lib/unistr/u8-prev.c \
                      src/lib/unistr/u8-uctomb-aux.c \
                      src/lib/unigbrk/u8-grapheme-breaks.c \
                      src/lib/unigbrk/u8-grapheme-next.c \
                      src/lib/unigbrk/u8-grapheme-prev.c \
                      src/lib/unigbrk/uc-gbrk-prop.c \
                      src/lib/unictype/pr_extended_pictographic.c \
                      src/lib/unictype/incb_of.c \
                      src/lib/unicase/tolower.c \
                      src/lib/unicase/toupper.c

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
