# 第一个项目

本章把最小程序扩展为包含多个文件和模块的可执行项目。

## 初始化

```bash
mkdir greeting
cd greeting
feng init greeting
```

初始化后的核心结构为：

```text
greeting/
├── feng.fm
└── src/
    └── main.ff
```

## 增加模块

创建 `src/greeting.ff`：

```feng
open module greeting.message;

open func make_greeting(name: string): string {
  return "你好，" + name;
}
```

修改 `src/main.ff`：

```feng
module greeting;

import greeting.message;
import std.collections;
import std.io;

func main(args: string[]) {
  let name = if args.length() > 1 {
    args[1];
  } else {
    "Feng";
  };
  println(make_greeting(name));
}
```

模块名与文件路径彼此独立：同一模块可以分布在多个 `.ff` 文件中，但每个文件只能声明一个模块。要让其他模块导入声明，模块和顶层声明都必须公开。

## 运行与传参

```bash
feng run
feng run -- Alice
```

`args[0]` 是程序路径，因此第一个用户参数位于 `args[1]`。

## 项目清单

默认可执行项目的 `feng.fm` 类似：

```text
[package]
name: "greeting"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"
```

如果发行包随附了标准库等包，`feng init` 还会把它们以精确版本写入 `[dependencies]`。不要手动编辑生成的构建产物；项目输入由 `feng.fm` 和 `src/` 下的源文件组成。

## 常用开发循环

```bash
feng check
feng run -- Alice
feng clean
```

`check` 适合编辑过程中的快速反馈，`run` 会先构建再运行，`clean` 会删除当前项目的全部构建产物。完整说明见[构建与运行](../projects/build-and-run.md)。
