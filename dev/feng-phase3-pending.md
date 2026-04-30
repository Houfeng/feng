# Phase 3 开工指导

> 本文档用于指导 Phase 3 的开工顺序、CLI 内部拆分方式与验收口径。
> `docs/` 下文档继续作为最终规范，不在本阶段承载“当前实现到哪一步”的临时状态；所有施工中的拆解、阶段性取舍、重构顺序只写在 `dev/`。
> 本文件名为 `feng-phase3-pending.md`，正文内容对应 Phase 3。

## 1. 目标

Phase 3 的目标不再是继续强化“直接输入源码文件”的编译器路径，而是把当前直编能力提升为“基于 `feng.fm` 的本地项目工作流”，并进一步打通 `.fb` 打包闭环。

本阶段有三条主线：

- 项目相关逻辑继续归属于 CLI，自身不拆到 `src/cli/` 之外；如需拆分，也只在 `src/cli/` 内部组织公共能力与项目级命令。
- `build` 的核心职责是：读取 `feng.fm`，收集源码，结合项目配置组装出完整编译参数，然后把这些参数交给 `feng` 编译路径；`build` 不重新实现前端、codegen 或宿主 C 编译逻辑。
- `pack` 的核心职责是：基于 `feng.fm` 和编译产物，产出 `.fb` 包；当前阶段先聚焦“本地项目可以打出 `.fb`”，不处理外部包消费。

## 2. 范围

### 2.1 本阶段包含

- `feng.fm` 解析。
- 本地项目 `build`、`check`、`run`、`clean`、`pack` 命令。
- 项目级公共能力下沉到 `src/cli/` 内部，供 `build` 等命令复用。
- 基于 `feng.fm` 的源码收集、输出路径计算和编译参数组装。
- `target = lib` 项目的本地打包闭环。
- `.fb` 包格式选型与打包实现；选型要求是能够保留文件系统信息（例如可执行位）且压缩/解压效率可接受。
- `.fi` 生成仍属于 Phase 3，但作为本阶段最后一项功能任务处理；当前先预留章节与落点，后续再细化。

### 2.2 本阶段实现子集

- 第三阶段先不处理外部包，因此不实现 `deps add/remove/install`，不做本地缓存，不消费外部 `.fb`。
- `build` 只处理当前项目 `feng.fm`，不展开外部依赖图。
- 当前阶段讨论的“项目 `feng.fm`”与分发包内的 `feng.fm` 不是同一职责：项目级命令读取开发态 `feng.fm` 以组装构建参数；分发包内的 `feng.fm` 留给 `feng build` / `deps` / `pack` 等上层工具表达和校验包元信息，编译器本身不读取 `feng.fm`。
- `check` 基于 `feng.fm` 驱动当前项目源码集合，不产出最终制品。
- `run` 仅针对 `target = bin` 项目；若项目目标不是 `bin`，应给出明确诊断。
- `pack` 仅针对 `target = lib` 项目；若项目目标不是 `lib`，应给出明确诊断。
- 当前阶段继续复用现有 CLI 直编主链路；项目级命令负责“组装参数并调用”，不复制一套新的编译实现。

### 2.3 本阶段明确不做

- 不实现外部包解析、版本选择、依赖下载与缓存。
- 不实现 `deps` 子命令的真实行为。
- 不在 `src/cli/` 之外新增独立项目模块。
- 不扩大 C ABI 兼容面。
- 不在本文件中展开 `.fi` 生成细节；该部分单独留空，待后续讨论。
- 不把尚未定稿的打包格式细节直接写回 `docs/`；若最终格式选择与现有规范不一致，应先更新主规范再落代码。

## 3. 现状约束

当前仓库已经完成 Phase 2 的直编闭环，但对 Phase 3 仍有四个直接缺口：

- 现有 CLI 已具备顶层直编和 `tool` 子路由，但还没有 `feng.fm` 驱动的项目级 `build/check/run/clean/pack` 实现。
- 当前编译主链路以“传入源码文件列表”为入口，尚未提供“读取项目配置 -> 收集源码 -> 组装编译参数”的项目层。
- 当前顶层直编仍明确拒绝 `--target=lib`，而 `pack` 要求 `target = lib` 的编译产物能够稳定落地。
- 当前 `.fb` 打包格式在主规范中已有既有描述；而本阶段又新增了“保留文件系统信息并兼顾效率”的约束，因此打包选型必须先在 `dev/` 中收敛，再决定是否回写主规范。

因此，Phase 3 不能只是在现有 `main.c` 上继续堆 `build` 和 `pack`，而需要先在 `src/cli/` 内部补项目级公共能力，再让各项目命令共享同一套项目上下文。

## 4. 目标结构

### 4.1 CLI 表面

Phase 3 完成后，CLI 表面至少应满足以下结构：

- `feng build [<path>] [--release]`
- `feng check [<path>] [--format <text|json>]`
- `feng run [<path>] [--release] [-- <program-args>...]`
- `feng clean [<path>]`
- `feng pack [<path>]`
- `feng <files...> --target=bin --out=<dir>` 继续保留为直编模式。
- `feng tool ...` 继续保留为调试/诊断子路由。

说明：

- 项目级命令服务于本地项目工作流，是 Phase 3 的主路径。
- 直编模式继续保留，作为底层编译器入口，不被项目级命令替代。
- `deps` 属于 Phase 4 范围，当前阶段不实现。

### 4.2 代码组织

项目相关逻辑仍然归属于 `src/cli/`，建议按“入口 / 公共能力 / 编译器底层 / 项目级命令 / tool 子命令”组织：

```text
src/cli/
  main.c
  cli.h
  common.c
  common.h
  frontend.c
  frontend.h
  compile/
    direct.c
    driver.c
    options.c
    options.h
  project/
    common.c            # 项目路径、输出布局、公共诊断
    common.h
    manifest.c          # feng.fm 解析
    manifest.h
    build.c
    check.c
    run.c
    clean.c
    pack.c
    archive.c           # .fb 打包与解包格式封装
    archive.h
  tool/
    tool.c
    lex.c
    parse.c
    semantic.c
    check.c
```

说明：

- `project/` 只是 `src/cli/` 内部的子目录，不是独立项目模块。
- `project/common.*` 与 `project/manifest.*` 负责供 `build/check/run/clean/pack` 复用，不把项目逻辑散落到每个命令文件里重复实现。
- `.fb` 打包相关逻辑单独收敛在 `project/archive.*`，避免把压缩格式、元信息保留和目录遍历细节塞进 `pack.c`。

### 4.3 `main.c` 的职责边界

`src/cli/main.c` 在 Phase 3 之后仍只负责：

- 初始化进程级上下文。
- 区分顶层直编、项目级命令和 `tool` 子路由。
- 路由到 `build/check/run/clean/pack` 或底层编译入口。
- 统一返回退出码。

以下内容不再留在 `main.c`：

- `feng.fm` 解析细节。
- 项目路径解析、源码收集和输出路径布局。
- `.fb` 打包与压缩格式处理。
- 各项目命令的具体参数解析与行为实现。

## 5. 工作拆解

### P0 先补 CLI 内部项目层公共能力

目标：在 `src/cli/` 内部建立项目级命令的公共落点，但不把项目逻辑拆出 CLI。

步骤：

1. 新建 `src/cli/project/`，承接项目级命令与共享辅助能力。
2. 抽出项目路径解析、输出目录计算、公共错误打印等共享逻辑到 `project/common.*`。
3. 在 `main.c` 中增加 `build/check/run/clean/pack` 路由，但具体实现留在各自文件中。

验收：

- 项目级命令的路由不再回填到 `main.c`。
- `src/cli/` 内已有稳定的项目级共享落点，可供后续 `build/check/run/clean/pack` 共用。

### P1 实现 `feng.fm` 解析与项目上下文

目标：让项目级命令可以稳定地从 `feng.fm` 构建统一的项目上下文。

步骤：

1. 在 `project/manifest.*` 中实现 `feng.fm` 解析，至少覆盖 `name`、`version`、`target`、`src`、`out`。
2. 实现 `<path>` 的统一解析规则：省略时使用当前目录；传目录时解析目录下 `feng.fm`；传文件时直接读取该 `feng.fm`。
3. 基于 `src` 配置收集本地 `.ff` 文件，并给出稳定顺序。
4. 基于 `out` 和 `target` 计算项目产物布局，供 `build/run/clean/pack` 共用。

验收：

- 项目级命令都能共享同一份项目上下文，而不是各自重复读 `feng.fm`。
- 对 `feng.fm` 缺失、字段非法、源码目录不存在等情况，都能给出明确诊断。

### P2 落地 `build/check/run/clean`

目标：先把本地项目工作流立住，再推进打包。

步骤：

1. `build`：读取项目上下文，收集源码，结合 `target/out/release` 组装完整编译参数，调用现有 `feng` 编译主链路。
2. `check`：读取项目上下文，复用前端与诊断输出，仅做检查，不产出最终制品。
3. `run`：在 `build` 成功后执行项目产物，并把 `--` 之后的参数透传给目标程序。
4. `clean`：按项目上下文删除对应的输出根目录，不触碰仓库中不属于该项目的其他产物。

验收：

- `build` 的职责边界收敛为“组装参数并调用编译器”，不复制编译实现。
- `check`、`run`、`clean` 都复用同一份项目上下文。
- `target = bin` 项目可以通过 `build` 和 `run` 完成本地闭环。

### P3 补齐 `target = lib` 与 `pack`

目标：让 `target = lib` 项目可以产出本地包，并完成 `.fb` 打包。

步骤：

1. 先补齐底层编译路径对 `target = lib` 的支持，使项目级 `build` 可以稳定产出库项目所需制品。
2. 在 `pack` 中读取项目上下文，校验 `target = lib`，并复用 `build` 或其底层产物准备流程。
3. 建立 `.fb` 打包 staging 目录，明确 `feng.fm`、`mod/`、`lib/` 等内容的来源与落点；其中包内 `feng.fm` 仅视为分发元信息，供 `feng build` / `deps` / `pack` 等上层工具读取，不作为编译器消费 `.fb` 的输入；编译器只消费 `.fi` 与实际库文件布局。
4. 在 `project/archive.*` 中封装 `.fb` 压缩打包逻辑；打包格式选型必须满足“保留文件系统信息”和“效率可接受”两条约束。

验收：

- `target = lib` 项目可通过 `feng pack` 产出 `.fb`。
- 打包逻辑不散落在 `pack.c` 中，压缩实现单独收敛。
- `.fb` 选型及实现能够覆盖文件权限等元信息保留需求。

### P4 项目级 smoke 与主回归（不含 `.fi`）

目标：把仓库回归入口切到新的项目工作流，并覆盖不依赖 `.fi` 的 `.fb` 打包主路径。

步骤：

1. 新增项目级 smoke，用 `feng.fm` 驱动 `build/check/run/clean/pack`。
2. 为 `target = bin` 和 `target = lib` 分别准备最小项目样例。
3. 增加 `.fb` 包结构检查与元信息保留检查。
4. 继续保持 `make test` 为主回归入口。

验收：

- 项目级命令已有独立 smoke 覆盖。
- `.fb` 包至少覆盖结构正确性与打包/解包基本行为。
- 现有直编路径不回退。

### P5 `.fi` 生成与最终回归

> `.fi` 生成仍属于 Phase 3，但作为本阶段最后一项功能任务处理。
> 本节暂留空，待后续细化讨论后补充。

## 6. 建议落地顺序

推荐顺序如下：

1. P0 CLI 内部项目层公共能力
2. P1 `feng.fm` 解析与项目上下文
3. P2 `build/check/run/clean`
4. P3 `target = lib` 与 `pack`
5. P4 项目级 smoke 与主回归（不含 `.fi`）
6. P5 `.fi` 生成与最终回归

原因：

- 先把项目级公共能力立住，后续命令才不会各自复制 `feng.fm` 解析和源码收集逻辑。
- `build/check/run/clean` 先落地，能尽早稳定本地项目工作流。
- `pack` 依赖 `target = lib` 的底层编译支持，因此应在 `build` 稳定后推进。
- 先把不依赖 `.fi` 的项目工作流、`.fb` 打包与回归入口稳定住，再在阶段末处理 `.fi` 生成，能降低包接口语义返工风险。

## 7. 验收口径

Phase 3 结束前，至少要满足以下口径：

- `feng build` 能基于 `feng.fm` 收集源码并完成编译。
- `feng check`、`feng run`、`feng clean` 已接入项目上下文。
- `feng pack` 能基于 `target = lib` 项目产出 `.fb`。
- 项目相关逻辑仍然留在 `src/cli/` 内部组织，没有拆出独立项目模块。
- `build` 没有复制前端、codegen 和宿主 C 编译细节，而是组装参数后调用底层编译路径。
- `.fb` 的打包实现已经覆盖文件系统元信息保留与效率要求。
- `.fi` 生成仍属于 Phase 3，并作为该阶段最后一项功能任务落地；落地后需完成最终回归。

## 8. 风险与约束

- 若项目级公共能力没有先抽出来，`build/check/run/clean/pack` 很容易各自复制 `feng.fm` 解析和路径处理逻辑，后续维护成本会迅速上升。
- 若 `build` 直接复制现有直编流程，而不是组装参数调用底层编译路径，CLI 会同时维护两套编译实现，后续行为很容易分叉。
- 若 `target = lib` 的底层编译支持不先补齐，`pack` 只能停留在壳命令层面，无法形成真实闭环。
- 若 `.fb` 压缩格式的约束先落代码、后改规范，会与 `docs/` 下的主规范形成冲突。
- 若 `.fi` 生成细节在未讨论清楚前硬塞进实现，会把包接口语义和项目工作流耦在一起，增加返工风险。

## 9. 交付建议

为降低风险，建议按以下批次提交：

1. 第一批：`src/cli/project/` 骨架 + `feng.fm` 解析 + `build/check/run/clean`。
2. 第二批：`target = lib` 底层支持 + `pack` + `.fb` 打包格式落地。
3. 第三批：项目级 smoke + `.fb` 打包回归补齐（不含 `.fi`）。
4. 第四批：`.fi` 生成细化 + 最终全量回归。

这样可以把“项目工作流落地”和“包接口/打包细节”分开评审，减少一次性改动过大的风险。
