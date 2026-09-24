# Standalone Release plugin build; LLVM symbols resolve from the loading compiler.
SOURCE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LLVM_ROOT ?= $(shell llvm-config --prefix 2>/dev/null)
LLVM_CONFIG := $(LLVM_ROOT)/bin/llvm-config
LLVM_VERSION := $(shell "$(LLVM_CONFIG)" --version 2>/dev/null)
ifneq ($(LLVM_VERSION),22.1.8)
$(error LLVM_ROOT must select a complete LLVM 22.1.8 SDK)
endif

CXX := $(LLVM_ROOT)/bin/clang++
CC := $(LLVM_ROOT)/bin/clang
LLVM_CXXFLAGS := $(shell "$(LLVM_CONFIG)" --cxxflags)
CXXFLAGS ?= -O3 -DNDEBUG
TEST_LINK_OPTIONS ?=
UBSAN_TEST_LINK_OPTIONS ?= $(TEST_LINK_OPTIONS)
BUILD_DIR ?= $(SOURCE_DIR)/../../build/llvm-c-eh/local
BUILD_DIR := $(abspath $(BUILD_DIR))
OBJECTS := $(addprefix $(BUILD_DIR)/,Plugin.o Protocol.o Lowering.o)
DEPENDS := $(OBJECTS:.o=.d)
COMPILE_FLAGS := $(LLVM_CXXFLAGS) $(CXXFLAGS) -fPIC -fvisibility=hidden \
  -fvisibility-inlines-hidden -Wall -Wextra -Werror
PROJECT_FLAGS := -isystem "$(LLVM_ROOT)/include" -DCEH_PLUGIN_VERSION='"0.1.0"'

ifeq ($(shell uname -s),Darwin)
LIBRARY := $(BUILD_DIR)/llvm_c_eh.dylib
PLUGIN_LINK_FLAGS := -bundle -Wl,-undefined,dynamic_lookup
else ifeq ($(shell uname -s),Linux)
LIBRARY := $(BUILD_DIR)/llvm_c_eh.so
PLUGIN_LINK_FLAGS := -shared -Wl,--version-script,"$(SOURCE_DIR)/src/exports.map"
else
$(error llvm-c-eh supports macOS and Linux hosts)
endif

.PHONY: all test test-ubsan clean
all: $(LIBRARY)

$(LIBRARY): $(OBJECTS) $(SOURCE_DIR)/src/exports.map $(SOURCE_DIR)/Makefile
	$(CXX) $(LLVM_CXXFLAGS) $(PLUGIN_LINK_FLAGS) $(LDFLAGS) $(OBJECTS) -o "$@"

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/src/%.cpp $(SOURCE_DIR)/Makefile
	mkdir -p "$(BUILD_DIR)"
	$(CXX) $(COMPILE_FLAGS) $(PROJECT_FLAGS) -MMD -MP -c "$<" -o "$@"

# These tests consume the Release library; only test-ubsan instruments the consumer.
test: all
	bash "$(SOURCE_DIR)/test/ir.sh" "$(LLVM_ROOT)/bin" "$(LIBRARY)" "$(BUILD_DIR)/ir"
	bash "$(SOURCE_DIR)/test/run.sh" "$(CC)" "$(LIBRARY)" "$(BUILD_DIR)/test" $(TEST_LINK_OPTIONS)
	bash "$(SOURCE_DIR)/test/passthrough.sh" "$(CC)" "$(LIBRARY)" "$(BUILD_DIR)/passthrough" $(TEST_LINK_OPTIONS)

test-ubsan: all
	bash "$(SOURCE_DIR)/test/passthrough.sh" "$(CC)" "$(LIBRARY)" "$(BUILD_DIR)/passthrough-ubsan" --sanitizer $(UBSAN_TEST_LINK_OPTIONS)
	bash "$(SOURCE_DIR)/test/sanitizer.sh" "$(CC)" "$(LIBRARY)" "$(BUILD_DIR)/sanitizer-checks" $(UBSAN_TEST_LINK_OPTIONS)
	bash "$(SOURCE_DIR)/test/run.sh" "$(CC)" "$(LIBRARY)" "$(BUILD_DIR)/ubsan" --sanitizer $(UBSAN_TEST_LINK_OPTIONS)

# Remove only this project's build products, never a caller-owned output directory.
clean:
	$(RM) $(OBJECTS) $(DEPENDS) $(LIBRARY)

-include $(DEPENDS)
