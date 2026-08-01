# 文件系统与 I/O

终端 I/O、路径处理和文件系统分别位于 `std.io`、`std.path` 与 `std.fs`。

## 标准输入输出

```feng
import std.io;

print("Name: ");
let name = readLine();
println("Hello, {0}", name);
```

`print` 不追加换行，`println` 追加一个换行。`stdio` 提供更底层的字节读写、标准错误输出和按行读取：

```feng
stdio.writeErrorLine("warning");
```

完整语义见[标准库 I/O 规范](../../specifications/feng-std-io.md)。

## 路径

`std.path` 只处理字符串，不访问文件系统：

```feng
import std.path;

let path = join("data", "users", "alice.json");
let parent = dirname(path);
let name = basename(path);
let extension = extname(path);
let normalized = normalize("data/./users/../config");
```

路径分隔符固定为 `/`。`resolve(base, target)` 使用调用方给出的 base，不读取当前工作目录。完整语义见[Path 规范](../../specifications/feng-std-path.md)。

## 文件便利函数

```feng
import std.fs;

writeText("message.txt", "你好世界");
let content = readText("message.txt");

if exists("message.txt") && isFile("message.txt") {
  rename("message.txt", "greeting.txt");
}
```

还可以使用 `readBytes`、`writeBytes`、`stat`、`isDir` 和 `remove`。

## 显式文件生命周期

```feng
let file = File.create("data.bin", FileMode.ReadWriteCreate);
defer {
  file.close();
}

file.writeText("Feng");
```

`File` 提供字节和文本读写、状态查询以及显式 `close()`。关闭后的文件不能继续读写。

## 目录

```feng
mkdir("output");
let entries = readDir("output");

let directory = Dir.create(".");
defer {
  directory.close();
}

for let entry in directory {
  println(entry.name);
}
```

目录遍历结果使用 `EntryInfo`，条目类别使用 `EntryKind`。文件系统错误通过 Feng 异常报告；具体行为见[Filesystem 规范](../../specifications/feng-std-fs.md)。
