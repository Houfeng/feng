# Feng 标准库 Path 规范

本文档定义随编译器分发的标准库 `std` 当前提供的路径处理 API 模型。`std.path` 是纯字符串操作模块，不访问文件系统。

## 1 职责

- 提供纯字符串的路径操作函数，不访问文件系统。
- 覆盖路径拼接、分解、规范化、解析等常见操作。

## 2 公开 API

模块：`std.path`。使用方通过 `import std.path;` 后使用以下符号。

所有函数均为纯字符串操作，不访问文件系统。

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `join` | `open func join(paths: string...): string` | 拼接路径段，使用 `/` 连接 |
| `dirname` | `open func dirname(path: string): string` | 返回路径的目录部分 |
| `basename` | `open func basename(path: string): string` | 返回路径的最后一部分（文件名或目录名） |
| `extname` | `open func extname(path: string): string` | 返回文件扩展名（含 `.`），无扩展名返回 `""` |
| `isAbsolute` | `open func isAbsolute(path: string): bool` | 判断路径是否为绝对路径 |
| `isRelative` | `open func isRelative(path: string): bool` | 判断路径是否为相对路径 |
| `resolve` | `open func resolve(base: string, target: string): string` | 将 `target` 基于 `base` 解析并规范化 |
| `normalize` | `open func normalize(path: string): string` | 规范化路径，解析 `.`、`..` 与多余的 `/` |
| `separator` | `open let separator: string` | 路径分隔符，值为 `"/"` |

## 3 语义

### 3.1 `join`

- `join` 将零个或多个路径段用 `separator` 连接为一个路径字符串。
- `join` 对结果执行 `normalize`，消除多余的 `separator` 与 `.`、`..` 段。
- `join` 零实参时返回 `""`。
- `join` 中若某一段为绝对路径，则忽略该段之前的所有段。

### 3.2 `dirname`

- `dirname` 返回路径中最后一个 `separator` 之前的部分。
- `dirname` 的路径不含 `separator` 时返回 `"."`。
- `dirname` 的路径以 `separator` 结尾时，先忽略尾部分隔符再取目录部分。
- `dirname("/")` 返回 `"/"`。

### 3.3 `basename`

- `basename` 返回路径中最后一个 `separator` 之后的部分。
- `basename` 的路径以 `separator` 结尾时，先忽略尾部分隔符再取名称部分。
- `basename("/")` 返回 `""`。

### 3.4 `extname`

- `extname` 返回路径 basename 中从最后一个 `.` 开始到末尾的子串，包含 `.` 本身。
- basename 不含 `.`、以 `.` 开头且不含其他 `.`、或 `.` 位于首位时，返回 `""`。
- 示例：`extname("index.html")` 返回 `".html"`；`extname(".gitignore")` 返回 `""`；`extname("archive.tar.gz")` 返回 `".gz"`。

### 3.5 `isAbsolute` 与 `isRelative`

- `isAbsolute` 在路径以 `/` 开头时返回 `true`，否则返回 `false`。
- `isRelative` 在路径不以 `/` 开头时返回 `true`，否则返回 `false`。
- `isRelative(path)` 的结果始终等价于 `!isAbsolute(path)`。

### 3.6 `resolve`

- `resolve(base, target)` 将 `target` 基于 `base` 解析并规范化。
- 当 `target` 为绝对路径时，忽略 `base`，返回 `normalize(target)`。
- 当 `target` 为相对路径时，返回 `normalize(join(base, target))`。
- 结果是否为绝对路径由 `base`（或绝对 `target`）决定，不做强制要求。
- `resolve` 是纯字符串操作，不访问文件系统，不读取当前工作目录；`base` 由调用方提供。

### 3.7 `normalize`

- `normalize` 解析路径中的 `.`（当前目录）与 `..`（上级目录），消除多余的连续 `/`。
- `normalize` 对绝对路径不消除根目录以上的 `..`；对相对路径允许保留前导 `..`。
- `normalize` 空字符串返回 `"."`。
- `normalize` 的结果保留原路径的尾部 `/`（若存在）。

## 4 规则

- [必须] `std.path` 的所有函数必须是纯字符串操作，不得访问文件系统。
- [必须] `join` 的参数为变长 `string...`，返回类型为 `string`。
- [必须] `join` 必须对结果执行规范化。
- [必须] `dirname` 与 `basename` 的返回类型为 `string`。
- [必须] `extname` 返回类型固定为 `string`；无扩展名时返回 `""`。
- [必须] `isAbsolute` 与 `isRelative` 返回 `bool`；两者结果必须互斥。
- [必须] `normalize` 返回 `string`；空字符串输入返回 `"."`。
- [必须] `resolve` 返回 `string`；结果是否为绝对路径由 `base`（或绝对 `target`）决定。
- [必须] `resolve` 当 `target` 为绝对路径时忽略 `base`；当 `target` 为相对路径时基于 `base` 解析。
- [必须] `separator` 的值固定为 `"/"`。
- [禁止] `std.path` 的任何函数访问文件系统或依赖当前工作目录。

## 5 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-builtin-type.md](./feng-builtin-type.md): `string` 的语义。
- [feng-std-fs.md](./feng-std-fs.md): 文件系统规范，`std.path` 常与 `std.fs` 配合使用。
