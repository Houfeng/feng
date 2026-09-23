# C 中间代码到 LLVM 原生异常控制流的发码协议

## 1. 状态与定位

状态：协议草案，按人工确认的通用插件方向整理，待 Review；尚未实现本协议接口。

本文是这组 C 发码协议的主文档。插件面向协议工作；任何以 C 为中间表示的编译器，
在满足协议与目标异常模型的前提下，都可以作为使用方。Feng 是其中一个使用方，
其接入方案见 [S11 修复文档](./feng-release-exception-unwind-bugfix.md)。

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

## 6. 实施与验收

- [ ] Review 首版接口名、签名及语义，固定协议版本；头文件与本文保持一致。
- [ ] 从已验证原型抽离通用转换，移除硬编码的语言符号、类型与头文件依赖。
- [ ] 验证由函数配置传入 personality；插件不得内置某个使用方的 personality 名称。
- [ ] 建立不包含、不链接 Feng runtime 的独立 C 夹具与最小测试 runtime，验证具体／兜底
      catch、纯 cleanup、嵌套区域、跨函数传播及间接调用。
- [ ] 使用不同名称的 personality 和不透明类型标识验证协议复用，保持插件二进制不变。
- [ ] 覆盖协议错误、CFG 分支／循环／提前退出、内联、异常结果 SSA、调试元数据、
      `-O0/-O2/-O3`、sanitizer、跨翻译单元与标记完全消除。
- [ ] 完成声明支持的 host 构建／加载与 target 原生执行；明确工具链版本和分发依赖。
- [ ] 使用方各自验证 runtime 所有权、具体语言行为、集成性能及全量回归；通用插件
      的独立测试不能代替这些接入验收。

参考：[LLVM personality](https://llvm.org/docs/LangRef.html#personality-function)、
[LLVM 异常控制流](https://llvm.org/docs/ExceptionHandling.html)、
[Pass 管线接入](https://llvm.org/docs/NewPassManager.html#inserting-passes-into-default-pipelines)。
