# Feng VS Code Extension

Feng Language 为 Feng 提供开箱即用的 VS Code 编辑体验。安装扩展后，你可以直接获得语法高亮、文档格式化、面向源文件的 Feng Language Server 客户端接入、`.fm` 清单文件支持，以及面向源码、清单、包和符号表文件的 Feng 专用图标支持。

## 功能一览

- 语法高亮：覆盖 Feng 常见关键字、字符串、注释、赋值与复合运算符，以及基础语法结构，并为 `.fm` 文件提供节标题与 `#` 注释高亮。
- 文档格式化：统一缩进、空白和常见语法间距，适合日常编辑时快速整理代码；其中包含复合赋值与位移运算符的空格规范；对 `.fm` 文件会额外按节对齐 `key: "value"` 项。
- Language Server 客户端：对于 Feng 源文件，扩展会通过已配置的 Feng 可执行文件启动 `feng lsp`，并用 VS Code 标准 Language Client 连接。hover、completion、definition、references、rename、diagnostics 以及后续语言能力都从当前 CLI 暴露出来的 LSP capability 集合获取。
- 诊断兼容回退：如果当前 Feng CLI 还没有暴露任何 LSP capability，扩展会临时保留现有的 `check` 诊断链路，避免打开/保存时的基础校验回退。
- 图标支持：扩展使用 Feng Logo；当当前文件图标主题没有提供 Feng 专用图标时，会分别回退到 `.feng`/`.ff` 源文件、`.fm` 清单、`.fb` 包文件与 `.ft` 符号表文件对应的内置 Feng 图标。

## 支持的文件后缀

- `.feng`、`.ff`：Feng 源文件
- `.fm`：Feng 清单文件
- `.fb`：Feng 包文件
- `.ft`：Feng 符号表文件

## 快速开始

1. 在 VS Code 扩展市场安装 Feng Language。
2. 打开任意 Feng 源文件，扩展会自动启用语法高亮。
3. 需要整理代码时，执行 VS Code 的“格式化文档”。
4. 如果你已经安装 Feng CLI，扩展会在打开 Feng 源文件时自动启动 `feng lsp`，语言能力由该 CLI 版本实际暴露的 LSP capability 决定。
5. 如果当前 CLI 版本仍返回空的 capability 集合，扩展会暂时继续使用旧的 `check` 链路提供打开/保存诊断，直到服务端能力补齐。

## 可选配置

如果 `feng` 可执行文件不在系统 `PATH` 中，可以通过 VS Code 设置指定路径：

```json
{
	"feng.executablePath": "./build/bin/feng"
}
```

这个路径既可以是绝对路径，也可以是相对于第一个工作区根目录的路径。

当 `feng.executablePath` 保持默认值时，扩展会直接使用系统 `PATH` 中的 `feng`。

## 格式化说明

当前格式化能力主要覆盖日常开发中最常见的整理需求：

- 按 `{}`、`()`、`[]` 结构整理缩进
- 清理每行尾部多余空白
- 统一文件换行符为 `\n`
- 规范二元与复合运算符空格，例如 `a+b` → `a + b`、`total+=1` → `total += 1`、`mask>>=1` → `mask >>= 1`
- 规范参数列表与实参列表，例如 `fn add(a:int,b:int)` → `fn add(a: int, b: int)`
- 规范对象字面量与类型标注中的 `:`、`,`、`{}` 周边空格

它的目标是提供稳定、可预期的日常格式化体验；当前不会主动重排复杂表达式的整体换行布局，也不会做跨行对齐。

对 `.fm` 清单文件，格式化器还会额外处理：

- 规范 `[section]` 节标题写法
- 规范 `#` 注释前缀空格
- 按节对齐 `key: "value"`，让值起始列保持一致

## Language Service 说明

- Feng 源文件通过标准 VS Code Language Client 启动 `feng lsp`。
- 默认执行程序名为 `feng`，按系统 `PATH` 查找。
- 如果你的 CLI 不在默认路径中，请通过 `feng.executablePath` 指定可执行文件位置。
- 内置 formatter 与 TextMate grammar 保持不变；只有语言服务能力改走 LSP。
- 如果当前安装的 CLI 还返回空的 LSP capability 集合，扩展会临时保留旧的诊断实现：项目文件走 `feng check --format json <file>`，独立文件走 `feng tool check <file>`。
