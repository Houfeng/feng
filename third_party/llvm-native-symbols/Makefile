# Standalone plugin: LLVM symbols are supplied by the loading compiler.
SOURCE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LLVM_ROOT ?= $(shell llvm-config --prefix 2>/dev/null)
LLVM_CONFIG := $(LLVM_ROOT)/bin/llvm-config
ifneq ($(shell "$(LLVM_CONFIG)" --version 2>/dev/null),22.1.8)
$(error LLVM_ROOT must select a complete LLVM 22.1.8 SDK)
endif
CXX := $(LLVM_ROOT)/bin/clang++
LLVM_CXXFLAGS := $(shell "$(LLVM_CONFIG)" --cxxflags)
BUILD_DIR ?= $(SOURCE_DIR)/../../build/llvm-native-symbols
BUILD_DIR := $(abspath $(BUILD_DIR))
CXXFLAGS ?= -O3 -DNDEBUG
TEST_LINK_OPTIONS ?=
TEST_CLANG ?= $(LLVM_ROOT)/bin/clang
COMPILE_FLAGS := $(LLVM_CXXFLAGS) $(CXXFLAGS) -fPIC -fvisibility=hidden \
  -fvisibility-inlines-hidden -isystem "$(LLVM_ROOT)/include" -Wall -Wextra -Werror
ifeq ($(shell uname -s),Darwin)
LIBRARY := $(BUILD_DIR)/llvm_native_symbols.dylib
LINK_FLAGS := -bundle -Wl,-undefined,dynamic_lookup
else ifeq ($(shell uname -s),Linux)
LIBRARY := $(BUILD_DIR)/llvm_native_symbols.so
LINK_FLAGS := -shared -Wl,--version-script,"$(SOURCE_DIR)/src/exports.map"
else
$(error supported plugin hosts are macOS and Linux)
endif

.PHONY: all test clean
all: $(LIBRARY)

$(BUILD_DIR)/Plugin.o: $(SOURCE_DIR)/src/Plugin.cpp $(SOURCE_DIR)/Makefile
	mkdir -p "$(BUILD_DIR)"
	$(CXX) $(COMPILE_FLAGS) -MMD -MP -c "$<" -o "$@"

$(LIBRARY): $(BUILD_DIR)/Plugin.o $(SOURCE_DIR)/src/exports.map $(SOURCE_DIR)/Makefile
	$(CXX) $(LLVM_CXXFLAGS) $(LINK_FLAGS) $(LDFLAGS) "$<" -o "$@"

test: all
	bash "$(SOURCE_DIR)/test/ir.sh" "$(LLVM_ROOT)/bin" "$(LIBRARY)" "$(BUILD_DIR)/test-ir"
	bash "$(SOURCE_DIR)/test/compile.sh" "$(TEST_CLANG)" "$(LIBRARY)" "$(BUILD_DIR)/test-compile"
	bash "$(SOURCE_DIR)/test/run.sh" "$(TEST_CLANG)" "$(LIBRARY)" "$(BUILD_DIR)/test-run" $(TEST_LINK_OPTIONS)

clean:
	$(RM) "$(BUILD_DIR)/Plugin.o" "$(BUILD_DIR)/Plugin.d" "$(LIBRARY)"

-include $(BUILD_DIR)/Plugin.d
