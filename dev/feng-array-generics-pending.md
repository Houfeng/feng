# Feng 数组 `fit` 泛型约束待开发项

> 本文档用于整理 `fit T[]` / `fit T[!]` 中由数组目标形式引入的元素类型参数如何声明、检查与导出约束。
> [docs/feng-fit-builtin-type.md](../docs/feng-fit-builtin-type.md) 是内建类型作为 `fit` 目标的专项规范；本文只写开发步骤与待定事项，不重复定义数组本体语义。

## 1. 当前前提

- `fit T[]` 与 `fit T[!]` 已支持由数组目标形式引入元素类型参数 `T`，其作用域覆盖整个 `fit` 声明。
- 当前文档和实现只支持无约束元素类型参数；尚不能表达 `T` 必须满足某个 `spec` 约束面。
- 普通 Feng 泛型已经支持 `T: Spec` 形式的约束声明，并通过语义检查与 witness 支持共享方法体；该约束面当前包括 object-form `spec` 与 callable-form `spec`。
- 数组 `fit` 目标引入的 `T` 属于 Feng 层语言泛型问题，继续走普通泛型约束 / witness 体系。
- 该能力与 `@runtime extern fn` 的 runtime API 泛型描述符互相独立。runtime API 泛型整理在 [dev/feng-runtime-generics-pending.md](./feng-runtime-generics-pending.md)。

## 2. 目标

为数组目标形式引入的元素类型参数补齐约束声明能力，使以下方向成为正规语言能力：

```feng
spec Equal<T> {
  fn same(other: T): bool;
}

fit T[!]: Searchable<T> where T: Equal<T> {
  fn indexOf(value: T): long {
    ...
  }
}
```

上例中的 `where` 只是待定占位示例；最终语法必须复用或扩展现有 Feng 泛型约束语法，不得在实现阶段临时发明。

需要解决的问题：

- 数组目标形式引入的 `T` 如何声明约束。
- 约束如何进入 `fit` 块体、右侧 object-form `spec` 适配和方法签名。
- object-form 约束如何参与方法体中的成员访问、方法调用和 witness 分发。
- callable-form 约束如何参与方法体中的直接调用和 invoke witness 分发。
- 约束如何导出到 `.ft`，供跨包消费者使用。
- 约束如何与 `fit T[]` / `fit T[!]` 的目标解析、孤儿适配和冲突规则协作。

## 3. 设计边界

### 3.1 与普通泛型约束保持一致

数组目标形式引入的 `T` 是语言层类型参数。它的约束语义应与普通泛型声明中的 `T: Spec` 保持一致：

- 约束目标必须是普通泛型约束规则允许的 `spec` 引用；当前应覆盖 object-form `spec` 与 callable-form `spec`。
- 约束满足检查复用现有可见 `fit` / `spec` 关系。
- 方法体内对 `T` 的 object-form 约束成员访问走普通 witness 分发。
- 方法体内对 `T` 的 callable-form 约束直接调用走普通 callable constraint invoke witness 分发。
- callable-form `spec` 可以作为 `T: Mapper` 这类泛型约束，但仍不得作为 `fit A: Mapper` 的右侧契约适配目标；`fit` 右侧契约适配边界继续遵循 [docs/feng-fit.md](../docs/feng-fit.md)。
- 不能因为目标是内建数组 `fit` 就引入第二套约束语义。

### 3.2 与数组目标解析的关系

现有 `fit T[]` / `fit T[!]` 解析规则继续保留：

- 若 `T` 命中当前可见范围中的同名 `type`、同名类型参数或内建类型名，则绑定已有符号。
- 仅在未命中且 `T` 非内建类型名时，才把 `T` 视为由数组目标形式引入的局部元素类型参数。

新增约束语法不得破坏该规则。只有当 `T` 确认为数组目标形式引入的类型参数，才允许在同一 `fit` 声明上为它声明新约束。

### 3.3 与 runtime API 泛型无必然联系

`fit T[]` 支持描述约束不依赖 runtime API 泛型描述符：

- 它面向 Feng 语义检查和 Feng 泛型共享体。
- 它使用普通泛型约束与 witness。
- 它不要求 C runtime 理解 witness。
- 它不应通过 runtime descriptor 反向影响语言层约束模型。

反过来，runtime API 泛型也不能替代数组 `fit` 约束声明能力。标准库可能临时通过 runtime helper 实现某些能力，但语言层仍需要正规的元素类型能力表达入口。

## 4. 待定语法点

需要在实现前确认最终声明语法。候选方向包括：

### 4.1 目标内联约束

```feng
fit T: Equal<T>[] {
  ...
}
```

该形态会与数组类型语法、`fit A: B` 契约适配分隔符产生歧义，当前不推荐直接采用，除非 parser 与规范能给出无歧义解释。

### 4.2 独立约束子句

```feng
fit T[] where T: Equal<T> {
  ...
}

fit T[!]: Searchable<T> where T: Equal<T> {
  ...
}
```

该形态能把“左侧目标”和“类型参数约束”分开，更适合数组目标形式引入的局部类型参数。若采用，需要把 `where` 子句纳入 `fit` 通用语法或内建数组 `fit` 专项语法。

### 4.3 显式类型参数头

```feng
fit<T: Equal<T>> T[] {
  ...
}
```

该形态与普通泛型声明头接近，但会改变当前 `fit T[]` “由目标形式隐式引入 T”的书写模型。若采用，需要同步更新 [docs/feng-fit.md](../docs/feng-fit.md) 与 [docs/feng-fit-builtin-type.md](../docs/feng-fit-builtin-type.md)。

### 4.4 当前建议

当前建议优先评估独立约束子句，因为它最少干扰既有数组目标解析规则，也能同时覆盖自扩展与契约适配：

```feng
fit T[] where T: Equal<T> {
  ...
}

fit T[!]: Searchable<T> where T: Equal<T> {
  ...
}
```

最终采用前必须先完成语法歧义检查，并由人工确认。

## 5. 语义规则草案

- [必须] 数组目标形式引入的类型参数可以声明一个或多个普通泛型约束。
- [必须] 约束目标的合法性、父约束传递、泛型 `spec` 实参检查，复用 [docs/feng-generics-draft.md](../docs/feng-generics-draft.md) 的普通泛型约束规则；当前包括 object-form `spec` 与 callable-form `spec`。
- [必须] `fit` 块体内，受 object-form 约束的元素类型参数可使用约束提供的字段、方法和 `spec` 视角能力。
- [必须] `fit` 块体内，受 callable-form 约束的元素类型参数可按约束签名直接调用，例如 `fn run(value: T) { value(arg); }` 这类路径必须复用普通 generic callable constraint invoke lowering。
- [必须] 约束在 direct-call 与 spec-call 路径中都应保持与普通泛型方法一致的 witness 行为。
- [必须] 数组目标方法的参数类型、返回类型和局部类型引用中出现的 `T`，都必须携带该 `T` 的约束上下文。
- [必须] 当数组 `fit` 同时声明右侧 object-form `spec` 时，右侧 `spec` 的类型实参与元素类型约束必须一起参与合法性检查。
- [必须] callable-form `spec` 只作为元素类型参数约束参与方法体能力检查，不改变 `fit` 右侧契约适配只能使用 object-form `spec` 的边界。
- [必须] 公开导出的数组 `fit` 若包含元素类型约束，`.ft` 必须完整记录类型参数、约束目标、未实例化签名骨架和右侧 `spec` 关系。
- [禁止] 对绑定到已有 `type`、已有类型参数或内建类型名的 `T` 再按数组目标局部参数声明新约束。
- [禁止] 使用 runtime API 泛型描述符替代语言层约束检查。
- [禁止] 为数组 `fit` 约束引入运行时查找、动态分派或装箱。

## 6. 分步 TODO

### 6.1 规范收敛

- [ ] 确认最终约束语法。
- [ ] 更新 [docs/feng-fit-builtin-type.md](../docs/feng-fit-builtin-type.md)，加入数组目标形式引入类型参数的约束声明规则。
- [ ] 必要时更新 [docs/feng-fit.md](../docs/feng-fit.md)，让通用 `fit` 语法能引用该约束子句。
- [ ] 必要时更新 [docs/feng-generics-draft.md](../docs/feng-generics-draft.md)，说明该能力复用普通泛型约束语义。
- [ ] 更新 [docs/feng-symbol-table.md](../docs/feng-symbol-table.md)，明确 `.ft` 中如何导出数组目标局部类型参数约束。

验收口径：

- 权威规范中只有一个地方定义数组目标约束语法。
- 其他文档只引用，不重复描述完整规则。

### 6.2 Parser / AST

- [ ] 扩展 `fit` 声明 AST，承载数组目标形式引入类型参数的约束列表。
- [ ] 解析最终确认的约束语法。
- [ ] 在语法或早期语义阶段拒绝约束非数组目标局部参数的写法。
- [ ] 保持现有 `fit T[]`、`fit T[!]`、`fit int[]`、`fit Existing[]` 解析行为不变。

验收口径：

- parser 能稳定区分数组目标、右侧契约适配和元素类型约束。
- 非法语法不会落到模糊错误。

### 6.3 Semantic

- [ ] 在数组目标形式引入 `T` 时，同时注册其约束事实。
- [ ] 复用普通泛型约束检查，验证 object-form / callable-form 约束目标、泛型实参和父约束。
- [ ] 方法体分析时，将 `T` 作为受约束类型参数加入当前泛型上下文。
- [ ] 方法体内通过 `T` 使用 object-form 约束成员时，走普通 witness sidecar / 约束调用路径。
- [ ] 方法体内直接调用受 callable-form 约束的 `T` 值时，走普通 callable constraint invoke 路径。
- [ ] 右侧 `spec` 适配检查必须同时考虑数组目标、元素类型约束和 `fit` 块新增成员。
- [ ] 诊断冲突：约束缺失、约束不满足、约束目标非法、约束写在非局部 `T` 上、跨包导入 `.ft` 约束缺失。

验收口径：

- `fit T[] where T: Named` 中可以在方法体内访问 `T` 的 `Named` 能力。
- `fit T[] where T: Mapper` 中若 `Mapper` 是 callable-form `spec`，可以在方法体内按 `Mapper` 签名直接调用 `T` 值。
- `fit int[] where T: Named` 一类写法被稳定拒绝。
- 未满足约束的调用点给出明确诊断。

### 6.4 Symbol / package export

- [ ] `.ft` 中 ARRAY type node 的元素 `TYPE_PARAM_REF` 必须关联约束事实。
- [ ] 公开数组 `fit` 导出时，保留类型参数顺序、约束目标、右侧 `spec` 关系和未实例化成员签名。
- [ ] `use` 外部包时，消费者只依赖 `.ft` 即可恢复数组目标约束并完成语义分析。
- [ ] 孤儿适配导出规则继续按 [docs/feng-fit.md](../docs/feng-fit.md) 执行，不因元素类型约束而放宽。

验收口径：

- provider 源码不可见时，consumer 仍能正确检查 `fit T[]` 约束。
- `.ft` 丢失约束事实时应被视为非法或触发明确诊断。

### 6.5 Codegen

- [ ] 数组 `fit` 共享方法体继续接收必要的泛型 descriptor / witness 上下文。
- [ ] 受约束 `T` 的方法调用、字段访问、spec coercion 与普通泛型约束 codegen 保持一致。
- [ ] direct-call 不引入额外装箱、运行时查表或动态分发。
- [ ] spec-call 继续复用现有 witness thunk 模型。
- [ ] 泛型数组返回值如 `Span<T>`、`T[]` 必须在调用点按接收者元素类型和约束上下文实例化。

验收口径：

- 生成 C 中对约束成员的访问经由普通 witness 路径。
- 直接数组方法调用成本不高于当前无约束数组 `fit` 方法调用。
- 现有 builtin array fit 用例保持通过。

### 6.6 标准库接入

- [ ] 在具备约束语法后，评估 `indexOf` 是否应提供约束型 Feng 层实现、runtime helper 型实现，或两者分别承担不同能力。
- [ ] 若声明 `Equal<T>` / `Comparable<T>` 等标准契约，必须先进入对应标准库 / 语言规范文档。
- [ ] 更新 [docs/feng-std-array.md](../docs/feng-std-array.md)，只记录标准库数组 API 语义，不在其中重复数组目标约束主规则。

验收口径：

- 标准库数组 API 的元素能力要求可被静态表达。
- 数组本体语义仍以 [docs/feng-builtin-type.md](../docs/feng-builtin-type.md) 为准。

### 6.7 测试与回归

- [ ] parser：合法约束语法、非法位置、与右侧 `spec` 适配组合。
- [ ] semantic：约束满足、约束缺失、约束目标非法、非局部 `T` 约束报错、跨包 `.ft` 恢复。
- [ ] symbol：ARRAY target 中 `TYPE_PARAM_REF` 约束导出与导入。
- [ ] codegen：受 object-form 约束数组 `fit` 方法 direct-call 与 spec-call；受 callable-form 约束数组 `fit` 方法 direct invoke。
- [ ] smoke / std：至少一个约束型数组扩展端到端场景。
- [ ] 全量执行 `make test`。

验收口径：

- 新增测试覆盖 parser、semantic、symbol、codegen、smoke / std。
- 全量回归通过。

## 7. 当前明确不做

- [ ] 不在本文定义 runtime API 泛型 descriptor。
- [ ] 不用 runtime descriptor 代替语言层 `T: Spec` 约束。
- [ ] 不允许数组目标约束绕过普通泛型约束满足检查。
- [ ] 不为 `fit T[]` / `fit T[!]` 引入运行时方法查找或装箱。
- [ ] 不在语法未确认前修改 parser / semantic 实现。

## 8. 建议执行顺序

1. 先确认最终约束语法。
2. 再更新权威文档，明确数组目标形式引入类型参数的约束规则。
3. 再扩展 parser / AST。
4. 再接入 semantic 约束检查与方法体上下文。
5. 再补 `.ft` 导出 / 导入。
6. 再完成 codegen 与标准库接入。
7. 最后补齐测试并执行全量回归。

## 9. 交付约束

- 实现前必须先由人工确认最终语法。
- 不得为了某个标准库方法引入不可推广的特殊处理。
- 任何额外运行时开销都必须先说明动机、影响和替代方案，并由人工决策。
- 每个实现阶段都必须补测试并执行全量回归。
