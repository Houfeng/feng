CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -I./include -I./src
UNIX_CPPFLAGS := -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
DARWIN_CPPFLAGS := -D_DARWIN_UNLIMITED_SELECT=1 -D_DARWIN_USE_64_BIT_INODE=1
LINUX_CPPFLAGS := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112

TARGET ?= libfeng_std_uv.a
HOST_OS :=   $(shell sh -c 'os=$$(uname -s); if [ "$$os" = Darwin ]; then printf macos; elif [ "$$os" = Linux ]; then printf linux; else printf %s "$$os" | tr "[:upper:]" "[:lower:]"; fi')
HOST_ARCH := $(if $(filter x86_64 amd64,$(shell uname -m)),x64,$(if $(filter arm64 aarch64,$(shell uname -m)),arm64,$(shell uname -m)))
HOST_PLATFORM := $(if $(filter linux,$(HOST_OS)),$(HOST_OS)-$(HOST_ARCH)-gnu,$(HOST_OS)-$(HOST_ARCH))
OUTPUT_DIR ?= ../../std/extlib/$(HOST_PLATFORM)
OUTPUT_NAME ?= $(TARGET)
OUTPUT_TARGET := $(OUTPUT_DIR)/$(OUTPUT_NAME)

COMMON_SRCS = src/fs-poll.c src/idna.c src/inet.c src/random.c src/strscpy.c src/strtok.c src/thread-common.c src/threadpool.c src/timer.c src/uv-common.c src/uv-data-getter-setters.c src/version.c
UNIX_BASE_SRCS = src/unix/async.c src/unix/core.c src/unix/dl.c src/unix/fs.c src/unix/getaddrinfo.c src/unix/getnameinfo.c src/unix/loop-watcher.c src/unix/loop.c src/unix/pipe.c src/unix/poll.c src/unix/process.c src/unix/random-devurandom.c src/unix/signal.c src/unix/stream.c src/unix/tcp.c src/unix/thread.c src/unix/tty.c src/unix/udp.c
DARWIN_SRCS = src/unix/proctitle.c src/unix/bsd-ifaddrs.c src/unix/kqueue.c src/unix/random-getentropy.c src/unix/darwin-proctitle.c src/unix/darwin.c src/unix/fsevents.c
LINUX_SRCS = src/unix/proctitle.c src/unix/linux.c src/unix/procfs-exepath.c src/unix/random-getrandom.c src/unix/random-sysctl-linux.c
WINDOWS_SRCS = src/win/async.c src/win/core.c src/win/detect-wakeup.c src/win/dl.c src/win/error.c src/win/fs.c src/win/fs-event.c src/win/getaddrinfo.c src/win/getnameinfo.c src/win/handle.c src/win/loop-watcher.c src/win/pipe.c src/win/thread.c src/win/poll.c src/win/process.c src/win/process-stdio.c src/win/signal.c src/win/snprintf.c src/win/stream.c src/win/tcp.c src/win/tty.c src/win/udp.c src/win/util.c src/win/winapi.c src/win/winsock.c

ifeq ($(HOST_OS),macos)
  SRCS = $(COMMON_SRCS) $(UNIX_BASE_SRCS) $(DARWIN_SRCS)
  CPPFLAGS += $(UNIX_CPPFLAGS) $(DARWIN_CPPFLAGS)
else ifeq ($(HOST_OS),linux)
  SRCS = $(COMMON_SRCS) $(UNIX_BASE_SRCS) $(LINUX_SRCS)
  CPPFLAGS += $(UNIX_CPPFLAGS) $(LINUX_CPPFLAGS)
else
  SRCS = $(COMMON_SRCS) $(WINDOWS_SRCS)
endif

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
