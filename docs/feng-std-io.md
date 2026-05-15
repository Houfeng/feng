# Feng 标准库 I/O 规范

本文档定义随编译器分发的标准库 `std` 当前提供的最小 I/O 入口：`input`、`output` 与 `print`。本文只规定公开 API 的语义，不规定底层必须绑定哪一种特定 C 函数实现。

## 1 职责

- 在不引入额外对象模型或格式化系统的前提下，为 Feng 提供最小可用的标准输入与标准输出入口。
- 保持 API 简单可组合：`input` 负责把标准输入读入调用方提供的字节缓冲；`output` 负责把字符串写到标准输出；`print` 在 `output` 之上提供换行输出。
- 当前阶段不定义格式化打印、字符串构造型输入或文件 I/O；这些能力留待后续独立规范扩展。

## 2 公开 API

这三个函数都定义在根模块 `std` 中。使用方通过 `use std;` 后即可直接调用。

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `input` | `pu fn input(buffer: byte[!]): long` | 从标准输入读取最多 `buffer.length()` 个字节到 `buffer` 中 |
| `output` | `pu fn output(text: string): long` | 将 `text` 的 UTF-8 字节序列写到标准输出，不自动追加换行 |
| `print` | `pu fn print(text: string): long` | 先输出 `text`，再输出单个换行符 `"\n"` |

## 3 语义

- `input(buffer)` 只把数据写入调用方提供的可写字节数组，不分配新数组，也不构造 `string`。
- `input(buffer)` 一次调用最多处理 `buffer.length()` 个字节；返回值为本次实际读取的字节数。返回 `0` 表示到达 EOF；返回负值表示底层输入操作失败。
- `output(text)` 以 `text` 当前的 UTF-8 字节序列为输出内容；返回值为本次实际写出的字节数。该函数不得隐式追加换行。
- `print(text)` 的语义是输出 `text + "\n"` 到标准输出。实现可以通过一次或多次底层输出调用完成，但对使用方可观察到的结果必须等价于把单个换行符追加到 `text` 末尾后再输出。
- `print` 当前仅接受 `string`。格式化能力不在本版范围内，后续若扩展，必须通过独立规范补充而不是改变本文件对现有签名的定义。

## 4 规则

- [必须] `input` 的参数必须是可写字节数组 `byte[!]`；不得把只读数组、`string` 或其他数组类型当作 `input` 的目标缓冲。
- [必须] `output` 与 `print` 当前只接受 `string`；不得为其引入隐式数值转字符串或隐式格式化规则。
- [必须] `output` 不得自动追加换行；需要换行时必须由调用方显式调用 `print` 或手动输出 `"\n"`。
- [必须] `print` 的换行行为固定为输出单个 `"\n"` 字节。
- [必须] `input`、`output` 与 `print` 都属于标准库 `std` 的公开顶层函数，而不是方法、成员函数或对象包装器。

## 5 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-builtin-type.md](./feng-builtin-type.md): `string`、`byte` 与数组的语义。
- [feng-interop.md](./feng-interop.md): 标准库通过普通 `extern fn` 暴露原生能力时的 ABI 约束。