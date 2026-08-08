# 泛型重载决议优化开发设计

> **状态**：方案待人工 Review，未实施。
> **日期**：2026-08-07。
> **范围**：函数与方法调用中的泛型候选筛选、契约匹配优先级、逐参数重载比较，以及为该比较放开可在调用侧消歧的重载声明。
> **关联开发**：[Object-form Spec 向上 Coercion 开发设计](./feng-object-spec-upcasting-dev.md)。两项开发独立实施；本文先完成重载决议优化，不实施 `spec` 向上 coercion 或 witness 变更。

## 1. 背景

当前重载决议将整个候选压缩为一个整数优先级：

```text
非泛型精确匹配 > 非泛型非精确匹配 > 泛型匹配
```

因此，当前实现存在以下限制：

1. 只要存在可匹配的非泛型候选，泛型候选即使具有更直接的 `spec` 约束也不能胜出。
2. 多参数候选只有一个整体优先级，不能按参数位置从左到右稳定消歧。
3. 声明阶段会把 `type` / `spec`、共同满足的两个 `spec` 等潜在重叠直接判为冲突，即使调用点的实参静态类型足以选出唯一候选。
4. 当前单一优先级不能表达“非父级 `spec`”“经父级关系匹配的 `spec`”“带约束泛型”等不同匹配质量。

本次优化把重载原则收敛为：

> 允许声明调用方能够通过实参静态类型、显式类型标注或显式转换准确消歧的重载；调用决议先筛选适用候选，再按每个参数的匹配类别从左到右比较。

## 2. 目标

本次开发需要完成：

1. 允许非泛型具体 `type` 参数与其匹配的 `spec` 参数组成重载。
2. 允许存在父子关系或共同具体满足类型的 object-form `spec` 参数组成重载。
3. 保持非泛型精确匹配优先于同位置的泛型匹配。
4. 在泛型非父级 `spec` 匹配与需要父级投影的非泛型 object-form `spec` 匹配之间，优先选择前者。
5. 对多参数候选按参数位置从左到右依次比较，不累计分数。
6. 当所有参数位置都不能确定唯一优先级时，在调用点报告二义性。
7. 顶层函数、普通成员方法、静态方法与 `fit` 方法使用同一套候选比较规则。
8. 本地声明与从 `.ft` 恢复的跨包声明使用同一套规则，Codegen 只消费 Semantic 已选定的声明。

## 3. 非目标

本次不实现或改变：

- Object-form 子 `spec` 到父 `spec` 的值表示转换、witness 父级入口和 Codegen lowering；这些属于独立的 `spec` 向上 coercion 开发。
- 比较两个父 `spec` 候选距离实参多少层。
- 根据声明顺序选择候选。
- 根据返回类型选择调用候选。
- 根据泛型参数名或泛型约束不同建立新的重载签名 identity。
- 泛型约束间的任意“更强”“更弱”推理；只使用已声明的名义 `spec` 关系判定非父级匹配或父级匹配。
- 泛型容器的协变或逆变。
- 非泛型优先于泛型的全局兜底规则。
- 函数值或方法值的目标 callable-form `spec` 消歧规则。
- 构造函数现有重载规则。
- 变长参数的形状、预打包参数和既有冲突规则。
- 数值字面量贴合等其他既有转换资格规则；本次只为其已合法匹配的结果确定重载类别，不扩大转换资格。

## 4. 术语

### 4.1 实参静态类型

重载决议只使用 Semantic 在调用点已知的实参静态类型，不依据运行时具体类型搜索候选。

显式类型标注或显式转换会先改变表达式的静态类型，再以转换后的静态类型参与候选筛选与比较：

```feng
let child: ChildSpec = UserType {};
test(child);                 // 按 ChildSpec 静态类型决议
test((ParentSpec)child);     // 按 ParentSpec 静态类型决议
```

### 4.2 非父级 `spec` 匹配

以下情况属于非父级 `spec` 匹配：

- 实参静态类型就是形参声明的 object-form `spec`。
- 实参静态类型是具体 `type`，该 `type` 直接声明满足目标 `spec`，或存在直接以该 `type` 为目标的 `fit Type: Spec`。
- 实参按 callable-form `spec` 的既有可调用形状规则合法匹配目标 callable-form `spec`。
- 实参按 union-form `spec` 的既有 member 进入规则合法匹配目标 union-form `spec`。
- 实参按 intersection-form `spec` 的既有合并契约规则合法匹配目标 intersection-form `spec`。
- 泛型候选的约束 `spec` 按上述对应 form 的规则接受推导出的类型实参，且不经过 object-form 父级边。

Object-form 的非父级匹配不经过 `spec Child: Parent` 的父级边。Callable-form、union-form 与 intersection-form 没有父级概念，因此它们的合法匹配始终属于非父级 `spec` 匹配，不得归入父级 `spec` 或泛型父级 `spec`。

非泛型形参形成上述匹配时归为“具体非父级 `spec`”；函数或方法自身的泛型形参通过约束形成上述匹配时归为“泛型非父级 `spec`”。

### 4.3 父级 `spec` 匹配

若从实参已直接具有的 object-form `spec` 视角到目标 `spec` 至少需要经过一条已声明的父级边，则属于父级 `spec` 匹配：

```feng
spec ParentSpec {}
spec ChildSpec: ParentSpec {}
type UserType: ChildSpec {}
```

在上述声明中，`UserType -> ChildSpec` 是非父级匹配，`UserType -> ParentSpec` 是父级匹配。

父级匹配只适用于 object-form `spec`。Callable-form、union-form 与 intersection-form 不得进入本类别。

本次不比较经过一层还是多层；所有合法父级路径均属于同一匹配类别。若两个候选只能依靠不同层数区分，则该参数位置不能消歧。

### 4.4 泛型 `spec` 匹配

泛型候选的形参是函数或方法自身声明的类型参数，且该类型参数带 `spec` 约束：

```feng
func test<T: ChildSpec>(value: T) {}
```

候选仍须先完成类型参数推导，并验证全部约束。推导失败或约束不满足时，候选不适用，不进入优先级比较。

比较时必须保留形参来自函数或方法自身泛型参数这一声明事实；不能在推导替换后把 `T` 伪装成非泛型精确 `type` 参数。

- Object-form 约束不经过父级边时，属于泛型非父级 `spec`；经过至少一条父级边时，属于泛型父级 `spec`。
- Callable-form、union-form 与 intersection-form 约束没有父级概念，合法匹配时始终属于泛型非父级 `spec`。

## 5. 声明合法性

### 5.1 允许可在调用侧消歧的潜在重叠

以下声明在本次优化后合法：

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

```feng
func render<T: ChildSpec>(value: T) {}
func render(value: ParentSpec) {}
```

声明阶段不得因为存在某个类型可以同时匹配两个不同参数类型，就直接拒绝整个重载集合。是否存在唯一最优候选改由具体调用点决定。

两个无父子关系、但存在同一个具体类型同时满足的 `spec` 参数也允许组成重载。以该具体类型直接调用时，如果各候选处于相同匹配类别且其他参数也不能消歧，则在调用点报告二义性；调用方可以先显式转换为目标 `spec` 再调用。

### 5.2 继续禁止的声明

以下规则保持不变：

- 名称、泛型参数数量和参数列表相同的重复签名继续冲突。
- 仅返回类型不同的候选继续冲突。
- 仅泛型参数名不同不构成重载。
- 仅泛型约束不同不构成重载。
- 现有变长参数重载冲突规则保持不变。
- `import` 不把不同来源的同名顶层函数合并为新的重载集合。

例如，以下声明仍不合法：

```feng
func test<T: SpecA>(value: T) {}
func test<U: SpecB>(value: U) {} // 仅类型参数名和约束不同
```

放开的是不同参数类型之间的潜在语义重叠，不是放开相同重载 identity 的重复声明。

### 5.3 调用死角边界

重载合法性遵循“只拒绝必然死角，不因动态死角阻止全部合法使用”的原则。必须区分以下三类情况。

#### 5.3.1 声明 identity 冲突

重复签名、仅返回类型不同、仅泛型参数名或约束不同等 §5.2 已禁止的情况，属于声明 identity 冲突，不进入调用点动态决议，继续在声明阶段拒绝。

#### 5.3.2 必然死角

如果某个重叠调用形状在声明阶段即可确定，且调用方在该形状上没有任何实参位置、显式类型实参、显式类型标注或显式转换能够选择目标候选，则属于必然死角，继续在声明阶段拒绝。

例如，现有变长参数规则中的以下声明保持冲突：

```feng
func test(values: int...) {}
func test(values: string...) {}
```

空调用 `test()` 没有实参位置可供调用方表达目标元素类型，因此该调用形状是声明时即可确定的必然死角。

#### 5.3.3 动态死角

如果重叠只在外层泛型参数被某个具体 receiver / owner 实例替换后出现，则属于动态死角。编译器不得因此拒绝泛型声明，也不得禁止该泛型实例；只在实际调用形成多个同级最优候选时报告 `AE0511`。

```feng
type Container<T> {
    func test(value: T) {}
    func test(value: string) {}
}

let ints: Container<int> = Container<int>();
ints.test(1); // 只有 test(value: T) 适用

let strings: Container<string> = Container<string>();
strings.test("value"); // T 替换为 string 后两个候选同为精确 type，AE0511
```

外层 `type` / `spec` / `fit` 泛型参数由 receiver / owner 实例确定，不是本次调用能够显式指定的调用级泛型参数。重载比较前必须先完成外层泛型参数替换；替换后的参数按普通具体类型或具体 `spec` 分类，不附加“调用级泛型”降级。

函数或方法自身声明的泛型参数不同：调用方可以通过显式类型实参选择泛型候选，因此具体候选优先不会形成绝对调用死角：

```feng
func select<T>(value: T): string { return "generic"; }
func select(value: string): string { return "concrete"; }

select("value");         // 选择具体候选
select<string>("value"); // 显式选择泛型候选
```

因此，只有函数或方法自身声明、可由本次调用显式提供或推导的类型参数，才使对应参数位置归入“泛型非父级 `spec`”“泛型父级 `spec`”或“无约束泛型”。

## 6. 候选适用性

优先级比较前，编译器必须独立判断每个候选是否适用：

1. 名称、参数个数、可见性和静态/实例调用形态匹配。
2. 显式提供类型实参时，只保留泛型参数数量准确匹配的泛型候选；非泛型候选不能消费显式类型实参。
3. 省略类型实参时，对泛型候选执行现有类型参数推导。
4. 推导出的每个类型实参必须满足声明约束；object-form、callable-form、union-form 与 intersection-form 约束必须在此阶段统一验证。
5. 每个普通参数或变长参数元素必须能按当前语言规则接受对应实参。
6. 任一条件不成立时，直接剔除该候选；不适用候选不参与优先级比较。

适用性与优先级必须分离。不得为了让某个候选胜出而放宽其类型参数推导、约束验证或参数转换资格。

## 7. 单参数匹配优先级

对进入候选集的候选，每个参数位置生成一个独立匹配类别。匹配顺序如下：

```text
精确 type
> 具体非父级 spec
> 泛型非父级 spec
> 父级 spec
> 泛型父级 spec
> 无约束泛型
```

含义如下：

| 优先级 | 匹配类别 | 说明 |
|---:|---|---|
| 1 | 精确 `type` | 非泛型形参是具体 `type`，与实参静态类型一致 |
| 2 | 具体非父级 `spec` | 非泛型 `spec` 形参与实参形成 §4.2 的非父级匹配；适用于全部四种 spec form |
| 3 | 泛型非父级 `spec` | 泛型形参的 `spec` 约束与推导类型形成 §4.2 的非父级匹配；适用于全部四种 spec form |
| 4 | 父级 `spec` | 非泛型 object-form `spec` 形参需要经过至少一条父级边匹配 |
| 5 | 泛型父级 `spec` | 泛型形参的 object-form `spec` 约束需要经过至少一条父级边才能接受推导类型 |
| 6 | 无约束泛型 | 裸类型参数 `T` 接受推导出的实参类型 |

数值越小越优先。该顺序表达两条共同规则：

1. 在相同契约关系下，非泛型具体形参优先于泛型形参。
2. 非父级、约束更贴近实参的泛型 `spec` 可以优先于需要父级投影的非泛型 object-form `spec`。
3. Callable-form、union-form 与 intersection-form 不参与父级比较，只能进入具体非父级或泛型非父级类别。

### 7.1 精确具体类型优先

```feng
func select(value: UserType): string { return "type"; }
func select<T: UserSpec>(value: T): string { return "generic"; }

select(UserType {}); // 选择 UserType
```

### 7.2 泛型非父级约束优先于非泛型父级

```feng
func select<T: ChildSpec>(value: T): string { return "generic-child"; }
func select(value: ParentSpec): string { return "parent"; }

let user = UserType {};
select(user);             // 选择 T: ChildSpec
select((ParentSpec)user); // 选择 ParentSpec
```

第二个调用中，显式转换后的实参静态类型是 `ParentSpec`，`T: ChildSpec` 不再适用；调用方由此准确选择父级候选。

### 7.3 不比较父级距离

```feng
spec RootSpec {}
spec ParentSpec: RootSpec {}
spec ChildSpec: ParentSpec {}

func select(value: ParentSpec) {}
func select(value: RootSpec) {}
```

若实参静态类型为 `ChildSpec`，两个候选都属于“父级 `spec`”，本参数位置不按一层或两层排序。没有其他参数能够消歧时，调用报告二义性。

调用方可显式声明或转换实参类型：

```feng
let parent: ParentSpec = child;
select(parent); // ParentSpec 成为具体非父级 spec
```

## 8. 多参数比较

多参数不计算总分，也不统计某个候选在多少个位置胜出。编译器按源码参数顺序从左到右逐个比较两个候选：

1. 当前参数位置的匹配类别不同，类别更优的候选立即胜出，停止比较后续参数。
2. 当前参数位置的匹配类别相同，继续比较下一个参数。
3. 所有参数位置的匹配类别都相同，两个候选无法排序。
4. 候选集经过两两优先级比较后必须存在唯一最优候选；否则调用报告二义性。

例如：

```feng
func choose(a: UserType, b: ParentSpec) {}
func choose(a: UserSpec, b: OtherType) {}
```

若第一个候选在第一个参数是“精确 `type`”，第二个候选在第一个参数是“具体非父级 `spec`”，则直接选择第一个候选；不再用第二个参数反转结果。

该规则是参数匹配类别的字典序比较。声明顺序、候选收集顺序和模块导入顺序均不得影响结果。

## 9. 二义性与调用侧消歧

以下情况在调用点报告现有重载二义性诊断 `AE0511`：

- 存在多个适用候选，但逐参数比较后没有唯一最优候选。
- 两个无父子关系的 `spec` 候选对某个具体类型均为同级非父级匹配。
- 两个父级 `spec` 候选只能依靠父级层数区分。
- 多个泛型候选得到相同的逐参数匹配类别。
- 外层泛型参数在具体 receiver / owner 实例中替换后，使多个候选得到相同的逐参数匹配类别。

调用方可以通过以下方式改变实参静态类型并消歧：

```feng
let target: DesiredSpec = value;
call(target);

call((DesiredSpec)value);
```

若调用显式提供类型实参，则由显式泛型参数数量先筛选候选：

```feng
select<int>(value);
```

显式类型实参不是同参数数量泛型候选之间的额外排序分数；筛选后仍按逐参数匹配类别比较。

## 10. 与 Spec 向上 Coercion 的阶段关系

本次重载优化先于 `spec` 向上 coercion 实施，二者边界如下：

1. 本次允许 `ChildSpec` / `ParentSpec` 等参数声明共存，并建立统一的调用候选比较框架。
2. 本次只把当前已有类型匹配能力产生的候选纳入比较，不新增子 `spec` 值到父 `spec` 值的转换资格。
3. 后续 `spec` 向上 coercion 实施后，父级候选会按本文定义自然进入“父级 `spec`”或“泛型父级 `spec`”类别。
4. Witness 父级路径仍只在唯一候选选定、目标形参类型确定后记录；候选探测不得生成最终 witness lowering 结论。
5. `spec` 向上 coercion 文档中当前关于“潜在重叠在声明阶段冲突”的阶段性规则，需要在本次方案审批并更新权威规范时同步修订；本文件创建阶段不修改该文档。

## 11. Semantic 实现设计

### 11.1 用逐参数结果替换整体整数优先级

当前 `compute_overload_match_priority(...)` 返回单个 `int`，不能表达本方案。实施时应改为明确的数据结构，例如：

```c
/* 单个参数位置的重载匹配类别，枚举顺序即比较顺序。 */
typedef enum FengOverloadParamMatchRank {
    FENG_OVERLOAD_PARAM_MATCH_EXACT_TYPE = 0,
    FENG_OVERLOAD_PARAM_MATCH_CONCRETE_NON_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_GENERIC_NON_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_GENERIC_PARENT_SPEC,
    FENG_OVERLOAD_PARAM_MATCH_UNCONSTRAINED_GENERIC
} FengOverloadParamMatchRank;

/* 一个适用候选的逐参数匹配结果。 */
typedef struct FengOverloadMatch {
    FengOverloadParamMatchRank *param_ranks;
    size_t param_rank_count;
} FengOverloadMatch;
```

最终命名和所有权形式可按现有 Semantic 内部结构统一，但必须保留以下事实：

- 每个参数位置独立保存类别。
- 外层类型参数必须先按 receiver / owner 实例完成替换；替换结果不标记为调用级泛型。
- 保留该位置是否来自函数或方法自身的调用级泛型参数。
- 保留非父级 `spec` 与父级 object-form `spec` 的区别。
- Callable-form、union-form 与 intersection-form 只能产生非父级类别。
- 不保存或计算父级距离。
- 比较函数只做从左到右的字典序比较。

### 11.2 统一候选探测结果

现有参数匹配函数除返回是否适用外，还需要为适用候选产出逐参数类别。泛型推导、四种 spec form 的约束验证与类别计算应在同一次候选探测中共享结果，避免重复解析类型关系。

顶层函数、模块公开函数、普通成员方法、静态方法和 `fit` 方法的所有调用决议入口必须复用同一个比较函数，不得分别复制优先级规则。

变长参数展开时，按实际实参从左到右比较；落在变长位置的普通实参使用变长元素形参的匹配类别。预打包 `...array` 的既有形状检查与拒绝原因保持不变。

### 11.3 候选集选择

候选集选择不得依赖遍历顺序。实现应维护当前唯一最优候选，并正确处理以下状态：

- 新候选严格优于当前候选：替换当前候选并清除同级二义状态。
- 新候选严格劣于当前候选：忽略新候选。
- 新候选与当前候选逐参数相等：记录二义性。

如果候选优先关系未来可能不是全序，实施时必须先验证上述单遍算法仍成立；若不成立，应先构造全部适用候选，再统一求唯一最优候选，不能依赖声明顺序得到偶然结果。本文当前定义的固定类别字典序是全序，相同向量表示无法消歧。

### 11.4 放开声明阶段潜在重叠检查

实施时需要调整：

- `validate_top_level_overload_overlap(...)`
- `validate_type_member_overload_overlap(...)`
- `validate_fit_member_overload_overlap(...)`
- `signatures_potentially_overlap(...)` 及其调用者
- `@mixable` 生成候选使用普通重载重叠结果的相关路径

上述路径不再因不同参数类型存在共同可接受实参而拒绝声明，但仍须保留 §5.2 的重复签名、仅返回类型区分、泛型 identity 和变长参数检查。

生成的 `@mixable` wrapper 必须与手写方法使用相同的声明合法性和调用决议规则；不得为生成方法保留一套更严格或更宽松的潜在重叠特判。

### 11.5 Semantic 结果与 Codegen

调用决议结束后，Semantic 继续记录唯一目标声明、推导或显式提供的类型实参，以及替换后的参数和返回类型。Codegen 不重新比较候选，也不重新推导泛型参数。

本次不改变函数 ABI、泛型描述符、witness ABI 或运行时接口。

## 12. 文档实施顺序

本开发文档 Review 通过后，代码实施前先更新权威规范：

1. 更新 [Feng 函数规范](../specifications/feng-function.md)：放开调用侧可消歧的潜在重叠，定义调用点二义性和逐参数比较原则。
2. 更新 [Feng 泛型规范草案](../specifications/feng-generics-draft.md)：把当前“所有非泛型候选均高于泛型候选”的规则替换为本文匹配类别顺序。
3. 检查错误码规范：若继续复用 `AE0511` 且不新增诊断码，只需确认文案适用于新的调用点二义性，不无关改动错误码。
4. 更新 [Object-form Spec 向上 Coercion 开发设计](./feng-object-spec-upcasting-dev.md)：仅引用本文作为重载规则来源，删除其中已经被本方案替代的阶段性冲突结论，避免重复定义。

完成上述规范更新并经人工确认后，才能开始代码和测试变更。

## 13. 代码实施顺序

1. 引入逐参数匹配类别和统一字典序比较函数。
2. 在候选比较前完成 receiver / owner 外层泛型参数替换，使动态具体化冲突留到调用点判定。
3. 让普通参数匹配产出“精确 `type` / 具体非父级 `spec` / 父级 `spec`”类别。
4. 让泛型推导与四种 spec form 的约束验证产出“泛型非父级 `spec` / 泛型父级 `spec` / 无约束泛型”类别。
5. 改造全部顶层函数与方法调用决议入口，移除整体 `match_priority` 依赖。
6. 放开不同参数类型的潜在重叠声明检查，保留声明 identity 冲突、必然死角与其他既有禁止项。
7. 统一本地、`fit`、`@mixable` 生成成员和跨包恢复声明的行为。
8. 确认 Semantic 选定声明、泛型实参和 Codegen 消费路径不受候选遍历顺序影响。
9. 完成专项测试后执行全量回归 `make test`。

## 14. 测试范围

### 14.1 声明合法性

- 顶层函数允许 `UserType` / `UserSpec` 参数重载。
- 成员方法、静态方法与 `fit` 方法允许相同组合。
- 允许 `ChildSpec` / `ParentSpec` 参数重载。
- 允许两个存在共同满足类型的不同 `spec` 参数重载。
- 外层泛型参数仅在部分具体实例中与具体参数重叠时允许声明。
- 重复参数签名继续拒绝。
- 仅返回类型不同继续拒绝。
- 仅泛型参数名或约束不同继续拒绝。
- 变长参数既有冲突继续拒绝。

### 14.2 单参数优先级

- 精确 `type` 胜过具体非父级 `spec`。
- 具体非父级 `spec` 胜过泛型非父级 `spec`。
- 泛型非父级 `spec` 胜过父级 `spec`。
- 父级 `spec` 胜过泛型父级 `spec`。
- 泛型父级 `spec` 胜过无约束泛型。
- Callable-form、union-form 与 intersection-form 的非泛型匹配归入具体非父级 `spec`。
- Callable-form、union-form 与 intersection-form 的泛型约束匹配归入泛型非父级 `spec`。
- 不满足泛型约束的候选在比较前被剔除。
- 显式类型实参只保留泛型 arity 匹配候选。
- 显式 `spec` 类型标注或 cast 可以选择原本非精确的候选。

### 14.3 多参数顺序

- 第一个参数已分出优先级时，不再由后续参数反转。
- 前几个参数同级时，由首个不同的后续参数决定。
- 全部参数同级时报告 `AE0511`。
- 候选声明顺序交换后结果不变。
- 跨包声明恢复顺序变化后结果不变。
- 变长位置的多个实参仍按实际参数顺序参与比较。

### 14.4 不比较父级距离

- 直接父和传递祖先同时作为候选时，不按层数选择，调用报告二义性。
- 把实参显式标注或转换为直接父后，直接父候选成为具体非父级 `spec` 并胜出。

### 14.5 动态死角

- `Container<int>` 中 `test(T)` / `test(string)` 只有前者适用，调用成功。
- `Container<string>` 中上述两个参数替换后同为精确 `string`，调用报告 `AE0511`。
- 不因 `Container<string>` 的动态冲突拒绝 `Container<T>` 声明或 `Container<string>` 实例化。
- 外层泛型参数替换后的参数不按调用级泛型降级。
- 函数或方法自身的泛型候选仍可通过显式类型实参到达。

### 14.6 回归与现有测试调整

仓库中已有测试明确断言以下旧行为：

- `type` / `spec` 潜在重叠在声明阶段被拒绝。
- 两个存在共同满足类型的 `spec` 参数在声明阶段被拒绝。
- 所有可匹配的非泛型候选均优先于泛型候选。

实施本方案时必须先取得人工批准，再修改这些既有测试的预期；不得直接删除测试。应把它们改造成新的声明合法性、精确优先级或调用点二义性覆盖，并新增 FCTS 行为测试验证最终选中的函数体。

所有非文档变更完成后执行全量回归：

```text
make test
```

## 15. 验收标准

本方案交付需同时满足：

- 重载声明不再因不同参数类型的潜在语义重叠被提前拒绝。
- 重复签名及其他明确禁止项没有被放宽。
- 所有适用候选先完成泛型推导与约束验证。
- 每个参数位置按本文类别生成稳定匹配结果。
- 外层泛型参数具体化产生的动态重叠只在实际调用点诊断，不阻止其他实例。
- 多参数严格从左到右比较，不累计分数。
- 父级 `spec` 不比较继承层数。
- Callable-form、union-form 与 intersection-form 不进入父级类别。
- 无唯一最优候选时稳定报告 `AE0511`。
- 显式标注、显式 cast 和显式泛型实参能够按本文规则消歧。
- 顶层函数、成员方法、静态方法、`fit` 方法、本地与跨包调用行为一致。
- Codegen 与 runtime 不承担重载决议，且无 ABI 或运行时性能开销变化。
- 专项测试与全量 `make test` 全部通过。
