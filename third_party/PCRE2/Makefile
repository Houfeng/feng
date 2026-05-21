CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./include -I./src -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8

TARGET ?= libfeng_std_pcre2.a
OUTPUT_DIR ?= ../../std/lib
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

SRCS = src/pcre2_chartables.c \
	   src/pcre2_auto_possess.c \
	   src/pcre2_chkdint.c \
	   src/pcre2_compile.c \
	   src/pcre2_compile_class.c \
	   src/pcre2_config.c \
	   src/pcre2_context.c \
	   src/pcre2_convert.c \
	   src/pcre2_dfa_match.c \
	   src/pcre2_error.c \
	   src/pcre2_extuni.c \
	   src/pcre2_find_bracket.c \
	   src/pcre2_jit_compile.c \
	   src/pcre2_maketables.c \
	   src/pcre2_match.c \
	   src/pcre2_match_data.c \
	   src/pcre2_newline.c \
	   src/pcre2_ord2utf.c \
	   src/pcre2_pattern_info.c \
	   src/pcre2_script_run.c \
	   src/pcre2_serialize.c \
	   src/pcre2_string_utils.c \
	   src/pcre2_study.c \
	   src/pcre2_substitute.c \
	   src/pcre2_substring.c \
	   src/pcre2_tables.c \
	   src/pcre2_ucd.c \
	   src/pcre2_valid_utf.c \
	   src/pcre2_xclass.c

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
