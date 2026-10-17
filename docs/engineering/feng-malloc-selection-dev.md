# Feng 可选内建分配器开发方案

> 状态：待人工 Review，尚未实现。日期：2026-10-17。
>
> 本文设计 `--malloc=system|builtin`：将 mimalloc 按目标平台构建为独立静态库，随 Feng
> 发行包提供，仅在最终可执行程序选择 `builtin` 时链接并覆盖分配函数。
> 当前证据来自 Linux GNU 环境的动态覆盖实验；静态链接及其他目标仍须验证。

## 1. 目标与边界

为应用提供构建时可选的通用分配器，不改变 Feng 语义、对象布局、ARC、循环回收算法、
异常清理或 std API，不增加按对象类型、分配大小或评测用例选择分配器的特殊路径。

建议默认使用 `system`，显式指定 `builtin` 时使用随发行包提供的 mimalloc。
选择在最终链接时完成，不在每次分配时增加分支、函数表或 Feng 包装层。
本方案只影响生成的应用，不改变 Feng 编译器进程自身的分配器。

**run、build、pack 和直编均接受 `--malloc`；显式指定时，target 必须为 bin，否则报错。**
静态库没有最终进程的分配器决定权。
Feng 库、std 和 runtime 保持既有分配函数引用，最终程序统一解析这些引用。
同一份 `.a` / `.fb` 应能分别用于 `system` 和 `builtin` 程序，无需重编库。
不将 mimalloc 合并进 runtime 或其他库，不在 `.ft` / `.fb` 中增加分配器要求。

本文是独立开发方案。实施时，CLI、构建及发行规则分别收敛到
[CLI 规范](../specifications/feng-cli.md)、[构建规范](../specifications/feng-build.md)
和[发行与安装](feng-release-and-install.md)，本文保留设计依据与验证记录。

## 2. 当前实现与实验证据

### 2.1 已有基础

- 仓库已有 `third_party/mimalloc/`，`MI_MALLOC_VERSION=212`，即 2.1.2。
  [`src/static.c`](../../third_party/mimalloc/src/static.c) 将实现编为单个对象，
  [`src/alloc-override.c`](../../third_party/mimalloc/src/alloc-override.c) 提供标准分配函数覆盖。
- 现有第三方 Makefile 使用 `MI_MALLOC_OVERRIDE`，但仅在源码目录生成一份本机库，
  没有 Feng 的目标平台分发规则；不能直接把这一份库复制到所有目标目录。
- runtime 对象创建使用 `calloc`，其他路径还使用 `malloc`、`realloc`、`free`。
  仅替换 `malloc/free` 不完整，也可能造成分配与释放不匹配。
- [`src/cli/compile/driver.c`](../../src/cli/compile/driver.c) 已在 bin 分支选择目标 runtime
  并执行最终链接。allocator 库的选择应加入这一位置，不进入 Codegen。
- `feng pack` 会先构建当前项目再打包；项目命令通过内部选项调用直编，递归依赖另行构建。
  各入口均需解析、传递 `--malloc`，但不应照搬 `--lto` 向依赖库传递的规则。

### 2.2 Linux ARM64 隔离实验

基线提交为 `22e4efd6157e179060ff84dffdaabe4a5e7267e7`。使用同一批 Release / ThinLTO
Feng 二进制，由同一个 ELF 加载器启动；实验组仅预加载仓库版本的 mimalloc。
绑定诊断确认 `malloc/calloc/realloc/free` 均进入 mimalloc，正式计时不启用诊断输出。
未修改编译器、runtime、std 或基准算法。

下表为整程序耗时中位数，单位 ms；RSS 为每次进程峰值的中位数，单位 MiB。

| 用例 | system | mimalloc | 耗时变化 | system RSS | mimalloc RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| binary-trees | 319.512 | 156.498 | −51.02% | 17.27 | 18.06 |
| nbody | 641.695 | 645.063 | +0.52% | 1.80 | 1.99 |
| text-scan | 92.865 | 107.506 | +15.77% | 245.48 | 250.11 |
| fannkuch-redux | 1646.760 | 1651.182 | +0.27% | 1.56 | 2.06 |
| spectral-norm | 101.982 | 102.187 | +0.20% | 1.80 | 2.11 |

binary-trees 深度为 16，预热 2 轮、正式 24 轮，24 对样本全部改善；同批 Swift 为
400.478 ms / 27.93 MiB。其他用例使用既有 standard 输入，先测 12 轮，nbody 和
text-scan 再独立复测 24 轮；表中这两项采用复测结果。text-scan 两批所有配对均变慢，
nbody 复测同轮配对中位数增加约 0.40%；另两项未确认稳定耗时差异。
两批共 264 次正式采样均通过既有输出校验，制品哈希在测量前后保持一致。

临时原始记录位于 `temp/mimalloc-20261017/`，包括 `report.md`、采样数据及绑定证据；
该目录可被清理，本文保留关键结论。当前证据支持提供显式选项，**不支持默认替换、
承诺所有用例无退化，或把动态覆盖结果当作静态链接的验收结果**。

## 3. CLI 与选项传递

run、build、pack 和直编统一支持该选项，并按实际 target 校验，此规则已由人工确认。
选项解析与目标合法性校验分开，不将 pack 中的 `--malloc` 视为未知选项。
默认值及其他建议仍待本方案 Review 确认。

| 场景 | 未指定 | `--malloc=system` | `--malloc=builtin` |
| --- | --- | --- | --- |
| 实际 `target=bin` | system | 不引入内建分配器 | 链接目标 mimalloc 库 |
| 实际 target 不是 bin | 既有行为 | 报错：仅适用于 bin | 报错：仅适用于 bin |

上述规则适用于四个入口。各命令原有的 target 限制保持不变：当前 pack 只接受 lib 项目，
因此显式指定 `--malloc` 时应报告目标不适用；本方案不扩展 pack 的打包目标类型。

示例：

```sh
feng main.ff --target=bin --release --malloc=builtin
feng build --release --malloc=builtin
feng run --malloc=system -- <program-args>
```

- Debug 与 Release 均可显式选择分配器；分配器默认值不随模式或 LTO 设置变化。
- 仅增加带值形式 `--malloc=system|builtin`。裸 `--malloc`、空值和其他值报错。
- 内部保留“未指定”状态，用于区分非 bin 的合法省略与非法显式指定；bin 在统一入口
  将未指定解析为 system。重复选项处理沿用现有 CLI 约定，不新增另一套规则。
- `build/run/pack` 将选项传入当前项目的共用构建入口，再传向直编；在开始构建前统一
  校验实际 target，非 bin 显式指定时报错。合法 bin 将选项传至最终链接。
  递归构建本地 lib 依赖时不传递，不因切换选项重编无关依赖；最终可执行文件必须按
  本次选择重新链接。
- `build --platform=...` 为每个目标选择对应库。`run` 在 `--` 后的参数仍原样交给程序。
- `system` 表示 Feng 不主动引入 mimalloc；它不撤销用户通过其他链接输入自行提供的
  分配器。显式 builtin 也不能保证覆盖原生依赖自行封装或私有绑定的分配器。
- builtin 库缺失、损坏或目标不符时明确失败，禁止静默退回 system。system 路径
  不查找、验证或链接 mimalloc，不受其文件缺失影响。

不新增 `feng.fm` 字段、环境变量或用户程序中的切换 API。

## 4. 制品构建与分发

### 4.1 独立、按目标分发

采用 `extlib/` 预构建方案，复用现有 libunwind 的维护制品布局及 runtime 的目标平台分工。
维护脚本生成真实 archive，本地构建建立软链，发行时复制为独立文件：

```text
extlib/<target-platform>/libmimalloc.a          # 维护脚本预构建的真实文件
build/lib/<target-platform>/libmimalloc.a       # 本地 make 建立的相对软链
  -> ../../../extlib/<target-platform>/libmimalloc.a
<feng-install>/lib/<target-platform>/libmimalloc.a  # 发行包内的真实文件
<feng-install>/lib/<target-platform>/libfeng_runtime.a
```

选择 `extlib/` 是为了沿用仓库已有的目标原生库管理方式。无需另设
`toolchain/<target-platform>/` 存储，也不在两个位置重复维护同一制品。

目标范围与当前 runtime 一致：`macos-arm64`、`linux-x64-gnu`、`linux-x64-musl`、
`linux-arm64-gnu`、`linux-arm64-musl`。每份 host 发行包包含这五份目标库，按最终
编译目标选取；不是像 LLVM 插件那样仅分发当前 host 一份。
库路径依照 Feng 安装根解析，支持安装搬移，不搜索本机 Homebrew 或系统 mimalloc。

构建建议以当前仓库源码为起点，暂不升级 mimalloc。首轮采用**原生机器码静态库**，
兼容 bin 的 `--lto=off|thin`；“像 runtime 一样分发”不要求复制 runtime 的 bitcode
格式。这样不额外引入分配器的 bitcode 版本及混合 SDK module flag 依赖。
如后续要将 mimalloc 也编为 ThinLTO，需另行证明收益与兼容性。

### 4.2 维护预构建、本地布局与发行接入

1. 增加 `scripts/build_mimalloc.sh` 维护入口，遵循
   [`build_libunwind.sh`](../../scripts/build_libunwind.sh) 的工程方式，使用对应 host 的
   工具及目标 SDK/sysroot。各目标在 `temp/` 的独立目录中编译、归档并校验，成功后
   安装到 `extlib/<target-platform>/libmimalloc.a`。不复用第三方源码目录中残留的
   `static.o`，不因构建失败覆盖已有有效制品。
2. 使用上游单文件入口及 `MI_MALLOC_OVERRIDE`。Release 构建必须显式定义 `NDEBUG`：
   当前源码在未定义它时默认启用 `MI_DEBUG=2`，仅指定 `-O3` 不足以关闭内部调试检查。
   优化和宏仅作用于 mimalloc 制品，不改变 Feng 的全局编译参数。
3. 本地 `make` 将预构建库映射到 `build/lib/<target-platform>/libmimalloc.a`，仅建立或
   修复相对软链，不增加 mimalloc 源码编译、归档或下载步骤。正确软链不重复写入，
   `make clean` 清除本地构建布局，不删除 `extlib/` 制品。库缺失时不自动执行维护脚本，
   builtin 的缺失诊断按 §3 处理。
4. 发行组件从本地布局读取 archive，复制其真实内容到组件及最终发行包的
   `lib/<target-platform>/libmimalloc.a`；不得把指向仓库 `extlib/` 的软链带入发行包。
   常规发行任务复用已维护的预构建制品，不重新编译 mimalloc。组装、manifest 和安装
   校验检查架构、文件格式、完整性与来源，复用 runtime 的目标归属关系。
5. 核实并补齐所选源码版本的上游许可证及随包许可文本。当前提取目录未包含独立
   LICENSE 文件，二进制正式分发前须补齐；不因此引入其他第三方库或源码升级。

正常构建和发行只消费预构建库，不调用 `scripts/fetch_mimalloc.sh` 或
`scripts/build_mimalloc.sh`。前者维护源码，后者维护目标制品；均不成为用户编译时
依赖。临时生成物放入 `temp/`，持久下载缓存才使用 `local/`。维护脚本支持对应 host
系统，不感知容器调度。Feng 编译器自身不链接 mimalloc。

## 5. 最终链接与覆盖边界

### 5.1 链接流程

在现有 bin 链接分支中执行：

1. 解析分配器选项。system 继续使用原链接参数；builtin 定位目标 `libmimalloc.a`。
2. 让 mimalloc 的覆盖实现进入最终程序，并使 Feng 生成代码、runtime 及参加静态链接的
   库对标准分配函数的引用解析到该实现。
3. 保持现有 std、runtime、用户库、平台支持库及优化选项的规则；只增加必要的 allocator
   链接输入和局部保留参数。lib 编译分支不新增分配器输入。

**不能仅添加一个 `-lmimalloc` 就认定完成。** 必须确认静态库按需抽取、符号解析、
ThinLTO 和 dead strip 后实际使用的实现。优先验证上游单对象静态覆盖方式与正确链接
顺序；若还需强制抽取，候选为 ELF 的 `--whole-archive/--no-whole-archive` 或
Mach-O 的 `-force_load`，作用范围只能是该库。最终采用的最小参数以隔离验证为准，
不全局关闭裁剪、内联、向量化或内建函数优化，不强制载入所有依赖库。

验收必须同时包含符号/链接证据与运行时分配归属证据；“库在命令行中”或“程序有
`malloc` 符号”不足以证明覆盖正确。程序运行不依赖 `LD_PRELOAD`、
`DYLD_INSERT_LIBRARIES` 或外部 mimalloc 动态库。

### 5.2 分配函数族与平台边界

采用上游对目标平台提供的覆盖集合，至少验证 `malloc/calloc/realloc/free`；同时核对
对齐分配、分配大小查询及原生 C/C++ 依赖使用的相关入口，避免混用不同分配器。
`calloc` 的零初始化、对齐、溢出检查和失败返回约定必须保持；不改变 Feng 类型零值
或 OOM 处理策略。实际地址、内存布局、RSS、可分配总量与耗时可以随分配器变化。

需要单独验证以下边界，不能以 Linux GNU 预加载结果代替：

- **macOS：** 上游源码分别提供动态 interpose 和 malloc zone 支持。静态接入需要确认
  正确的上游构建方式，以及与系统库之间的分配/释放归属；不能假定主程序的同名符号
  自动替换所有系统内部调用。优先复用上游机制，不自行增加 Feng 的分配器桥接层。
- **Linux GNU / musl：** 分别验证现有链接方式、libc 绑定和跨库分配/释放。已有源码
  含平台差异，不能把 GNU 制品复用于 musl。
- **多个原生依赖：** 同一程序只引入一次内建 archive；检查用户依赖另带分配器时的
  冲突，不靠链接顺序随机选择。自定义 allocator 或私有绑定不在通用覆盖承诺内。
- **Sanitizer：** ASan 的分配拦截与 allocator 的协同必须验证。不得为通过测试而
  静默切回 system、跳过诊断断言或声称未覆盖的内存错误已受检查。如果当前上游构建
  无法支持所需组合，应记录原因并提交人工决定，再确定支持边界。

静态覆盖若在某目标上无法满足上述条件，先记录证据并 Review，不自行换为动态分发、
修改 runtime API 或引入按用例选择分配器的方案。

## 6. 预计变更范围

| 范围 | 必要改动 | 保持不变 |
| --- | --- | --- |
| CLI 公共选项、直编 options/direct/driver | 解析、传递、bin 限定及目标库链接 | 语义分析、Codegen、runtime ABI |
| project build/run/pack 与共用编译入口 | 解析、传递选项；按实际 target 校验，非 bin 显式使用时报错 | 依赖库编译选项及各命令原有 target 限制 |
| mimalloc 维护脚本与 extlib 制品 | 预构建、校验并安装各目标 archive | 正常 make 和发行流程不编译 mimalloc |
| Makefile 的本地布局规则 | 在 build/lib 下建立预构建库的相对软链 | 编译器与 LLVM 工具选择策略 |
| 发行组件、组装、安装校验 | 将预构建库复制为真实文件，增加 manifest 与许可证 | 现有 host/target 分工及 sysroot 来源 |
| CLI/build/release 主规范及 usage | 记录经 Review 的规则、同步帮助文本 | 无关规范与包格式 |
| test/、必要的 FCTS 覆盖 | 选项、链接、发行与语言行为验证 | 原用例算法及断言强度 |

以上是待实施范围，不表示每个关联文件都必须修改。每处改动须对应必要行为；
不顺带改写源码下载脚本、升级其他工具或调整已有性能用例。

## 7. 验证与验收

### 7.1 功能与兼容性

| 维度 | 必须验证的内容 |
| --- | --- |
| CLI | 四个入口均识别选项；默认/system/builtin；非法值；build/run/pack 传递；非 bin 显式指定报目标不适用；pack 不报未知选项；`--` 后程序参数 |
| 构建模式 | Debug/Release × lto off/thin × system/builtin；保留当前默认 LTO 规则 |
| 依赖 | 同一份 `.a/.fb` 链接两种程序；本地依赖不接收 malloc 选项；多个包只引入一份 allocator；切换后重新链接 |
| 维护与本地布局 | 五个 target 的归档与匹配；维护构建失败不覆盖旧库；make 建立/修复相对软链且不编译 mimalloc；正确软链不重复写入；clean 保留 extlib |
| 发行制品 | 独立库为真实文件、内容与预构建输入一致；安装搬移且不依赖仓库；builtin 缺失/损坏失败；system 无此库仍可编译 |
| 实际覆盖 | 从生成代码、runtime、独立原生库发起分配；核对实际实现及跨边界释放；检查最终程序无 mimalloc 动态依赖 |
| 分配行为 | 零初始化、零大小、对齐、realloc 保留内容与失败后的旧块、大小溢出、跨线程释放和线程退出 |
| Feng 行为 | 普通对象、值对象、union、数组、字符串、异常清理、终结器和循环回收；无 ARC 语义变化 |
| 调试与诊断 | DAP 原有对象显示和断点；Sanitizer 支持边界与诊断有效性 |

优先新增独立用例。修改已有入口注册或迁移既有夹具前，按仓库规则取得人工批准。
Linux 容器先验证；macOS 验证配合本机安全授权，首次启动预检与正式计时分开。
正式实现后执行 Linux 与 macOS 的 `make test`，并覆盖 builtin 的相关功能矩阵；
默认 system 的全量通过不能替代 builtin 验证。性能评测仍独立于 `make test`。

### 7.2 性能

复用 [benchmarks](../../benchmarks/README.md) 的五项输入、输出校验和采集方式，
以同一提交、同一模式分别构建 system 与 builtin，串行交错测量耗时和峰值 RSS，
并记录目标、库版本、构建参数及制品身份。两组唯一的预期区别是分配器链接选择。

- 首先确认 system 相对现有基线没有新增运行时成本。
- 确认静态 builtin 的 binary-trees 收益，并重测 text-scan 等已观察到的负向结果，
  不承诺静态结果必然复制动态实验中的 51% 改善。
- 记录链接耗时和最终程序体积；新增内存与运行时间成本一并提交人工 Review。
- 若某平台未验证或仍有退化，明确列出，不以平均值或只展示获益用例代替结论。

## 8. 分步 Todo

### 阶段一：隔离验证静态接入

- [x] 人工确认 run/build/pack/直编均支持该选项，显式指定且 target 不是 bin 时报错。
- [x] 人工要求由维护脚本预构建独立库，本地 make 建立软链，发行时提供对应目标制品；本文采用 extlib → build/lib 软链 → 发行 lib 的布局。
- [ ] 人工确认默认 system 及原生静态库分发方案。
- [ ] 在 `temp/` 中按五个目标构建 archive；确认 Release 宏、许可及 macOS 上游接入方式。
- [ ] 不改正式源码，验证最小链接参数、实际覆盖、跨库归属、LTO/裁剪及 Sanitizer 边界。
- [ ] Linux 与 macOS 重测五项性能，记录静态收益、退化与 RSS，确定正式接入方案。

### 阶段二：最小实现与分发

- [ ] 先将已确认规则写入 CLI/build/release 主规范，再实现四个入口的选项解析、build/run/pack 传递、target 校验及 bin 链接。
- [ ] 增加维护预构建脚本并生成 extlib 目标制品；本地 make 仅建立 build/lib 软链。
- [ ] 接入发行组件的真实文件复制、组装、安装校验与许可证；保持 runtime 独立。
- [ ] 同步命令 usage 与 `feng-cli-help.txt`，确认未增加包格式或 runtime API 变更。

### 阶段三：完整验证与交付

- [ ] 补齐 §7 的独立用例，必要的既有测试注册变更先取得批准。
- [ ] Linux 容器全量回归与 builtin 矩阵通过，再完成 macOS 全量回归及覆盖验证。
- [ ] 使用正式构建制品重跑五项评测，Review 每项耗时、RSS 和已知成本。
- [ ] 更新验证记录、给出完整变更清单与 commit message，由人工提交。
