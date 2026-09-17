# Feng

[English](README.md) · **简体中文** · [Español](README.es.md) · [Português](README.pt-BR.md) · [日本語](README.ja.md) · [Русский](README.ru.md) · [Deutsch](README.de.md) · [Français](README.fr.md)

Feng 是一门语法简洁的静态类型编译型语言，提供显式契约与自动内存管理。

[官网](https://feng-lang.com/index-zh.html) · [用户手册](docs/manual/zh-CN/README.md) · [下载发行版](https://github.com/Houfeng/feng/releases)

## 特性

- **强类型与静态类型**：支持类型推导、泛型、闭包与模式匹配。
- **显式契约**：通过 `spec` 声明契约，通过 `fit` 为类型适配契约或补充扩展成员。
- **自动内存管理**：使用自动引用计数（ARC）与循环引用回收管理托管对象的生命周期。
- **C 互操作**：通过 `extern func` 与 ABI 注解调用 C 库。
- **配套工具**：提供项目构建、依赖管理、语言服务与调试支持，以及 VS Code 和 Zed 插件。

## 安装

官方安装包支持 Apple Silicon Mac，以及 x86-64、ARM64 的 GNU/Linux 主机。

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

安装完成后重新打开终端，执行 `feng --version` 确认安装成功。版本选择与手动安装见[安装指南](docs/manual/zh-CN/getting-started/installation.md)。

## 快速开始

在空目录中创建项目：

```bash
mkdir hello_feng
cd hello_feng
feng init hello_feng
```

将生成的 `src/main.ff` 改为：

```feng
module hello_feng;

import std.io;

func main(args: string[]) {
  println("Hello, Feng!");
}
```

运行程序：

```bash
feng run
```

输出 `Hello, Feng!`。使用 `feng check` 检查项目，使用 `feng build --release` 构建发布版本。继续阅读[第一个项目](docs/manual/zh-CN/getting-started/first-project.md)。

## 文档

- [语言与工具规范](docs/specifications/README.md)
- [标准库指南](docs/manual/zh-CN/standard-library/README.md)
- 编辑器支持：[VS Code](editors/feng-vscode/README-zh_CN.md)、[Zed](editors/feng-zed/README.md)
- [工程文档](docs/engineering/README.md)

## 从源码构建

准备 Clang、Make 和 Git LFS；macOS 还需要 Xcode Command Line Tools。

```bash
git lfs install
git clone https://github.com/Houfeng/feng.git
cd feng
git lfs pull
make all
```

编译器位于 `build/bin/feng`。执行 `make test` 运行全量回归测试，其中 `test/` 验证编译器与运行时，`fcts/` 验证语言行为。

## 许可证

[MIT](LICENSE)
