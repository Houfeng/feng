# 通用 C IR LLVM 异常处理插件开发方案

## 1. 状态与定位

状态：独立开发与交付方案，按人工确认的分步交付方向整理，待 Review；协议接口仍为
草案，尚未完成生产插件实现或独立验收。

本文是插件及其 C 发码协议的主文档，也是 S11 两步交付中的第一步。插件面向协议工作；
任何以 C 为中间表示的编译器，
在满足协议与目标异常模型的前提下，都可以作为使用方。Feng 是其中一个使用方，
其接入和 S11 修复属于第二步，见 [S11 修复文档](./feng-release-exception-unwind-bugfix.md)。

第一步交付独立源码、协议头文件、测试、手工维护构建脚本及工具链预构建产物。
源码放在 `third_party/`，维护者在本地手工预构建后放入 `toolchain/`；普通构建与 CI
只使用、验证预构建产物，不自动构建插件。第二步再修改 Feng 发码、编译驱动及 runtime。
第一步验收通过不表示 S11 已修复，也不能代替第二步的 Feng 全量回归。

本文的 C IR 指生成的 C 中间代码。插件实际处理 Clang 产生的 LLVM IR，在优化前将
显式的异常区域标记转换为原生异常控制流，再由 LLVM 完成优化及机器码生成。
插件不从普通 C 调用名称、源码行号或某个语言的对象布局推测异常语义。

已验证的 S11 原型证明了区域转换与原生展开的可行性，但仍使用带 Feng 名称的标记，
并硬编码 personality。本文新增的通用接口、可指定的 personality 和独立使用方测试
尚未完成验证，不能把原型结果直接计作通用协议验收通过。

## 2. 分层职责

| 层次 | 职责 |
| --- | --- |
| C 发码使用方 | 生成协议标记、catch 分派主体及资源清理代码；确定语言层面的异常和所有权语义 |
| 通用 LLVM Pass | 校验协议、分析区域状态、建立异常控制流与 SSA 合流、保留调用约定及调试信息、消除全部标记 |
| 使用方 runtime | 创建及销毁异常记录，提供 personality，解析原生异常表，解释类型标识并管理异常生命周期 |
| LLVM 后端与平台展开器 | 生成目标机器码与展开信息，执行相应平台的原生栈展开 |

插件及协议头文件不依赖使用方的 AST、FT、类型描述符定义、对象布局、ARC 或 runtime
头文件。插件不直接插入某个语言的 retain/release、catch begin/end 或载荷装箱调用；
这些代码由使用方生成，插件只按其所在区域建立正常与异常控制流。

这种职责划分不要求新增运行时的通用调度层。协议参数均在编译期处理，机器码中
不得保留协议调用、区域状态查询或用于前端保活的人工分支。

## 3. 首版适用边界

- 输入采用 GNU C11，允许用 `&&label` 描述当前函数内的 landing 入口。
- 首版采用 `invoke / landingpad / resume` 模型及与之配套的原生展开协议；目标范围
  为 macOS ARM64 和 Linux x64／ARM64 GNU／musl。当前原型的实际运行与交叉编译
  结果分别见 [S11 验证记录](./feng-release-exception-unwind-bugfix.md#51-执行证据)。
- 使用方的 personality 必须兼容该模型、LLVM 生成的异常表及异常记录／selector 交接约定。
  “不绑定语言”不表示可以任意替换为不同异常模型的 personality。
- Windows funclet 等其他模型不属于首版范围；`setjmp/longjmp` 不属于本协议。
- 标记转换必须先于会内联、消除或重排保护区域的 LLVM 优化；`-O0` 同样执行转换。
- 使用方须以 `-fexceptions` 启用配套的 Clang 异常编译模式，并确保 C 声明中的
  `nothrow`／LLVM `nounwind` 承诺真实有效；插件不能把普通 C 的默认编译方式视为协议输入。
- 协议版本、LLVM 插件 API 版本和 LLVM 构建版本分别管理。协议版本相同不代表插件
  二进制可以被任意 Clang 加载。

## 4. 接口草案

以下为首版命名与签名草案。正式协议头文件只依赖标准整数类型和目标的原生展开声明，
不包含任何语言 runtime 头文件。声明不提供运行时实现，必须由配套 Pass 处理。

```c
#include <stdint.h>
#include <unwind.h>

#define CIR_EH_PROTOCOL_VERSION 1u

/* 使用方提供的原生异常匹配与展开入口。 */
typedef _Unwind_Reason_Code (*CirEhPersonality)(
    int version,
    _Unwind_Action actions,
    uint64_t exception_class,
    struct _Unwind_Exception *exception,
    struct _Unwind_Context *context);

/* 配置当前函数的协议版本与 personality。 */
extern void cir_eh_configure(uint32_t version, CirEhPersonality personality);

/* 声明当前函数内的区域；返回值仅用于保留其 landing 入口。 */
extern _Bool cir_eh_region(uint32_t id, uint32_t parent, void *landing,
                           uint32_t catch_count, ...);

/* 设置后续调用所属的编译期区域，0 表示没有本函数内的处理区域。 */
extern void cir_eh_activate(uint32_t id);

/* 读取当前异常路径上该区域接收的原生异常记录。 */
extern void *cir_eh_exception(uint32_t id);

/* 读取与上述记录配套的原生 selector。 */
extern int32_t cir_eh_selector(uint32_t id);

/* 取得当前函数中某个类型标识对应的原生 selector。 */
extern int32_t cir_eh_typeid(const void *type_key);

/* 将本区域尚未处理的异常传给父区域或继续向调用方展开。 */
extern _Noreturn void cir_eh_propagate(uint32_t id);
```

### 4.1 函数配置与类型标识

每个使用协议的函数在入口声明且只声明一次 `cir_eh_configure`。所有标记的版本、区域
编号、父编号及数量参数必须是编译期常量，版本必须受支持。personality 必须是具备
上述原生签名的函数符号引用；插件将其写入该 LLVM 函数的 `personality` 属性，不生成
运行时的 personality 选择操作。配置只作用于当前函数，不隐式作用于其他函数或翻译单元。

`type_key` 必须是可用于 LLVM catch 条目的常量指针，例如使用方静态描述符的地址。
插件保存该引用，不读取它指向的数据，也不规定具体类型如何匹配。类型匹配由
personality 完成；首版约定空指针条目表示兜底匹配，使用方 personality 须遵守此约定。

类型标识在不同编译单元间的身份规则由使用方维护。插件不建立新的类型注册表，
也不为跨包类型或泛型类型增加特殊分支。

### 4.2 区域声明与激活

区域编号在一个函数内唯一，0 保留为无本地处理区域；非零父编号须引用同函数内的
已声明区域，父关系必须无环。纯 cleanup 区域的 `catch_count` 为 0。
catch 列表按使用方的匹配优先顺序传入，数量必须与实际变参个数相符；每个变参均为
`const void *` 常量，兜底条目使用 `(const void *)0` 且须最后。

`cir_eh_region` 的返回值仅用于以下保活形式，不作为用户可观察的值：

```c
/* 区域声明本身不激活保护范围。 */
if (cir_eh_region(2u, 1u, &&landing, 1u,
                  (const void *)&example_type_key)) goto landing;
cir_eh_activate(2u);
```

以上是区域片段，父区域与入口须由完整函数定义；`example_type_key` 表示使用方的
静态类型标识对象。Pass 将人工条件入口消除，并建立真正的异常入口。
除这条可消除的保活边外，普通路径不得跳入或顺序落入 landing。

`cir_eh_activate` 只改变编译期的区域状态。使用方在分支、循环、提前退出、处理器入口
和正常离开保护范围时维护该状态；Pass 在 CFG 上验证它。对一个可能展开的调用，
若状态不能唯一确定，必须编译失败，不能按文本位置、最近标记或默认区域猜测。

### 4.3 异常结果与传播

`cir_eh_exception` 和 `cir_eh_selector` 只能在对应区域已接收到异常的路径中使用。
二者来自该区域自身的 landingpad，或子区域向它传播并合流的同一份 SSA 结果；
插件必须验证定义支配其使用。
异常记录是借用的不透明引用，其存续与所有权由使用方 runtime 保证。

`cir_eh_typeid` 转换为 `llvm.eh.typeid.for`。原生 selector 不等于源码 catch 序号，
也不能假定在内联前后不变。使用方通过该标记生成分派，并在需要时自行转换为其源码序号。

区域的 landingpad 应包含本区域与有效父区域的处理器信息。若本层不处理当前 selector，
由使用方执行相应清理后调用 `cir_eh_propagate`：同函数内有父区域时，向父入口传递原
异常结果并进行 SSA 合流；没有父区域时使用 LLVM `resume`。不能直接跳过同函数父处理器。

`cir_eh_propagate` 是编译期终结操作，无普通后继。即使前端或 sanitizer 在其后生成
其他指令，Pass 也必须统一切断普通续接路径，不能依赖特定插桩名称或紧邻 `unreachable`。

该标记只负责继续传播尚未处理的异常。进入 catch 后的所有权转移、结束 catch、
语言层面的重新抛出等，仍由使用方 runtime 协议处理；不能直接用它替换任意重抛 API。

## 5. 转换与校验顺序

1. 收集配置、区域与类型列表，校验版本、函数签名、标识、父关系和标记使用方式。
2. 删除人工保活入口及其死前驱；按终结语义处理传播标记。
3. 以普通函数入口和声明的 landing 入口分析 CFG，校验区域状态和异常结果的可用范围。
4. 将受保护区域中无法可靠证明 `nounwind` 的调用转换为 `invoke`，包括间接调用；
   `noreturn` 不等于 `nounwind`。不根据使用方函数命名推断可否抛出。
5. 建立 landingpad、类型 selector、父区域 SSA 合流和向调用方的 `resume`。
6. 保留原调用约定、属性、operand bundles、必要元数据与调试位置；消除全部协议标记，
   运行 LLVM verifier。协议调用不得以外部符号或真实函数调用留在最终对象中。

没有协议标记的函数不因本插件获得新的语言语义。含有标记但缺配置、版本不支持、
类型不符、存在区域歧义或普通边进入 landing 等情况，均由插件输出中立的编译期错误；
不回退到忽略标记的编译方式。使用方负责将错误关联到自己的构建或诊断入口。

### 5.1 Clang 接入

插件使用 LLVM C++ API，在单次 Clang 调用的 `PipelineStart` 接入点完成转换；Pass
声明为 required，使 `-O0` 下的 `optnone` 不阻止转换。不改变用户函数原有优化级别，
不依赖独立 `opt` 流程、IR 文本替换、整函数禁优化或强制禁内联。

插件动态库由编译时的 Clang 加载，不链接进生成程序，也不要求使用方主编译器链接
LLVM 库。插件自身的加载依赖须单独验证；不能据此要求生成程序链接 C++ 标准库或
C++ 异常 runtime。正常使用不输出逐函数转换日志。

## 6. 独立源码与预构建产物

按以下职责组织，目录与脚本在实施时创建，本次只整理文档：

| 位置 | 内容与职责 |
| --- | --- |
| `third_party/cir-eh/` | 独立协议头文件、Pass 源码、构建配置、测试及使用说明；可在无 Feng 源码和 runtime 的环境中构建、验证 |
| `scripts/build_cir_eh.sh` | 仓库维护入口；显式接收完整 LLVM SDK，手工执行构建、测试和预构建产物安装 |
| `toolchain/cir-eh/<host>/` | 通过验证的插件动态库、配套协议头文件及构建元信息；供编译与分发使用 |

插件目录与 `toolchain/llvm/` 并列。现有 `scripts/trim_llvm.sh` 会替换 LLVM 的 host
目录，分开存放可避免重新剪裁 LLVM 时删除插件。预构建元信息至少记录插件源码版本、
协议版本、LLVM 版本、host 及影响加载兼容性的构建配置，用于复现构建和核对配套关系。

插件按执行 Clang 的 **host** 构建，不按生成程序的 target 重复构建：

| Host | 预构建目录 |
| --- | --- |
| macOS ARM64 | `toolchain/cir-eh/macos-arm64/` |
| Linux x64 GNU | `toolchain/cir-eh/linux-x64-gnu/` |
| Linux ARM64 GNU | `toolchain/cir-eh/linux-arm64-gnu/` |

同一 host 插件处理 Clang 为不同 target 产生的 IR。首版 target 范围见 §3；Linux musl
是 target，不因此要求增加一个 musl host 插件。三个 host 都需有独立的构建与加载记录。

## 7. 手工构建与分发流程

首版配套 LLVM **22.1.8**。构建需要完整开发 SDK 的头文件、构建信息及必要工具；
已剪裁的发行工具链不作为完整 SDK。维护脚本负责以下顺序：

1. 校验显式提供的 SDK、LLVM 版本和 host，记录构建配置；不隐式安装或切换系统工具链。
2. 在工程 `build/` 或 `temp/` 内构建插件，运行独立测试；不在临时系统目录执行产物。
3. 分别用该 host 的 bundled Clang 和 sanitizer 测试使用的宿主 Clang 加载、转换并执行
   夹具。二者均须为 22.1.8，但版本号相同不能代替 LLVM C++ ABI、导出符号和依赖检查；
   也不要求 `clang` 与 `cc` 指向同一文件。
4. 全部验证通过后，将动态库、头文件和元信息安装到对应预构建目录。产物不得引用
   维护者本机的临时 SDK 绝对路径；在预期使用环境验证其加载依赖。

若同一插件产物不能兼容 bundled 与宿主 Clang，记录具体构建差异并提交人工决策，
不能自动增加第二套实现、回退到普通 C 异常路径或将版本号检查当作兼容通过。
完整 SDK 和仅用于测试的 runtime 不随插件放进 Feng 用户程序或普通发行包。

普通 `make`、`make test` 和 CI 可以加载预构建插件执行验证，但不从源码重建它。
源码或 SDK 配置变更后，由维护者重新执行脚本并更新对应预构建产物。现有
`scripts/toolchain-prebuilt-publish.sh` 归档整个 `toolchain/`，可包含新增插件目录；
沿用现有工具链预构建分发流程，不增加 CI 即时编译插件的步骤。
Feng 用户发行包的安装路径与组装接入属于 [S11 第二步](./feng-release-exception-unwind-bugfix.md#46-构建分发及既有测试接入)。

## 8. 独立验收边界

测试使用 C 夹具和**不包含、不链接 Feng runtime** 的最小测试 runtime，实际创建、匹配、
清理和传播原生异常。测试 runtime 只服务于插件验收，不成为通用产品 runtime。
只检查插件能加载或 IR 中出现 `invoke`，不足以独立交付。

| 范围 | 必须验证的行为 |
| --- | --- |
| 协议复用 | 同一插件二进制支持不同名称的 personality 和不透明类型标识；无 Feng 符号或布局依赖 |
| 异常路径 | 具体／兜底 catch、纯 cleanup、嵌套区域、同函数父区域传播、跨函数及跨翻译单元传播、间接调用 |
| 控制流与优化 | 分支、循环、提前退出、必抛与条件抛、`noreturn`／`nounwind`、真实内联、异常结果 SSA 合流及清理顺序 |
| 输入校验 | 缺配置、错误版本／签名／常量、重复区域、非法父关系、错误类型列表、区域歧义、非法入口和异常结果使用；输出明确错误 |
| 编译组合 | `-O0/-O2/-O3`、sanitizer、跨翻译单元；保留调用约定、属性、必要元数据及调试位置，通过 LLVM verifier |
| 产物 | 无残留协议符号、协议调用、区域状态查询和人工保活分支；不靠禁优化、禁内联或关闭 sanitizer 通过 |
| 平台 | 三个 host 的实际加载；五个 target 的编译、异常表检查及原生执行，分别记录，交叉编译不代替执行 |

正常路径不增加协议查询、运行时区域注册或 `setjmp/longjmp` 操作。仍须记录正常／异常
路径耗时、栈空间与代码体积；“标记已消除”不等于所有程序成本必然相同。出现新增
运行成本时先记录和分析，再由人工审定，不能用一个小样本代替成本验收。

独立验收覆盖插件协议与原生异常控制流；具体语言的载荷、所有权、ARC、泛型、FT 和
完整包构建仍由使用方验收。S11 的历史原型只作为可行性依据，不勾选本节正式验收项。

## 9. 实施与交付 Todo

### 9.1 协议与组织

- [x] 人工确认先独立交付插件，再接入 Feng 修复 S11；采用 `third_party` 源码与手工预构建、
      `toolchain` 分发方式，普通构建及 CI 不自动构建插件。
- [ ] Review 首版接口名、签名及语义，固定协议版本；头文件与本文保持一致。
- [ ] Review §6–§8 的构建输入、产物组织、兼容性和独立验收边界。

### 9.2 独立实现与测试

- [ ] 建立 `third_party/cir-eh/` 独立工程；定义协议头文件、构建入口与使用说明。
- [ ] 从已验证原型抽离通用转换，移除硬编码的语言符号、类型与头文件依赖。
- [ ] 完成配置与区域校验、CFG 状态分析、invoke 转换、landing 入口、selector／SSA、
      父区域传播和标记消除；保留调用 ABI 及调试元数据。
- [ ] 建立独立 C 夹具和最小测试 runtime，逐项覆盖 §8；不得用 Feng 集成用例代替。

### 9.3 预构建与独立交付

- [ ] 实现维护脚本，校验 LLVM 22.1.8 完整 SDK；构建、验证通过后安装预构建产物。
- [ ] 完成三个 host 的 bundled／宿主 Clang 加载验证及五个 target 的执行验收，记录依赖
      与性能；未通过的平台明确记录为未交付，不能只以 macOS 原型通过宣布完整交付。
- [ ] 更新工具链预构建产物及元信息，验证分发后仍能加载；普通构建与 CI 不增加源码构建步骤。
- [ ] 完成独立测试和沙箱外 `make test`，确认插件交付未改变现有 Feng 行为；后者此时仍走
      原有后端，不能据此宣布 S11 修复。
- [ ] 回填验收记录与产物信息，输出英文 commit message，等待人工 Review，不自动提交。
      独立交付确认后，按 S11 文档另行实施 Feng 接入。

参考：[LLVM personality](https://llvm.org/docs/LangRef.html#personality-function)、
[LLVM 异常控制流](https://llvm.org/docs/ExceptionHandling.html)、
[Pass 管线接入](https://llvm.org/docs/NewPassManager.html#inserting-passes-into-default-pipelines)。
