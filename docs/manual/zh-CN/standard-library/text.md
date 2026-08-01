# 文本

导入 `std.text` 使用字符串扩展、格式化、Unicode 视图和正则表达式：

```feng
import std.text;
```

## 字符串操作

```feng
let text = "  Feng Language  ";
let trimmed = text.trim();
let lower = trimmed.toLowerCase();
let upper = trimmed.toUpperCase();
let found = trimmed.contains("Language");
let index = trimmed.indexOf("Lang");
let parts = trimmed.split(" ");
```

常用方法还包括 `startsWith`、`endsWith`、`replace`、`repeat`、`padStart`、`padEnd`、`clone` 和 `toBytes`。

`string.length()` 返回 UTF-8 字节数，`at(index)` 与 `getByte(index)` 也按字节位置工作。不要用它们统计用户可见字符。

## 拼接与格式化

```feng
let joined = string.join(", ", ["Feng", "C", "LLVM"]);
let message = string.format("{0}: {1}", "score", "100");
```

占位符使用零基下标 `{0}`、`{1}`。非法或越界占位符按原文本保留。

满足 `Display` 的同类参数也可直接格式化：

```feng
import std.numeric;

let message = string.format<int>("{0} + {1} = {2}", 20, 22, 42);
```

## Unicode 视图

字符串内部使用 UTF-8。需要按 Unicode 码点遍历时使用 rune 视图，需要按用户可见字素簇遍历时使用 grapheme 视图。两者避免把多字节字符误当成多个文本字符。

## 正则表达式

```feng
let pattern = RegExp("[0-9]+");
defer {
  pattern.destroy();
}

let matched = pattern.test("Feng 42");
let matches = pattern.findAll("1, 2, 3");
let replaced = pattern.replaceAll("v1 v2", "version");
```

可用 `RegExpFlag` 配置大小写、多行、dot-all 等行为。`RegExp` 持有原生资源，使用完应调用 `destroy()`，并优先用 `defer` 保证清理。

字符串还提供 `matches`、`replacePattern` 与 `splitPattern` 等快捷方法。
