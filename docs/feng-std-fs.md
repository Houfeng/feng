# Feng 标准库 Filesystem 规范

本文档定义随编译器分发的标准库 `std` 当前提供的文件系统 API 模型。本文只规定公开 API 的语义：底层实现基于 C ABI 路径（`libc` 或 `libuv`），但不要求绑定到某一个固定的 C 符号组合。

## 1 职责

- 为 Feng 提供可复用的文件读写对象 `File`，把底层字节 I/O 与上层文本便利收敛到同一公开面。
- 提供目录遍历对象 `Dir`，支持按指定批次读取目录条目，并支持 `for/in` 迭代。
- 提供条目信息类型 `EntryInfo`（目录遍历轻量结果）与 `StatInfo`（完整元信息结果）。
- 提供条目种类枚举 `EntryKind`，统一标识文件系统中的实体类型。
- 提供路径级的顶层便利函数，覆盖常见一次性文件操作（读写、查询、创建、删除、重命名）。
- 保持低层 API bytes-first：`File.read` 与 `File.write` 直接处理 `byte` 缓冲或 `byte[]` 数据，不隐式引入文本编码规则。

---

## 2 `std.fs` 公开 API

模块：`std.fs`。使用方通过 `import std.fs;` 后使用以下符号。

### 2.1 `FileMode` 枚举

`FileMode` 枚举覆盖 POSIX `open()` 全部有意义的标志组合，每个枚举值对应一组确定的 POSIX flags。

| 符号 | POSIX flags | 说明 |
| --- | --- | --- |
| `FileMode.Read` | `O_RDONLY` | 只读打开，文件必须已存在 |
| `FileMode.Write` | `O_WRONLY \| O_CREAT` | 只写打开，不存在则创建，存在则保留原内容，写入从偏移 0 开始覆盖 |
| `FileMode.WriteTrunc` | `O_WRONLY \| O_CREAT \| O_TRUNC` | 只写打开，不存在则创建，存在则清空原内容，从空文件开始写入 |
| `FileMode.WriteNew` | `O_WRONLY \| O_CREAT \| O_EXCL` | 只写打开，排他创建，文件已存在则失败，从空文件开始写入 |
| `FileMode.WriteAppend` | `O_WRONLY \| O_CREAT \| O_APPEND` | 只写打开，不存在则创建，保留原内容，写入始终追加到末尾 |
| `FileMode.ReadWrite` | `O_RDWR` | 读写打开，文件必须已存在，写入从偏移 0 开始覆盖 |
| `FileMode.ReadWriteCreate` | `O_RDWR \| O_CREAT` | 读写打开，不存在则创建，存在则保留原内容，写入从偏移 0 开始覆盖 |
| `FileMode.ReadWriteTrunc` | `O_RDWR \| O_CREAT \| O_TRUNC` | 读写打开，不存在则创建，存在则清空原内容，从空文件开始读写 |
| `FileMode.ReadWriteNew` | `O_RDWR \| O_CREAT \| O_EXCL` | 读写打开，排他创建，文件已存在则失败，从空文件开始读写 |
| `FileMode.ReadWriteAppend` | `O_RDWR \| O_CREAT \| O_APPEND` | 读写打开，不存在则创建，保留原内容，写入始终追加到末尾 |
| `FileMode.AppendExisting` | `O_RDWR \| O_APPEND` | 读写打开，文件必须已存在，保留原内容，写入始终追加到末尾 |

### 2.2 `EntryKind` 枚举

`EntryKind` 标识文件系统中的实体类型，替代 `isFile` / `isDir` / `isSymlink` 等布尔标志，杜绝不自治状态。

| 符号 | POSIX `d_type` | POSIX `st_mode` | 说明 |
| --- | --- | --- | --- |
| `EntryKind.File` | `DT_REG` | `S_IFREG` | 普通文件 |
| `EntryKind.Dir` | `DT_DIR` | `S_IFDIR` | 目录 |
| `EntryKind.Symlink` | `DT_LNK` | `S_IFLNK` | 符号链接 |
| `EntryKind.Fifo` | `DT_FIFO` | `S_IFIFO` | 命名管道 |
| `EntryKind.Socket` | `DT_SOCK` | `S_IFSOCK` | 套接字 |
| `EntryKind.CharDevice` | `DT_CHR` | `S_IFCHR` | 字符设备 |
| `EntryKind.BlockDevice` | `DT_BLK` | `S_IFBLK` | 块设备 |
| `EntryKind.Other` | `DT_UNKNOWN` | — | 未知或不可识别的类型 |

### 2.3 `File` 类型

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `File` | `open type File` | 文件读写对象，封装文件描述符与读写操作 |
| `File.open` | `open static func open(path: string, mode: FileMode): File` | 打开或创建文件，返回 `File` 实例 |
| `File.read` | `open func read(buffer: byte[!]): long` | 从文件读取至多 `buffer.length()` 个字节，返回实际读取字节数，EOF 返回 `0` |
| `File.write` | `open func write(bytes: byte[]): long` | 将 `bytes` 写到文件，返回实际写入字节数 |
| `File.writeText` | `open func writeText(text: string): long` | 将 `text` 的 UTF-8 字节写到文件 |
| `File.readText` | `open func readText(): string` | 从当前偏移读取全部剩余字节并返回 UTF-8 `string` |
| `File.readLines` | `open func readLines(): string[]` | 从当前偏移读取全部剩余内容并按行分割，行尾分隔符不属于返回内容 |
| `File.close` | `open func close(): void` | 关闭文件并释放底层文件描述符 |
| `File.isClosed` | `open func isClosed(): bool` | 返回文件是否已关闭 |
| `File.stat` | `open func stat(): StatInfo` | 获取当前文件的元信息 |

### 2.4 `StatInfo` 类型

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `StatInfo` | `open type StatInfo` | 文件/目录的完整元信息，由 `stat` 系列操作返回 |
| `StatInfo.kind` | `let kind: EntryKind` | 条目种类 |
| `StatInfo.size` | `let size: i64` | 大小，单位字节（对目录无直观意义） |
| `StatInfo.mode` | `let mode: u64` | 权限位（POSIX mode） |
| `StatInfo.modifiedAt` | `let modifiedAt: i64` | 最后修改时间，Unix 时间戳（秒） |
| `StatInfo.accessedAt` | `let accessedAt: i64` | 最后访问时间，Unix 时间戳（秒） |
| `StatInfo.changedAt` | `let changedAt: i64` | 最后元数据变更时间，Unix 时间戳（秒），对应 POSIX `st_ctime` |

### 2.5 `EntryInfo` 类型

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `EntryInfo` | `open type EntryInfo` | 目录遍历的轻量条目信息，由 `Dir.read` 或 `Dir` 迭代返回 |
| `EntryInfo.name` | `let name: string` | 条目名称（不含路径分隔符） |
| `EntryInfo.kind` | `let kind: EntryKind` | 条目种类 |
| `EntryInfo.stat` | `open func stat(parentPath: string): StatInfo` | 获取该条目的完整元信息，内部调用 POSIX `stat`（跟踪符号链接） |

### 2.6 `Dir` 类型

| 符号 | 签名 | 说明 |
| --- | --- | --- |
| `Dir` | `open type Dir` | 目录遍历对象，封装目录句柄与读取操作 |
| `Dir.read` | `open func read(batchSize: long): EntryInfo[]` | 读取下一批至多 `batchSize` 个目录条目，遍历结束后返回空数组 |
| `Dir.next` | `open func next(): IteratorResult<EntryInfo>` | `@iterator` 方法，逐条返回目录条目，支持 `for/in` 迭代 |
| `Dir.close` | `open func close(): void` | 关闭目录并释放底层目录句柄 |
| `Dir.isClosed` | `open func isClosed(): bool` | 返回目录是否已关闭 |
| `openDir` | `open func openDir(path: string): Dir` | 打开目录，返回 `Dir` 实例 |

### 2.7 顶层便利函数

这些函数定义在模块 `std.fs` 中。使用方通过 `import std.fs;` 后可直接调用。

#### 文件读写

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `readText` | `open func readText(path: string): string` | 读取文件全部内容作为 UTF-8 字符串返回 |
| `writeText` | `open func writeText(path: string, text: string): void` | 将文本以 UTF-8 编码写入文件，不存在则创建，存在则清空 |
| `readBytes` | `open func readBytes(path: string): byte[]` | 读取文件全部内容作为字节数组返回 |
| `writeBytes` | `open func writeBytes(path: string, bytes: byte[]): void` | 将字节数组写入文件，不存在则创建，存在则清空 |

#### 路径查询

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `exists` | `open func exists(path: string): bool` | 判断路径是否存在（文件或目录） |
| `isFile` | `open func isFile(path: string): bool` | 判断路径是否为普通文件 |
| `isDir` | `open func isDir(path: string): bool` | 判断路径是否为目录 |
| `stat` | `open func stat(path: string): StatInfo` | 获取路径的完整元信息（跟踪符号链接） |

#### 目录操作

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `mkdir` | `open func mkdir(path: string): void` | 创建单个目录，父目录必须存在 |
| `rmdir` | `open func rmdir(path: string): void` | 删除空目录 |
| `readDir` | `open func readDir(path: string): EntryInfo[]` | 一次性读取目录全部条目 |

#### 文件操作

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `remove` | `open func remove(path: string): void` | 删除文件或符号链接 |
| `rename` | `open func rename(from: string, to: string): void` | 重命名或移动文件/目录 |

---

## 3 `std.fs` 语义

### 3.1 文件打开模式

`FileMode` 的 11 个枚举值覆盖 POSIX `open()` 的全部有意义组合，分为三个维度：

**访问模式**（互斥）：

- 只读：`Read`
- 只写：`Write`、`WriteTrunc`、`WriteNew`、`WriteAppend`
- 读写：`ReadWrite`、`ReadWriteCreate`、`ReadWriteTrunc`、`ReadWriteNew`、`ReadWriteAppend`、`AppendExisting`

**创建行为**：

- 不创建（文件必须存在）：`Read`、`ReadWrite`、`AppendExisting`
- 创建（不存在则创建）：其余所有模式
- 排他创建（已存在则失败）：`WriteNew`、`ReadWriteNew`

**写入位置**：

- 当前位置：除 `Append` 系列外的所有模式
- 始终追加到末尾：`WriteAppend`、`ReadWriteAppend`、`AppendExisting`

**打开时对已有内容的处理**：

- 保留原内容，写入从偏移 0 开始覆盖：`Write`、`ReadWrite`、`ReadWriteCreate`
- 清空原内容，从空文件开始写入：`WriteTrunc`、`ReadWriteTrunc`
- 保留原内容，写入始终追加到末尾：`WriteAppend`、`ReadWriteAppend`、`AppendExisting`
- 不涉及：`Read`（只读）、`WriteNew`、`ReadWriteNew`（排他创建，文件不存在）

- `File.open` 的默认文件创建权限为 `0644`（用户读写、组和其他用户只读）。
- `File.open` 在底层调用失败时抛出异常。

### 3.2 文件读写

- `File.read(buffer)` 以 `buffer` 作为调用方提供的可写字节缓冲；返回值为本次实际读入字节数。
- `File.read(buffer)` 在到达 EOF 时返回 `0`；若底层读操作失败，则返回底层的负值错误结果。
- `File.read(buffer)` 在 `buffer.length() == 0` 时必须直接返回 `0`，不得访问底层文件。
- `File.write(bytes)` 以 `bytes` 当前内容为输出字节序列；返回值为本次实际写出的字节数，失败时返回底层的负值错误结果。
- `File.write(bytes)` 在 `bytes.length() == 0` 时必须直接返回 `0`，不得访问底层文件。
- `File.read` 与 `File.write` 都不得自动追加换行、NUL 终止符或其他额外字节。
- `File.read` 与 `File.write` 在文件已关闭时必须抛出异常。

### 3.3 文件文本便利

- `File.writeText(text)` 的语义固定等价于把 `text` 当前的 UTF-8 字节序列传给 `File.write`。
- `File.readText()` 从当前文件偏移读取全部剩余字节并解码为 UTF-8 `string`。
- `File.readLines()` 以换行符 `"\n"` 作为行分隔符，忽略行内回车字节 `"\r"` 以兼容 `CRLF`；返回内容不包含行尾分隔符。
- `File.readText` 与 `File.readLines` 在文件已关闭时必须抛出异常。

### 3.4 文件生命周期

- `File.close` 关闭文件并释放底层文件描述符，同时将 `isClosed` 设为 `true`。
- `File.close` 在文件已关闭时不抛出错误，行为幂等。
- `File` 不提供隐式自动关闭机制；调用方必须在不再需要时显式调用 `close`。
- `File` 不得暴露底层文件描述符。

### 3.5 `EntryKind`

- `EntryKind` 是一个单一值枚举，每个实例恰好属于一种 kind，不存在同时属于多种 kind 的不自治状态。
- `EntryKind.Symlink` 表示符号链接本身；符号链接的指向目标类型需通过 `stat`（跟踪链接）获取。
- `EntryKind.Other` 覆盖文件系统返回 `DT_UNKNOWN` 或其他不可识别类型的情况。

### 3.6 `StatInfo`

- `StatInfo.kind` 标识条目种类。当 `StatInfo` 由 `stat`（跟踪符号链接）获取时，`kind` 为目标的真实类型，不会是 `Symlink`。
- `StatInfo.size` 对普通文件表示文件内容字节数；对目录表示目录元数据大小（平台相关，无直观意义）。
- `StatInfo.mode` 表示 POSIX 权限位。
- `StatInfo.modifiedAt`、`accessedAt` 与 `changedAt` 均为 Unix 时间戳，精度为秒。
- `StatInfo.changedAt` 对应 POSIX `st_ctime`（最后元数据变更时间），不是创建时间。
- `StatInfo` 的所有字段均为只读。
- `StatInfo` 的字段结构对文件和目录完全一致，不因条目种类不同而缺失字段。

### 3.7 `EntryInfo`

- `EntryInfo.name` 仅包含条目名称，不含路径分隔符与父路径前缀。
- `EntryInfo` 列表中不得包含 `.` 与 `..` 条目。
- `EntryInfo.kind` 取自 `readdir` / `scandir` 的条目类型信息；当底层返回 `DT_UNKNOWN` 时，`kind` 为 `Other`。
- `EntryInfo.stat(parentPath)` 拼接 `parentPath` 与 `name` 后调用 POSIX `stat`（跟踪符号链接），返回目标的 `StatInfo`。底层调用失败时抛出异常。

### 3.8 目录遍历

#### 批次读取

- `Dir.read(batchSize)` 每次调用返回下一批至多 `batchSize` 个 `EntryInfo`。
- `Dir.read(batchSize)` 的 `batchSize` 必须大于 `0`，否则抛出异常。
- `Dir.read(batchSize)` 在遍历结束后返回空数组（`EntryInfo[]` 长度为 `0`）。
- `Dir.read(batchSize)` 在目录已关闭时必须抛出异常。

#### `for/in` 迭代

- `Dir` 通过 `@iterator` 标注的 `next()` 方法支持 `for/in` 迭代。
- `Dir.next()` 每次调用返回一个 `IteratorResult<EntryInfo>`（即 `(bool, EntryInfo)` 元组）；`true` 表示产出有效条目，`false` 表示遍历结束。
- `Dir.next()` 在目录已关闭时必须抛出异常。

#### 批次读取与迭代的互斥

- `Dir.read` 与 `Dir.next` 共享同一个底层遍历状态；调用方不得在同一次目录遍历中混用两种方式。

#### 生命周期

- `Dir.close` 关闭目录并释放底层目录句柄，同时将 `isClosed` 设为 `true`。
- `Dir.close` 在目录已关闭时不抛出错误，行为幂等。
- `Dir` 不提供隐式自动关闭机制；调用方必须在不再需要时显式调用 `close`。
- `openDir` 在路径无效、权限不足或路径不是目录时抛出异常。

### 3.9 符号链接处理

- `readdir` / `scandir` 返回的 `EntryInfo.kind` 可以是 `Symlink`，不跟踪链接。
- `EntryInfo.stat(parentPath)` 调用 POSIX `stat`，跟踪符号链接，返回目标的 `StatInfo`（`kind` 为目标的真实类型）。
- 顶层 `stat(path)` 调用 POSIX `stat`，跟踪符号链接。
- 顶层 `exists(path)`、`isFile(path)` 与 `isDir(path)` 均跟踪符号链接。
- 当前版本不提供 `lstat`（不跟踪链接的 stat）；若未来需要，可新增 `lstat` 顶层函数。

### 3.10 顶层便利函数

- `readText(path)` 以 `FileMode.Read` 打开文件，读取全部内容后关闭。
- `writeText(path, text)` 以 `FileMode.WriteTrunc` 打开文件，写入 `text` 的 UTF-8 字节后关闭。
- `readBytes(path)` 以 `FileMode.Read` 打开文件，读取全部字节后关闭。
- `writeBytes(path, bytes)` 以 `FileMode.WriteTrunc` 打开文件，写入全部字节后关闭。
- `readText`、`writeText`、`readBytes` 与 `writeBytes` 必须自动管理文件的打开与关闭，即使中途抛出异常也必须关闭已打开的文件。
- `exists(path)` 在路径可访问时返回 `true`，不存在或不可访问时返回 `false`。`exists` 不得抛出异常。
- `isFile(path)` 在路径指向普通文件时返回 `true`，否则返回 `false`。`isFile` 不得抛出异常。
- `isDir(path)` 在路径指向目录时返回 `true`，否则返回 `false`。`isDir` 不得抛出异常。
- `stat(path)` 获取路径的完整元信息并返回 `StatInfo`（跟踪符号链接），路径无效或不可访问时抛出异常。
- `mkdir(path)` 创建单个目录，父目录不存在或路径已存在时抛出异常。
- `rmdir(path)` 删除空目录，目录不存在或非空时抛出异常。
- `readDir(path)` 一次性读取目录全部条目并返回 `EntryInfo[]`，底层复用 `Dir` 类型的打开与读取逻辑。
- `remove(path)` 删除文件或符号链接，不得删除目录。路径无效时抛出异常。
- `rename(from, to)` 重命名或移动文件/目录。源路径无效时抛出异常。

---

## 4 `std.fs` 错误

底层操作失败时，使用 `throw` 抛出字符串异常，遵循 `fs/<operation>-failed` 命名模式：

| 异常文本 | 抛出条件 |
| --- | --- |
| `fs/open-failed` | 打开文件失败（路径无效、权限不足等） |
| `fs/read-failed` | 底层读操作失败 |
| `fs/write-failed` | 底层写操作失败 |
| `fs/close-failed` | 关闭文件描述符失败 |
| `fs/stat-failed` | 获取元信息失败 |
| `fs/mkdir-failed` | 创建目录失败 |
| `fs/rmdir-failed` | 删除目录失败 |
| `fs/opendir-failed` | 打开目录失败 |
| `fs/readdir-failed` | 读取目录条目失败 |
| `fs/remove-failed` | 删除文件失败 |
| `fs/rename-failed` | 重命名失败 |
| `fs/file-closed` | 对已关闭的 `File` 执行读/写操作 |
| `fs/dir-closed` | 对已关闭的 `Dir` 执行读取或迭代操作 |

---

## 5 `std.fs` 规则

- [必须] `File.read` 的参数类型固定为 `byte[!]`，返回类型固定为 `long`。
- [必须] `File.write` 的参数类型固定为 `byte[]`，返回类型固定为 `long`。
- [必须] `File.read` 的语义直接面向 bytes，不得隐式执行 UTF-8 编码、解码或字符串拼接。
- [必须] `File.writeText` 的语义固定等价于把 `text` 当前的 UTF-8 字节序列传给 `File.write`。
- [必须] `File.readText` 的返回类型固定为 `string`。
- [必须] `File.readLines` 的返回类型固定为 `string[]`；返回内容不得包含行尾分隔符。
- [必须] `File.close` 必须幂等，重复关闭不抛出错误。
- [必须] `File.isClosed` 返回 `bool`。
- [必须] `File` 不得暴露底层文件描述符。
- [必须] `EntryKind` 为单一值枚举；每个条目恰好属于一种 kind。
- [必须] `StatInfo` 的所有字段必须只读。
- [必须] `StatInfo.kind` 由 `stat`（跟踪符号链接）获取时，不得为 `Symlink`。
- [必须] `StatInfo.changedAt` 对应 POSIX `st_ctime`（元数据变更时间），不得映射为创建时间。
- [必须] `EntryInfo.name` 不得包含路径分隔符。
- [必须] `EntryInfo` 列表不得包含 `.` 与 `..` 条目。
- [必须] `EntryInfo.stat` 必须调用 POSIX `stat`（跟踪符号链接），返回 `StatInfo`。
- [必须] `Dir.read` 的 `batchSize` 参数类型固定为 `long`，返回类型固定为 `EntryInfo[]`。
- [必须] `Dir.read` 的 `batchSize` 必须大于 `0`，否则抛出异常。
- [必须] `Dir.read` 在遍历结束后必须返回空数组。
- [必须] `Dir.next` 必须标注 `@iterator`，返回类型固定为 `IteratorResult<EntryInfo>`。
- [必须] `Dir.read` 与 `Dir.next` 共享底层遍历状态，不得混用。
- [必须] `Dir.close` 必须幂等，重复关闭不抛出错误。
- [必须] `Dir.isClosed` 返回 `bool`。
- [必须] `openDir` 的返回类型固定为 `Dir`。
- [必须] 顶层 `stat` 必须跟踪符号链接。
- [必须] 顶层 `exists`、`isFile` 与 `isDir` 必须跟踪符号链接。
- [必须] 顶层便利函数 `readText` 的签名固定为 `readText(path: string): string`。
- [必须] 顶层便利函数 `writeText` 的签名固定为 `writeText(path: string, text: string): void`。
- [必须] 顶层便利函数 `readBytes` 的签名固定为 `readBytes(path: string): byte[]`。
- [必须] 顶层便利函数 `writeBytes` 的签名固定为 `writeBytes(path: string, bytes: byte[]): void`。
- [必须] `readText`、`writeText`、`readBytes` 与 `writeBytes` 必须自动管理文件的打开与关闭。
- [必须] `exists`、`isFile` 与 `isDir` 返回 `bool`，不得抛出异常。
- [必须] `mkdir` 返回 `void`。
- [必须] `rmdir` 返回 `void`。
- [必须] `readDir` 返回 `EntryInfo[]`。
- [必须] `remove` 返回 `void`，不得删除目录。
- [必须] `rename` 返回 `void`。
- [禁止] 为 `File` 或 `Dir` 提供隐式自动关闭机制。
- [禁止] 为 `File` 或 `Dir` 提供隐式的文件锁或并发同步语义。
- [禁止] 在便利函数中重新实现一套独立文件操作行为，导致与 `File` / `Dir` 语义分叉。
- [禁止] 路径参数使用非 `string` 类型。

---

## 6 `std.fs` 实现约束

- `std.fs` 对 `extlib` 的调用必须走 C ABI 路径。
- `std.fs` 不通过 `@runtime` 访问 `extlib`。
- 底层实现可使用 `libc` 或 `libuv`，由实现选择，不要求绑定到某一个固定的 C 库。

---

## 7 关联

- [feng-language.md](./feng-language.md): 语言核心总览。
- [feng-builtin-type.md](./feng-builtin-type.md): `string`、`byte`、`byte[]` 与 `byte[!]` 的语义。
- [feng-std-io.md](./feng-std-io.md): 标准 I/O 规范，`Stdio` 的 bytes-first 与文本便利分层模式。
- [feng-std-path.md](./feng-std-path.md): 路径处理规范，纯字符串路径操作。
- [feng-interop.md](./feng-interop.md): `extern func` 与 C ABI 导入规则。
- [feng-iterator.md](./feng-iterator.md): `@iterator` 注解与 `for/in` 展开规则。
