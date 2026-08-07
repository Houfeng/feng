# Object-form Spec 向上转换问题备忘

> **状态**：历史备忘，方案已收敛；实施以 [Object-form Spec 向上 Coercion 开发设计](./feng-object-spec-upcasting-dev.md) 为准。
> **日期**：2026-08-03。
> **性质**：engineering 历史备忘。本文保留当时的事实、历史讨论与候选方向，不再作为当前实现依据。

> **后续决策（2026-08-07）**：object-form 子 `spec` 值允许在 expected-type 位置沿已声明父关系自动建立父视角，也允许使用等价的显式 cast；witness 只保存具名直接父级，传递祖先投影由编译器展开为固定读取链。本文后续关于“只承诺显式转换”和其他 witness 候选方向的内容均仅作历史记录。

## 1. 目的

本文记录 object-form `spec` 值从子 `spec` 视角转换到父 `spec` 视角时的实现缺口，例如：

```feng
spec A {
    func a(): string;
}

spec B: A {
    func b(): string;
}

type X: B {
    func a(): string { return "a"; }
    func b(): string { return "b"; }
}

let b: B = X {};
let a: A = (A)b;
```

当前权威规范要求允许显式的“子 object-form `spec` → 父 object-form `spec`”向上转换，但当前编译器尚未实现这条转换。

本文聚焦以下问题：

1. 为什么现有 `{ subject, witness }` 值表示不能直接完成该转换。
2. 多父、传递祖先和菱形继承对 witness 布局的影响。
3. 如何在不做运行时搜索的前提下，由编译器生成固定的转换代码。
4. 各候选方案在运行时成本、静态数据大小、编译期生成量和跨包 ABI 上的差异。

本文不讨论或决定：

- 是否增加父 `spec` 到子 `spec` 的向下转换。
- 是否允许无关 object-form `spec` 之间转换。
- 是否把隐式赋值扩展为子 `spec` 到父 `spec` 的隐式转换。
- union-form 到共同 object-form `spec` 的运行时投影。
- intersection-form 到单个成员 `spec` 的转换语义。

## 2. 相关现有规范与实现

### 2.1 规范状态

[Feng 语言 `spec` 规范](../specifications/feng-spec.md) 当前规定：

- 具体 `type` 可以显式转换到其已经满足的 object-form `spec`。
- 子 object-form `spec` 可以显式转换到其父 object-form `spec`。
- 转换资格必须在编译期确定。
- 转换成立后，编译器必须直接构造静态已知的目标 `spec` 视角。
- 运行时不得搜索候选 `spec`、试探转换或回退。

规范当前只承诺**显式**向上转换。历史 FCTS 注释中的：

```feng
let a: A = b;
```

是隐式赋值形式，不能据此推导语言必须支持隐式子 `spec` 转父 `spec`。是否支持隐式形式属于另一项语言语义决策。

### 2.2 当前 object-form `spec` 值表示

[Object-form Spec Codegen 交付记录](./feng-spec-codegen-delivered.md) 定义的当前运行时表示为：

```c
struct FengSpecValue__S {
    void *subject;
    const struct FengSpecWitness__S *witness;
};
```

其中：

- `subject` 是实际接收者，参与现有生命周期管理。
- `witness` 是与目标 `spec` 结构匹配的静态只读分发表，不参与 ARC。
- 不同 `spec` 具有不同的具名 witness 结构。
- 具体 `(T, S)` 组合的 witness 当前按需生成并缓存。
- 成员调用形态为 `value.witness->slot(value.subject, ...)`。

### 2.3 当前已支持的转换

具体类型到祖先 `spec` 已经能够在具体类型仍然静态可见时直接发码：

```feng
let x = X {};
let a: A = x;
```

编译器在该站点知道具体类型 `X` 和目标 `spec A`，可以直接构造：

```c
struct FengSpecValue__A a = {
    .subject = x,
    .witness = &FengSpecWitness__X__as__A,
};
```

[FCTS spec 测试](../../fcts/fcts_bin/src/test_spec.ff) 中已有“concrete type to ancestor spec”行为覆盖。

### 2.4 当前未支持的转换

当源值的静态类型已经是子 `spec B` 时，具体类型已经从语言值视角擦除：

```feng
let b: B = X {};
let a: A = (A)b;
```

当前 semantic 的 object-form coercion 记录路径只面向具体 `type`、builtin、array 等可形成具体 subject key 的来源；cast 校验也没有 object-form 子 `spec` 到父 `spec` 的专门分支。因此，上述显式转换当前会被拒绝。

相关实现入口包括：

- [`record_object_spec_coercion_site_if_applicable`](../../src/semantic/analyzer.c)：记录具体 subject 到 object-form `spec` 的 coercion。
- [`type_ref_satisfies_spec_type_ref`](../../src/semantic/analyzer.c)：当前不把 object-form `spec` 声明作为满足另一 object-form `spec` 的具体 subject。
- [`validate_cast_expr`](../../src/semantic/analyzer.c)：当前没有 object-form `spec` 父级可达转换规则。
- [`cg_emit_expr`](../../src/codegen/codegen.c)：现有 object-form coercion 根据具体 subject key 选择 `(T, S)` witness，没有已擦除 spec 值到父 spec 值的 lowering。

[FCTS spec 测试](../../fcts/fcts_bin/src/test_spec.ff) 保留了被注释的“child spec assignment to parent spec”用例及原始问题说明。

代码中已有的 `spec-to-spec slot witness` 主要服务于泛型约束转发和静态成员适配，不能视为语言层 object-form `spec` 值向上转换已经实现。

## 3. 信息缺口

假设：

```feng
spec B: A {}
let b: B = X {};
```

概念上，`b` 保存：

```text
{
    subject: X*,
    witness: Witness__X__B*
}
```

构造 `A` 视角需要：

```text
{
    subject: X*,
    witness: Witness__X__A*
}
```

现有 `B` 值具有：

- 原始 `subject`；
- 当前 `B` 视角的 witness。

现有值表示或 witness ABI 没有定义：

- `Witness__X__B` 到 `Witness__X__A` 的固定映射；
- 父 witness 指针槽；
- 祖先 witness 表；
- 可复用父表的布局前缀；
- 根据 witness 取得运行时具体类型 `X` 的稳定身份；
- 根据 `(X, A)` 做动态查找的运行时注册表。

因此，问题不是父 witness 不存在。对于已知具体类型 `X`，编译器可以生成 `Witness__X__A`。问题是：源值只暴露 `B` witness 时，当前 ABI 没有从该 witness 取得对应 `A` witness 的通路。

## 4. 父关系是静态 DAG

Object-form `spec` 可以有多个父 `spec`，父关系是禁止循环的静态有向无环图，不是单继承链。例如：

```text
    A
   / \
  B   C
   \ /
    D
```

对应：

```feng
spec A {}
spec B: A {}
spec C: A {}
spec D: B, C {}
```

这带来以下事实：

1. 单个 `parent` 指针不足以表达多父关系。
2. 直接父数量在 `spec` 声明完成后静态可知。
3. 去重后的全部祖先数量也可由编译器静态计算。
4. `D → A` 可能存在多条静态路径。
5. 菱形中的 `A` 应如何去重、两条路径是否必须得到同一 witness，需要明确约束。

这里不需要因为父关系是 DAG 就引入运行时图遍历。源 `spec` 和目标 `spec` 在显式转换点均静态已知，编译器可以在语义阶段验证可达性、选择路径，并在 codegen 中发出固定槽位访问。

## 5. 历史讨论记录

历史讨论目前只保存在 [FCTS spec 测试](../../fcts/fcts_bin/src/test_spec.ff) 的注释及 Git 历史中，没有形成独立设计文档：

1. `b21fdfc5`：记录当前 `B` witness 无法取得 `A` witness，并提出保存父 witness 列表、逐层向上取得父表。
2. `12d5855e`：补充生成 `(X, B)` witness 时，还需要保证 `X` 对父级及祖先的 witness 已生成并可引用。
3. `9c22e998`：补充“祖先表 + 编译期固定偏移”和“直接父列表 + 多级固定访问”两种方向，并用 Rust trait upcasting 作类比。

原注释使用了“运行时递归向上查找”的表述。更准确的表述应是：

- 若编译器已经确定每一级父槽的位置，可以把整条路径展开为固定指针读取序列。
- 该过程没有循环、动态遍历、名称查询、候选比较或回退。
- 若只保存直接父指针，运行时指针读取次数仍随继承路径深度增长，复杂度是 `O(depth)`。
- 若源 witness 提供目标祖先的直接入口，upcast 可为单次固定读取，复杂度是 `O(1)`。

## 6. 候选方向

本节仅记录候选方向，不作选择。

### 6.1 直接父 witness 指针

每个子 witness 结构为每个直接父 `spec` 保存一个固定槽位：

```c
struct Witness__X__D {
    const struct Witness__X__B *parent_B;
    const struct Witness__X__C *parent_C;
    /* D 视角成员槽 */
};
```

编译器可将 `D → A` 固定展开为：

```c
a.subject = d.subject;
a.witness = d.witness->parent_B->parent_A;
```

也可以选择另一条静态路径：

```c
a.witness = d.witness->parent_C->parent_A;
```

性质：

- 无运行时搜索、循环或条件分支。
- 直接父转换需要一次指针读取。
- 远祖转换需要与选定路径深度相同的固定指针读取次数，即 `O(depth)`。
- 每个 witness 的额外指针数等于直接父数量。
- 生成 `(X, D)` witness 时，必须保证被引用的直接父 witness 已生成或可链接。
- 菱形路径的规范化与 witness 一致性需要额外规则。

### 6.2 扁平祖先 witness 入口

每个子 witness 保存去重后的全部传递祖先入口，目标索引由编译器确定：

```c
a.witness = d.witness->ancestors[INDEX_OF_A_IN_D];
```

实现形态可以是统一数组，也可以是编译器生成的具名、具类型字段；本文不预设具体 C ABI。

性质：

- 无运行时搜索、循环或条件分支。
- 任意祖先 upcast 为一次固定槽位读取，即 `O(1)`。
- 每个 witness 的额外指针数等于去重后的传递祖先数量。
- 必须定义稳定的祖先闭包排序和菱形去重规则。
- 生成 `(X, Child)` witness 时，必须保证全部祖先 witness 已生成或可链接。
- 深而宽的父图会增加静态表大小，但不会增加每次转换的读取次数。

### 6.3 前缀复用与额外父 vptr 的混合布局

选择一条父链作为 witness 布局前缀，使该链上的向上转换可以直接复用或固定调整现有 witness；其他父级或祖先通过额外 vptr 槽取得。

性质：

- 常见单父链可能无需读取新 witness 指针。
- 非前缀祖先可通过编译期固定 vptr 槽一次取得，保持 `O(1)`。
- 比全部祖先一律单独存指针更节省部分静态空间。
- witness 布局算法、成员去重、泛型替换和跨包 ABI 明显更复杂。
- 需要保证父 witness 的可用视图确实构成稳定前缀；当前 witness 成员闭包和子优先去重规则不能直接证明这一点。

Rust 当前 trait upcasting 的实现策略属于此类混合思路：第一条 supertrait 链使用前缀复用，其他需要调整的 supertrait 使用 `TraitVPtr` 固定槽。Rust 的具体 vtable 布局不是稳定语言 ABI，Feng 不能直接复制其实现细节。参考：

- [Rust RFC 3324：dyn upcasting](https://rust-lang.github.io/rfcs/3324-dyn-upcasting.html)
- [rustc 当前 vtable 布局实现](https://doc.rust-lang.org/nightly/nightly-rustc/src/rustc_trait_selection/traits/vtable.rs.html)

### 6.4 完全扁平的兼容前缀

让子 witness 直接内嵌父 witness 的全部槽，并使目标父 witness 成为子 witness 的兼容子对象或前缀。

性质：

- 某些父链 upcast 可以零读取地复用 witness 指针，或只做编译期固定地址调整。
- 多父、共同祖先、成员重载、子优先去重可能造成重复布局或非连续父视图。
- 若为所有父路径复制完整表，最坏情况下可能产生显著甚至组合式的静态膨胀。
- 需要重新定义当前“每个 spec 独立具名 witness 结构”的 ABI 关系。

### 6.5 转发 adapter

理论上可以构造父形状 adapter，使父成员 thunk 转发到子 witness 的对应槽。但当前目标父 witness thunk 只接收 `subject`，无法自然取得原始子 witness。要让 adapter 工作，通常还需要：

- 把原始 `{ subject, child_witness }` 包装为新的 subject；或
- 扩大目标 spec 值表示；或
- 仍然在 child witness 中保存一个与具体 child witness 绑定的父 adapter 指针。

前两种会改变值表示或引入额外分配、间接层及生命周期复杂度；第三种本质上重新回到父 witness 指针方案。该方向目前没有被确认可接受。

### 6.6 运行时注册表或动态搜索

可设想根据运行时类型身份和目标 `spec` 查询 witness，但这要求：

- spec 值携带或可恢复稳定的具体类型身份；
- runtime 保存 `(具体类型, spec) → witness` 注册信息；
- 转换执行查询、候选处理或失败路径。

这与当前规范中“转换资格在编译期确定、运行时不得搜索或回退”的要求不一致，也会增加运行时开销。若要采用，必须先由人工决定修改既有规范和性能边界；本文不建议或默认采用该方案。

## 7. 不能混淆的三类关系

### 7.1 Object-form 多父继承

```feng
spec D: B, C {}
```

这是名义父关系。本文讨论的子 `spec` 到父 `spec` 向上转换针对这一关系。

### 7.2 Intersection-form

```feng
spec I: B & C;
```

Intersection-form 当前通过 merged witness 合并成员槽，详见 [Intersection Type 设计](./feng-intersection-type-draft.md)。它不是 object-form 父关系：

- `type X: I` 当前不允许。
- `I` 没有 object-form 自有成员或父链语义。
- 当前规范没有承诺 intersection-form 值到单个成员 object-form `spec` 的转换。

如果未来需要 `I → B`，也会遇到从 merged witness 取得 `B` witness 或兼容视图的问题，但该问题应单独设计，不能借 object-form upcast 顺带扩展。

### 7.3 Union-form 到共同 Spec

```feng
spec U: X1 | X2;
let u: U = ...;
let a = (A)u;
```

即使 `X1`、`X2` 都满足 `A`，union 值仍需要先根据 active member 选择具体投影路径。当前 [Union Type 规范](../specifications/feng-union-type.md) 明确不支持该能力。它可能需要基于 tag 的运行时选择，不属于本文的普通 object-form 父级 upcast。

## 8. 必须进一步确认的正确性问题

### 8.1 菱形路径与 witness 唯一性

在 `D → B → A` 与 `D → C → A` 两条路径同时存在时，需要决定：

- 两条路径是否必须指向同一个 `(X, A)` witness 实例。
- 若只要求语义等价，如何验证各槽实现来源完全一致。
- 编译器是否选择规范路径；若选择，路径排序依据是什么。
- 跨包恢复父图后，provider 与 consumer 是否会选择相同路径和符号。

当前 `(T, S)` witness 的缓存唯一化是实现事实，但其作用域、可见 `fit`、导入恢复和链接身份是否足以成为 upcast ABI 保证，需要专门核查。

### 8.2 子优先成员与父视角语义

当前 object-form `spec` 成员闭包存在父成员展开、同签名去重和子优先规则。需要确定：

- `B` 重新声明与 `A` 相同签名成员时，`B → A` 后应使用哪一个实现来源。
- 独立生成的 `(X, A)` witness 是否必然与 `B` witness 中继承的 `A` 视角一致。
- 通过可见 `fit` 满足成员时，不同编译单元是否可能得到不同实现来源。

只有这些语义明确后，才能判断菱形中的两条父路径是否可无条件合并。

### 8.3 默认 witness

无初始化的 `B` 值使用 `B` 的默认 subject 和默认 witness。把它转换为 `A` 时不能未经证明就复用 `A` 的独立默认 witness，因为：

- `A` 默认 witness 的 thunk 可能预期 `A` 默认 subject 布局。
- 实际 subject 仍可能是 `B` 的默认 subject。
- `B` 默认 subject 是否以兼容方式包含 `A` 所需状态，当前没有 upcast ABI 保证。

可能需要为“`B` 默认 subject 作为 `A`”生成专门父 witness，或为默认 subject 定义兼容布局。该问题必须纳入方案，不能只验证普通具体类型 `X`。

### 8.4 Builtin、array、enum 与值语义 subject

具体类型进入 `spec` 时可能使用直接对象、共享引用、scalar box、value box 或其他 subject storage。向上转换必须保持同一 subject 的所有权和布局解释正确，并确保目标父 witness 的 thunk 与该 storage 形态匹配。

如果父 witness 直接来自既有 `(subject kind, Parent)` 组合，该问题可能自然解决；如果通过布局前缀或 adapter 复用子 witness，则必须逐类验证。

### 8.5 泛型父 Spec

例如：

```feng
spec A<T> {}
spec B<T>: A<T> {}
```

需要保证：

- 父 witness 入口携带正确的类型实参替换结果。
- 祖先排序和符号名基于规范声明身份，而不是源码短名或别名。
- 泛型共享体中的 `B<T> → A<T>` 不要求运行时重新解析类型实参。
- 具体 wrapper、descriptor 和 witness 的引用关系在本地与跨包场景一致。

### 8.6 静态成员与泛型约束槽

Object-form `spec` 的静态成员不能通过 spec 值访问，但可通过泛型约束 witness 分派。值级 upcast witness 与泛型约束 slot witness 是否共享布局或生成机制，目前没有结论。实现时不能因为已有内部 `spec-to-spec slot witness` 就默认两者 ABI 等价。

## 9. 编译期生成与跨包问题

任何保存父或祖先 witness 指针的方案都会扩大当前按需生成闭包。

例如，若要生成：

```text
Witness__X__D
```

且其中引用：

```text
Witness__X__B
Witness__X__C
Witness__X__A
```

编译器必须保证这些被引用实体：

1. 已在当前编译单元生成；或
2. 由 provider 以稳定符号导出；或
3. 能由 consumer 依据公开声明事实生成且不依赖 provider 私有源码。

需要核查：

- `.ft` 是否保存恢复父图、泛型替换和 witness 目标身份所需的全部事实。
- `.fb` 二进制分发是否允许 consumer 补生成父 witness，还是必须由 provider 提供。
- private representation type 是否可能进入公开 witness thunk 签名或生成依赖。
- provider 与 consumer 对祖先闭包排序、去重和符号命名是否完全一致。
- dead stripping / LTO 是否允许删除未实际引用的祖先表，而不改变语言 ABI。

这正是历史讨论中“Rust 会为 upcast 所需表面做更完整的编译期生成，而 Feng 当前按需 witness 路径不会自动生成全部祖先闭包”这一差异的核心。

## 10. 性能与空间评估维度

最终方案至少需要分别评估：

| 维度 | 需要回答的问题 |
| --- | --- |
| Upcast 运行时成本 | 零读取、一次读取，还是 `O(depth)` 固定读取？是否产生分支？ |
| 成员调用成本 | Upcast 后是否仍为一次 witness 槽读取 + 一次函数指针调用？ |
| 分配 | 转换是否需要 box、wrapper 或其他堆分配？ |
| 值大小 | `{ subject, witness }` 是否保持不变？ |
| Witness 大小 | 与直接父数量、祖先数量还是成员闭包大小相关？ |
| 静态代码大小 | 是否为所有祖先生成 thunk、adapter 或重复表？ |
| 编译时间 | 祖先闭包生成、去重、泛型实例化和跨包恢复成本如何？ |
| 链接与 ABI | witness 符号和槽位是否稳定，是否需要 ABI 版本升级？ |
| 生命周期 | 转换是否只是复制/移动同一 subject，ARC 操作是否保持等价？ |

根据项目原则，任何增加每次成员调用成本、增加 upcast 运行时分支、引入额外分配或扩大 runtime 私有 ABI 的方案，都必须由人工决策后才能实施。

## 11. 待人工决策事项

在实现前，至少需要明确以下决策：

1. 语言层是否只实现规范已经承诺的**显式**子 `spec` → 父 `spec` 转换；隐式转换继续禁止。
2. 采用直接父指针、扁平祖先入口、混合前缀/vptr，还是其他通用布局。
3. Upcast 是否要求严格 `O(1)`，还是允许无循环的 `O(depth)` 固定指针读取。
4. 是否要求 upcast 后的每次成员调用成本与普通 object-form `spec` 调用完全一致。
5. 菱形祖先采用唯一 witness、规范路径还是只保证语义等价。
6. 父/祖先 witness 的编译期生成闭包与按需裁剪规则。
7. 默认 witness、value box、scalar box 等非普通 subject 的父视角生成规则。
8. 泛型父 `spec` 和跨包 `.ft` / `.fb` 的稳定符号及 ABI 规则。
9. 是否以及如何与 intersection merged witness、泛型 constraint witness 共享基础设施。
10. 现有私有 runtime ABI 是否需要变化；若需要，明确版本和兼容策略。

在这些问题没有收敛前，不应只在 cast 校验中加入父级可达特判，也不应只为单父单层用例增加专用 codegen 分支。

## 12. 后续测试范围备忘

方案确定并实施时，应新增测试，而不是修改现有测试来掩盖行为差异。至少应覆盖：

### 12.1 Semantic

- 直接父显式 upcast 成功。
- 传递祖先显式 upcast 成功。
- 多父分别 upcast 到每个父成功。
- 菱形 upcast 到共同祖先成功且路径语义一致。
- 父到子、无关 spec、依赖运行时具体类型的转换继续失败。
- 隐式子 spec 到父 spec 是否失败，按最终语言决策锁定。
- 泛型父 spec 类型实参替换正确。
- 跨模块和跨包父关系恢复正确。

### 12.2 Codegen

- 生成代码不含运行时搜索、循环、候选比较或回退。
- 直接父与远祖使用最终方案规定的固定槽位访问。
- 菱形不会生成不一致的父 witness。
- Upcast 保留原 subject，不错误复制、释放或重复 retain。
- 方法、`let` 字段 getter、`var` 字段 getter/setter 均通过正确父 witness 分派。
- 默认 witness、对象 subject、builtin/enum/array/value box subject 均正确。
- 泛型共享体、wrapper 和 constraint witness 路径正确。

### 12.3 FCTS

- 具体类型 → 子 spec → 父 spec 的端到端行为。
- 多父和菱形继承的端到端行为。
- 父视角成员调用和字段读写行为。
- 跨包 provider/consumer 的端到端行为。

完成实现后必须按项目规则执行全量回归 `make test`，且不得在 Codex 沙箱中执行。

## 13. 当前结论

截至本文记录日期，可以确认：

1. 当前规范要求显式 object-form 子 `spec` → 父 `spec` 向上转换。
2. 当前 semantic 与 codegen 尚未实现该能力。
3. 现有 `{ subject, witness }` 表示保留 subject，但 child witness 没有取得目标 parent witness 的 ABI 通路。
4. 多父和菱形关系不要求运行时搜索；父图、目标和路径均可在编译期确定。
5. 直接父列表可生成无循环、无搜索的固定多级读取，但远祖成本为 `O(depth)`。
6. 直接祖先入口或混合前缀/vptr 可以使任意已知目标 upcast 保持 `O(1)`，代价是更复杂的 witness 布局和更大的编译期生成闭包。
7. Rust 当前采用混合 vtable 策略，不是简单的统一祖先数组；其思路只能作为参考，不能替代 Feng 的 ABI 决策。
8. 默认 witness、菱形唯一性、泛型、交叉类型边界和跨包生成是最终方案必须同时解决的问题。
9. 当前没有最终方案决策，不应开始代码实现。
