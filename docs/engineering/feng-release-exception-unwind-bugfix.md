# 基于通用 LLVM 异常插件的 S11 修复方案

## 1. 状态与范围

状态：2026-09-24 人工批准开始实施。独立插件、driver／测试／发行接入及 macOS
测试专用补丁 LLD 已提交，接入结果见[工具链接入方案](./feng-llvm-c-eh-integration-dev.md)。
本阶段实施本文的 Codegen／runtime 修复；既有测试按 §4.6 迁移，保留行为和所有权断言。
Codegen／runtime 实现、本机完整 `make test` 及显式 release std／FCTS 已通过。
运行成本 S31 尚待人工审定，原生 x64 CI 尚未执行，暂不标记整体交付完成；
实际结果见 §7，不能以工具加载或历史原型通过代替。

本问题在[异常元信息开发方案](./feng-callable-exception-effects-dev.md)阶段二的额外
release 验证中发现，原编号为 S11。2026-09-21 人工决定先独立交付 defer 阶段二，
将本问题移至本文单独处理；既有用例在非 release 模式下可以通过，继续正常执行，
不为隔离 release 缺陷而注释默认模式的有效覆盖。

S11 早于异常元信息阶段一，不是 defer 边界检查或本轮断点修复引入。
语言行为仍遵循[异常规范](../specifications/feng-exception.md)及
[defer 规范](../specifications/feng-defer.md)，本文不重新定义异常语义。

本次实施不改变语言异常语义，也不扩展为 sanitizer 默认配置调整。
不得以某个表达式、具体 callee 或测试名称为条件增加特判；增加运行开销时须先由人工审定。

人工已明确：`setjmp/longjmp` 存在正常路径开销，且是 Feng 已弃用的异常方案。
本次 S11 修复不恢复该机制，也不将其列为候选方案。

人工已确认采用“通用 C 发码协议＋面向协议的 LLVM 异常处理插件”。2026-09-24
独立工具提交后，按用户提出的顺序，将原两步计划细分为以下三个阶段，接入细节单独 Review：

| 步骤 | 主文档与交付范围 |
| --- | --- |
| 第一步：独立工具 | [插件开发文档](./c-ir-llvm-exception-plugin-dev.md)：协议、独立实现与测试、插件及测试 LLD 的手工维护脚本与预构建产物 |
| 第二步：工具链接入 | [接入开发文档](./feng-llvm-c-eh-integration-dev.md)：driver、开发／发行布局、测试编译入口及 macOS UBSan 链接器 |
| 第三步：S11 修复 | 本文：Feng Codegen、原生异常表及 runtime 衔接、统一重建与完整 release 回归 |

通用协议只在插件文档定义，本文引用使用；工具链接入只在接入文档定义。普通构建与 CI
消费预构建产物，不自动编译插件。工具提交和加载验收均不等于 S11 已修复；
此前 Feng 原型的通过结果不能替代正式插件下的 Feng 验收。

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
- 按人工澄清，该既有用例及原断言始终保持启用；修复后的默认和显式 release 结果见 §7.2。
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

上述源码的预期是抛出 `"header"` 后进入 string catch，执行结束时 `observed` 为
`"body"`，程序退出 0。已验证的 macOS ARM64 对照如下，修复后的正式验收见 §7.2：

| 相同源码的构建／执行方式 | 修复前 | 修复后 |
| --- | --- | --- |
| 默认构建，生成程序为 `-O0` | 正常捕获，退出 0 | 正常捕获，退出 0 |
| `--release`，生成程序为 `-O2` | 编译成功；执行时报 `uncaught exception (unwind reason=5)`，收到 SIGABRT，直接执行故障二进制退出 134 | 正常捕获，退出 0 |

复验必须实际执行生成程序；只验证 release 编译成功不能验证异常捕获行为。
此复现不依赖 std，也不要求开启 UBSan／ASan。示例保留了原始路径中的 Lambda、
callable 调用和 catch 变量遮蔽，但没有证明这些写法全部都是触发缺陷的必要条件。
它是普通合法源码；根因是旧后端没有向宿主优化器完整表达异常控制流，具体变化见 §3.1，
不能把问题范围限定为某一种源码写法。

在已修复版本执行上述命令应当通过。要观察修复前的失败，须按 §2.3 使用旧编译器与其
配套 runtime，在隔离目录内统一重建，不能混用旧对象和新 runtime。编译与执行产物
均放在工程内。

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

修复前 `src/codegen/codegen.c` 生成普通 C 调用、GNU 标签地址和手动注册的 `FengLSDA`。
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

### 3.3 为什么此前未被回归发现，以及当前覆盖边界

2026-09-25 核对 Git 后确认：原始 BIND10 用例已在 `22b479eb`（2026-09-02）加入。
该提交和阶段一之前 `5b5b95a6` 的 Makefile 均以 `feng run ./fcts/fcts_bin` 执行完整
FCTS，没有 `--release`，std 套件也使用默认构建。用例引入日期不等于缺陷引入日期；
目前能确认缺陷早于阶段一，更早的首次引入点仍未确定。

此前遗漏的关键是构建模式，而不是缺少这段普通异常源码：

- Feng 编译器自身以 C `-O2` 构建，与它为 Feng 程序选择的优化级别是两回事。
- 默认 `feng run` 为生成程序选择 `-O0`，本例能够通过；`test-normal` 也不表示完整
  std／FCTS 会以 release 模式运行。
- 阶段二额外执行完整 FCTS 的 `--release` 后，才发现优化后捕获失败。因此此前
  `make test` 通过不能证明这组 release 异常行为正确，这是已确认的覆盖缺口。

截至本次记录，S11 默认／release 最小复现、隐藏源码的四组包优化组合已经加入
`test/cli/native_exception.inc`，由 `test_cli` 纳入 `make test`。完整 std／FCTS 的
release 也已单独执行并通过，结果见 §7.2；但 Makefile 的 `std-tests`／`fcts-tests`
目标仍只执行默认模式，不能把单独完成的 release 验收写成全量入口已经自动覆盖。
将完整 release 套件固定纳入后续回归流程仍是待决定事项，本次记录不更改测试入口。

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

接口与转换规则统一见[插件文档 §4–§5](./c-ir-llvm-exception-plugin-dev.md#4-协议接口)。
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

这改变了编译器与 runtime 的交接协议，但不需要增加字段或 API 参数。历史原型直接包含
当时的 `feng_runtime.h`，使用 `feng_exception.c` 的独立副本验证；本次正式实现使用
同一交接方式，保持结构布局及 API 签名。

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

原生展开 API 不提供整张 LSDA 的长度；产品解析依赖加载器提供的有效原生表地址，并
检查表内声明的 call-site／类型表边界、action 偏移、循环和整数溢出。独立 decoder
测试额外传入完整缓冲区边界以验证截断输入；间接类型引用仍由加载器重定位保证可读。

已验证可以保持对象、类型／泛型／spec 描述符、`FengUnwindException`、清理链、
`FengFrameMarker`、`FengCatchContext` 的布局，以及现有异常 API 签名和普通调用约定。
`FengLSDA` 声明和注册 API 保留，但新生成代码不再调用手动注册路径；旧的非空注册
明确要求统一重编，不静默接受无法被新 personality 使用的旧异常表。
新后端不要求修改 FT 格式或版本。

所有相关本地产物、std、依赖包静态库和最终程序必须统一重编；旧对象采用的异常表及
交接协议与新 runtime 不兼容。仅重编最终入口或仅刷新 FT 不足以完成迁移。
清理和重建沿用已有构建／包流程，不通过临时提高 FT 版本代替原生产物重建。

### 4.6 前置接入与本阶段测试迁移

driver 的 bin／lib、开发与发行布局、编译器兼容性、普通 LLD 和 macOS UBSan
测试链接器均先按[接入文档](./feng-llvm-c-eh-integration-dev.md)实施并验收。
本文不再重复定义这些任务；S11 直接使用该阶段已验证的实际编译通路。
插件保持安静，原型调查输出不进入产品默认 CLI 输出。

原 M01–M03 的任务按依赖拆分，保留编号供既有讨论追溯：

| 编号 | 前置接入阶段 | S11 阶段 |
| --- | --- | --- |
| M01 | Makefile 的 sanitizer 编译路径加载插件，显式选择配套测试链接器 | 使用同一通路验证新生成的原生异常协议；保持 sanitizer 检查及覆盖 |
| M02 | `test/codegen/test_codegen.c`、`test/debug/test_debug.c`、`test/codegen/test_defer_generic_context.c` 的直接 C 编译统一加载插件，保留现有发码断言 | 将旧 CFI／静态区域结构断言迁移为等价原生 IR／对象断言；保留 `-Werror`、目标与行为断言 |
| M03 | 手写旧 LSDA 的 runtime 夹具继续按原协议执行 | `test/runtime/test_nested_exception.c:nested_native_try` 改用原生 EH 协议，保留记录身份、引用计数、析构、线程／子进程断言，并加入优化构建验证 |

2026-09-24 人工明确批准 S11 阶段的上述断言与夹具迁移。该授权保留全部行为、
所有权、析构和性能断言；只因 S11 暂停的合法用例须恢复，原 N08 的非法源码不属于此范围。
新 Codegen 与 runtime 上线后，按 §4.5 统一重建 std、依赖包静态库与最终程序。
BIND10 与其他合法 Feng 源码用例保持原样；原 N08 和已批准保留的非法清理源码
注释不因此删除。

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
以下只安排前置工具链接入完成后的 S11 改造；正式修复仍须完成所有未勾选项。
插件自身的问题回到其主文档记录、分析、修复并重新预构建，不在 Feng 侧增加特判。

### 6.1 Review 边界

- [x] 人工确认通用 C 发码协议、面向协议的 LLVM 插件，以及 Feng 作为使用方的分层方向。
- [x] 人工确认先独立交付插件，再接入 Feng；普通构建与 CI 使用手工预构建产物。
- [x] 确认[插件独立交付](./c-ir-llvm-exception-plugin-dev.md#9-实施与交付-todo)已提交，
      使用固定的协议与配套产物；原生平台的未验证边界继续单独记录。
- [x] 确认[工具链接入阶段](./feng-llvm-c-eh-integration-dev.md)已获人工验收并提交，
      本阶段直接消费其已提交工具定位和构建流程。
- [x] 批准 §4.6 的 S11 阶段断言／夹具迁移；原行为断言和性能门槛不降低。
- [x] 确认按已验证协议调整 runtime 内部实现；保持现有结构、API 签名和 FT 格式，统一重编。

### 6.2 按依赖顺序实施

- [x] 完成缺陷、历史对照、原型方案、平台编码及初步成本记录。
- [x] 验证现有 C → 优化前 Pass → 原生 EH 的通路，以及原生成 C 的 release 修复效果。
- [x] P01：Feng 主规范的实现／构建边界引用已交付的协议并记录工具链要求；不重复定义协议，
      不改变语言异常语义。
- [x] P02：核对前置接入阶段的产物与验收记录，使用其实际编译通路验证本次新增的
      Feng 协议发码；不重复实现工具定位、发行布局或测试链接器选择。
- [x] P03：在 Feng Codegen 的 callable／Scope 抽象中接入区域模型和协议；覆盖普通函数、
      方法、Lambda、泛型共享体及含 catch 的 defer helper，不对某类表达式单独修补。
- [x] P04：实现有边界检查的原生 LSDA decoder、personality 及显式 context 交接；保留现有
      catch 生命周期和未捕获终止行为，移除新路径对物理帧清理和 TLS pending 交接的依赖。
- [x] P05：确认 bin／lib、发行包及 sanitizer 通路正确消费新协议；按 §4.5 完成
      runtime、std、依赖包与最终程序的统一重建，验证未混用旧原生产物。
- [x] P06：按批准范围完成 §4.6 的 S11 测试迁移；不得通过保留禁优化夹具、
      跳过 Pass、删除合法用例或降低断言来取得回归成功。

### 6.3 覆盖及交付

- [x] 本轮验证均使用第一步独立交付的预构建插件；独立协议测试与历史原型不计作 Feng 验收。
- [x] 编译器测试验证 Feng 发码到协议的映射：作用域嵌套、合流／循环／提前退出、必抛与
      条件抛、间接调用、`noreturn`／`nounwind`、内联后的 selector、同函数父 catch，
      并检查最终 IR／对象的控制流与标记消除；协议自身的校验用例由第一步维护。
- [x] runtime 测试覆盖原记录身份、引用计数、嵌套 catch、清理中内部捕获、重抛、新异常替换、
      未捕获清理／终止、线程隔离及一次性析构；异常表解码增加 4／8 字节和错误输入验证。
- [x] BIND10 及本文最小复现保持原源码／断言，纳入自动默认／release 对照；FCTS 覆盖各合法
      载荷、泛型共享体、类型／方法泛参、跨包、defer 顺序、各种退出和析构时序。
- [x] 源码隐藏后的 FT producer／consumer 在默认／release 下运行，验证静态库与共享体重新
      构建完整；保持 FT 格式／版本和函数参数 ABI 不变。
- [x] 运行 LSP／DAP、源码停点与变量读取回归；异常后端不得引入编辑器语义或提示噪音。
- [ ] 三个 host 的 Feng 编译路径及发行包均能加载已交付插件；五个 target 完成 Feng 程序
      原生执行验证，交叉编译不能代替执行。本地执行结果见 §7.2；x64 Rosetta 不计作
      原生 x64 验收，原生 x64 CI 及本次变更的远端发行验证留待提交后执行。
- [x] 扩展性能矩阵，记录正常／异常路径、复杂清理、泛型和跨包的时间、栈空间及代码体积，
      实测结果及限制见 §7.1。
- [ ] 人工审定 S31 的新增运行成本；审定前不标记最终交付，不私自扩展性能优化方案。
- [x] 在沙箱外完成 `make test`，另外显式运行 `feng run std/std_test --release` 和
      `feng run fcts/fcts_bin --release`；确认生成程序使用 release，不只看编译器自身优化级别。
- [ ] 回填平台、构建、性能和全量结果，输出英文 commit message，等待人工 Review，不自动提交。

## 7. 本次实施问题与验证记录

先记录问题，再分析并解决；未确定的范围、方案或新增运行成本由人工决策。

| 编号 | 现象、依据与处理状态 |
| --- | --- |
| S12 | 旧手写 LSDA 的 `test/runtime/test_nested_exception.c:nested_native_try` 在 macOS LLVM 22.1.8、补丁 LLD、ASan＋UBSan 下向 catch begin 传错 context；仅 UBSan、仅 ASan 的同一 runtime 测试通过。LLDB 确认原 context 和清理链完整，但 x19 已被复用为 body 函数地址，landing 仍将它当作 context。IR 仅有人工保活普通前驱，没有来自 body 调用的异常边。属于 S11 的同类异常控制流缺失，按 M03 迁移后专项回归，不用关闭 sanitizer、禁内联或禁优化修补。 |
| S13 | 当前 `make test` 在 macOS 和 Linux 实际均仅启用 UBSan；Makefile 的 Linux ASan 注释不能作为已执行证据。本阶段补充 S11 的 ASan＋UBSan 专项验证，默认 sanitizer 配置不在本次变更范围。 |
| S14 | 首次编译发现目标 `<unwind.h>` 将 `_Unwind_GetLanguageSpecificData` 声明为返回 `uintptr_t`。decoder 接收字节指针，调用边界需显式转换；不更改展开 ABI 或按平台特判。 |
| S15 | 既有 `test/codegen/test_nested_exception.c` 断言生成 C 完全不含 `feng_frame_release_to`，对应旧 personality 的物理帧清理。新后端需在异常 cleanup 中显式调用该 API，故按已批准的 M02 迁移该结构断言；原正常 return／break／continue 的清理次数、顺序和绑定所有权断言保留。 |
| S16 | 首轮 Codegen 回归在既有 `_obj1` 名称断言失败：根清理实现改变了函数 marker 的临时名分配方式，令后续临时名移位。恢复原 `cg_fresh_temp` 分配顺序，避免扩散到无关发码和测试。 |
| S17 | 新 decoder 夹具的 function-relative 类型键恰好等于函数基址，编码成 0；按原生约定它表示空指针而非该类型。修正新夹具为非零相对地址，并保留正／负偏移及空指针独立检查；不是修改 decoder 来适配错误输入。 |
| S18 | 既有整数、tuple 零成本测试扫描整个函数的 `if (`／`goto`，误计了新协议的编译期保活分支。按既有整数测试的边界“函数帧序言不属于被测操作”，从正常函数体入口检查对应发码；新增原生 IR 检查另行验证协议和保活分支已消除，原求值次数、分配和分支要求不变。 |
| S19 | 新 IR 夹具将公开函数的 callable 参数声明为私有 spec，先被既有 AE0327 可见性检查拒绝。修正新夹具的 spec 可见性后再进入发码验证，不改语义规则。 |
| S20 | 新 O1／O2／O3 及 sanitizer IR 矩阵被插件拒绝：Clang 将 catch 内提前 return 与正常路径合并到内部 lifetime 清理块，区域数据流合流为 0／函数区域。原发码只在 try 边界切换状态不足以表达该续接路径。由统一表达式、语句与 scope 清理入口重申词法区域，覆盖宿主隐式合流，不按表达式种类或 sanitizer 特判，也不放宽插件校验。 |
| S21 | 新 FCTS 文件的三处循环初始化漏写 `var`，断言遗漏必传消息参数及 `std.numeric` 的 Display 满足关系导入，依次被既有诊断拒绝。修正新增用例；不修改语言规则或既有用例。 |
| S22 | 既有 `test/cli/llvm_c_eh.sh` 用空 Feng 程序比较加载／不加载插件的 IR 完全一致。这是前置接入阶段“尚未迁移 Feng EH”的对照；S11 后 Feng 已发出协议，不能再作为无协议输入。保留 bin／lib、重定位、失败诊断和全部独立无协议对照；该段新增 Feng 协议消除与原生 EH 检查，IR／汇编等值比较改用纯 C 无协议输入。2026-09-24 人工已批准此测试迁移。 |
| S23 | 完整 FCTS 发码发现独立 union projection helper 带入了外层函数的区域编号，被插件以 missing configure 拒绝。该 helper 有独立 Scope，但首次实现将根区域放在 CG 的当前函数字段中。将区域和协议启用状态统一归属词法 Scope 的根；嵌套生成 helper 自然隔离，不按 helper 名称跳过、不新增运行时 marker。 |
| S24 | 区域状态改由 Scope 持有后的首次 FCTS 构建出现编译器崩溃，LLDB 定位到序言写入空 Scope。普通 finalizer 的 Scope 原本在序言之后才创建；调整为与其他 callable 一致，先创建根 Scope 再发序言，并在统一序言入口验证此前提。未改变 finalizer 的执行或清理顺序。 |
| S25 | 完整 std release 首次构建在 `FengTokenTransformer.findDeclarationEnd` 被插件以 ambiguous region 拒绝。优化前 IR 确认 return／continue 的 lifetime 清理合流传播到循环回边，循环条件调用之前缺少区域声明。所有循环头统一在迭代入口恢复所属 Scope，语句入口也恢复区域以覆盖 continue 到 update 的续接；新增含 return／continue 的循环 IR 矩阵，不关闭优化、不放宽插件状态检查。 |
| S26 | 新 CLI 最小复现曾被系统以 SIGKILL 结束，磁盘签名验证通过。同一二进制原字节复制到新路径后正常退出 0；不同优化配置使用独立路径后首个默认产物仍复现，因此“覆盖已执行文件”不足以解释。系统日志未确认拒绝原因，不能归因于 EH、签名缓存或某个安全组件。最终沙箱外完整 `make test` 的 UBSan、普通两阶段均在原 `temp/` 目录直接通过这两组新增用例；未迁移目录、未加复制／重试、未更改 driver 或系统安全设置。保留早期失败记录，不再需要为本次验收迁移执行目录。 |
| S27 | Linux 两个容器的 std release 各有 12 个 Unicode／TUI 宽度断言失败，其余 595 个通过；容器启动环境未设置 CI 的 UTF-8 locale。改为 CI 的 `LANG=C.UTF-8 LC_ALL=C.UTF-8` 后两个 host 的 607 个用例全部通过，未调整用例。 |
| S28 | 两个 Linux 容器并行构建不同平台时，项目共用 `build/pkg/*.fb` 被另一平台覆盖，导致依赖包缺少目标静态库。专项验收改用各 host 独立的项目源码副本；每个副本按 GNU、musl 顺序构建，不修改 Feng 的包或缓存逻辑。 |
| S29 | 新增 ASan＋UBSan、`-fno-sanitize-recover=all` 的真实 Feng 程序矩阵在泛型共享体被插件拒绝：configure 不在 entry block。泛型描述符恢复位于原 EH 序言前，sanitizer 为其读操作插入分支。协议配置须在统一 callable C 主体入口生成，逻辑 frame 仍按原顺序建立；不改变插件规则或关闭 sanitizer 检查。 |
| S30 | Debug 单测以旧 CFI 的 `#if !defined(_WIN32)` 作为 lambda 前缀结束位置，新后端不再生成该文本。按已批准的 M02 改用 frame 声明边界，保留十个 lambda、前缀行数以及逐行源码文件／行号断言；不改断点预期。 |
| S31 | 与已提交版本 `0575d00d` 的隔离构建比较，原生 EH 存在部分运行成本，不能宣称所有开销不变。下表和栈报告已实测；按 §6.3 及 AGENTS.md，成本是否接受待人工决定，不私自扩大到异常元信息驱动的额外优化。 |
| S32 | 最终 Linux 复验重建了隔离项目目录，但准备命令只复制 `src` 和 `feng.fm`，漏掉 std manifest 声明的资源目录；项目检查在进入发码前拒绝缺失资源。按原 manifest 补齐临时项目的资源，再原样运行用例，不更改产品配置或断言。 |
| S33 | 最终 Linux ARM64 release 复验中，bundled Clang 的前端进程被 Killed。内核日志确认 `CONSTRAINT_MEMCG`／`oom_kill=1`：1 GB 限额内 Feng 约 216 MB、Clang 约 798 MB，合计触及容器上限。std 的 607 项此前已通过；使用已有本地镜像建立临时 2 GB 容器后，GNU／musl FCTS 均为 1516／1516，musl std 为 607／607。保持相同源码、参数和断言，未调整产品、CI 配置或关闭优化。 |

### 7.1 正常与异常路径成本（待人工 Review）

同机 LLVM 22.1.8，原编译器和 runtime 从 `0575d00d` 导出重编；相同 Feng 源码、
原优化选项、相同结果断言，一次预热后轮换顺序测七次，以下为中位数（毫秒）。
这是微基准结果，不代表所有实际应用；旧 release 异常路径有 S11，未拿错误结果作基线。

| 场景／循环次数 | 旧默认 | 新默认 | 旧 release | 新 release |
| --- | ---: | ---: | ---: | ---: |
| 普通调用／2000 万 | 106.360 | 119.159 | 49.575 | 50.493 |
| try 正常返回／2000 万 | 150.672 | 152.057 | 98.703 | 90.764 |
| 资源＋defer／200 万 | 81.274 | 82.635 | 74.290 | 71.986 |
| 泛型正常返回／1000 万 | 109.179 | 109.959 | 74.428 | 72.409 |
| 每轮抛出并捕获／20 万 | 419.743 | 600.927 | 不作为正确基线 | 326.786 |
| 跨包静态库 try 正常返回／1000 万 | 未测 | 未测 | 56.908 | 53.617 |

Clang `-fstack-usage` 显示，默认构建的普通被调函数从 144 增至 160 字节，
泛型共享体从 288 增至 336 字节；部分 release finalizer 从 80 增至 96 字节。
另一方面，优化后的 try 主调用栈从 `main 64＋Feng main 352` 变为内联后的
`main 288`；泛型样本从 `64＋368` 变为 `304`。内联策略未受禁用。

原生 EH 的异常值保存、寄存器恢复和 cleanup／resume 会产生实际成本；
编译期协议调用及人工保活的条件分支已经消除，try 正常路径不新增协议查询、堆分配或 ARC。
这不表示 O0 正常路径没有增量：汇编中仍可见额外中转跳转和部分调用结果的栈读写。
后续成本归因、优化与验收单独见[原生异常性能优化方案](./feng-native-exception-performance-dev.md)，
不能把这些残留一概视为原生 EH 必需的成本。
普通 release 样本的整包文件由 67,776 增至 85,008 字节，含新 decoder 及原生异常表；
其他三个正常路径 release 样本均保持约 85 KB。结构布局和 runtime API 签名未变。

S12 的实施前证据位于工程 `build/asan-lld-probe.FHbmkr/` 与
`temp/asan-catch-analysis/`，包括两类 sanitizer 对照、插件原生协议对照及 LLDB／IR。
临时产物可能被全量构建清理；正式验收须建立可重复的仓库用例。

### 7.2 正式实现验收结果（2026-09-24）

以下使用本次正式 Codegen／runtime 和已提交的预构建插件。最终完整回归后只回填
本文，没有修改产品代码或测试；不以 §5 的历史原型替代本轮结果。

| 验证 | 结果与边界 |
| --- | --- |
| macOS ARM64，沙箱外 `make test` | 退出 0。UBSan、普通两阶段均通过；包含编译器／runtime／CLI／LSP／DAP、91 个 smoke、607 个 std、1516 个 FCTS、性能约束、增量与发行脚本、插件接入／搬移验证。普通阶段使用 bundled Clang，UBSan 使用既有 host Clang＋测试 LLD 通路。 |
| macOS ARM64，显式 release | 最终代码重建并运行 std 607／607、FCTS 1516／1516；原 BIND10 通过。 |
| Linux ARM64 GNU，原生容器 | 最终代码重建 Feng 和 runtime；runtime 所有权、线程隔离和 decoder 测试通过；release std 607／607、FCTS 1516／1516。S33 的 1 GB 失败记录保留，FCTS 最终在 2 GB 容器中构建通过。 |
| Linux ARM64 musl，原生容器 | 静态链接产物实际执行；release std 607／607、FCTS 1516／1516。 |
| Linux x64 GNU，Rosetta 容器 | 最终代码重建 Feng 和 runtime；runtime 测试通过；release std 607／607、FCTS 1516／1516。未计作原生 x64 结果。 |
| Linux x64 musl，Rosetta 容器 | 静态链接产物实际执行；release std 607／607、FCTS 1516／1516。未计作原生 x64 结果。 |
| 真实 Feng 发码的 LLVM IR | `-O0/-O1/-O2/-O3` × 普通／ASan＋UBSan（不可恢复）矩阵通过；原生 EH、真实内联、协议消除、纯算术无附加分支及 tuple 无附加堆分配断言保留。 |
| macOS ASan＋UBSan 专项运行 | instrumented runtime 测试通过；正常调用、try、资源清理、泛型、抛出／捕获五组真实 Feng 程序在 O0／O2／O3 均通过，共 15 组。使用既有补丁 LLD，不更改默认 sanitizer 配置。 |
| 原始无 std 复现和隐藏源码的包 | 已加入 CLI 自动测试；原始复现 default／release 通过，FT producer／consumer 的四种优化组合在普通、UBSan 阶段均通过，覆盖静态调用、泛型载荷、类型／方法泛参、函数值和原样重抛。 |
| 既有暂停用例核对 | 没有仅因 S11 而仍被注释的合法用例；BIND10 原样执行。N08 与清理中外抛的注释源码仍属于阶段二禁止的行为，保留对应 AE1507 负例。 |

macOS 最终显式 release 指令为：

```sh
mkdir -p build/s11-final/macos-temp
FENG_TEMP_DIR="$PWD/build/s11-final/macos-temp" build/bin/feng run std/std_test --release
FENG_TEMP_DIR="$PWD/build/s11-final/macos-temp" build/bin/feng run fcts/fcts_bin --release
```

Linux 两个 host 分别使用独立构建和项目副本，GNU／musl 顺序构建，避免 S28 的包产物
互相覆盖；locale 使用 CI 的 `C.UTF-8`。这轮没有提交或推送代码，没有运行远端 CI、
正式签名／公证／发布；这些边界不计作本地通过。

本机日志保存在 `/private/tmp/feng-s11-make-test.log`、
`feng-s11-final-std-release.log`、`feng-s11-final-fcts-release.log`、
`feng-s11-final-linux-*-release.log`、`feng-s11-final-linux-*-remaining.log`；
S33 内核证据为 `feng-s11-linux-arm64-oom.log`。性能源码、产物、IR 和栈报告归档于
`/private/tmp/feng-s11-implementation-evidence-20260924.tgz`，所有编译与执行仍在仓库目录内。
日志／归档是本机 Review 证据，不作为回归测试依赖。

参考：[LLVM 异常处理](https://llvm.org/docs/ExceptionHandling.html)、
[Pass 管线接入](https://llvm.org/docs/NewPassManager.html#inserting-passes-into-default-pipelines)、
[Clang Pass 插件参数](https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-fpass-plugin)、
[原生展开接口](https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html)。
