# Feng CLI

`feng` 同时提供项目工作流、语言服务入口和高级编译诊断。完整命令定义见[CLI 规范](../../specifications/feng-cli.md)，精确帮助文本见[CLI 帮助基准](../../specifications/feng-cli-help.txt)。

## 全局帮助

```bash
feng --help
feng --version
feng build --help
```

参数错误时，CLI 会先说明原因，再输出对应 usage，并以非零状态退出。

## 日常项目命令

| 命令 | 用途 |
| --- | --- |
| `feng init [name] [--target=bin\|lib]` | 在空目录初始化项目 |
| `feng check [path] [--format text\|json]` | 检查整个项目，不生成最终制品 |
| `feng build [path] [--release]` | 构建项目 |
| `feng run [path] [--release] [-- args...]` | 构建并运行可执行项目 |
| `feng clean [path]` | 删除项目构建产物 |
| `feng pack [path]` | release 构建并打包库项目 |

大多数项目命令的 `path` 可以是项目目录或 `feng.fm`。省略时使用当前目录的清单。

## 依赖命令

```bash
feng deps add <name> <version-or-path>
feng deps remove <name>
feng deps install
feng deps install --force
```

Feng 使用精确版本，不提供 `deps update`。升级依赖时再次执行 `deps add` 写入新版本。

## 平台选项

`build` 和 `pack` 支持重复的 `--platform=<platform>`。单平台时还可使用 `--sysroot=<path>`。`run` 固定选择当前主机平台，不接受 `--platform`。

## 语言服务与调试服务

```bash
feng lsp --stdio
feng dap --stdio
```

这两个入口供编辑器客户端启动，不需要在普通交互式终端中直接使用。

## 高级诊断

```bash
feng tool lex <file>
feng tool parse <file>
feng tool semantic <file> [more-files...]
feng tool check <file> [more-files...]
feng tool compile <file>
```

`tool` 面向编译细节排查和第三方工具集成。普通项目优先使用 `feng check` 和 `feng build`。

## 当前没有的命令

- 没有 `feng test`：测试是普通可执行项目，通过 `feng run` 执行。
- 没有 `feng fmt`：当前格式化由编辑器插件提供。
- 没有 `feng publish`：当前没有官方 registry 发布工作流。
- 没有 `feng deps update`：依赖版本始终精确。
