# Phase 1A 模块级任务分解

> 本文档把 [feng-plan.md](./feng-plan.md) 中 Phase 1A 的目标拆成可独立验收的工作单元，作为后续实施与评审的基线，仅描述「做什么 / 验收口径」，不重复语言规范本身的语义。

## 1 范围与出口

### 1.1 入口（来自 feng-plan §3 Phase 1A）

- 后端与运行时骨架：建立 C 发码主链路 + 最小运行时骨架。
- 确定性 ARC 核心：`string` / 数组 / 普通 `type` / 闭包环境的 retain/release、作用域退出清理、异常路径清理、单路径终结器执行。
- 本地源码 → C 发码最小闭环：模块级 `let`/`var`、顶层 `fn`、普通 `type`、构造函数、方法调用、对象字面量、数组、控制流、异常展开、`main` 入口。

### 1.2 验证里程碑（硬门槛）

- 新增 `examples/phase1a/*.ff` 及 `test/smoke/phase1a/*` smoke 集合：
  - hello 子集：模块级绑定、顶层 `fn`、`type` + 字段初始化、`@cdecl("c")` 调用 `printf` 等，编译为 C 后链接生成可执行文件且运行结果与期望一致。
  - control flow 子集：`if`/`while`/`for` 三段式与 `for/in`、`break`/`continue`、数组下标读写。
  - exception 子集：`throw` / `try`/`catch` / `finally`，覆盖正常路径与 `throw` 跨多层 `finally` 的展开。
- 现有 `make test`（`test_lexer`/`test_parser`/`test_semantic`）保持全绿。
- `examples/hello_world.ff`、`examples/debug.ff` 因当前包含 Phase 1A 子集之外的语义（方法重载、`match` 表达式、`spec` 等），按 P1 决策保持不动，**不**作为 1A 的硬验证目标。

### 1.3 暂不属于 1A（明确推迟）

- 循环检测器、终结器复活、复杂多阶段回收 → Phase 1B。
- 项目级 CLI：`init`/`build`/`run`/`check`/`clean`/`pack`/`deps` → Phase 3。
- 外部包（`.fb`/`.fi` 解析、`use` 跨包消解）→ Phase 4。
- C ABI 完整桥接（除 `@cdecl` 调用 libc 之外的扩展）→ Phase 5。
- 标准库 → Phase 6。

## 2 工作分解（模块）

每个任务都附「输入 / 产物 / 验收」，便于单独提交、单独评审。

### T1 后端入口与 IR 边界

- 输入：`FengSemanticAnalysis` + 各 `FengProgram` AST（已附着 `FengResolvedCallable`）。
- 产物：`src/codegen/codegen.h`、`src/codegen/codegen.c`（仅入口与上下文骨架，不含具体发码）。
- 验收：编译器侧能调用 `feng_codegen_emit_module(...)` 得到一段空 C 翻译单元（含 runtime 头引用 + 前置声明区），无类型推导回归。

### T2 语义元数据补充

- 输入：现有 `analyzer.c` 中私有 `InferredExprType`、本地符号解析。
- 产物：在 `parser.h` 暴露最小化的 `FengInferredType`（kind + 内建类型枚举 / 类型 decl 引用 / array 元素类型链表），并由 analyzer 在如下节点写入：
  - 表达式（call/字面量/binary/unary/cast/identifier/member/index/lambda）的 `inferred_type` 槽。
  - `let`/`var` 绑定的最终类型（解决 `let x = expr;` 的推导落点）。
  - 方法/函数体在 `return`/`throw` 上的目标类型。
- 验收：`make test` 全绿；新增 `test/semantic/test_semantic_inferred.c` 抽样验证多种节点的 `inferred_type` 与 §3.1 表格一致；不对外承诺除上述节点之外的填充。

### T3 运行时骨架

- 输入：[feng-lifetime.md](../docs/feng-lifetime.md) §4、§12.1、§12.2、§13.2 的 ARC 核心约束。
- 产物：`src/runtime/feng_runtime.h` 公共头 + `src/runtime/`（`feng_object.c`、`feng_string.c`、`feng_array.c`、`feng_panic.c`、`feng_exception.c`）。
- 验收：`build/lib/libfeng_runtime.a` 可独立链接；新增 `test/runtime/test_runtime.c` 覆盖 retain/release 计数、字符串拼接、数组读写越界 panic、异常 throw/catch/unwind、终结器单路径调用各 ≥ 1 个用例。

### T4 C 发码核心

- 输入：T1 入口 + T2 元数据 + T3 运行时 ABI。
- 产物：`src/codegen/`（`emit_module.c`、`emit_decl.c`、`emit_stmt.c`、`emit_expr.c`、`emit_type.c`、`mangle.c`、`naming.c`）。
- 子任务（按 1A 子集）：
  - C1 顶层声明：模块级 `let`/`var`、顶层 `fn`、普通 `type`（字段 + 构造函数 + 方法 + 单路径终结器），方法重载与 `spec`/`fit` 不在 1A。
  - C2 表达式：标识符、字面量（含字符串 → `feng_string_literal_n`）、对象字面量、数组字面量、二元 / 一元 / 比较 / 逻辑、字段访问、方法调用、`extern fn` 调用（仅 `@cdecl`）、cast（限内建数值与 `string` ↔ `string` 的恒等、显式数值转换）。
  - C3 语句：`let`/`var`、赋值、`if`、`while`、`for`（三段式 + `for/in` 的数组迭代）、`break`/`continue`、`return`、`throw`、`try`/`catch`/`finally`、表达式语句。
  - C4 ARC 自动插入：作用域出口 release、临时值 release、异常展开 release、构造返回值 ownership 转移。
- 验收：T4 完成后，T6 的 smoke 全集可编译、链接、运行通过。

### T5 CLI 直接编译模式

- 输入：[feng-cli.md](../docs/feng-cli.md) §2、[feng-build.md](../docs/feng-build.md) §2。
- 产物（src/cli/main.c 重组）：
  - 把现有 `feng lex|parse|semantic|check` 移入 `feng tool ...`。
  - 新增直接模式：`feng <files> --target=bin --out=<path> [--release]`，调用 codegen 写入临时 `.c`，再 fork `$CC`（默认 `cc`）链接 `libfeng_runtime.a`，产出可执行文件。
  - 保留 `--lib`、`--pkg` 解析与转发；`--pkg` 在 1A 不被消费但必须不报错（不接受任何 `.fb`，传入即明确诊断为 unsupported in Phase 1A）。
- 验收：`feng test/smoke/phase1a/hello.ff --target=bin --out=build/smoke/hello` 能产物可执行并打印期望输出；`feng tool semantic …` 行为与现有一致。

### T6 端到端测试与回归

- 产物：
  - `examples/phase1a/`：hello、object、array、control_flow、exception 五个最小示例。
  - `test/smoke/phase1a/`：每个示例对应一个 `.ff` + `.expected` 文本（程序 stdout 与 exit code），由 `tools/run_smoke.sh`（新增）驱动 `feng` 编译 + 运行 + 比对。
  - Makefile 增加 `runtime`、`smoke` 目标；`make test` 同时跑 `test_lexer`、`test_parser`、`test_semantic`、`test_runtime`、`smoke`。
- 验收：`make test` 在本机与 CI 均全绿；任何新增/修改文件都附对应回归用例。

## 3 任务依赖与执行顺序

```
T1 ──┐
     ├─→ T4 (codegen) ──→ T5 (CLI) ──→ T6 (smoke)
T2 ──┘                                     ▲
T3 (runtime + 单测) ───────────────────────┘
```

- T1/T2/T3 可并行启动；T4 需要三者完成方可进入。
- T5 仅依赖 T4 的稳定入口；smoke 依赖 T5。
- 任一任务进入 PR 评审都需附对应单测/集成测，未达验收口径不得合入。

## 4 风险与对策

- 风险 R1：`InferredExprType` 暴露后影响现有语义回归 → 对策：T2 仅追加字段、不改既有结构语义，并以 `test_semantic_inferred.c` 锁定行为。
- 风险 R2：发码 ABI 与运行时头不一致 → 对策：T3 与 T4 共用 `feng_runtime.h`，发码侧通过 `naming.c` 集中所有 mangling，禁止散落字符串。
- 风险 R3：异常路径下 ARC 漏 release → 对策：T4 C4 子任务统一以「作用域 cleanup record」实现，并由 T6 的 exception smoke 校验 retain/release 平衡（runtime 内置计数 hook，`FENG_RT_DEBUG=1` 时崩溃）。
- 风险 R4：`@cdecl` 调用 libc 在不同平台符号差异 → 对策：1A 仅承诺 `printf`、`puts`、`exit` 三个最小集合，`examples/phase1a` 只用 `printf`/`puts`。
- 风险 R5：宿主 `cc` 找不到 → 对策：T5 在缺失时给出明确诊断；CI 矩阵指定 `gcc` 与 `clang` 各跑一次。

## 5 关联文档

- [feng-plan.md](./feng-plan.md)
- [feng-runtime-codegen-draft.md](./feng-runtime-codegen-draft.md)
- [feng-phase1a-tests.md](./feng-phase1a-tests.md)
- [docs/feng-lifetime.md](../docs/feng-lifetime.md)
- [docs/feng-exception.md](../docs/feng-exception.md)
- [docs/feng-cli.md](../docs/feng-cli.md)
- [docs/feng-build.md](../docs/feng-build.md)
