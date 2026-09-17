# 用户手册覆盖核查与补充建议

核查日期：2026-09-17。

本文记录用户手册的发布链接问题与内容缺口，不定义新的语言规则。核查对象为
`docs/manual/en/`、`docs/manual/zh-CN/`，依据为当前规范、源码与已有兼容性用例。
第 3、4 节的 16 项语言主题已同步补齐到中英文手册，第 6 节的标准库教程仍待安排。
链接修复见第 1 节，已有内容问题的处理结果见第 5 节，补写中确认的实现限制见第 9 节。
独立 mixin 章节、spec seal 可见性及 fit 孤儿规则的后续补充见第 10 节。

## 1 发布链接核查

官网的 [Eleventy 配置](../../website/eleventy.config.js) 只从 `docs/manual/` 生成中英文手册，
不会生成规范或工程文档页面。修复前，61 个手册 Markdown 文件中共有 93 个链接及图片引用，
其中有两处相对链接越过手册边界：

| 页面 | 原目标 | 处理 |
| --- | --- | --- |
| [中文异常处理](../manual/zh-CN/language/error-handling.md) | `../../../specifications/feng-exception.md#2-throw-语句` | 移除规范链接，直接解释匿名 catch 重抛的适用范围 |
| [英文异常处理](../manual/en/language/error-handling.md) | 同上 | 与中文同步修复 |

生成页面检查还发现，[中文代码格式化](../manual/zh-CN/tooling/formatter.md) 指向代码风格第 5.1、
5.2 节的两个锚点不存在。原链接使用 `#51-…`、`#52-…`，官网生成的标题 ID 使用
`#5-1-…`、`#5-2-…`。这两处已改为链接到代码风格页面，并在文字中保留节号与标题。

[文档总入口](../README.md) 原来要求“工程文档和使用手册应引用规范”，容易被理解为手册也应添加
规范链接。现已区分工程文档的规范引用与手册的使用说明；手册链接边界仍统一引用
[手册内容边界](../manual/README.md#内容边界)，不再另设规则。

## 2 覆盖结论

当前中英文各有 31 个页面，其中语言指南各有 12 章。基础绑定、数组、元组、枚举、函数、流程控制、
四种 spec 形式、泛型、异常和模块原本已有介绍。首轮针对缺少教程的完整特性，以及既有特性的
进阶用法和必要边界扩充了 10 对现有页面；后续将 mixin 拆为独立章节，并补充 spec seal 可见性
及 fit 孤儿规则。上述调整没有改变语言行为。

下面以中文章节定位已补内容，英文对应路径已同步更新。规范与源码链接仅用于本工程核查记录；
发布到手册的正文直接解释用法，并只链接手册内的关联章节。

## 3 第一优先级：主要特性使用说明（已补齐）

| 主题 | 补充前情况 | 已补内容 | 手册位置与依据 |
| --- | --- | --- | --- |
| 成员展开与 `@mixable` | 全文未介绍 | 三种展开形式与初始化差异；纯字段混入；行为复用；显式成员与冲突；`@mixable seal` 字段和方法的直接授权；TUI 中的实际复用方式 | [成员展开（mixin）](../manual/zh-CN/language/mixins.md)独立章节。依据：[类型规范 §4.2](../specifications/feng-type.md)、[函数规范 §4.3](../specifications/feng-function.md)、[成员展开用例](../../fcts/fcts_bin/src/test_mixin.ff)、[seal 展开用例](../../fcts/fcts_bin/src/test_mixable_seal.ff) |
| `@value` 值类型 | 自定义类型末尾只有一句介绍，没有声明或行为示例 | 普通对象与值类型的赋值、传参、返回差异；引用字段仍共享所指对象；实例方法的 `self`；形成方法值时的值捕获；与 tuple、`@abi` 的区别 | [自定义类型](../manual/zh-CN/language/user-defined-types.md)“值类型与方法值捕获”。依据：[类型规范 §4.1](../specifications/feng-type.md)、[值类型用例](../../fcts/fcts_bin/src/test_value_type.ff)、[方法值捕获用例](../../fcts/fcts_bin/src/test_value_method_capture.ff) |
| 自定义迭代器 | 流程控制只提到 `@iterable` / `@iterator` 名称 | 完整的容器与游标示例；返回具名 `(bool, E)` 元组；直接遍历游标；通过 fit 接入；每次遍历的状态与结束条件 | [流程控制](../manual/zh-CN/language/control-flow.md)“编写自定义迭代器”。依据：[迭代器规范](../specifications/feng-iterator.md)、[迭代器用例](../../fcts/fcts_bin/src/test_iterator.ff) |
| `@friend` 定向访问 | 全文未介绍 | 为指定 type 开放 seal 字段、实例方法与静态方法；同包 fit 的使用；授权不穿透模块和类型可见性；受限工厂的例子 | [模块与可见性](../manual/zh-CN/language/modules-and-visibility.md)“用 @friend 定向开放 seal 成员”。依据：[可见性规范 §10.3](../specifications/feng-visibility.md)、[friend 用例](../../fcts/fcts_bin/src/test_friend.ff) |
| 内存与资源生命周期 | 分散提到自动管理、闭包捕获、终结器和 defer，缺少连贯说明 | 托管对象与外部资源的区别；强引用与闭包保活；循环引用回收；终结器的使用限制；显式关闭与 defer 的配合；C 指针 owner 保活 | [自定义类型](../manual/zh-CN/language/user-defined-types.md)“内存与资源生命周期”。依据：[生命周期规范](../specifications/feng-lifetime.md)、[终结器捕获用例](../../fcts/fcts_bin/src/test_finalizer_capture.ff) |
| 对象契约与交叉契约进阶 | 已有基本声明、满足关系、父契约视角与交叉声明 | 多父契约；交叉契约到成员视角的显式转换；static / seal requirement；通过约束类型参数访问静态能力 | [契约与 fit](../manual/zh-CN/language/contracts-and-fit.md)“多父契约、静态能力与 seal requirement”。依据：[spec 规范 §4](../specifications/feng-spec.md)、[父视角用例](../../fcts/fcts_bin/src/test_spec_upcast.ff)、[交叉视角用例](../../fcts/fcts_bin/src/test_intersection_projection.ff)、[静态契约用例](../../fcts/fcts_bin/src/test_spec_static_method_value.ff) |
| 联合类型与多级匹配 | 主要是 `int` / `string` 两成员示例 | 嵌套联合；`A -> B` 多级模式；多个成员的子集绑定；显式 let / var 匹配绑定；首成员默认值；进入联合时的精确优先及确定性路径选择；以对象契约或可调用契约为成员 | [模式匹配](../manual/zh-CN/language/pattern-matching.md)“嵌套联合、多级模式与子集绑定；以对象契约和可调用契约为联合成员”。依据：[联合类型规范 §3](../specifications/feng-union-type.md)、[流程控制规范 §3](../specifications/feng-flow.md)、[嵌套联合用例](../../fcts/fcts_bin/src/test_nested_union.ff) |
| fit 的完整使用面 | 只有普通对象适配和实例扩展方法 | 泛型 `fit Box<T>`；静态扩展；标量、string、数组、tuple、enum 的扩展示例；无块体适配；不能添加字段；孤儿适配的包内边界 | [契约与 fit](../manual/zh-CN/language/contracts-and-fit.md)“泛型与其他类型的 fit”。依据：[fit 规范](../specifications/feng-fit.md)、[tuple 规范 §9](../specifications/feng-tuple.md)、[数组实际扩展](../../std/std/src/collections/Array.ff)、[字符串实际扩展](../../std/std/src/text/String.ff) |
| 变长参数转发与函数值 | 已有 `T...`、Lambda 和普通对象方法值；`...expr` 仅在格式化器页提到 | `f(...items)` 预打包转发及位置、数组可写性限制；变参 callable spec；顶层函数、静态方法、spec 方法形成函数值；泛型函数值显式闭合；不同 callable spec 的显式转换；可调用零值 | [函数](../manual/zh-CN/language/functions.md)“转发变长参数；函数值的来源、转换与零值”。依据：[变长参数规范 §4.3](../specifications/feng-function-variadic.md)、[函数规范 §4.1](../specifications/feng-function.md)、[spec 规范](../specifications/feng-spec.md)、[变参用例](../../fcts/fcts_bin/src/test_variadic.ff)、[静态方法值用例](../../fcts/fcts_bin/src/test_static_method_value.ff) |
| 泛型组合 | 已有 type / func / method、单一对象约束和不变性 | 泛型 spec 的各形态；父契约与自约束；callable / union / intersection 约束实例；目标类型推导示例；无约束参数允许做什么；同类 type / spec 按泛参数量重载；方法值与静态成员的交叉用法 | [泛型](../manual/zh-CN/language/generics.md)“泛型契约与约束组合；自约束、目标推导与声明重载”。依据：[泛型规范](../specifications/feng-generics-draft.md)、[泛型用例](../../fcts/fcts_bin/src/test_generic.ff)、[交叉泛型用例](../../fcts/fcts_bin/src/test_intersection_generic.ff) |

## 4 第二优先级：基础特性的边界和例子（已补齐）

| 主题 | 已补内容 | 手册位置 | 依据 |
| --- | --- | --- | --- |
| 默认值与对象初始化 | 对象默认值不执行构造函数、递归零值的有限性、枚举首项和联合首成员默认值；字段初值 → 构造函数 → 对象字面量顺序与 let 字段单次绑定 | [自定义类型](../manual/zh-CN/language/user-defined-types.md)“默认值与初始化顺序” | [类型规范 §4.1、§5](../specifications/feng-type.md)、[enum 规范](../specifications/feng-enum.md)、[绑定规范](../specifications/feng-binding.md) |
| 模块与静态初始化 | 首次访问初始化、import 不执行模块初始化、实际执行的初始化依赖循环、闭合泛型类型的独立静态状态 | [模块与可见性](../manual/zh-CN/language/modules-and-visibility.md)“模块绑定与静态初始化” | [模块规范 §5](../specifications/feng-module.md)、[类型规范 §5](../specifications/feng-type.md)、[模块绑定用例](../../fcts/fcts_bin/src/test_module_binding_semantics.ff)、[泛型静态绑定用例](../../fcts/fcts_bin/src/test_generic_static_binding.ff) |
| 数值字面量与运算 | 进制前缀、数字分隔符、科学计数法；优先级表；整数缩窄与精度损失、bool 边界、不同数值类型的相等比较 | [类型](../manual/zh-CN/language/types.md)“数值字面量与显式转换”“运算符优先级” | [表达式规范 §3、§5、§6.1](../specifications/feng-expression.md)、[内建类型规范](../specifications/feng-builtin-type.md)、[数值字面量用例](../../fcts/fcts_bin/src/test_numeric_literal.ff) |
| 数组、tuple 与 enum 进阶 | 数组各层写权限对照；空元组与不同具名元组显式转换；枚举显式取值、首项默认值与 tuple / enum 扩展行为 | [类型](../manual/zh-CN/language/types.md)“数组每一层的写权限”“空元组、具名转换与枚举行为” | [内建类型规范](../specifications/feng-builtin-type.md)、[tuple 规范](../specifications/feng-tuple.md)、[enum 规范](../specifications/feng-enum.md) |
| 异常与清理边界 | 具体类型精确匹配、panic 与可捕获异常的区别、defer 控制转移限制、嵌套清理顺序、在辅助函数内部捕获清理异常 | [异常处理](../manual/zh-CN/language/error-handling.md)“精确匹配与清理边界”；当前实现限制见第 9 节 | [异常规范](../specifications/feng-exception.md)、[defer 规范](../specifications/feng-defer.md)、[defer 用例](../../fcts/fcts_bin/src/test_defer.ff) |
| C 互操作完整流程 | C 静态库与 Feng 绑定打包、应用消费、原生库配置、@abi 引用语义、按值与借址、opaque 资源释放、固定 C 签名的泛型 extern | [C 互操作](../manual/zh-CN/interop/c-interop.md)完整项目及后续两节；当前实现限制见第 9 节 | [互操作规范](../specifications/feng-interop.md)、[函数规范](../specifications/feng-function.md)、[构建规范](../specifications/feng-build.md) |

## 5 现有内容问题的处理结果

1. **match 按实际实现说明。** [当前语法检查](../../src/parser/parser.c) 对表达式形式始终要求
   else，即使已列出联合类型的全部成员；缺失时报告 `SE1103`。独立语句形式可省略 else。
   中英文“模式匹配”和“契约与 fit”的四处表达式示例已补齐 else，模式匹配章节同步明确这一边界。
2. **类型适配按实际实现说明。** [当前语义检查](../../src/semantic/analyzer.c) 与
   [已有父契约视角用例](../../fcts/fcts_bin/src/test_spec_upcast.ff) 支持具体类型进入已声明满足的
   对象契约位置，以及子对象契约进入直接或间接父契约位置。中英文类型章已区分已确定类型间的
   显式转换、字面量贴合与基于已声明关系的契约视角建立。父子关系在 object-form spec 定义时已
   显式声明，因此自动父视角投影不视为隐式类型转换；语言层分类统一见
   [spec 规范 §4](../specifications/feng-spec.md)。契约章节已有绑定、传参和返回示例，并说明仅有
   相同结构不足以建立满足关系，交叉契约到组成契约仍须显式转换。该修订不改变语言行为。
3. **@value 已实现。** [@value 开发记录](./feng-value-type-dev.md) 的标题、状态、历史背景和
   §9.17 已修正，不再把已交付能力标为尚未实现。值类型方法值捕获的实施记录见
   [对应开发文档](./feng-value-type-method-value-capture-dev.md)，行为证据见
   [值类型用例](../../fcts/fcts_bin/src/test_value_type.ff) 与
   [方法值捕获用例](../../fcts/fcts_bin/src/test_value_method_capture.ff)。第 3 节的 @value 使用教程本轮已补齐；
   其他草案的状态仍应按其具体实现分别核对。
4. **中英文结构与覆盖面对齐。** 首轮已核对两种语言的 30 对页面及各级标题、示例、关联链接和说明。
   英文补齐了元组创建与默认值、解构类型标注限制、普通构造不适用于 tuple、泛型语法高亮、
   格式化范围和指针后缀风格；中文补齐了异常载荷中的 @abi / @value 说明及分支表达式的关联入口。
   两种语言同步修订本节涉及的语义说明。后续同步要求统一记录在
   [手册多语言规则](../manual/README.md#多语言规则)。

## 6 标准库层面的补充

这些是库的使用教程，与语言语法缺口分别安排：

- **基础类型与常用契约**：`Option<T>` / `None`、`Union<…>`、`Tuple<…>`、`Action` / `Func`、
  `Error`、自定义 `Display` 与 `Hashable`。当前手册提到了部分契约名称，但没有完整的实现教程。
  依据：[基础库源码](../../std/std/src/basic/)、[Hashable](../../std/std/src/collections/Hashable.ff)。
- **事件**：监听、取消监听、触发、监听上限与复制后的共享行为。
  依据：[Event 规范](../specifications/feng-std-event.md)、[Event 实现](../../std/std/src/basic/Event.ff)。
- **线程与同步**：线程创建和等待、Mutex、CondVar、Once、WaitGroup 的典型协作流程。
  依据：[线程模块](../../std/std/src/thread/)。
- **文本进阶**：rune / grapheme 实际遍历示例、解析与格式化的边界。
  Unicode 视图目前只有概念描述。依据：[文本模块](../../std/std/src/text/)。
- **TUI**：最小应用、布局、输入、焦点及事件处理。
  依据：[TUI 模块](../../std/std/src/tui/)、[已有标准库用例](../../std/std_test/src/test_tui.ff)。

以下内容不能直接作为“已可用功能”补写：

- `async` / `await`、`@awaitable`、属性、运算符重载、自定义注解仍有明确的候选或未实施文档，
  需先确认交付状态。参见 [异步草案](./feng-async-awaitable-draft-2.md)、
  [属性草案](../specifications/feng-prop-draft.md)、[运算符草案](./feng-operator-overload-draft.md)、
  [自定义注解草案](./feng-custom-annotation-draft-2.md)。
- [Future.await](../../std/std/src/async/Future.ff) 当前直接抛出未实现错误；
  [Promise.resolve / reject](../../std/std/src/async/Promise.ff) 仍是空实现。
- [网络模块](../../std/std/src/net/) 中四个文件目前只有模块声明，没有对应的客户端或服务端 API。
- [StringBuilder.ff](../../std/std/src/text/StringBuilder.ff) 目前只有模块声明，不能作为已可用的字符串构建 API 介绍。
- 迭代器规范提到通用 `Iterator<T>` 及组合子，但当前
  [Iterator.ff](../../std/std/src/collections/Iterator.ff) 只声明了 `IteratorResult<T>`。
  自定义迭代协议可以补教程，不能顺带宣称 `Iterator<T>.map/filter/take` 已可用。
- `@runtime` 是私有运行时接口，不应纳入面向普通开发者的公开功能教程。

## 7 首轮交付与后续

1. 第 3、4 节的 16 项主题已覆盖，集中扩充现有 10 对中英文页面。生命周期说明放在自定义类型
   章节，并与异常处理、C 互操作互链；页面路径与两种语言的导航保持对应。
2. 新增 19 个可独立编译运行的 Feng 程序和一个完整 C + Feng 项目；提供 import、入口、必要文件、
   构建命令和预期输出。两种语言使用相同程序，说明、章节次序与使用限制同步。
3. 第 9 节两项实现限制保留记录，手册采用已验证的写法。本轮不修改规范语义、编译器、运行时或
   已有测试用例。
4. 第 6 节的标准库任务教程仍待单独安排，未纳入本轮完成范围。

## 8 首轮验证（独立 mixin 章节补充前）

- `npm run docs:build` 成功，中英文共生成 60 个手册页面。
- 30 对页面的标题层级、代码块顺序与关联链接一致；每种语言各有 215 个标题、190 个代码块。
  新增完整程序及 C 项目各文件的代码与实际验证输入逐一核对，两种语言一致。
- 新增 19 个完整 Feng 程序均通过当前本地编译器的编译、链接和运行，实际输出与手册列出的结果
  一致；C 项目通过本机 C 编译、静态归档、`feng pack` 和应用端 `feng run`，输出符合预期。
  本轮运行验证平台为 `macos-arm64`；Linux 命令替换说明未在本轮执行。
- 第 5 节已有的 match / 契约视角正反例验证结果保留；本轮没有改变这些语言行为。
- 61 个手册 Markdown 文件的 138 个链接及图片引用检查通过；本地目标存在，手册没有指向规范或
  工程文档的链接，语言页面的本地链接均保留在各自的发布目录内。
- 生成页面中检查 5,188 处站内 href、120 处站内资源引用，其中 920 处带锚点；未发现缺失文件或
  缺失锚点。
- 本文和文档总入口中的本地链接目标均存在；`git diff --check` 通过。
- 本次仅修改文档，按仓库规则未运行编译器全量回归。

## 9 补写中确认的实现限制

以下是在当前仓库编译器、`macos-arm64` 上复现的实现差异，不作为新的语言规则；本轮仅记录，
不修改代码。对应手册已经同时说明当前限制与验证可用的写法。

### 9.1 defer 块内直接 try/catch 未能捕获异常

以下程序可以编译，但运行输出 `feng: panic: uncaught exception (unwind reason=5)` 并以
`SIGABRT` 终止，未由块内的具名 catch 处理：

```feng
module manual_cleanup_limit;
/** Raises a recoverable Feng exception. */
func close_resource() { throw "close failed"; }
/** Reproduces catch handling directly inside deferred cleanup. */
func cleanup() {
  defer {
    try close_resource() catch error: string {}
  }
}
/** Runs the minimal deferred-catch case. */
func main(args: string[]) { cleanup(); }
```

[异常处理教程](../manual/zh-CN/language/error-handling.md)使用 `close_safely()` 在辅助函数内部
捕获异常，再由 defer 调用该函数；完整示例正常运行并输出清理失败信息。

### 9.2 跨包直接调用有字段 @abi 类型的 extern 发码失败

在 [C 互操作项目](../manual/zh-CN/interop/c-interop.md)中，若直接公开
`point_shift_value(Point): Point` 和 `point_shift_pointer(Point*)` 两个 extern，让应用包直接
调用前者并以 `&point` 调用后者，绑定包可以打包，但应用生成的 C 缺少导入类型的完整
`Point__AbiLayout` 以及 `__abi_value`、`__abi_box`、`__abi_ptr` 辅助声明，宿主 C 编译失败。

教程将这两类 C 调用留在声明 `Point` 的绑定包中，向应用公开普通 Feng 函数 `shift_value` 和
`shift_in_place`。该项目已经完成打包和跨包运行，按值副本、普通引用共享与借址修改结果均符合
手册列出的输出。这是当前可用的包装方式，不表示已修复直接跨包 ABI 调用。

## 10 mixin、seal 可见性与孤儿规则的后续补充

- [成员展开（mixin）](../manual/zh-CN/language/mixins.md)独立成章，依次介绍纯字段混入、
  `@mixable` 方法、直接 mix 授权、显式成员与多层展开，以及 TUI 用法。自定义类型章节保留
  简要入口；中英文首页、目录树和官网导航已同步更新。
- TUI 说明依据 [Button](../../std/std/src/tui/widgets/Button.ff)、
  [View](../../std/std/src/tui/widgets/View.ff)、[Container](../../std/std/src/tui/widgets/Container.ff)
  与 [VStack](../../std/std/src/tui/widgets/VStack.ff) 的现有实现。示例使用实际 TUI 类型，
  按 VStack 的方式定制样式准备，并区分直接 mix 方法授权与 Widget 契约的 seal 访问权限；
  这不是第 6 节所列的完整 TUI 应用教程。
- [模块与可见性](../manual/zh-CN/language/modules-and-visibility.md)新增 spec seal 专节，说明
  实现类型之间通过契约协作、fit 实现上下文、成员原声明 spec，以及具体类型和契约视角各自的
  可见性。[契约与 fit](../manual/zh-CN/language/contracts-and-fit.md)保留契约满足方面的说明，
  并区分同包 fit 选用 seal 实现与通过 spec 视角访问成员的不同条件。
- fit 章节单列孤儿规则，说明包内可用、包外不导出、`open fit` 的提示，以及纯方法扩展不受
  孤儿规则限制。示例为标准库 Rect 适配 Display，同时提供可按普通规则导出的 area 扩展。
- 同一特性在关联章节中可以按不同重点介绍，并保持必要解释与链接；这项编写约定已记录到
  [手册内容边界](../manual/README.md#内容边界)。

本次验证结果：

- 新增 6 个完整示例（mixin 4 个、spec seal 1 个、孤儿规则 1 个）均在 `macos-arm64` 上通过
  当前本地编译器的编译、链接和运行，输出符合手册。孤儿适配构建输出取消导出的 info 提示。
- 31 对页面的标题层级、代码块顺序及本地链接顺序一致；每种语言各有 223 个标题、195 个代码块。
  新增 6 个示例的中英文代码与实际验证输入逐一一致。
- `npm run docs:build` 成功，生成 62 个手册页面。手册 Markdown 的 158 个链接及图片引用、
  生成站点的本地链接与锚点检查通过；手册链接未越过发布边界。
- 仅修改 docs 和 website 导航配置，未修改编译器、运行时或已有测试，按仓库规则未运行全量回归。
