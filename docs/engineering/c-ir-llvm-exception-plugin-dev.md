# llvm-c-eh 插件开发方案

## 1. 状态与定位

状态：2026-09-23 人工批准按本文实施独立插件阶段；协议 v1 为本阶段实现基线。
独立实现、测试和预构建产物已于 2026-09-24 提交，实际验收结果与平台边界见
§7.3／§11；提交记录不代替尚未完成的原生平台验收，也不表示 S11 已修复。

本文是插件及其 C 发码协议的主文档，也是 S11 分阶段交付中的独立工具阶段。插件面向协议工作；
任何以 C 为中间表示的编译器，
在满足协议与目标异常模型的前提下，都可以作为使用方。Feng 是其中一个使用方，
后续先按[工具链接入文档](./feng-llvm-c-eh-integration-dev.md)接入 driver、测试与发行，
再按 [S11 修复文档](./feng-release-exception-unwind-bugfix.md)改造异常发码与 runtime。

第一步交付独立源码、协议头文件、测试、手工维护构建脚本及工具链预构建产物。
源码放在 `third_party/`，维护者在本地手工预构建后放入 `toolchain/`；普通构建与 CI
只使用、验证预构建产物，不自动构建插件。工具链接入与异常后端修复分别 Review、验收。
独立工具验收不能代替后续 Feng 全量回归。

本文的 C IR 指生成的 C 中间代码。插件实际处理 Clang 产生的 LLVM IR，在优化前将
显式的异常区域标记转换为原生异常控制流，再由 LLVM 完成优化及机器码生成。
插件不从普通 C 调用名称、源码行号或某个语言的对象布局推测异常语义。

项目名采用 `llvm-c-eh`，避免与 ClangIR 的 CIR 名称混淆。这是独立项目提供的 LLVM
编译器扩展，项目名及协议符号中的 LLVM 不表示其为 LLVM 官方内建接口。

已验证的 S11 原型证明了区域转换与原生展开的可行性，但仍使用带 Feng 名称的标记，
并硬编码 personality。通用实现与独立测试的当前结果见 §11；不能把原型结果直接
计作本阶段通用协议验收通过。

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
- 各版本号的归属与用途见 §4.4。协议版本相同不代表插件二进制可以被任意 Clang 加载。

## 4. 协议接口

命名、首版接口签名与语义按人工批准的实施基线统一如下。

| 用途 | 名称 |
| --- | --- |
| 插件项目 | `llvm-c-eh` |
| 编译期协议标记前缀 | `__llvm_c_eh_` |
| 协议版本宏 | `__LLVM_C_EH_PROTOCOL_VERSION` |
| personality 函数指针类型 | `__llvm_c_eh_personality_fn` |
| 协议头文件 | `llvm_c_eh.h` |

`__llvm_c_eh_` 用于本扩展的标记和类型别名，`__LLVM_C_EH_` 用于宏，均为本扩展保留
前缀，使用方普通代码不得占用；不额外增加尾部双下划线或 `marker`。双下划线表达
实现保留命名，标记语义仍由配套 Pass 识别、校验并转换，不因名称本身而获得 Clang 内建语义。

上述宏、类型别名及下文七个标记均由本项目定义，实施时统一放入
`llvm_c_eh.h`。宏标识该头文件采用的发码协议版本，用途与校验过程见 §4.4；类型别名
描述使用方 personality 的函数指针，其签名遵循既有展开 ABI，不另创 personality ABI。
七个标记的用途在声明注释中逐一列出，完整语义见 §4.1–§4.3；插件负责转换它们，
不提供同名的运行时函数实现。

以下名称由既有 C 语言规定或由相应工具链头文件提供，保持原名和原定义：

| 名称 | 定义来源 | 用途 |
| --- | --- | --- |
| `uint32_t`、`uint64_t`、`int32_t` | C 标准头文件 `<stdint.h>` | 固定宽度整数，用于协议参数及原生 ABI 参数 |
| `_Bool`、`_Noreturn` | C 语言关键字 | 布尔类型、不返回的函数声明；不是插件定义的类型或宏 |
| `_Unwind_Reason_Code` | 原生展开 ABI，`<unwind.h>` | 枚举类型，表示找到处理器、继续展开、安装上下文等处理结果 |
| `_Unwind_Action` | 原生展开 ABI，`<unwind.h>` | 表示搜索阶段、清理阶段等操作标志 |
| `struct _Unwind_Exception` | 原生展开 ABI，`<unwind.h>` | 展开器识别的异常记录头，供使用方 runtime 组织其完整异常记录 |
| `struct _Unwind_Context` | 原生展开 ABI，`<unwind.h>` | 展开器提供的当前栈帧上下文，通过展开 API 访问 |

原生类型的依据为[原生异常展开 ABI](https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html)。
仓库现有 libunwind 的对应定义见 [unwind.h](../../third_party/libunwind/include/unwind.h)
和 [unwind_itanium.h](../../third_party/libunwind/include/unwind_itanium.h)；生成 C 应包含
目标工具链提供的 `<unwind.h>`，不复制这些类型定义，也不依赖仓库绝对路径。
例如 `_URC_HANDLER_FOUND` 表示找到处理器，`_URC_CONTINUE_UNWIND` 表示继续展开，
`_URC_INSTALL_CONTEXT` 表示将控制权交给指定入口，`_URC_END_OF_STACK` 表示已到栈顶
仍未找到处理器。这些枚举值同样来自既有 ABI，不由插件重新定义。

具体 personality 函数由使用方 runtime 实现，负责解释其类型标识及原生异常表。
Feng 使用现有的 `__feng_personality_v0`，接入见 [S11 §4.2](./feng-release-exception-unwind-bugfix.md#42-feng-到通用协议的映射)。
插件只接收其函数符号，不实现 Feng 的类型匹配或异常生命周期；其他使用方可以传入
自己的 personality。`__llvm_c_eh_personality_fn` 仅为函数指针类型，不是该函数的实现。

正式协议头文件只依赖标准整数类型和目标的原生展开声明，不包含任何语言 runtime
头文件。声明不提供运行时实现，必须由配套 Pass 处理；已有 `_Unwind_*` ABI 名称保持原样。

```c
#include <stdint.h>
#include <unwind.h>

/* 本项目定义的 C 发码协议版本，由 Pass 在编译期校验。 */
#define __LLVM_C_EH_PROTOCOL_VERSION 1u

/* 本项目定义的函数指针别名；签名遵循原生展开 ABI，实现由使用方提供。 */
typedef _Unwind_Reason_Code (*__llvm_c_eh_personality_fn)(
    int version,
    _Unwind_Action actions,
    uint64_t exception_class,
    struct _Unwind_Exception *exception,
    struct _Unwind_Context *context);

/* 配置当前函数的协议版本与 personality。 */
extern void __llvm_c_eh_configure(uint32_t version,
                                  __llvm_c_eh_personality_fn personality);

/* 声明当前函数内的区域；返回值仅用于保留其 landing 入口。 */
extern _Bool __llvm_c_eh_region(uint32_t id, uint32_t parent, void *landing,
                               uint32_t catch_count, ...);

/* 设置后续调用所属的编译期区域，0 表示没有本函数内的处理区域。 */
extern void __llvm_c_eh_activate(uint32_t id);

/* 读取当前异常路径上该区域接收的原生异常记录。 */
extern void *__llvm_c_eh_exception(uint32_t id);

/* 读取与上述记录配套的原生 selector。 */
extern int32_t __llvm_c_eh_selector(uint32_t id);

/* 取得当前函数中某个类型标识对应的原生 selector。 */
extern int32_t __llvm_c_eh_typeid(const void *type_key);

/* 将本区域尚未处理的异常传给父区域或继续向调用方展开。 */
extern _Noreturn void __llvm_c_eh_propagate(uint32_t id);
```

### 4.1 函数配置与类型标识

每个使用协议的函数在入口声明且只声明一次 `__llvm_c_eh_configure`。所有标记的版本、区域
编号、父编号及数量参数必须是编译期常量，版本必须受支持。personality 必须是具备
上述原生签名的函数符号引用；插件将其写入该 LLVM 函数的 `personality` 属性，不生成
运行时的 personality 选择操作。配置只作用于当前函数，不隐式作用于其他函数或翻译单元。

`type_key` 必须是可用于 LLVM catch 条目的常量指针，例如使用方静态描述符的地址。
插件保存该引用，不读取它指向的数据，也不规定具体类型如何匹配。类型匹配由
personality 完成；首版约定空指针条目表示兜底匹配，使用方 personality 须遵守此约定。
具体校验为：空指针，或去除指针转换及零偏移地址计算后能引用全局符号的指针。
整数伪造的非空地址、符号内部的非零偏移地址不属于 LLVM 22 原生类型表可保留的
类型标识，必须报错；不能接受后让后端将其降为空指针兜底条目。

类型标识在不同编译单元间的身份规则由使用方维护。插件不建立新的类型注册表，
也不为跨包类型或泛型类型增加特殊分支。

### 4.2 区域声明与激活

区域编号在一个函数内唯一，0 保留为无本地处理区域；非零父编号须引用同函数内的
已声明区域，父关系必须无环。纯 cleanup 区域的 `catch_count` 为 0。
catch 列表按使用方的匹配优先顺序传入，数量必须与实际变参个数相符；每个变参均为
`const void *` 常量，兜底条目使用 `(const void *)0` 且须最后。

`__llvm_c_eh_region` 的返回值仅用于以下保活形式，不作为用户可观察的值：

```c
/* 区域声明本身不激活保护范围。 */
if (__llvm_c_eh_region(2u, 1u, &&landing, 1u,
                      (const void *)&example_type_key)) goto landing;
__llvm_c_eh_activate(2u);
```

以上是区域片段，父区域与入口须由完整函数定义；`example_type_key` 表示使用方的
静态类型标识对象。Pass 将人工条件入口消除，并建立真正的异常入口。
除这条可消除的保活边外，普通路径不得跳入或顺序落入 landing。

`__llvm_c_eh_activate` 只改变编译期的区域状态。使用方在分支、循环、提前退出、处理器入口
和正常离开保护范围时维护该状态；Pass 在 CFG 上验证它。对一个可能展开的调用，
若状态不能唯一确定，必须编译失败，不能按文本位置、最近标记或默认区域猜测。

### 4.3 异常结果与传播

`__llvm_c_eh_exception` 和 `__llvm_c_eh_selector` 只能在对应区域已接收到异常的路径中使用。
二者来自该区域自身的 landingpad，或子区域向它传播并合流的同一份 SSA 结果；
插件必须验证定义支配其使用。
异常记录是借用的不透明引用，其存续与所有权由使用方 runtime 保证。

`__llvm_c_eh_typeid` 转换为 `llvm.eh.typeid.for`。原生 selector 不等于源码 catch 序号，
也不能假定在内联前后不变。使用方通过该标记生成分派，并在需要时自行转换为其源码序号。

区域的 landingpad 应包含本区域与有效父区域的处理器信息。若本层不处理当前 selector，
由使用方执行相应清理后调用 `__llvm_c_eh_propagate`：同函数内有父区域时，向父入口传递原
异常结果并进行 SSA 合流；没有父区域时使用 LLVM `resume`。不能直接跳过同函数父处理器。

`__llvm_c_eh_propagate` 是编译期终结操作，无普通后继。即使前端或 sanitizer 在其后生成
其他指令，Pass 也必须统一切断普通续接路径，不能依赖特定插桩名称或紧邻 `unreachable`。

该标记只负责继续传播尚未处理的异常。进入 catch 后的所有权转移、结束 catch、
语言层面的重新抛出等，仍由使用方 runtime 协议处理；不能直接用它替换任意重抛 API。

### 4.4 协议版本与校验

`__LLVM_C_EH_PROTOCOL_VERSION` 表示当前协议头文件采用的 C 发码协议版本，由本项目
定义。它供生成 C 的使用方声明发码约定，使独立交付的插件可以判断是否理解这些标记
及其语义，避免新旧协议混用时被静默解释为另一种含义。

本方案保留显式协议版本校验：每个使用协议的函数必须通过 `__llvm_c_eh_configure`
传入受支持的编译期版本常量。宏本身不是 C 或 LLVM 的强制要求，直接传入 `1u` 也能
表达首版版本；统一使用头文件宏可避免使用方散落版本字面量。保留宏是协议的统一
书写方式，不能把“宏不是语法必需”理解为可以省略版本参数或校验。

例如，在使用方生成函数的入口写入以下片段；`example_personality` 是使用方已声明的
原生 personality 函数符号：

```c
__llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, example_personality);
```

C 预处理器将宏展开成整数常量，Clang 生成 IR 后，Pass 读取配置标记中的该常量并校验。
不支持的版本编译报错；支持则按相应协议转换并消除配置标记。宏名称不是插件在 IR 中
查找的标识，最终程序也不保留协议版本查询、比较或分派操作，不增加运行时版本校验开销。

以下版本分别由不同接口管理，不能互相替代：

| 版本 | 定义方及用途 |
| --- | --- |
| `__LLVM_C_EH_PROTOCOL_VERSION` | 本项目定义；C 发码使用方与插件之间的协议版本，通过 `__llvm_c_eh_configure` 传入 |
| personality 参数中的 `int version` | 原生展开 ABI 定义；由展开器调用 personality 时传入，表示该调用接口的 ABI 版本 |
| `LLVM_PLUGIN_API_VERSION` | LLVM 的 `PassPlugin.h` 定义；用于检查 LLVM 加载 Pass 插件的接口版本，保持其原名 |
| LLVM `22.1.8` | 配套工具链和开发 SDK 的版本，涉及插件二进制的构建与加载兼容性 |

其中两个函数参数虽然都名为 `version`，但 `__llvm_c_eh_configure` 接收的是本项目的
发码协议版本，personality 接收的是展开器提供的 ABI 版本；不得用协议版本宏替代后者。
LLVM 插件接口的定义见 [LLVM 22.1.8 PassPlugin.h](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/include/llvm/Plugins/PassPlugin.h)。
协议版本校验不能代替 §7 的 LLVM 工具链和插件加载兼容性验证。

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
原生 EH 输入不与未转换的协议在同一函数中混用；一个 landing 标签只属于一个区域。
受保护的 `musttail` 无法同时保留本协议要求的本地 unwind 后继，必须明确报错；
普通 nounwind 内联汇编和无标记函数不受此限制。

### 5.1 Clang 接入

插件使用 LLVM C++ API，在单次 Clang 调用的 `PipelineStart` 接入点完成转换；Pass
声明为 required，使 `-O0` 下的 `optnone` 不阻止转换。不改变用户函数原有优化级别，
不依赖独立 `opt` 流程、IR 文本替换、整函数禁优化或强制禁内联。

插件动态库由编译时的 Clang 加载，不链接进生成程序，也不要求使用方主编译器链接
LLVM 库。插件自身的加载依赖须单独验证；不能据此要求生成程序链接 C++ 标准库或
C++ 异常 runtime。正常使用不输出逐函数转换日志。

发布插件使用 Release 构建，不带 sanitizer 插桩。除本协议的转换外，不改变编译
选项、优化级别、链接器、sanitizer 或非协议代码的行为；编译期插件及其依赖不得进入
目标程序。对无协议输入，增加加载／不加载同一插件的 IR、汇编和运行结果对照；
对协议输入，验证原有调用 ABI、调试信息及 sanitizer 检查保留。已确认的工具链固有
失败单独记录，不通过自动更换链接器或关闭检查来宣称通过。
插件本身不要求用 UBSan 构建；开启 UBSan 的对象是使用 Release 插件编译的上层程序。
除合法程序的异常行为外，独立负例还须验证受保护代码和 catch 代码中的真实 UB
仍被检测，保留使用方配置的终止／恢复行为，不被异常区域捕获或吞掉。

## 6. 独立源码与预构建产物

按以下职责组织独立实现及维护入口：

| 位置 | 内容与职责 |
| --- | --- |
| `third_party/llvm-c-eh/` | 独立协议头文件、Pass 源码、构建配置、测试及使用说明；可在无 Feng 源码和 runtime 的环境中构建、验证 |
| `scripts/build_llvm_c_eh.sh` | 仓库维护入口；选择全部或单个 host，发现／覆盖完整 SDK，手工执行构建、测试和预构建产物安装 |
| `toolchain/llvm-c-eh/<host>/` | 通过验证的插件动态库、配套协议头文件及许可证；供编译与分发使用 |

插件目录与 `toolchain/llvm/` 并列。现有 `scripts/trim_llvm.sh` 会替换 LLVM 的 host
目录，分开存放可避免重新剪裁 LLVM 时删除插件。预构建元信息至少记录插件源码版本、
协议版本、LLVM 版本、host 及影响加载兼容性的构建配置，用于复现构建和核对配套关系。
动态库产物统一使用小写基名 `llvm_c_eh`：Linux 为 `lib/llvm_c_eh.so`，macOS 为
`lib/llvm_c_eh.dylib`；构建、测试、安装及使用示例均使用同一命名。

插件按执行 Clang 的 **host** 构建，不按生成程序的 target 重复构建：

| Host | 预构建目录 |
| --- | --- |
| macOS ARM64 | `toolchain/llvm-c-eh/macos-arm64/` |
| Linux x64 GNU | `toolchain/llvm-c-eh/linux-x64-gnu/` |
| Linux ARM64 GNU | `toolchain/llvm-c-eh/linux-arm64-gnu/` |

同一 host 插件处理 Clang 为不同 target 产生的 IR。首版 target 范围见 §3；Linux musl
是 target，不因此要求增加一个 musl host 插件。三个 host 都需有独立的构建与加载记录。

## 7. 手工构建与分发流程

维护入口要求（2026-09-23 人工确认）：

- 不指定平台时构建 §6 的全部三个 host；`--platform=<host>` 只构建指定 host。
- `--llvm-root=<path>` 为可选覆盖；省略时在对应构建环境查找已安装的完整
  LLVM 22.1.8 SDK，校验版本和必要组件，找不到时明确报错，不自动安装工具链。
- 独立工程统一使用 Makefile，不再依赖 CMake／Ninja；从 `llvm-config` 读取 SDK
  编译配置，保留导出入口、无 SDK RPATH 和不另链接 LLVM 的规则。
- 全平台维护入口为 macOS ARM64：macOS 在本机执行，Linux 分别在已准备的本机
  Apple Container `llvm-c-eh-arm64`／`llvm-c-eh-x64` 中执行。每个容器须将当前仓库
  挂载到 `/work` 并安装完整 SDK；脚本校验架构和挂载，不自动安装 SDK 或创建环境。
  仅停止本次脚本启动的容器，不影响原本运行的容器。
- 显式 SDK 路径属于所选 host 的构建环境，须与 `--platform` 一起使用；全平台模式
  由各环境独立发现 SDK。无兼容 SDK、构建或验证失败均返回非零；全平台模式仍完成
  其他 host 的尝试并汇总结果，不用某个平台成功代替整体成功。

选择容器的依据：当前 `build_libunwind.sh` 也是在 macOS／Linux 对应 CPU 环境分别
构建，只在 Linux 切换 GNU／musl。现有 Linux sysroot 不含 C++ 标准库开发文件，
实际交叉编译 `#include <vector>` 报头文件缺失，亦没有 Linux LLVM 开发 SDK。
理论上补齐这些依赖可以交叉构建插件，但仍需在 Linux 加载验证。本阶段不扩展
sysroot 或新增交叉 SDK 管理，使用人工已授权的 Apple Container。

首版配套 LLVM **22.1.8**。构建需要完整开发 SDK 的头文件、构建信息及必要工具；
已剪裁的发行工具链不作为完整 SDK。维护脚本负责以下顺序：

1. 使用显式 SDK，或通过 `llvm-config`／`llvm-config-22` 及已安装的版本目录查找 SDK；
   校验 LLVM 版本、完整性和 host，记录构建配置；不隐式安装或切换系统工具链。
2. 在工程 `build/` 或 `temp/` 内构建插件，运行独立测试；不在临时系统目录执行产物。
3. 分别用该 host 的 bundled Clang 和 sanitizer 测试使用的宿主 Clang 加载、转换并执行
   夹具。二者均须为 22.1.8，但版本号相同不能代替 LLVM C++ ABI、导出符号和依赖检查；
   也不要求 `clang` 与 `cc` 指向同一文件。
4. 全部验证通过后，将动态库、头文件和许可证安装到对应预构建目录。产物不得引用
   维护者本机的临时 SDK 绝对路径；在预期使用环境验证其加载依赖。

维护构建记录只保留在 `build/`：插件的 `build-info.txt`、`source-files.sha256` 位于
`build/llvm-c-eh/maintainer-<host>/`；补丁 LLD 的 `build-info.txt` 和 `SHA256SUMS`
位于 `build/test-lld/macos-arm64/`。`toolchain/llvm-c-eh/` 与
`toolchain/test_tools/lld/` 不保存或分发上述记录文件，已生成的同名文件也须移除。

若同一插件产物不能兼容 bundled 与宿主 Clang，记录具体构建差异并提交人工决策，
不能自动增加第二套实现、回退到普通 C 异常路径或将版本号检查当作兼容通过。
完整 SDK 和仅用于测试的 runtime 不随插件放进 Feng 用户程序或普通发行包。

普通 `make`、`make test` 和 CI 可以加载预构建插件执行验证，但不从源码重建它。
源码或 SDK 配置变更后，由维护者重新执行脚本并更新对应预构建产物。现有
`scripts/toolchain-prebuilt-publish.sh` 归档整个 `toolchain/`，可包含新增插件目录；
沿用现有工具链预构建分发流程，不增加 CI 即时编译插件的步骤。
Feng 用户发行包的安装路径与组装由[工具链接入方案](./feng-llvm-c-eh-integration-dev.md)统一定义。

### 7.1 工具链配套约束

2026-09-24 人工明确：Apple 工具链与 LLVM 官方工具链分开使用，不混用。

- Apple 原生对照使用 Apple Clang、Apple ld 及其配套编译资源；不将其结果作为
  LLVM 22.1.8 插件的加载或行为验收结果。
- LLVM 插件构建及使用方验收使用 LLVM 22.1.8 Clang、同版本 LLD 和配套编译资源；
  显式选择并记录实际链接器，不能只检查 Clang 版本后沿用默认 Apple ld。
- LLVM sanitizer 验证使用完整 LLVM SDK 的配套 sanitizer 资源；发行场景使用已剪裁
  的 bundled 工具链，不要求其提供 sanitizer runtime。macOS 目标 SDK 与系统库的
  既有要求见 [构建规范](../specifications/feng-build.md#22-目标平台与工具链转换)，
  其路径与编译器、链接器、sanitizer 资源分别记录。
- 本文此前的 LLVM Clang＋Apple ld／ld-classic 实验仅保留为历史定位证据，不能
  代替上述配套工具链的正式验收；已有失败不因重新分组而记为通过。

### 7.2 UBSan 专用预构建 LLD

2026-09-24 人工批准：I04 的补丁 LLD 仅用于 macOS UBSan 验证，维护者手工预构建
并验证后长期复用。后续人工批准将测试工具统一放入 `toolchain/test_tools/`，由现有
`toolchain/**` Git LFS 规则覆盖，并随工具链预构建 Release 分发，以减少日常 CI 的
LFS 下载。补丁 LLD 不替换 `toolchain/llvm/` 中的原版 LLD，也不进入 Feng 发行包。
Feng Makefile／CI 的消费方式及发行排除规则见[接入文档](./feng-llvm-c-eh-integration-dev.md)。

- `third_party/lld/` 只维护固定版本的来源、SHA256、许可证、补丁和构建说明，不提交
  完整 LLVM 源码。独立构建使用 `lld/`、公共 CMake 文件及 LLVM 22.1.8 完整开发 SDK；
  上游测试工具从同版本源码或 SDK 获取，不新增自行编写的 Python 脚本。
- `scripts/build_test_lld.sh` 为唯一手工维护入口：获取并校验固定源码、应用补丁、调用
  LLD 上游构建系统、运行 Mach-O 回归及本缺陷对照，验证后安装至
  `toolchain/test_tools/lld/macos-arm64/`。下载、编译和测试的中间文件放在 `build/`／`temp/`。
  校验在本次预构建中完成，不增加独立 check 脚本、检查模式或编译器代理脚本。
- 人工批准复用 LLD 上游 CMake：沿用其生成文件、库依赖和测试配置，避免重新实现
  第三方构建系统；README 须说明原因及维护依赖。此决定不改变 llvm-c-eh 的 Makefile。
  既有 `scripts/fetch_llvm.sh` 下载的是预编译 LLVM SDK，本构建入口另行获取固定
  LLD 源码归档，或通过参数复用已有归档；不修改既有下载脚本。
- 预构建工具须能搬移，不能依赖维护者 Homebrew 的绝对路径；保留所需依赖及许可证，
  记录 LLVM 版本、host、源码和补丁散列、产物散列及验证结果。
- 独立缺陷验收使用完整 LLVM 22.1.8 Clang 与补丁 LLD，覆盖正常配置和 UBSan 配置；
  不降低 sanitizer 检查或优化级别。后续插件验收仍须分别记录原版 LLD 普通结果和
  补丁 LLD UBSan 结果，不能相互替代。Feng 如何消费这些工具见[接入文档](./feng-llvm-c-eh-integration-dev.md)。

交付检查：

- [x] 补齐来源、补丁、手工构建入口和可搬移预构建产物。
- [x] 维护构建验证固定源码、工具版本、产物依赖、上游 Mach-O 测试及本缺陷回归。
- [x] 搬移后的最终产物重新通过本缺陷回归。
- [x] 除人工批准的 `.gitattributes` 整目录 LFS 规则外，不修改 Feng 现有配置、代码及测试。
- [x] 完成全量 `make test`，保留日志并等待人工 Review。

2026-09-24 独立预构建记录：SDK 自动发现和本地源码归档复用通过，最终产物已安装。
上游 Mach-O 测试 355 项通过、3 项不适用、0 项失败；ARM64／x64 的 10 项入口前缀
FDE 地址检查及原生 C++ 普通／UBSan × O0／O2／O3 共 18 项通过。最终签名产物搬移至
含空格路径后再次通过这 28 项。依赖审计仅见 macOS 系统库，无 Homebrew 动态依赖或
RPATH。另用同一 Release 插件完成原版 LLD 普通配置与补丁 LLD UBSan 配置的既有
`test/run.sh` 对照，两组均通过 O0／O2／O3、替换 personality 和 33 项非法协议检查；
未改动插件或测试源码。日志见 `third_party/lld/temp/prebuild-final.log`、
`plugin-normal.log`、`plugin-ubsan.log`（忽略目录）。
随后沙箱外完整 `make test` 返回 0：UBSan 与普通配置均通过，两个阶段的 std 均为
607／607、FCTS 均为 1508／1508，编译器、CLI、DAP、FT 等既有回归全部完成。
全量日志为 `third_party/lld/temp/make-test-final.log`。本次全量测试沿用原有配置，
不表示 Feng 已接入补丁 LLD 或 llvm-c-eh。

### 7.3 macOS 插件产物补齐

2026-09-24 核对确认：§7.2 交付的是测试用 LLD，尚未完成 macOS 插件的维护入口
验收和安装，`toolchain/llvm-c-eh/macos-arm64/` 仍缺失。补齐独立插件流程，继续遵守
Feng 现有 Makefile、CI、编译器、runtime 和既有测试不变的边界。

- macOS 插件自身以 Release 构建，显式使用原版 LLVM LLD；bundled／宿主 Clang
  的普通行为、无协议对照、搬移及成本验证也使用原版 LLD。
- 使用同一插件进行 UBSan 验收时，显式使用 §7.2 的预构建补丁 LLD；无协议对照、
  协议内真实 UB 诊断和完整异常行为均覆盖。维护入口只消费预构建 LLD，不自动构建它。
- 本阶段新增的独立测试驱动统一接收可选链接参数，仅在链接命令传入；不改夹具、断言、
  优化级别或 sanitizer 检查。不引入编译器代理或额外检查脚本。
- 构建记录分别保存两种配置实际选择的链接器版本和散列。全部验收通过后安装动态库、
  协议头文件和许可证，构建记录位置遵循 §7；不能用补丁 LLD 的成功代替原版 LLD 的普通验证。

- [x] 补齐维护入口的 macOS 链接器选择及独立驱动的链接参数传递。
- [x] 完成同一 Release 插件的全部 macOS 独立验收与搬移验证，安装至预构建目录。
- [x] 沙箱外完成全量 `make test`，回填结果，等待人工 Review。

2026-09-24 单平台维护入口实际返回 0，已安装
`toolchain/llvm-c-eh/macos-arm64/lib/llvm_c_eh.dylib`、协议头文件、许可证及来源记录。
同一 Release 插件通过 IR、bundled／宿主普通 O0／O2／O3、UBSan O0／O2／O3、
无协议输入对照、协议内真实 UB 终止／恢复、33 项非法输入、搬移、导出和依赖审计。
独立 Makefile 的 `test`／`test-ubsan` 入口也实际通过，链接选项未进入 `-c`／`-S`
命令。产物 SHA256 记录于各 host 维护构建目录内的 `build-info.txt`，其存放规则见 §7；
实际 Mach-O 头记录最低 macOS 26.0，本次验证使用本机系统，不声称覆盖更早版本。
日志为 `third_party/llvm-c-eh/temp/macos-installed-final.log` 和
`macos-make-entry-final.log`（忽略目录）。
本轮后续沙箱外完整 `make test` 返回 0，UBSan／普通阶段的 std 均为 607／607、
FCTS 均为 1508／1508，其他既有编译器、CLI、DAP、FT 与构建回归全部通过；日志为
`third_party/llvm-c-eh/temp/macos-delivery-make-test.log`。Feng 仍使用原有后端，
本结果不表示 S11 已接入或修复。

2026-09-24 按人工要求统一产物名称、`test_tools` 目录及整目录 LFS 规则后，再次
实际执行无参数维护入口，三个 host 均完成构建、独立测试及安装，整体返回 0；
最终日志为 `third_party/llvm-c-eh/temp/layout-all-hosts-final.log`。三份产物对应同一
源码及维护脚本散列，源码清单和二进制散列核对通过。搬移后的补丁 LLD 校验和、版本
及独立 UBSan 验收通过；Git 属性确认文本与二进制文件均使用整目录 LFS 规则。
Linux x64 仍在 Rosetta 容器中验证，不替代原生验收。
最终目录及 LFS 变更后的沙箱外完整 `make test` 返回 0，UBSan／普通配置各自的 std
均为 607／607、FCTS 均为 1508／1508，编译器、CLI、DAP、FT 等既有回归全部通过。
日志为 `third_party/llvm-c-eh/temp/layout-make-test-final.log`。

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
- [x] 人工确认项目名 `llvm-c-eh`、协议前缀 `__llvm_c_eh_` 及 §4 的配套命名。
- [x] Review 首版接口签名及语义，固定协议版本；头文件与本文保持一致。
- [x] Review §6–§8 的构建输入、产物组织、兼容性和独立验收边界。

### 9.2 独立实现与测试

- [x] 建立 `third_party/llvm-c-eh/` 独立工程；定义协议头文件、构建入口与使用说明。
- [x] 从已验证原型抽离通用转换，移除硬编码的语言符号、类型与头文件依赖。
- [x] 完成配置与区域校验、CFG 状态分析、invoke 转换、landing 入口、selector／SSA、
      父区域传播和标记消除；保留调用 ABI 及调试元数据。
- [ ] 建立独立 C 夹具和最小测试 runtime，逐项覆盖 §8；不得用 Feng 集成用例代替。

### 9.3 预构建与独立交付

- [x] 实现维护脚本，校验 LLVM 22.1.8 完整 SDK；构建、验证通过后安装预构建产物。
- [ ] 完成三个 host 的 bundled／宿主 Clang 加载验证及五个 target 的执行验收，记录依赖
      与性能；未通过的平台明确记录为未交付，不能只以 macOS 原型通过宣布完整交付。
- [ ] 更新工具链预构建产物及元信息，验证分发后仍能加载；普通构建与 CI 不增加源码构建步骤。
- [ ] 完成独立测试和沙箱外 `make test`，确认插件交付未改变现有 Feng 行为；后者此时仍走
      原有后端，不能据此宣布 S11 修复。
- [ ] 回填验收记录与产物信息，输出英文 commit message，等待人工 Review，不自动提交。
      后续先按工具链接入文档实施，再按 S11 文档修复异常后端。

## 10. 实施记录

2026-09-23 开始独立插件阶段。本机宿主与 bundled Clang 均为 22.1.8，Homebrew 完整
SDK、CMake、Ninja 可用，双方的 ABI breaking checks 均关闭。按每个 host 一份产物
实施，两套 Clang 的加载属于常规验收，不额外设为人工决策前置条件。
历史原型归档仍可读取；只提取算法与证据参考，不沿用 Feng 依赖或将原型视作正式实现。
后续问题在此先记录、再分析处理；跨平台执行和成本记录在实际验证后填写。

| 编号 | 问题、分析与处理 |
| --- | --- |
| I01 | 首次独立链接在 macOS 报 LLVM 未定义符号。源码编译通过，原因是独立使用 AddLLVM 时未自动设置从加载进程解析符号的链接选项；按插件既定设计显式启用 macOS dynamic lookup，不另链接一份 LLVM，不改变生成程序。 |
| I02 | 本机 container 服务起初未启动，已有镜像仅含 LLVM 21。已在任务专用 ARM64／x64 容器按 CI 相同源安装并校验 22.1.8 完整 SDK；x64 容器使用 Rosetta。分别记录本机、交叉编译、原生及转译执行结果，不混算验收。 |
| I03 | 新建独立夹具的首次运行在销毁总数断言失败；逐项核算为主线程 13 次、工作线程 800 次，原预期 814 多算一次。此前各行为断言和存活计数为零均通过；修正新夹具预期为 813，不改变插件或已有测试。 |
| I04 | 宿主 Clang UBSan 首轮执行中，大聚合间接调用的异常 66 未被捕获；非 sanitizer 的三个优化级别均通过。已保留夹具，正在对比插桩前后 IR 与原生异常表，尚不判断为插件或使用方 runtime 的问题。 |
| I05 | CTest 使用 macOS 自带 Bash 3.2，空数组在 `set -u` 下展开时报 unbound variable。新建脚本改用兼容的已定义数组展开，不要求升级系统 shell，也不改变测试参数。 |
| I06 | 正常路径成本测量发现，独立 macOS ARM64 catch 样本相较无 catch 的同一调用，LLVM 因处理器中两个外部调用的活跃值多保存 x19/x20，栈从 16 增至 32 字节，正常路径多一条保存与一条恢复。后续与等价原生 C++ catch 对照，正常路径指令和栈空间一致，未发现插件额外成本；Feng 实际接入的成本仍须在第二阶段验证，不宣称有 catch 与无 catch 的机器指令完全相同。 |
| I07 | Linux ARM64 的 bundled／宿主／完整 UBSan 行为与 IR 测试均通过，但安装前依赖检查阻止发布：LLVM 的 AddLLVM 自动加入 SDK RUNPATH。插件只需加载进程提供的 LLVM 符号，不需要该搜索路径；在构建配置中关闭自动 RPATH 后重新验证，不忽略检查或携带本机 SDK 路径分发。 |
| I08 | 新建维护脚本在最小 Linux 镜像缺少 git 时，printf 内命令替换失败未终止脚本，导致元信息的源码 revision 为空。补齐 git 前置检查，并把 revision 读取作为独立的必成功步骤；重新生成产物及元信息，不能把不完整记录当作正式交付。 |
| I09 | 边界验证发现 `(void *)1` 虽是 LLVM 常量指针，却不能作为原生类型表符号；后端将其输出为 0，改变成兜底语义。原校验仅排除了 undef/poison，不够完整。按 LLVM 类型表实际表示能力收紧通用校验，并增加伪造地址、非零偏移等负例，防止静默误编译。 |
| I10 | 执行全量回归时，既有 CLI 测试会整体删除仓库根 `temp/`，与并行的插件验证工作区冲突，临时日志和构建被删除。源码、已安装 SDK 与预构建目录未受影响。不修改既有测试；独立验证与全量回归改为顺序执行，日志另存到插件目录下的 `temp/`，来源散列只覆盖项目源码。本次沙箱外 `make test` 仍完整执行并返回 0，但当次重定向日志被清理，不伪称日志完整保留。 |
| I11 | 导出符号审计发现插件内部 ceh 函数也具有外部可见性。实现改为默认隐藏，只显式导出 LLVM 加载入口；Linux 的标准库模板实例仍带外部可见性，因此同时使用链接器导出列表限定入口，避免覆盖宿主编译器符号。重新验证两套 Clang 的加载与行为。 |
| I12 | 新增 UBSan 错误函数指针负例先被 Clang 的静态类型转换警告及 `-Werror` 拦截，尚未进入运行检查。仅在该负例的故意错误转换处局部关闭对应编译警告，保留全部 sanitizer 检查和其他警告；不改产品编译选项。 |
| I13 | macOS 后续对照中，插件目录内临时可执行文件在进入测试前被 SIGKILL，包括无插件且无 UBSan 的原生 C++ 基线；沙箱外重试仍出现，磁盘代码签名校验通过。同一二进制复制到仓库标准 `build/` 后正常运行，证据表明与执行位置有关。维护脚本统一在仓库 `build/llvm-c-eh/` 构建与执行，并与全量回归顺序执行；这些 SIGKILL 不计为插件或 UBSan 行为结果。 |
| I14 | 切换 Makefile 后首次无参数调度完成三平台尝试：macOS 保留 I04 失败，ARM64 通过，x64 容器因未安装 make 明确失败；脚本汇总失败并恢复容器停止状态。手工补齐测试容器的 make 后继续验收，不在维护脚本中隐式安装依赖。 |
| I15 | Makefile 移植的 macOS 链接参数曾使用 `-dynamiclib`，额外产生包含构建绝对路径的 dylib 身份记录；原独立工程使用可加载 module。保持原来的 `-bundle` 链接方式，使插件没有该身份记录，重新核对导出、依赖及 Clang 加载，不改变生成程序或链接器选择。 |
| I16 | 测试 LLD 预构建首次静态链接已去掉 `libLLVM.dylib`，仍带有 Homebrew 的 zstd 动态路径。SDK 与 LLD ELF 组件分别选择了共享 zstd；维护构建须同时使用 SDK 的静态依赖目标及上游 `LLVM_USE_STATIC_ZSTD` 选项，再校验依赖闭包，不接受携带本机绝对路径的产物。 |
| I17 | 首次正式预构建的上游 Mach-O 355 项及独立缺陷 28 项通过，安装前被维护脚本的版本文本断言拦截。上游实际输出为 `Feng UBSan test tools patch 1 LLD 22.1.8`，原断言错误地假定 vendor 位于版本后的括号中；按实际输出修正构建内校验，不改版本号或测试断言。 |
| I18 | 补丁 LLD 独立交付后 macOS 插件目录仍缺失。已有对照只运行了 `test/run.sh`，维护入口仍让部分测试沿用默认链接器，且未执行完整验收及安装。按 §7.3 在独立维护流程中分别显式选择普通／UBSan 链接器，补齐全部验证后再安装产物；Feng 接入不在本次范围。 |

I04 后续证据：完全不加载插件的独立 C++ `try/catch` 最小程序，在 Homebrew Clang
22.1.8、`-O0 -fsanitize=undefined` 与本机 Apple ld 下同样以未捕获的 `int` 终止。
插件测试对应 IR 的 invoke/landingpad 正确；对象文件包含完整的函数 unwind 范围，
最终二进制的 compact unwind 顶层末尾边界却等于最后一个函数的起点，展开在该函数
停止，尚未调用使用方 personality。换用 bundled LLD 也未通过，不能据此自动更换链接器。
该问题属于工具链组合的原生异常兼容性，继续缩小触发条件；不删用例、不关闭 sanitizer，
也不在通用 Pass 中添加针对某个测试函数的补丁。

I04 对比实验：显式 `-Wl,-ld_classic` 保留全部 UBSan 检查，在 O0/O2/O3 下完整独立
行为用例均通过；单独关闭 function sanitizer 的定位实验也通过，指向其函数前缀与
Mach-O 链接后的 unwind 范围处理。`ld-classic` 在本机仍可用，但已被 Apple 标为弃用。
用户尚未批准采用该链接器，并明确要求 Release 插件不破坏上层的 UBSan 配置与行为。
因此仅保留该定位实验，不用它替代默认链接器的验收，也不更改 Feng 或 CI 的配置。

I04 因果验证（2026-09-23）：原生 C++ 对照不加载插件，先生成目标文件，再把同一
目标文件分别交给 Apple ld 1230.1、ld-classic 与 bundled LLD 22.1.8 链接。
O0／O2 下，仅启用 `-fsanitize=function` 就足以触发默认 Apple ld 的失败；完整
UBSan 同样失败，不启用 sanitizer 或仅为定位关闭 function 检查则通过。
相同目标文件使用 ld-classic／LLD 均通过这个最小用例。function 插桩在函数入口前
增加 8 字节类型签名并标记 `.alt_entry`；目标文件中的函数展开起点和长度正确，
默认 Apple ld 生成的最终 compact unwind 顶层终止项却等于最后一个函数的入口，
未覆盖该函数体。完整 UBSan 的原生 O0 样本中，`produce` 起点为 `0x8e8`、
长度为 `0x1a4`，有效终点应为 `0xa8c`，最终表却以 `0x8e8` 结束。

仅在 `build/llvm-c-eh/i04-analysis/` 的诊断副本中，把上述表的一个 32 位终点
修正为实际代码终点并重新临时签名；不改函数机器码、插桩、异常载荷或 catch。
修正后，原生 function／完整 UBSan 的 O0 用例均成功捕获 66。对原插件完整
UBSan O0 行为程序做同样实验，将终点从 `0x3678` 修正为 `0x3828` 后，行为、
聚合 ABI、嵌套生命周期及并发断言也全部通过。该实验确认错误展开表是当前默认
链接器失败的直接原因；这是定位证据，不是产品修复，不发布手工修补的二进制。
对照矩阵、目标文件、汇编和展开表保存在该诊断目录的 `results.txt` 及相邻文件。

LLD 的边界单独记录：它通过上述最小 C++ 对照，但完整插件 UBSan 用例仍在异常 8
处失败；最终表将 `ceh_test_smoke` 的 DWARF 展开条目放在入口前 8 字节，而入口处
又是零编码，不能据最小用例通过就替换正式链接器。[LLVM #224309](https://github.com/llvm/llvm-project/issues/224309)
报告了 ARM64 Mach-O 下 function sanitizer 与异常展开相关的问题，但其组合为
LLD＋ASan＋function，不能认定与本机 Apple ld 的末尾边界缺陷完全相同。

后续链接器对照：bundled Clang 22.1.8＋bundled LLD 在不启用 UBSan 时，通过完整
独立行为用例的 O0／O2／O3、替代 personality 和 33 个协议负例；完整 UBSan 仍按
上述结果失败。本机另装的 Command Line Tools ld 1267 也未通过原生 C++ 对照及
插件完整 UBSan O0 用例，不能仅凭链接器版本较新就判定已修复。发行包按既定策略
不启用 UBSan；该问题属于开发测试组合的阻塞，不代表普通发行配置已经失败。
实验未修改 Makefile、Feng driver、默认链接器或发布产物。

触发范围对照：本机 Apple Clang 17.0.0 与 Homebrew Clang 22.1.8 对同一个
`-fsanitize=undefined` 参数展开出的检查集合不同，前者不包含 `function`，后者
包含；已保存双方 `-###` 输出。Apple Clang 编译原生 C++ 对照时，O0／O2／O3
均能捕获异常。保留 Homebrew 完整 UBSan 和原目标文件，仅在链接末尾追加一个
不执行的诊断函数，也能让原抛出函数恢复捕获：错误终点移到了新增的最后一个函数。
这验证了该复现依赖最终代码布局，不能泛化为所有 macOS UBSan 程序都会失败。
追加函数仅用于定位，不是修复方案。另已在临时目录缩小为普通 `void (*)()` 调用
及 `throw int`／`catch(int)` 的原生 C++ 程序，仍可复现，排除了聚合参数、插件协议
及自定义 runtime 的参与。目前未找到与本机条件完全一致的公开修复，不能写成
“社区无人遇到”。

自身配置复核：以不包含任何头文件的普通 C++ 程序，分别测试函数指针调用、直接
调用及仅在 `main` 内 throw／catch。编译和运行均使用 `env -i`；禁用 Homebrew
默认配置，分别配对 Xcode／Command Line Tools 的 SDK 与链接器，并另测 macOS 14
部署目标，12 组均以未捕获异常退出。显式指定实际 linker version 1267 后仍失败。
同一最小程序由 bundled Clang 22.1.8 编译、仅借用 Homebrew 的同版本 sanitizer
resource directory 时也失败；Apple Clang 的默认 UBSan 对照通过。结果位于
`build/llvm-c-eh/i04-audit/`。这轮未发现插件、Feng、头文件、环境变量或 SDK／链接器
混用造成的错误；结论仍限于本机，尚不能用它代替另一台 macOS 上的独立复现或
上游确认，亦未采用更换链接器、关闭检查等绕过措施。

依赖复核：上述最小程序未链接仓库的 `libfeng_unwind.a`。其 C++ 异常使用系统
`libc++`／`libSystem` 及系统展开器；通常另由 Clang 链入 UBSan runtime。进一步
保留 `-fsanitize=undefined` 生成的目标文件，只在诊断链接步骤省略 sanitizer
runtime（该最小目标文件没有 sanitizer handler 引用），最终仅依赖系统两库，
仍以未捕获异常退出。由此排除该最小复现由 Feng 的 libunwind 或 UBSan runtime
动态库冲突引起；不能写成完全没有系统运行库依赖。

公开资料核对（2026-09-23）：[LLVM 官方 UBSan 测试记录](https://blog.llvm.org/2013/04/testing-libc-with-fsanitizeundefined.html)
确有异常未捕获的案例，但其原因是 libc++／libc++abi 重复的类型信息，并非本机已观察到的
展开表终止边界错误。按 [Homebrew LLVM 22 的运行库配置说明](https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/l/llvm%4022.rb)
另做独立对照：最小原生程序显式链接同套 Homebrew libc++／libc++abi／libunwind，
普通构建返回 0，完整 UBSan 仍返回 134，并输出 `malformed __unwind_info`；日志位于
`build/llvm-c-eh/i04-audit/brew-runtimes-*.{compile,run}.txt`。统一这组运行库未解决本机问题，
实验没有改变正式构建配置。[LLVM #109074](https://github.com/llvm/llvm-project/issues/109074)
另记录了 macOS function sanitizer 的函数签名误报；Hermes 通过关闭该检查规避，
但这不是异常展开问题的修复，也不符合本项目保留上层 UBSan 配置的要求。
上述资料以及前述 #224309 均不能作为本机精确问题已获上游确认或已有修复的证据。

I04 候选方案验证：Homebrew Clang 22.1.8＋bundled LLD 22.1.8 配合通用选项
`-femit-compact-unwind-non-canonical`，保留完整 UBSan，通过了独立插件行为用例
O0／O2／O3、替代 personality 及 33 个协议负例。该选项只允许符合编码条件的函数
采用 compact unwind；仍需验证必须回退 DWARF 的函数，才能判断是否足以作为通用方案。
目前只用于 `build/llvm-c-eh/i04-candidates/` 下的诊断，不更改正式配置。
后续加入合法 `preserve_all` 调用约定的原生 C++ 对照，要求保存 compact unwind
无法编码的寄存器：普通构建通过，完整 UBSan 仍失败。因此上述选项组合不是完整修复。
下一步在临时工作区验证 LLD 的通用 FDE 重定位处理：函数前缀使 FDE 引用成为
“节起始符号＋偏移”，目前符号关联可能丢失该偏移；修复必须保留实际入口及原始重定位
语义，不能依靠特定函数、固定 8 字节或规避 DWARF。未经审定不替换发行工具链。
定位实验已通过：仅在目标文件副本中将 FDE 的“前缀符号＋偏移”改写为数学等价的
实际入口符号引用，保持机器码与 UBSan 检查不变；原有 LLD 随后通过完整插件行为的
O0／O2／O3 及 `preserve_all` 原生对照。无需 compact-unwind 特殊选项。
该实验验证了重定位信息丢失的方向；目标文件转换脚本仅用于定位，仍需在 LLD 内部
实现并验证修正，不能将诊断脚本作为正式链接步骤。
临时 LLD 补丁初稿通过完整 UBSan 行为和真实错误检查，但上游 Mach-O 回归检出
`eh-frame.s` 的 `ld -r` 输入兼容问题：配对重定位的第二项没有保存字段位置，
规范化时必须使用第一项的字段位置。保留上游用例与断言，修正后重新验证。

I04 已验证的修复候选（2026-09-23）：修改 LLD 22.1.8 的
`lld/MachO/InputFiles.cpp::targetSymFromCanonicalSubtractor`，从原始重定位恢复
实际目标地址，再将目标与 PC 两侧规范化为对应符号和正确偏移。它处理的是一般的
符号加偏移，不识别 Feng、插件协议、sanitizer 或固定前缀长度；同时保留已合并函数的
空符号处理。未修改 Clang、插件、使用方 runtime 或 ABI，也未关闭 UBSan 检查。
修复只改变链接期生成的异常元数据，不添加程序正常路径指令。

验证使用同一份原始目标文件：自行构建的未修改 LLD 复现失败，修复版直接链接后
通过，无需改写目标文件或额外 compact-unwind 选项。最终结果：

- 上游全部 Mach-O 测试 355 项通过，3 项按上游条件不适用，0 项失败；范围包括 ARM64、
  x64、`ld -r` 输入、不同 personality、展开表和符号处理。未宣称其他后端或缺失的
  GoogleTest 单元测试已执行。
- 原生 C++ 的 main 内捕获、聚合函数指针调用及必须采用 DWARF 的 `preserve_all`
  三类程序，在普通／完整 UBSan × O0／O2／O3 的 18 组运行中全部通过。
- 原插件完整行为在普通／完整 UBSan × O0／O2／O3 下全部通过，包括嵌套生命周期、
  聚合 ABI、并发和替代 personality；33 个协议负例保持原断言并通过。
- UBSan 的真实溢出、越界、错误函数指针、异常处理体内错误，以及终止／恢复模式均通过。
- ARM64／x64 × 4／8／16／32 字节前缀的 8 组链接对照，最终 FDE 起点均精确对应函数入口。

候选补丁与日志保存在 `third_party/llvm-c-eh/temp/i04-lld-candidate/`。
这为“macOS 使用修复后的 LLD”提供了实证方案；当时只构建了本机诊断版，未替换
系统、Homebrew、bundled 工具链，也未发布 macOS 插件。后续人工批准的独立 LLD
交付及 macOS 插件验收见 §7.2、§7.3。默认 Apple ld 组合的失败仍保留，不能因候选
验证通过而将该默认组合标为已通过。

I06 本机五次样本：无 catch 正常调用为 1.208–1.279 ns/次，有 catch 为
1.263–1.283 ns/次；完整创建、抛出、捕获并销毁为 2398.7–2491.3 ns/次。
这是特定夹具的观测值，不作为通用性能保证；该轮临时文件已受 I10 清理影响。
最终复测日志及机器码另存于插件目录 `temp/evidence/`，成本观测见 §11。
已对比同样包含两个外部调用的原生 C++ catch：其正常路径也保存 x19/x20、使用
32 字节栈，与插件样本的正常路径指令一致。插件没有额外引入此成本；“原生 EH 无
正常路径注册／检查”不等于“有 catch 和无 catch 的整个函数总是同样大小和指令数”。

## 11. 当前验收结果（更新至 2026-09-24）

本节记录实际结果，不改变 §8 的交付条件。所有组合均加载同一 host 的 Release 插件，
插件不含 sanitizer 插桩；UBSan／ASan 参数只用于编译独立使用方。
初次全平台结果记录于 §7.1 约束明确前；macOS 已按 §7.3 补齐配套工具链验收。
其他组合保留原始验证边界，不能把不同工具链组合或转译执行混作原生验收。

| 验证项 | 结果及边界 |
| --- | --- |
| 协议／IR | 三个 host 均通过 33 个 C 协议负例、14 个 IR 负例、verifier、幂等性、调用约定／属性／operand bundle／调试位置检查；x64 为 Rosetta 环境。 |
| macOS ARM64 | 已按 §7.3 完成全部 macOS 独立验收并安装产物；普通配置使用原版 LLVM LLD，UBSan 使用测试专用补丁 LLD。之前默认链接器的 I04 失败保留为历史证据，不代表原版链接器的 UBSan 缺陷已消失。 |
| Linux ARM64 GNU host | 同一 `.so` 的 bundled／宿主加载、O0/O2/O3 行为、UBSan、ASan＋UBSan、真实 UB 诊断／恢复、路径搬移后加载均通过；原生 ARM64 容器。 |
| Linux x64 GNU host | 同上项目均通过；使用 Rosetta，产物仍需原生 x64 验收，不能据此勾选全部平台完成。 |
| Linux 四个 target | 最终插件交叉编译 GNU／musl × ARM64／x64，在 O0/O2/O3 的对象、异常表及协议消除检查通过。12 组程序执行通过；ARM64 为原生执行，x64 为 Rosetta。只链接独立测试 runtime 与仓库现有第三方 LLVM libunwind，不链接 Feng runtime。 |
| 维护入口／Makefile | 统一产物名后的无参数维护入口已完成三个 host 的构建、独立验证及安装，整体返回 0，三份产物对应同一源码散列；x64 执行边界仍见上行。之前已验证显式平台＋SDK、自动发现、7 组非法参数／缺失路径、容器架构及挂载检查、停止状态恢复；早先 macOS 因 I04 失败的记录保留在实施记录中。 |
| Feng 全量回归 | 三轮沙箱外 `make test` 均返回 0，最新一轮覆盖 Makefile／多 host 调度变更后的工作区；sanitizer／普通模式的 std 均为 607/607、FCTS 均为 1508/1508，编译器、CLI／DAP、增量构建及发行脚本均通过。日志 `feng-regression-make-final.log` 完整保存。Feng 仍使用现有后端，不能代替插件测试或说明 S11 已修复。 |

macOS 原生 C++ 对照 `test/native-unwind.cpp` 使用相同 Clang、默认链接器和 O0/O2/O3：
不启用 UBSan 时，加载／不加载插件均返回 0；启用 UBSan 时，两者均以未捕获 `int`
终止（退出码 134）。测试在仓库 `build/` 执行，已排除 I13 的执行位置问题。
这证明该复现不依赖插件；不等同于默认工具链的完整 UBSan 异常组合已通过。

最终五次成本样本（ns/次）：macOS 无 catch 为 1.271–1.988，有 catch 为
1.267–1.282，完整 throw／catch／释放为 2423.6–2484.2；Linux ARM64 对应
1.277–1.280、1.260–1.286、805.4–903.1。x64 Rosetta 只保留观测，不作为原生性能结论。
macOS 样本 Mach-O `__TEXT` 汇总从 24 增至 160 字节，后者包含实际 handler 和异常相关
内容，不能把所有新增字节当成正常路径指令；正常路径栈及指令的原生对照见 I06。

当前三个 host 的产物均位于 `toolchain/llvm-c-eh/`，附源码文件散列、构建配置、
二进制散列和验证项目；macOS 的后续完整验收与安装见 §7.3。Linux x64 的原生验证
限制仍按上表记录，不因 macOS 交付而改记为通过。
日志归档在 `third_party/llvm-c-eh/temp/`（忽略目录），包括 `macos-final.log`、
`linux-arm64-final.log`、`linux-x64-final.log`、`targets-arm64.log`、`targets-x64.log`、
`native-control/results-build.txt` 与 `evidence/`；失败用例源码保留在 `test/`。
Makefile 切换后的早先入口日志为 `all-hosts-final.log`；指定平台与交叉目标复核日志为
`linux-{arm64,x64}-make-final.log`、`targets-{arm64,x64}-make-final.log`、
`maintainer-cli-checks.log`。已验证的是当前准备好的 Apple Container 环境，不声称 SDK
或容器缺失时也能直接构建；此时脚本按约定报错。

上述历史结果之后，人工已批准按 §7.2 独立维护 UBSan 专用补丁 LLD；本轮不实施
Feng 现有配置或代码接入。原生 x64 验收仍须独立记录，不降低 sanitizer 检查。

参考：[LLVM personality](https://llvm.org/docs/LangRef.html#personality-function)、
[LLVM 异常控制流](https://llvm.org/docs/ExceptionHandling.html)、
[Pass 管线接入](https://llvm.org/docs/NewPassManager.html#inserting-passes-into-default-pipelines)。
