# 泛型重载决议优化开发设计

> **状态**：方案待人工 Review，未实施。
>
> **日期**：2026-08-08。
>
> **范围**：函数与方法调用的候选筛选、逐参数优先级、调用点消歧，以及对应的重载声明合法性。
>
> **关联开发**：[Object-form Spec 向上 Coercion 开发设计](./feng-object-spec-upcasting-dev.md)。两项开发独立实施；本文不实现 `spec` 向上 coercion 或 witness 变更。

## 1. 核心决策

当前实现把整个候选压缩为一个整数：

```text
非泛型精确匹配 > 非泛型非精确匹配 > 泛型匹配
```

本次改为：

1. 先独立判断候选是否适用。
2. 为每个实参位置生成一个匹配类别。
3. 多参数从左到右按匹配类别比较，第一个不同的位置决定结果，不累计分数。
4. 存在唯一最优候选时选取；否则在调用点报告 `AE0511`。
5. 允许声明调用方能够通过静态类型、显式标注、显式转换或显式类型实参消歧的潜在重载。

匹配类别由形参的**外层类型形状**决定：

- `T` 的外层是调用级类型参数，按 `T` 的约束分类。
- `Pair<K, V>` 的外层是具体 `type Pair`，按整个 `Pair<...>` 与实参的关系分类；`K`、`V` 只参与既有推导和约束验证，不单独参与该实参位置的优先级。
- `UserSpec` 的外层是 `spec`，按非父级或父级关系分类。

## 2. 目标与边界

### 2.1 目标

- 允许具体 `type` / `spec`、子 `spec` / 父 `spec` 等可在调用侧消歧的重载。
- 具体 `type` 的精确匹配和合法目标贴合均优先于 `spec` 匹配。
- 约束更贴近实参的调用级泛型可以优先于需要父级关系的非泛型 `spec`。
- 顶层函数、普通成员方法、静态方法、`fit` 方法和 `@mixable` 生成方法使用同一规则。
- 本地声明与从 `.ft` 恢复的跨包声明行为一致；Codegen 只消费 Semantic 已选定的声明。

### 2.2 非目标

本次不实现或改变：

- Object-form 子 `spec` 到父 `spec` 的值表示转换、witness 父级入口或 Codegen lowering。
- 父级 `spec` 的继承距离比较。
- 根据声明顺序、候选收集顺序、返回类型或模块导入顺序选择候选。
- 根据泛型参数名或泛型约束差异建立新的重载 identity。
- 泛型约束间未显式声明的“更强”“更弱”推理。
- 泛型实例的协变或逆变。
- 数值类型、字面量或其他表达式的既有转换资格；本文只定义已经合法的匹配如何排序。
- 函数值、方法值、构造函数和变长参数形状的既有规则。
- 全局“非泛型候选一定优于泛型候选”的兜底规则。

## 3. 候选适用性

优先级比较前，每个候选必须独立通过以下检查：

1. 名称、可见性、静态/实例调用形态和参数个数匹配。
2. 显式提供类型实参时，只保留泛型参数数量准确匹配的泛型候选；非泛型候选不能消费显式类型实参。
3. 省略类型实参时，按现有规则推导调用级类型参数。
4. 推导或显式提供的类型实参满足全部约束。
5. 每个普通参数或变长参数元素能按现有语言规则接受对应实参。

任一条件不成立即剔除候选。适用性与排序必须分离，不得为排序放宽推导、约束或转换资格。

重载决议只使用调用点已知的实参静态类型。显式标注或显式转换先改变表达式的静态类型，再参与候选筛选和排序：

```feng
let child: ChildSpec = UserType {};
test(child);                 // 按 ChildSpec 决议
test((ParentSpec)child);     // 按 ParentSpec 决议
```

## 4. 单参数匹配优先级

### 4.1 直观对照表

| 优先级 | 匹配类别 | 典型情况 |
|---:|---|---|
| 1 | 精确具体 `type` | `UserType` 实参匹配 `UserType` 参数；整数字面量的默认类型匹配 `int` 参数 |
| 2 | 具体 `type` 目标贴合 | 数值字面量按既有规则从默认类型贴合到其他具体数值 `type` |
| 3 | 非泛型非父级 `spec` | `UserType` 直接满足 `UserSpec`；`UserSpec` 匹配 `UserSpec` |
| 4 | 调用级泛型非父级 `spec` | `UserType` 匹配调用级泛型参数 `T: UserSpec` |
| 5 | 非泛型父级 `spec` | `ChildSpec` 经父级关系匹配 `ParentSpec` 参数 |
| 6 | 调用级泛型父级 `spec` | 实参经父级关系满足调用级泛型约束 `T: ParentSpec` |
| 7 | 无约束调用级泛型 | 实参匹配调用级泛型参数 `T` |

顺序为：

```text
精确具体 type
> 具体 type 目标贴合
> 非泛型非父级 spec
> 调用级泛型非父级 spec
> 非泛型父级 spec
> 调用级泛型父级 spec
> 无约束调用级泛型
```

数值越小越优先。

### 4.2 具体 `type` 匹配

“精确具体 `type`”要求形参是具体 `type`，且形参与实参静态类型在别名归一和必要的外层泛型替换后完全一致。

“具体 `type` 目标贴合”包括所有已经由现有语言规则允许、但目标不是实参默认静态类型的具体 `type` 贴合。本文不为数值类型增加转换，只把既有合法贴合与 `spec` 匹配分级；任何具体 `type` 目标贴合均优先于 `spec`。

以数值字面量为例，假设 `int` 已按现有声明满足 `NumericSpec`：

```feng
func pick(value: int) {}
func pick(value: i8) {}
func pick(value: NumericSpec) {}

pick(1);
```

若 `1` 的默认类型是 `int`，则三个候选依次为：

1. `int`：精确具体 `type`。
2. `i8`：在值域检查通过时属于具体 `type` 目标贴合。
3. `NumericSpec`：在 `int` 满足该契约时属于非泛型非父级 `spec`。

因此选择 `int`。若没有 `int` 候选但 `i8` 贴合合法，则 `i8` 仍优先于 `NumericSpec`。

两个非默认具体数值目标之间不按位宽或值域排序：

```feng
func narrow(value: i8) {}
func narrow(value: i16) {}

narrow(1); // 两个候选同为具体 type 目标贴合，AE0511
```

已具有静态类型的绑定值不享有字面量目标贴合。例如 `let value: i64 = 1;` 产生 `i64` 值，不能因此隐式匹配 `i32` 参数。

### 4.3 `spec` 匹配

非父级 `spec` 匹配不经过 object-form 父级边，包括：

- 实参静态类型就是目标 `spec`。
- 具体 `type` 直接声明满足目标 object-form `spec`，或存在直接 `fit Type: Spec`。
- 按既有规则合法匹配 callable-form、union-form 或 intersection-form `spec`。

若从实参已经直接具有的 object-form `spec` 视角到目标 `spec` 至少经过一条已声明父级边，则属于父级 `spec` 匹配：

```feng
spec ParentSpec {}
spec ChildSpec: ParentSpec {}
type UserType: ChildSpec {}
```

其中 `UserType -> ChildSpec` 是非父级匹配，`UserType -> ParentSpec` 是父级匹配。

Callable-form、union-form 和 intersection-form 没有父级类别，其合法匹配只能进入非父级类别。

调用级泛型形参只有在外层本身是类型参数时才进入泛型类别：

```feng
func direct<T: UserSpec>(value: T) {}
```

候选须先完成推导和约束验证；排序时不能把推导后的 `T` 伪装成非泛型精确 `type`。

### 4.4 复合形参按外层类型分类

复合形参按外层类型整体分类，不读取内部类型实参的优先级：

```feng
func use<K, V: ValueSpec>(value: Pair<K, V>) {}
```

对 `Pair<KeyType, ValueType>` 实参：

- `K`、`V` 只用于既有类型参数推导与约束验证。
- 推导替换后的完整形参若与实参静态类型相同，该位置属于“精确具体 `type`”。
- 不因为 `K` 无约束而降为“无约束调用级泛型”，也不因为 `V: ValueSpec` 而归入泛型 `spec`。

该规则适用于任意层数和任意数量的内部类型实参，避免为 `Pair<K, V>`、`Map<K, List<V>>` 等形状建立内部优先级合并规则。

外层 owner 的类型参数同样先按 receiver / owner 实例替换，再按替换后的完整外层形状分类：

```feng
type Holder<T> {
    func use(value: Pair<T, string>) {}
}
```

### 4.5 不比较父级距离

```feng
spec RootSpec {}
spec ParentSpec: RootSpec {}
spec ChildSpec: ParentSpec {}

func select(value: ParentSpec) {}
func select(value: RootSpec) {}
```

若实参静态类型为 `ChildSpec`，两个候选均为“非泛型父级 `spec`”，不按一层或两层排序；没有其他参数消歧时报告 `AE0511`。

调用方可通过静态类型选择目标：

```feng
let parent: ParentSpec = child;
select(parent); // ParentSpec 成为非泛型非父级 spec
```

## 5. 多参数比较与调用点消歧

候选按参数源码顺序进行字典序比较：

1. 当前参数位置类别不同，类别更优的候选立即胜出。
2. 当前参数位置类别相同，继续比较下一位置。
3. 全部位置相同，两个候选无法排序。
4. 全部适用候选中必须存在唯一最优候选，否则报告 `AE0511`。

不累计分数，也不统计候选胜出的位置数量：

```feng
func choose(a: UserType, b: ParentSpec) {}
func choose(a: UserSpec, b: OtherType) {}
```

若第一个位置分别为“精确具体 `type`”和“非泛型非父级 `spec`”，直接选择第一个候选，不再用第二个位置反转结果。

显式类型实参先按泛型参数数量筛选候选，筛选后仍使用相同的逐参数比较；它不是额外排序分数：

```feng
select<int>(value);
```

调用方也可以通过显式标注或显式转换改变实参静态类型：

```feng
let target: DesiredSpec = value;
call(target);
call((DesiredSpec)value);
```

## 6. 重载声明合法性

### 6.1 允许调用侧可消歧的潜在重叠

以下重载合法：

```feng
type UserType: UserSpec {}

func test(value: UserType) {}
func test(value: UserSpec) {}
```

```feng
spec ChildSpec: ParentSpec {}

func show(value: ChildSpec) {}
func show(value: ParentSpec) {}
```

两个无父子关系、但存在共同满足类型的 `spec` 也允许组成重载。以共同满足类型直接调用且没有唯一最优候选时，在调用点报告 `AE0511`；调用方可先显式转换到目标 `spec`。

### 6.2 继续禁止的声明

- 相同名称、泛型参数数量和参数列表的重复签名。
- 仅返回类型不同的候选。
- 仅泛型参数名或泛型约束不同的候选。
- 现有变长参数冲突。
- `import` 把不同来源的同名顶层函数合并为新的重载集合。

放开的是不同参数类型之间的潜在语义重叠，不是相同重载 identity。

### 6.3 必然死角与动态死角

如果某个重叠调用形状不依赖外层泛型实例即可在声明阶段确定，且调用方无法通过任何实参位置、显式类型实参、显式标注或显式转换选择目标候选，则属于**必然死角**，必须在定义处拒绝。当前语言规则下的必然死角必须全部覆盖；遗漏到调用点属于编译器 Bug。

例如两个不同元素类型的变长参数均接受空调用，`test()` 没有实参位置表达目标元素类型，必须在定义处拒绝：

```feng
func test(values: int...) {}
func test(values: string...) {}
```

复合形参按外层类型分类，因此下列单参数重载对 `Pair<int, string>` 得到相同的“精确具体 `type`”；显式类型实参只能选择泛型候选，无法选择具体候选，属于定义期可证明的必然死角：

```feng
func use<K, V>(value: Pair<K, V>) {}
func use(value: Pair<int, string>) {} // 定义处冲突
```

若其他参数位置能够消歧，则应结合完整参数列表判断，不得只因某一个位置重叠而拒绝。

如果重叠只有在外层 receiver / owner 泛型参数替换为特定类型后才出现，则属于**动态死角**。开放泛型定义和实例化均合法，只在实际调用没有唯一最优候选时报告 `AE0511`：

```feng
type Container<T> {
    func test(value: T) {}
    func test(value: string) {}
}

let ints: Container<int> = Container<int>();
ints.test(1); // 只有 test(T) 适用

let strings: Container<string> = Container<string>();
strings.test("value"); // 两个候选均为精确具体 type，AE0511
```

外层泛型参数先按 receiver / owner 实例替换，不属于本次调用级泛型；替换结果按普通外层类型形状分类。

## 7. 数值类型与泛型实例边界

### 7.1 数值类型和别名

`i8`、`i16`、`i32`、`i64` 等是彼此独立的具体类型。不得根据名称、位宽或值域建立父子关系、隐式转换关系或额外重载优先级。

`int` 是平台相关别名：32 位平台归一为 `i32`，64 位平台归一为 `i64`。类型 identity、重复签名、精确匹配和类别计算前均须归一。因此，`test(int)` 与当前平台对应的 `test(i32)` 或 `test(i64)` 是重复签名。

无目标上下文的整数字面量默认推导为 `int`，浮点字面量默认推导为 `double`。字面量直接作为实参时，使用与先绑定到无显式标注变量相同的默认类型：

```feng
pick(1);

let value = 1;
pick(value); // 与上一调用选择相同候选
```

默认类型候选属于精确具体 `type`；其他合法具体数值目标属于具体 `type` 目标贴合。

### 7.2 不变泛型实例

泛型实例保持不变。即使 `ChildSpec: ParentSpec`，`Box<ChildSpec>` 与 `Box<ParentSpec>` 也不是同一类型，二者之间不存在自动转换；重载排序不得透过 `Box` 的内部类型实参传播父级 `spec` 关系。

本次也不扩大调用级泛型推导。`Pair<K, V>` 只有在现有规则能够推导、或调用方显式提供类型实参时才可能适用；“按外层 `Pair` 分类”只影响排序，不增加推导能力。

## 8. 与 Spec 向上 Coercion 的关系

本次只建立重载候选比较框架，不新增子 `spec` 值到父 `spec` 值的转换资格。后续实现 `spec` 向上 coercion 后，新产生的父级候选按本文自然进入“非泛型父级 `spec`”或“调用级泛型父级 `spec`”。

Witness 父级路径只在唯一候选选定、目标形参确定后记录；候选探测不得生成最终 lowering 结论。本文审批后，[Object-form Spec 向上 Coercion 开发设计](./feng-object-spec-upcasting-dev.md)只引用本文的重载规则，不重复定义。

## 9. Semantic 实现约束

### 9.1 逐参数结果

以逐参数类别向量替换当前 `compute_overload_match_priority(...)` 的整体整数。建议内部枚举顺序直接对应本文七级顺序：

```c
typedef enum FengOverloadParamMatchRank {
    FENG_OVERLOAD_PARAM_MATCH_EXACT_CONCRETE_TYPE = 0,
    FENG_OVERLOAD_PARAM_MATCH_CONCRETE_TYPE_ADAPTATION,
    FENG_OVERLOAD_PARAM_MATCH_NON_GENERIC_NON_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_CALL_GENERIC_NON_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_NON_GENERIC_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_CALL_GENERIC_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_UNCONSTRAINED_CALL_GENERIC
} FengOverloadParamMatchRank;
```

最终命名可按现有代码风格调整，但实现必须满足：

- 每个实参位置只产生一个类别。
- 先完成别名和 receiver / owner 外层泛型替换。
- 复合形参只按外层类型形状分类；内部类型实参只参与适用性检查。
- Object-form 区分非父级与父级，但不计算父级距离。
- Callable-form、union-form 和 intersection-form 只产生非父级 `spec` 类别。
- 比较函数只做从左到右的字典序比较。

### 9.2 统一候选探测与选择

适用性和匹配类别应在同一次候选探测中共享推导、替换和类型关系结果。顶层函数、模块公开函数、普通成员方法、静态方法、`fit` 方法与 `@mixable` wrapper 必须复用同一比较逻辑。

本文类别向量构成全序；相同向量表示无法消歧。候选选择不得依赖遍历顺序：新候选更优则替换，较劣则忽略，相同则记录二义状态。若实现中出现不可传递或不可比较的关系，必须改为收集全部适用候选后统一求唯一最优候选。

变长参数按实际实参位置从左到右比较，普通实参使用变长元素形参的类别；预打包参数的既有形状检查保持不变。

### 9.3 声明检查与 Codegen

以下声明重叠路径需要改造：

- `validate_top_level_overload_overlap(...)`
- `validate_type_member_overload_overlap(...)`
- `validate_fit_member_overload_overlap(...)`
- `signatures_potentially_overlap(...)` 及其调用者
- `@mixable` 生成方法的重叠检查路径

这些路径不再拒绝所有潜在重叠，但必须保留 identity 冲突并完整拒绝必然死角。

Semantic 选定唯一声明后，继续记录类型实参及替换后的参数和返回类型。Codegen 不重新比较候选；本次不改变函数 ABI、泛型描述符、witness ABI 或 runtime 接口，不增加运行时开销。

## 10. 实施 TODO

### 10.1 文档与规范

- [x] 整理本开发设计，明确外层类型形状、七级优先级和逐参数比较规则。
- [ ] 人工 Review 并确认本方案。
- [ ] 更新 [Feng 函数规范](../specifications/feng-function.md)，将候选适用性、逐参数比较、二义性和声明合法性收敛为权威规则。
- [ ] 更新 [Feng 泛型规范草案](../specifications/feng-generics-draft.md)，删除“所有非泛型候选全局优先”的旧规则并引用函数规范。
- [ ] 检查 `AE0511` 文案是否适用于新的调用点二义性；不需要新诊断码时不改错误码规范。
- [ ] 更新 [Object-form Spec 向上 Coercion 开发设计](./feng-object-spec-upcasting-dev.md)，仅引用权威重载规则并删除重复或过时结论。

### 10.2 Semantic 实现

- [ ] 引入七级单参数类别、候选类别向量和字典序比较函数。
- [ ] 统一别名归一、实参静态类型取得和 receiver / owner 外层泛型替换时机。
- [ ] 让候选探测区分精确具体 `type`、具体 `type` 目标贴合和各类 `spec` 匹配。
- [ ] 让裸调用级类型参数按约束分类；让复合形参只按外层类型形状分类。
- [ ] 改造全部顶层函数和方法重载入口，移除整体 `match_priority` 依赖。
- [ ] 改造本地、跨包、`fit` 和 `@mixable` 路径，共享同一候选比较实现。
- [ ] 放开调用侧可消歧的潜在重叠，同时保留 identity 冲突。
- [ ] 审计当前语言全部调用形状，完整实现必然死角的定义期检查，并验证声明顺序不影响结论。
- [ ] 确认 Semantic 结果完整记录唯一声明和类型实参，Codegen 无需参与重载决议。

### 10.3 测试与回归

- [ ] 在修改任何既有测试前取得人工批准。
- [ ] 增加 compiler tests：声明合法性、identity 冲突、必然死角、`AE0511` 和 AST/Semantic 目标选择。
- [ ] 增加 FCTS：七级相邻优先级、显式消歧和最终函数体行为。
- [ ] 覆盖多参数字典序、声明顺序交换、跨包恢复、变长参数与动态死角。
- [ ] 覆盖 `int` 归一、默认数值类型、具体数值目标贴合优先于 `spec`，以及多个目标贴合二义性。
- [ ] 覆盖 `Pair<K, V>`、多层复合形参、owner 泛型替换和泛型实例不变性。
- [ ] 覆盖 object/callable/union/intersection 四种 `spec` form，确认只有 object-form 可进入父级类别。
- [ ] 所有非文档变更完成后，在沙箱外执行全量回归 `make test`。

## 11. 验收标准

- 所有调用入口先筛选适用候选，再按七级类别从左到右比较。
- 具体 `type` 精确匹配和合法目标贴合均优先于 `spec`；数值类型之间不引入新转换或位宽排序。
- 复合形参只按外层类型形状分类，内部类型实参不参与该位置排序。
- 可在调用侧消歧的潜在重载合法；identity 冲突和全部必然死角仍在定义处拒绝。
- 无唯一最优候选时稳定报告 `AE0511`，且结果不受声明、收集或导入顺序影响。
- 父级 `spec` 不比较层数；泛型实例保持不变；本次无 ABI 或运行时开销变化。
- 专项测试与全量 `make test` 全部通过。
