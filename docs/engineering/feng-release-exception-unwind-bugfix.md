# 基于通用 LLVM 异常插件的 S11 修复方案

## 1. 状态与范围

状态：缺陷已确认，产品尚未修复；已完成实施前原型验证。本文为两步交付中的第二步，
须在通用插件独立交付确认后实施 Feng 接入，接入方案待人工 Review。

本问题在[异常元信息开发方案](./feng-callable-exception-effects-dev.md)阶段二的额外
release 验证中发现，原编号为 S11。2026-09-21 人工决定先独立交付 defer 阶段二，
将本问题移至本文单独处理；既有用例在非 release 模式下可以通过，继续正常执行，
不为隔离 release 缺陷而注释默认模式的有效覆盖。

S11 早于异常元信息阶段一，不是 defer 边界检查或本轮断点修复引入。
语言行为仍遵循[异常规范](../specifications/feng-exception.md)及
[defer 规范](../specifications/feng-defer.md)，本文不重新定义异常语义。

本次文档工作只拆分交付职责并保留问题、证据与方案，不实施插件或 S11 产品改造。
后续不得以某个表达式、具体 callee 或测试名称为条件增加特判；增加运行开销时须先由人工审定。

人工已明确：`setjmp/longjmp` 存在正常路径开销，且是 Feng 已弃用的异常方案。
本次 S11 修复不恢复该机制，也不将其列为候选方案。

人工已确认采用“通用 C 发码协议＋面向协议的 LLVM 异常处理插件”，并分两步独立交付：

| 步骤 | 主文档与交付范围 |
| --- | --- |
| 第一步：通用插件 | [插件开发文档](./c-ir-llvm-exception-plugin-dev.md)：协议、独立实现与测试、手工维护脚本、`third_party` 源码和 `toolchain` 预构建产物 |
| 第二步：S11 修复 | 本文：Feng Codegen、编译驱动、runtime 衔接、发行包接入与完整回归 |

通用协议只在插件文档定义，本文引用使用；不重复安排插件实现任务。普通构建与 CI
使用第一步已验证的预构建产物，不自动编译插件。第一步验收通过不等于 S11 已修复；
通用接口仍为待 Review 草案，也不能将此前 Feng 原型的通过结果计作独立插件验收。

2026-09-21 按人工要求补充实施前验证：在工程 `temp/` 内建立独立原型，验证 LLVM
接入、异常表解析、既有结构与函数签名下的异常交接，以及内联后的清理。原型可使用
runtime 源码副本，不修改产品代码或既有测试；结果须区分实际运行、交叉编译与尚未验证。
该次工作交付的是原型步骤及验证记录，不表示 S11 已完成修复；历史结果保留于 §5。

## 2. 已确认的现象与复现

### 2.1 原始 FCTS 用例

`fcts/fcts_bin/src/test_name_binding_scope.ff` 中的
`BIND10 catch body shadows its clause-header binding`：

- 默认构建下通过；完整 release 构建成功，但执行到该用例时终止。
- 日志为 `feng: panic: uncaught exception (unwind reason=5)`，进程信号为 6。
- 当时日志最后通过的是 `BIND10 infix match while body shadows its condition binding`。
- 按人工澄清，该既有用例及原断言保持启用；当前默认回归通过，后续还须验证 release。
- 这只是已确认触发点，不表示其他 release 异常路径已经安全；不能通过持续注释用例宣称 S11 已修复。

阶段二的 23 个 defer 行为用例、源码／FT 边界负例及 Debug／DAP 用例继续执行。
原 N08 与 landing 清理外抛的注释源码继续保留；它们由阶段二的编译期负例验证，
与本问题的合法异常被错误展开不同。

### 2.2 不依赖 std 的最小复现

将以下源码放在工程内 `temp/s11-release-exception/src/main.ff`：

```feng
module release_catch_audit;

@cdecl("libc", "exit") extern func quit(code: i32): void;
spec Callback(): void;

/** 抛出应由调用方捕获的字符串。 */
func fail(value: string): void { throw value; }

/** 保留原始用例的 callable 调用路径。 */
func invoke(callback: Callback): void { callback(); }

/** 成功捕获并完成 catch 局部遮蔽后正常退出。 */
func main(args: string[]): void {
  invoke(() {
    var observed = "";
    try fail("header") catch error: string {
      observed = error;
      let error = "body";
      observed = error;
    }
    if observed != "body" { quit(1); }
  });
}
```

同目录项目的 `feng.fm`：

```text
[package]
name: "release_catch_audit"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"
```

从仓库根目录分别运行：

```sh
build/bin/feng run temp/s11-release-exception
build/bin/feng run temp/s11-release-exception --release
```

期望两者均退出 0。已确认的 macOS arm64 结果是默认构建退出 0，release 报上述
uncaught 错误；直接执行故障二进制的退出码为 134。编译产物及执行文件均放在工程内。

### 2.3 历史版本对照

| 版本 | 验证方法 | 结果 |
| --- | --- | --- |
| 阶段一之前 `5b5b95a6`，即 `5ddcd4c4` 的父提交 | 隔离导出源码，使用相同工具链重编该版本编译器及 runtime；执行上述无 std 程序 | 默认退出 0，release 退出 134 |
| 阶段一 `5ddcd4c4` | 使用同一 runtime 对照执行 | release 同样退出 134 |
| 阶段二实现 | 相同最小源码及原始 FCTS release 验证 | 同样复现 |

阶段一提交没有修改 Codegen、异常 runtime 或宿主编译驱动。前两个历史版本为
最小程序生成的 release C 逐字节一致，SHA-256 为
`beefef7796f752ba1914e0cb191d3fecaea30fc242182323e88948b68ed9fd4c`。
尚未追溯更早的首次引入提交，不能据此认定缺陷始于某一次更早的异常功能变更。

本机调查证据（不是测试运行依赖）：

- `/private/tmp/feng-s11-pre-phase1-evidence-20260921.tgz`：历史编译器／runtime、源码、生成 C、二进制。
- `/private/tmp/feng-s11-pre-phase1-build.log`、`feng-s11-pre-phase1-debug.log`、
  `feng-s11-pre-phase1-release.log`、`feng-s11-phase1-release-control.log`：历史对照记录。
- `/private/tmp/feng-defer-stage2-fcts-release.log`：原始 FCTS release 失败记录。
- `/private/tmp/feng-defer-pre-regression-audits-20260921.tgz`：前期生成代码与隔离实验。

上述路径可能随本机清理消失；仓库中的原始用例及本文的独立复现是后续复验依据。

## 3. 原因与已经完成的隔离实验

### 3.1 优化器看不到异常后继

当前 `src/codegen/codegen.c` 生成普通 C 调用、GNU 标签地址和手动注册的 `FengLSDA`。
函数上的 CFI 标注指向空占位表；`src/runtime/feng_exception.c` 的 personality 使用
全局注册表寻找受保护地址区间，不读取后端生成的原生调用点异常表。

宿主 C 优化器没有得到“调用可以转入 catch／清理入口”的控制流信息，已观察到：

1. `-O2` 内联必抛函数后，try 区间尾标号不可达，地址被降为常数 `1`，导致匹配不到处理器。
2. 仅保住尾标号后，catch 入口参数准备仍可能被移到标号之前，展开器跳入时跳过必要计算。
3. 内联后的多个 Feng 函数边界共用一个原生帧，而当前 personality 按一个原生帧清理一个
   Feng 函数边界，逻辑边界与物理栈帧不再一一对应。

### 3.2 实验结论及限制

| 隔离实验 | 观察结果 | 结论 |
| --- | --- | --- |
| 空 `asm goto` 保留结束标号 | uncaught 转为 catch 上下文错误 | 只保住地址不足以修复 |
| 另为必抛 callee 禁用内联 | catch 上下文错误仍在 | 单点 `noinline` 不足以修复 |
| catch 所在函数禁用优化并保留必要帧 | 最小复现退出 0 | 仅用于隔离；未批准将整函数禁优化作为产品方案 |
| 显式暴露候选控制流边，保留优化与必要帧 | 最小复现及异常前局部值修改探针退出 0 | 尚未证明寄存器、内联、嵌套清理及所有优化组合正确 |

上述修改仅用于生成 C 的隔离实验，没有加入产品代码。`asm goto` 的普通控制流边
不能直接视为原生异常控制流；不能以几个探针通过替代通用正确性证明。

`make test` 的 std／FCTS 行为目标使用默认构建，生成程序为 `-O0`；显式 `--release`
才使用 `-O2`。编译器自身的构建优化级别与生成程序的优化级别不同。
此前默认全量回归通过，没有覆盖这组 release 行为，属于已确认的验证覆盖缺口。

## 4. 消费独立插件的 Feng 接入方案

### 4.1 单次 Clang 调用中的 LLVM Pass

使用第一步交付并验证过的通用 Pass 插件，在单次 Clang 调用中建立原生异常控制流。
插件的接入点、版本、构建和独立验收要求见[插件开发文档](./c-ir-llvm-exception-plugin-dev.md)，
Feng 按以下流程使用：

```text
Feng Codegen → 遵循通用异常协议的 C
            → Clang 生成 LLVM IR → 通用 EH Pass
            → 原有 LLVM 优化、机器码生成及链接
```

Feng 主编译器继续使用现有 C 实现，不链接 LLVM 库；插件由 Clang 加载，不进入最终
Feng 程序。保留原有优化级别，不以禁优化或禁内联修复 S11。本机原型的单次 Clang
接入及 `-O0/-O2/-O3` 结果见 §5，正式接入必须使用独立交付产物重新验证。

### 4.2 Feng 到通用协议的映射

接口与转换规则统一见[插件文档 §4–§5](./c-ir-llvm-exception-plugin-dev.md#4-接口草案)。
Feng 在 Codegen 的 callable／资源作用域抽象中维护区域，不从源码行号、表达式类别、
callee 名称或特定测试推测保护范围。具体接入如下：

| Feng 提供的信息或代码 | 通用协议中的用途 |
| --- | --- |
| `__feng_personality_v0` 函数符号 | 显式配置当前函数的 personality；插件不硬编码该名称 |
| callable／Scope 的嵌套及清理边界 | 区域、父区域、landing 入口及区域状态 |
| 具体 catch 的静态类型描述符地址，匿名 catch 的空指针 | 不透明的类型标识列表；由 Feng personality 解释 |
| catch 类型分派、源码 clause 序号、context 赋值及所有权 API 调用 | Feng 生成的处理器主体；插件只建立对应异常入口和 SSA 结果 |
| 已有 frame marker 及资源清理调用 | Feng 生成的 cleanup 主体；插件不插入或解释 ARC 操作 |

当前具体 catch 的闭合类型限制见[异常规范 §3.3](../specifications/feng-exception.md#33-catch)；
现有 Codegen 已将其描述符放入静态 catch 表，能够作为原生 landingpad 的常量类型条目。
共享体中的 throw 继续从现有泛型描述符获得实际载荷身份，不需要将开放泛参放入 catch 表。

Feng 生成代码包含通用协议头文件，不将标记加入 runtime API、`feng_runtime_contract.inc`
或 FT。插件和该头文件均不包含 Feng runtime 头文件，不读取 Feng 类型布局或 catch context，
也不新增通用运行时调度层。原型的 `__feng_eh_*` 标记和硬编码 personality 仅保留为实验
证据，正式实现以 Review 后的通用协议为准。

### 4.3 函数清理、嵌套区域与内联

每个已有函数清理边界的 callable 建立对应的根清理入口：调用现有
`feng_frame_release_to(&function_marker)`，再将原 landingpad 结果交给 `resume`。
因此，即使函数没有源码 catch，只持有资源并向上抛，也有正确的异常清理路径。
不为原本没有函数 marker 的纯 helper 或非外抛 defer helper 随意增加 marker。

嵌套传播遵循[协议 §4.3](./c-ir-llvm-exception-plugin-dev.md#43-异常结果与传播)。
Feng 生成本区域的类型分派和未匹配时的资源清理，再交给协议继续传播；同函数的外层
catch 必须仍有机会处理。LLVM 后续内联按原生 EH 规则组合这些控制流。

catch 主体的异常区域是其外层区域，不能再次匹配当前 catch；正常退出、return、break、
continue 与 catch 退出继续复用已有资源作用域退出逻辑，并同步更新区域状态。
类型匹配前的清理、匿名重抛以及 catch 中抛出新异常均走同一套机制。

异常记录与 selector 始终作为 landingpad 的 SSA 值保留。清理回调内部可以捕获另一条
异常，返回后继续传播的仍是原异常；不能重新从可被嵌套处理覆盖的 TLS 临时槽取回它。
原型已验证递归清理、内联后同一原生函数内的多个 Feng marker，以及这种嵌套捕获。

### 4.4 与现有 runtime 的明确衔接

`__feng_personality_v0` 改为读取 `_Unwind_GetLanguageSpecificData` 返回的原生异常表，
通过 `_Unwind_GetIPInfo`、区域起址、调用点区间、action 链和类型表选择处理动作。
继续使用 Feng exception class 和描述符身份匹配；不引入 C++ RTTI 或新的异常根。

- search 阶段只判断是否存在处理器，不修改 catch 所有权或弹清理链。
- cleanup 阶段向目标寄存器写入异常记录和原生 selector，再安装 landingpad。
  寄存器编号使用目标编译器的 `__builtin_eh_return_data_regno`，不硬编码某个 CPU 的编号。
- personality 不再按物理栈帧调用 `feng_cleanup_release_to_frame_marker()`。
  对应的逻辑 marker 由编译器生成的 cleanup 入口准确弹出一次。
- 外来 exception class 的处理范围沿用现有约束，不在 S11 中增加跨语言异常匹配能力。

保留 catch API 签名的具体方式已经验证：成功匹配到源码 catch 后，由生成代码写入
现有字段，再进入原 API：

```c
context.exception = exception_from_landingpad;
exception_from_landingpad->matched_clause = source_clause_index;
feng_exception_catch_begin(&context);
```

`feng_exception_catch_begin` 改为接收 `context.exception` 中已交付的记录，不再从
`g_pending_unwind` 接收。其“先建立异常所有权，再执行 try 资源清理，再加入活动 catch
栈”的顺序保持不变。`feng_caught_value`、`feng_caught_clause`、catch end 和 rethrow
继续使用现有 context 链；`matched_clause` 仍表示源码序号，不能直接填入原生 selector。
不匹配本层 catch 的清理路径不调用 catch begin/end，也不抢占异常所有权。

这改变了编译器与 runtime 的交接协议，但不需要增加字段或 API 参数。原型直接包含
当前 `feng_runtime.h`，使用 `feng_exception.c` 的独立副本完成了上述修改；产品文件未改动。

### 4.5 平台、布局与重编边界

当前五个 target 共用上述 lowering 和 personality 算法；已观察到的编码区别如下。
数值是本轮目标输出的验证结果，不应以 target 名称分支替代对编码字段的解析。

| Target | 展开信息 | 原型中有 catch 的类型表编码 |
| --- | --- | --- |
| macOS ARM64 | Mach-O，Compact Unwind／DWARF | `0x9b`：间接、PC 相对、有符号 4 字节 |
| Linux x64 GNU／musl | ELF，DWARF | `0x9b` |
| Linux ARM64 GNU／musl | ELF，DWARF | `0x9c`：间接、PC 相对、有符号 8 字节 |

上述目标的原型调用点表均使用 ULEB128，LPStart 省略；纯 cleanup 区域可以省略类型表。
decoder 须统一处理实际字段编码、符号扩展及间接地址，检查长度、偏移和整数溢出；
未支持的编码应明确失败。macOS 的 text-relative／data-relative 基址 API 不可用，
当前输出也未使用这两种编码，不能无条件引用这些 API 或猜测基址。
Windows 及 32 位目标不属于当前已交付范围，不在此次修复中扩展支持。

已验证可以保持对象、类型／泛型／spec 描述符、`FengUnwindException`、清理链、
`FengFrameMarker`、`FengCatchContext` 的布局，以及现有异常 API 签名和普通调用约定。
`FengLSDA` 声明和注册 API 可以保留，但新生成代码不再调用手动注册路径。
新后端不要求修改 FT 格式或版本。

所有相关本地产物、std、依赖包静态库和最终程序必须统一重编；旧对象采用的异常表及
交接协议与新 runtime 不兼容。仅重编最终入口或仅刷新 FT 不足以完成迁移。
清理和重建沿用已有构建／包流程，不通过临时提高 FT 版本代替原生产物重建。

### 4.6 构建、分发及既有测试接入

正式接入点包括 `src/cli/compile/driver.c` 的 bin 和 lib 两条宿主编译路径。
两者都加载同一 host 插件；debug／release、直接编译／项目构建／打包不能遗漏任何入口。
生成的 C 使用协议要求的 `-fexceptions` 等编译配置，不能只增加插件加载参数。
插件默认安静运行，原型的 `Feng EH lowered ...` 调查输出不进入产品默认 CLI 输出。

插件来源为第一步交付的 `toolchain/llvm-c-eh/<host>/`；Feng 的构建与测试只定位、校验并
使用其中的动态库和配套协议头文件。源码、SDK 和手工构建流程由插件文档维护；
不将源码构建插件设为 `make`、`make test` 或 CI 的前置任务。

预构建产物变化须使依赖它的生成 C／对象重新构建；沿用现有构建依赖和缓存失效机制，
空增量构建不得重写预构建产物。`scripts/release_assemble.sh` 目前仅复制对应 host 的
LLVM 目录，第二步须增加插件产物的安装路径、依赖闭包及签名校验，将已构建产物装入
发行包；发布流程不重新编译插件。覆盖 bin／lib、项目构建、包构建与发行包内的相对路径。

现有 `FENG_CC` 选择优先级仍可保留，但选中的编译器必须兼容配套插件。
不兼容时应在编译开始前给出明确错误，不回退到已知错误的普通 C 异常路径。
这对可覆盖工具链的兼容范围有实际影响，属于 §6 的明确 Review 项。

本地及 CI 已按 LLVM 22.1.8 校验 `clang` 与 `cc`，不要求二者指向同一文件。
版本统一本身不能替代插件加载兼容性验证。sanitizer 阶段继续使用具有完整 sanitizer
资源的宿主 Clang，并加载第一步验证过的同一 host 插件；不将已剪裁的 bundled Clang
当作完整 sanitizer SDK，也不在本次接入中重新设计工具链安装方式。

对既有测试的必要适配也已定位，实施前应一并获准，不能在回归失败后逐例绕过：

| 编号 | 当前代码与原因 | 拟进行的适配 |
| --- | --- | --- |
| M01 | `Makefile:test-sanitize` 用 `FENG_CC=$(CC)` 选择宿主 Clang；目前版本已统一为 22.1.8，但尚未接入插件 | 在现有 sanitizer 编译路径加载配套插件与协议头文件，保留宿主 sanitizer 资源、全部覆盖及断言 |
| M02 | `test/codegen/test_codegen.c`、`test/debug/test_debug.c`、`test/codegen/test_defer_generic_context.c` 直接调用 `cc` 编译生成 C | 统一经配套编译入口加载 Pass，保留 `-Werror`、目标平台与原有行为断言；旧 CFI／静态区域结构断言迁移到等价原生 IR／对象断言 |
| M03 | `test/runtime/test_nested_exception.c:nested_native_try` 手写旧 LSDA 协议，并以 `noinline, optnone` 隔离优化 | 夹具改用统一的原生 EH 标记协议并进入 Pass；保留记录身份、引用计数、析构顺序、线程／子进程等全部断言，加入优化构建验证 |

上述测试本轮均未修改。BIND10 与其他合法 Feng 源码用例保持原样；无需通过改写
throw/catch 语言用例来适配后端。原 N08 和已批准保留的非法清理源码注释不因此删除。

## 5. 历史原型验证结果与边界（2026-09-21 至 2026-09-22）

### 5.1 执行证据

基于工作区提交 `be58584d`、bundled LLVM 22.1.8，在 macOS 26.5.1 ARM64 上验证。
原型和运行产物位于 `temp/s11-native-eh/`；没有修改产品源码、既有测试或头文件布局。

以下结果使用的是原型的 Feng 标记和 personality。它们验证了 S11 的转换路径；
通用协议的版本配置、personality 参数、独立头文件和无 Feng 依赖的使用方验证尚未完成。

| 验证项 | 结果 |
| --- | --- |
| 同一最小 Feng 源码的当前编译器 release 产物 | 退出 134，仍为 uncaught exception |
| 为该源码的实际生成 C 副本接入标记协议和 runtime 副本 | `-O0/-O2/-O3` 均退出 0；保留原 lambda、string 载荷及 catch 局部遮蔽逻辑 |
| 扩展 C 协议原型 | 三种优化级别全部通过；覆盖具体／兜底 catch、异常前局部值、递归、嵌套清理、重抛、新异常替换、载荷析构次数、间接调用和独立编译的 producer／consumer |
| 真实内联 | `-O2` IR 中同一函数包含多个 Feng 函数 marker；清理计数仍正确；未加入 `noinline`／`optnone` 产品限制 |
| 匹配 LLVM 22 resource-dir 的 UBSan | 扩展原型与修改过的 runtime 源码副本使用 UBSan，执行退出 0，无 UBSan 诊断 |
| 五个 target 的对象生成 | 每个 target 的扩展原型、独立 producer、实际生成 C 副本和 runtime 副本共 20 个对象均生成成功 |
| 标记消除 | 对象无 `__feng_eh_*`、手动 LSDA 注册或空 CFI 表引用；IR 包含真实 `invoke/landingpad/resume` |

实际生成 C 的适配脚本只是本次最小复现的实验工具，不作为产品 Codegen 实现；正式实现
必须从作用域模型直接生成协议。独立 producer／consumer 验证的是原生调用及描述符传递，
不能替代 Feng FT／泛型共享体／完整包构建回归。

Linux 四个 target 本轮完成交叉编译和异常表检查，**没有实际运行验证**；本机容器服务
未启动。两个 Linux host 的插件构建／加载、GNU／musl 原生执行仍属于正式交付验收。
已读取 Linux Clang 的 ELF 导出表确认存在 PassBuilder 接口，但不把它计作插件运行通过。
本轮也未执行完整 `make test`、完整 release std／FCTS 或 Feng DAP 回归。

### 5.2 成本验证

使用独立翻译单元中的同一普通调用行为，交替执行旧手动区域路径与原生 EH 原型各
7 次；正常路径每次 2,000,000 次调用，原型异常路径每次 20,000 次调用。
未用错误的旧 release 异常执行结果作为性能比较基线。

| 指标 | 旧路径 | 原生 EH 原型 |
| --- | --- | --- |
| 普通调用中位数 | 5.290 ns／次 | 4.769 ns／次 |
| 普通调用最小／最大值 | 5.077／7.787 ns | 4.577／7.042 ns |
| 原型抛出／捕获中位数 | 不作比较 | 1,471.950 ns／次 |
| 同一测试函数的静态栈用量 | 192 字节 | 176 字节 |
| 对象 `__TEXT`／`__DATA` | 320／80 字节 | 292／0 字节 |

该小样本说明接入不必然增加正常路径查询、堆分配或强制禁内联成本；不代表所有程序
更快，也不是生产性能预算。标记完全消除、没有新增异常装箱／ARC 操作已通过原型确认。
真实泛型、复杂清理、跨包及各平台的保存寄存器、栈空间、代码体积和耗时仍须在集成后
测量；出现新增运行开销时按既有人工要求审定，不能据此样本自动批准。

### 5.3 先记录、再分析与解决的问题

| 问题 | 分析与处理 |
| --- | --- |
| macOS 不提供 text/data-relative 基址查询 | 从 SDK 与 vendored libunwind 确认限制；检查五平台输出后，统一解码实际使用的 4／8 字节 PC-relative 编码，未知编码明确失败 |
| LLVM 22 `eh.typeid.for` 创建失败 | 该 intrinsic 使用 `anyptr` 重载；显式传入指针类型后解决，不修改类型匹配语义 |
| 人工 C 入口残留前驱导致 PHI verifier 失败 | 在创建异常 PHI 前按普通入口与 landing 根清理不可达前驱，未以 `undef` 掩盖问题 |
| 手写探针普通路径意外落入 landing | 修正探针退出逻辑；正式协议必须拒绝普通边进入 landing 的输入 |
| UBSan 在 noreturn 标记后增加控制流 | 继续传播标记按终结指令切断普通后继，再统一清理死路径；没有匹配 UBSan 函数名或关闭 sanitizer |
| 启动阶段曾出现 SIGKILL／137 | 保留失败记录；独立沙箱外复验及最终对照全部通过，没有修改系统安全设置，未将原因未经验证地归到 EH 算法 |
| 完整 SDK 的 `llvm-config` 启动曾退出 137 | 本机使用已有匹配头文件成功构建插件；正式构建仍须检查 SDK／LLVM 配套信息，不依赖该次工具失败作为设计前提 |

主要证据为 `runtime-results.json`、`result-ubsan.log`、`cross-compile-final.log`、
`bench-results.json`、生成 IR／汇编和 `investigation.md`；失败批次单独保留。
本机归档为 `/private/tmp/feng-s11-native-eh-evidence-20260922.tgz`，包含原型源码、
构建脚本、产物和日志；所有二进制验证均从工程 `temp/` 执行，不从归档目录执行。
临时证据用于 Review，不成为测试运行依赖；正式回归必须在仓库测试中重新建立。

## 6. 实施与验收 Todo

单次 Clang 内的 Pass 接入及保留既有结构／API 的交接方式已经通过原型验证。
以下只安排 Feng 接入，按依赖顺序实施；正式修复仍须完成所有未勾选项。
插件自身的问题回到其主文档记录、分析、修复并重新预构建，不在 Feng 侧增加特判。

### 6.1 Review 边界

- [x] 人工确认通用 C 发码协议、面向协议的 LLVM 插件，以及 Feng 作为使用方的分层方向。
- [x] 人工确认先独立交付插件，再接入 Feng；普通构建与 CI 使用手工预构建产物。
- [ ] 确认[插件独立交付](./c-ir-llvm-exception-plugin-dev.md#9-实施与交付-todo)已经完成，
      协议与配套产物固定；未完成时不开始第二步产品改造。
- [ ] 确认 Feng 工具链兼容性要求：`FENG_CC` 保留选择优先级，但不兼容配套插件的工具链明确报错。
- [ ] 批准 §4.6 的 M01–M03 既有测试／工具链适配；原行为断言和性能门槛不降低。
- [ ] 确认按已验证协议调整 runtime 内部实现；保持现有结构、API 签名和 FT 格式，统一重编。

### 6.2 按依赖顺序实施

- [x] 完成缺陷、历史对照、原型方案、平台编码及初步成本记录。
- [x] 验证现有 C → 优化前 Pass → 原生 EH 的通路，以及原生成 C 的 release 修复效果。
- [ ] P01：Feng 主规范的实现／构建边界引用已交付的协议并记录工具链要求；不重复定义协议，
      不改变语言异常语义。
- [ ] P02：核对已交付的三个 host 插件、协议头文件及构建元信息，确定 Feng 加载入口和
      兼容性检查；使用现有 bundled／sanitizer 宿主 Clang 验证实际编译路径。
- [ ] P03：在 Feng Codegen 的 callable／Scope 抽象中接入区域模型和协议；覆盖普通函数、
      方法、Lambda、泛型共享体及含 catch 的 defer helper，不对某类表达式单独修补。
- [ ] P04：实现有边界检查的原生 LSDA decoder、personality 及显式 context 交接；保留现有
      catch 生命周期和未捕获终止行为，移除新路径对物理帧清理和 TLS pending 交接的依赖。
- [ ] P05：同时接入 bin／lib 编译、预构建插件的发行包组装、工具兼容性检查和增量依赖；
      测试使用现有同版本宿主 sanitizer 资源。完成 std、依赖包与最终程序的统一重建。
- [ ] P06：按批准范围迁移 M01–M03，建立共享的测试编译入口；不得通过保留禁优化夹具、
      跳过 Pass、删除合法用例或降低断言来取得回归成功。

### 6.3 覆盖及交付

- [ ] 使用第一步独立交付的预构建插件完成下述 Feng 验收；独立协议测试与历史原型均不能代替。
- [ ] 编译器测试验证 Feng 发码到协议的映射：作用域嵌套、合流／循环／提前退出、必抛与
      条件抛、间接调用、`noreturn`／`nounwind`、内联后的 selector、同函数父 catch，
      并检查最终 IR／对象的控制流与标记消除；协议自身的校验用例由第一步维护。
- [ ] runtime 测试覆盖原记录身份、引用计数、嵌套 catch、清理中内部捕获、重抛、新异常替换、
      未捕获清理／终止、线程隔离及一次性析构；异常表解码增加 4／8 字节和错误输入验证。
- [ ] BIND10 及本文最小复现保持原源码／断言，纳入自动默认／release 对照；FCTS 覆盖各合法
      载荷、泛型共享体、类型／方法泛参、跨包、defer 顺序、各种退出和析构时序。
- [ ] 源码隐藏后的 FT producer／consumer 在默认／release 下运行，验证静态库与共享体重新
      构建完整；保持 FT 格式／版本和函数参数 ABI 不变。
- [ ] 运行 LSP／DAP、源码停点与变量读取回归；异常后端不得引入编辑器语义或提示噪音。
- [ ] 三个 host 的 Feng 编译路径及发行包均能加载已交付插件；五个 target 完成 Feng 程序
      原生执行验证，交叉编译不能代替执行。
- [ ] 扩展性能矩阵，记录正常／异常路径、复杂清理、泛型和跨包的时间、栈空间及代码体积；
      发现新增运行成本先提交人工决策，再继续产品方案。
- [ ] 在沙箱外完成 `make test`，另外显式运行 `feng run std/std_test --release` 和
      `feng run fcts/fcts_bin --release`；确认生成程序使用 release，不只看编译器自身优化级别。
- [ ] 回填平台、构建、性能和全量结果，输出英文 commit message，等待人工 Review，不自动提交。

参考：[LLVM 异常处理](https://llvm.org/docs/ExceptionHandling.html)、
[Pass 管线接入](https://llvm.org/docs/NewPassManager.html#inserting-passes-into-default-pipelines)、
[Clang Pass 插件参数](https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-fpass-plugin)、
[原生展开接口](https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html)。
