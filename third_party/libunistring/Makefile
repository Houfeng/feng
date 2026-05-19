CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./src -I./src/lib

TARGET = libfeng_u8_text.a

SRCS = src/lib/unistr/u8-mbtouc-aux.c \
                      src/lib/unistr/u8-mbtoucr.c \
                      src/lib/unistr/u8-mbsnlen.c \
                      src/lib/unistr/u8-strmbtouc.c \
                      src/lib/unistr/u8-next.c \
                      src/lib/unistr/u8-prev.c \
                      src/lib/unigbrk/u8-grapheme-breaks.c \
                      src/lib/unigbrk/u8-grapheme-next.c \
                      src/lib/unigbrk/u8-grapheme-prev.c \
                      src/lib/unigbrk/uc-gbrk-prop.c \
                      src/lib/unictype/pr_extended_pictographic.c \
                      src/lib/unictype/incb_of.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
