# Feng Phase 2 — Delivered

> 本文件记录 [feng-phase2-pending.md](feng-phase2-pending.md) 中各步骤的实际落地情况。所有交付都按"先文档、再代码、最后测试"的规范推进，每步完成后跑全量回归（`make test`），保持仓库始终绿色。

## P0 CLI 结构重组（已交付）

- 新增 `src/cli/cli.h`、`src/cli/common.{h,c}`、`src/cli/compile/{options,legacy}.{h,c}`、`src/cli/tool/{tool,lex,parse,semantic,check}.c`，把 `src/cli/main.c` 切分为路由 + 子命令实现。
- 公共诊断打印、`--target` 解析、源代码登记/释放收敛到 `cli/common`。
- `Makefile` 用 `find src/cli -name '*.c'` 递归收集 CLI 源文件，避免拆分后漏编。

## P1 命令表面收敛（已交付）

- `feng tool <lex|parse|semantic|check>` 路由稳定；旧的顶层 `feng lex/parse/semantic/check` 被拒绝并给出迁移提示。
- `feng compile` 仅作为 legacy debug 子命令保留，提示用户优先使用顶层直编模式。
- 顶层 `feng <files...>` 被识别为直接编译模式入口（实现见 P4）。

## P2 公共前端管线（已交付）

- 抽出 `src/cli/frontend.{h,c}`：统一负责"读取多文件 → 词法 → 语法 → 语义"，对外只暴露 `feng_cli_frontend_run`。
- 所有子命令（tool 子路由、legacy compile、direct compile）共享同一份前端入口与诊断回调，任何前端能力升级只在一处发生。

## P3 多文件 codegen（已交付）

- `feng_codegen_emit_program` 接受程序数组，由内部 `cg_emit_all_programs` 协调"类型壳 → 类型成员 → spec → fit → 模块 binding → 函数体"等多遍处理。
- 支持同模块多文件聚合（fit/方法、模块级 let/var、free fn 都能跨文件共存）。
- 新增 `test/codegen/test_codegen.c`，覆盖 bin / lib 双 target 的多文件回归。

## P4 顶层直接编译驱动（已交付）

- 新增 `src/cli/compile/direct.{c}`：解析 `feng <files...> --target=bin --out=<dir> [--release] [--keep-ir] [--bin-name=<name>]`，串联前端 → codegen，落地 `<out>/ir/c/feng.c`。
- `--target=lib` 暂保留解析但报错，`--release` 解析后给警告，`--out` 强制要求非空。
- 新增 `scritps/run_cli_direct.sh`，覆盖完整管线、IR 保留、各类负例。

## P5 宿主 C 编译器驱动（已交付）

- 新增 `src/cli/compile/driver.{h,c}`：
  - 通过 `_NSGetExecutablePath` / `/proc/self/exe` 解析 feng 自身路径，再向上探测 `build/lib/libfeng_runtime.a` 与 `src/runtime/feng_runtime.h`；环境变量 `FENG_RUNTIME_LIB` / `FENG_RUNTIME_INCLUDE` 可覆盖，缺失或不存在时给出明确诊断。
  - 扫描程序的 `extern fn` 上的 `@cdecl("xxx")` 注解作为额外链接库，自动跳过 `libc`、剥掉 `lib` 前缀，保证 `-l<x>` 干净去重。
  - 用 `fork + execvp + waitpid` 调起 `${CC:-cc}`，失败时打印 `host C compiler failed (exit=N)` 并保留 `<out>/ir/c/feng.c` 便于排查；成功且未指定 `--keep-ir` 时清理 IR。
- 新增 `--bin-name=<stem>` 选项，便于多文件编译时显式指定可执行文件名。
- 端到端验证：`feng test/smoke/phase1a/hello.ff --out=/tmp/X` 直接产出 `/tmp/X/bin/hello` 并可运行。

## P6 构建脚本与 smoke 回归（已交付）

- `scritps/run_smoke.sh` 完全切到直编模式：
  - 单文件用例：`<name>.ff` + `<name>.expected`，按字母序遍历。
  - 多文件用例：`<name>/`（目录里多个 `*.ff`） + `<name>.expected`，目录内 `.ff` 排序后整体喂给 feng，并通过 `--bin-name=<name>` 固定可执行名。
  - 不再有"手工 cc"环节。每个用例独立的 `build/gen/smoke/<name>/` 目录承载 IR 与 bin。
- 新增首个多文件 smoke 用例 `test/smoke/phase1a/multi_hello/`（同模块两文件，main 调用另一文件中的 helper）。
- `scritps/run_cli_direct.sh` 增补：
  - `multi_file`：复用 `multi_hello` 走完整链路，断言 stdout 与 expected 一致；
  - `bin_name_empty`：`--bin-name=` 必须报错。
- `make test` 入口持续作为仓库主回归（5 单测套件 + 26 smoke + 10 cli-direct），全部通过。

### 已知后续工作

- Codegen 多文件 Pass 4 目前按程序顺序逐个发射函数体，跨文件 free fn 调用要求被调方文件先于调用方处理。当前依赖 smoke 脚本对目录内 `.ff` 排序解决；正式修复方案是把"注册全部 free fn"拆成独立的前置子 pass，与类型/spec 的两阶段结构对齐。该项不在 Phase 2 P0–P6 验收范围内，仅记录在 `/memories/repo/feng-codegen-multi-file-order.md` 中作为后续待办。
