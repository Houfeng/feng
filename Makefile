CC ?= cc
CPPFLAGS ?= -Isrc -Ithird_party/miniz
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
LDFLAGS ?=
# Phase 1B cycle collector relies on pthread (recursive mutex). Unit tests link
# runtime objects directly, so they also link the vendored unwinder archive.
RUNTIME_LDLIBS ?= $(LIBUNWIND_LIB) -lpthread
DEPFLAGS = -MMD -MP

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin

LEXER_SRCS := $(wildcard src/lexer/*.c)
PARSER_SRCS := $(wildcard src/parser/*.c)
SEMANTIC_SRCS := $(wildcard src/semantic/*.c)
CODEGEN_SRCS := $(wildcard src/codegen/*.c)
DEBUG_SRCS := $(wildcard src/debug/*.c)
DAP_SRCS := $(wildcard src/dap/*.c)
SYMBOL_SRCS := $(wildcard src/symbol/*.c)
RUNTIME_SRCS := $(wildcard src/runtime/*.c)
ARCHIVE_SRCS := $(wildcard src/archive/*.c)
THIRD_PARTY_SRCS := third_party/miniz/miniz.c
CLI_SRCS := $(shell find src/cli -name '*.c')
TEST_ARCHIVE_SRCS := $(wildcard test/archive/*.c)
TEST_LEXER_SRCS := $(wildcard test/lexer/*.c)
TEST_PARSER_SRCS := $(wildcard test/parser/*.c)
TEST_SEMANTIC_SRCS := $(wildcard test/semantic/*.c)
TEST_RUNTIME_SRCS := $(wildcard test/runtime/*.c)
TEST_CODEGEN_SRCS := $(wildcard test/codegen/*.c)
TEST_DEBUG_SRCS := $(wildcard test/debug/*.c)
TEST_CLI_SRCS := $(wildcard test/cli/*.c)
TEST_SYMBOL_SRCS := $(wildcard test/symbol/*.c)
TEST_CLI_SUPPORT_SRCS := src/cli/common.c src/cli/frontend.c \
	src/cli/lsp/server.c src/cli/lsp/runtime.c src/cli/lsp/main.c \
	src/cli/dap/main.c \
	src/cli/project/common.c src/cli/project/init.c src/cli/project/manifest.c \
	src/cli/project/build.c \
	src/cli/project/check.c \
	src/cli/project/compile.c \
	src/cli/project/run.c \
	src/cli/project/pack.c \
	src/cli/deps/manager.c \
	src/cli/deps/main.c \
	src/cli/compile/options.c src/cli/compile/direct.c src/cli/compile/driver.c

CLI_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(PARSER_SRCS) $(SEMANTIC_SRCS) $(CODEGEN_SRCS) $(DEBUG_SRCS) $(DAP_SRCS) $(SYMBOL_SRCS) $(ARCHIVE_SRCS) $(THIRD_PARTY_SRCS) $(CLI_SRCS))
RUNTIME_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(RUNTIME_SRCS))
TEST_ARCHIVE_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(ARCHIVE_SRCS) $(THIRD_PARTY_SRCS) $(TEST_ARCHIVE_SRCS))
TEST_LEXER_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(TEST_LEXER_SRCS))
TEST_PARSER_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(PARSER_SRCS) $(TEST_PARSER_SRCS))
TEST_SEMANTIC_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(PARSER_SRCS) $(SEMANTIC_SRCS) $(SYMBOL_SRCS) $(ARCHIVE_SRCS) $(THIRD_PARTY_SRCS) $(TEST_SEMANTIC_SRCS))
TEST_RUNTIME_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(RUNTIME_SRCS) $(TEST_RUNTIME_SRCS))
TEST_CODEGEN_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(PARSER_SRCS) $(SEMANTIC_SRCS) $(CODEGEN_SRCS) $(DEBUG_SRCS) $(SYMBOL_SRCS) $(ARCHIVE_SRCS) $(THIRD_PARTY_SRCS) $(TEST_CODEGEN_SRCS))
TEST_DEBUG_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(PARSER_SRCS) $(SEMANTIC_SRCS) $(CODEGEN_SRCS) $(DEBUG_SRCS) $(SYMBOL_SRCS) $(ARCHIVE_SRCS) $(THIRD_PARTY_SRCS) $(TEST_DEBUG_SRCS))
TEST_CLI_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(PARSER_SRCS) $(SEMANTIC_SRCS) $(CODEGEN_SRCS) $(DEBUG_SRCS) $(DAP_SRCS) $(SYMBOL_SRCS) $(ARCHIVE_SRCS) $(THIRD_PARTY_SRCS) $(TEST_CLI_SUPPORT_SRCS) $(TEST_CLI_SRCS))
TEST_SYMBOL_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LEXER_SRCS) $(PARSER_SRCS) $(SEMANTIC_SRCS) $(SYMBOL_SRCS) $(ARCHIVE_SRCS) $(THIRD_PARTY_SRCS) $(TEST_SYMBOL_SRCS))
DEPS := $(CLI_OBJS:.o=.d) $(RUNTIME_OBJS:.o=.d) $(TEST_ARCHIVE_OBJS:.o=.d) \
	$(TEST_LEXER_OBJS:.o=.d) $(TEST_PARSER_OBJS:.o=.d) \
	$(TEST_SEMANTIC_OBJS:.o=.d) $(TEST_RUNTIME_OBJS:.o=.d) \
	$(TEST_CODEGEN_OBJS:.o=.d) $(TEST_DEBUG_OBJS:.o=.d) $(TEST_CLI_OBJS:.o=.d) \
	$(TEST_SYMBOL_OBJS:.o=.d)

THIRD_PARTY_CFLAGS := $(filter-out -Werror -pedantic,$(CFLAGS)) -Wno-unused-function

LIB_DIR := $(BUILD_DIR)/lib
ifeq ($(OS),Windows_NT)
STATIC_LIB_PREFIX :=
STATIC_LIB_EXT := .lib
else
STATIC_LIB_PREFIX := lib
STATIC_LIB_EXT := .a
endif

# Detect host target (os-arch) for extlib path
_UNAME_S := $(shell uname -s)
_UNAME_M := $(shell uname -m)
ifeq ($(_UNAME_S),Darwin)
  _HOST_OS := macos
else ifeq ($(_UNAME_S),Linux)
  _HOST_OS := linux
else
  _HOST_OS := windows
endif
ifeq ($(_UNAME_M),arm64)
  _HOST_ARCH := arm64
else ifeq ($(_UNAME_M),aarch64)
  _HOST_ARCH := arm64
else
  _HOST_ARCH := x64
endif
HOST_TARGET := $(_HOST_OS)-$(_HOST_ARCH)
EXTLIB_DIR := extlib/$(HOST_TARGET)

RUNTIME_LIB := $(LIB_DIR)/$(STATIC_LIB_PREFIX)feng_runtime$(STATIC_LIB_EXT)
LIBUNWIND_LIB := $(EXTLIB_DIR)/$(STATIC_LIB_PREFIX)feng_unwind$(STATIC_LIB_EXT)

.PHONY: all cli runtime test test-normal smoke cli-tests cli-project-tests std-tests fcts-tests perf-constraints test-sanitize clean

all: cli runtime

cli: $(BIN_DIR)/feng

runtime: $(RUNTIME_LIB)

test: test-sanitize test-normal

test-normal:
	$(MAKE) clean
	$(MAKE) $(BIN_DIR)/test_archive $(BIN_DIR)/test_lexer $(BIN_DIR)/test_parser $(BIN_DIR)/test_semantic $(BIN_DIR)/test_runtime $(BIN_DIR)/test_codegen $(BIN_DIR)/test_debug $(BIN_DIR)/test_cli $(BIN_DIR)/test_symbol smoke cli-tests cli-project-tests std-tests fcts-tests perf-constraints
	$(BIN_DIR)/test_archive
	$(BIN_DIR)/test_lexer
	$(BIN_DIR)/test_parser
	$(BIN_DIR)/test_semantic
	$(BIN_DIR)/test_runtime
	$(BIN_DIR)/test_codegen
	$(BIN_DIR)/test_debug
	$(BIN_DIR)/test_cli
	$(BIN_DIR)/test_symbol

# Sanitize testing strategy:
# - macOS: UBSan only (ASan causes deadlock/infinite loop even with LLVM 21)
# - Linux CI: Should use full ASan + UBSan (-fsanitize=address,undefined)
#
# ASan issues on macOS (verified with LLVM 21.1.8):
# 1. dyld initialization deadlock: ASan's __malloc_init conflicts with libSystem
# 2. libfeng_unwind stack unwinding conflicts with ASan's fake stack mechanism
# 3. DYLD_INSERT_LIBRARIES workaround does not resolve the issue
#
# Recommendation: Use Linux containers/VMs for full ASan testing on macOS hosts
test-sanitize:
	$(MAKE) clean
	@echo "=== Sanitize Test (UBSan only on macOS) ==="
	@echo "Note: ASan causes deadlock on macOS (dyld + libunwind conflict)."
	@echo "For full ASan + UBSan testing, use Linux CI."
	$(MAKE) runtime CFLAGS="-fsanitize=undefined -g -O1 -std=c11 -Wall -Wextra -pedantic"
	$(MAKE) cli $(BIN_DIR)/test_archive $(BIN_DIR)/test_lexer $(BIN_DIR)/test_parser $(BIN_DIR)/test_semantic $(BIN_DIR)/test_runtime $(BIN_DIR)/test_codegen $(BIN_DIR)/test_debug $(BIN_DIR)/test_cli $(BIN_DIR)/test_symbol CFLAGS="-fsanitize=undefined -g -O1 -std=c11 -Wall -Wextra -pedantic" LDFLAGS="-fsanitize=undefined"
	$(BIN_DIR)/test_archive
	$(BIN_DIR)/test_lexer
	$(BIN_DIR)/test_parser
	$(BIN_DIR)/test_semantic
	$(BIN_DIR)/test_runtime
	$(BIN_DIR)/test_codegen
	$(BIN_DIR)/test_debug
	FENG_CC_FLAGS="-fsanitize=undefined" $(BIN_DIR)/test_cli
	$(BIN_DIR)/test_symbol
	FENG_CC_FLAGS="-fsanitize=undefined" $(MAKE) smoke cli-tests cli-project-tests std-tests fcts-tests perf-constraints

perf-constraints: cli runtime
	FENG_TEMP_DIR=$(CURDIR)/temp ./scripts/run_perf_constraints.sh

std-tests: cli runtime
	FENG_TEMP_DIR=$(CURDIR)/temp $(BIN_DIR)/feng run ./std_test

fcts-tests: cli runtime
	FENG_TEMP_DIR=$(CURDIR)/temp $(BIN_DIR)/feng run ./fcts/fcts_bin

smoke: cli runtime
	FENG_TEMP_DIR=$(CURDIR)/temp ./scripts/run_smoke.sh

cli-tests: cli runtime
	FENG_TEMP_DIR=$(CURDIR)/temp ./scripts/run_cli_direct.sh

cli-project-tests: cli runtime
	FENG_TEMP_DIR=$(CURDIR)/temp ./scripts/run_cli_project.sh

$(BIN_DIR)/feng: $(CLI_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CLI_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_lexer: $(TEST_LEXER_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_LEXER_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_archive: $(TEST_ARCHIVE_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_ARCHIVE_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_parser: $(TEST_PARSER_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_PARSER_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_semantic: $(TEST_SEMANTIC_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_SEMANTIC_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_runtime: $(TEST_RUNTIME_OBJS) $(LIBUNWIND_LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_RUNTIME_OBJS) $(LDFLAGS) $(RUNTIME_LDLIBS) -o $@

$(BIN_DIR)/test_codegen: $(TEST_CODEGEN_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_CODEGEN_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_debug: $(TEST_DEBUG_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_DEBUG_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_cli: $(TEST_CLI_OBJS) $(RUNTIME_LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_CLI_OBJS) $(LDFLAGS) -o $@

$(BIN_DIR)/test_symbol: $(TEST_SYMBOL_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(TEST_SYMBOL_OBJS) $(LDFLAGS) -o $@

$(RUNTIME_LIB): $(RUNTIME_OBJS) $(LIBUNWIND_LIB)
	@mkdir -p $(LIB_DIR)
	@rm -rf $(BUILD_DIR)/temp/runtime-libunwind-objs
	@mkdir -p $(BUILD_DIR)/temp/runtime-libunwind-objs
	cd $(BUILD_DIR)/temp/runtime-libunwind-objs && $(AR) x ../../../$(LIBUNWIND_LIB)
	$(AR) rcs $@ $(RUNTIME_OBJS) $(BUILD_DIR)/temp/runtime-libunwind-objs/*.o

# libunwind is a pre-built vendored library; run scripts/build_libunwind.sh once to produce it.
$(LIBUNWIND_LIB):
	@echo "error: $@ not found" >&2
	@echo "hint:  run scripts/build_libunwind.sh to build libunwind into extlib/$(HOST_TARGET)/" >&2
	@exit 1

$(OBJ_DIR)/third_party/miniz/%.o: third_party/miniz/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(THIRD_PARTY_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	$(RM) -r $(BUILD_DIR)

-include $(DEPS)