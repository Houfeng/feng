# Feng Zed 插件

Feng Zed 插件提供以下能力：

- 语法高亮（Tree-sitter + highlights 查询）
- LSP（启动 `feng lsp --stdio`）
- DAP（启动 `feng dap --stdio`）

## 目录结构

- `extension.toml`：Zed 扩展清单
- `src/lib.rs`：Rust 扩展入口（LSP/DAP 启动命令）
- `languages/feng/config.toml`：语言元数据与文件后缀
- `languages/feng/highlights.scm`：语法高亮规则
- `languages/feng/brackets.scm`：括号配对规则
- `debug_adapter_schemas/feng.json`：DAP 配置 schema
- `grammars/tree-sitter-feng`：最小可用 tree-sitter 语法仓库

## 本地开发安装

1. 在 Zed 中执行 `zed: install dev extension`
2. 选择当前目录：`editors/feng-zed`

## 依赖

- `feng` 可执行文件可在 PATH 中找到，或在 Zed 环境中可访问
- LSP 命令：`feng lsp --stdio`
- DAP 命令：`feng dap --stdio`

## 调试配置示例

在项目根目录创建 `.zed/debug.json`：

```json
{
  "adapters": {
    "feng": {
      "type": "feng"
    }
  },
  "configurations": {
    "Feng Launch": {
      "adapter": "feng",
      "request": "launch",
      "program": "${ZED_WORKTREE_ROOT}/build/bin/your_program",
      "cwd": "${ZED_WORKTREE_ROOT}",
      "args": []
    }
  }
}
```

## 说明

- 当前语法高亮基于内置最小语法，重点覆盖关键字、注释、字符串、数字、类型和函数调用。
- 更精细的语法结构可在后续迭代中增强 `tree-sitter-feng` 的 grammar 与 queries。
