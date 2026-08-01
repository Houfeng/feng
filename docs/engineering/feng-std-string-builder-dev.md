# Feng 标准库 StringBuilder 开发文档

> 本文记录 `std.text.StringBuilder` 的设计与实现。
> **状态**：草案阶段，尚未实现。
> **文件**：`std/std/src/text/StringBuilder.ff`
> **模块**：`std.text`

---

## 0 背景与定位

### 职责

高效的字符串构建，支持逐字节/逐段追加。Lexer 的字符串扫描和数字扫描依赖此组件。

### 依赖关系

- Lexer（`std.compiler.FengLexer`）的 `scanString()`、`scanNumber()` 等方法使用 StringBuilder 构建字符串
- 作为 `std.text` 模块的基础设施，可供其他模块使用

---

## 1 设计

```feng
open module std.text;

/**
 * 高效的字符串构建器。
 * 底层用可变 byte[] 缓冲，支持逐字节和逐段追加，最终产出 string。
 */
open type StringBuilder {
  seal var buffer: byte[!];
  seal var count: int;

  /** 创建空构建器，初始容量 64 */
  open func StringBuilder();

  /** 创建指定初始容量的构建器 */
  open func StringBuilder(capacity: int);

  /** 追加单个字节 */
  open func append(ch: byte);

  /** 追加字符串 */
  open func append(text: string);

  /** 追加字节数组的前 length 个字节 */
  open func append(bytes: byte[], length: int);

  /** 追加整个字节数组 */
  open func append(bytes: byte[]);

  /** 产出最终字符串（拷贝 buffer 的前 count 字节） */
  open func toString(): string;

  /** 清空内容，保留缓冲区可重用 */
  open func clear();

  /** 当前已追加的字节数 */
  open func length(): int;
}
```

---

## 2 实现要点

- 底层 `byte[!]`（可写数组）+ `count` + 自动扩容
- 扩容策略：容量不足时倍增（`newCapacity = oldCapacity * 2`），初始容量 64
- `toString()` 拷贝 `buffer[0..count]` 产出新 string
- `clear()` 仅重置 `count = 0`，不释放缓冲区

---

## 3 开放问题

1. **初始容量**：64 是否合适？可根据 Lexer 实际使用场景的统计数据调整
2. **是否需要支持更多 append 变体**：如 `append(int)` 数字转字符串、`append(char)` Unicode 字符等
