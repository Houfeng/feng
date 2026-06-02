# std.text RuneView 与 GraphemeView 实现方案

## 1 背景

`std/src/text/RuneView.ff` 和 `std/src/text/Grapheme.ff` 当前仅有骨架（type 定义 + `length()` 占位返回 0）。`third_party/libunistring/` 已集成 UTF-8 rune/grapheme 子集，编译为 `libfeng_std_unistring.a`，通过 `std/extlib/` 在用户程序链接时自动引入。

目标：实现 `RuneView` 和 `GraphemeView` 的 `length()` 和 `at(index)` 方法。

## 2 可用的 libunistring API

头文件位于 `third_party/libunistring/include/`：

**feng_u8_rune.h**：
- `size_t u8_mbsnlen(const uint8_t *s, size_t n)` — n 字节中的 rune 数量
- `const uint8_t *u8_next(ucs4_t *puc, const uint8_t *s)` — 读取下一个 rune，返回下一位置（到达末尾返回 NULL）

**feng_u8_grapheme.h**：
- `const uint8_t *u8_grapheme_next(const uint8_t *s, const uint8_t *end)` — 下一个 grapheme 边界（`s == end` 时返回 NULL）

## 3 核心约束

- `&string` 零拷贝获取 string 内部 data 的 `char*` 裸指针
- `@cdecl("feng_std_unistring", ...)` 可直接调用 libunistring 函数
- 但 Feng 当前仅有 `feng_pointer_is_null` 和 `feng_pointer_equal`，缺少指针算术
- libunistring 的迭代 API 需要计算 `end = data + length` 指针偏移，以及 `next - cursor` 字节距离

## 4 方案：新增两个通用 runtime 指针函数

在 `src/runtime/feng_runtime_contract.c` 中新增：

```c
/* 将指针移动 offset 字节，返回新位置。 */
void *feng_pointer_move(void *ptr, int64_t offset) {
    return (char *)ptr + offset;
}

/* 返回两个指针之间的字节距离（a - b）。 */
int64_t feng_pointer_diff(void *a, void *b) {
    return (int64_t)((char *)a - (char *)b);
}
```

这两个是通用的 C 互操作基础设施，与 rune/grapheme 无关：
- `feng_pointer_move` — 指针偏移，用于计算 end 指针或前进 cursor
- `feng_pointer_diff` — 指针距离，用于将指针位置转换回字节偏移量

## 5 Feng 侧实现

### 5.1 RuneView.ff

```feng
open module std.text;

@cdecl("feng_std_unistring", "u8_mbsnlen")
extern func u8_mbsnlen(s: string*, n: long): long;

@cdecl("feng_std_unistring", "u8_next")
extern func u8_next(puc: u32*, s: string*): string*;

@runtime
extern func feng_pointer_move(ptr: string*, offset: long): string*;

@runtime
extern func feng_pointer_diff(a: string*, b: string*): long;

open fit string {
  open func runes(): RuneView {
    return RuneView(self);
  }
}

open type RuneView {
  seal let text: string;

  open func RuneView(text: string) {
    self.text = text;
  }

  /** Returns the number of runes (Unicode scalar values) in this string. */
  open func length(): long {
    return u8_mbsnlen(&self.text, self.text.length());
  }

  /** Returns the substring of the rune at the given logical index. */
  open func at(index: long): string {
    if index < 0 {
      throw "RuneView index out of bounds";
    }
    let data: string* = &self.text;
    var cursor: string* = data;
    var uc: u32 = 0;
    var i: long = 0;
    while i < index {
      cursor = u8_next(&uc, cursor);
      if feng_pointer_is_null(cursor) {
        throw "RuneView index out of bounds";
      }
      i += 1;
    }
    let next: string* = u8_next(&uc, cursor);
    if feng_pointer_is_null(next) {
      throw "RuneView index out of bounds";
    }
    let start = feng_pointer_diff(cursor, data);
    let end = feng_pointer_diff(next, data);
    return self.text.clone(start, end);
  }
}
```

### 5.2 Grapheme.ff

```feng
open module std.text;

@cdecl("feng_std_unistring", "u8_grapheme_next")
extern func u8_grapheme_next(s: string*, end: string*): string*;

@runtime
extern func feng_pointer_move(ptr: string*, offset: long): string*;

@runtime
extern func feng_pointer_diff(a: string*, b: string*): long;

@runtime
extern func feng_pointer_is_null(ptr: string*): bool;

@runtime
extern func feng_pointer_equal(a: string*, b: string*): bool;

open fit string {
  open func graphemes(): GraphemeView {
    return GraphemeView(self);
  }
}

open type GraphemeView {
  seal let text: string;

  open func GraphemeView(text: string) {
    self.text = text;
  }

  /** Returns the number of grapheme clusters in this string. */
  open func length(): long {
    let data: string* = &self.text;
    let end: string* = feng_pointer_move(data, self.text.length());
    var cursor: string* = data;
    var count: long = 0;
    while !feng_pointer_equal(cursor, end) {
      cursor = u8_grapheme_next(cursor, end);
      if feng_pointer_is_null(cursor) {
        break;
      }
      count += 1;
    }
    return count;
  }

  /** Returns the substring of the grapheme cluster at the given logical index. */
  open func at(index: long): string {
    if index < 0 {
      throw "GraphemeView index out of bounds";
    }
    let data: string* = &self.text;
    let end: string* = feng_pointer_move(data, self.text.length());
    var cursor: string* = data;
    var i: long = 0;
    while i < index {
      cursor = u8_grapheme_next(cursor, end);
      if feng_pointer_is_null(cursor) {
        throw "GraphemeView index out of bounds";
      }
      i += 1;
    }
    if feng_pointer_equal(cursor, end) {
      throw "GraphemeView index out of bounds";
    }
    let next: string* = u8_grapheme_next(cursor, end);
    let start = feng_pointer_diff(cursor, data);
    let cluster_end: long = 0;
    if feng_pointer_is_null(next) {
      cluster_end = self.text.length();
    } else {
      cluster_end = feng_pointer_diff(next, data);
    }
    return self.text.clone(start, cluster_end);
  }
}
```

## 6 变更文件

| 文件 | 操作 |
|------|------|
| `src/runtime/feng_runtime_contract.c` | 新增 `feng_pointer_move` 和 `feng_pointer_diff` |
| `std/src/text/RuneView.ff` | 重写 |
| `std/src/text/Grapheme.ff` | 重写 |
| `std_test/src/test_string.ff` | 追加 rune/grapheme 测试 |

## 7 测试用例

- `"hello".runes().length() == 5`
- `"你好".runes().length() == 2`
- `"你好".runes().at(1) == "好"`
- `"abc".graphemes().length() == 3`
- ZWJ emoji（如 `"👨‍👩‍👧‍👦"`）grapheme 长度为 1
- `at(index)` 越界抛出异常

## 8 关联

- [feng-extlib-draft.md](./feng-extlib-draft.md) — libunistring 选型与 vendoring 约定
- `third_party/libunistring/README.md` — 可用 API 清单
