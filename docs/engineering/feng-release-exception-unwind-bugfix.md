# Release 异常控制流与展开修复方案（S11）

## 1. 状态与范围

状态：缺陷已确认，尚未修复；生产实现及性能方案待 Review。

本问题在[异常元信息开发方案](./feng-callable-exception-effects-dev.md)阶段二的额外
release 验证中发现，原编号为 S11。2026-09-21 人工决定先独立交付 defer 阶段二，
将本问题移至本文单独处理；既有用例在非 release 模式下可以通过，继续正常执行，
不为隔离 release 缺陷而注释默认模式的有效覆盖。

S11 早于异常元信息阶段一，不是 defer 边界检查或本轮断点修复引入。
语言行为仍遵循[异常规范](../specifications/feng-exception.md)及
[defer 规范](../specifications/feng-defer.md)，本文不重新定义异常语义。

本次拆分只保留问题、证据与方案，不实施 S11 的后端或 runtime 改造。
后续不得以某个表达式、具体 callee 或测试名称为条件增加特判；增加运行开销时须先由人工审定。

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

## 4. 推荐方案与 Review 边界

推荐使用宿主原生异常控制流，具体接入仍需原型验证与人工 Review。

### 4.1 在优化前建立原生异常控制流

保留普通值与调用的现有 C 代码生成，在 LLVM 优化前增加结构化的 Feng EH 转换步骤：

```text
Feng → 现有 C 生成及编译期异常区域信息 → 未优化的 LLVM IR
     → Feng EH 转换 → LLVM 正常优化与机器码
```

受保护的可能外抛调用，包括 `unknown`，表达为 `invoke`；catch／清理入口使用
`landingpad`；清理后继续展开使用 `resume`。准确维护调用属性、正常后继、异常后继
与嵌套区域，不能采用文本替换 IR 或在 `-O2` 已破坏控制流之后补标签。
当前仓库没有该转换步骤，所需 LLVM 开发组件及构建／分发接入属于后续方案工作。

### 4.2 调整 personality 及清理入口

- personality 读取后端生成的异常表，继续按 Feng 类型描述符匹配，向 landingpad
  提供异常记录与 selector；不引入 C++ 类型继承或 RTTI 匹配。
- 后端 selector 不是源码 catch 的固定序号，内联后可能改变；生成的分派必须按原生 EH
  规则处理，不能原样假设等于 `matched_clause`。
- 编译器生成清理入口，优先复用现有 `feng_frame_release_to(&marker)`，显式清理对应
  的逻辑边界后继续展开；personality 不再额外按原生帧弹一个边界，防止内联错配与重复清理。
- 保留现有异常所有权规则及 catch begin/end；清理中内部捕获其他异常后，必须继续展开
  原异常。需要验证普通 catch、匿名重抛、landing 清理与嵌套 catch 的完整生命周期。

### 4.3 结构、ABI 与成本

设计目标是保留对象、类型／泛型／spec 描述符、`FengUnwindException`、清理链、
`FengFrameMarker`、`FengCatchContext` 的现有布局；保留 `feng_throw`、`feng_rethrow`、
catch begin/end 的签名及普通函数调用约定。S11 不要求修改 FT 格式或版本。
既有 `FengLSDA` 声明与注册 API 可保留，新路径改由后端生成异常表，不要求为此改变结构布局。

需要变化的是编译产物与 personality 之间的异常表及落点协议；因此不能承诺新旧对象文件
无需重编即可混用。人工已明确可以统一重编，这与修改对象布局或函数参数 ABI 是不同事项。
runtime 的表解析与展开内部实现仍然需要调整，不能将方案描述成仅改编译期检查。

方案不以新增装箱、堆分配、每次调用查询或统一禁止内联为前提。
实际寄存器保存、栈空间、代码体积、正常路径与异常路径成本尚未测量；
不能承诺零开销或宣称性能最优。任何新增运行开销及偏离上述 ABI 边界的需求，先提交人工决策。

参考：[LLVM 异常处理](https://llvm.org/docs/ExceptionHandling.html)、
[LLVM invoke](https://llvm.org/docs/LangRef.html#invoke-instruction)、
[LLVM callbr 的目标地址限制](https://llvm.org/docs/LangRef.html#callbr-instruction)、
[原生展开接口](https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html)。

## 5. 实施与验收 Todo

- [x] 记录原始失败用例、独立最小源码、生成代码原因及阶段一之前的对照证据。
- [x] 按人工决定与 defer 阶段二拆分，明确未修复状态与回归范围。
- [x] 保留原始 BIND10 用例及原断言的默认模式覆盖，不注释可通过的既有用例。
- [ ] 细化原生 EH 接入、selector 分派、内联清理、跨包和各已支持目标平台方案，并由人工 Review。
- [ ] 在工程内完成通用原型，核对 `invoke`／landingpad、异常表及优化后的真实机器码。
- [ ] 测量正常路径、抛出／捕获、嵌套清理、泛型和跨包调用的时间、栈空间及代码体积；新增成本先由人工审定。
- [ ] 按批准方案实现；检查 runtime 函数签名、结构布局、FT 与重新构建策略。
- [ ] 对原始 BIND10 用例及最小复现增加独立自动化的默认／release 对照，保留原断言。
- [ ] 覆盖内联／非内联、必抛／条件抛、调用前局部值修改、直接／间接调用、泛型与跨包、具体／兜底 catch、重抛、嵌套清理及载荷析构时序。
- [ ] 执行 Debug／DAP 与语义／FT 回归，保证后端转换不改变诊断、变量读取及源码停点行为。
- [ ] 在沙箱外执行完整 `make test`，另外显式执行 std／FCTS 的 release 行为回归；默认模式通过不能代替后者。
- [ ] 回填结果、已支持平台与性能记录，输出英文 commit message，等待人工 Review，不自动提交。
