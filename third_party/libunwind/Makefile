CC ?= cc
CXX ?= c++
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra -fPIC
CXXFLAGS ?= $(CFLAGS) -std=c++11 -fno-exceptions -fno-rtti
CPPFLAGS ?= -I./include -I./src -DNDEBUG -D_LIBUNWIND_IS_NATIVE_ONLY -D_LIBUNWIND_DISABLE_VISIBILITY_ANNOTATIONS

ifeq ($(shell uname -s),Linux)
CPPFLAGS += -D_LIBUNWIND_USE_DLADDR=0
endif

TARGET ?= libfeng_unwind.a
OUTPUT_DIR ?= ../../build/lib
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

CSRC = src/Unwind-sjlj.c \
       src/UnwindLevel1.c \
       src/UnwindLevel1-gcc-ext.c
CXXSRC = src/libunwind.cpp
ASMSRC = src/UnwindRegistersRestore.S \
         src/UnwindRegistersSave.S

OBJS = $(CSRC:.c=.o) $(CXXSRC:.cpp=.o) $(ASMSRC:.S=.o)

all: $(OUTPUT_TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

$(OUTPUT_TARGET): $(TARGET)
	mkdir -p $(OUTPUT_DIR)
	cp $(TARGET) $(OUTPUT_TARGET)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

install: $(OUTPUT_TARGET)

clean:
	$(RM) $(OBJS) $(TARGET) $(OUTPUT_TARGET)

.PHONY: all install clean
