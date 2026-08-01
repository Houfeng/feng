# 标准库指南

Feng 标准库随官方发行包提供，按模块组织常用数据结构、文本、I/O、文件系统、数值、时间、平台与进程能力。

## 使用标准库

标准库能力通过模块导入：

```feng
import std.collections;
import std.io;
import std.text;
```

部分模块通过 `fit` 为内建类型补充方法。只有导入相应模块后，这些扩展才进入当前文件作用域。例如数组的 `length()` 来自 `std.collections`，字符串的文本方法来自 `std.text`。

## 章节

- [集合](./collections.md)：数组扩展、`Span<T>`、`List<T>`、`Map<K, V>` 与 `Set<K>`。
- [文本](./text.md)：字符串、格式化、Unicode 视图与正则表达式。
- [文件系统与 I/O](./filesystem-and-io.md)：终端输入输出、路径、文件和目录。
- [时间与数学](./time-and-math.md)：`DateTime` 与 `Math`。
- [平台与进程](./platform-and-process.md)：系统信息、内存、CPU 和子进程。

本目录是任务导向指南，不替代标准库规范或源码中的 API 文档。需要精确的边界、错误和复杂度保证时，应继续阅读对应的 `docs/specifications/feng-std-*.md`。
