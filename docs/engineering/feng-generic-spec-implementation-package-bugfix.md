# Feng 泛型 `spec` 满足关系及跨包实现符号修复开发文档

> 状态：已完成（行为修复、依赖方向修正及全量回归均通过）
> （2026-08-19）
>
> 本文档只处理泛型 `type` / 泛型 `fit` 已声明的 object-form `spec` 名义满足关系
> 在同包和跨包使用时的正确关闭，以及跨 package-public `.ft` / `.fb` 边界时实现
> 符号的可链接性。方法级泛型 requirement 与静态字段链接分别由其他专项处理。

## 1 依据与目标

本专项以以下权威规范为准：

- [Feng 语言 `spec` 规范](../specifications/feng-spec.md)；
- [Feng 泛型规范草案](../specifications/feng-generics-draft.md)；
- [Feng 包规范](../specifications/feng-package.md)；
- [Feng 符号表规范](../specifications/feng-symbol-table.md)；
- [Feng 语言 `fit` 规范](../specifications/feng-fit.md)。

本专项必须同时满足以下实施约束：

- 同一编译分析中的同包使用和只读取 `.fb` 的跨包使用必须全部正确，不允许只修
  其中一侧；
- 同包和跨包必须复用现有名义关系查询、类型参数替换、父 spec 实例化、witness
  materialization、Symbol 选择及 package-callable 发码链路；不得按 imported 状态、
  成员名或测试场景增加特判，也不得为 type/fit 分别建立平行算法；type 与 fit 的
  关系模板来源差异只能由现有 relation source 抽象统一承载；
- 必须保持符号表规范既有的单向依赖：Exporter 消费 Semantic 结果并生成符号表，
  核心编译器只能通过不暴露 `symbol` 类型的抽象查询接口消费符号事实；Semantic
  和 Codegen 均不得包含 `symbol/*` 头文件、调用 Exporter 或进入 FT writer；
- 已经工作的非泛型关系、普通公开泛型 fit、fit 可见性、Feng 成员可见性、witness
  ABI、公开/capability 符号身份及运行时开销必须保持不变。

需要保证下面两类声明在同一编译分析中可以正确使用，并且 provider 独立打包后仍
可由 consumer 正确使用：

```feng
open spec Reader<T> {
  seal func read(value: T): T;
}

// 关系来源一：泛型 type 声明头。
open type DirectReader<T>: Reader<T> {
  seal func read(value: T): T {
    return value;
  }
}

// 关系来源二：泛型 fit。
open type FitReader<T> {}

open fit FitReader<T>: Reader<T> {
  seal func read(value: T): T {
    return value;
  }
}
```

同包使用点或只导入 `.fb` 的 consumer 必须能够：

```feng
let direct = DirectReader<int>();
let directView: Reader<int> = direct;

let fitted = FitReader<int>();
let fittedView: Reader<int> = fitted;
```

并通过现有 witness 调用声明期选中的实现方法。跨包 consumer 不得读取 provider
源码、workspace-cache `.ft`，任何使用点都不得重新进行无名义结构满足。

## 2 已确认的现状

### 2.1 现有覆盖并非空白

FCTS 已经覆盖：

- 泛型 object-form spec；
- `open fit Type<T>: Spec<T>`；
- 泛型 type 声明头上的父 spec；
- spec 参数、返回值、父视角和 witness 调用。

代表用例包括：

- `fcts/fcts_lib/src/test/lib_generic_fit_subject.ff`；
- `fcts/fcts_bin/src/test_generic_fit_subject.ff`；
- `fcts/fcts_lib/src/test/lib_generic_composition_coverage.ff`；
- `fcts/fcts_bin/src/test_generic_composition_coverage.ff`。

这些用例证明普通泛型 spec/fit 组合的主体能力存在，但不能替代一个严格的
“provider 先独立打包，consumer 只读取 package-public `.ft` 和归档库”的
CLI 回归。实施前 `test/cli/test_cli.c` 中对应严格探针仍被 `#if 0` 禁用；本专项已
恢复其中属于泛型 owner/fit 关系的部分，方法级泛型 requirement 探针继续留给独立
专项。

### 2.2 泛型 type 声明头关系存在两个已定位缺口

已确认的最小场景为：

```feng
open spec Surface<T> {
  func value(input: T): T;
}

open type Value<T>: Surface<T> {
  func value(input: T): T {
    return input;
  }
}
```

provider 可以构建和打包；consumer 从 `.fb` 导入 `Value<int>` 后，将其赋给
`Surface<int>` 报 `AE1003`。同一 `Value<int> -> Surface<int>` 关系在同包使用点也会
进入当前错误的开放成员重检路径。代码排查已经定位到两个彼此独立、触发范围不同
但均需修复的通用缺口。

第一处在实施前位于 Symbol writer：

- `FENG_DECL_TYPE` 的 `declared_specs` 当前调用不带类型参数作用域的
  `fill_declared_specs()`；
- `FENG_DECL_SPEC` 的 `parent_specs` 使用同一路径；
- `FENG_DECL_FIT` 的 target/spec 则已经调用
  `build_type_from_type_ref_with_tparams()` / `fill_declared_specs_with_tparams()`。

因此 `Surface<T>` / `Parent<Box<T>>` 中的 `T` 在 type/spec 声明关系里会被写成
普通单段 `NAMED`，而不是现有 FT 已支持的 `TYPE_PARAM_REF`。reader 的
`synthesize_type_ref()`、type/spec 的 `declared_specs` / `parent_specs` 恢复路径均已
存在；当前没有证据表明需要修改 reader 或 FT 类型节点格式。这里应让 type/spec
复用 generic fit 已经工作的 `_with_tparams` writer 路径，并用 round-trip 测试验证
reader。

第二处在实施前位于同包与跨包共用的使用点名义实例查询，而且也是原 `AE1003` 的直接
触发点：

- `type_decl_satisfies_spec_type_ref()` 先按 `FengSpecRelation` 确认 type/spec 声明
  身份，随后却再次逐个扫描 requirement；
- 该函数只接收开放的 `type_decl`，没有具体 source type ref，因而会把 type 成员
  中的 `T` 直接与 `Surface<int>` requirement 中已经关闭的 `int` 比较；
- 该路径不区分声明来自当前 source 还是 imported FT，因此不是跨包专属问题；
- 失败后的 `visible_fit_instantiates_spec_type_ref()` 只处理可见 fit 的直接 RHS，
  没有与之对应的 type 声明头实例关闭路径，也没有覆盖 fit RHS 的泛型父 spec
  关闭。

`FengSpecRelation` 当前按 subject declaration 与 spec declaration 建立索引并记录
relation source/可见性，不保存类型实参；它适合继续作为名义候选和可见性索引，
不应为本修复改造成另一套泛型关系表。精确实例证明应从 relation source 指向的
type `declared_specs` 或 fit `specs` 取得结构化关系模板，使用现有类型参数替换和
父 spec 实例化能力完成关闭，再比较完整 spec 类型身份。

不得通过同包或跨包使用点对成员重新做结构满足来绕过上述名义实例关闭，也不得
分别实现两套实例关闭算法。

### 2.3 普通泛型 fit 关系可恢复

严格 `.fb` 探针确认，以下公开路径能够通过 Semantic、Codegen 和链接：

```feng
open fit FitValue<T>: Surface<T> {
  open func value(input: T): T {
    return input;
  }
}
```

因此“泛型 type owner”和“泛型 fit”不能被描述为同一个名义关系恢复故障。
泛型 fit 的普通公开实现不是本专项要重写的路径，只作为基线回归保留。

### 2.4 泛型 fit 的 seal 实现缺少跨包符号

把同一泛型 fit 的 requirement 和实现方法改为合法的 seal 组合后：

- consumer 能恢复 fit 名义关系；
- spec coercion 和 witness 选择能够通过；
- 生成 C 能引用相应 fit 实现 wrapper/shared body；
- 最终链接缺少稳定的 `FengFitMethod...` provider 符号。

这说明该问题不是 fit 关系不可见，也不是 witness 不存在，而是泛型 callable
package surface 没有完整复用声明期已经记录的 spec implementation dependency
事实。

当前非泛型 type/fit 方法已经通过现有
`cg_member_uses_package_callable_surface()` 统一消费该事实；Symbol writer 也已经
通过同一个 declaration-time selection sidecar 把选中的 seal 实现方法作为编译器
依赖收录。因此本专项不得重新设计非泛型 package-callable 机制。

实际缺口位于现有泛型专用发码路径：

- 泛型 shared body 的 package 导出判断仍只识别公开成员和既有
  `@mixable seal static` 能力；
- 泛型 type 方法的 public/private 符号域使用同样的过窄判断；
- 泛型 fit shared symbol 的编号域没有以 package-public consumer 可恢复的成员集合
  为边界；普通 `fm<N>` 当前会统计同一 fit 中所有方法，而 `fc<N>` 只统计
  `@mixable seal static` 方法。

代码检查确认，泛型 type 自有 seal 方法在修复 2.2 的名义关系后也会经过上述
泛型专用判断。因此修复必须让这些路径复用现有统一语义事实，不能只针对
`FengFitMethod...` 名称或某个具体测试增加导出分支。

### 2.5 泛型参数的 spec 约束视角未按精确实例关闭 static 成员签名

在验证泛型 seal static requirement 的 witness 调用时，已确认以下同包最小场景
会在 Semantic 阶段错误地报 `AE1003`：

```feng
spec Surface<A> {
  static func marker(): A;
}

func invoke<U: Surface<int>>(): int {
  return U.marker();
}
```

当前 `U.marker()` 和 `U.field` 的类型推导没有使用约束中的精确
`Surface<int>` 实例，而是误把 callable 类型参数 `U` 当作 spec owner 类型参数，
按参数名替换成员类型。现有部分 FCTS 中 callable 参数与 spec 参数都恰好命名为
`T`，因而没有暴露这一错误；只要两侧改名，递归形态 `U: Surface<U>` 同样失败。

该缺口直接影响本专项要求验证的泛型 spec seal static 方法调用，且与 3.2 节的
名义关系实例关闭遵循同一个不变量：成员签名必须由“声明该成员的 spec + 从约束
实例投影得到的精确 declaring-spec 实例”关闭，参数名不构成类型身份。修复应复用
现有 `substitute_spec_member_type_ref_for_instance()` 和父 spec 实例化能力，同时覆盖
static 方法返回值、参数和 static 字段类型；不得增加按参数名、是否跨包或成员名的
特判。

本项不包含 object-form spec requirement 自身声明方法级泛参时的解析、匹配或
witness ABI；该问题仍由独立专项处理。

### 2.6 具体泛型类型实参的 witness 物化曾丢失闭合实例身份

在验证同一泛型声明的多个闭合实例时，已确认以下 Codegen 缺口：显式传入
`Direct<int>` 作为受 `Surface<int>` 约束的方法类型实参，类型解析结果本身正确；
但泛型描述符生成路径曾把该具体 `UserType` 降为只含 type declaration 的
`FengSemanticSubjectKey`，随后按声明查找已注册实例。若 `Direct<string>` 先被注册，
该查找可能错误复用其 witness。

这不是新的满足规则，也不是跨包特判。object-form spec coercion 已有
`cg_ensure_witness_instance_for_type()`，能够以完整 `UserType` 保留闭合类型实参并
选择精确 witness。泛型描述符路径必须复用该现有入口；builtin、enum、array 与
spec-value 仍使用各自现有 subject-key/slot-witness 路径。该修复不改变 descriptor
或 witness ABI，不增加运行时判断及调用开销。

### 2.7 完整合法矩阵暴露的三个既有 Codegen 链路缺口

在按本专项要求验证多闭合实例、泛型 fit 和 `@value` owner 时，还确认了三个直接
阻断这些合法场景的既有缺口。它们不是新增语言能力，也不应以测试特判绕过：

- object/intersection-form 的闭合泛型 spec 实例共享 carrier struct tag；该 tag
  不能区分 `Surface<int>` 与 `Surface<string>`。闭合实例去重必须改用现有、按实例
  唯一的 aggregate descriptor 名称；callable-form 同理使用其 closure descriptor
  名称，不改变任何生成布局或 ABI；
- 泛型 fit shell 原先晚于 type/spec member 关闭注册，而具体 fit target 的替换可能
  首次发现新的闭合 spec 实例。fit shell 必须在统一 member-registration 阶段之前
  注册，使 type、spec 与 fit 发现的闭合实例继续经过同一成员关闭链路；
- `@value` owner 的 object-form spec witness 使用 box storage，但 static witness
  slot 与 subject storage 无关。应抽取 reference owner 已有的 static method/field
  thunk 生成逻辑并由两类 witness 共用；这只复用既有 static slot、storage 与
  ensure-init ABI，不增加新的静态字段链接模型。

上述三项都只调整编译期注册、去重或代码复用顺序，不增加运行时查找、分支、数据
结构或调用层级。

### 2.8 上一轮实现曾引入 Codegen 到 Exporter 的反向依赖

上一轮行为实现完成后复查确认，`src/codegen/codegen.c` 曾直接包含
`symbol/export.h`，并在每次
`feng_codegen_emit_program()` 时调用 `feng_symbol_build_package_selection()`。该入口
会重新构建完整 Symbol graph，再进入 FT writer 的 package-public declaration
selection。这不是“核心通过抽象接口查询符号事实”，而是 Codegen 直接调用
Exporter，并间接依赖 FT writer 的选择实现。

该实现违反 [Feng 符号表规范](../specifications/feng-symbol-table.md#40-分层边界) 和
[外部包 use 支持交付](./feng-use-pkg-delivered.md#2-关键架构事实) 已确立的依赖方向：

```text
Semantic result -> Exporter -> symbol table/query implementation
                                      |
                                      v
                         neutral abstract query
                                      |
                                      v
                                  Codegen
```

允许的依赖是 Exporter 依赖核心编译器的数据模型，以及核心编译器依赖中立的抽象
查询接口；禁止出现 `Codegen -> symbol/export`、`Codegen -> FT writer` 或把 Symbol
具体类型放进核心公共 API。

正常 CLI 编译当时还会先调用 Symbol export，然后在 Codegen 内再次构建 Symbol
graph 和 package selection。该重复工作只影响编译期，不增加生成程序的运行时
开销，但仍属于不应保留的职责耦合和重复计算。

当前修正已在 Codegen 公共 API 中增加不暴露 Symbol 类型的只读 package symbol
query；Symbol selection 改为消费外层已经构建的 graph，CLI 使用同一 graph 完成
符号表输出和 selection 构建，再把独立 selection 适配为抽象 query 注入 Codegen。
`src/codegen/` 和 `src/semantic/` 均不再包含 `symbol/*` 或调用 `feng_symbol_*`。
Symbol graph 在 selection 建立后即可释放；selection 只借用仍由 Semantic analysis
持有的 source declaration identity，并由 CLI 持有到本次 Codegen 返回。

在改为由外层始终注入 package symbol query 后，定向 CLI 验证还暴露了一个调用方
契约错误：原代码曾用“Codegen options 非空”间接判断 debug context 是否存在；
query 使非调试构建也需要 options 后，该判断会在 `.fd` 写出路径解引用空的
debug context。当前修正使用独立的 `emit_debug_info` 条件统一控制 line directive、
`.fd` 路径及 `.fd` 写出，不能继续把一个可扩展 options 容器是否存在当作某项
独立能力的开关。该修正只恢复原有非调试行为，不改变调试格式、Codegen ABI 或
生成程序开销。

## 3 正确性模型

### 3.1 声明时做结构证明

provider 分析以下声明时：

```feng
open type Value<T>: Surface<T> { ... }
open fit FitValue<T>: Surface<T> { ... }
```

必须在开放泛型作用域中完成现有结构满足检查，并记录：

- 关系来源及其 owner；
- target 类型模板；
- spec 类型模板及类型参数位置；
- 每个 requirement 选中的 type/fit 实现成员；
- 父 spec 闭包中的同类选择。

这一步继续应用公开 requirement 与 seal requirement 的既有实现可见性兼容
规则。非法关系必须在 provider 声明时拒绝，不能推迟到 consumer。

上述实现选择使用现有 `SpecImplementationSelection`，只存在于 provider 当前编译
过程，不序列化到 `.ft`。关系本身及其结构化泛型使用仍通过现有 relation、
`FT_ATTR_DECLARED_SPECS` 和类型节点表达；两类事实不得混为一套新的 wire 映射。

### 3.2 使用时做名义关系关闭

同包或跨包使用点使用 `Value<int>` / `FitValue<int>` 时，都只执行同一流程：

1. 通过 `FengSpecRelation` 查询当前位置可见的 relation source；
2. 从 relation source 对应的 type `declared_specs` 或 fit `specs` 取得直接关系
   模板，按具体 subject 的类型实参关闭直接 spec 使用，再逐层关闭父 spec 使用；
3. 按现有语义类型身份精确比较关闭后的 spec 实例；
4. 名义证明成立后，复用现有 witness materialization，从当前声明或
   package-public `.ft` 已恢复的实现骨架中解析并绑定对应 slot，取得既有发码所需
   的实现声明和链接信息。

当具体泛型 type 实参进入第 4 步时，witness materialization 必须保留完整闭合
`UserType` 身份；只含 declaration 的 subject key 不能区分同一声明的多个闭合实例，
不得用于选择具体 type witness。

通过泛型参数的约束视角访问 static 成员时，还必须以该参数声明中的完整 spec
类型引用作为实例，并在成员来自父 spec 时先投影到实际 declaring spec，再关闭
成员参数、返回值或字段类型。该过程只消费已经解析的名义约束，不重新证明结构
满足。

第 4 步中的成员解析只负责恢复 witness 所需的实现信息，不得反过来作为满足关系
证明。使用点不重新进行一般结构满足，也不因为 imported `.ft` 中恰好存在同名
seal 方法而建立新关系；外包自定义 spec/fit 仍不能据此选择 imported type 的 seal
成员。provider 的 `SpecImplementationSelection` 不存在于 consumer，本文也不新增
序列化 selection 映射。同包与跨包只允许在关系模板和实现声明的来源上不同，关系
关闭及 witness 选择算法必须相同。

关闭结果必须满足以下不变量：

- `Value<int>: Surface<int>` 只证明 `Value<int>` 满足 `Surface<int>`，不得证明其
  满足 `Surface<string>`；
- 若 `spec Child<T>: Parent<Box<T>>`，则 `Child<int>` 的父关系必须关闭为
  `Parent<Box<int>>`，不得退化为只比较 `Child` 与 `Parent` 的声明身份。

泛型实例继续保持不变性；这里的模板替换不是泛型 variance。

### 3.3 泛型 fit 可见性

`fit Type<T>: Spec<T>` 只有在该 fit 按现有 module/import 规则对使用点可见时才
参与名义关系查询。关闭泛型 fit 模板不扩大 fit 的可见范围，也不允许外包自行
定义 spec/fit 后选择 imported type 的 seal 成员。

本专项不得把 fit 关系挂到 type 上成为无条件全局关系，也不得因为二进制中存在
实现符号就绕过 fit 可见面。

### 3.4 选中 seal 泛型方法的 package callable surface

对每个进入 package-public `.ft` 的名义关系，声明期选中的泛型实现方法必须
进入现有 package callable surface：

```text
package_callable(member) =
    existing_public_callable(member)
    or existing_mixable_seal_static_callable(member)
    or selected_by_package_public_spec_relation(member)
```

`selected_by_package_public_spec_relation` 是 provider 编译期事实：成员必须由现有
declaration-time selection sidecar 选中，并且该 selection 所属名义关系按既有
package-public 规则可导出。对 fit 必须采用 orphan export 降级后的最终可见性。
该 selection sidecar 不写入 `.ft`，consumer 也不查询 provider sidecar。关系导出
资格和选中成员进入 package-public 初始集合的判定必须复用现有统一规则，不能在
各个 Codegen 分支重复实现另一套可见性判断。

consumer 不重新计算上述 provider 谓词。名义证明和 witness materialization 完成
后，现有 `SpecWitness` 继续保存当前具体 `(subject, spec)` 的精确 `impl_member`，并
用于 witness slot 绑定；它不能单独承担 package shared symbol 的全局分类。原因是
`SpecWitness` 按 coercion site 按需物化，而同一泛型 owner 可能有多个已导出关系：
consumer 只使用后一个关系时，前一个关系的 witness 可以不存在，但 provider 的
package 方法序号仍已同时计入两个关系选中的实现。

上述公式描述的是 provider 侧现有非泛型 package-callable 判定已经具备、泛型路径
必须复用的统一不变量，不是要求增加第二套 classifier。consumer 如何无歧义恢复
同一 package symbol 域，见 3.5 节。最终统一判定必须覆盖：

- 泛型 type owner 的实例方法和静态方法；
- 泛型 fit 的实例方法和静态方法；
- 使用 owner 泛参但没有方法级泛参的方法；
- 被选中实现成员在 provider 中的既有 shared body，以及 consumer 闭合实例继续
  生成的既有薄 wrapper；
- 父 spec requirement 的实现选择。

selection 选择的是声明成员，不是生成 wrapper。provider shared body 必须取得既有
package linkage；consumer 薄 wrapper 继续按现有闭合实例路径生成，并引用同一个
稳定 shared symbol，不要求把薄 wrapper 另行导出。分类抽象不得按方法级泛参数量
另设分支，但 object-form spec 方法级泛型的解析、匹配、witness ABI 和行为验证仍由
独立专项负责，不属于本专项完成条件。

不得使用“seal 方法已经出现在 `.ft`”作为选中证明，因为 `.ft` 还可能因
`@mixable` 或 reified callable dependency 收录其他 seal helper。provider 是否把
成员作为 spec 实现依赖收录，唯一依据仍是声明期统一满足选择；consumer 只能在
名义关系证明成立后按 3.2 节恢复 witness 信息。

### 3.5 稳定链接身份

被选中的 seal 泛型方法复用对应公开泛型方法的现有 shared body、薄 wrapper、
函数描述符和 reified dependency 路径。provider 与 consumer 必须只根据
package-public `.ft` 可恢复的声明事实得到一致符号身份：

- owner 类型参数顺序；
- type/fit 来源身份；
- 实例/static 形态；
- 重载/成员稳定序号；
- 完整未实例化签名；
- reified aggregate/type/callable dependencies。

普通私有声明顺序、workspace-cache 中额外成员、consumer 使用了哪些关系以及
witness 本地物化顺序均不得影响符号名称或序号。不得为 spec seal 泛型实现新建
专用符号前缀或专用 thunk ABI。

代码排查确认，当前文档原先假定的“provider 使用 declaration-time selection、
consumer 使用按需 witness selection 即可归一符号域”并不充分。`SpecWitness` 是
按 coercion site 物化的局部结果，不能决定 owner 全部 package 方法的编号集合。

现有 Symbol writer 已经拥有可复用的最终事实：

- initial tree 收录公开方法、`@mixable seal static` 方法和
  `is_spec_implementation_dependency` 方法；
- dependency closure 继续收录被已选声明引用的 reifiable callable dependency；
- 普通 type/fit seal 方法不会进入 package-public FT；
- writer 在最终选择后仍按声明树顺序输出所选成员，consumer imported AST 保留该
  相对顺序。

因此本专项不新增 FT flag/attr，也不让 consumer 推断某个成员“满足了哪个 spec”。
package-public 方法收录闭包继续由 Symbol 层拥有；provider Codegen 只能通过核心
公共头文件定义的只读抽象查询接口询问某个 source declaration 是否进入该闭包，
不得包含 Symbol 头文件、持有 Symbol graph/selection 类型，或自行调用 Exporter/FT
writer。shared body 是否取得 package linkage 仍只由 3.4 节既有 package-callable
语义判定决定，不因某个方法仅作为 reifiable dependency 被收录就擅自扩大链接面。
consumer 对 imported type/fit 而言，只把 package-public FT 中实际存在的私有方法
视为 package-public 中额外收录的编译器依赖成员。后者只决定 C 符号身份，不参与
Feng 成员查找、spec 满足证明或 witness slot 选择，因而不会扩大 seal 可见性。

抽象查询接口只表达 Codegen 所需的中立事实，例如“该 source declaration 是否属于
当前 package 的稳定符号域”；`user`、索引结构和查询实现均由外层持有，接口中不得
出现 `.ft`、writer、provider 或 Symbol graph 类型。编译驱动负责在 Semantic 成功
后构建一次 Symbol graph、完成符号表输出、取得基于同一 graph 的只读查询并注入
Codegen，且保证查询及其借用的 source declaration identity 生命周期覆盖本次发码；
query 若已形成独立快照，不要求继续持有 Symbol graph。
`feng_codegen_emit_program()` 的直接
调用者若要生成依赖 package 符号域的库代码，也必须显式注入该抽象查询；Codegen
不得通过隐藏回退重新构建 Symbol graph，也不得复制一套近似的收录算法。

为避免新增 spec seal 实现扰动已经工作的公开泛型符号，package 编号顺序固定为：

1. 先按现有规则编号原有公开/既有 capability 域成员；
2. 再按源声明相对顺序编号 package-public 中额外收录的编译器依赖成员；
3. 未进入 package-public 收录闭包的 provider-local 方法不参与上述编号。

据此：

- 泛型 type 继续复用现有 `m<N>` / `i<N>` 前缀；原有公开方法、构造器、finalizer
  和既有 `@mixable seal static` 保持当前 `m<N>` 编号，额外收录的编译器依赖成员
  追加在该域尾部；未收录私有方法继续使用 `i<N>`；
- 泛型 fit 的原有普通公开方法继续使用 `fm<N>`，`@mixable seal static` 继续使用
  现有 `fc<N>`；额外收录的编译器依赖成员追加在 `fm<N>` 的公开方法之后；普通
  provider-local 方法必须进入与 `fm<N>` 不相交的通用 internal 域（采用
  `fi<N>`），否则同名重载可能与重新编号后的 `fm<N>` 冲突。

`fi<N>` 是所有 provider-local 泛型 fit 方法的内部域，不是 spec seal 专用 ABI；它
只作为现有 `cg_fit_method_shared_cname()` 链路内的内部命名域，不新增发码、调用或
链接链路，不进入 package contract，也不增加运行时开销。package selection 的所有
权仍属于 Symbol 层；核心只能消费中立查询结果。不得改为 FT 新标志、consumer
结构扫描、Codegen 内部重建选择闭包或具体测试名称特判。

### 3.6 `.ft` 边界

符号表规范已经要求：

- 公开泛型 type/spec/fit 的类型参数和未实例化关系进入 package-public `.ft`；
- 已导出关系选中的 seal 实现方法作为编译器依赖进入 `.ft` 并保持 seal；
- 泛型 callable 保留完整签名和 reified dependencies。

本专项优先修复这些既有通用事实的写入、读取或消费，不增加逐槽 witness plan、
spec seal 专用 flag/attr、运行时数据、运行时 relation 表或新的结构满足机制。
consumer 无需恢复 provider 的 selection 原因，只需消费 writer 已经输出的最终
package 方法集合。

`SpecImplementationSelection` sidecar 继续只存在于 provider，不序列化逐
requirement 映射；consumer 继续通过结构化名义关系与已收录实现骨架构造现有
witness。

## 4 范围边界

### 4.1 本次包含

- 同一编译分析中的泛型 type/fit 名义关系关闭、父 spec 关闭和 witness 调用；
- `open type Owner<T>: Spec<T>` 及 spec 右侧的多类型参数、重排/嵌套类型
  实参，例如 `Owner<T, U>: Spec<Box<U>, T>`；
- `open fit Owner<T>: Spec<T>` 的现有可见关系，以及其 spec 右侧的同类类型实参
  替换；fit 左侧仍必须按现有规范逐位置直接引用目标 type 的全部泛参，不允许
  特化、增删、重排或嵌套改写；
- 泛型 type/fit 中被公开关系选中的 seal 实例方法和静态方法；
- 通过泛型参数的精确 object-form spec 约束访问上述静态方法，以及同一既有约束
  视角下 static 方法签名和 static 字段类型的实例关闭；
- 父 object-form spec 闭包及其逐层结构化类型实参替换；
- 引用 type 与 `@value type` 的现有泛型 owner 路径；
- provider 独立 pack、consumer 只读 `.fb` 的编译、链接和运行。

### 4.2 本次不包含

- object-form spec requirement 自身声明方法级泛参时的解析、匹配和 witness ABI；
- 泛型 owner 的方法级约束引用 owner 类型参数时的 Codegen 开放 spec 实例注册，
  例如 `Host<T>.invoke<U: Surface<T>>`；同一最小用例已在当前分支和干净 `HEAD`
  编译器上得到相同 `CE0031`，确认是既有独立缺陷，而非本专项引入的回归；
- spec static 字段实现的 storage/ensure 链接；
- 重写已经工作的非泛型 type/fit package-callable 路径；
- 新增结构满足、variance、运行时关系查询或 witness 缓存；
- 新增逐 requirement/slot selection 映射或修改 `.ft` wire 格式；
- 修改 fit 导入/可见性规则；
- 修改 type/spec seal 访问、`@friend` 或 `@mixable` 授权；
- object-form spec 方法值；
- 与正确性无关的泛型单态化或 witness 性能优化。

## 5 性能与兼容性

- 关系关闭和实现选择全部在编译期完成；
- 正常编译流程只构建一次本地 Symbol graph；Codegen 通过外层注入的只读抽象查询
  消费稳定符号域事实，不得再次构建 Symbol graph 或进入 FT selection；
- 生成程序不增加运行时名称查找、关系搜索、成员扫描、缓存、锁或分配；
- witness 布局和每次调用间接层级不变；
- 选中 seal 方法只改变 provider package symbol 的链接能力和 consumer 对该既有
  符号的编译期引用身份，不改变其 Feng visibility；
- 普通公开泛型 type/fit 路径保持现有发码和开销；
- 在等价 relation/fit 可见面条件下，同包与跨包的合法/非法关系集合必须一致；修复
  前已经工作的同包直接泛型 fit、非泛型 type/fit 和公开泛型成员不得改变语义或
  生成程序行为；
- 公开/capability 泛型方法保持现有 C 符号身份和编号；经 Review 允许新增的
  provider-local 内部域只改变非 package contract 的内部 C 身份，不得改变调用
  行为；
- 未被导出关系选中的 seal helper 不得新增二进制公开符号。

如果正确修复需要修改 runtime ABI、增加运行时分支或把 fit 关系变为无条件全局
关系，必须暂停并由人工决策。

## 6 测试要求

### 6.1 编译器测试

- Semantic：provider 开放泛型 type/fit 声明期满足检查与选择 sidecar；在同一
  analysis 中覆盖直接泛型 type、直接泛型 fit、泛型父 spec 的正向关闭，以及类型
  实参不一致的拒绝；
- Semantic：以不同的 callable/spec 类型参数名覆盖 `U: Surface<U>` 与
  `U: Surface<int>` 的 static 方法返回值、参数及 static 字段类型，确认按完整约束
  实例而不是按参数名关闭；父 spec 成员必须先投影到 declaring-spec 实例；
- Symbol：package-public `.ft` 对泛型 type `declared_specs`、泛型 spec
  `parent_specs`、泛型 fit target/specs 中 `TYPE_PARAM_REF`，以及选中 seal 方法签名
  和 reified dependencies 的 round-trip；同时验证普通 seal 方法不进入
  package-public 收录闭包；
- Imported Semantic：对与同包 Semantic 相同的关系矩阵，验证 consumer 关闭
  `Owner<int> -> Spec<int>`；同时验证类型实参不一致和父 spec 嵌套替换不会退化为
  只比较声明身份，确保 imported 与 current-source 只更换事实来源、不更换算法；
- Codegen：分别验证同包和跨包的泛型 type/fit 被选中 seal 实例/static 方法使用
  现有 shared body、薄 wrapper 和稳定符号；在选中方法之前插入未导出的普通 seal
  方法，并让同一 owner 的多个已导出关系只在 consumer 使用其中一个，验证符号身份
  不受私有成员或 witness 物化集合影响；非泛型、公开泛型和现有 capability
  package-callable 基线保持不变；
- Codegen：同一泛型 type/fit 声明同时存在多个闭合实例，且目标闭合实例只作为
  显式方法类型实参出现时，验证泛型描述符使用该实例的精确 witness，不得按 type
  declaration 误取先注册的其他实例；
- 依赖边界：`src/codegen/` 与 `src/semantic/` 不包含 `symbol/*` 头文件、不调用
  `feng_symbol_*`；Codegen 单测通过假实现注入抽象查询，CLI 隔离用例通过 Symbol
  实现注入同一接口，二者对相同 source declaration 得到一致稳定符号域；
- 编译流程：验证正常 CLI 路径复用同一个本地 Symbol graph 完成导出和 Codegen
  查询，不在 Codegen 内重新构建 graph 或 package selection；
- CLI：provider 独立 `pack` 后，consumer 只依赖 `.fb` 完成 coercion、witness
  调用和链接。

负向覆盖必须包括：

- 未导入 fit 所在 module 时关系不可用；
- 类型实参不一致时不得通过；
- 无关普通 seal helper 不进入 package-public callable surface；
- consumer 外包自定义 spec/fit 不能选择 imported type seal 成员；
- `.ft` 中因 `@mixable` 或 reified dependency 存在的 seal 方法不被误当作满足选择；
- 泛型 fit 中普通 package-callable、`@mixable seal static` 与 provider-local 方法
  使用互不冲突的既有/内部符号域。

### 6.2 FCTS

在 `fcts_bin` 增加同包行为组，并在 `fcts_lib -> fcts_bin` 增加对应跨包行为组；两组
至少覆盖：

- 泛型 type 声明头关系；
- 泛型 fit 关系；
- type/fit seal 实例方法和静态方法；
- 父 spec 的嵌套类型实参替换；
- reference/value owner；
- 多个闭合实例，确认 witness 和返回值不串实例。

两组对相同合法场景必须得到一致结果。FCTS 之外必须保留隔离 `.fb` CLI 用例，
防止本地 workspace-cache 或依赖源码偶然掩盖 package-public 事实缺失。

## 7 TODO 与实施顺序

- [x] **验证（根因定位）**：已确认 type/spec Symbol writer 未使用现有类型参数
  builder；reader 已具备对应类型节点与关系列表恢复路径；consumer 的直接 type
  查询缺少具体 source type ref 并错误地重新扫描开放成员，且该查询由同包和跨包
  共用；现有 fit 精确匹配仅覆盖直接 RHS。根因和代码位置见 2.2 节。
- [x] **Review 决策（已批准，2026-08-18）**：复用现有 package-public 方法收录
  闭包，不增加 FT 标志；该集合只用于稳定符号域/编号，linkage 继续使用 3.4 节
  既有判定；package-public 中额外收录的编译器依赖成员采用追加编号，泛型 fit 的
  provider-local 方法采用通用 `fi<N>` 内部域。实施必须保持现有
  公开/capability 符号身份和同包调用行为。
- [x] **实际变更（Symbol 关系模板写出）**：让泛型 type 的 `declared_specs` 与泛型
  spec 的 `parent_specs` 复用 `fill_declared_specs_with_tparams()`；保持现有
  `FT_ATTR_DECLARED_SPECS` 和类型节点 wire，不新增另一套 relation 表示。
- [x] **验证（Symbol reader 与 round-trip）**：增加 type 声明头和 spec 父关系的
  `TYPE_PARAM_REF` round-trip，验证现有 reader 可直接恢复；该项原则上只增加测试，
  只有测试证明 reader 的通用恢复路径确有缺口时才转为实际修复并暂停 Review。
- [x] **实际变更（名义关系实例关闭）**：在现有
  `type_ref_satisfies_spec_type_ref()` 及 relation source 查询链路中收敛统一的关系
  模板关闭：直接 type 从 `declared_specs`、可见 fit 从 `specs` 取得 head，使用具体
  subject 类型实参关闭，并复用现有类型参数替换、父 spec 递归实例化与语义类型
  相等能力。`FengSpecRelation` 继续只承担 declaration 候选索引和 fit 可见性过滤；
  不新增 current/imported、type/fit 各自独立的查询链路。
- [x] **实际变更（移除使用时结构证明）**：让 `type_ref -> spec type ref` 的满足查询
  使用上一步的精确闭合名义关系；不再由
  `type_decl_satisfies_spec_type_ref()` 扫描 type/fit 成员证明满足。声明期
  `verify_type_satisfies_spec()` 与名义证明之后的 witness slot 绑定保持现状。
- [x] **实际变更（约束视角成员签名关闭）**：让泛型参数的 object-form spec 约束
  保留并消费完整 constraint type ref；static 成员来自父 spec 时先复用现有父实例
  投影，再通过 declaring spec 的类型参数关闭方法参数、返回值和字段类型。删除
  当前把 callable 类型参数误作 spec owner 参数的按名替换，不新增跨包/static
  特判，也不扩展到 requirement 自身的方法级泛型 ABI。
- [x] **验证（约束视角成员签名）**：增加 callable/spec 参数异名、闭合约束、递归
  约束和父 spec 约束用例；验证 static 方法调用及 static 字段读写的类型检查，并
  保持现有同名参数用例行为不变。
- [x] **验证（同包名义关系）**：增加同一 analysis 内直接泛型 type、直接泛型 fit、
  type/fit 泛型父 spec、嵌套/重排类型实参的正负用例；确认 `Value<int>` 只能满足
  精确关闭后的 spec 实例，并确认现有同包直接泛型 fit 行为不变。
- [x] **验证（普通泛型 fit 基线）**：固定现有公开泛型 fit 的隔离 `.fb` 正向用例；
  对照同包矩阵验证 fit direct head 与泛型父 spec 的精确关闭。该项只验证，不重写
  fit visibility、已经工作的 fit target 参数映射或同包直接 fit 路径。
- [x] **实际变更（Symbol package 方法收录闭包）**：Symbol writer 的 initial tree
  与 dependency closure 已统一得到 package 方法选择，覆盖既有公开、mixable、
  spec implementation dependency 与可达 reifiable callable dependency；该闭包
  继续由 Symbol 层拥有，不授权核心直接调用其构建入口。
- [x] **实际变更（核心抽象查询接口）**：在核心公共 API 定义只读、实现无关的
  package symbol query，只接受 opaque `user` 与核心 source declaration identity，
  返回稳定符号域事实；接口不得暴露 Symbol、FT、provider 或 writer 类型，也不得
  改变现有 `FengSemanticImportedModuleQuery` 的模块查询职责。
- [x] **实际变更（Symbol 查询适配）**：让 Symbol 层基于已经构建的同一个 graph
  提供上述查询的实现，复用 writer 的最终 package-public selection；不得从
  `FengSemanticAnalysis` 再构建第二个 graph，也不得在 CLI 或 Codegen 复制闭包
  算法。
- [x] **实际变更（外层生命周期与注入）**：编译驱动在 Semantic 成功后只构建一次
  Symbol graph，用它完成公开/workspace FT 输出并取得只读查询，再通过 Codegen
  options 注入；独立 query selection 的生命周期覆盖发码，graph 在 selection
  建立后即可释放，二者均由外层负责。
- [x] **实际变更（解除 Codegen 反向依赖）**：删除 `src/codegen/codegen.c` 对
  `symbol/export.h`、`FengSymbolPackageSelection` 和 `feng_symbol_*` 的全部依赖；
  provider 成员符号域只查询注入接口，imported declaration 继续消费抽象模块查询
  已恢复的核心声明事实，不新增 current/imported 语义规则。
- [x] **实际变更（复用泛型 callable linkage 判定）**：让泛型 type/fit 的 provider
  shared body 导出复用 3.4 节现有 package-callable 判定，使 spec selection 选中的
  seal 方法取得 package linkage；不得把“仅被 Symbol 依赖闭包收录”直接等同于
  package linkage，保持非泛型路径与 reified callable 既有行为不变。
- [x] **实际变更（复用泛型 package 符号域判定）**：provider 使用 package 方法
  收录结果，consumer 对 imported type/fit 仅按 package-public FT 实际保留的方法
  集合恢复 package symbol 域；该分类不得进入成员可见性、名义满足、witness 选择
  或 linkage 授权。consumer 薄 wrapper 保持现有本地闭合路径并引用同一 shared
  symbol；不按 fit/type、实例/static 或具体成员名增加分支。
- [x] **实际变更（稳定符号域）**：使选中 seal 泛型 type 方法与泛型 fit 方法的
  public/private 符号域、shared symbol 和成员序号只由 package-public `.ft` 可恢复的
  owner、fit、成员及签名事实决定。保持原有公开/capability 成员编号不变，将
  package-public 中额外收录的编译器依赖成员追加到对应 package 域；泛型 type
  复用 `m/i`，泛型 fit 复用 `fm/fc`，provider-local 方法进入已批准的通用 `fi`
  域。不得计入未收录的普通 seal 成员，不得增加 spec-seal 专用符号前缀或 thunk
  ABI。
- [x] **验证（reified dependencies）**：只验证被选中实现现有 shared body、薄
  wrapper 所需的 aggregate、managed type 与 callable dependencies 已完整往返；
  同时放置一个因 reifiable callable dependency 收录的 seal 方法，验证其参与
  package 符号域稳定编号但不被误当作 spec 实现或新增 linkage 授权。只有确认存在
  通用事实丢失时才列为实际修复，不扩展到其他 reified 行为，也不为当前用例增加
  依赖特判。
- [x] **实际变更（精确闭合 witness 物化）**：具体泛型 type 作为受 object-form
  spec 约束的方法类型实参时，泛型描述符复用现有
  `cg_ensure_witness_instance_for_type()`，不再把完整 `UserType` 降为只能表示声明
  身份的 subject key；其他 subject 类别保持既有路径。
- [x] **验证（精确闭合 witness 物化）**：同一泛型 type/fit 的其他闭合实例先注册，
  目标实例只出现在显式方法类型实参中，验证生成 C 绑定目标闭合实例 witness，且
  不串用先注册实例。
- [x] **实际变更（闭合实例与注册顺序）**：闭合泛型 spec 按现有实例唯一 descriptor
  身份去重，并把泛型 fit shell 注册提前到统一 member-registration 之前；不新增
  carrier、descriptor、witness 或 fit 专用链路。
- [x] **实际变更（共用 static witness thunk）**：抽取 reference owner 已有的
  subject-independent static method/field thunk，由 reference 与 boxed value witness
  共用；保持既有 slot、storage、ensure-init 和调用 ABI。
- [x] **验证（完整 Codegen 合法矩阵）**：以多个闭合 spec 实例、generic fit、
  mapped parent 和 `@value` owner 的实例/static witness 覆盖上述编译期链路，并验证
  生成 C 可编译、跨包行为与同包一致。
- [x] **验证（编译器用例）**：补齐 Semantic、Symbol、Codegen、Imported Semantic
  和隔离 `.fb` CLI 正负用例；同包与 imported Semantic 使用同一场景矩阵，并恢复
  现有 `#if 0` 探针中属于本专项的部分。
- [x] **验证（FCTS）**：补齐泛型 type/fit seal 实现的同包与跨包可观察行为用例，
  两侧对相同合法场景必须一致。
- [x] **验证（行为修复基线）**：依赖方向问题发现前的实现已在沙箱外执行
  `make test`；UBSan 与正常 `-O2 -Werror` 两轮均通过，FCTS 两轮均为 799/799，
  CLI、std、性能、增量构建、发布脚本和 bundled package 检查全部通过。该结果只
  作为行为基线，不代表当前专项已经完成架构验收。
- [x] **验证（抽象接口与依赖边界）**：Codegen 单测使用假查询验证 package-base、
  package-dependency 与 internal 域；Symbol/CLI 用例验证真实 adapter 返回同一结果；
  静态检查核心目录不存在 `symbol/*` include 和 `feng_symbol_*` 调用；同时验证非调试
  编译即使注入 query 也不会进入 `.fd` 写出路径。
- [x] **验证（修正后全量回归）**：已在沙箱外重新执行 `make test`；UBSan 与默认
  `-O2 -Werror` 两阶段均通过，std 两轮均为 579/579，FCTS 两轮均为 799/799，
  smoke、CLI、性能约束、增量构建、发布脚本、bundled package 与预构建工具链检查
  全部通过。

实施应分成“关系模板/名义关闭”和“package callable/稳定符号”两个明确阶段；前一
阶段不依赖 3.5 节的符号事实决策。本专项已完成 2.8 节依赖方向修正并重新回归；
后续由 [泛型 owner 方法约束引用 owner 泛参修复](feng-generic-owner-method-constraint-bugfix.md)
处理 `Host<T>.invoke<U: Surface<T>>`。object-form spec 方法级泛型继续由
`feng-object-form-spec-generic-method-bugfix.md` 单独处理。三个问题
不得合并实施，也不得为了统一改动而改变 fit visibility、既有普通泛型 fit target
参数映射或非泛型 callable 路径。
