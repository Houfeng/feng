CC ?= gcc
AR ?= ar
# -O3: 开启极致优化
# -DMI_MALLOC_OVERRIDE: 允许作为全局替换默认 malloc
CFLAGS ?= -O3 -Wall -I./include -DMI_MALLOC_OVERRIDE

TARGET = libmimalloc.a

# mimalloc 官方推荐的单文件单体编译模式，能够让编译器做最极致的内联优化（LTO级性能）
SRCS = src/static.c
OBJS = src/static.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

src/static.o: src/static.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
