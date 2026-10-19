# Feng 可选内建分配器开发方案

> 状态：实现、独立用例、Linux 完整回归及五项性能评测已完成，待人工 Review；macOS 构建、CLI 选项及发行布局检查通过，全量回归和性能验证待完成。日期：2026-10-18。
>
> 本文设计 `--alloc=system|builtin`：将 mimalloc 按目标平台构建为独立静态库，随 Feng
> 发行包提供，仅在最终可执行程序选择 `builtin` 时链接并覆盖分配函数。
> §2.2 是早期动态覆盖证据；本轮静态实现的验证记录见 §9，二者分别记录。

## 1. 目标与边界

为应用提供构建时可选的通用分配器，不改变 Feng 语义、对象布局、ARC、循环回收算法、
异常清理或 std API，不增加按对象类型、分配大小或评测用例选择分配器的特殊路径。

默认使用 `system`，显式指定 `builtin` 时使用随发行包提供的 mimalloc。
选择在最终链接时完成，不在每次分配时增加分支、函数表或 Feng 包装层。
本方案只影响生成的应用，不改变 Feng 编译器进程自身的分配器。

**run、build 和直编接受 `--alloc`；显式指定时，target 必须为 bin，否则报错。**
pack 只支持 lib，不支持该选项，沿用现有未知选项报错。
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
- 项目命令通过内部选项调用直编，递归依赖另行构建。build/run 需解析、传递
  `--alloc`，但不应照搬 `--lto` 向依赖库传递的规则。pack 虽会触发构建，但只支持
  lib，不增加该选项。

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

人工已确认选项名为 `alloc`，默认使用 system，不增加选项别名。
run、build 和直编支持该选项，并按实际 target 校验。
下表适用于这三个入口：

| 场景 | 未指定 | `--alloc=system` | `--alloc=builtin` |
| --- | --- | --- | --- |
| 实际 `target=bin` | system | 不引入内建分配器 | 链接目标 mimalloc 库 |
| 实际 target 不是 bin | 既有行为 | 报错：仅适用于 bin | 报错：仅适用于 bin |

pack 只接受 lib 项目，因此不注册、不解析或传递 `--alloc`。传入该选项时，
由现有未知选项处理自然报错，不增加专门诊断分支。各命令原有的 target 限制保持不变。

示例：

```sh
feng main.ff --target=bin --release --alloc=builtin
feng build --release --alloc=builtin
feng run --alloc=system -- <program-args>
```

- Debug 与 Release 均可显式选择分配器；分配器默认值不随模式或 LTO 设置变化。
- 仅增加带值形式 `--alloc=system|builtin`。裸 `--alloc`、空值和其他值报错。
- 内部保留“未指定”状态，用于区分非 bin 的合法省略与非法显式指定；bin 在统一入口
  将未指定解析为 system。重复选项处理沿用现有 CLI 约定，不新增另一套规则。
- `build/run` 将选项传入当前项目的共用构建入口，再传向直编；在开始构建前统一
  校验实际 target，非 bin 显式指定时报错。合法 bin 将选项传至最终链接。
  递归构建本地 lib 依赖时不传递，不因切换选项重编无关依赖；最终可执行文件必须按
  本次选择重新链接。
- `build --platform=...` 为每个目标选择对应库。`run` 在 `--` 后的参数仍原样交给程序。
- `system` 表示 Feng 不主动引入 mimalloc；它不撤销用户通过其他链接输入自行提供的
  分配器。显式 builtin 也不能保证覆盖原生依赖自行封装或私有绑定的分配器。
- builtin 库缺失、损坏及错误目标平台的处理按 [CLI 规范 §2.4](../specifications/feng-cli.md#24-顶层直编补充选项)。
  system 路径不查找、验证或链接 mimalloc，不受其文件缺失影响。

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

构建建议以当前仓库源码为起点，暂不升级 mimalloc。**人工已确认：本轮 mimalloc
预构建为原生机器码静态库，不启用 LTO。** 该库须兼容 bin 的 `--lto=off|thin`，
runtime 与 libunwind 保持现有构建方式。mimalloc 的 LTO 支持留待后续按实际需求、
性能收益及兼容性验证结果另行决定，不纳入本轮实施与验收。

### 4.2 维护预构建、本地布局与发行接入

1. 增加 `scripts/build_mimalloc.sh` 维护入口，遵循
   [`build_libunwind.sh`](../../scripts/build_libunwind.sh) 的工程方式，使用对应 host 的
   工具及目标 SDK/sysroot。各目标在 `temp/` 的独立目录中编译、归档并校验，成功后
   安装到 `extlib/<target-platform>/libmimalloc.a`。不复用第三方源码目录中残留的
   `static.o`，不因构建失败覆盖已有有效制品。
2. 使用上游单文件入口及 `MI_MALLOC_OVERRIDE`。Release 构建必须显式定义 `NDEBUG`：
   当前源码在未定义它时默认启用 `MI_DEBUG=2`，仅指定 `-O3` 不足以关闭内部调试检查。
   优化和宏仅作用于 mimalloc 制品，不改变 Feng 的全局编译参数。
3. 本地 `make` 复用 `RUNTIME_PLATFORMS` 选择目标，具体范围见
   [发行与安装规范 §5](./feng-release-and-install.md#5-toolchain-形态)，不改变 runtime
   的位置及构建规则。将预构建库映射到 `build/lib/<target-platform>/libmimalloc.a`，仅建立或
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
| project build/run 与共用编译入口 | 解析、传递选项；按实际 target 校验，非 bin 显式使用时报错 | 依赖库编译选项、pack 选项解析及各命令原有 target 限制 |
| mimalloc 维护脚本与 extlib 制品 | 预构建、校验并安装各目标 archive | 正常 make 和发行流程不编译 mimalloc |
| Makefile 的本地布局规则 | 在 build/lib 下建立预构建库的相对软链 | 编译器与 LLVM 工具选择策略 |
| 发行组件、组装、安装校验 | 将预构建库复制为真实文件，增加 manifest 与许可证 | 现有 host/target 分工及 sysroot 来源 |
| CLI/build/release 主规范及 usage | 记录经 Review 的规则、同步帮助文本 | 无关规范与包格式 |
| test/、必要的 FCTS 覆盖 | 选项、链接、发行与语言行为验证 | 原用例算法及断言强度 |

以上限定实施范围，不表示每个关联文件都必须修改。每处改动须对应必要行为；
不顺带改写源码下载脚本、升级其他工具或调整已有性能用例。

## 7. 验证与验收

### 7.1 功能与兼容性

| 维度 | 必须验证的内容 |
| --- | --- |
| CLI | run/build/直编识别选项；省略默认为 system；显式 system/builtin；非法值；build/run 传递；非 bin 显式指定报目标不适用；pack 沿用未知选项报错；`--` 后程序参数 |
| 构建模式 | 主程序 Debug/Release × lto off/thin × system/builtin；mimalloc 固定为原生机器码库，保留主程序当前默认 LTO 规则 |
| 依赖 | 同一份 `.a/.fb` 链接两种程序；本地依赖不接收 allocator 选项；多个包只引入一份 allocator；切换后重新链接 |
| 维护与本地布局 | 五个 target 的归档与匹配；维护构建失败不覆盖旧库；三种 host 布局中即使备齐五个平台的预构建库，也只为本地 runtime 目标建立软链；make 建立/修复相对软链且不编译 mimalloc；正确软链不重复写入；clean 保留 extlib |
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

- [x] 人工确认选项名为 `--alloc=system|builtin`，不增加选项别名。
- [x] 人工确认 run/build/直编支持该选项，显式指定且 target 不是 bin 时报错；pack 不支持，沿用未知选项报错。
- [x] 人工要求由维护脚本预构建独立库，本地 make 建立软链，发行时提供对应目标制品；本文采用 extlib → build/lib 软链 → 发行 lib 的布局。
- [x] 人工确认本轮 mimalloc 不启用 LTO，使用原生机器码静态库；后续另行评估 LTO。
- [x] 人工确认默认 system。
- [x] 在 `temp/` 中按五个目标构建 archive；确认 Release 宏、许可及 macOS 上游接入方式。
- [x] 在 Linux 隔离验证最小链接参数、实际覆盖、跨库归属、与主程序 lto off/thin 的混合链接、裁剪及 Sanitizer 边界。
- [x] Linux 重测五项性能，记录静态收益、退化与 RSS，确定正式接入方案。
- [ ] macOS 完成静态覆盖矩阵与五项性能验证；本轮不以 Linux 结果替代。

### 阶段二：最小实现与分发

- [x] 先将已确认规则写入 CLI/build/release 主规范，再实现 run/build/直编的选项解析、build/run 传递、target 校验及 bin 链接。
- [x] 增加维护预构建脚本并生成 extlib 目标制品；本地 make 仅建立 build/lib 软链。
- [x] 接入发行组件的真实文件复制、组装、安装校验与许可证；保持 runtime 独立。
- [x] 同步命令 usage 与 `feng-cli-help.txt`，确认未增加包格式或 runtime API 变更。

### 阶段三：完整验证与交付

- [x] 补齐 §7 的独立用例，必要的既有测试注册变更已取得批准。
- [x] Linux 容器 builtin 功能、GNU/musl、模式/LTO、UBSan 和 DAP 矩阵通过。
- [x] 按批准范围补齐 `native_symbols_release.sh` 的新增发行文件夹具，Linux 容器完整 `make test` 通过。
- [ ] macOS 全量回归及覆盖验证。
- [x] 使用正式构建制品重跑 Linux 五项评测，列明每项耗时、RSS 和已知成本，提交人工 Review。
- [x] 更新验证记录、给出变更范围与 commit message，由人工 Review、提交。

## 9. 静态实现验证记录（2026-10-18）

本轮基线为 `484031ad`，当前任务按人工要求先执行 Linux 容器回归和五项评测。
macOS 全量回归及静态覆盖的完整执行矩阵保留待验证，不以制品已生成代替执行结果。

macOS 本地构建补充发现：新增软链检查在 `$(shell ...)` 中使用未转义的 shell `#`
前缀删除语法，GNU Make 3.81 将其解释为注释，导致包括 `clean`、`all` 在内的所有
目标解析失败；Linux GNU Make 4.4.1 未复现。修复仅将该检查的路径转换改为 Make
自带的 `patsubst`，不更改工具选择、构建目标或软链规则。修复后 macOS 的
`make clean`、干净 `make all`、重复构建及相对软链检查均已通过。
新增 allocator 用例使用非空命令或参数数组，兼容 macOS Bash 3.2 在 `set -u`
下的空数组行为；保持测试场景、编译参数和原有检查不变。macOS 发行用例已通过。
选项改为 `--alloc` 后，三个入口的帮助、非法值、旧拼写拒绝及 bin 限制已在 macOS
验证；帮助说明仍为 `Allocator (bin only)`。最终版本的 Linux 完整 `make test`
于 2026-10-18 再次通过，包含 Sanitizer 和普通两阶段，最终退出码为 0。

本地 mimalloc 软链范围随后收敛为 `RUNTIME_PLATFORMS`，runtime 的构建位置和发行包
五平台布局保持不变。新增三种 host 的布局用例，验证备齐五份预构建库时仅创建各自
runtime 目标的软链。macOS GNU Make 3.81 定向验证通过，清理旧的非本地目标软链后
runtime 文件 SHA-256 未变；Linux ARM64 容器再次完成全量 `make test`，Sanitizer 和
普通两阶段均通过，最终退出码为 0，发行组装和安装校验通过。

macOS CI 随后发现原生分配测试夹具定义 `_POSIX_C_SOURCE` 后使用
`<malloc/malloc.h>`，使 SDK 隐藏了所需的 Darwin 类型声明。修复仅在该夹具的
Apple 分支、所有头文件之前补充 `_DARWIN_C_SOURCE`，保留现有断言、编译参数及
Linux 宏配置；不修改编译器或 runtime。
宏修复后的 Linux ARM64 容器完整 `make test` 再次通过，退出码为 0；Sanitizer 和普通
两阶段的 std 均为 610/610，FCTS 均为 1709/1709，allocator GNU/musl 矩阵通过。
修复后的夹具在本机 SDK 26.5 和 27.0 下编译通过。macOS allocator 专项测试随后在
system 链接阶段遇到 `mi_is_in_heap_region` 未定义：`weak_import` 声明未提供链接时
所需的符号定义。无头文件的最小程序在 Apple ld 和 LLD 上均复现，该问题独立于
宏修改。经人工批准，仅在 macOS 链接该原生夹具的测试调用中，通过现有
`FENG_CC_FLAGS` 传入 `-Wl,-U,_mi_is_in_heap_region`，允许这一可选查询符号缺失；
system 下弱引用为空，builtin 下仍调用 mimalloc 的真实查询函数。保留全部断言，
其他未定义符号仍报错，不修改编译器、runtime、生产链接参数或 Linux 测试行为。
该链接修复合入测试脚本后，Linux ARM64 容器完整 `make test` 再次通过，退出码为 0；
两阶段 std 均为 610/610，FCTS 均为 1709/1709，普通阶段的 allocator GNU/musl
矩阵、build/run 切换、发行布局及 system/builtin × LTO off/thin DAP 全部通过。
macOS 本轮完整专项运行在前置 `asan-disabled/bin/empty` 启动时被 SIGKILL (9)
终止，尚未执行到原生夹具。按人工安排，macOS 的完整 allocator 专项和全量回归
由开发者稍后手工验证；不以此前临时对照程序通过代替完整测试结果。

2026-10-19 的 macOS 手工回归继续发现两项测试警告：共享库夹具使用裁剪后的 Clang
调用 Apple ld，后者查找未随包分发的 `libLTO.dylib`；原生夹具的 `-U` 链接参数经
`FENG_CC_FLAGS` 传到依赖库的纯编译阶段，触发 unused-argument 警告。修复仅让共享库
夹具显式使用随包 LLD，并用 Clang 的 `--start-no-unused-arguments` /
`--end-no-unused-arguments` 包围这一个测试链接参数，允许纯编译阶段忽略它，其他参数
诊断保持有效。维护脚本补齐执行权限，并在发行测试中验证可直接调用。
人工决定保留链接器对错误平台库的既有处理，见 CLI 规范 §2.4；因此移除编译阶段要求
错误平台库必须失败的反例，保留发行阶段的目标平台校验及其拒绝用例。编译器、runtime
和生产链接参数均不修改。
本轮 Linux ARM64 容器完整 `make test` 通过，退出码为 0；两阶段 std 均为 610/610、
FCTS 均为 1709/1709，allocator 完整矩阵、build/run、发行校验及 DAP 均通过。
macOS 的 Debug/Release × LTO off/thin × system/builtin 八组项目构建（含依赖库）
均在 `-Werror` 下通过；共享库链接无上述警告，局部标记外的无用参数仍会被诊断。
macOS 完整执行回归尚未完成：发行专项在复制后编译器的版本检查处失败，单独启动
同内容副本复现 SIGKILL (9)、退出码 137，原位置的 `--version` 正常；后续仍由开发者
手工验证。临时日志位于 `temp/allocator-test-fix-20261019/`。

已确认的实现和证据：

- 五个平台的 archive 均由 `scripts/build_mimalloc.sh` 生成，每份仅含原生
  `mimalloc.o`；保留仓库 2.1.2 源码，补齐该版本上游 MIT 许可证。
- Linux GNU 中，仅依赖按需抽取时，另一个原生依赖若先提供标准分配函数，mimalloc
  可以完全不被抽取。局部强制抽取后，相同输入会因重复符号而失败；因此正式链接
  使用构建规范 §2.5 的局部保留参数。
- Linux ARM64 的 GNU/musl、Debug/Release、LTO off/thin、system/builtin 矩阵已通过。
  测试从 Feng 数组、独立 C 库及 C++ 发起分配，通过弱引用的上游归属查询确认实际
  堆归属；覆盖跨线程及跨库释放、对齐、零初始化、溢出和 realloc 失败后的旧块。
- 独立用例已验证库依赖复用、build/run 切换、安装搬移、缺失/损坏失败、发行阶段拒绝错误目标库、
  冲突分配器报错、本地软链及维护失败保留原制品；现有 FCTS 已用 builtin 运行通过。
- DAP 的 system/builtin × LTO off/thin 四组独立用例已通过，检查源断点、数组逻辑元素、
  对象字段和正常退出。Sanitizer 阶段另以 system × LTO off/thin 验证调试行为，
  同时检查 builtin + ASan 的拒绝诊断；该阶段已完整通过。
- 使用相同 std/runtime、源码和输出路径构建五项用例，新版 system 与基线生成的
  五个可执行文件均逐字节一致。
- Linux ARM64 容器完整 `make test` 已通过，最终退出码为 0，包含 ASan/UBSan 和普通
  两阶段；两阶段 std 均为 610 项、FCTS 均为 1709 项，零失败、零跳过。普通阶段还
  通过 builtin FCTS、GNU/musl 模式矩阵、UBSan 诊断和四组 allocator/LTO DAP 用例。
  `test/cli/native_symbols_release.sh` 按人工批准仅补齐库、许可证、manifest 和模拟源
  目录输入，原插件检查步骤与断言保持不变；该用例在完整回归中通过。
- Linux x64 GNU/musl 的独立原生分配归属用例通过（macOS 上的 x64 Linux 容器经
  Rosetta 执行）；不据此声称已完成 x64 Feng 全量回归。

人工已批准的兼容边界：当前静态 mimalloc 与 ASan 组合在 Linux GNU 的初始化阶段
崩溃。LLDB 回溯显示 ASan 初始化 interceptor 时调用动态加载器，加载器的分配进入
`mi_process_init`，随后使用尚未初始化完成的 `clock_gettime` interceptor 并跳到空地址。
普通构建和 UBSan 隔离实验通过；这不是用户 main 中的内存错误，也不是 Feng 异常路径。
因此按 CLI 主规范明确拒绝 builtin + ASan，不修改上游源码或自动替换分配器。
Sanitizer 回归保留 system 覆盖，并验证 builtin 的拒绝诊断；普通回归执行 builtin
完整矩阵及 DAP，另外验证 UBSan。不能将带 ASan 的 runtime 与普通 builtin 程序混合。

临时构建、独立测试与 ASan 回溯位于 Linux 容器
`/workspace/temp/allocator-20261018/`；该目录不作为永久归档。正式性能数据在完成后
归档到 `benchmarks/results/`。

### 9.1 Linux ARM64 性能对照

使用现有五项 standard 输入和校验器，Release / ThinLTO，预热 2 轮、正式 24 轮，
将基线、system、builtin 和 Swift 串行交错测量。完整样本、制品身份和结果见
[allocator 对照归档](../../benchmarks/results/allocator-2026-10-18-linux-arm64/report.md)。
下表为中位数；百分比为 builtin 相对 system 的耗时变化，负值表示变快。

| 用例 | system / ms | builtin / ms | 耗时变化 | system RSS / MiB | builtin RSS / MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| nbody | 631.080 | 633.602 | +0.4% | 1.80 | 1.89 |
| binary-trees | 317.003 | 147.279 | −53.5% | 17.27 | 18.03 |
| text-scan | 107.034 | 119.557 | +11.7% | 245.48 | 249.91 |
| fannkuch-redux | 1626.960 | 1640.128 | +0.8% | 1.56 | 1.90 |
| spectral-norm | 101.387 | 101.136 | −0.2% | 1.80 | 2.02 |

builtin 显著改善 binary-trees，但增加 text-scan 耗时和峰值内存；其余三项差异小于
1%，不据此声称存在稳定收益或退化。五项 builtin 程序体积均约增加 136 KiB。
这些是显式选择 builtin 的成本，不改变默认 system。默认 system 与变更前基线的
五个程序逐字节一致；最终编译器重建后，基线/system/builtin 共 15 个 Feng 程序仍与
本轮已采样程序逐字节一致，复核记录见
[final-build-verification.json](../../benchmarks/results/allocator-2026-10-18-linux-arm64/final-build-verification.json)。

原有八种语言／运行配置的五项正式评测也已重新构建并执行：184 项输出校验通过，
预热 2 轮、正式 7 轮，采用默认 system。原始样本和源码快照见
[完整跨语言结果](../../benchmarks/results/2026-10-18T02-00-14-921Z/report.md)。
该结果与上表属于不同采样轮次，不将两轮绝对耗时混合计算 allocator 收益。
