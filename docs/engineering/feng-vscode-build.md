# VS Code 插件构建

状态：已交付。

## 构建入口

在仓库根目录运行 `bash ./scripts/build_editor_extentions.sh`。该脚本进入 `editors/feng-vscode/` 后执行 `npm run pack`；也可以在插件目录直接执行该命令。

## 测试与打包顺序

- 插件使用 npm 的 `prepack` 钩子执行 `npm test`，统一约束上述两个构建入口。
- `npm test` 执行 `package.json` 中 `scripts.test` 列出的插件 JavaScript 测试；测试清单在该字段维护。
- 任一测试失败时，命令返回非零退出码，不执行后续 VSIX 打包。
- 测试全部成功后，`pack` 调用仓库本地的 `vsce package` 生成 VSIX；`npm run pack -- ...` 的打包参数继续传递给 VSCE。

仓库的 `make test` 与插件 JavaScript 测试是独立的验证入口。涉及非文档变更时，仍按仓库要求在沙箱外执行 `make test`。

## 调试冒烟测试

调试冒烟用例的临时项目使用当前 [标准库目录布局](../../std/README.md)，并按 [CLI 规范](../specifications/feng-cli.md) 的宿主平台产物布局检查生成的可执行文件与调试信息，随后验证真实 DAP 会话中的断点命中及源码堆栈映射。

## 验证

- 在隔离测试目录复用实际 `prepack` / `pack` 配置，令 JavaScript 测试返回退出码 23：`npm run pack` 同样返回 23，VSCE 未执行。
- 沙箱外执行真实构建入口 `bash ./scripts/build_editor_extentions.sh`，并将 `TMPDIR` 指向工程 `temp/`：8 组 JavaScript 测试全部通过，包括真实调试冒烟测试；日志确认所有通过结果均出现在 VSCE 打包之前。
- 已生成 `editors/feng-vscode/feng-language-0.1.18.vsix`，核对包内配置、新图标及原图备份均与工作区一致。
- 沙箱外执行 `make test`：通过，退出码为 0，覆盖 UBSan 与普通构建两个阶段。
