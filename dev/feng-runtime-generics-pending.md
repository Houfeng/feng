# Feng Runtime API 泛型待开发项

> 本文档用于整理 `@runtime extern fn` 的泛型支持方向、ABI 边界、实现步骤与验收口径。
> [dev/feng-runtime-interop-pending.md](./feng-runtime-interop-pending.md) 是 `@runtime extern fn` 总体互操作方案；本文只补充 runtime API 泛型能力，不重复定义 `@runtime` 的目标注解、非公开定位与 contract 白名单规则。

## 1. 当前前提

- `@runtime extern fn` 与 Feng 层普通泛型处于不同语义层级：前者是编译器到 C runtime contract 的调用路径；后者是语言级泛型、约束与 witness 分发机制。
- 两者可以共用同一个类型实参描述符载体：`FengGenericParamDescriptor`。普通泛型共享体消费其中的值模型字段与 `witness`；runtime helper 消费其中的 runtime 类型分类字段。
- `FengGenericParamDescriptor.witness` 继续只服务非 runtime 泛型共享体：在二进制分发场景下，结构按具体类型单态，方法体可共享，共享方法体通过 descriptor 与 witness 操作已单态结构。
- runtime API 的实现方是 C 方法，不能直接消费 Feng witness。runtime helper 需要的是编译期已知、且 C runtime 可直接理解的类型分类信息；该信息由新增的 `FengRuntimeTypeKind` 字段承载。
- 当前 generic runtime extern 已支持 `T[]` 这类可降为稳定 C surface 的形态，例如 `feng_array_length_i64<T>(value: T[]): long` 和 `feng_array_slice<T>(value: T[], ...): T[]`。
- 当前 `T[]` 支持属于局部特化路径；后续目标是把它升级为统一的 descriptor-aware runtime 泛型调用机制，而不是继续为每种包裹形态增加特判。
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

Feng 源层仍用普通泛型声明语法表达类型参数；codegen 在调用 runtime C 符号时，为每个类型实参传入对应的 `FengGenericParamDescriptor`。该 descriptor 由普通泛型共享体与 runtime helper 共同使用，但双方读取的字段不同。

runtime API 泛型的 descriptor 传递模型必须与普通 Feng 泛型保持一致：按类型参数声明顺序平铺展开，每个类型参数传入一个 `const FengGenericParamDescriptor *`，不把多个类型参数打包成数组，也不把嵌套泛型类型展开成递归 descriptor 树。`type_kind` 只描述当前 descriptor 对应的类型实参本身；例如 `Box<int>` / `Box<T>` 作为类型实参时分类为对象，`T[]` / `Box<T>[]` 作为类型实参时分类为数组，首版不通过该 descriptor 递归描述对象的泛型实参或数组元素类型。

该 descriptor 的职责是把编译期已知类型事实编码为稳定 C ABI 数据，包括但不限于：

- 类型大小。
- 值模型分类（`FengValueKind`）。
- by-value aggregate 描述符。
- 内建标量 / enum / `string` / 数组 / 对象 / C 指针 / object-form spec / callable-form spec 等当前已有类型类别。
- 约束 witness（仅普通 Feng 泛型约束分发使用）。

首版不单独引入 runtime 专用泛型描述符；后续若某个 runtime contract 需要递归类型 payload、数组元素 descriptor、指针目标描述符、union member 表等更复杂信息，再先更新本文并由人工确认扩展方式。

### 2.2 与 Feng 层泛型的边界

- runtime API 泛型不得消费普通 Feng 泛型的 witness 语义。
- runtime API 泛型复用 `FengGenericParamDescriptor` 作为类型实参描述符载体，但只消费 runtime 类型分类和值模型字段，不读取 `witness`。
- 普通 Feng 泛型仍负责语言层类型检查、约束满足、witness 生成、共享方法体分发与二进制分发模型。
- runtime API 泛型只负责 C runtime contract 如何接收并消费泛型类型实参信息。
- 两者在语法上都使用 `<T>`，在 ABI 上共用同一个 descriptor 载体，但语义职责通过字段消费边界分开：普通泛型消费 `size` / `kind` / `aggregate` / `witness`，runtime helper 消费 `type_kind` 以及必要的值模型字段。

### 2.3 与当前 `T[]` 特判的关系

当前 `T[]` generic extern 支持应被视为 runtime API 泛型的一种普通应用场景：

- `T` 的类型信息由 `FengGenericParamDescriptor` 传入，其中 `type_kind` 供 runtime helper 做语义分派。
- 数组值本身继续按现有 runtime 数组 carrier 传递。
- `T[]` 不再要求 codegen 为 wrapped shape 维护专属推导和专属 extern surface 判断。
- 原有 `feng_array_length_i64<T>`、`feng_array_slice<T>` 行为必须保持兼容，并作为迁移后的回归基线。

## 3. 泛型参数描述符扩展

### 3.1 新增 runtime 类型分类

新增 runtime 可识别的类型分类枚举，例如：

```c
// 实现时，需要按下的结构，且下方枚举值的注释也要保留
typedef enum FengRuntimeTypeKind {
    FENG_RUNTIME_TYPE_BOOL = 1,
    FENG_RUNTIME_TYPE_I8 = 2,
    FENG_RUNTIME_TYPE_I16 = 3,
    FENG_RUNTIME_TYPE_I32 = 4,
    FENG_RUNTIME_TYPE_I64 = 5,
    FENG_RUNTIME_TYPE_U8 = 6,
    FENG_RUNTIME_TYPE_U16 = 7,
    FENG_RUNTIME_TYPE_U32 = 8,
    FENG_RUNTIME_TYPE_U64 = 9,
    FENG_RUNTIME_TYPE_F32 = 10,
    FENG_RUNTIME_TYPE_F64 = 11,
    FENG_RUNTIME_TYPE_ENUM = 12,       /* user enum lowered as its integer representation */
    FENG_RUNTIME_TYPE_STRING = 13,
    FENG_RUNTIME_TYPE_ARRAY = 14,
    FENG_RUNTIME_TYPE_OBJECT = 15,     /* concrete user type value, represented as a managed object reference */
    FENG_RUNTIME_TYPE_POINTER = 16,    /* C interop pointer type written as T* */
    FENG_RUNTIME_TYPE_SPEC = 17,       /* object-form spec fat value */
    FENG_RUNTIME_TYPE_CALLABLE = 18    /* callable-form spec value, including lambdas and bound method values */
} FengRuntimeTypeKind;
```

首版枚举只包含当前语言和实现已经存在的类型分类，不预留尚未引入的未来类型成员；后续新增语言类型时，必须先更新本文再扩展 `FengRuntimeTypeKind`。
枚举值必须全部显式指定，避免混用显式值与隐式递增值；新增成员时只能追加新值，不得复用或重排已有值。

`FengRuntimeTypeKind` 不是 `FengValueKind` 的重复：

- `FengValueKind` 回答值生命周期和复制策略：trivial / managed pointer / aggregate。
- `FengRuntimeTypeKind` 回答 runtime helper 的语义分派：整数、浮点、enum、字符串、数组、对象、C 指针、object-form spec、callable-form spec 等当前已有类型。

### 3.2 扩展 `FengGenericParamDescriptor`

首版不新增独立的 runtime 泛型描述符，而是在现有 `FengGenericParamDescriptor` 中增加 `type_kind`。字段顺序固定为 `size` / `kind` / `type_kind` / `aggregate` / `witness`，其中 `type_kind` 必须紧跟 `kind`，位于 `aggregate` 之前：

```c
typedef struct FengGenericParamDescriptor {
    size_t size;
    FengValueKind kind;
    FengRuntimeTypeKind type_kind;
    const FengAggregateValueDescriptor *aggregate;
    const void *witness;
} FengGenericParamDescriptor;
```

字段消费边界：

- 普通无约束泛型共享体消费 `size` / `kind` / `aggregate`。
- 普通受约束泛型共享体在此基础上消费 `witness`。
- runtime helper 消费 `type_kind`，必要时消费 `size` / `kind` / `aggregate`，但不得消费 `witness`。
- codegen 在具体实例化点填充全部字段，因为只有 codegen 同时知道具体类型、值模型、runtime 类型分类和当前约束面的 witness。
- codegen 生成 `FengGenericParamDescriptor` compound literal 时应使用 designated initializer，不能依赖字段位置；其中 `.type_kind` 必须紧随 `.kind` 填充。

### 3.3 不把 witness 暴露给 runtime

runtime helper 不把普通 Feng witness 作为操作入口：

- C runtime 不知道普通 witness 表对应哪个语言约束。
- C runtime 不知道 witness slot 布局与参数 / 返回 ABI。
- C runtime 不应通过普通 witness 实现 runtime helper 的核心语义。

若某个 runtime helper 后续确实需要比 `type_kind` 更多的信息，应显式设计 C ABI 可消费的信息，并先更新本文，例如：

- runtime type tag + runtime 内部分派。
- 由 codegen 生成并符合稳定 C ABI 的 helper thunk。
- contract 专属 operation table。

这些都属于 runtime contract 设计，不等同于普通 Feng 泛型 witness。

## 4. Lowering 方向

### 4.1 类型参数传递

对 `@runtime extern fn` 的每个类型参数，codegen 传入对应的 `FengGenericParamDescriptor` 实参。建议按类型参数声明顺序传递隐藏 descriptor 参数，并在实现前固定其在 C 符号参数列表中的位置。

该隐藏 descriptor 的传递顺序和传递方式必须与普通泛型向共享体传递 descriptor 的规则保持一致。

示例 Feng 声明：

```feng
@runtime
extern fn feng_expression_equal<T>(left: T, right: T): bool;
```

对应 C contract 可收敛为类似形态：

```c
bool feng_expression_equal(
    const FengGenericParamDescriptor *T,
    const void *left,
    const void *right
);
```

其中裸 `T` 参数使用地址形式传递，避免 C ABI 需要按未知大小直接传值。具体地址物化规则需与普通 Feng 调用的所有权、生命周期和临时值规则对齐。

裸 `T` 返回值也必须避免按未知大小直接走 C 返回寄存器。当前收敛规则是：

- Feng 声明仍保持 `extern fn foo<T>(...): T;`
- 对应 C contract 改为 `void foo(const FengGenericParamDescriptor *T, ..., void *out);`
- 调用点按普通泛型共享体的 direct-`T` return 规则，在本地分配 concrete `T` storage，并把 `&storage` 作为隐藏 out carrier 追加到 runtime contract 实参末尾。
- runtime helper 写入 `out` 时，managed pointer 与 aggregate 必须补 retain，保证返回值所有权语义与普通 Feng 返回一致。

示例 Feng 声明：

```feng
@runtime
extern fn foo<T>(value: T): T;
```

对应 C contract 可收敛为类似形态：

```c
void foo(
    const FengGenericParamDescriptor *T,
    const void *value,
    void *out
);
```

`@runtime extern` 返回裸 `T` 的剩余落地步骤必须单独完成，不能因为普通泛型共享体已经支持 direct-`T` return 就默认 runtime contract 自动继承：

1. 在 semantic 层接受 `@runtime extern fn foo<T>(...): T;` 这一表面形态，但不得依赖返回位单独反向推导类型实参。
2. 在 extern 注册阶段，只对 `uses_runtime_contract` 的 generic extern 放宽 bare-`T` return 的 stable surface 校验；普通 extern 继续沿用现有 C ABI 规则。
3. 在 generic runtime extern 调用 lowering 阶段，识别原始返回位是裸 `T`，为具体实例化后的 `T` 分配本地 storage，并把 `&storage` 作为隐藏 out carrier 追加到 runtime contract 实参末尾。
4. 调用结果表达式必须改为该本地 storage，并复用普通泛型 direct-`T` return 的 ownership / cleanup 规则，确保 managed pointer 与 aggregate 的 retain 语义一致。
5. 只有当某个正式长期保留的 runtime contract API 真实需要 `: T` 时，才为它声明 `void foo(..., void *out)` 形态；不得为了测试链路而在正式文档中新增临时 API。
6. 回归测试至少覆盖 semantic、codegen 和全量回归；若未来存在正式 public contract API，再追加 runtime 端到端测试。

### 4.2 包裹泛型类型

对 `T[]`、`Box<T>` 等包含类型参数的参数或返回类型，lowering 不应再依赖局部特判：

- 参数 carrier 按该 Feng 类型的自然 runtime 表示传递。
- 裸 `T` 返回值继续走 4.1 中的统一 out carrier；其余包含类型参数的返回值按该 Feng 类型的自然 runtime 表示返回。
- 类型参数事实通过隐藏 `FengGenericParamDescriptor` 传递。
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

`feng_expression_equal<T>` 可作为 runtime API 泛型的首个高价值消费者，用于标准库数组 `indexOf` 等需要对元素执行相等比较的场景。由于数组 `fit T[!]` 的 `indexOf` 本身会进入普通泛型共享体，它天然已经拥有 `_T: FengGenericParamDescriptor`，因此首版 helper 应直接接收该 descriptor，而不是要求共享体再构造第二套 runtime 描述符。

该 helper 的设计约束：

- 不改变普通 `==` 运算符的 analyzer / codegen 规则。
- 由标准库显式调用 helper，而不是让任意泛型 `T == T` 自动通过。
- 相等语义必须与语言规范中的表达式 / spec 相等语义对齐；本文不重新定义 equality 主语义。
- 不得使用 `memcmp` 作为浮点、aggregate、object-form spec 等类型的语义兜底。
- 标量、enum、`string`、数组、C 指针、对象、object-form spec、callable-form spec 等当前可进入数组的元素类型，都必须有明确且生产级的处理策略。
- helper 可以读取 `FengGenericParamDescriptor.type_kind` 做语义分派，可以读取 `size` / `kind` / `aggregate` 完成必要的值访问判断，但不得读取 `witness`。

如果某一类类型当前缺少规范化相等语义，必须先回到对应权威规范确定语义，再实现 helper，不得在 runtime 中临时补特殊规则。

## 6. 分步 TODO

### 6.1 规范与 ABI 收敛

- [x] 在本文确认 runtime API 泛型与普通 Feng 泛型分层独立。
- [x] 明确 `FengRuntimeTypeKind` 的枚举成员、稳定性边界、不得预留未来类型成员，以及与 `FengValueKind` 的职责区分。
- [x] 明确 `FengGenericParamDescriptor.type_kind` 固定紧跟 `kind`，并同步生命周期和 ABI 兼容策略。
- [x] 明确裸 `T` 参数、裸 `T` 返回值、`T[]` 参数、`T[]` 返回值的 C contract carrier，以及隐藏 `FengGenericParamDescriptor` 参数顺序。
- [x] 明确 runtime 泛型 descriptor 传递与普通泛型一致：多类型参数平铺、按声明顺序传递，嵌套泛型首版不展开为递归 descriptor payload。
- [x] 明确 descriptor 与 `src/runtime/feng_runtime_contract.inc` 的关系：contract 白名单仍是唯一允许符号来源，`FengGenericParamDescriptor` 是泛型 contract 符号的隐藏 ABI 参数。
- [ ] 更新 [dev/feng-runtime-interop-pending.md](./feng-runtime-interop-pending.md) 中的 runtime lowering 章节，只做引用，不重复展开本文细节。

验收口径：

- 文档中不再引入首版独立 runtime API 泛型描述符。
- 文档中清楚区分 runtime helper 消费 `type_kind` 与普通泛型约束分发消费 `witness`。
- `T[]` 被描述为通用 runtime 泛型机制的实例，而不是独立特判。
- runtime 泛型与普通泛型共享同一套 descriptor 展开顺序；文档中不得引入平行的 descriptor 数组或嵌套 descriptor 树。

### 6.2 Runtime header / contract

- [x] 在 runtime public ABI 中新增 `FengRuntimeTypeKind`。
- [x] 扩展 `FengGenericParamDescriptor`，增加 `type_kind` 字段。
- [x] 为 contract helper 增加接收 `const FengGenericParamDescriptor *` 的 C 原型。
- [x] 将 `feng_array_length_i64<T>` 与 `feng_array_slice<T>` 纳入 descriptor-aware runtime 泛型 contract；若 helper 语义不需要读取元素类型 descriptor，也必须在 contract 中明确 descriptor 是隐藏 ABI 参数且可不消费。
- [x] 保持 `src/runtime/feng_runtime_contract.inc` 作为允许 `@runtime` 声明使用的唯一符号清单。
- [x] 现有 X-macro 片段已可直接表达 hidden `FengGenericParamDescriptor` 参数，无需额外 contract 条目扩展。

验收口径：

- generated C 只引用 runtime public ABI 中声明的符号和类型。
- runtime contract 白名单仍可被 codegen 精确检查。

### 6.3 Semantic

- [x] 允许 `@runtime extern fn` 在参数位使用泛型参数的裸形态和包裹形态。
- [ ] 允许 `@runtime extern fn` 在返回位使用裸 `T`，并接通对应调用链路。
- [x] 把 runtime generic extern 的推导从 `T[]` wrapped shape 扩展为结构化泛型匹配。
- [x] 对冲突推导、无法推导、显式类型实参数量不匹配给出稳定诊断。
- [x] 确保普通 C ABI extern 不获得 runtime 泛型特权。
- [x] 确保普通 Feng 泛型约束 / witness 规则不被 `type_kind` 反向改变。

验收口径：

- `@runtime extern fn foo<T>(value: T)` 可通过语义分析。
- `@runtime extern fn foo<T>(value: T): T` 的返回位支持仍待交付。
- `@runtime extern fn foo<T>(value: T[])` 继续通过语义分析。
- 非 runtime extern 中同类签名不会绕过 C ABI 规则。

### 6.4 Codegen

- [x] 为具体类型实参生成带 `type_kind` 的 `FengGenericParamDescriptor` 实参。
- [x] 裸 `T` 参数按地址 carrier 发码。
- [ ] 裸 `T` 返回值按统一 out carrier 或其他已确认 ABI 发码。
- [x] `T[]` 参数继续传递 `FengArray *` carrier，同时传入元素 `T` 的 `FengGenericParamDescriptor`。
- [x] 将 `feng_array_length_i64<T>(value: T[])` 与 `feng_array_slice<T>(value: T[], ...)` 从当前 `T[]` generic extern 专属路径迁移到统一 descriptor-aware lowering，作为首批兼容迁移对象。
- [ ] 在 `feng_array_length_i64<T>` 与 `feng_array_slice<T>` 迁移完成并通过回归后，移除旧的 `T[]` generic extern 专属特判处理。
- [ ] 替换当前 generic extern `T[]` 专属 stable surface 判断，改为统一 `FengGenericParamDescriptor`-aware lowering。
- [x] 保持现有 `feng_array_length_i64<T>`、`feng_array_slice<T>` 发码行为的用户可见语义不变。

其中，`@runtime extern` 返回裸 `T` 的 codegen 剩余步骤如下：

1. 在 extern 注册阶段，仅对 runtime contract generic extern 放宽 bare-`T` return 的 stable surface 校验。
2. 在 generic runtime extern 调用阶段，识别原始返回位是裸 `T`，并为 concrete `T` 分配本地结果 storage。
3. 将 `&storage` 作为隐藏 out carrier 追加到 runtime contract 实参末尾，而不是继续假设 C direct return。
4. 调用结束后，把该 local storage 作为表达式结果继续流转，并复用普通泛型 direct-`T` return 的 managed / aggregate cleanup 规则。
5. 若未来某个正式 runtime contract API 采用 bare-`T` return，contract 文档应直接声明 `void foo(..., void *out)` 形态；不得为了测试链路而新增临时 public API。

验收口径：

- 生成 C 中能看到 `FengGenericParamDescriptor` 隐藏实参。
- 当前数组 runtime helper 的既有测试继续通过。
- 裸 `T` runtime helper 能生成稳定 C 调用代码。

### 6.5 Runtime helper

- [x] 新增 `feng_expression_equal` 或等价命名的泛型相等 helper。
- [x] 按 `FengGenericParamDescriptor.type_kind` 与值模型字段执行生产级相等比较。
- [ ] 对不具备已确认相等语义的类型直接失败或由语义层提前拒绝，不做不安全兜底。
- [x] 补充必要的 runtime 单测。

验收口径：

- 浮点比较使用数值 `==`，不使用字节比较。
- `string` 使用内容比较。
- 身份语义类型使用身份比较。
- enum 使用规范化底层整数比较，不用通用 `memcmp` 兜底。
- aggregate / spec 等复杂类型只按已确认规范执行；object-form spec 若按 subject + witness 比较，必须先有 runtime 可稳定读取的 fat value 布局。

### 6.6 标准库接入

- [x] 在 `std/src/builtin/array.ff` 中通过 runtime helper 实现 `indexOf`。
- [x] 更新 [docs/feng-std-array.md](../docs/feng-std-array.md) 中 `indexOf` 的标准库语义。
- [ ] 补充 std / smoke 用例，覆盖基础标量、浮点、`string`、数组或对象等代表性元素类型。

验收口径：

- 数组中可放入的元素类型，在规范允许相等比较的范围内都能被 `indexOf` 正确处理。
- `indexOf` 不依赖任意泛型 `T == T` 在 Feng 层通过。

### 6.7 测试与回归

- [x] semantic：泛型 runtime extern 裸 `T` 参数、`T[]`、显式类型实参、冲突推导、非 runtime extern 反例。
- [ ] semantic：`@runtime extern` 返回裸 `T` 的声明 / 调用回归。
- [x] codegen：`FengGenericParamDescriptor.type_kind` 发码、裸 `T` carrier、`feng_array_length_i64<T>` / `feng_array_slice<T>` 的 `T[]` 迁移兼容。
- [ ] codegen：`@runtime extern` 返回裸 `T` 的 out carrier 发码回归。
- [x] runtime：descriptor 分类、相等 helper 各类型分支。
- [x] std：`indexOf` 端到端场景。
- [ ] smoke：`indexOf` 端到端场景。
- [ ] 全量执行 `make test`。

验收口径：

- 新增测试覆盖 semantic、codegen、runtime、std / smoke 四层。
- 全量回归通过。

## 7. 当前明确不做

- [ ] 不在首版引入独立的 `FengRuntimeGenericParamDescriptor`。
- [ ] 不让 C runtime 直接解析或调用普通 Feng witness。
- [ ] 不让 runtime helper 消费 `FengGenericParamDescriptor.witness`。
- [ ] 不把 runtime API 泛型开放为稳定用户 C ABI。
- [ ] 不为 `T[]` 继续新增局部特判来规避通用 descriptor 设计。
- [ ] 不在本文实现或定义 `fit T[]` 的元素类型约束；该能力归属 [dev/feng-array-generics-pending.md](./feng-array-generics-pending.md)。

## 8. 建议执行顺序

1. 先确认 `FengRuntimeTypeKind` 枚举与 `FengGenericParamDescriptor.type_kind` ABI。
2. 再扩展 semantic，使 generic runtime extern 支持裸 `T` 与结构化推导。
3. 再实现 codegen 对 `type_kind` 的填充，并迁移现有 `T[]` helper 到统一 descriptor-aware lowering。
4. 再实现 `feng_expression_equal` 作为首个消费者。
5. 最后接入 `std` 数组 `indexOf`，补齐文档和测试。

## 9. 交付约束

- 所有 runtime 泛型能力必须先在本文或后续权威规范中写清楚 ABI，再修改代码。
- 运行时性能敏感路径不得引入多余装箱、堆分配或运行时查表；如确需增加运行时开销，必须先由人工决策。
- 任何类型的相等语义不明确时，必须回到对应权威规范确认，不得在 runtime helper 中临时发明规则。
- `type_kind` 只用于 runtime helper 语义分派，不得替代普通泛型约束 witness。
- 所有实现阶段都必须补测试并执行全量回归。
