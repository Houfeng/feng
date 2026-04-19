CC ?= cc
CPPFLAGS ?= -Isrc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDFLAGS ?=

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

LEXER_SRCS := $(wildcard src/lexer/*.c)
CLI_SRCS := $(wildcard src/cli/*.c)
TEST_LEXER_SRCS := $(wildcard test/lexer/*.c)

CLI_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(CLI_SRCS))
TEST_LEXER_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(TEST_LEXER_SRCS))

.PHONY: all cli test clean

all: cli

cli: $(BIN_DIR)/feng

test: $(BIN_DIR)/test_lexer
	$(BIN_DIR)/test_lexer

$(BIN_DIR)/feng: $(CLI_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CLI_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_lexer: $(TEST_LEXER_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_LEXER_OBJS) $(LDFLAGS) -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	$(RM) -r $(BUILD_DIR)