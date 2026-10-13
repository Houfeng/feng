# Feng ThinLTO 性能优化方案

> **状态：已批准实施，代码变更待人工 Review。日期：2026-10-13。**
>
> 本文整理 runtime、lib、bin 的 ThinLTO 方案、已完成的隔离实验及正式交付条件。
> 三态选项、项目命令透传、直编层解析默认值、runtime 单格式分发及最小改动要求
> 已由人工确认。按本轮要求，先完成主规范与必要代码变更，人工 Review 后再补用例、
> 重建制品并执行回归和性能验证；本文实验数据不代表正式实现已验收。

## 1. 目标与范围

通过 ThinLTO 让 LLVM 在最终链接时取得 Feng 库和 runtime 的函数体，进行跨编译单元
内联及后续优化。目标是在保持语言语义、可观察行为和既有 SDK 分工的前提下，使三个
基础单线程基准不慢于同机、同轮 Swift，并使普通应用也能获得通用优化收益。

建议采用以下组合：

1. 分发的 Feng runtime 开启 ThinLTO，允许 Clang 的内建库函数优化。
2. Feng 生成的 bin/lib 统一允许内建库函数优化，不因 SDK-free 编译而附带
   `no-builtins`；各平台原有 SDK/sysroot 使用方式不变。
3. 增加一个编译选项控制当前 bin/lib 是否以 ThinLTO 形式编译；Release 默认开启，
   Debug 默认关闭，显式选项优先于模式默认值。
4. Debug 默认保持原生对象、`-O0 -g` 和现有 `.fd` 调试链路；最终链接继续接受
   依赖库及 runtime 中的 bitcode。

**所有改动必须先证明必要，再实施；以满足已确认要求的最小改动交付。**
具体证据要求见 §7.1，本文列出的候选位置和实验参数不构成全部修改的授权。

本轮保持 Release 的 O2、向量化策略、浮点设置、安全检查及 runtime 私有 ABI。
不修改基准算法，不增加评测、元素类型或特定数学函数的特殊分支，不新增 LLVM 插件，
不实施 `@inline`、不恢复 `@memory`，不要求第三方原生依赖全部重编为 bitcode。

当前 [`@inline` 草案](feng-inline-dev.md) 是另一项待审方案；本方案不依赖它，也不改变
其状态。此前 [text-scan 优化](feng-text-scan-performance-optimize.md) 的 std.fs 实现
及 runtime 头文件支持作为现有基线保留。

新增规则收敛到以下主规范，本文保留实施依据与验证记录：

| 内容 | 主规范或现有依据 |
| --- | --- |
| CLI 选项与命令语法 | [CLI 规范](../specifications/feng-cli.md) |
| 编译模式、参数传递、SDK-free、链接与调试 | [构建规范](../specifications/feng-build.md) |
| `.fb` 中的库制品与依赖 | [包规范](../specifications/feng-package.md) |
| runtime 与工具链发行布局 | [发行与安装](feng-release-and-install.md) |
| 黑盒性能测量 | [benchmarks 主规范](../../benchmarks/README.md) |

## 2. 已验证的性能与原因

### 2.1 同批测量结果

最新 [完整实验记录](../../benchmarks/results/2026-10-13T04-38-13-433Z-runtime-thin-rtlo/EXPERIMENT.md)
使用 Apple M4 Pro、macOS 27.0.1、Clang/LLD 23.1.3。保持 O2、原插件、原浮点设置和
相同生成 C，按既有采样器串行轮换执行，预热 2 轮、正式 7 轮。
输入分别为 nbody 20,000,000 步、binary-trees 深度 16、text-scan 256,000,000 字节。

以下是整程序耗时中位数，单位 ms，越小越好。“std/bin”及“全部”均允许内建函数优化，
且 std 保持 SDK-free。

| 用例 | 原版 Feng | 仅 std/bin ThinLTO | runtime/std/bin ThinLTO | C++ | Rust | Go | Swift |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| nbody | 1390.974 | 799.997 | 786.026 | 660.284 | 649.202 | 2785.993 | 1009.681 |
| binary-trees | 301.178 | 283.934 | 266.202 | 222.210 | 178.266 | 232.599 | 308.907 |
| text-scan | 104.923 | 101.319 | 91.960 | 112.897 | 77.146 | 128.354 | 109.317 |

全部 ThinLTO 相对原版分别减少 **43.49%、11.61%、12.35%** 用时；相对 Swift 分别减少
**22.15%、13.82%、15.88%**。三个用例各 7 个正式样本均快于同项全部 Swift 样本。
这证明本机实验组合已达到目标，不代表所有平台或应用都必然提速。

在 std/bin 已使用 ThinLTO 的基础上，runtime 参与后，binary-trees 和 text-scan
分别减少 6.25%、9.24% 用时；nbody 仅减少 1.75%，两组样本范围重叠，不认定为稳定的
额外收益。nbody、binary-trees 仍分别比同批 C++ 多用时 19.04%、19.80%。

RSS 是运行基准程序时的进程驻留内存指标。这里由现有
[采样器](../../benchmarks/scripts/measure.c) 通过 `wait4` 的 `ru_maxrss` 取得每次进程
运行期间的峰值，再对 7 个正式样本取中位数。它统计进程内存，不是链接器的内存，
也不是仅统计 Feng 对象或堆分配。

下面比较的是同批“仅 std/bin ThinLTO”和“再让 runtime 参与 ThinLTO”，单位为 KiB：

| 用例 | 仅 std/bin ThinLTO | runtime/std/bin ThinLTO | 中位数差值 | 相对变化 |
| --- | ---: | ---: | ---: | ---: |
| nbody | 1808 | 1872 | +64 | +3.54% |
| binary-trees | 30752 | 30832 | +80 | +0.26% |
| text-scan | 251872 | 251952 | +80 | +0.032% |

以上数值已与 [原始样本](../../benchmarks/results/2026-10-13T04-38-13-433Z-runtime-thin-rtlo/evidence/paired-samples.jsonl)
核对。这是观测到的峰值差异；现有采样没有区分代码页、堆、栈等组成，具体来源尚未
定位。不能把它解释成新增了等量的堆分配，也不能认定每个 ThinLTO 程序都有固定的
64–80 KiB 开销。正式制品需要复测并核实原因。

### 2.2 为什么需要同时处理 ThinLTO 和 no-builtins

已有实验依次确认：

| 组合 | 已观察到的结果 |
| --- | --- |
| 只开启 std/bin ThinLTO，保留原有属性差异 | std 带 `no-builtins`，bin 不带；该调用方向的属性不兼容阻止部分内联，nbody 未明显改善 |
| std/bin 都带 `no-builtins` | 恢复相关内联，但仍保留 libm sqrt 调用；nbody 反而更慢 |
| std/bin 都允许内建函数优化，开启 ThinLTO | 最终 nbody 中 Math.sqrt 包装及数组 length 调用消失，两个开方点成为硬件开方 |
| SDK-free std 允许内建函数优化，但不开启 ThinLTO | std 内部开方发码已改善，bin 仍调用包外包装，未重现明显 nbody 收益 |
| runtime 也使用 ThinLTO | 更多 runtime 函数体进入跨模块优化，binary-trees、text-scan 进一步改善 |

前三项的对照见 [属性组合实验](../../benchmarks/results/2026-10-13T03-30-14-647Z-builtins-host/EXPERIMENT.md)，
第四项见 [SDK-free 无 LTO 实验](../../benchmarks/results/2026-10-13T04-14-24-597Z-sdkfree-nolt/EXPERIMENT.md)。
各轮有各自的同批对照，不应把不同批次的数字拼成单变量测量。

最新实验检查了实际 std/runtime 归档成员和最终链接保存的 IR、机器码：

- Feng runtime 的 14 个 C 文件均生成 bitcode；std、这些 runtime 成员及被检查的
  bin IR 均不带 `no-builtins` / `nobuiltin`。
- nbody 最终 IR 有两个 `llvm.sqrt.f64`，实际汇编有两条 `fsqrt`，对应主模块中不再有
  Math.sqrt、std 数组 length 和直接 libm sqrt 调用。
- runtime 参与后，最终主模块内若干 retain、清理登记和数组长度调用消失；
  binary-trees 的两个对象分配调用展开为可见的分配与初始化，部分 release 调用仍保留。

这些证据证明优化链路已生效，但没有分别量化包装内联、开方转换、内存访问优化等
每一项的独立贡献，不能把全部收益归结为少调用一次函数。

本轮 120 次实际输入检查、216 次预热及正式执行均通过，未观察到 SIGKILL、超时或
采样中断。完整语言回归、跨平台矩阵及下述 Debug 组合尚未完成，不能以基准通过替代。

## 3. 编译选项与默认行为

选项为 `--lto=off|thin`，只提供关闭和 ThinLTO 两种显式值，不增加 Full LTO 模式。
**人工已确认内部保留“未指定、off、thin”三个状态：**未指定时由 Release/Debug
模式决定，分别使用 thin/off；显式指定时直接使用该值，不受构建模式覆盖。

| 当前编译目标 | 构建模式 | 未指定选项 | 显式 `off` | 显式 `thin` |
| --- | --- | --- | --- | --- |
| bin | Release | ThinLTO | 当前生成 C 编译为原生对象 | 当前生成 C 编译为 ThinLTO bitcode |
| lib | Release | ThinLTO | 归档当前库的原生对象 | 归档当前库的 ThinLTO bitcode |
| bin/lib | Debug | 关闭 | 原生对象，保留 `-O0 -g` | ThinLTO bitcode，仍保留 `-O0 -g` |

**该选项控制本次编译的代码形态，不要求或禁止已有依赖使用 bitcode。**
`--lto=off` 的 bin 仍使用现有 LLD 链接，可以消费 ThinLTO 库；链接器必须把这些库中
实际参与链接的 bitcode 生成机器码。不能把 `off` 实现为拒绝 bitcode 输入。

**人工已确认 `build`、`pack`、`run` 向直编层透传 LTO 选项。**项目命令只接收、校验
并传递“未指定、off、thin”状态，不提前把未指定改为具体值，也不各自实现默认策略：

- 直编 `feng <files> --target=bin|lib`：统一结合收到的 LTO 状态和 Release/Debug
  模式决定实际值；直接调用与项目命令调用共用这套逻辑。
- `feng build`、`feng run`：当前项目及递归本地依赖均向直编层原样传递 LTO 状态，
  同时沿用现有构建模式传递；已有二进制 `.fb` 不重编。
- `feng pack`：保持当前固定 Release 构建，向内部构建及直编层原样传递 LTO 状态。
  未指定最终由直编层解析为 thin；显式 off/thin 不被打包流程覆盖。

透传沿用现有项目层构造 `FengCliDirectOptions` 并调用 `feng_cli_direct_run` 的内部
调用链，只补必要的选项传递，不为此改成拼接 shell 命令或启动新的直编进程。

命令示例（正式实现尚待回归验证）：

```sh
feng build --release              # 默认 ThinLTO
feng build                        # 默认 Debug，当前项目及本地依赖不启用 ThinLTO
feng build --release --lto=off     # Release，当前项目及本地依赖生成原生对象
feng pack --lto=off               # 用原生对象重新构建当前库并打包
```

参数内部应区分“未指定、off、thin”，在直编层统一解析实际值；避免多个命令各自维护
默认规则。沿用现有 `FENG_CC_FLAGS` 透传和工具选择规则，不为此新增环境变量或包装脚本。
若用户通过后端参数显式覆盖 LTO 或 builtin 设置，按实际后端参数工作；默认方案的
性能结论不涵盖这些人为覆盖组合。

## 4. SDK-free 与内建函数优化

### 4.1 保持现有 SDK 分工

| 场景 | 拟保留的依赖方式 |
| --- | --- |
| macOS 目标 lib，未显式指定 sysroot | SDK-free，只使用 Feng、插件及 Clang 自包含头文件 |
| macOS 目标 lib，显式指定 sysroot | 沿用用户指定的 SDK 路径 |
| macOS bin | 沿用现有 SDK 编译及最终链接路径 |
| macOS runtime C | 沿用原有 SDK，继续取得 pthread、stdio 等系统声明 |
| Linux bin/lib/runtime | 沿用各目标的 bundled sysroot，区分 GNU 与 musl |

SDK-free 指编译该库不需要系统 SDK 头文件；它不要求使用 C 的 freestanding 模式，
也不意味着 macOS 可执行文件无需 SDK 即可链接。

### 4.2 SDK-free 参数调整

变更前，macOS SDK-free lib 路径附加 `-ffreestanding` 和 `FENG_COMPILE_SDK_FREE`。
前者同时影响 builtin、`__STDC_HOSTED__`、默认 unwind tables 及 `main` 处理，不能
将移除它描述为只删除一个 LLVM 属性。参见 [Clang freestanding 说明](https://clang.llvm.org/docs/UsersManual.html#freestanding-builds)。

SDK-free 分支直接表达所需策略。正式参数以实验组合为起点，按 §7.1 排除冗余项；
主规范见 [构建规范 §2.2](../specifications/feng-build.md#22-目标平台与工具链转换)，选择依据如下：

1. 允许 hosted 模式下的标准库内建优化，不再由 Feng 自动添加 `-ffreestanding`
   或全局禁止 builtin 的参数；bin/lib 使用一致的 builtin 策略。
2. 保留 `FENG_COMPILE_SDK_FREE` 及现有自包含声明。正式实现使用 `-nostdlibinc`
   隔离系统头文件并保留所选 Clang 的 resource include，继续加入既有 Feng 和插件
   include。Clang 23.1.3 的 `-###` 已确认此选项只留下 resource headers；不需新增
   resource 路径查询或重复维护工具定位。历史实验使用的是 `-nostdinc` 加显式 include。
3. 保持原 SDK-free 路径的默认 unwind-table 策略，显式表达对应参数；保留
   `-fexceptions` 和已有异常插件处理，不能因 hosted 切换而改变异常清理行为。
   Clang 23.1.3 对现有 macOS ARM64 目标的 `-###` 对照确认，仅需
   `-fno-unwind-tables`；实验中的 `-fno-asynchronous-unwind-tables` 没有额外作用，
   不纳入正式参数。
4. 核对头文件依赖、宏及目标属性。实验中的 std 只依赖 22 个 Feng/插件/Clang 头文件，
   正式验证还需覆盖普通库和 Linux host 交叉编译 macOS 库。

实验用追加 `-fhosted` 覆盖原参数是隔离验证手段；正式实现应生成清晰的一组参数，
不把相互覆盖的实验参数串作为长期实现。上述变化对 ThinLTO 开关独立生效：即使
`--lto=off`，SDK-free lib 也允许内建优化。

保持当前浮点选项及 external 别名声明方式，不添加 `fast-math`、`-fno-math-errno`
或硬编码 sqrt 替换。由 LLVM 按实际声明、目标规则及证明完成合法优化，不能把
基准中的两个硬件开方点推广为所有 sqrt 输入都可无条件替换。

## 5. runtime 与二进制库分发

ThinLTO 编译阶段产生带摘要的 LLVM bitcode，最终链接按需导入函数体并优化；无需把
Feng 源码或 C 函数体放入 `.ft`、`.fb` 头文件。参见 [Clang ThinLTO](https://clang.llvm.org/docs/ThinLTO.html)。

拟保留原 `.a` 和 `.fb` 布局，将参与优化的归档成员由原生对象改为 ThinLTO bitcode。
目标平台信息仍随各自制品保存，不能把 bitcode 当作可跨目标平台复用的通用制品。
bitcode 不是附带原生机器码的双格式对象，本方案不使用 `-fembed-bitcode`，也不声称
任意不支持 LLVM bitcode 的原生链接器都能消费这样的归档。

Feng 继续使用既有 Clang、LLD、llvm-ar/ranlib 选择与定位规则。LLD 自带 bitcode
处理能力，无需新增 Feng 插件。混合原生与 bitcode 依赖时，只有可见的 bitcode
函数体参与跨模块优化；原生对象仍正常链接。参见 [LLD 的 LTO 支持](https://lld.llvm.org/)。

runtime 实施范围限定为 Feng 自身的 C 源文件，当前为 14 个，复用 Makefile 的
`RUNTIME_SRCS` 和各平台构建路径，不硬编码文件列表。使用 runtime 专用的编译参数，
避免把 ThinLTO 扩散到 Feng 编译器、LSP、测试程序及第三方库的全局 `CFLAGS`。
现有 libunwind 和 std 的第三方原生依赖保持原有构建方式，归档允许混合成员。

人工已确认：runtime 开启 ThinLTO，以单一格式随发行包分发，不因某个消费端 bin 的
`off` 改变，不额外发行原生副本或 embed-bitcode 双格式。其具体成员范围按上段限定。
开发构建、发行构建及测试必须验证实际使用的制品形态，
不能只检查编译命令含有 `-flto=thin`。

兼容方向按已讨论的要求采用“新 LLVM 读取旧 bitcode”，不要求旧 LLVM 读取新 bitcode。
这与 LLVM 的 bitcode 向后兼容政策一致，但不替代 Feng 的 `.ft`、ABI 或目标平台
兼容规则，也不保证旧调试元数据和旧性能表现完全保留。升级时仍需用既有库制品验证。
参见 [LLVM bitcode 兼容政策](https://llvm.org/docs/DeveloperPolicy.html#ir-backwards-compatibility)。

## 6. Debug 行为与边界

人工已确认：Debug 在 LTO 未指定时默认为 off，具体指当前项目及递归本地依赖以
`-O0 -g` 生成原生对象；显式值仍按 §3 处理。
主项目的函数体因此不进入 ThinLTO 的跨模块内联；已有 bitcode 库不会被内联进已经
编译为原生机器码的主项目函数。

最终链接仍可能对 runtime 和已发布的 bitcode 库进行优化与机器码生成。这意味着
Debug 构建仍有 bitcode 链接成本，不能宣传成“整个链接完全不运行 LTO”。也不应仅为
关闭当前项目 ThinLTO 而切换链接器或拒绝已有依赖。

| 调试对象 | 拟保留的行为和限制 |
| --- | --- |
| 当前 Debug bin | `-O0 -g`、稳定源码映射和 `.fd`；断点、步进、变量及异常栈必须回归 |
| 源码可用的本地依赖 | 随顶层 Debug 构建，产生并合并现有 `.fd`，支持依赖内源码调试 |
| 已发布的 Release `.fb` | 当前 `pack` 固定 Release，且不分发 `.fd`；不承诺与本地 Debug 依赖相同的源码调试体验 |
| runtime | 保持发行用优化制品；验证原有隐藏帧、异常展开与清理行为 |
| 显式 Debug `--lto=thin` | 保留 `-O0 -g` 和 `.fd`，需要独立测试，不能仅凭参数推断其调试效果与默认 Debug 完全相同 |

建议不把所有预编译依赖的链接优化强行降到 O0。库在之前 Release 编译中已经消除的
变量、结构或调试信息，也无法在消费端通过 O0 恢复。调试保证以现有
[CLI 调试规范](../specifications/feng-cli.md) 和 [构建调试规范](../specifications/feng-build.md)
为准，验证失败时先定位原因，不放宽既有断言来适配新行为。

## 7. 实施位置与验证矩阵

### 7.1 最小改动与必要性证据

每项改动必须对应已确认的要求，并说明现有实现缺少什么、为什么必须修改、为何不能
用更小的改动满足要求。依据可来自明确的代码调用链、实际编译参数与 IR，或受控对照
实验；不能仅凭猜测、顺手整理或未来可能有用而扩大变更。

- 新增或替换编译参数前，逐项核对现有默认值及实际产物。已符合要求的路径不改；
  例如 runtime 已允许 builtin 时，不为一致性再添加等效参数。
- 实验参数只作为候选。能证明移除某项仍满足目标及语义、SDK-free、调试等约束时，
  不纳入该项；必要性尚未证明时，先分析或实验，不直接进入正式变更。
- 优先复用现有模块、构建链路和测试入口。只补覆盖缺口；不得借优化重构无关模块、
  增加额外配置或改写已有测试。已有用例确需修改时仍须人工批准。
- Review 时逐项附上必要性依据和验证结果；无法证明必要的改动不保留。

以下是检查范围，不是必须修改的文件清单：

| 位置 | 拟处理事项 |
| --- | --- |
| `src/cli/compile/options.*`、`direct.c`、`driver.*` | 接收三态，在直编层统一解析实际值；生成 ThinLTO 与 SDK-free 参数 |
| `src/cli/project/` 与本地依赖构建调用链 | `build/pack/run` 向直编层透传三态，不在项目层解析默认值 |
| `Makefile` 的 runtime 构建规则 | 只对分发 runtime 添加 ThinLTO，保持既有 host/target、SDK 及工具选择 |
| 现有发行与安装验证入口 | 重建和核实 runtime、std、`.fb` 的归档形态及正常安装后的消费能力 |
| 现有测试体系 | 补充选项、混合链接、SDK-free、调试和语言行为覆盖；已有用例修改另行获得批准 |

不新增对 `local/` 的构建或测试依赖，不改变工具优先级，不新增临时包装脚本或测试
执行机制。正式实现采用已有模块与测试入口；如发现还需要更大改动，先列出事实与
最小修复，再提交人工决策。

### 7.2 必须覆盖的验证

| 维度 | 覆盖要求 |
| --- | --- |
| CLI | bin/lib × Debug/Release × 默认/off/thin；非法参数诊断；build/pack/run 及多层本地依赖保留三态直至直编层，结果与直接调用一致 |
| 制品与混合链接 | native bin + bitcode lib/runtime、bitcode bin + native lib、全部 bitcode、多包混合；检查真实归档成员与输出 |
| 模式切换 | 连续切换 off/thin、Debug/Release 后重建、重打包，不复用不匹配的旧对象或归档 |
| SDK-free | 无 SDK 的 macOS lib 编译、Linux host 交叉编译 macOS lib、显式 sysroot；检查实际头文件依赖 |
| 平台 | macOS ARM64，Linux x64/ARM64 的 GNU 与 musl；沿用已有支持矩阵，在对应运行环境验证行为 |
| 异常与生命周期 | 同包/跨包 throw、catch、重抛、defer、ARC、循环回收、初始化失败清理；验证两种现有插件处理后的 IR 仍正确 |
| 调用与身份 | 普通函数、callable、闭包、泛型、全局状态、私有依赖、多包符号；保持取址和相等语义 |
| C ABI 与数值 | external 别名及既有合法声明组合；正常浮点值、负值、NaN、无穷与有符号零，保留现行 C ABI 和浮点可观察行为 |
| 调试 | 主项目和本地依赖断点、next/stepIn/stepOut、变量、watch、调用栈、异常栈、`.fd` 指纹；覆盖预编译 bitcode 依赖 |
| 工具与发行 | 搬移安装、既有工具显式覆盖、所选工具失败诊断；新 LLVM 消费旧库，原生与 bitcode `.fb` 混合 |
| 成本与性能 | 编译及链接耗时、构建峰值内存、制品大小、运行耗时/RSS；检查实际 IR/汇编优化，而非只比较选项 |

编译器和链接结构检查放在 `test/`；纯 Feng 语言行为归入 `fcts/`，纯 std 行为沿用
`std/std_test`。需要 C 夹具的测试完整放在 `test/`，不把同一用例拆成两套入口。
保持完整 `make test`，不得用三个基准或一次成功链接替代全量回归。

## 8. 分步 Todo 与交付条件

### 阶段一 确认规范

- [x] 确认内部未指定/off/thin 三态、按构建模式解析默认值及最小改动要求。
- [x] 确认 build/pack/run 向直编层透传三态，由直编层统一解析实际值。
- [x] 确认 Debug 未指定时为 off，runtime 开启 ThinLTO 并以单一格式分发。
- [x] 将批准的 CLI 行为写入 CLI 主规范，将编译、链接、SDK-free 与调试规则写入
  构建主规范；包及发行文档只补充自身负责的制品约束，避免重复定义。

### 阶段二 最小实现与人工 Review

- [x] 按 §7.1 记录本轮代码改动必要性，排除等效参数及无关重构。
- [x] 实现项目及本地依赖向直编层透传 LTO 三态，由直编层统一解析实际值。
- [x] 调整 SDK-free 参数；正式头文件依赖与行为验证留在下一阶段。
- [x] 为 Feng runtime 构建规则启用 ThinLTO，保留原生 libunwind 成员。
- [x] 确认并适配发行脚本对 runtime bitcode 成员的校验方式，使用校验环境中的工具。
- [ ] 人工 Review 代码；通过后再补用例、重建制品和执行回归，不提前进入测试阶段。

### 阶段三 补齐测试、全量验证与性能验收

- [ ] 重建 runtime、std 及必要发行库。
- [ ] 按第 7 节补齐用例，核实插件产物、真实 bitcode 成员及混合链接。
- [ ] 验证默认 Debug 及显式 ThinLTO Debug，不改变现有调试契约。
- [ ] 在 Linux 容器及 macOS 完成全量 `make test`；macOS 回归在沙箱外运行，
  安全软件授权与正式计时分开，有中断则记录并重新完成受影响验证。
- [ ] 完成目标平台、SDK-free 交叉编译及正常发行安装矩阵；缺失环境明确记录，
  不把编译通过等同于目标平台运行通过。
- [ ] 用正式构建路径重跑三项五语言基准，保留全部样本、制品身份、IR/汇编及报告，
  写入 `benchmarks/results/`。
- [ ] 量化已知的链接时间成本，比较构建峰值内存、制品体积及运行 RSS；核实新增
  运行时成本并提交人工决策，不把已知的链接时间增加重复列为未决设计问题。
- [ ] 将 Review 决策和最终结果回填本文，提交建议 commit message，由人工提交。

验收以语义、异常清理及 Debug 回归通过为前提。性能目标按同机、同轮的三个基础用例
分别判断，不把本轮 0.786 / 0.266 / 0.092 秒作为所有机器的固定承诺；也不把三项通过
外推为所有应用无回退。正式配置下仍需保存 IR/汇编证据，确认收益来自通用后端优化。

## 9. 已确认决策与待验证事项

已确认 §3 的三态、项目命令透传及直编层默认值规则，以及 §7.1 的最小改动要求。
本次 Review 进一步确认：

1. **Debug：** LTO 未指定时默认为 off；具体编译和调试边界见 §6。
2. **runtime：** 开启 ThinLTO，以单一格式随发行包分发；成员范围与消费规则见 §5。
3. **链接时间：** 增加属于已知成本，正式验证时量化，不再作为未决设计问题。

§2.1 的 RSS 差值属于待核实的运行内存观测，需复测并定位来源；它与链接时间成本
是两个指标。构建峰值内存、制品体积和完整 Debug 行为同样保留为验证事项。
已确认的设计不因记录在本文而视为已实施或已通过回归。

## 10. 本轮实现与 Review 记录（2026-10-13）

本轮实现已确认的编译选项、参数链及必要的发行校验适配；测试、制品重建和性能验收尚未进行。

| 变更位置 | 必要性 |
| --- | --- |
| `cli.h`、`common.*` | 用一个枚举保留三态，用同一解析函数限定显式值，避免各命令独立解释默认值 |
| `compile/options.*`、`direct.c`、`driver.h` | 将 CLI 状态送到后端；不传该字段会丢失显式 off/thin |
| `compile/driver.c` | 在 bin/lib 共用参数函数中决定是否加 `-flto=thin`；SDK-free 的必要参数与排除项见 §4.2 |
| `project/build.c`、`run.c`、`pack.c`、`compile.c`、`common.h` | 沿原有内部调用链传递选项，pack 固定 Release 时仍保留显式值 |
| `deps/manager.*` | 让递归本地依赖使用同一 LTO 状态；已存在的二进制包不重编 |
| `main.c` | 现有帮助入口需显示四种编译入口支持的新选项 |
| `Makefile` | 仅平台 runtime 对象增加 ThinLTO；补充 Makefile 前置依赖，避免更新编译规则后继续使用旧的原生对象 |
| `release_component.sh`、`release_assemble.sh`、`release_verify_install.sh` | 现有 Mach-O/ELF 检查会拒绝 bitcode；使用本机工具读取原始 triple，保留平台检查 |
| `.github/workflows/release.yml` | 组装任务原本未安装 LLVM，复用既有安装步骤取得 `llvm-dis`；其余校验任务已具备该工具 |
| CLI、构建、包及发行主规范 | 分别定义选项、后端行为与自身负责的制品边界 |

静态检查：修改的 C 文件使用现有 Clang 23.1.3、macOS SDK 和原有警告等级通过
`-fsyntax-only`；三个发行校验脚本通过 `bash -n`，工作流通过 YAML 语法解析，
`git diff --check` 通过。`-###` 对照确认：删除 freestanding 后
默认新增同步 unwind tables，增加 `-fno-unwind-tables` 可恢复原策略；
`-nostdlibinc` 保留 resource headers 并排除隐式系统头路径。它们不能替代后续实际
编译、链接、语言行为、调试及性能回归。

发行校验的工具边界已按人工要求收敛：`llvm-dis` 使用本机／CI 安装版本，不加入
发行包，不修改裁剪清单或正常编译工具选择。规则统一见
[发行与安装 §4](feng-release-and-install.md#4-压缩包目录结构)。此前提出增加随包工具
并非必要，未实施该方案。

已用本机现有 LLVM 23.1.3 `llvm-dis` 只读检查此前实验的 `feng_array.o`，原始 triple
为 `arm64-apple-macosx27.0.0`。可行方式是由 `llvm-dis` 解码，再由发行脚本核对
triple；并非工具自身按 Feng 平台名作出判断。此检查不编译或执行制品，也不替代
后续运行验证。工具职责见 [LLVM 官方说明](https://llvm.org/docs/CommandGuide/llvm-dis.html)。
