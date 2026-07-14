# Feng Runtime Contract API

## 1. 范围与权威来源

- 本文只说明当前允许通过 `@runtime extern func` 声明使用的 runtime contract API，以及每个 API 的用途。
- 允许列表的唯一权威来源是 `src/runtime/feng_runtime_contract.inc`；本文是解释性索引，不是第二份白名单定义。
- 若 future 变更新增、删除或重命名 runtime contract API，必须先修改 `src/runtime/feng_runtime_contract.inc`，再同步更新本文。

本文现有解释条目包括：

1. `feng_string_utf8_length`
2. `feng_string_from_utf8_bytes`
3. `feng_array_length_i64`
4. `feng_array_slice`
5. `feng_expression_equal`
6. `feng_pointer_is_null`
7. `feng_pointer_equal`
8. `feng_pointer_get_scalar`

本轮新增以下 array storage contract API，完整语义与实施边界统一由
`dev/feng-array-storage-dev.md` 定义，本文不重复维护算法细节：

1. `feng_array_storage_get_capacity`
2. `feng_array_storage_insert`
3. `feng_array_storage_remove`
4. `feng_array_storage_migrate`

以上 API 已加入 `src/runtime/feng_runtime_contract.inc`，可以通过
`@runtime extern func` 声明使用。

## 2. API 清单

### 2.1 `feng_string_utf8_length`

- C 符号：`int64_t feng_string_utf8_length(FengString *value)`
- Feng 声明形态：`extern func feng_string_utf8_length(value: string): long;`
- 用途：读取 `string` 的逻辑长度，并把 runtime 内部的 `size_t` 长度转换成 Feng 侧稳定使用的 `long`（`i64`）。
- 语义说明：返回的是 UTF-8 字节长度，不是 Unicode code point 数量，也不是显示宽度。
- 主要使用点：标准库 `std/src/text/String.ff` 用它实现 `string.length()`。
- 边界说明：实现对 `NULL` 做了容错并返回 `0`，但 contract 的正常使用语义仍然是“对一个合法 `string` 值取长度”。若内部长度无法装入 `int64_t`，runtime 直接失败。

### 2.2 `feng_string_from_utf8_bytes`

- C 符号：`FengString *feng_string_from_utf8_bytes(FengArray *value, int64_t length)`
- Feng 声明形态：`extern func feng_string_from_utf8_bytes(value: byte[], length: long): string;`
- 用途：把一个字节数组的前 `length` 个字节复制成新的 `string`，用于“字节输入 -> Feng 字符串值”的 runtime bridge。
- 语义说明：
  - `length` 必须满足 `0 <= length <= value.length()`。
  - 返回值是新的 Feng `string` 值；当 `length == 0` 时，返回共享空字符串单例。
  - 当前实现只做按字节复制，不在这里额外执行 UTF-8 合法性校验。
- 主要使用点：标准库 `std/src/text/String.ff` 用它实现 `string.fromUtf8Bytes(bytes)`；I/O 模块通过该公开静态方法把输入或格式化后的字节数组转换为 `string`。
- 边界说明：若 `length` 为负或超过数组长度，runtime 直接失败。

### 2.3 `feng_array_length_i64`

- C 符号：`int64_t feng_array_length_i64(const FengGenericParamDescriptor *type, const FengArray *value)`
- Feng 声明形态：`extern func feng_array_length_i64<T>(value: T[]): long;`
- 用途：读取数组当前层的元素个数，并把 runtime 内部的 `size_t` 长度转换成 Feng 侧 `long`（`i64`）。
- ABI 说明：Feng 层的 `<T>` 不直接出现在显式声明中；generated C 会在调用点把 `T` 对应的 `FengGenericParamDescriptor` 作为隐藏参数放在最前面传给 runtime contract。
- 语义说明：返回的是“元素个数”，不是字节数，也不是递归展开后的总元素数。
- 主要使用点：标准库 `std/src/collections/Array.ff` 用它实现数组 `length()`。
- 边界说明：实现对 `NULL` 做了容错并返回 `0`，但 contract 的正常使用语义仍然是“对一个合法数组值取长度”。若数组长度无法装入 `int64_t`，runtime 直接失败。

### 2.4 `feng_array_slice`

- C 符号：`FengArray *feng_array_slice(const FengGenericParamDescriptor *type, const FengArray *value, int64_t start, int64_t length)`
- Feng 声明形态：`extern func feng_array_slice<T>(value: T[], start: long, length: long): T[!];`
- 用途：复制数组的一个右开区间子段 `[start, start + length)`，并返回一个新的 `FengArray`。适用于调用方需要“独立数组值”而不是共享视图的场景。
- ABI 说明：generated C 会把元素类型 `T` 的 `FengGenericParamDescriptor` 作为隐藏参数放在最前面传入；当前 helper 沿用现有数组 carrier，不额外暴露第二套数组类型元数据。
- 语义说明：
  - `start`、`length` 必须非负，且满足 `start + length <= value.length()`。
  - 返回值是新数组，不与源数组共享 payload 存储。
  - runtime 返回的新数组本身可写；标准库只读数组 `clone` 在语言层将其显式降级为 `T[]`，可写数组 `clone` 则保持 `T[!]`。
  - 对元素的复制策略由源数组的元素类别决定：
    - trivial 元素按字节复制；
    - managed pointer 元素逐槽 retain；
    - aggregate 元素逐元素走 `feng_aggregate_assign`。
  - 因而返回数组对托管子元素拥有独立持有权。
- 当前状态：已进入 runtime contract 白名单，并作为标准库数组复制能力的底层 helper。
- 边界说明：这是一条“复制型数组 helper”，不等同于语言层 `std.collections` 的 `slice(start, end) -> Span<T>` 只读视图语义；后者共享底层数组，这里返回的是新数组。

### 2.5 `feng_expression_equal`

- C 符号：`bool feng_expression_equal(const FengGenericParamDescriptor *type, const void *left, const void *right)`
- Feng 声明形态：`extern func feng_expression_equal<T>(left: T, right: T): bool;`
- 用途：比较两个 `T` 值的运行时相等性，用于标准库数组 `indexOf` 等显式 helper 场景。
- ABI 说明：generated C 会把 `T` 的 `FengGenericParamDescriptor` 作为隐藏首参传入；裸 `T` 参数按地址 carrier 传给 runtime contract，因此 helper 不再接收旧的 `T[] + index` 过渡形态。
- 语义说明：helper 按 `type_kind` 与当前值模型分派，实现基础数值 / `bool` / `enum` / `string` 的值语义，以及数组 / 对象 / 指针 / `spec` / callable 的引用身份语义。
- 主要使用点：标准库 `std/src/collections/Array.ff` 用它实现 `T[!].indexOf(value)`。
- 边界说明：这是标准库显式调用的 runtime helper，不改变普通 `==` 运算符的 analyzer / codegen 规则。

### 2.6 `feng_pointer_is_null`

- C 符号：`bool feng_pointer_is_null(void *ptr)`
- Feng 声明形态：`extern func feng_pointer_is_null(ptr: T*): bool;`（其中 `T` 为任意 `@abi seal` 类型）
- 用途：检查一个 C 侧不透明指针是否为 NULL。Feng 没有空指针字面量，用户代码通过此函数检测分配失败或 C API 返回的 NULL。
- 语义说明：当 `ptr == NULL` 时返回 `true`，否则返回 `false`。
- 主要使用点：标准库 `std/src/process/Process.ff` 用它检测 `popen` 返回的文件流指针是否为 NULL。
- 边界说明：仅适用于不透明 C 指针类型，不适用于 Feng 托管对象指针。

### 2.7 `feng_pointer_equal`

- C 符号：`bool feng_pointer_equal(void *left, void *right)`
- Feng 声明形态：`extern func feng_pointer_equal(left: T*, right: T*): bool;`（其中 `T` 为任意 `@abi seal` 类型）
- 用途：比较两个 C 侧不透明指针是否相等（地址相等性）。
- 语义说明：当 `left == right`（指针地址相同）时返回 `true`，否则返回 `false`。
- 主要使用点：用户代码需要比较两个 C 侧指针是否指向同一对象时使用。
- 边界说明：仅做地址比较，不比较指向的内容。仅适用于不透明 C 指针类型，不适用于 Feng 托管对象指针。

### 2.8 `feng_pointer_get_scalar`

- C 符号：`void feng_pointer_get_scalar(const FengGenericParamDescriptor *type, void *ptr, void *result)`
- Feng 声明形态：`extern func feng_pointer_get_scalar<T>(ptr: T*): T;`（其中 `T` 为 ABI 兼容标量类型）
- 用途：从 C 侧指针 `ptr` 处读取一个标量值。与 `feng_pointer_move` 组合可实现任意偏移处的标量读取，形成完整的不透明指针读取能力。
- ABI 说明：generated C 把 `T` 对应的 `FengGenericParamDescriptor` 作为隐藏首参传入，用户参数 `ptr` 居中，返回值通过末位 `void *result` out 参数传出，遵循泛型 bare-T return 的标准降低约定（描述符... → 用户参数... → &result_carrier）。
- 语义说明：
  - `T` 仅限 ABI 兼容标量类型（`int`、`long`、`byte`、`u32`、`float`、`double`、`bool`、`enum`）。
  - 非标量类型（`string`、`T[]`、type 实例等）runtime panic。
  - NULL 指针 runtime panic。
  - 读取大小由 `FengTrivialDescriptor.size` 决定，执行 `memcpy` 语义。
- 主要使用点：标准库 `std/src/text/RegExp.ff` 用它读取 PCRE2 ovector（`size_t*` 数组）中的匹配偏移量。
- 边界说明：指针偏移通过已有 `feng_pointer_move` 完成，`feng_pointer_get_scalar` 仅负责读取，职责单一。

### 2.9 `feng_array_storage_get_capacity`

- C 符号：`intptr_t feng_array_storage_get_capacity(const FengGenericParamDescriptor *type, const FengArray *array)`
- Feng 声明形态：`extern func feng_array_storage_get_capacity<T>(array: T[!]): int;`
- 用途：读取容器 backing array 的固定容量；完整语义见 `dev/feng-array-storage-dev.md`。

### 2.10 `feng_array_storage_insert`

- C 符号：`void feng_array_storage_insert(const FengGenericParamDescriptor *type, FengArray *array, intptr_t index, const void *value)`
- Feng 声明形态：`extern func feng_array_storage_insert<T>(array: T[!], index: int, value: T): void;`
- 用途：在已有容量内插入单个元素；完整语义与生命周期规则见 `dev/feng-array-storage-dev.md`。

### 2.11 `feng_array_storage_remove`

- C 符号：`void feng_array_storage_remove(const FengGenericParamDescriptor *type, FengArray *array, intptr_t index, intptr_t count)`
- Feng 声明形态：`extern func feng_array_storage_remove<T>(array: T[!], index: int, count: int): void;`
- 用途：删除 backing array 的指定元素区间；完整语义与生命周期规则见 `dev/feng-array-storage-dev.md`。

### 2.12 `feng_array_storage_migrate`

- C 符号：`FengArray *feng_array_storage_migrate(const FengGenericParamDescriptor *type, FengArray *array, intptr_t new_capacity)`
- Feng 声明形态：`extern func feng_array_storage_migrate<T>(array: T[!], newCapacity: int): T[!];`
- 用途：创建固定容量的新 backing array 并迁移有效元素；完整语义与生命周期规则见 `dev/feng-array-storage-dev.md`。

## 3. 维护规则

- 修改 runtime contract API 列表时，先改 `src/runtime/feng_runtime_contract.inc`。
- 若 API 需要新的 helper 实现，落在 `src/runtime/feng_runtime_contract.c` 或对应 runtime 实现文件中。
- 然后同步更新本文、标准库 `@runtime` 声明以及相关回归测试。
