# std.text RegExp 正则表达式实现方案

> 完整平台标识与 `std/extlib/<platform>/` 目录取值以 [../docs/feng-os-arch.md](../docs/feng-os-arch.md) 为准。

## 1 背景

### 1.1 PCRE2 静态库（已就绪）

`third_party/PCRE2/` 已 vendor PCRE2 10.45 源码，配置为 8-bit 静态库 + Unicode/UTF 支持，
构建产物为 `std/extlib/<platform>/libfeng_std_pcre2.a`。

当前编译宏：
- `PCRE2_CODE_UNIT_WIDTH=8` — 固定 8-bit code unit，与 Feng string 的 UTF-8 字节表示一致
- `HAVE_CONFIG_H` — 使用 vendor 固定的 `config.h`

预构建使用的完整 host 平台、bundled LLVM、SDK / sysroot、构建隔离及产物校验
统一遵循 [feng-std-extlib-build.md](./feng-std-extlib-build.md)。

### 1.2 目标

基于 PCRE2 静态库 + `@cdecl` C ABI 互操作，为 Feng 标准库实现 `RegExp` 类型，
提供编译、匹配、搜索、分割、替换等正则表达式能力。

核心约束：
- PCRE2 不透明结构（`pcre2_code`、`pcre2_match_data`）通过 `@abi seal type` 在 Feng 侧表示为不透明指针
- 编译后的 `pcre2_code` 可复用，避免重复编译开销
- 所有返回 string 的操作返回新 string，与 string 不可变语义一致
- 匹配失败不抛异常，通过返回值表达（`None`、空数组、`-1` 等）
- 正则编译错误通过 `throw` 报告

## 2 可用基础设施

### 2.1 PCRE2 C API（via @cdecl）

需要调用的 PCRE2 函数：

| C 函数 | 用途 |
|--------|------|
| `pcre2_compile_8` | 编译正则表达式 pattern |
| `pcre2_code_free_8` | 释放编译后的 code |
| `pcre2_match_data_create_from_pattern_8` | 根据 pattern 创建 match_data |
| `pcre2_match_data_free_8` | 释放 match_data |
| `pcre2_match_8` | 执行匹配 |
| `pcre2_get_ovector_pointer_8` | 获取 ovector（偏移量数组） |
| `pcre2_get_ovector_count_8` | 获取 ovector 捕获组数量 |
| `pcre2_get_error_message_8` | 获取编译/匹配错误信息 |
| `pcre2_pattern_info_8` | 查询 pattern 信息（如捕获组数量） |
| `pcre2_substitute_8` | 执行替换 |

说明：PCRE2 在 `PCRE2_CODE_UNIT_WIDTH=8` 下，所有公开符号展开后带 `_8` 后缀。

### 2.2 已有 Feng 基础设施

- `feng_alloc(size)` / `feng_free(ptr)` — 原生内存分配/释放（`@runtime`）
- `feng_string_utf8_length(value)` — string 字节长度
- `feng_string_to_utf8_bytes(value)` — string → byte[]
- `feng_string_from_utf8_bytes(value, length)` — byte[] → string
- `feng_string_slice(value, start, length)` — 字节级子串截取
- `@abi seal type` — 不透明 C 结构指针声明
- `@cdecl("feng_std_pcre2", ...)` — 链接 PCRE2 静态库中的符号

## 3 新增 runtime API

### 3.1 `feng_pointer_get_scalar<T>`

PCRE2 的 ovector 是 `size_t*` 数组，Feng 指针不透明，无法解引用。
新增一个通用 runtime 指针标量读取原语，与已有指针原语形成完整闭包：

| 已有 API | 能力 |
|----------|------|
| `feng_pointer_is_null` | 判空 |
| `feng_pointer_equal` | 比较 |
| `feng_pointer_move` | 偏移 |
| `feng_pointer_diff` | 距离 |
| **`feng_pointer_get_scalar`（新增）** | **读取标量值** |

Feng 声明：

```feng
@runtime
extern func feng_pointer_get_scalar<T>(ptr: T*): T;
```

C 实现：

```c
/* 读取 ptr 处的标量值。
 * T 必须是 ABI 兼容标量类型（int/long/byte/u32/float/double/bool/enum）。
 * 非标量类型 panic。NULL 指针 panic。
 * 偏移通过 feng_pointer_move 组合完成。 */
void feng_pointer_get_scalar(const FengGenericParamDescriptor *type,
                             void *ptr, void *result) {
    if (type->type_kind > FENG_TYPE_KIND_SCALAR_MAX) {
        feng_panic("feng_pointer_get_scalar: only scalar types allowed");
    }
    if (ptr == NULL) {
        feng_panic("feng_pointer_get_scalar: null pointer");
    }
    memcpy(result, ptr, type->size);
}
```

使用方式（组合 `feng_pointer_move` 做偏移）：

```feng
let ovector = pcre2_get_ovector_pointer(md);
let start = feng_pointer_get_scalar<long>(feng_pointer_move(ovector, i * (long)16));
let end   = feng_pointer_get_scalar<long>(feng_pointer_move(ovector, i * (long)16 + (long)8));
```

约束：
- `T` 仅限 ABI 兼容标量类型，非标量（string、T[]、type 实例）runtime panic
- NULL 指针 runtime panic
- 指针偏移通过已有 `feng_pointer_move` 完成，职责单一

## 4 Feng 侧类型设计

### 4.1 不透明 C 类型声明

```feng
/* PCRE2 编译后的正则表达式对象（不透明） */
@abi
seal type Pcre2Code {}

/* PCRE2 匹配结果数据（不透明） */
@abi
seal type Pcre2MatchData {}
```

### 4.2 @cdecl 声明

```feng
/* ---------- PCRE2 核心 API ---------- */

@cdecl("feng_std_pcre2", "pcre2_compile_8")
extern func pcre2_compile(
  pattern: byte*, length: long, options: u32,
  errorcode: int*, erroroffset: long*,
  ccontext: long
): Pcre2Code*;

@cdecl("feng_std_pcre2", "pcre2_code_free_8")
extern func pcre2_code_free(code: Pcre2Code*): void;

@cdecl("feng_std_pcre2", "pcre2_match_data_create_from_pattern_8")
extern func pcre2_match_data_create_from_pattern(
  code: Pcre2Code*, gcontext: long
): Pcre2MatchData*;

@cdecl("feng_std_pcre2", "pcre2_match_data_free_8")
extern func pcre2_match_data_free(match_data: Pcre2MatchData*): void;

@cdecl("feng_std_pcre2", "pcre2_match_8")
extern func pcre2_match(
  code: Pcre2Code*, subject: byte*, length: long, startoffset: long,
  options: u32, match_data: Pcre2MatchData*, mcontext: long
): int;

@cdecl("feng_std_pcre2", "pcre2_get_ovector_pointer_8")
extern func pcre2_get_ovector_pointer(match_data: Pcre2MatchData*): long*;

@cdecl("feng_std_pcre2", "pcre2_get_ovector_count_8")
extern func pcre2_get_ovector_count(match_data: Pcre2MatchData*): u32;

@cdecl("feng_std_pcre2", "pcre2_get_error_message_8")
extern func pcre2_get_error_message(errorcode: int, buffer: byte*, buffer_size: long): int;

@cdecl("feng_std_pcre2", "pcre2_substitute_8")
extern func pcre2_substitute(
  code: Pcre2Code*, subject: byte*, length: long, startoffset: long,
  options: u32, match_data: Pcre2MatchData*, mcontext: long,
  replacement: byte*, rlength: long,
  outputbuffer: byte*, outlengthptr: long*
): int;

/* ---------- runtime 指针原语 ---------- */

@runtime
extern func feng_pointer_get_scalar<T>(ptr: T*): T;

@runtime
extern func feng_pointer_move(ptr: long*, offset: long): long*;

@runtime
extern func feng_pointer_is_null(ptr: Pcre2Code*): bool;
```

说明：
- `ccontext`/`gcontext`/`mcontext` 参数为可选 PCRE2 上下文指针，传 `(long)0` 表示使用默认
- `Pcre2Code*` / `Pcre2MatchData*` 为不透明指针，Feng 侧不解引用
- `pcre2_get_ovector_pointer` 返回 `long*`（C 侧为 `size_t*`，64-bit 平台等宽），通过 `feng_pointer_get_scalar<long>` + `feng_pointer_move` 读取各组偏移量

### 4.3 RegExpFlag 枚举

```feng
open enum RegExpFlag {
  /* 大小写不敏感匹配 */
  CaseInsensitive = 0x00000008,
  /* 多行模式：^ 和 $ 匹配行边界 */
  Multiline = 0x00000400,
  /* 单行模式：. 匹配包括换行在内的任意字符 */
  DotAll = 0x00000020,
  /* 扩展模式：忽略未转义的空白和 # 注释 */
  Extended = 0x00000080,
  /* 非贪婪模式：量词默认最短匹配 */
  Ungreedy = 0x00040000,
}
```

值直接对应 PCRE2 的 `uint32_t` 编译选项常量。公开构造函数只接受 `RegExpFlag` 枚举形态，底层 `u32` 组合仅作为内部实现细节使用。
UTF-8 和 UCP 模式（`PCRE2_UTF | PCRE2_UCP`）在 `RegExp` 构造函数中强制启用，
用户不需要手动指定。

### 4.4 Match 类型

```feng
/**
 * Represents a single regex match result, including captured groups.
 */
open type Match {
  /** 匹配的完整子串 */
  open let text: string;
  /** 匹配起始的字节偏移（相对原始 subject） */
  open let start: long;
  /** 匹配结束的字节偏移（右开，相对原始 subject） */
  open let end: long;
  /** 捕获组数组，groups[0] 为整体匹配，groups[1..] 为各捕获组 */
  open let groups: string[];
}
```

### 4.5 RegExp 类型

```feng
/**
 * A compiled regular expression backed by PCRE2.
 * Instances are immutable and thread-safe after construction.
 */
open type RegExp {
  seal let code: long;
  seal let pattern: string;
  seal let flagBits: u32;

  /**
   * Compiles a regular expression pattern.
   * UTF-8 and Unicode property matching are enabled by default.
   * @param pattern - the regex pattern string
   * @throws string if the pattern is invalid
   */
  open func RegExp(pattern: string) { ... }

  /**
   * Compiles a regular expression pattern with a single RegExpFlag.
   * @param pattern - the regex pattern string
   * @param flag - a RegExpFlag value
   * @throws string if the pattern is invalid
   */
  open func RegExp(pattern: string, flag: RegExpFlag) { ... }

  /**
   * Returns whether this regex matches the entire subject string.
   * @param subject - the string to test
   * @return true if the entire subject matches
   */
  open func isMatch(subject: string): bool { ... }

  /**
   * Returns whether this regex matches anywhere within the subject string.
   * @param subject - the string to test
   * @return true if any substring matches
   */
  open func test(subject: string): bool { ... }

  /**
   * Finds the first match in the subject string.
   * @param subject - the string to search
   * @return Match if found, None if no match
   */
  open func find(subject: string): None | Match { ... }

  /**
   * Finds all non-overlapping matches in the subject string.
   * @param subject - the string to search
   * @return array of Match objects
   */
  open func findAll(subject: string): Match[] { ... }

  /**
   * Splits the subject string by matches of this regex.
   * @param subject - the string to split
   * @return array of substrings between matches
   */
  open func split(subject: string): string[] { ... }

  /**
   * Replaces the first match with the replacement string.
   * Replacement may use $0 for full match and $1..$9 for captured groups.
   * @param subject - the string to search
   * @param replacement - the replacement string (supports backreferences)
   * @return new string with the first match replaced
   */
  open func replaceFirst(subject: string, replacement: string): string { ... }

  /**
   * Replaces all matches with the replacement string.
   * Replacement may use $0 for full match and $1..$9 for captured groups.
   * @param subject - the string to search
   * @param replacement - the replacement string (supports backreferences)
   * @return new string with all matches replaced
   */
  open func replaceAll(subject: string, replacement: string): string { ... }

  /**
   * Releases the underlying PCRE2 resources.
   * Must be called when the RegExp is no longer needed.
   */
  open func destroy(): void { ... }
}
```

## 5 方法签名与语义

### 5.1 构造与销毁

| 方法 | 签名 | 语义 |
|------|------|------|
| `RegExp(pattern)` | `func RegExp(pattern: string)` | 编译 pattern，默认 UTF-8 + UCP |
| `RegExp(pattern, flag)` | `func RegExp(pattern: string, flag: RegExpFlag)` | 编译 pattern，附加单个公开枚举 flag |
| `destroy()` | `func destroy(): void` | 释放 `pcre2_code`，之后不可再使用 |

### 5.2 匹配测试

| 方法 | 签名 | 返回值 |
|------|------|--------|
| `isMatch` | `func isMatch(subject: string): bool` | 是否整串匹配（锚定首尾） |
| `test` | `func test(subject: string): bool` | 是否存在任意匹配 |

### 5.3 搜索

| 方法 | 签名 | 返回值 |
|------|------|--------|
| `find` | `func find(subject: string): None \| Match` | 首个匹配或 None |
| `findAll` | `func findAll(subject: string): Match[]` | 所有非重叠匹配 |

### 5.4 分割与替换

| 方法 | 签名 | 返回值 |
|------|------|--------|
| `split` | `func split(subject: string): string[]` | 按匹配拆分 |
| `replaceFirst` | `func replaceFirst(subject: string, replacement: string): string` | 替换首个 |
| `replaceAll` | `func replaceAll(subject: string, replacement: string): string` | 替换全部 |

## 6 Feng 侧实现

### 6.1 构造函数

```feng
open func RegExp(pattern: string) {
  self.pattern = pattern;
  /* 强制启用 UTF-8 (0x00080000) 和 UCP (0x00020000) */
  self.flagBits = (u32)0x00080000 | (u32)0x00020000;
  self.code = compilePattern(pattern, self.flagBits);
}

open func RegExp(pattern: string, flag: RegExpFlag) {
  self.pattern = pattern;
  self.flagBits = (u32)(int)flag | (u32)0x00080000 | (u32)0x00020000;
  self.code = compilePattern(pattern, self.flagBits);
}
```

其中 `compilePattern` 是文件私有辅助函数，包装 `pcre2_compile` 调用与错误转换：上述两个公开构造函数以及 `String.ff` 中的便捷方法均由其负责生成 `pcre2_code` 句柄。

### 6.2 test / isMatch

```feng
open func test(subject: string): bool {
  let subjectBytes = subject.toBytes();
  let md = pcre2_match_data_create_from_pattern(self.code, (long)0);
  let rc = pcre2_match(
    self.code, &subjectBytes, (long)subjectBytes.length(),
    (long)0, (u32)0, md, (long)0
  );
  pcre2_match_data_free(md);
  return rc >= 0;
}

open func isMatch(subject: string): bool {
  let subjectBytes = subject.toBytes();
  let md = pcre2_match_data_create_from_pattern(self.code, (long)0);
  /* PCRE2_ANCHORED (0x80000000) | PCRE2_ENDANCHORED (0x20000000) */
  let rc = pcre2_match(
    self.code, &subjectBytes, (long)subjectBytes.length(),
    (long)0, (u32)0x80000000 | (u32)0x20000000, md, (long)0
  );
  pcre2_match_data_free(md);
  return rc >= 0;
}
```

### 6.3 find

```feng
open func find(subject: string): None | Match {
  let subjectBytes = subject.toBytes();
  let md = pcre2_match_data_create_from_pattern(self.code, (long)0);
  let rc = pcre2_match(
    self.code, &subjectBytes, (long)subjectBytes.length(),
    (long)0, (u32)0, md, (long)0
  );
  if rc < 0 {
    pcre2_match_data_free(md);
    return None();
  }
  let result = buildMatch(subject, md);
  pcre2_match_data_free(md);
  return result;
}
```

### 6.4 findAll

```feng
open func findAll(subject: string): Match[] {
  let subjectBytes = subject.toBytes();
  let subjectLen = (long)subjectBytes.length();
  let matches = List<Match>();
  let md = pcre2_match_data_create_from_pattern(self.code, (long)0);
  var offset: long = 0;
  while offset <= subjectLen {
    let rc = pcre2_match(
      self.code, &subjectBytes, subjectLen,
      offset, (u32)0, md, (long)0
    );
    if rc < 0 {
      break;
    }
    let m = buildMatch(subject, md);
    matches.add(m);
    /* 前进到匹配结束位置；若空匹配则至少前进 1 字节 */
    if m.end == offset {
      offset += (long)1;
    } else {
      offset = m.end;
    }
  }
  pcre2_match_data_free(md);
  return matches.entries();
}
```

### 6.5 buildMatch（内部辅助）

```feng
/* PCRE2_UNSET 在 64-bit 平台上为 ~(size_t)0 = 0xFFFFFFFFFFFFFFFF，
 * 作为 long 读取后等于 -1。 */
seal let PCRE2_UNSET: long = (long)-1;

seal func ovectorStart(ovector: long*, index: u32): long {
  return feng_pointer_get_scalar<long>(
    feng_pointer_move(ovector, (long)index * (long)16)
  );
}

seal func ovectorEnd(ovector: long*, index: u32): long {
  return feng_pointer_get_scalar<long>(
    feng_pointer_move(ovector, (long)index * (long)16 + (long)8)
  );
}

seal func buildMatch(subject: string, md: Pcre2MatchData*): Match {
  let ovector = pcre2_get_ovector_pointer(md);
  let ovCount = pcre2_get_ovector_count(md);
  let groups = List<string>();
  var i: u32 = 0;
  while i < ovCount {
    let s = ovectorStart(ovector, i);
    let e = ovectorEnd(ovector, i);
    if s == PCRE2_UNSET || e == PCRE2_UNSET {
      groups.add("");
    } else {
      groups.add(subject.clone(s, e));
    }
    i += (u32)1;
  }
  let matchStart = ovectorStart(ovector, (u32)0);
  let matchEnd = ovectorEnd(ovector, (u32)0);
  let matchText = subject.clone(matchStart, matchEnd);
  return Match {
    text: matchText,
    start: matchStart,
    end: matchEnd,
    groups: groups.entries()
  };
}
```

### 6.6 split

```feng
open func split(subject: string): string[] {
  let subjectBytes = subject.toBytes();
  let subjectLen = (long)subjectBytes.length();
  let parts = List<string>();
  let md = pcre2_match_data_create_from_pattern(self.code, (long)0);
  var pos: long = 0;
  while pos <= subjectLen {
    let rc = pcre2_match(
      self.code, &subjectBytes, subjectLen,
      pos, (u32)0, md, (long)0
    );
    if rc < 0 {
      break;
    }
    let ovector = pcre2_get_ovector_pointer(md);
    let matchStart = ovectorStart(ovector, (u32)0);
    let matchEnd = ovectorEnd(ovector, (u32)0);
    if matchStart > pos {
      parts.add(subject.clone(pos, matchStart));
    } else {
      parts.add("");
    }
    if matchEnd == pos {
      pos += (long)1;
    } else {
      pos = matchEnd;
    }
  }
  /* 追加最后一段 */
  if pos <= subjectLen {
    if pos < subjectLen {
      parts.add(subject.clone(pos, subjectLen));
    } else {
      parts.add("");
    }
  }
  pcre2_match_data_free(md);
  return parts.entries();
}
```

### 6.7 replaceFirst / replaceAll

使用 `pcre2_substitute_8` 实现，支持反向引用（`$0`、`$1` 等）。

```feng
open func replaceFirst(subject: string, replacement: string): string {
  return substituteInternal(subject, replacement, (u32)0);
}

open func replaceAll(subject: string, replacement: string): string {
  /* PCRE2_SUBSTITUTE_GLOBAL = 0x00000100 */
  return substituteInternal(subject, replacement, (u32)0x00000100);
}

seal func substituteInternal(subject: string, replacement: string, options: u32): string {
  let subjectBytes = subject.toBytes();
  let replBytes = replacement.toBytes();
  /* 初始输出缓冲：subject 长度 × 2，至少 256 */
  var outLen: long = subjectBytes.length() * (long)2;
  if outLen < (long)256 {
    outLen = (long)256;
  }
  var outBuf: byte[!] = byte[: outLen];
  var actualLen: long = outLen;
  let rc = pcre2_substitute(
    self.code, &subjectBytes, (long)subjectBytes.length(),
    (long)0,
    options | (u32)0x00000200,  /* PCRE2_SUBSTITUTE_EXTENDED for $n backrefs */
    (long)0,  /* match_data: NULL 让 PCRE2 内部创建 */
    (long)0,  /* mcontext: NULL */
    &replBytes, (long)replBytes.length(),
    &outBuf, &actualLen
  );
  /* PCRE2_ERROR_NOMEMORY (-48): 缓冲不够大，actualLen 已更新为所需大小 */
  if rc == -48 {
    outLen = actualLen + (long)1;
    outBuf = byte[: outLen];
    actualLen = outLen;
    let rc2 = pcre2_substitute(
      self.code, &subjectBytes, (long)subjectBytes.length(),
      (long)0,
      options | (u32)0x00000200,
      (long)0, (long)0,
      &replBytes, (long)replBytes.length(),
      &outBuf, &actualLen
    );
    if rc2 < 0 {
      throw "RegExp substitute failed";
    }
  } else if rc < 0 && rc != -1 {
    /* rc == -1 (PCRE2_ERROR_NOMATCH): 无匹配，actualLen 为原始 subject 长度 */
    throw "RegExp substitute failed";
  }
  if rc == -1 {
    return subject.clone();
  }
  return string.fromUtf8Bytes((byte[])outBuf.clone((long)0, actualLen));
}
```

说明：
- `pcre2_substitute` 参数 `PCRE2_SUBSTITUTE_OVERFLOW_LENGTH (0x00001000)` 可在缓冲不足时
  通过 `outlengthptr` 返回实际需要的大小，启用此标志避免猜测缓冲大小
- 使用 `PCRE2_SUBSTITUTE_EXTENDED` 启用 `$n` 反向引用语法
- 若 `match_data` 传 NULL，PCRE2 会内部创建临时 match_data

补充：`substituteInternal` 中 `options` 应追加 `PCRE2_SUBSTITUTE_OVERFLOW_LENGTH (0x00001000)` 以保证
首次调用缓冲不足时 `actualLen` 被正确更新：

```feng
options | (u32)0x00000200 | (u32)0x00001000
```

### 6.8 destroy

```feng
open func destroy(): void {
  if !feng_pointer_is_null(self.code) {
    pcre2_code_free(self.code);
  }
}
```

## 7 string 扩展方法

在 `std/src/text/RegExp.ff` 中通过 `fit string` 提供便捷方法，内部创建临时 `RegExp`。

```feng
open fit string {

  /**
   * Returns whether this string matches the given regex pattern.
   * @param pattern - regex pattern string
   * @return true if the entire string matches
   */
  open func matches(pattern: string): bool {
    let re = RegExp(pattern);
    let result = re.isMatch(self);
    re.destroy();
    return result;
  }

  /**
   * Replaces all matches of a regex pattern with the replacement string.
   * @param pattern - regex pattern string
   * @param replacement - replacement string (supports $0..$9 backreferences)
   * @return new string with all matches replaced
   */
  open func replacePattern(pattern: string, replacement: string): string {
    let re = RegExp(pattern);
    let result = re.replaceAll(self, replacement);
    re.destroy();
    return result;
  }

  /**
   * Splits this string by a regex pattern.
   * @param pattern - regex pattern string
   * @return array of substrings
   */
  open func splitPattern(pattern: string): string[] {
    let re = RegExp(pattern);
    let result = re.split(self);
    re.destroy();
    return result;
  }

}
```

说明：便捷方法每次调用都会编译正则表达式，适合一次性使用。
对需要复用的 pattern 应直接使用 `RegExp` 对象。

## 8 性能分析

| 方法 | 时间复杂度 | 堆分配 |
|------|-----------|--------|
| `RegExp(pattern)` | O(m) 编译 | **1**（pcre2_code） |
| `test` / `isMatch` | O(n) 匹配 | **1**（临时 match_data） |
| `find` | O(n) 匹配 | **2**（match_data + Match） |
| `findAll` | O(n·k) k=匹配数 | **O(k)**（Match 数组 + match_data） |
| `split` | O(n·k) | **O(k)**（parts 数组 + match_data） |
| `replaceFirst` | O(n) | **1~2**（substitute 输出缓冲） |
| `replaceAll` | O(n·k) | **1~2**（substitute 输出缓冲） |
| `destroy` | O(1) | **0**（释放） |

PCRE2 的 JIT 编译器（若后续启用）可将 `pcre2_match` 加速 3-10 倍，
当前方案预留了 JIT 的扩展空间。

## 9 变更文件

| 文件 | 操作 |
|------|------|
| `src/runtime/feng_runtime_contract.c` | 新增 `feng_pointer_get_scalar` |
| `src/runtime/feng_runtime_contract.inc` | 注册 `feng_pointer_get_scalar` |
| `std/src/text/RegExp.ff` | 实现 RegExp / Match / RegExpFlag + @cdecl 声明；定义 `matches` / `replacePattern` / `splitPattern` 便捷方法 |
| `std_test/src/test_regexp.ff` | 新增测试用例 |

## 10 测试用例

### 10.1 构造与基本匹配

- `RegExp("hello").test("hello world") == true`
- `RegExp("hello").test("world") == false`
- `RegExp("hello").isMatch("hello") == true`
- `RegExp("hello").isMatch("hello world") == false`（isMatch 整串匹配）
- `RegExp("\\d+").test("abc123") == true`
- `RegExp("\\d+").test("abc") == false`

### 10.2 大小写不敏感

- `RegExp("hello", RegExpFlag.CaseInsensitive).test("HELLO") == true`
- `RegExp("hello", RegExpFlag.CaseInsensitive).isMatch("Hello") == true`

### 10.3 find

- `RegExp("\\d+").find("abc123def")` → `Match { text: "123", start: 3, end: 6 }`
- `RegExp("\\d+").find("abcdef")` → `None`
- `RegExp("(\\w+)@(\\w+)").find("user@host")` → groups: `["user@host", "user", "host"]`

### 10.4 findAll

- `RegExp("\\d+").findAll("a1b22c333")` → 3 个 Match：`"1"`, `"22"`, `"333"`
- `RegExp("\\d+").findAll("abc")` → 空数组

### 10.5 split

- `RegExp("[,;]").split("a,b;c")` → `["a", "b", "c"]`
- `RegExp("\\s+").split("hello  world")` → `["hello", "world"]`
- `RegExp("\\d+").split("abc")` → `["abc"]`

### 10.6 replaceFirst / replaceAll

- `RegExp("\\d+").replaceFirst("a1b2c3", "X")` → `"aXb2c3"`
- `RegExp("\\d+").replaceAll("a1b2c3", "X")` → `"aXbXcX"`
- `RegExp("(\\w+)@(\\w+)").replaceAll("user@host", "$2/$1")` → `"host/user"`

### 10.7 Unicode

- `RegExp("\\p{L}+").find("abc123中文def")` → `Match { text: "abc" }`
- `RegExp("中.").find("中文abc")` → `Match { text: "中文" }`
- `RegExp("\\p{Han}+").findAll("hello中文world你好")` → `["中文", "你好"]`

### 10.8 编译错误

- `RegExp("[invalid")` → 抛出包含 "compile error" 的异常
- `RegExp("(?P<name")` → 抛出异常

### 10.9 string 便捷方法

- `"hello123".matches("\\w+") == true`
- `"hello123".matches("\\d+") == false`
- `"a1b2c3".replacePattern("\\d+", "X") == "aXbXcX"`
- `"a,b;c".splitPattern("[,;]")` → `["a", "b", "c"]`

### 10.10 边界情况

- `RegExp("").test("") == true`
- `RegExp("").findAll("abc")` → 4 个空匹配（每个字符边界）
- `RegExp("a*").findAll("bbb")` → 空匹配序列
- `RegExp("\\d+").split("")` → `[""]`
- 匹配后调用 `destroy()`，确认资源释放不崩溃

## 11 关联

- [feng-extlib-draft.md](./feng-extlib-draft.md) — PCRE2 选型与 vendoring 约定
- [feng-std-string-dev.md](./feng-std-string-dev.md) — string 搜索/变换方法（互补关系）
- [feng-interop-delivered.md](./feng-interop-delivered.md) — @abi / @cdecl 互操作规则
- [feng-runtime-contract-api.md](./feng-runtime-contract-api.md) — runtime contract API 清单
