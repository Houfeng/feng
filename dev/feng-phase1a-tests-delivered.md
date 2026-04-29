# Phase 1A smoke 与回归测试清单

> 本清单约定 Phase 1A 必须落地的测试集合，配合 [feng-phase1a-tasks.md](./feng-phase1a-tasks.md) §1.2 的硬门槛使用。任何 1A PR 合入前必须保证下列用例 100% 通过。

## 1 单元测试

### 1.1 既有回归（保持不动，必须全绿）

- `make test` 触发：`test_lexer` / `test_parser` / `test_semantic`。
- 准入条件：变更不得修改任何既有断言；如需新增语义元数据，新增独立测试文件。

### 1.2 语义元数据补充（T2）

文件：`test/semantic/test_semantic_inferred.c`

| 用例 | 验证点 |
| --- | --- |
| `inferred_int_literal` | `let x = 1;` 的绑定与表达式 `inferred_type` 均为 `i32` |
| `inferred_string_literal` | `let s = "hi";` 推导为 `string` |
| `inferred_array_literal` | `let a = [1, 2, 3];` 推导为 `i32[]`，元素类型可读取 |
| `inferred_call_return` | `let r = greet();` 取自 `fn greet(): string` 的返回类型 |
| `inferred_member_access` | `user.age` 推导为字段声明类型 `i32` |
| `inferred_binary_arith` | `a + b`（两侧 `i32`）推导为 `i32` |
| `inferred_if_expression` | `let v = if c { 1 } else { 2 };` 推导为 `i32` |

### 1.3 运行时单测（T3）

文件：`test/runtime/test_runtime.c`，使用与现有测试一致的极简断言宏。

| 用例 | 验证点 |
| --- | --- |
| `retain_release_balance` | 多次 retain/release 后 `refcount` 归零并触发 finalizer 一次 |
| `release_null_noop` | `feng_release(NULL)` 不崩溃 |
| `string_literal_persistent` | 重复 `feng_string_literal("ok", 2)` 返回同一指针，refcount 不变（持久） |
| `string_concat` | `"foo" + "bar"` 长度与字节序与期望一致，结果对象 release 后内存被回收（hook 计数） |
| `array_index_oob_panic` | `feng_array_check_index` 越界触发 `feng_panic`（用 `setjmp` 捕获 abort hook） |
| `exception_throw_catch` | 单层 try/throw/catch：异常对象在 catch 完成后被 release |
| `exception_unwind_release` | throw 跨 1 层局部块：被跨过的局部对象 refcount 正确归零 |
| `finalizer_single_path` | 自定义 finalizer 在 release-to-zero 时被恰好调用 1 次 |

### 1.4 codegen 单元（T4）

文件：`test/codegen/test_codegen.c`

| 用例 | 验证点 |
| --- | --- |
| `emit_empty_program` | 仅 `mod x;` 的程序输出可编译的 C，含 runtime header include |
| `emit_global_let` | 模块级 `let n: i32 = 1;` 输出含正确符号名与初始化 |
| `emit_top_fn` | `fn add(a: i32, b: i32): i32` 输出签名与返回 |
| `emit_type_with_finalizer` | type + finalizer 输出 descriptor 表与 finalizer 函数 |
| `reject_lib_target` | 1A 传入 `lib` 目标，返回错误信息含 "lib target not yet supported" |
| `reject_unsupported_features` | 含 `match` 表达式 / 方法重载 / `spec` 输入时返回明确诊断（不静默成功） |

## 2 端到端 smoke（T6）

目录：`examples/phase1a/` 与 `test/smoke/phase1a/`，由 `tools/run_smoke.sh` 驱动：编译 → 链接 → 运行 → 比对 stdout 与 exit code。

| 名称 | 覆盖语义 | 期望 stdout | 期望 exit |
| --- | --- | --- | --- |
| `hello` | 模块级 `let`、`@cdecl` 调用 `puts`/`printf`、`main` | `Hello, Phase 1A` | 0 |
| `object` | `type` + 字段 + 构造 + 方法、对象字面量、字段读写 | `User: Houfeng / 18` | 0 |
| `array` | 数组字面量、下标读、`for/in` 遍历、`feng_array_check_index` 路径 | `1 2 3` 一行 | 0 |
| `control_flow` | `if`/`else if`/`while`/三段式 `for`/`break`/`continue` | `pass` | 0 |
| `exception_caught` | `throw` → `catch` → `finally` 全路径，验证 finally 执行 | `caught:bad / done` | 0 |
| `exception_uncaught` | `main` 抛出未捕获异常 | stderr 含 `unhandled exception` | 非 0 |

约束：

- 每个 smoke `.ff` 不得引入 1A 范围外的语义；`spec`/`fit`/`match`/方法重载在 1A 全部禁止出现。
- `.expected.stdout` 与 `.expected.exit` 与源码同目录共置，CI 严格按字节比对。
- smoke 启用 `FENG_RT_DEBUG=1`，触发 retain/release 平衡检查；任意失败立即报错。

## 3 回归矩阵

`make test` 必须按下列顺序串行执行（前序失败即中断）：

```
1. test_lexer
2. test_parser
3. test_semantic
4. test_semantic_inferred
5. test_runtime
6. test_codegen
7. smoke (tools/run_smoke.sh)
```

CI 矩阵：

- `CC=gcc`、`CC=clang` 各跑一次。
- macOS（当前开发主机）+ Linux（CI 容器）各跑一次。
- `FENG_RT_DEBUG=0` 与 `=1` 各跑一次 smoke。

## 4 验收记录模板

每次 1A 子任务（T1–T6）合入 PR 时附下列段落，便于复盘与版本对账：

```
- 任务编号：T?
- 关联文档：feng-phase1a-tasks.md §?
- 新增/修改测试：…
- 本地全量回归：make test => OK / FAIL（附日志）
- 偏差与遗留：…
```
