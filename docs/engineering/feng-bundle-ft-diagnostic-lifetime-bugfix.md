# 二进制包 FT 读取失败的诊断生命周期修复

> **状态：独立修复完成，容器全量回归通过，待人工 Review。日期：2026-09-29。**
>
> 基线：`d3eb9a08dc20ddd216c02c848e1f2740e1c54866`。
> 开发者要求先备份并撤销本轮 `@memory` 实现，再单独修复本问题。
> 本修复不恢复 `@memory`、FT 新字段、函数属性或插件处理。

## 1. 问题与证据

`src/cli/compile/driver.c` 的 `scan_bundle_dependencies` 读取 `.fb` 内的公开 FT。
当 `feng_symbol_ft_read_bytes` 失败时，原分支先释放 `source_name`，随后又把它
传给 `set_errorf` 构造错误消息，存在释放后读取；底层 `FengSymbolError` 也未释放。

触发条件是 ZIP 本身可读取，但其中的 FT 内容不合法。这属于编译失败路径的资源
生命周期问题，与本轮内存推导无关。前轮已在基线 driver 上通过 ASan 复现，见
[历史验证记录](../../temp/apple-container-verify-20260929/RESULTS.md)。
本次新增独立坏 FT 用例，不依赖已撤销的内存摘要格式，并重新执行修复前后对照。

## 2. 修复范围

在路径和底层诊断仍有效时，先构造拥有自身字符串存储的上层错误消息；随后释放
底层错误、FT 缓冲区、路径及 ZIP reader，最后返回构造结果。

不修改成功编译路径、错误消息格式、FT 协议、语言语义、runtime ABI 或异常插件。
生产代码只修复这一处失败分支，不借本轮扩大到其他诊断路径。

## 3. 测试与验收

新增 CLI 回归用例生成合法 ZIP，并放入截断 FT、损坏头部和合法结构中的损坏数据，
验证 driver 返回失败，诊断包含完整的包路径、FT entry 路径及底层失败原因。
同时覆盖包路径含空格、多个坏包连续诊断和正常 FT 读取对照；测试不调用外部 C 编译器。

测试归入 `test/`，因为它验证驱动错误诊断和资源生命周期，不新增 Feng 语言行为。
用例独立新增并接入原 CLI 测试入口，不改动任何既有用例的输入或断言。

- [x] 保存完整本地备份，并将本轮生产代码、测试和构建接入恢复到基线。
- [x] 先形成本文，明确问题、修复边界与独立测试要求。
- [x] 单独实现先构造诊断、后清理资源的修复。
- [x] 新增不依赖内存摘要的 CLI 回归用例。
- [x] 容器中以基线 driver 运行新用例，确认 ASan 复现释放后读取。
- [x] 容器中验证修复后专项用例通过，ASan／UBSan／泄漏检查无错误。
- [x] 在隔离源码与基线异常插件下执行完整 `make test`。
- [x] 回填实际结果，等待人工 Review，不自动提交。

本轮证据保存于 `temp/apple-container-verify-20260929/memory-rollback/`。
独立补丁与验证日志另存于仓库外备份目录中的 `verification-evidence.tar.gz`，
位置见[回撤备份说明](../../temp/memory-rollback-20260929-0JCCMY/README.md)。
macOS 原生执行仍按开发者安排后续进行，不将容器结果等同于 macOS 验收。

## 4. 验证记录

环境为 Apple container 内 Ubuntu 26.04／Linux arm64，Clang 22.1.8。
使用新隔离目录 `/workspace/feng-ft-diagnostic-20260929`，从回撤后的源码重建
不含 FunctionContracts 的异常插件，不复用本轮已终止实现的构建目录或包缓存。

| 检查 | 实际结果与证据 |
| --- | --- |
| 基线 driver ＋新增用例 | 退出码 1；ASan 在 `scan_bundle_dependencies` 的原 `driver.c:636` 报 heap-use-after-free，见 [baseline-asan.txt](../../temp/apple-container-verify-20260929/memory-rollback/baseline-asan.txt)。 |
| 独立修复 ＋同一用例 | 8 项通过，ASan／UBSan 和 detect_leaks=1 无报错，见 [fixed-run.log](../../temp/apple-container-verify-20260929/memory-rollback/fixed-run.log)。 |
| 宿主与容器源码一致性 | driver 与新增测试的 SHA-256 相同，见 [final-source-sha256.txt](../../temp/apple-container-verify-20260929/memory-rollback/final-source-sha256.txt)。 |
| 完整 make test | 退出码 0；ASan／UBSan 与普通阶段均通过 std 607/607、FCTS 1666/1666，失败与跳过均为 0，见 [make-test.log](../../temp/apple-container-verify-20260929/memory-rollback/make-test.log)。 |

8 项由三种坏 FT 和一种有效 FT 对照，分别在普通路径及含空格路径运行组成。
有效 FT 对照故意不提供静态库，确认越过 FT 解析后得到独立的缺库诊断；不调用 Clang。
既有测试用例输入和断言未修改，只新增源文件并注册到 CLI 测试入口和构建列表。

完整回归于 2026-09-29 04:36:24–04:49:53 UTC 执行，约 13 分 29 秒。两阶段
均执行了新增 8 项检查及原有编译器／CLI／Symbol 测试；smoke 91 项、性能约束、
增量构建、发布／安装与异常插件接入检查均通过。发布脚本的模拟 macOS 检查通过，
不表示执行了 macOS 原生 Feng 程序；原生回归仍留待开发者安排。

验证准备曾遇到临时目标未创建输出目录、macOS AppleDouble 旁文件被当作 C
源码两项问题。已修正临时驱动并使用不含元数据的源码归档；未修改生产编译规则或
已有测试来绕过它们。失败日志与后续完整成功日志均保留于上述证据目录。
