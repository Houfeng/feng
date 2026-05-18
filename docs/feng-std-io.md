# Feng 标准库 I/O 规范

本文档定义随编译器分发的标准库 `std` 当前提供的最小 I/O 入口：`input`、`output` 与 `print`。本文只规定公开 API 的语义，不规定底层必须绑定哪一种特定 C 函数实现。

## 1 职责

- 在不引入额外对象模型或格式化系统的前提下，为 Feng 提供最小可用的标准输入与标准输出入口。
- 保持 API 简单可组合：`input` 负责从标准输入按行读取 UTF-8 文本并返回 `string`；`output` 负责把字符串写到标准输出；`print` 在 `output` 之上提供换行输出。
- 当前阶段不定义格式化打印或文件 I/O；这些能力留待后续独立规范扩展。

## 2 公开 API

这三个函数都定义在根模块 `std` 中。使用方通过 `use std;` 后即可直接调用。

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `input` | `pu fn input(): string` | 从标准输入读取一行 UTF-8 文本，不包含行尾换行；若在读取任何字节前到达 EOF，返回 `""` |
| `output` | `pu fn output(text: string): long` | 将 `text` 的 UTF-8 字节序列写到标准输出，不自动追加换行 |
| `print` | `pu fn print(text: string): long` | 先输出 `text`，再输出单个换行符 `"\n"` |

## 3 语义

- `input()` 每次调用从标准输入读取一行 UTF-8 文本并返回新的 `string` 值；行尾分隔符不属于返回内容。
- `input()` 以换行符 `"\n"` 或 EOF 作为本次读取结束条件。若在读取到任何字节前到达 EOF，返回 `""`；若在读到部分字节后遇到 EOF，则返回该部分字节构成的最后一行。
- `output(text)` 以 `text` 当前的 UTF-8 字节序列为输出内容；返回值为本次实际写出的字节数。该函数不得隐式追加换行。
- `print(text)` 的语义是输出 `text + "\n"` 到标准输出。实现可以通过一次或多次底层输出调用完成，但对使用方可观察到的结果必须等价于把单个换行符追加到 `text` 末尾后再输出。
- `print` 当前仅接受 `string`。格式化能力不在本版范围内，后续若扩展，必须通过独立规范补充而不是改变本文件对现有签名的定义。

## 4 规则

- [必须] `input` 当前不接受参数；不得要求调用方为 `input` 提供外部字节缓冲。
- [必须] `input` 的返回类型固定为 `string`；返回内容不得包含触发本次按行读取结束的 `"\n"`。
- [必须] `input` 在读取任何字节前到达 EOF 时必须返回 `""`，而不是报错、返回 `null` 或暴露底层错误码。
- [必须] `output` 与 `print` 当前只接受 `string`；不得为其引入隐式数值转字符串或隐式格式化规则。
- [必须] `output` 不得自动追加换行；需要换行时必须由调用方显式调用 `print` 或手动输出 `"\n"`。
- [必须] `print` 的换行行为固定为输出单个 `"\n"` 字节。
- [必须] `input`、`output` 与 `print` 都属于标准库 `std` 的公开顶层函数，而不是方法、成员函数或对象包装器。

## 5 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-builtin-type.md](./feng-builtin-type.md): `string` 与 `byte` 的语义。
- [feng-std-array.md](./feng-std-array.md): 数组与 `byte[]` 的语义。
- [feng-interop.md](./feng-interop.md): 标准库通过普通 `extern fn` 暴露原生能力时的 ABI 约束。
