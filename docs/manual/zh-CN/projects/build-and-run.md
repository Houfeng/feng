# 构建与运行

本章介绍普通项目开发使用的构建命令。完整参数见[CLI 规范](../../../specifications/feng-cli.md)，平台行为见[编译与构建规范](../../../specifications/feng-build.md)。

## 构建项目

```bash
feng build
feng build --release
```

可以传入项目目录或 `feng.fm` 路径：

```bash
feng build ./examples/hello_world
feng build ./examples/hello_world/feng.fm
```

调试构建保留便于调试的信息；`--release` 启用发布构建模式。

## 运行项目

```bash
feng run
feng run --release
feng run -- argument-one argument-two
```

`run` 只运行当前主机平台的 `target: "bin"` 项目。库项目不能直接运行。

## 目标平台

未在 `feng.fm` 声明 `platform` 时，不带参数的 `build` 默认构建主机平台。可以显式选择平台：

```bash
feng build --platform=macos-arm64
feng build --platform=linux-x64-gnu
feng build --platform=linux-arm64-musl
```

`--platform` 可重复传入。若清单已经声明平台列表，该列表同时是默认目标集合和允许列表。

Linux 平台名必须包含 `gnu` 或 `musl`。目标标识合法并不保证当前安装拥有所需工具链或 SDK。

## 自定义 sysroot

单平台构建可以显式指定 sysroot：

```bash
feng build --platform=linux-x64-musl --sysroot=/opt/sysroots/musl-x64
```

一次指定 `--sysroot` 时只能构建一个平台。Feng 不会下载、复制或替用户授权第三方 SDK。

## 打包库

`target: "lib"` 项目使用：

```bash
feng pack
feng pack --platform=macos-arm64 --platform=linux-x64-gnu
```

`pack` 固定执行 release 构建，成功后在 `<out>/pkg/` 生成 `.fb`。所有请求的平台都必须构建成功，才会产生最终包。

## 清理

```bash
feng clean
```

该命令删除当前项目清单所对应的全部构建产物。

## 直接编译源文件

第三方构建系统或高级场景可以直接传入源文件：

```bash
feng src/main.ff --target=bin --name=myapp --out=build
```

直接编译模式不读取 `feng.fm`，也不解析依赖树。日常项目开发应优先使用 `build`、`run` 和 `check`。
