# Phase 2 开工指导

> 本文档用于指导 Phase 2 的开工顺序、代码拆分方式与验收口径。
> `docs/` 下文档继续作为最终规范，不在本阶段承载“当前实现到哪一步”的临时状态；所有施工中的拆解、阶段性取舍、重构顺序只写在 `dev/`。

## 1. 目标

Phase 2 的目标不是继续停留在“生成 C 文件”，而是把当前实现推进到“本地多文件源码 -> 宿主 C 编译器 -> 可执行文件”的直编闭环。

本阶段同时承担一项 CLI 结构治理工作：

- 现有 `lex`、`parse`、`semantic`、`check` 不再占用顶层命令，统一收进 `tool` 子路由。
- `src/cli/main.c` 只保留入口、参数分发和退出码汇总，不再继续堆叠各命令的具体实现。
- 顶层直接编译能力与 `tool` 下各子命令都拆到独立文件中维护。

## 2. 范围

### 2.1 本阶段包含

- 顶层直接编译模式：接受本地 `.ff` 文件列表，以 `--out` 指定输出根目录，产出可执行文件与中间产物。
- 多文件单包本地编译：同一次调用可输入多个本地源码文件。
- 语义分析结果直接进入 codegen，并生成单个 C 翻译单元。
- CLI 结构重组：`tool` 子路由落地，`main.c` 瘦身，命令实现拆文件。
- 宿主 C 编译器调用：自动串起生成 C、链接 runtime、产出最终二进制。
- 现有 `extern fn` 注解带来的原生库链接信息继续可用。

### 2.2 本阶段实现子集

- `--target` 在 Phase 2 首批实现中仅支持 `bin`；`lib` 明确保留给后续阶段，不在本阶段落地。
- `--release` 在 Phase 2 首批实现中可先忽略，不作为阻塞项；如保留解析入口，应明确为“暂未生效”或“当前未实现”，避免误导为已有发布模式差异。
- `--out` 按 [docs/feng-cli.md](docs/feng-cli.md#L44) 的说明视为输出根目录，而不是单个最终文件路径。
- Phase 2 直编闭环至少需要落地以下目录语义：
  - `<out>/ir/c`：编译生成的 C 中间文件。
  - `<out>/bin`：最终可执行文件。
  - `<out>/lib`：运行时静态库等库产物所在目录，继续复用当前仓库构建布局。
- `gen`、`pkg` 等目录语义先与 CLI 文档保持一致的命名预留，但不要求在 Phase 2 首批实现中全部消费。

### 2.3 本阶段明确不做

- 不修改 `docs/` 下规范文档。
- 不实现 `feng build`、`feng run`、`feng check`、`feng clean`、`feng pack` 这类项目级工作流。
- 不引入 `feng.fm` 解析。
- 不实现 `.fi` 生成、`.fb` 打包、外部包索引与缓存。
- 不扩大 C ABI 兼容面。
- 不把“临时过渡行为”写进最终规范。
- 不在 Phase 2 首批实现中要求 `--target=lib`。
- 不在 Phase 2 首批实现中要求 `--release` 产生真实构建模式差异。

## 3. 现状约束

当前实现有三处直接阻塞 Phase 2：

- `src/cli/main.c` 既承担入口，也塞满了 `lex`、`parse`、`semantic`、`check`、`compile` 的全部实现，文件已经过长，不适合继续叠加 Phase 2 逻辑。
- `src/codegen/codegen.c` 当前仍显式拒绝多文件发码，无法满足单包多文件输入。
- 现有 smoke 路径仍依赖脚本手工调用宿主 `cc`，说明真正的“编译器直编闭环”尚未落到 CLI 内部。

因此，Phase 2 不能只给当前 `compile` 命令继续打补丁，而需要先做 CLI 分层，再打通多文件 codegen 与宿主编译驱动。

## 4. 目标结构

### 4.1 CLI 表面

Phase 2 完成后，CLI 表面应满足以下结构：

- 顶层直接模式：`feng <files...> --target=bin --out=<dir>`
- `tool` 子路由：`feng tool lex ...`、`feng tool parse ...`、`feng tool semantic ...`、`feng tool check ...`

说明：

- 顶层直接模式服务于编译器直接调用，是 Phase 2 的主路径。
- 当前阶段只要求 `bin` 目标；`lib` 继续留给后续阶段。
- 当前阶段先不把 `--release` 作为落地阻塞项。
- `tool` 子路由仅承载调试/诊断型命令，不再与未来项目级顶层命令抢占命名空间。

### 4.1.1 输出目录结构

Phase 2 指导实现需要与 [docs/feng-cli.md](docs/feng-cli.md#L44) 中已增加的 `--out` 说明保持一致。

建议当前阶段按以下方式落地：

- `--out` 接受一个输出根目录；未显式传入时，后续实现可考虑回退到 `./build`，但这不是本指导文件要求先实现的重点。
- 生成的 C 中间文件放到 `<out>/ir/c`。
- 最终可执行文件放到 `<out>/bin`。
- 运行时库继续沿用当前仓库已有的 `build/lib` 产物布局；若后续要完全并入 `<out>/lib`，应作为单独小步调整，不与首批直编闭环耦合。

首批实现不要求一次性把 `gen`、`pkg` 等目录全部打通，但命名和布局不应与 CLI 文档冲突。

### 4.2 代码组织

推荐把 CLI 拆成“入口 / 公共能力 / 顶层命令 / tool 子命令”四层：

```text
src/cli/
  main.c                 # 只保留入口与路由
  cli.h                  # CLI 公共声明
  common.c               # 公共参数/诊断/文件读取/编译管线辅助
  common.h
  compile/
    direct.c             # 顶层直接编译模式
    driver.c             # 宿主 C 编译器调用、中间文件、链接驱动
    options.c            # 顶层直接模式参数解析
    options.h
  tool/
    tool.c               # `feng tool ...` 子路由
    lex.c
    parse.c
    semantic.c
    check.c
```

如果采用子目录形式，需同步调整 `Makefile` 的 `src/cli/*.c` 收集方式，确保 `src/cli/**/*.c` 会被编译进 CLI。

### 4.3 main.c 的职责边界

`src/cli/main.c` 在 Phase 2 之后只应负责：

- 初始化进程级上下文。
- 分辨是顶层直接模式还是显式子命令。
- 路由到 `tool` 或直接编译入口。
- 统一返回退出码。

以下内容不再留在 `main.c`：

- 词法/语法/语义/检查的具体实现。
- 多文件加载与语义管线细节。
- 生成 C 文件与调用宿主编译器的细节。
- 各子命令的长段参数解析。

## 5. 工作拆解

### P0 先做结构重组，不先堆功能

目标：先把 CLI 的结构拆开，为 Phase 2 的功能实现腾出稳定落点。

步骤：

1. 新建 `src/cli/common.*`，承接当前 `main.c` 里通用的文件读取、诊断打印、`LoadedSource` 生命周期管理等基础能力。
2. 新建 `src/cli/tool/`，把现有 `run_lex_command`、`run_parse_command`、`run_semantic_command`、`run_check_command` 分别迁出。
3. 新建 `src/cli/compile/`，把当前 `run_compile_command` 的职责拆成“参数解析”“前端管线”“宿主编译驱动”三层。
4. 把 `main.c` 改成纯路由入口，不再直接包含各命令实现。

验收：

- `main.c` 只剩入口与分发逻辑。
- `tool` 子命令可独立维护。
- 后续顶层直接模式新增逻辑不需要继续向 `main.c` 回填实现。

### P1 收敛命令表面

目标：把现有调试命令迁入 `tool`，为顶层直接模式腾出正式入口。

步骤：

1. 把现有 `lex`、`parse`、`semantic`、`check` 顶层入口迁为 `feng tool ...`。
2. 顶层不再保留这些命令名。
3. 顶层保留“直接模式”和后续阶段预留命令位，不在本阶段引入项目级命令实现。

验收：

- `feng tool lex <file>`、`feng tool parse <file>`、`feng tool semantic ...`、`feng tool check ...` 可工作。
- 旧顶层 `feng lex|parse|semantic|check` 不再作为正式表面存在。
- CLI 帮助输出与实际路由一致。

### P2 抽公共编译前端管线

目标：为顶层直接模式与 `tool semantic/check` 复用同一套“读文件 -> parse -> semantic -> 诊断”基础管线。

步骤：

1. 复用现有多文件加载逻辑，抽成公共入口。
2. 让 `tool semantic` 与 `tool check` 走共享前端管线，而不是各自复制加载和清理逻辑。
3. 顶层直接模式也调用同一套前端管线，避免再走单文件特例。

验收：

- 前端管线支持多个输入文件。
- 诊断上下文仍能正确按源文件输出。
- 顶层直接模式不再依赖单文件版本的 compile 实现。

### P3 打通多文件 codegen

目标：移除单文件发码限制，让完整 `FengSemanticAnalysis` 能落成一个 C 翻译单元。

步骤：

1. 修改 `codegen` 入口，使其面向完整 analysis 工作，而不是只取一个 program。
2. 定义稳定的发码顺序：模块级绑定、顶层函数、类型、方法、字符串字面量表、`main` 包装。
3. 维持现有语义规则：`bin` 目标下仍要求跨全部输入程序只有一个 `main(args: string[])`。

验收：

- 同一次编译可处理两个及以上本地 `.ff` 文件。
- 生成的 C 翻译单元可以通过宿主 C 编译器。
- 重复或缺失 `main` 的诊断语义不回退。

### P4 新建顶层直接编译驱动

目标：让顶层直接模式成为真正的编译器主路径。

步骤：

1. 在 `src/cli/compile/options.*` 中独立实现顶层参数解析。
2. 在 `src/cli/compile/direct.c` 中组织“前端管线 -> codegen -> 宿主编译驱动”的主流程。
3. 不再使用旧的 `compile` 子命令作为主入口。
4. 在首批实现中，`--target` 只接受 `bin`；若传入 `lib`，直接给出明确 unsupported 诊断。
5. `--release` 先不作为首批落地项；如暂时保留参数入口，需在帮助和诊断中明确“当前未实现”。

验收：

- `feng <files...> --target=bin --out=<dir>` 可进入完整编译流程。
- 产物布局与 `--out` 目录语义一致，至少能稳定产出 `<out>/ir/c` 与 `<out>/bin`。
- `--target=lib` 会被清晰拒绝，而不是静默忽略。
- 错误参数会在参数解析阶段直接给出明确诊断。

### P5 宿主 C 编译器驱动

目标：把“生成 C 文件后再手工 cc”的脚本流程收进 CLI 内部。

步骤：

1. 在 `src/cli/compile/driver.c` 中实现中间 C 文件写出，并按 `--out` 目录结构落到 `<out>/ir/c`。
2. 定位 runtime 头文件与静态库路径。
3. 调用宿主 `CC`；若未设置，则回退到 `cc`。
4. 汇总现有 `extern fn` 注解所需的原生库链接参数。
5. 最终可执行文件写到 `<out>/bin`。
6. 成功时清理临时工件，失败时保留关键中间 C 文件并输出路径。

验收：

- CLI 内部可以独立完成“生成 C -> 调宿主编译器 -> 产出可执行文件”。
- 不再要求 smoke 脚本手工执行第二次 `cc`。
- `--out` 目录下的关键产物位置稳定可预期。
- 失败时能定位是 codegen 失败、runtime 缺失还是宿主编译失败。

### P6 构建脚本与 smoke 回归

目标：把仓库里的验证路径切到新的直编模式。

步骤：

1. 调整 `Makefile`，使 CLI 源文件拆分后仍能正确收集。
2. 保持 runtime 静态库仍由 Makefile 构建。
3. 修改 `scripts/run_smoke.sh`，从“手工 cc”迁移为“直接调用顶层编译模式”。
4. 补充多文件相关 smoke 与负例。

验收：

- `make test` 可继续作为仓库主回归入口。
- smoke 不再依赖脚本层重复实现编译链。
- 至少覆盖 hello、对象、数组、控制流、异常，以及多文件编译场景。

## 6. 建议落地顺序

推荐顺序如下：

1. P0 CLI 结构重组
2. P1 命令表面收敛
3. P2 公共前端管线
4. P3 多文件 codegen
5. P4 顶层直接编译驱动
6. P5 宿主 C 编译器驱动
7. P6 Makefile 与 smoke 回归

原因：

- 先拆结构，再做功能，可以避免一边改行为一边继续把逻辑塞进 `main.c`。
- 先收敛 `tool` 子路由，可以尽早稳定 CLI 表面，减少后续返工。
- 多文件 codegen 和宿主编译驱动是直编闭环的两个核心功能点，必须在公共前端管线稳定后再接入。

## 7. 建议新增文件

以下文件是本阶段推荐直接建立的最小集合：

- `src/cli/cli.h`
- `src/cli/common.h`
- `src/cli/common.c`
- `src/cli/tool/tool.c`
- `src/cli/tool/lex.c`
- `src/cli/tool/parse.c`
- `src/cli/tool/semantic.c`
- `src/cli/tool/check.c`
- `src/cli/compile/options.h`
- `src/cli/compile/options.c`
- `src/cli/compile/direct.c`
- `src/cli/compile/driver.c`

如实现中发现 `tool` 和顶层直接模式之间仍有较多共用逻辑，可再补：

- `src/cli/frontend.h`
- `src/cli/frontend.c`

用于承接 parse/semantic/codegen 前的公共编译上下文。

## 8. 验收口径

Phase 2 结束前，至少要满足以下口径：

- 顶层直接模式可以输入多个本地 `.ff` 文件并产出一个可执行文件。
- `lex`、`parse`、`semantic`、`check` 已全部迁入 `tool` 子路由。
- `main.c` 不再保存大段命令实现。
- CLI 代码已经按命令拆到单独文件中维护。
- 宿主 C 编译器调用在 CLI 内部完成。
- 仓库回归脚本不再手工重建同一段编译逻辑。

## 9. 风险与约束

- 若 CLI 改成子目录拆分，Makefile 必须同步更新，否则新文件不会进入构建。
- 若多文件 codegen 只做表层拼接，而没有定义稳定的声明/初始化顺序，后续会引入隐蔽链接或初始化缺陷。
- 若 `tool` 子路由迁移不彻底，顶层命令表面会继续污染未来的 `build/run/check` 命名空间。
- 若继续保留大段实现留在 `main.c`，本阶段的结构治理目标将直接失败。
- 若 `--out` 的目录语义与 CLI 文档不一致，后续项目级工作流会在产物布局上再次返工。
- 若过早把 `--release` 做成半成品但没有真实差异，用户会误判发布模式已成立，因此首批实现宁可明确未实现，也不要给出伪支持。

## 10. 交付建议

为降低风险，建议按以下批次提交：

1. 第一批：CLI 拆文件 + `tool` 子路由落地，不改变 codegen 行为。
2. 第二批：公共前端管线 + 多文件 codegen。
3. 第三批：顶层直接模式 + 宿主 C 编译驱动。
4. 第四批：Makefile、smoke、全量回归补齐。

这样可以把“结构重组”和“功能打通”分开评审，减少一次性改动过大带来的风险。
