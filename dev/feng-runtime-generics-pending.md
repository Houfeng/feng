# Feng Runtime API 泛型待开发项

> 本文档用于整理 `@runtime extern fn` 的泛型支持方向、ABI 边界、实现步骤与验收口径。
> [dev/feng-runtime-interop-pending.md](./feng-runtime-interop-pending.md) 是 `@runtime extern fn` 总体互操作方案；本文只补充 runtime API 泛型能力，不重复定义 `@runtime` 的目标注解、非公开定位与 contract 白名单规则。

## 1. 当前前提

- `@runtime extern fn` 与 Feng 层普通泛型不是同一种泛型机制。前者是编译器到 C runtime contract 的 ABI 描述机制；后者是语言级泛型、约束与 witness 分发机制。
- `FengGenericParamDescriptor` 与其中的 `witness` 服务非 runtime 泛型共享体：在二进制分发场景下，结构按具体类型单态，方法体可共享，共享方法体通过 descriptor 与 witness 操作已单态结构。
- runtime API 的实现方是 C 方法，不能直接消费 Feng witness。runtime 泛型需要的是编译期已知、且 C runtime 可直接理解的类型信息。
- 当前 generic runtime extern 已支持 `T[]` 这类可降为稳定 C surface 的形态，例如 `feng_array_length_i64<T>(value: T[]): long` 和 `feng_array_slice<T>(value: T[], ...): T[]`。
- 当前 `T[]` 支持属于局部特化路径；后续目标是把它升级为通用 runtime 泛型参数描述机制，而不是继续为每种包裹形态增加特判。
- `fit T[]` / `fit T[!]` 支持声明元素类型约束是独立语言能力，整理在 [dev/feng-array-generics-pending.md](./feng-array-generics-pending.md)。两项能力都必须做，但职责不同，不能互相替代。

## 2. 设计边界

### 2.1 runtime API 泛型的定位

runtime API 泛型只服务 `@runtime extern fn` 的 lowering：

```feng
@runtime
extern fn helper<T>(value: T): bool;

@runtime
extern fn helper2<T>(values: T[]): long;
```

Feng 源层仍用普通泛型声明语法表达类型参数；codegen 在调用 runtime C 符号时，为每个类型实参生成并传入 runtime 专用类型描述符。

该描述符的职责是把编译期已知类型事实编码为稳定 C ABI 数据，包括但不限于：

- 类型大小。
- 值模型分类（`FengValueKind`）。
- by-value aggregate 描述符。
- managed object descriptor。
- 内建标量 / `string` / 数组 / 对象 / closure / 指针 / enum / `type` / object-form spec 等类型类别。
- 数组元素类型描述符。
- 其他 runtime contract 明确需要、且编译期可稳定获得的信息。

### 2.2 与 Feng 层泛型的边界

- runtime API 泛型不得复用普通 Feng 泛型的 witness 语义。
- runtime API 泛型可以复用同一套编译期类型事实来源，但 descriptor ABI 必须独立。
- 普通 Feng 泛型仍负责语言层类型检查、约束满足、witness 生成、共享方法体分发与二进制分发模型。
- runtime API 泛型只负责 C runtime contract 如何接收泛型类型实参信息。
- 两者在语法上都使用 `<T>`，但不应在实现上合并为同一套 descriptor 或同一套调用协议。

### 2.3 与当前 `T[]` 特判的关系

当前 `T[]` generic extern 支持应被视为 runtime API 泛型的一种普通应用场景：

- `T` 的类型信息由 runtime 泛型描述符传入。
- 数组值本身继续按现有 runtime 数组 carrier 传递。
- `T[]` 不再要求 codegen 为 wrapped shape 维护专属推导和专属 extern surface 判断。
- 原有 `feng_array_length_i64<T>`、`feng_array_slice<T>` 行为必须保持兼容，并作为迁移后的回归基线。

## 3. Runtime 泛型描述符

### 3.1 新增独立描述符

新增 runtime 专用描述符，例如：

```c
typedef struct FengRuntimeGenericParamDescriptor FengRuntimeGenericParamDescriptor;
```

该描述符与 `FengGenericParamDescriptor` 分开定义，命名上明确其消费方是 runtime contract，而不是 Feng 泛型共享体。

首版字段需按实现前的最终 ABI 设计落定，但至少应覆盖 runtime contract 需要的编译期类型事实：

- `size_t size`
- `FengValueKind value_kind`
- `const FengAggregateValueDescriptor *aggregate`
- `const FengTypeDescriptor *managed_desc`
- runtime 可识别的类型类别 tag
- 对数组等递归类型的元素描述符引用
- 对 enum、指针、object-form spec 等需要额外静态信息的类型 payload

### 3.2 不携带普通 witness 语义

runtime 描述符不把普通 Feng witness 作为操作入口：

- C runtime 不知道普通 witness 表对应哪个语言约束。
- C runtime 不知道 witness slot 布局与参数 / 返回 ABI。
- C runtime 不应通过普通 witness 实现 runtime helper 的核心语义。

若某个 runtime helper 确实需要可调用操作，应显式设计 C ABI 可消费的信息，例如：

- runtime type tag + runtime 内部分派。
- 由 codegen 生成并符合稳定 C ABI 的 helper thunk。
- contract 专属 operation table。

这些都属于 runtime contract 设计，不等同于普通 Feng 泛型 witness。

## 4. Lowering 方向

### 4.1 类型参数传递

对 `@runtime extern fn` 的每个类型参数，codegen 生成一个 runtime 泛型描述符实参。建议按类型参数声明顺序传递隐藏 descriptor 参数，并在实现前固定其在 C 符号参数列表中的位置。

示例 Feng 声明：

```feng
@runtime
extern fn feng_expression_equal<T>(left: T, right: T): bool;
```

对应 C contract 可收敛为类似形态：

```c
bool feng_expression_equal(
    const FengRuntimeGenericParamDescriptor *T,
    const void *left,
    const void *right
);
```

其中裸 `T` 参数使用地址形式传递，避免 C ABI 需要按未知大小直接传值。具体地址物化规则需与普通 Feng 调用的所有权、生命周期和临时值规则对齐。

### 4.2 包裹泛型类型

对 `T[]`、`Box<T>` 等包含类型参数的参数或返回类型，lowering 不应再依赖局部特判：

- 参数 carrier 按该 Feng 类型的自然 runtime 表示传递。
- 类型参数事实通过隐藏 runtime descriptor 传递。
- codegen 使用统一结构化类型匹配推导类型实参。
- 返回类型中含类型参数时，调用点仍按普通 Feng 泛型调用规则完成类型实例化。

### 4.3 类型实参推导

runtime 泛型 extern 的类型实参推导应从当前 `T[]` wrapped inference 泛化为结构化匹配：

- 支持从裸 `T` 参数推导。
- 支持从 `T[]` 参数推导。
- 支持从多个参数合并同一个类型参数的约束，并对冲突给出稳定诊断。
- 支持显式类型实参优先。
- 不把 runtime 泛型推导扩散到普通非 runtime extern 的 C ABI 路径。

## 5. 首个消费者：泛型相等 helper

`feng_expression_equal<T>` 可作为 runtime API 泛型的首个高价值消费者，用于标准库数组 `indexOf` 等需要对元素执行相等比较的场景。

该 helper 的设计约束：

- 不改变普通 `==` 运算符的 analyzer / codegen 规则。
- 由标准库显式调用 helper，而不是让任意泛型 `T == T` 自动通过。
- 相等语义必须与语言规范中的表达式 / spec 相等语义对齐；本文不重新定义 equality 主语义。
- 不得使用 `memcmp` 作为浮点、aggregate、object-form spec 等类型的语义兜底。
- 标量、enum、`string`、数组、指针、对象、callable、`type`、object-form spec 等可进入数组的元素类型，都必须有明确且生产级的处理策略。

如果某一类类型当前缺少规范化相等语义，必须先回到对应权威规范确定语义，再实现 helper，不得在 runtime 中临时补特殊规则。

## 6. 分步 TODO

### 6.1 规范与 ABI 收敛

- [ ] 在本文确认 runtime API 泛型与普通 Feng 泛型分层独立。
- [ ] 明确 runtime 泛型 descriptor 的名称、字段、生命周期和 C ABI 传参顺序。
- [ ] 明确裸 `T` 参数、裸 `T` 返回值、`T[]` 参数、`T[]` 返回值的 C contract carrier。
- [ ] 明确 descriptor 与 `src/runtime/feng_runtime_contract.inc` 的关系：contract 白名单仍是唯一允许符号来源，泛型 descriptor 是这些符号的隐藏 ABI 参数。
- [ ] 更新 [dev/feng-runtime-interop-pending.md](./feng-runtime-interop-pending.md) 中的 runtime lowering 章节，只做引用，不重复展开本文细节。

验收口径：

- 文档中不再把 runtime API 泛型描述符与 `FengGenericParamDescriptor` / witness 混用。
- `T[]` 被描述为通用 runtime 泛型机制的实例，而不是独立特判。

### 6.2 Runtime header / contract

- [ ] 在 runtime public ABI 中新增 runtime 泛型参数描述符和必要的类型类别 tag。
- [ ] 为 contract helper 增加 descriptor-aware C 原型。
- [ ] 保持 `src/runtime/feng_runtime_contract.inc` 作为允许 `@runtime` 声明使用的唯一符号清单。
- [ ] 若 X-macro 片段无法表达 hidden descriptor 参数，先设计 contract 条目扩展方式，再修改 codegen。

验收口径：

- generated C 只引用 runtime public ABI 中声明的符号和类型。
- runtime contract 白名单仍可被 codegen 精确检查。

### 6.3 Semantic

- [ ] 允许 `@runtime extern fn` 声明使用泛型参数的裸形态和包裹形态。
- [ ] 把 runtime generic extern 的推导从 `T[]` wrapped shape 扩展为结构化泛型匹配。
- [ ] 对冲突推导、无法推导、显式类型实参数量不匹配给出稳定诊断。
- [ ] 确保普通 C ABI extern 不获得 runtime 泛型特权。
- [ ] 确保普通 Feng 泛型约束 / witness 规则不被 runtime descriptor 反向改变。

验收口径：

- `@runtime extern fn foo<T>(value: T)` 可通过语义分析。
- `@runtime extern fn foo<T>(value: T[])` 继续通过语义分析。
- 非 runtime extern 中同类签名不会绕过 C ABI 规则。

### 6.4 Codegen

- [ ] 为具体类型实参生成 `FengRuntimeGenericParamDescriptor` 实参。
- [ ] 裸 `T` 参数按地址 carrier 发码。
- [ ] 裸 `T` 返回值按统一 out carrier 或其他已确认 ABI 发码。
- [ ] `T[]` 参数继续传递 `FengArray *` carrier，同时传入 `T` 的 runtime descriptor。
- [ ] 替换当前 generic extern `T[]` 专属 stable surface 判断，改为统一 descriptor-aware lowering。
- [ ] 保持现有 `feng_array_length_i64<T>`、`feng_array_slice<T>` 发码行为的用户可见语义不变。

验收口径：

- 生成 C 中能看到 runtime descriptor 隐藏实参。
- 当前数组 runtime helper 的既有测试继续通过。
- 裸 `T` runtime helper 能生成稳定 C 调用代码。

### 6.5 Runtime helper

- [ ] 新增 `feng_expression_equal` 或等价命名的泛型相等 helper。
- [ ] 按 descriptor 中的类型事实执行生产级相等比较。
- [ ] 对不具备已确认相等语义的类型直接失败或由语义层提前拒绝，不做不安全兜底。
- [ ] 补充必要的 runtime 单测。

验收口径：

- 浮点比较使用数值 `==`，不使用字节比较。
- `string` 使用内容比较。
- 身份语义类型使用身份比较。
- aggregate / spec 等复杂类型只按已确认规范执行。

### 6.6 标准库接入

- [ ] 在 `std/src/builtin/array.ff` 中通过 runtime helper 实现 `indexOf`。
- [ ] 更新 [docs/feng-std-array.md](../docs/feng-std-array.md) 中 `indexOf` 的标准库语义。
- [ ] 补充 std / smoke 用例，覆盖基础标量、浮点、`string`、数组或对象等代表性元素类型。

验收口径：

- 数组中可放入的元素类型，在规范允许相等比较的范围内都能被 `indexOf` 正确处理。
- `indexOf` 不依赖任意泛型 `T == T` 在 Feng 层通过。

### 6.7 测试与回归

- [ ] semantic：泛型 runtime extern 裸 `T`、`T[]`、显式类型实参、冲突推导、非 runtime extern 反例。
- [ ] codegen：descriptor 发码、裸 `T` carrier、`T[]` 迁移兼容。
- [ ] runtime：descriptor 分类、相等 helper 各类型分支。
- [ ] std / smoke：`indexOf` 端到端场景。
- [ ] 全量执行 `make test`。

验收口径：

- 新增测试覆盖 semantic、codegen、runtime、std / smoke 四层。
- 全量回归通过。

## 7. 当前明确不做

- [ ] 不把 `FengGenericParamDescriptor` 改造成 runtime API 泛型描述符。
- [ ] 不让 C runtime 直接解析或调用普通 Feng witness。
- [ ] 不把 `@runtime extern fn` 泛型与普通 Feng 泛型共享同一个 ABI。
- [ ] 不把 runtime API 泛型开放为稳定用户 C ABI。
- [ ] 不为 `T[]` 继续新增局部特判来规避通用 descriptor 设计。
- [ ] 不在本文实现或定义 `fit T[]` 的元素类型约束；该能力归属 [dev/feng-array-generics-pending.md](./feng-array-generics-pending.md)。

## 8. 建议执行顺序

1. 先确认 runtime descriptor ABI 与 hidden 参数顺序。
2. 再扩展 semantic，使 generic runtime extern 支持裸 `T` 与结构化推导。
3. 再实现 codegen descriptor 发码，并迁移现有 `T[]` helper。
4. 再实现 `feng_expression_equal` 作为首个消费者。
5. 最后接入 `std` 数组 `indexOf`，补齐文档和测试。

## 9. 交付约束

- 所有 runtime 泛型能力必须先在本文或后续权威规范中写清楚 ABI，再修改代码。
- 运行时性能敏感路径不得引入多余装箱、堆分配或运行时查表；如确需增加运行时开销，必须先由人工决策。
- 任何类型的相等语义不明确时，必须回到对应权威规范确认，不得在 runtime helper 中临时发明规则。
- 所有实现阶段都必须补测试并执行全量回归。
