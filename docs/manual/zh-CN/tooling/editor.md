# 编辑器支持

Feng 官方 VS Code 插件提供语法高亮、格式化、语言服务和调试集成。

## 安装插件

从 [Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=houfeng.feng-language) 安装 **Feng Language**。

插件识别以下文件：

- `.feng`、`.ff`：Feng 源文件。
- `.fm`：Feng 项目或包清单。
- `.fb`：Feng Bundle。
- `.ft`：Feng 符号表。

## 文档注释

在 Feng 源文件中输入 `/**` 时，插件会自动补全带前导空格的文档注释结束符。此时按回车键，VS Code 会展开并对齐标准的多行文档注释结构：

```feng
/**
 *
 */
```

## CLI 路径

插件默认从 `PATH` 启动 `feng`。如果命令不在 `PATH` 中，在 VS Code 设置中指定：

```json
{
  "feng.executablePath": "/absolute/path/to/feng"
}
```

也可以使用相对于第一个工作区根目录的路径：

```json
{
  "feng.executablePath": "./build/bin/feng"
}
```

该设置同时用于 `feng lsp` 和 `feng dap`。

## 语言服务

打开 Feng 文件后，插件会启动 `feng lsp`。当前语言服务支持：

- 诊断。
- 悬停信息。
- 自动补全。
- 跳转到定义。
- 查找引用。
- 重命名。

更换或重新构建 CLI 后，从命令面板执行 **Feng: Restart Language Server**，或点击状态栏中的 Feng LSP 状态项。

## 调试

在“运行和调试”视图创建 Feng 配置。插件会为可执行项目建立构建任务，并使用 `feng dap --stdio` 启动调试会话。

生成配置时，插件优先从当前文件最近的 `feng.fm` 推导 `program`、工作目录和构建任务。需要手动配置时，核心字段包括：

```json
{
  "type": "feng",
  "request": "launch",
  "name": "Debug Feng",
  "program": "${workspaceFolder}/build/macos-arm64/bin/myapp",
  "cwd": "${workspaceFolder}",
  "args": []
}
```

`program` 中的平台目录应与本机实际构建平台一致。当前调试使用本地非 release 构建。
