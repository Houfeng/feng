# 快速开始

本章用一个最小项目介绍创建、运行和检查 Feng 程序的基本流程。

## 创建项目

`feng init` 只能在空目录中执行：

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

命令会创建项目清单 `feng.fm` 和入口文件 `src/main.ff`。

## 编写程序

将 `src/main.ff` 改为：

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("你好世界");
}
```

每个源文件首先声明所属模块。`import std.io;` 将标准输出函数引入当前文件，`main(args: string[])` 是可执行项目的入口。

## 运行程序

在包含 `feng.fm` 的目录中执行：

```bash
feng run
```

输出：

```text
你好世界
```

传递程序参数时，在 Feng 的选项之后使用 `--`：

```bash
feng run -- first second
```

参数会出现在 `main` 的 `args` 数组中。

## 检查与构建

只做项目级语义检查，不生成最终制品：

```bash
feng check
```

构建调试版本：

```bash
feng build
```

构建发布版本：

```bash
feng build --release
```

开发产物按平台放在 `build/<platform>/` 中。可执行文件位于 `build/<platform>/bin/`。

## 下一步

阅读[第一个项目](./first-project.md)了解多文件组织，然后按顺序学习[值与绑定](../language/values-and-bindings.md)及后续语言章节。
