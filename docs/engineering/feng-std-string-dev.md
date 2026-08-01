# std.text 字符串操作实现方案

## 1 背景

### 1.1 RuneView 与 GraphemeView（已交付）

`std/std/src/text/Rune.ff` 和 `std/std/src/text/Grapheme.ff` 提供 `RuneView` 和 `GraphemeView` 视图类型，
分别实现了 `length()` 和 `at(index)` 方法。通过 `@cdecl` 调用 `third_party/libunistring/` 的 UTF-8
rune/grapheme 子集，配合 `feng_pointer_move`/`feng_pointer_diff` 等 runtime 指针基础设施完成。

### 1.2 string 搜索与变换方法（本次目标）

为 `string` 类型（字节级）增加搜索和变换方法：`indexOf`、`lastIndexOf`、`contains`、
`startsWith`、`endsWith`、`replace`、`trim`、`trimStart`、`trimEnd`。

核心约束：
- `string` 在用户层语义不可变，所有变换方法返回新 string
- UTF-8 是自同步编码，字节级搜索对合法 UTF-8 子串完全正确
- 优先使用 Feng + 已有 C ABI 实现，仅在必要时扩展通用 runtime 能力

## 2 可用基础设施

### 2.1 Runtime 指针函数（已有）

- `feng_pointer_is_null(ptr)` — 指针判空
- `feng_pointer_equal(a, b)` — 指针比较
- `feng_pointer_move(ptr, offset)` — 指针偏移
- `feng_pointer_diff(a, b)` — 指针距离

### 2.2 Runtime 字符串函数（已有）

- `feng_string_utf8_length(value)` — UTF-8 字节长度
- `feng_string_slice(value, start, length)` — 字节级截取，返回新 string
- `feng_string_concat(left, right)` — 拼接，返回新 string（`+` 运算符底层调用）

### 2.3 libunistring（已有，via @cdecl）

- `u8_next(puc, s)` — 正向读取下一个 rune，输出 code point
- `u8_prev(puc, s, start)` — 反向读取上一个 rune，输出 code point
- `u8_mbsnlen(s, n)` — rune 计数
- `u8_grapheme_next(s, end)` / `u8_grapheme_prev(s, start)` — grapheme 遍历

### 2.4 Feng 层已有 string 方法

- `length()` — 字节长度
- `at(index)` — 单字节子串
- `clone()` / `clone(start, end)` — 字节区间拷贝
- `==` / `!=` — 全串字节比较

## 3 新增 runtime 函数

新增 1 个通用 runtime 函数，用于字节区间比较，避免搜索操作中的中间分配：

```c
/* 比较两个 FengString 的字节子区间是否相等。
 * 区间均为右开 [start, end)。当两个区间长度不等时返回 false。 */
bool feng_string_range_equal(FengString *a, int64_t a_start, int64_t a_end,
                             FengString *b, int64_t b_start, int64_t b_end) {
    size_t a_len, b_len, a_total, b_total, range_len;

    a_total = feng_string_length(a);
    b_total = feng_string_length(b);

    if (a_start < 0 || a_end < a_start || (size_t)a_end > a_total) {
        feng_panic("feng_string_range_equal: a range [%" PRId64 ", %" PRId64 ") "
                   "out of bounds (length=%zu)", a_start, a_end, a_total);
    }
    if (b_start < 0 || b_end < b_start || (size_t)b_end > b_total) {
        feng_panic("feng_string_range_equal: b range [%" PRId64 ", %" PRId64 ") "
                   "out of bounds (length=%zu)", b_start, b_end, b_total);
    }

    a_len = (size_t)(a_end - a_start);
    b_len = (size_t)(b_end - b_start);
    if (a_len != b_len) {
        return false;
    }

    range_len = a_len;
    if (range_len == 0U) {
        return true;
    }

    return memcmp(feng_string_data(a) + a_start,
                  feng_string_data(b) + b_start,
                  range_len) == 0;
}
```

设计要点：
- 是 `feng_string_equal`（全串比较）的区间版本，属于 string 类型的通用能力扩展
- 零中间分配，直接 `memcmp` 比较底层字节
- 边界检查对齐已有 `feng_string_slice` 的防御风格

## 4 方法签名与语义

所有方法通过 `fit string` 扩展到 `string` 类型上，定义在 `std/std/src/text/String.ff` 中。

### 4.1 搜索方法

| 方法 | 签名 | 返回值 |
|------|------|--------|
| `indexOf` | `func indexOf(target: string): long` | 首次出现的字节偏移，未找到返回 `-1` |
| `indexOf` | `func indexOf(target: string, from: long): long` | 从 `from` 开始首次出现的字节偏移 |
| `lastIndexOf` | `func lastIndexOf(target: string): long` | 最后一次出现的字节偏移 |
| `contains` | `func contains(target: string): bool` | 是否包含子串 |
| `startsWith` | `func startsWith(prefix: string): bool` | 前缀匹配 |
| `endsWith` | `func endsWith(suffix: string): bool` | 后缀匹配 |

### 4.2 变换方法

| 方法 | 签名 | 返回值 |
|------|------|--------|
| `replace` | `func replace(old: string, replacement: string): string` | 替换所有出现，返回新 string |
| `trim` | `func trim(): string` | 去除首尾 Unicode 空白，返回新 string |
| `trimStart` | `func trimStart(): string` | 去除首部 Unicode 空白 |
| `trimEnd` | `func trimEnd(): string` | 去除尾部 Unicode 空白 |

## 5 Feng 侧实现

### 5.1 搜索方法

搜索方法全部基于 `feng_string_range_equal` 实现，零中间分配。

```feng
@runtime
extern func feng_string_range_equal(
  a: string, a_start: long, a_end: long,
  b: string, b_start: long, b_end: long
): bool;

open fit string {

  /** 返回 target 首次出现的字节偏移，未找到返回 -1。 */
  open func indexOf(target: string): long {
    return self.indexOf(target, (long)0);
  }

  /** 从 from 字节偏移开始，返回 target 首次出现的字节偏移，未找到返回 -1。 */
  open func indexOf(target: string, from: long): long {
    let tlen = target.length();
    if tlen == (long)0 {
      return from;
    }
    let slen = self.length();
    if from < (long)0 {
      throw "String indexOf from out of bounds";
    }
    if from + tlen > slen {
      return (long)-1;
    }
    let limit = slen - tlen;
    var i = from;
    while i <= limit {
      if feng_string_range_equal(self, i, i + tlen, target, (long)0, tlen) {
        return i;
      }
      i += (long)1;
    }
    return (long)-1;
  }

  /** 返回 target 最后一次出现的字节偏移，未找到返回 -1。 */
  open func lastIndexOf(target: string): long {
    let tlen = target.length();
    if tlen == (long)0 {
      return self.length();
    }
    let slen = self.length();
    if tlen > slen {
      return (long)-1;
    }
    var i = slen - tlen;
    while i >= (long)0 {
      if feng_string_range_equal(self, i, i + tlen, target, (long)0, tlen) {
        return i;
      }
      i -= (long)1;
    }
    return (long)-1;
  }

  /** 返回是否包含子串 target。 */
  open func contains(target: string): bool {
    return self.indexOf(target) >= (long)0;
  }

  /** 返回是否以 prefix 开头。 */
  open func startsWith(prefix: string): bool {
    let plen = prefix.length();
    if plen == (long)0 {
      return true;
    }
    if plen > self.length() {
      return false;
    }
    return feng_string_range_equal(self, (long)0, plen, prefix, (long)0, plen);
  }

  /** 返回是否以 suffix 结尾。 */
  open func endsWith(suffix: string): bool {
    let slen = suffix.length();
    if slen == (long)0 {
      return true;
    }
    let len = self.length();
    if slen > len {
      return false;
    }
    return feng_string_range_equal(self, len - slen, len, suffix, (long)0, slen);
  }

}
```

### 5.2 replace

基于 `indexOf` 定位 + `clone` 截取 + `+` 拼接构建结果。

```feng
open fit string {

  /** 替换所有 old 出现为 replacement，返回新 string。 */
  open func replace(old: string, replacement: string): string {
    let olen = old.length();
    if olen == (long)0 {
      return self.clone();
    }
    let slen = self.length();
    var result = "";
    var pos: long = 0;
    var found = self.indexOf(old, pos);
    while found >= (long)0 {
      if found > pos {
        result = result + self.clone(pos, found);
      }
      result = result + replacement;
      pos = found + olen;
      found = self.indexOf(old, pos);
    }
    if pos == (long)0 {
      return self.clone();
    }
    if pos < slen {
      result = result + self.clone(pos, slen);
    }
    return result;
  }

}
```

### 5.3 trim 系列

使用 `u8_next`（正向）和 `u8_prev`（反向）遍历 rune 边界获取 code point，
对 code point 检查 Unicode 空白集合（25 个确定码位），找到首/尾非空白边界后
一次 `clone` 截取。

#### Unicode 空白码位表

```
U+0009 HT      U+000A LF      U+000B VT      U+000C FF      U+000D CR
U+0020 Space   U+0085 NEL     U+00A0 NBSP    U+1680 Ogham
U+2000–U+200A（11 个排版空格）
U+2028 行分隔   U+2029 段分隔   U+202F 窄NBSP  U+205F 中数学空格
U+3000 全角空格
```

#### 实现

`u8_prev` 需在 `std/std/src/text/Rune.ff` 中补充 `@cdecl` 声明（libunistring 已提供）。

```feng
/** 检查 Unicode code point 是否为空白字符。 */
func isUnicodeWhitespace(cp: u32): bool {
  if cp <= (u32)0x0020 {
    return cp == (u32)0x0009 || cp == (u32)0x000A || cp == (u32)0x000B
        || cp == (u32)0x000C || cp == (u32)0x000D || cp == (u32)0x0020;
  }
  if cp == (u32)0x0085 || cp == (u32)0x00A0 {
    return true;
  }
  if cp == (u32)0x1680 {
    return true;
  }
  if cp >= (u32)0x2000 && cp <= (u32)0x200A {
    return true;
  }
  if cp == (u32)0x2028 || cp == (u32)0x2029 {
    return true;
  }
  if cp == (u32)0x202F || cp == (u32)0x205F || cp == (u32)0x3000 {
    return true;
  }
  return false;
}

open fit string {

  /** 去除首部 Unicode 空白字符，返回新 string。 */
  open func trimStart(): string {
    let len = self.length();
    if len == (long)0 {
      return "";
    }
    let data: string* = &self;
    let end: string* = feng_pointer_move(data, len);
    var cursor: string* = data;
    var uc: u32 = 0;
    while !feng_pointer_equal(cursor, end) {
      let next: string* = u8_next(&uc, cursor);
      if feng_pointer_is_null(next) {
        break;
      }
      if !isUnicodeWhitespace(uc) {
        break;
      }
      cursor = next;
    }
    let start = feng_pointer_diff(cursor, data);
    if start == len {
      return "";
    }
    return self.clone(start, len);
  }

  /** 去除尾部 Unicode 空白字符，返回新 string。 */
  open func trimEnd(): string {
    let len = self.length();
    if len == (long)0 {
      return "";
    }
    let data: string* = &self;
    var cursor: string* = feng_pointer_move(data, len);
    var uc: u32 = 0;
    while !feng_pointer_equal(cursor, data) {
      let prev: string* = u8_prev(&uc, cursor, data);
      if feng_pointer_is_null(prev) {
        break;
      }
      if !isUnicodeWhitespace(uc) {
        break;
      }
      cursor = prev;
    }
    let trimmed = feng_pointer_diff(cursor, data);
    if trimmed == (long)0 {
      return "";
    }
    return self.clone((long)0, trimmed);
  }

  /** 去除首尾 Unicode 空白字符，返回新 string。 */
  open func trim(): string {
    return self.trimStart().trimEnd();
  }

}
```

## 6 性能分析

| 方法 | 时间复杂度 | 堆分配次数 |
|------|-----------|-----------|
| `indexOf` / `lastIndexOf` | O(n·m) 比较 | **0**（`feng_string_range_equal` 零分配） |
| `contains` | 同 indexOf | **0** |
| `startsWith` / `endsWith` | O(m) | **0** |
| `replace` | O(n·m) 搜索 + O(k) 拼接 | **O(k)**，k = 匹配次数 |
| `trimStart` / `trimEnd` | O(w) 扫描 + 1 次 clone | **1** |
| `trim` | trimStart + trimEnd | **2** |

## 7 变更文件

| 文件 | 操作 |
|------|------|
| `src/runtime/feng_runtime_contract.c` | 新增 `feng_string_range_equal` |
| `src/runtime/feng_runtime_contract.inc` | 注册 `feng_string_range_equal` |
| `std/std/src/text/Rune.ff` | 补充 `u8_prev` 的 `@cdecl` 声明 |
| `std/std/src/text/String.ff` | 新增 `feng_string_range_equal` 声明 + `isUnicodeWhitespace` + 10 个方法 |
| `std/std_test/src/test_string.ff` | 追加测试用例 |

## 8 测试用例

### 8.1 indexOf / lastIndexOf
- `"hello world".indexOf("world") == 6`
- `"hello world".indexOf("xyz") == -1`
- `"hello".indexOf("") == 0`
- `"aabaa".indexOf("a", 2) == 3`
- `"aabaa".lastIndexOf("a") == 4`
- `"你好世界".indexOf("世界") == 6`（字节偏移）

### 8.2 contains / startsWith / endsWith
- `"hello world".contains("world") == true`
- `"hello world".contains("xyz") == false`
- `"hello".startsWith("hel") == true`
- `"hello".startsWith("world") == false`
- `"hello".endsWith("llo") == true`
- `"hello".endsWith("hel") == false`
- `"".startsWith("") == true`

### 8.3 replace
- `"hello world".replace("world", "feng") == "hello feng"`
- `"aaa".replace("a", "bb") == "bbbbbb"`
- `"hello".replace("xyz", "abc") == "hello"`
- `"hello".replace("", "x") == "hello"`（空 old 不替换）

### 8.4 trim
- `" hello ".trim() == "hello"`
- `"  hello".trimStart() == "hello"`
- `"hello  ".trimEnd() == "hello"`
- `"hello".trim() == "hello"`（无空白不变）
- `"   ".trim() == ""`（全空白）
- `"\t\n hello \r\n".trim() == "hello"`（混合 ASCII 空白）
- 含 Unicode 空白（全角空格 U+3000 等）的 trim 测试

## 9 扩展方法（第二批）

### 9.1 新增可用基础设施

通过扩展 libunistring vendor，增加 unicase 模块：

- `uc_tolower(ucs4_t)` — 码位级小写映射（via @cdecl）
- `uc_toupper(ucs4_t)` — 码位级大写映射（via @cdecl）

已有 runtime 函数复用：

- `feng_string_to_utf8_bytes(value)` — 返回 UTF-8 字节数组（已在 runtime 中，新增 String.ff 声明）

### 9.2 方法签名与语义

| 方法 | 签名 | 返回值 |
|------|------|--------|
| `isEmpty` | `func isEmpty(): bool` | 是否为空串 |
| `repeat` | `func repeat(count: long): string` | 重复 count 次 |
| `toBytes` | `func toBytes(): byte[]` | UTF-8 字节数组 |
| `split` | `func split(separator: string): string[]` | 按分隔符拆分 |
| `join` | `static func join(separator: string, parts: string[]): string` | 数组用分隔符拼接 |
| `toUpperCase` | `func toUpperCase(): string` | Unicode 大写转换 |
| `toLowerCase` | `func toLowerCase(): string` | Unicode 小写转换 |
| `padStart` | `func padStart(targetLength: long, pad: string): string` | 左填充至目标长度 |
| `padEnd` | `func padEnd(targetLength: long, pad: string): string` | 右填充至目标长度 |

### 9.3 实现方案

#### isEmpty / repeat / toBytes

纯 Feng 实现，无新 C 依赖。

```feng
open func isEmpty(): bool {
  return self.length() == (long)0;
}

open func repeat(count: long): string {
  // 循环拼接
}

open func toBytes(): byte[] {
  return feng_string_to_utf8_bytes(self);
}
```

#### split / join

`split` 使用 `List<string>` 收集片段 + `entries()` 返回 `string[]`。
`join` 遍历 `string[]` 用 `+` 拼接。

```feng
open func split(separator: string): string[] {
  // indexOf 定位 + clone 截取 + List<string> 收集
}

open static func join(separator: string, parts: string[]): string {
  // 遍历 parts 用 separator 拼接
}
```

#### toUpperCase / toLowerCase

声明 `@cdecl("feng_std_unistring", "uc_tolower")` / `uc_toupper`。
遍历码位 → 映射 → 纯 Feng UTF-8 编码 → `List<byte>` → `string.fromUtf8Bytes`。

```feng
@cdecl("feng_std_unistring", "uc_toupper")
extern func uc_toupper(uc: u32): u32;

@cdecl("feng_std_unistring", "uc_tolower")
extern func uc_tolower(uc: u32): u32;

open func toUpperCase(): string {
  // u8_next 遍历 → uc_toupper 映射 → UTF-8 编码到 List<byte> → fromUtf8Bytes
}
```

#### padStart / padEnd

计算所需填充字节数，用 `repeat` + `clone` 截取到精确长度后拼接。

### 9.4 性能分析

| 方法 | 时间复杂度 | 堆分配 |
|------|-----------|--------|
| `isEmpty` | O(1) | **0** |
| `repeat` | O(n·count) | **O(count)** 拼接 |
| `toBytes` | O(n) | **1** |
| `split` | O(n·m) 搜索 | **O(k)** k=片段数 |
| `join` | O(total) | **O(k)** 拼接 |
| `toUpperCase`/`toLowerCase` | O(n) 遍历 | **1**（List + fromUtf8Bytes） |
| `padStart`/`padEnd` | O(targetLength) | **O(1)**~**O(2)** |

### 9.5 测试用例

#### split
- `"a,b,c".split(",")` → `["a","b","c"]`
- `"hello".split(",")` → `["hello"]`
- `",,".split(",")` → `["","",""]`
- `"".split(",")` → `[""]`
- `"a".split("")` → `["a"]`（空分隔符返回原串数组）

#### toUpperCase / toLowerCase
- `"hello".toUpperCase() == "HELLO"`
- `"HELLO".toLowerCase() == "hello"`
- `"Hello World".toUpperCase() == "HELLO WORLD"`
- `"café".toUpperCase() == "CAFÉ"`
- `"CAFÉ".toLowerCase() == "café"`
- `"你好".toUpperCase() == "你好"`（非拉丁字符不变）
- `"".toUpperCase() == ""`

#### repeat
- `"ab".repeat(3) == "ababab"`
- `"x".repeat(0) == ""`
- `"".repeat(5) == ""`
- `"hi".repeat(1) == "hi"`

#### isEmpty
- `"".isEmpty() == true`
- `"a".isEmpty() == false`
- `" ".isEmpty() == false`

#### join
- `string.join(",", ["a","b","c"]) == "a,b,c"`
- `string.join(",", []) == ""`
- `string.join("", ["a","b"]) == "ab"`
- `string.join("--", ["x"]) == "x"`

#### toBytes
- `"hello".toBytes().length() == 5`
- `"你好".toBytes().length() == 6`
- `"".toBytes().length() == 0`

#### padStart / padEnd
- `"hi".padStart((long)5, "0") == "000hi"`
- `"hello".padStart((long)3, "0") == "hello"`（已够长不变）
- `"hi".padEnd((long)5, "0") == "hi000"`
- `"hello".padEnd((long)3, "0") == "hello"`
- `"hi".padStart((long)5, "ab") == "abahi"`（多字符填充）
- `"hi".padEnd((long)5, "ab") == "hiaba"`

### 9.6 变更文件

| 文件 | 操作 |
|------|------|
| `scripts/fetch_libunistring.sh` | 增加 unicase 模块同步 |
| `third_party/libunistring/Makefile` | 新增 unicase 源文件 |
| `third_party/libunistring/include/feng_u8_case.h` | 新增公开头 |
| `std/std/src/text/String.ff` | 新增 9 个方法 + @cdecl 声明 |
| `std/std_test/src/test_string.ff` | 追加测试用例 |

## 10 关联

- [feng-extlib-draft.md](./feng-extlib-draft.md) — libunistring 选型与 vendoring 约定
- `third_party/libunistring/README.md` — 可用 API 清单
