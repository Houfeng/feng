# Feng object-form `spec` 方法级泛型未来支持分析文档

> 状态：未来能力，当前实施停止（2026-08-19）。按照
> [方法级泛参暂不支持备注](./feng-object-form-spec-method-generic-restriction-note.md)，
> Parser 继续保留语法与 AST，Semantic 将暂时拒绝 object-form `spec` 方法自己
> 声明泛参。本文保留已经完成的通用修复和动态 witness descriptor 路由分析，
> 不再作为当前支持能力的实施清单。
>
> 本文档只跟踪 object-form `spec` 方法级泛型从声明解析、满足检查到
> witness 调用的完整正确性修复。`spec seal` 的可见性与授权语义继续以
> [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 为准；本文不重新
> 定义该语义。

## 1 依据与目标

语言语义以以下权威规范为准：

- [Feng 语言 `spec` 规范](../specifications/feng-spec.md)；
- [Feng 泛型规范草案](../specifications/feng-generics-draft.md)；
- [Feng 语言函数规范](../specifications/feng-function.md)；
- [Feng 符号表规范](../specifications/feng-symbol-table.md)。

本文形成时的规范曾规定 object-form `spec` 的实例方法和静态方法支持方法级泛型，
并试图修复以下声明与调用。当前阶段已决定由 Semantic 暂时拒绝这类 requirement；
下例只作为未来恢复能力时的目标，不代表当前合法语义：

```feng
open spec Mapper {
  func map<T>(value: T): T;
  seal static func hiddenMap<T>(value: T): T;
}

open type MapperImpl: Mapper {
  func map<U>(value: U): U {
    return value;
  }

  seal static func hiddenMap<U>(value: U): U {
    return value;
  }
}
```

未来恢复该能力时，本专项的目标是让 object-form `spec` 方法级泛型完整进入**现有泛型
callable 与静态 descriptor tree 模型**，并覆盖：

- 实例方法与静态方法；
- 无修饰（公开）与 `seal` requirement；
- `type` 自有实现与 `fit` 方法实现；
- 非泛型 owner、泛型 `spec` owner、泛型 `type` owner 和泛型 `fit`；
- 显式类型实参、现有类型实参推导、泛型约束和返回值；
- 同包调用以及只依赖 package-public `.ft` / `.fb` 的跨包调用。

本专项不得增加运行时 descriptor 构造、缓存、查找、反射或额外动态分派。

## 2 当前事实与问题根因

### 2.1 已修复：Semantic 声明解析进入统一 callable 作用域

Parser 已经把方法级类型参数写入 `FengCallableSignature.type_params`，无需
增加语法或 AST。原 Semantic 解析 object-form `spec` 成员时直接解析方法参数和
返回类型，没有复用 type/fit 方法所走的统一 `resolve_callable()`，因此签名中的
方法类型参数会被当作普通具名类型查询并报告 `AE1013`：

```feng
open spec Identity {
  func identity<T>(value: T): T;
}
```

`resolve_callable()` 已经统一完成：

- 方法泛参压栈与退栈；
- owner 泛参与方法泛参的遮蔽检查；
- 方法泛参约束解析；
- 参数和返回类型解析；
- callable 上下文恢复。

`c66d2e93` 已让 object-form `spec` 方法签名复用该入口，并正确设置、恢复当前
spec owner 与 callable member 上下文。该修改同时收敛了非泛型 spec 方法的声明
解析，必须保留。当前暂不支持规则落地后，Parser/AST、统一解析入口和既有
`AE1013` 检查均原样保留；Semantic 只在编译 spec 的成员方法签名验证处增加
更早的专用限制诊断，因此该声明不会继续进入上述通用解析路径。

### 2.2 已修复：满足检查表达方法泛参身份

原满足检查只比较显式参数和返回类型，没有先检查方法泛参 arity，也没有把
requirement 与实现方法的泛参按声明位置建立对应关系。正确匹配必须同时处理：

- 方法泛参 arity；
- 按位置建立的 alpha-equivalent 映射，泛参名称不参与身份；
- spec owner 实参与实现 owner 实参替换；
- 参数、返回值与变长参数形态；
- “实现约束不得比 requirement 更严格”的约束兼容方向。

`c66d2e93` 已实现统一 callable 满足比较：先检查泛参 arity，再按声明位置做
alpha-equivalent 映射，同时替换 spec owner 实参，并按“实现约束不得更严格”检查
约束方向。满足验证、快速满足查询和最终 witness 选择复用该比较抽象。

该抽象也承担普通非泛型 requirement 的签名比较，因此必须保留。当前限制落地后，
其中方法泛参分支不再使 spec 泛型方法成为合法声明，只作为未来恢复能力的已完成
基础。

### 2.3 已部分修复：调用表示与 witness ABI 尚缺动态 descriptor 路由

原 `UserSpecMember` 只记录普通参数、返回类型、static 等事实，没有记录：

- 方法泛参声明顺序及约束；
- owner 泛参 + 方法泛参的双层未闭合签名；
- 方法级泛型参数在稳定 callable ABI 中的对应位置。

因此原实现解析签名中的 `T` 时会报告
`CE0032: codegen: unknown type 'T'`。原 object-form witness 方法槽位也只有：

```text
[subject] -> 显式参数 -> [out]
```

而现有 type/fit 泛型方法共享体已经接收：

```text
[subject] -> [owner descriptor] -> function descriptor
          -> 方法泛参 descriptors -> 显式参数 -> [out]
```

`bc4e2e67` 已完成以下部分：

- resolved-callable 记录 requirement、witness spec surface、subject 和已解析的方法
  类型实参；
- spec 调用按方法类型实参替换参数与返回类型；
- `UserSpecMember` 在 spec owner + 方法泛参双层作用域中解析声明签名；
- 实例/static 泛型 spec 方法拥有带 function descriptor 与方法泛参 descriptor 的
  witness 槽位和 type/fit thunk 骨架；
- 原声明作用域泛参索引不再直接查询另一个 caller 作用域的 descriptor。

因此，签名和实现体都不需要 reified dependency 的基础用例已经可以完成
Semantic 与 Codegen。但该提交没有解决“动态 witness 所选实现 + 同一实现
descriptor 子树”的一致路由，完整能力仍不正确。

2026-08-19 的实施过程曾隔离确认：声明解析与满足检查通过后，
`subject.identity<int>(value)` 的返回类型仍保留 requirement 中未替换的方法泛参，
并在期望 `int` 的返回位置报告 `AE1003`。`bc4e2e67` 已通过统一
resolved-callable 类型实参事实和调用签名替换修复这一阶段；它不是当前剩余问题。

在上述 Semantic 调用事实补齐后，实施过程中的同一隔离用例进一步到达 Codegen，并在
object-form `spec` 参数上调用 `subject.identity<int>(value)` 时报告：

```text
CE0237: codegen: missing generic descriptor for erased storage
```

实际代码排查确认，`CE0237` 这一处历史失败本身不是 2.5 的动态 descriptor 路由
问题，而是旧 spec witness 调用分支直接使用 `UserSpecMember` 开放签名中的泛参索引，
并把该索引交给只查询
**当前 caller 泛参作用域**的 `cg_generic_param_desc_name()`。前者属于 spec owner +
spec 方法声明作用域，后者属于当前函数/方法作用域，二者索引即使数值相同也没有
身份关系：

- 非泛型 caller 中没有对应 descriptor，因而报告 `CE0237`；
- 泛型 caller 中若恰好存在相同下标，旧路径可能误取 caller 的其他泛参
  descriptor，属于作用域身份错误，不能视为合法复用。

`bc4e2e67` 已先使用 resolved-callable 中的方法类型实参闭合 spec 方法的参数和
返回类型，再按既有泛型 callable 规则生成并传递方法泛参 descriptor，从而消除
这处跨作用域索引误用。其通用不变量必须保留：任何声明作用域中的开放泛参索引
都不得直接用于查询另一个 caller 作用域的 descriptor。

### 2.4 实现身份静态已知时，既有 callable dependency tree 可以闭合

当前共享体的泛型调用链已经采用静态 descriptor tree：

1. Semantic 为共享 callable 收集 `FengReifiableDepSet.callable_deps`；
2. `cg_emit_closed_callable_fdesc()` 在最终具化点递归闭合被调用 callable 的
   descriptor 子树；
3. 子树按固定 slot 写入
   `FengFunctionDescriptor.reified_callable_deps`；
4. 共享体通过 `_desc->reified_callable_deps[slot]` 取得下一层 descriptor，
   传给下一共享体；下一共享体重复相同逻辑。

该链路可以表达任意层次、且每条 callable 边的实际实现身份能在最终具化点确定的
合法嵌套调用。例如 `invoke<T: Surface>()` 最终以 `T = A` 具化时，编译器能够
把 requirement `Surface.identity<U>` 映射为 `A.identity<U>`，再以本次方法实参
递归闭合 `A.identity` 的实现依赖树。

当前仍缺少的是：通过 object-form `spec` requirement 发生的方法调用，没有形成
与普通 type/fit 方法调用等价的 **witness-dispatched callable dependency 节点**。

具体表现为：

- `FengResolvedCallableKind` 和 `FengReifiableCallableDep` 当前只表达已直接
  解析到 function/type/fit/constructor 的 callable；
- spec 视角调用没有保存 requirement 成员身份、spec owner 实例、方法类型
  实参及 witness 分派来源；
- `collect_from_expr()` 因而不能把该调用加入 caller 的 callable dependency
  集合；
- Codegen 也就无法为该调用分配稳定 slot；当实现身份静态已知时，也无法递归
  闭合实际实现方法的 dependency 子树并把 descriptor 传入 witness。

补齐该节点是必要修复，但只足以覆盖最终具化点能够确定实际 witness 实现的路径。
它不能单独解决下一节所述的动态 object-form 路由。

### 2.5 动态 object-form witness 缺少实现 descriptor 路由

以下示例完整展示当前尚未解决的问题：

```feng
spec Surface {
  func identity<T>(value: T): T;
}

func invokeView(subject: Surface) {
  subject.identity<int>(1);
}
```

`invokeView()` 中的调用点只知道：

- requirement 是 `Surface.identity`；
- 方法实参已经闭合为 `int`；
- 实际实现由运行时 `witness` 决定。

假设存在两个实现，且两个实现共享体使用不同的 reified dependency：

```feng
type A: Surface {
  func identity<U>(value: U): U {
    let box = Box<U>(value);
    return box.value;
  }
}

type B: Surface {
  func identity<U>(value: U): U {
    let list = List<U>();
    // ...
  }
}
```

`A.identity<int>` 的 `FengFunctionDescriptor` 子树需要包含 `Box<int>`，而
`B.identity<int>` 的子树需要包含 `List<int>`。两棵实现树本身都能由现有
descriptor emitter 正确生成；缺失的是 `invokeView()` 如何随运行时 witness
在两棵树之间进行一致路由：

```text
subject.witness == A witness
    -> A.identity
    -> A.identity<int> descriptor tree（包含 Box<int>）

subject.witness == B witness
    -> B.identity
    -> B.identity<int> descriptor tree（包含 List<int>）
```

当前 witness 槽位只根据 witness 选择实现函数。调用点生成一个固定的
`FengFunctionDescriptor *`，witness thunk 再把同一指针原样传给被选中的实现
共享体。这样虽然函数可以在运行时分派到 A 或 B，却没有同步分派与该函数匹配的
实现 descriptor 树。requirement 自身没有实现方法体依赖；把 requirement 的空树
传给 A 或 B 也不能替代实际实现树。

该问题也不能归因于“方法泛参尚未闭合”：

- object-form `spec` 的类型级泛参在形成和传入 spec 值时已经闭合；
- `identity<int>` 的方法级泛参也已经在调用点闭合；
- 静态 descriptor 生成点缺少的是“本次运行时 witness 将选择 A 还是 B”这一
  实现身份，因而无法把已经闭合的 `int` 绑定到正确的实现共享体依赖树。

例如把上例扩展为 `Surface<X>`、`A<X>` 与 `B<X>`，即使最终已经得到
`Surface<string>` 且方法实参是 `int`，仍可能在运行时选择
`A<string>.identity<int>` 或 `B<string>.identity<int>`。类型级泛参闭合不会消除
动态实现选择。

因此，问题的准确表述是：

> 方法泛参能够闭合，实际实现也能由 witness 在运行时选择；但当前 ABI 只路由
> 实现函数，没有把同一实现对应的闭合 descriptor 树与函数一起路由。

本问题与 2.3 的泛参索引作用域误用相互独立。索引误用必须作为编译器正确性 Bug
修复；修复索引后仍需解决本节的实现 descriptor 路由。

### 2.6 callable-form `spec` 当前为什么没有同一问题

该缺口并非 object-form 这种值表示本身造成，而是“运行时动态选择实现 + 调用时
方法泛型 + 实现方法体具有不同 reified dependency tree”的组合问题。

当前 Feng callable-form `spec` 不允许一个已经形成的 callable 值在调用时再次
指定函数或方法泛参。泛型来源必须先显式闭合，再形成普通 callable 值：

```feng
spec Identity<T>(value: T): T;

func identity<T>(value: T): T {
  return value;
}

let fn: Identity<int> = identity<int>;
fn(1);
```

形成 `fn` 时，来源实现、完整类型实参、闭合
`FengFunctionDescriptor` 和 invoke adapter 已经同时确定。形成后的值只能写
`fn(1)`，不能再写 `fn<int>(1)`。callable 值自己的 implementation-specific
invoke adapter 与其保存或引用的 reification descriptor 共同完成路由，所以即使
两个 callable 值背后来自不同函数，也不会让同一个调用点 descriptor 在两个
不同实现树之间选择。

如果未来支持保留开放方法泛参的多态 callable 值，并允许对同一个值调用
`fn<int>()`、`fn<string>()`，则 callable-form 也会出现与本节相同的实现
descriptor 路由问题；这不是当前 callable-form 语义。

### 2.7 静态实现身份路径的闭合不变量

泛型声明体允许保留尚未闭合的类型表达式。例如：

```feng
func invoke<T: Surface>(value: T): Object {
  return value.create<int>();
}
```

分析 `invoke` 时，`T` 尚未闭合是正常状态，`value.create<int>()` 不应在此处
报错。编译器应记录 caller 视角的开放 dependency 节点。

当 callable 边的实际实现身份能够在最终具化点确定时，正确不变量如下：

1. 类型 owner 泛参从类型描述符取得；方法泛参从本次调用的方法泛参描述符
   取得；二者共同参与类型表达式替换。
2. 当共享体 A 调用共享体 B 时，B 的 callable dependency 子树进入 A 的固定
   slot；若 A 自身仍开放，该子树随 A 的开放 dependency 继续向外传播。
3. 共享体 B 再调用共享体 C 时，B 以同样方式从自己的 descriptor slot 取得
   C 的 descriptor 并继续下传；嵌套层数不改变处理模型。
4. 最终具化点必须提供全部显式或推导得到的根类型实参；若实际实现身份也已
   确定，则递归闭合整棵 descriptor tree，所有生成结果继续是 `static const`。
5. 若调用缺少且无法推导类型实参，或约束不满足，应在 Semantic 阶段按非法
   调用诊断；若 Semantic 已判定调用合法，而最终具化仍留下开放节点或缺少
   slot，则属于编译器 dependency 收集、传播或闭合错误。

不得把“声明体分析阶段仍开放”误判为运行时才能具化，也不得通过 runtime
provider/cache 弥补普通编译期 dependency tree 的漏收集。对于 2.5 中实现身份
确实只由运行时 witness 确定的路径，不能再假定最终 wrapper 已静态知道实现；其
路由方案必须另行分析并经人工 Review。

## 3 未来修复方案

以下内容不属于当前 Semantic 限制的实施范围。声明解析、泛型签名满足、调用签名
闭合和通用 dependency 事实的修复方向已经
明确。动态 object-form witness 的实现 descriptor 路由尚未确定；本节明确区分
可以继续复用的既有链路与必须先完成架构分析的缺口，不预设新的运行时机制或 ABI。

### 3.1 声明解析

object-form `spec` 的字段继续按字段类型规则解析；每个普通方法成员统一调用
现有 `resolve_callable()`：

- 实例方法以 `allow_self = false` 解析，因为 spec 签名没有方法体；
- 静态方法使用同一路径；
- 构造器、终结器等非法 spec member 继续由既有成员种类检查拒绝；
- 不增加 spec 专用类型参数表、类型引用种类或诊断码。

无修饰和 `seal` 方法走完全相同的泛型声明解析；visibility 不参与类型参数
作用域建立。

### 3.2 requirement 与实现方法的泛型签名匹配

泛型方法满足检查按以下顺序执行：

1. 方法名称、实例/静态形态和现有可见性兼容规则匹配；
2. 方法泛参 arity 相同；
3. 双方方法泛参按声明位置建立一一对应，不比较泛参名称；
4. 在 spec owner 实参替换和方法泛参位置映射同时生效的环境下，比较显式
   参数数量、顺序、变长标记和类型；
5. 在同一环境下比较返回类型；
6. 检查实现方法的泛参约束能够接受 requirement 允许的全部调用。

约束兼容采用“实现不得比 requirement 更严格”的安全规则：

```text
allowed(requirement constraint) ⊆ allowed(implementation constraint)
```

- requirement 无约束时，实现也必须无约束；
- requirement 有约束而实现无约束时允许；
- 双方都有约束时，使用 owner 实参替换后的现有名义 spec 关系证明
  requirement 约束能够满足实现约束；
- 实现约束更窄、或无法证明上述包含关系时拒绝满足。

该比较必须是可复用的 callable 泛型签名比较抽象，供直接 `type`、`fit`、
父 spec requirement 和 witness 选择统一调用；不得在各入口按类型参数名称
或字符串分别实现。

### 3.3 统一表达 witness-dispatched callable dependency

在现有 resolved callable / reifiable callable dependency 抽象中增加能够表达
object-form spec 方法调用的通用形态。该 dependency 至少保留：

- requirement 的原始 `FengTypeMember` 身份；
- spec 声明及 caller 视角的 spec owner 实例类型；
- receiver/subject 的 caller 视角类型；
- 方法类型实参，顺序与 requirement 声明一致；
- 实例或 static 分派形态；
- 编译期已选定的名义 witness 关系或可继续具化的 witness 来源。

该表示不是 spec 专用 descriptor 旁路，而是现有 callable dependency 的一种
分派来源。排序 key、去重、slot 分配、递归闭合、导入导出和调用点查询继续
复用同一套 callable dependency 基础设施。

调用解析继续复用普通泛型 callable 的规则：

- 显式类型实参数量必须与 requirement 的方法泛参 arity 一致；
- 无显式类型实参时，按现有规则从实参和目标上下文推导；
- 类型实参约束按 requirement 检查；满足检查已保证实现能够接受该调用；
- 参数 coercion、返回类型替换和变长参数规则保持一致；
- 不允许在运行时根据 witness 函数地址重新解析重载或推导类型实参。

### 3.4 descriptor tree 的统一收集与静态实现身份路径

新增的 witness-dispatched dependency 必须先进入现有
`FengReifiableDepSet.callable_deps`，并按现有树形规则完成统一收集与开放传播：

1. **收集**：`collect_from_expr()` 从 Semantic 已解析的 spec 调用事实追加
   dependency；不得从成员名或生成后的 C 函数名反推。
2. **开放传播**：若 receiver、spec owner 或方法类型实参仍引用 caller 泛参，
   保留 caller 视角的开放类型树；共享体本身不要求提前闭合。
3. **静态实现绑定**：当当前层的 witness 来源被具化为具体 type/fit 实现时，复用
   现有 `FengSpecWitness` 选择结果，把 requirement 节点映射到实际实现 callable；
   不重新做结构满足检查。
4. **递归闭合**：复用 `cg_emit_closed_callable_fdesc()` 的既有递归过程，以
   实际实现的 `FengReifiableDepSet` 和本次 owner/method 类型实参生成子树；
   实现方法继续按自己的固定 dependency slot surface 读取。
5. **固定 slot**：子树继续写入 caller descriptor 的
   `reified_callable_deps[slot]`；共享体只做一次固定下标读取。
6. **继续下传**：若实现共享体再调用其他共享 callable，其 descriptor 已是
   当前子树的下一层，继续按相同规则取 slot 并传递。

闭合参数顺序继续复用现有 callable 规则：先放 owner 泛参，再放方法泛参。
例如 `Host<X>.invoke<U: Surface<X>>(value: U)` 内调用 `value.create<U>()`，而实际
`create<V>()` 实现体继续调用 `Helper<X, V>` 时：`X` 由 `Host` 的类型描述符
提供，`U` 由 `invoke` 的方法泛参描述符提供，`create` 的 `V` 在 dependency
边上被映射为 caller 的 `U`；最终 wrapper 递归替换后形成
`invoke -> create -> Helper<X, U>` 的闭合 descriptor 子树。不存在只闭合一层
或只收集直接复合类型的例外。

上述第 3 至第 6 步只在最终具化点能够静态确定 witness 实现身份时成立。对于
`invoke<A>()` 这类路径，最终具化点继续只生成不可变
`static const FengFunctionDescriptor` 及其子节点，不增加运行时类型树遍历、
descriptor 合成、cache、interning 或按名字查找。

对于 `invokeView(subject: Surface)`，dependency 收集仍然必须记录 requirement、
方法实参和 witness 分派来源，但不能在编译 `invokeView` 时把 requirement 节点
错误绑定为任意一个实现，也不能生成 requirement 空 descriptor 并交给实际实现。
此时第 3 步所需的 A/B 实现身份只在运行时存在，如何把它与已经闭合的方法实参
组合并路由到匹配的静态实现树，是 2.5 所述的待分析问题。

### 3.5 witness 方法槽位 ABI 的必要不变量与待决点

泛型 spec 方法进入实际实现共享体时，必须继续满足现有 shared callable ABI：
传入的 `FengFunctionDescriptor *` 必须指向与被调实现完全匹配的**实际实现
子树**，而不能指向 requirement 自身的空树或另一个实现的树。参数顺序为：

```text
实例方法：subject
       -> FengFunctionDescriptor
       -> 方法泛参 FengGenericParamDescriptor（按 requirement 声明顺序）
       -> 显式参数（按声明顺序）
       -> 可选 out

静态方法：FengFunctionDescriptor
       -> 方法泛参 FengGenericParamDescriptor（按 requirement 声明顺序）
       -> 显式参数（按声明顺序）
       -> 可选 out
```

当实现身份静态已知时，descriptor 的取得规则继续统一为：

- 当前位于共享体内：从当前 `_desc->reified_callable_deps[slot]` 读取；
- 当前已经是最终闭合调用点：由既有 closed callable descriptor emitter 返回
  对应静态节点地址。

在这类静态路径中，concrete type/fit witness thunk 只负责既有 ABI 适配，然后把
已经匹配的同一个子树 descriptor 传给实际实现共享体：

```text
实例实现：subject
       -> owner descriptor
       -> FengFunctionDescriptor
       -> 方法泛参 FengGenericParamDescriptor
       -> 显式参数
       -> 可选 out

静态实现：owner descriptor
       -> FengFunctionDescriptor
       -> 方法泛参 FengGenericParamDescriptor
       -> 显式参数
       -> 可选 out
```

- type、fit、managed/value subject 与 static 形态继续复用现有 owner descriptor
  和参数 direct/address ABI 适配；
- spec-to-spec slot witness、父 spec 与 intersection adapter 只转发 descriptor、
  方法泛参 descriptors、显式参数和 `_out`，不得截断子树；
- default witness 按既有默认方法语义消费新增隐藏参数，不创建实现依赖；
- requirement 与实现方法泛参按位置对应。若约束 surface 不同，只能复用编译期
  已证明安全的现有 witness 前缀/父约束适配，不增加运行时约束检查。

动态 object-form 路径当前不满足上述前提：同一个 `invokeView` 调用点不能静态
选择 A 或 B 的实现子树，而现有 witness 槽位只选择函数指针。后续方案必须保证
“实现函数 + 同一实现的闭合 descriptor 树”一致路由，并同时覆盖实例/static、
type/fit、父 spec/intersection/slot adapter 和跨包；具体承载位置及 ABI 尚未决定。

在方案得到人工 Review 前，不得复制实现方法体、另建 generic spec 专用 shared
ABI、在 thunk 内动态生成 dependency tree，或引入运行时查找、cache 和 descriptor
构造。

### 3.6 Codegen 成员表示

`UserSpecMember` 的方法表示需要保留至少以下声明级事实：

- 方法泛参 arity、声明顺序和约束；
- owner + method 双层作用域中的未闭合参数与返回类型模板；
- 每个显式参数和返回值的稳定 generic callable ABI 分类；
- 对应的原始 `FengTypeMember` 身份。

开放泛型 spec 与闭合 spec 实例必须从同一原始声明槽位选择 ABI，不能按某个
具体类型实参重新决定 direct/address 表示。父 spec、intersection 展平和 slot
adapter 克隆成员时必须完整复制上述事实。

### 3.7 `.ft` 与跨包

跨包能够静态恢复的声明与实现事实继续复用通用 callable dependency symbol graph：

- requirement 方法的泛参、约束和未实例化签名必须完整 round-trip；
- caller 的 witness-dispatched callable dependency 必须保留稳定 requirement
  身份、owner/receiver 类型表达式和方法类型实参；
- 实际 type/fit 实现 callable 的 reifiable dependency graph 继续由其既有
  compiler dependency symbol 事实提供；
- 当 witness 实现身份静态已知时，consumer 在最终具化点沿符号身份递归闭合
  descriptor tree，不读取 provider 源码，也不重新进行结构满足检查；
- seal 实现只作为编译器依赖参与闭合，`.ft` 中仍保持 seal，普通成员访问不得
  因此获得可见性。

实施时先验证当前 `.ft` writer/reader 能否承载上述通用事实：

- 若现有字段足够，只修复 Semantic/Codegen 消费；
- 若 callable dependency 缺少 witness 分派身份，则扩展**通用 callable
  dependency 表示及其 writer/reader**；
- 不增加 spec 专用 section、字符串拼接链接协议或运行时查找表。

跨包实现方法的名义满足关系与实现符号恢复继续复用
[泛型 spec 满足关系跨包修复开发文档](./feng-generic-spec-implementation-package-bugfix.md)
已经建立的通用链路；本专项不得复制另一条 package 恢复路径。

动态 object-form 路由必须额外验证开放世界场景：声明 `Surface` 和
`invokeView(Surface)` 的 provider 可以先独立编译，后续 consumer package 才定义
A/B 并把其 object-form 值传入 `invokeView`。方案不得假定 provider 编译时能够
枚举未来实现，也不得依赖包循环、provider 源码重编译或链接期全程序特化。

## 4 未来支持范围边界

### 4.1 本次包含

- object-form spec 自有及父 spec 继承的方法级泛型 requirement；
- 实例、静态、公开和 seal 方法；
- type/fit 实现；
- owner 泛参和方法泛参同时出现；
- 方法泛参约束引用 owner 泛参或其他已在作用域中的方法泛参；
- 标量、managed、aggregate/object-form spec 类型实参及返回值；
- 同包、package `.fb` 和泛型约束调用；
- witness-dispatched callable dependency 的统一收集，以及实现身份静态已知时在
  既有 descriptor tree 中的固定 slot 分配、递归闭合和下传；
- `invokeView(subject: Surface)` 这类动态 object-form 调用的实现函数与实现
  descriptor 一致路由；具体方案须先完成架构分析与人工 Review。

### 4.2 本次不包含

- object-form spec 方法值；该能力由
  [成员方法值补齐开发草案](./feng-object-form-spec-method-value-dev.md)
  独立跟踪；
- callable-form spec 对开放泛型函数/方法的反向推导；
- 新的 variance、结构满足、JIT、运行时名称解析或反射式泛型调用；
- runtime descriptor provider、descriptor cache/interning 或动态 metadata 构造；
- 更改 `spec seal`、`type seal`、`@friend`、`@mixable` 的访问规则；
- 泛型 owner/fit 的 package 名义关系恢复及 seal 实现符号导出；
- 静态字段 storage/ensure 跨包链接；
- 与正确性无关的 witness 或泛型调用性能优化。

## 5 性能与兼容性

- 非泛型 spec 方法的 witness 布局和发码路径保持不变；
- 普通泛型 function/type/fit 方法继续使用现有编译期 descriptor tree；
- 已确认的静态实现身份路径继续复用泛型 shared callable 本来就需要的 function
  descriptor 与方法泛参 descriptor 隐藏参数，不增加额外动态分派层；
- 该路径的每一层共享体仍只进行固定下标 slot 读取，并把子 descriptor 指针传给
  下一层；所有 descriptor 继续是 `.rodata` 中的 `static const` 数据；
- 动态 object-form 路由方案不得默认增加运行时分配、cache、锁、名称查找、类型
  树遍历、反射或约束检查；若无法在该边界内完整实现，必须由人工决策；
- 不改变任何成员的 Feng 可见性，也不让 `.ft` 中的 seal 实现成为普通可见成员。

若静态实现身份路径中的合法调用无法在最终具化点闭合 descriptor tree，必须先定位是
dependency 未收集、开放表达式未传播、witness 实现未绑定，还是 package 符号
事实缺失；不得改用运行时 provider 绕过。动态实现身份路径已经确认不是普通的
tree 漏收集，必须单独分析一致路由方案。若方案需要改变上述性能边界，必须暂停并
由人工决策。

## 6 未来恢复能力时的测试要求

### 6.1 编译器测试

- Parser（验证）：确认 object-form spec 实例/static 泛型方法 AST 已完整保留；
- Semantic：公开/seal、实例/static、显式/推导类型实参、约束、泛参改名、arity
  不匹配、约束方向不兼容；
- 满足选择：type、fit、父 spec、泛型 owner + 方法泛型双层替换；
- dependency 收集：spec witness 调用形成稳定 callable dependency 节点，开放
  类型表达式可跨多层共享 callable 向外传播；
- descriptor tree：验证实现身份静态已知时，最终具化递归闭合两层以上
  shared-call 子树，所有调用点从固定 slot 取 descriptor；
- Codegen：标量、managed、aggregate 参数与返回值，实例/static witness thunk，
  type/fit 实现，以及同一 requirement 的不同实现具有不同 dependency graph；
- 动态路由：验证同一个 `Surface` 调用点分别接收 A/B object-form 值时，实现函数
  和 `Box<int>` / `List<int>` descriptor 子树始终成对选择；
- callable-form（验证）：泛型来源在形成 callable 值前已经闭合，形成后的 invoke
  adapter 与来源 descriptor 保持绑定，普通调用不进入本专项的动态路由；
- Symbol：package-public `.ft` 对 requirement surface、witness dependency 身份
  和递归 callable dependency graph 完整 round-trip；
- CLI：provider 先打包 `.fb`，consumer 只通过包导入并执行泛型 spec 方法；
- 生成 C（验证）：descriptor 均为 `static const`，不存在新增 runtime provider、
  cache 或按名称查找路径。

### 6.2 FCTS

必须在 `fcts_lib -> fcts_bin` 增加可观察语言行为覆盖：

- 公开实例方法与 seal 实例方法；
- 公开静态方法与 seal 静态方法；
- type 与 fit 实现；
- 泛型 spec owner 与非泛型 spec owner；
- 方法泛参名称不同但位置等价；
- 约束、显式类型实参、推导及 aggregate/object-form spec 实参；
- `invoke<T: Surface>()` 内调用 spec 泛型方法，并再进入至少一层泛型 shared
  callable，验证 descriptor tree 连续下传；
- `invokeView(subject: Surface)` 分别接收同一 requirement 的两个实现，且两个实现
  使用不同 reified dependency 子树；
- 返回值和副作用均可观察，避免只验证“能够编译”。

object-form spec 方法值不纳入本专项 FCTS。

## 7 已完成基础与未来 TODO

本节未完成项全部暂停，不属于当前开发任务。当前限制的实施顺序以
[独立备注](./feng-object-form-spec-method-generic-restriction-note.md#7-实施-todo)
为准；以下清单仅供未来重新启用该能力时继续使用。

- [x] **历史变更（未来规则基础）**：曾把方法泛参约束兼容方向写入
  `feng-spec.md` 并由 `feng-generics-draft.md` 引用；当前限制落地时，主规范将
  改为暂不允许，详细匹配规则只保留在本文供未来恢复参考。
- [x] **实际变更（Semantic 声明，待最终回归）**：让 object-form spec 普通
  方法统一复用 `resolve_callable()`。
- [x] **实际变更（Semantic 满足，待最终回归）**：实现按位置 alpha-equivalent
  的泛型 callable 签名比较，统一覆盖 type、fit、父 spec 和 witness 选择。
- [x] **分析（动态路由现状）**：确认 A/B 实现 descriptor 树均可生成；当前
  object-form witness 只路由函数，不能让同一个动态调用点静态选择匹配的 A/B
  实现 descriptor 树；该问题与泛参索引作用域误用相互独立。
- [ ] **设计决策（阻塞后续 descriptor/witness 发码）**：基于既有静态 descriptor
  tree、witness 表示和跨包开放世界约束，分析“实现函数 + 同一实现的闭合
  descriptor 树”一致路由方案；凡涉及新 ABI、运行时机制或增量运行时开销的
  方案，须先由人工 Review。
- [x] **实际变更（Semantic 调用表示基础）**：通用 resolved callable 已补充
  witness-dispatched spec 方法形态，并记录 requirement、owner/receiver、
  实例/static、方法类型实参和 witness 来源；动态实现 descriptor 路由不由该
  Semantic 事实单独解决。
- [x] **部分实际变更（dependency 收集基础）**：spec 泛型方法调用已经能够进入
  现有 `FengReifiableDepSet.callable_deps` 并保留 witness surface；实现身份静态
  已知时的实际实现映射和动态身份路径仍分别受后续闭合与设计决策约束。
- [ ] **实际变更（静态实现 descriptor 闭合）**：扩展现有 callable dependency
  解析，按已具化 witness 关系映射到实际 type/fit 实现，并复用
  `cg_emit_closed_callable_fdesc()` 递归闭合子树；不得新增并行 emitter。
- [x] **实际变更（Codegen 成员表示基础）**：`UserSpecMember` 的解析、克隆和
  实例化路径已经能够保存并解析方法泛参及 owner + method 双层泛型签名模板。
- [x] **部分实际变更（Codegen witness ABI 骨架）**：实例/static 泛型方法槽位、
  调用点及 type/fit thunk 已能转发 function descriptor、方法泛参 descriptor、
  显式参数和 `_out`；这只完成 ABI 形状，不代表 descriptor 内容正确。
- [ ] **实际变更（Codegen descriptor 一致路由）**：在设计决策通过后，让
  实例/static、type/fit、父 spec/intersection/slot adapter 和 default witness
  一致选择“实现函数 + 同一实现 descriptor 子树”，不得把 requirement 空
  descriptor 传给实现共享体。
- [ ] **验证（Symbol 现状）**：确认 `.ft` 已完整 round-trip requirement 方法泛参、
  约束、签名和现有 callable dependency graph。
- [ ] **条件性实际变更（Symbol）**：仅当上一项证明通用 callable dependency
  无法表达 witness 分派身份时，扩展通用 symbol view/writer/reader；不得新增
  spec 专用 section 或旁路。
- [ ] **验证（既有泛型 owner/fit package 链路）**：确认本专项直接复用最近完成的
  名义关系、实现符号和 callable dependency 恢复，不复制 package 恢复逻辑。
- [ ] **验证（callable-form 边界）**：确认当前 callable-form 泛型来源在值形成前
  闭合，形成后的 invoke adapter 与来源 descriptor 成对绑定，不存在调用时泛型
  实现路由缺口。
- [ ] **验证（编译器用例）**：补齐 Parser、Semantic、dependency、Symbol、
  Codegen、生成 C 和隔离 `.fb` CLI 正负用例，重点覆盖多层 descriptor tree。
- [ ] **验证（FCTS）**：补齐同包与跨包可观察行为用例并注册执行。
- [ ] **验证（回归）**：执行定向测试后，在沙箱外执行 `make test` 全量回归。

实施中若静态实现身份路径出现未闭合节点，先沿“调用解析 → dependency 收集 →
symbol round-trip → witness 实现绑定 → descriptor 递归闭合 → slot 下传”逐层
定位；非架构性漏接可按本文修复并补充用例。动态实现身份路径必须先完成 TODO 中
的设计决策；若需要新运行时机制、额外运行时成本或独立 ABI，必须暂停并由人工
决策。
