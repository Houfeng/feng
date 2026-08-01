# feng build / fb 对齐优化 TODO

目标：让实现与当前规范一致（`[assets]` 配置、`extlib` 分发（可含静态库与动态库）、`target=bin/lib` 处理边界），并补齐测试与回归验证。

## 0. 现状差距（已确认）

- `feng.fm` 解析器当前只处理 `[package]` / `[dependencies]` / `[registry]`，尚未解析 `[assets]`。
- `.fb` 打包当前只写入 `mod/` 与 `lib/<platform>/...`，未写入 `extlib/` 与 `[assets]` 指定资源。
- `target=bin` 构建链路虽有动态库释放策略描述，但尚未与 `extlib/` 目录形成明确实现闭环。
- 文档已收敛为 `lib + extern/extlib` 链接来源，需保证实现侧不引入额外隐式来源。

## 1. Manifest 层：补齐 `[assets]` 解析与数据结构

实现职责：

- `feng build`：实现 `[assets]` 解析、校验、写回与诊断。

- [x] 1.1 在 `FengCliProjectManifest` 中新增 assets 映射结构（目标目录 -> 源路径）。
- [x] 1.2 扩展 `manifest.c` 解析逻辑，支持 `[assets]` 节并校验：
	- key 非空（目标目录）。
	- value 非空（相对 `feng.fm` 目录路径）。
	- 禁止与现有字段冲突（节内重复 key 报错）。
- [x] 1.3 扩展 manifest 写回逻辑，按稳定顺序输出 `[assets]`。
- [x] 1.4 错误信息统一：包含文件、行号、字段名。

验收：

- 能正确解析如下示例：

```text
[assets]
extlib: "lib/"
```

- 非法输入（空 key / 空 value / 重复 key）给出明确诊断。

## 2. Build 层：实现 `[assets]` 复制逻辑

实现职责：

- `feng build`：实现 `target=bin/lib` 下 assets 路径解析、复制与冲突策略。

- [x] 2.1 统一路径解析基准：`[assets]` 的 value 一律相对 `feng.fm` 所在目录。
- [x] 2.2 `target=bin`：将每条 assets 映射复制到可执行文件同级目标目录。
- [x] 2.3 `target=lib`：将每条 assets 映射复制到打包 staging（用于后续写入 `.fb`）。
- [x] 2.4 复制策略明确：
	- 目录递归复制。
	- 保持相对层级。
	- 目标目录已存在时按构建产物刷新为最新内容。

验收：

- `target=bin` 产物目录出现配置目标目录及文件。
- `target=lib` 打包前 staging 中出现对应目录树。

## 3. FB 打包层：加入 `extlib/` 与 assets 目录写入

实现职责：

- `feng build`：实现 `.fb` 写包扩展（`extlib` 与 assets 目录）。

- [x] 3.1 扩展 `FengFbLibraryBundleSpec`，增加可选目录输入：
	- `extlib_root`（按平台分发目录）
	- `assets_entries`（目标目录映射）
- [x] 3.2 `fb.c` 写包时新增目录写入：
	- `extlib/<platform>/...`（若存在，允许同时包含静态库与动态库）
	- `[assets]` 指定的目标目录树
- [x] 3.3 保持现有 `mod/` + `lib/` 写入逻辑回归测试通过。

验收：

- `.fb` 解压后包含：`mod/`、`lib/`、可选 `extlib/`、可选 assets 目标目录。
- 未配置时不生成空目录噪音。

## 4. extlib 动态库释放

实现职责：

- `feng` 顶层命令：编译完成后，将 `extlib/<platform>/` 下的动态库释放到可执行文件同级目录（静态库只链接不释放）。

规则约束：`extlib` 中静态库（`.a/.lib`）仅参与链接，不参与运行期释放。

- [x] 4.1 `target=bin` 由 `feng` 顶层命令执行释放逻辑，只扫描 `extlib/<platform>/` 下动态库后缀：
	- Linux: `.so`
	- macOS: `.dylib`
	- Windows: `.dll`
- [x] 4.2 释放目标固定为可执行文件同级目录。
- [x] 4.3 不释放 `lib/` 下 feng 正式静态库；`extlib` 中静态库（`.a/.lib`）只链接不释放。

验收：

- 有 `extlib` 动态库时会释放到可执行文件同级。
- `extlib/*.a|*.lib` 与 `lib/*.a|*.lib` 均不会触发运行期释放动作（`extlib` 静态库仅链接）。

## 5. 链接来源边界：实现与文档一致

实现职责：

- `feng build`：将依赖树平铺为 `--pkg` 等参数后传给 `feng`，不再参与链接参数拼装。
- `feng` 顶层命令：负责根据传入参数拼装链接来源，禁止引入任何隐式扫描来源。

- [x] 5.1 feng 库链接来源：
	- `.fb` 的 `lib/<platform>`
	- `extern func` 元信息解析出的原生库
- [x] 5.2 禁止新增隐式扫描来源（磁盘全盘扫描、未声明目录自动注入）。
- [x] 5.3 `extlib` 中静态库若参与链接，必须通过显式规则接入（例如 `extern func` 元信息），不做目录自动注入。

验收：

- 代码路径中不存在新增隐式库发现逻辑。

## 6. 测试补齐（不改旧用例语义）

- [x] 6.1 Manifest 单测：`[assets]` 合法/非法解析。
- [x] 6.2 Build 集成：
	- `target=bin` 复制 assets 到同级目标目录。
	- `target=lib` 将 assets 打进 `.fb`。
- [x] 6.3 Bundle 集成：`extlib` + assets 目录写包与解包校验。
- [x] 6.4 Runtime 集成：验证 `extlib` 静态库只链接不释放，且仅 `extlib` 动态库触发释放。
- [x] 6.5 回归：现有 `pack/deps/build` 主链通过。

## 7. 回归与发布前检查

- [x] 7.1 全量测试所有 cases 通过。
- [x] 7.2 文档-实现一致性复核：
	- `docs/specifications/feng-build.md`
	- `docs/specifications/feng-package.md`
	- `docs/specifications/feng-cli.md`
- [x] 7.3 变更说明整理：新增行为、冲突策略、已知限制。

变更说明：

- `[assets].extlib` 在 `target=lib` 下直接 staging 到 `build/extlib/`,打包后对应 `.fb/extlib/`,不额外插入 `assets/` 目录层。
- `target=bin` 仅从依赖包 `.fb/extlib/<当前平台>/` 释放动态库到可执行文件同级目录；`extlib` 静态库与 `.fb/lib/<平台>/` 正式静态库都不会被运行期释放。
- 运行期释放若遇到多个依赖包提供同名动态库,构建直接报错,避免输出目录静默覆盖。
- 链接来源边界维持为 `.fb/lib/<平台>` 与 `extern func` 元信息；`extlib` 不做目录自动注入。
