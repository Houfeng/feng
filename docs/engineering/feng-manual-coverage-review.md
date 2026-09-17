# 用户手册覆盖核查与补充建议

核查日期：2026-09-17。

本文记录用户手册的发布链接问题与内容缺口，不定义新的语言规则。核查对象为
`docs/manual/en/`、`docs/manual/zh-CN/`，依据为当前规范、源码与已有兼容性用例。
下列补充内容是建议，尚未作为新教程写入手册；链接修复见第 1 节。

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

中英文各有 30 个页面，其中语言指南各有 11 章。基础绑定、数组、元组、枚举、函数、流程控制、
四种 spec 形式、泛型、异常和模块均已有介绍。主要缺口是部分完整特性没有教程，以及已经介绍的
特性缺少进阶用法和必要边界；不宜把这些现有章节整体判定为“未覆盖”。

下面以中文章节定位补写位置，英文对应路径应同步更新。规范与源码链接仅用于本工程核查记录，
后续发布到手册的正文应直接解释用法，并只链接手册内的关联章节。

## 3 第一优先级：补足主要特性的使用说明

| 主题 | 手册现状 | 建议补充 | 建议位置与依据 |
| --- | --- | --- | --- |
| 成员展开与 `@mixable` | 全文未介绍 | `...: Source;`、`...: Source = Source(...);`、`... = Source(...);` 三种形式；字段初始化差异；行为复用；显式成员与冲突；`@mixable seal` 字段和方法的访问边界 | 自定义类型中建立入口，可独立成章。依据：[类型规范 §4.2](../specifications/feng-type.md)、[函数规范 §4.3](../specifications/feng-function.md)、[成员展开用例](../../fcts/fcts_bin/src/test_mixin.ff)、[seal 展开用例](../../fcts/fcts_bin/src/test_mixable_seal.ff) |
| `@value` 值类型 | 自定义类型末尾只有一句介绍，没有声明或行为示例 | 普通对象与值类型的赋值、传参、返回差异；引用字段仍共享所指对象；实例方法的 `self`；形成方法值时的值捕获；与 tuple、`@abi` 的区别 | 扩充自定义类型，并与函数章节互链。依据：[类型规范 §4.1](../specifications/feng-type.md)、[值类型用例](../../fcts/fcts_bin/src/test_value_type.ff)、[方法值捕获用例](../../fcts/fcts_bin/src/test_value_method_capture.ff) |
| 自定义迭代器 | 流程控制只提到 `@iterable` / `@iterator` 名称 | 完整的容器与游标示例；返回具名 `(bool, E)` 元组；直接遍历游标；通过 fit 接入；每次遍历的状态与结束条件 | 扩充流程控制或单列迭代器教程。依据：[迭代器规范](../specifications/feng-iterator.md)、[迭代器用例](../../fcts/fcts_bin/src/test_iterator.ff) |
| `@friend` 定向访问 | 全文未介绍 | 为指定 type 开放 seal 字段、实例方法与静态方法；同包 fit 的使用；授权不穿透模块和类型可见性；受限工厂的例子 | 模块与可见性。依据：[可见性规范 §10.3](../specifications/feng-visibility.md)、[friend 用例](../../fcts/fcts_bin/src/test_friend.ff) |
| 内存与资源生命周期 | 分散提到自动管理、闭包捕获、终结器和 defer，缺少连贯说明 | 托管对象与外部资源的区别；强引用与闭包保活；循环引用回收；终结器的使用限制；显式关闭与 defer 的配合；C 指针 owner 保活 | 建议独立生命周期教程，与异常处理、C 互操作互链。依据：[生命周期规范](../specifications/feng-lifetime.md)、[终结器捕获用例](../../fcts/fcts_bin/src/test_finalizer_capture.ff) |
| 对象契约与交叉契约进阶 | 已有基本声明、满足关系和交叉声明 | 父 spec 与多父契约；子契约到父契约视角；交叉契约到成员视角的显式转换；static / seal requirement；通过约束类型参数访问静态能力 | 契约与 fit；泛型章节引用相关用法。依据：[spec 规范 §4](../specifications/feng-spec.md)、[父视角用例](../../fcts/fcts_bin/src/test_spec_upcast.ff)、[交叉视角用例](../../fcts/fcts_bin/src/test_intersection_projection.ff)、[静态契约用例](../../fcts/fcts_bin/src/test_spec_static_method_value.ff) |
| 联合类型与多级匹配 | 主要是 `int` / `string` 两成员示例 | 嵌套联合；`A -> B` 多级模式；多个成员的子集绑定；显式 let / var 匹配绑定；首成员默认值；进入联合时的路径选择与歧义；以对象契约或可调用契约为成员 | 模式匹配。依据：[联合类型规范 §3](../specifications/feng-union-type.md)、[流程控制规范 §3](../specifications/feng-flow.md)、[嵌套联合用例](../../fcts/fcts_bin/src/test_nested_union.ff) |
| fit 的完整使用面 | 只有普通对象适配和实例扩展方法 | 泛型 `fit Box<T>`；静态扩展；标量、string、数组、tuple、enum 的扩展示例；无块体适配；不能添加字段；孤儿适配的包内边界 | 契约与 fit。依据：[fit 规范](../specifications/feng-fit.md)、[tuple 规范 §9](../specifications/feng-tuple.md)、[数组实际扩展](../../std/std/src/collections/Array.ff)、[字符串实际扩展](../../std/std/src/text/String.ff) |
| 变长参数转发与函数值 | 已有 `T...`、Lambda 和普通对象方法值；`...expr` 仅在格式化器页提到 | `f(...items)` 预打包转发及位置、数组可写性限制；变参 callable spec；顶层函数、静态方法、spec 方法形成函数值；泛型函数值显式闭合；不同 callable spec 的显式转换；可调用零值 | 函数为主，契约章节互链。依据：[变长参数规范 §4.3](../specifications/feng-function-variadic.md)、[函数规范 §4.1](../specifications/feng-function.md)、[spec 规范](../specifications/feng-spec.md)、[变参用例](../../fcts/fcts_bin/src/test_variadic.ff)、[静态方法值用例](../../fcts/fcts_bin/src/test_static_method_value.ff) |
| 泛型组合 | 已有 type / func / method、单一对象约束和不变性 | 泛型 spec 的各形态；父契约与自约束；callable / union / intersection 约束实例；目标类型推导示例；无约束参数允许做什么；同类 type / spec 按泛参数量重载；方法值与静态成员的交叉用法 | 泛型；其他章节承接其所属特性的规则。依据：[泛型规范](../specifications/feng-generics-draft.md)、[泛型用例](../../fcts/fcts_bin/src/test_generic.ff)、[交叉泛型用例](../../fcts/fcts_bin/src/test_intersection_generic.ff) |

## 4 第二优先级：完善基础特性的边界和例子

| 主题 | 当前缺口与建议 | 依据 |
| --- | --- | --- |
| 默认值与对象初始化 | 已有标量、数组、tuple 零值，但应补对象默认值不执行构造函数、递归零值的有限性、枚举取首项、联合取首成员，以及字段初值 → 构造函数 → 对象字面量的顺序和 let 字段单次绑定示例 | [类型规范 §4.1、§5](../specifications/feng-type.md)、[enum 规范](../specifications/feng-enum.md)、[绑定规范](../specifications/feng-binding.md) |
| 模块与静态初始化 | 尚未说明首次访问触发初始化、import 不执行模块初始化、初始化依赖循环、泛型类型每个闭合实例拥有独立静态状态 | [模块规范 §5](../specifications/feng-module.md)、[类型规范 §5](../specifications/feng-type.md)、[模块绑定用例](../../fcts/fcts_bin/src/test_module_binding_semantics.ff)、[泛型静态绑定用例](../../fcts/fcts_bin/src/test_generic_static_binding.ff) |
| 数值字面量与运算 | 尚缺 `0b`、`0o`、数字分隔符、科学计数法的教程；没有运算符优先级表；显式转换只给出基本写法，应补缩窄、精度损失、bool 边界及不同类型的相等比较方式 | [表达式规范 §3、§5、§6.1](../specifications/feng-expression.md)、[内建类型规范](../specifications/feng-builtin-type.md)、[数值字面量用例](../../fcts/fcts_bin/src/test_numeric_literal.ff) |
| 数组、tuple 与 enum 进阶 | 数组需有内外层写权限独立的对照例子；tuple 需补空元组、不同具名元组间显式转换和扩展方法；enum 已介绍显式取值限制，宜增加实际声明及通过 fit 补行为的例子 | [内建类型规范](../specifications/feng-builtin-type.md)、[tuple 规范](../specifications/feng-tuple.md)、[enum 规范](../specifications/feng-enum.md) |
| 异常与清理边界 | 已有基本 throw / catch / defer；应补具体类型精确匹配、panic 与可捕获异常的区别、defer 内控制转移限制、嵌套作用域清理顺序及清理函数抛错的处理 | [异常规范](../specifications/feng-exception.md)、[defer 规范](../specifications/feng-defer.md)、[defer 用例](../../fcts/fcts_bin/src/test_defer.ff) |
| C 互操作完整流程 | 已有函数、指针、数组、回调片段；应补可运行的 C + Feng 最小项目、原生库配置、`@abi` 不改变普通对象引用语义、按值与借址的对照、opaque 指针及受限泛型 extern 的用法 | [互操作规范](../specifications/feng-interop.md)、[函数规范](../specifications/feng-function.md)、[构建规范](../specifications/feng-build.md) |

## 5 补写前应先处理的现有内容问题

1. **联合 match 表达式示例缺少 else。** 中英文的“模式匹配”和“契约与 fit”各有一个
   `let … = match …` 示例没有 else。手册自己的结果说明与
   [流程控制规范 §3.4](../specifications/feng-flow.md) 都要求表达式形式包含 else；
   [当前语义检查](../../src/semantic/analyzer.c) 在缺失时报告 `AE1108`。
   后续补写应先修正这四处示例。本次只记录，没有扩展为整本手册的示例修订。
2. **“跨类型转换必须显式”需要讲清契约视角的语境。** 类型章开头的概括没有交代
   [spec 规范](../specifications/feng-spec.md) 已允许的具体类型进入已满足契约、子 spec 进入父 spec
   的上下文适配。应与契约章节的实际赋值、参数例子一起说明，避免读者误以为都必须手写 cast。
3. **文档状态不能替代实现核查。** [@value 开发记录](./feng-value-type-dev.md) 仍写“尚未实现”，
   但当前 [Parser](../../src/parser/parser.c) 已识别 `FENG_ANNOTATION_VALUE`，并已有值类型
   FCTS。泛型规范文件也仍带草案状态。补写应按具体特性的当前规则与源码核对，不能把整份草案
   一律当成已交付或未交付；需要补齐的权威规则应先在其主规范收敛。
4. **中英文需要检查内容同步。** 两种语言的页面路径一致，但中文格式化器页的泛型间距、类型后缀
   两条说明在英文对应页中没有体现。新增教程及现有说明修订应同步，而不只是维持目录一致。

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

## 7 建议执行顺序

1. 先修正第 5 节中现有示例和表述问题，明确教程依据。
2. 补成员展开、值类型、迭代器、friend 与生命周期，使主要特性都有可跟随的入口。
3. 扩充 spec / fit、联合、泛型、函数值，再补基础边界和标准库任务教程。
4. 新增独立页面时，同步中英文手册导航与官网导航；完整示例给出必要 import、入口和预期结果，
   并验证示例及构建后的站内文件与锚点链接。

以上是补写顺序建议，不表示本次已新增这些教程，也不涉及语言行为、编译器或运行时修改。

## 8 本次验证

- `npm run docs:build` 成功，中英文共生成 60 个手册页面。
- 61 个手册 Markdown 文件的 91 个链接及图片引用检查通过；本地目标存在，语言页面的本地链接
  均保留在各自的发布目录内。
- 生成页面中共检查 5,047 处站内链接，未发现缺失文件或缺失锚点。
- 本文和文档总入口中的本地链接目标均存在。
- 本次仅修改文档，未运行编译器全量回归，也未修改已有测试用例。
