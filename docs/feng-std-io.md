# Feng 标准库 I/O 规范

本文档定义随编译器分发的标准库 `std` 当前提供的标准输入输出模型。本文只规定公开 API 的语义：底层实现必须基于 libc，但不要求绑定到某一个固定的 libc 符号组合。

## 1 职责

- 为 Feng 提供一个可复用的标准输入输出对象 `Stdio`，把底层字节 I/O 与上层文本封装收敛到同一处公开面。
- 保持低层 API bytes-first：`read`、`write` 与 `writeError` 都直接处理 `byte` 缓冲或 `byte[]` 数据，不隐式引入文本编码规则。
- 提供最小必要的按行便利能力：`Stdio.readLine` 与 `Stdio.writeLine` 都直接面向 UTF-8 `string`，`Stdio.write` 与 `Stdio.writeError` 提供 `string` 便利重载，顶层 `readLine`、`writeLine`、`format`、`print` 与 `println` 负责默认实例上的文本包装。
- 为现有使用方保留顶层函数 `readLine`、`writeLine`、`format`、`print` 与 `println`；这些函数只是默认实例 `stdio` 的语义包装或共享格式化入口，不另建独立实现分支。

## 2 公开 API

这些符号都定义在根模块 `std` 中。使用方通过 `import std;` 后即可直接调用。

### 2.1 `Stdio` 与默认实例

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Stdio` | `open type Stdio` | 标准输入输出对象，封装标准输入、标准输出与标准错误的默认传输行为 |
| `Stdio.read` | `open func read(buffer: byte[!]): long` | 从标准输入读取至多 `buffer.length()` 个字节写入 `buffer`，返回本次实际读入字节数 |
| `Stdio.write` | `open func write(bytes: byte[]): long` | 将 `bytes` 原样写到标准输出，不自动追加换行 |
| `Stdio.write` | `open func write(text: string): long` | 将 `text` 当前的 UTF-8 字节序列写到标准输出，不自动追加换行 |
| `Stdio.writeError` | `open func writeError(bytes: byte[]): long` | 将 `bytes` 原样写到标准错误，不自动追加换行 |
| `Stdio.writeError` | `open func writeError(text: string): long` | 将 `text` 当前的 UTF-8 字节序列写到标准错误，不自动追加换行 |
| `Stdio.writeLine` | `open func writeLine(text: string): long` | 将 `text` 当前的 UTF-8 字节序列写到标准输出，并追加单个 `"\n"` 字节 |
| `Stdio.writeErrorLine` | `open func writeErrorLine(bytes: byte[]): long` | 将 `bytes` 原样写到标准错误，并追加单个 `"\n"` 字节 |
| `Stdio.writeErrorLine` | `open func writeErrorLine(text: string): long` | 将 `text` 当前的 UTF-8 字节序列写到标准错误，并追加单个 `"\n"` 字节 |
| `Stdio.readLine` | `open func readLine(): string` | 从标准输入读取一行 UTF-8 文本，不包含行尾换行 |
| `stdio` | `open let stdio = Stdio()` | 绑定到标准输入、标准输出与标准错误的默认实例 |

### 2.2 兼容顶层函数

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `readLine` | `open func readLine(): string` | `stdio.readLine()` 的顶层包装 |
| `writeLine` | `open func writeLine(text: string): long` | `stdio.writeLine(text)` 的顶层包装 |
| `format` | `open func format(fmt: string, args: string...): string` | 返回将 `fmt` 按 `{argsIndex}` 规则插值后的字符串，不自动追加换行 |
| `print` | `open func print(format: string, args: string...): long` | 将 `format` 按 `{argsIndex}` 规则插值后输出到默认标准输出，不自动追加换行 |
| `println` | `open func println(format: string, args: string...): long` | 将 `format` 按 `{argsIndex}` 规则插值后输出到默认标准输出，并追加单个 `"\n"` |

## 3 语义

### 3.1 低层字节 I/O

- `stdio` 默认绑定标准文件描述符 `0/1/2`，分别代表标准输入、标准输出与标准错误。
- `Stdio.read(buffer)` 以 `buffer` 作为调用方提供的可写字节缓冲；返回值为本次实际读入字节数。
- `Stdio.read(buffer)` 在到达 EOF 时返回 `0`；若底层 libc 读操作失败，则返回底层的负值错误结果。
- `Stdio.read(buffer)` 在 `buffer.length() == 0` 时必须直接返回 `0`，不得访问底层输入流。
- `Stdio.write(bytes)` 与 `Stdio.writeError(bytes)` 以 `bytes` 当前内容为输出字节序列；返回值为本次实际写出的字节数，失败时返回底层的负值错误结果。
- `Stdio.write(text)` 与 `Stdio.writeError(text)` 的语义固定等价于把 `text` 当前的 UTF-8 字节序列传给对应的 bytes 重载。
- `Stdio.write(bytes)`、`Stdio.write(text)`、`Stdio.writeError(bytes)` 与 `Stdio.writeError(text)` 都不得自动追加换行、NUL 终止符或其他额外字节。
- `Stdio.writeLine(text)` 的语义固定为把 `text` 当前的 UTF-8 字节序列写到标准输出，并在末尾追加单个 `"\n"` 字节；其返回值包含该换行字节。
- `Stdio.writeErrorLine(bytes)` 与 `Stdio.writeErrorLine(text)` 的语义固定为把对应内容写到标准错误，并在末尾追加单个 `"\n"` 字节；其返回值包含该换行字节。
- 当前实现直接调用 libc 的 `write(fd, ...)` 写入文件描述符，不经过 `FILE*` 缓冲；标准库 `stdio` 当前不提供 `flush()`，因为没有独立的用户态缓冲需要刷新。

### 3.2 `readLine`

- `Stdio.readLine()` 每次调用从标准输入读取一行 UTF-8 文本并返回新的 `string` 值；行尾分隔符不属于返回内容。
- `Stdio.readLine()` 以换行符 `"\n"` 或 EOF 作为本次读取结束条件。若在读取到任何字节前到达 EOF，返回 `""`；若在读到部分字节后遇到 EOF，则返回该部分字节构成的最后一行。
- `Stdio.readLine()` 必须忽略行内的回车字节 `"\r"`，以兼容 `CRLF` 输入；该规则只影响按行读取包装，不改变低层 `read` 的原始字节语义。
- `Stdio.readLine()` 若底层 `read` 返回负值错误结果，不得静默吞掉错误；实现必须抛出明确的标准库异常文本。

### 3.3 `format`、`print` 与 `println`

- 顶层 `format(fmt, args...)` 先解析 `fmt` 的 UTF-8 字节序列，再返回插值后的新 `string`，不自动追加换行。
- 顶层 `print(format, args...)` 先解析 `format` 的 UTF-8 字节序列，再把解析结果写到默认标准输出，不自动追加换行。
- 顶层 `println(format, args...)` 与 `print(format, args...)` 拥有相同的入参与返回语义，唯一区别是 `println` 会在末尾自动追加单个 `"\n"` 字节。
- 占位符语法固定为 `{argsIndex}`：其中 `argsIndex` 是十进制、零基、仅由 ASCII 数字组成的参数下标，例如 `{0}`、`{1}`、`{12}`。
- 当占位符合法且 `argsIndex` 在 `args` 范围内时，`print` 必须以对应 `args[argsIndex]` 的 UTF-8 字节序列替换该占位符。
- 当 `{...}` 片段不是合法占位符，或合法但下标越界时，`print` 必须按字面文本原样输出该片段，不得报错、丢弃或做其他隐式转换。
- 顶层 `format(fmt, args...)` 的返回值是插值完成后的 `string`，其中不包含任何隐式追加的换行字节。
- 顶层 `print(format, args...)` 的返回值是实际写到默认标准输出的总字节数；若调用方在 `format` 中显式包含 `"\n"`，该换行字节计入返回值。
- 顶层 `println(format, args...)` 的返回值是实际写到默认标准输出的总字节数，包含其自动追加的单个换行字节。
- 需要换行时，调用方可以显式在 `format` 中加入 `"\n"`，或者直接调用 `writeLine(text)`。
- `format`、`print` 与 `println` 都只接受 `string` 与 `string...`；不得为其引入隐式数值转字符串、`spec` 值格式化或其他未定义的格式系统。

### 3.4 顶层兼容包装

- 顶层 `readLine()` 的语义固定等价于 `stdio.readLine()`。
- 顶层 `writeLine(text)` 的语义固定等价于 `stdio.writeLine(text)`。
- 顶层 `format(fmt, args...)` 与 `print/println` 必须共享同一套 `{argsIndex}` 插值语义。
- 顶层 `print(format, args...)` 的语义固定等价于把 variadic `args` 组装成 `string[]`、完成 `{argsIndex}` 插值后委托给 `stdio.write(...)`。
- 顶层 `println(format, args...)` 的语义固定等价于把 variadic `args` 组装成 `string[]`、完成 `{argsIndex}` 插值后委托给 `stdio.writeLine(...)`。
- 兼容顶层函数不得拥有与 `stdio` 方法不一致的独立分支逻辑。

## 4 规则

- [必须] `Stdio` 是当前标准库 I/O 的权威公开抽象；默认实例名固定为 `stdio`。
- [必须] `Stdio.read` 的参数类型固定为 `byte[!]`，返回类型固定为 `long`。
- [必须] `Stdio.write` 与 `Stdio.writeError` 必须同时提供 `byte[]` 与 `string` 两个公开重载，返回类型固定为 `long`。
- [必须] `Stdio.writeLine` 的参数类型固定为 `string`，返回类型固定为 `long`。
- [必须] `Stdio.writeErrorLine` 必须同时提供 `byte[]` 与 `string` 两个公开重载，返回类型固定为 `long`。
- [必须] `Stdio.read` 的语义直接面向 bytes，不得隐式执行 UTF-8 编码、解码或字符串拼接。
- [必须] `Stdio.writeLine` 必须把 `string` 当前的 UTF-8 字节序列写到标准输出，并且只允许在末尾追加单个 `"\n"` 字节。
- [必须] `Stdio.writeErrorLine` 必须只在末尾追加单个 `"\n"` 字节。
- [必须] `Stdio.readLine` 的返回类型固定为 `string`；返回内容不得包含触发本次按行读取结束的 `"\n"`。
- [必须] `Stdio` 不得公开 `print` 方法；带格式插值的文本输出便利能力只由顶层 `print` 提供。
- [必须] 顶层 `readLine` 的签名固定为 `readLine(): string`。
- [必须] 顶层 `writeLine` 的签名固定为 `writeLine(text: string): long`。
- [必须] 顶层 `format` 的签名固定为 `format(fmt: string, args: string...): string`。
- [必须] 顶层 `print` 的签名固定为 `print(format: string, args: string...)`。
- [必须] 顶层 `println` 的签名固定为 `println(format: string, args: string...): long`，并且其入参与返回类型必须与 `print` 完全一致。
- [必须] 顶层 `print` 默认不得追加换行；需要换行时只能由调用方显式提供 `"\n"` 或调用 `writeLine`。
- [必须] 顶层 `println` 必须自动追加且只能追加一个 `"\n"` 字节。
- [必须] `{argsIndex}` 占位符中的下标按零基解释。
- [必须] 非法或越界占位符按字面文本输出，不得抛错或静默删除。
- [必须] 当前标准库 `stdio` 不得暴露 `flush()`；只有在引入独立可见缓冲后才允许新增。
- [必须] 顶层 `readLine`、`writeLine`、`format`、`print` 与 `println` 都必须与默认实例或共享格式化语义保持一致。
- [禁止] 为 `write` 或 `writeError` 自动追加换行。
- [禁止] 为 `print` 或 `println` 引入除 `{argsIndex}` 之外的隐式格式规则。
- [禁止] 在兼容层重新实现一套独立 I/O 行为，导致与 `stdio` 语义分叉。

## 5 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-builtin-type.md](./feng-builtin-type.md): `string`、`byte`、`byte[]` 与 `byte[!]` 的语义。
- [feng-function-variadic.md](./feng-function-variadic.md): 顶层 `print(format, args: string...)` 使用的变长参数规则。
- [feng-interop.md](./feng-interop.md): 标准库通过普通 `extern func` 暴露原生能力时的 ABI 约束。
