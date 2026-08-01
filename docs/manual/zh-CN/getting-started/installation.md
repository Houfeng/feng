# 安装 Feng

本章帮助你安装 Feng 工具链并确认命令可用。

## 支持的平台

当前官方安装包支持以下主机：

- Apple Silicon Mac：`macos-arm64`
- x86-64 Linux：`linux-x64-gnu`
- ARM64 Linux：`linux-arm64-gnu`

Windows、Intel Mac 与纯 musl Linux 主机当前没有官方工具包。

## 快速安装

在终端执行：

```bash
curl -fsSL https://feng-lang.com/install.sh | bash
```

安装脚本会下载与当前主机匹配的最新稳定版本，将工具链安装到 `~/.feng`，并把 `~/.feng/bin` 加入当前 shell 的启动文件。安装结束后重新打开终端，或按安装脚本最后输出的提示重新加载启动文件。

确认安装结果：

```bash
feng --version
feng --help
```

## 安装指定版本

安装脚本接受完整的 GitHub Release 标签。通过管道执行脚本时，用 `bash -s --` 传递参数：

```bash
curl -fsSL https://feng-lang.com/install.sh | bash -s -- --version=v0.1.0
```

版本必须包含 `v` 前缀。再次运行安装脚本会原子替换现有的 `~/.feng` 安装；安装失败时，脚本会恢复原安装。

## 安装内容

`~/.feng` 中包含：

- `bin/feng`：Feng 命令行工具。
- `pkg/`：随发行包提供的 Feng 包。
- `include/` 与 `lib/`：运行时头文件和各目标平台运行时库。
- `toolchain/`：构建与调试所需的 LLVM 工具及目标 sysroot。

安装脚本需要 `curl`、`unzip` 与常见 POSIX 命令。脚本会根据 `SHELL` 更新 zsh、bash、fish、ksh 或默认 profile 的启动文件。

## 手动安装

也可以从 [Feng Releases](https://github.com/Houfeng/feng/releases) 下载与主机匹配的压缩包。解压后应保持发行目录结构完整，并将其中的 `bin` 目录加入 `PATH`；不要只复制 `feng` 可执行文件，因为编译、链接和调试还需要同一发行包中的运行时与工具链。

## 下一步

继续阅读[快速开始](./quick-start.md)，创建并运行第一个 Feng 程序。
