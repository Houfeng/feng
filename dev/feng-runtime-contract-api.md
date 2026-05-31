# Feng Runtime Contract API

## 1. 范围与权威来源

- 本文只说明当前允许通过 `@runtime extern func` 声明使用的 runtime contract API，以及每个 API 的用途。
- 允许列表的唯一权威来源是 `src/runtime/feng_runtime_contract.inc`；本文是解释性索引，不是第二份白名单定义。
- 若 future 变更新增、删除或重命名 runtime contract API，必须先修改 `src/runtime/feng_runtime_contract.inc`，再同步更新本文。

当前 contract API 共 5 个：

1. `feng_string_utf8_length`
2. `feng_string_from_utf8_bytes`
3. `feng_array_length_i64`
4. `feng_array_slice`
5. `feng_expression_equal`

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
- Feng 声明形态：`extern func feng_array_slice<T>(value: T[], start: long, length: long): T[];`
- 用途：复制数组的一个右开区间子段 `[start, start + length)`，并返回一个新的 `FengArray`。适用于调用方需要“独立数组值”而不是共享视图的场景。
- ABI 说明：generated C 会把元素类型 `T` 的 `FengGenericParamDescriptor` 作为隐藏参数放在最前面传入；当前 helper 沿用现有数组 carrier，不额外暴露第二套数组类型元数据。
- 语义说明：
  - `start`、`length` 必须非负，且满足 `start + length <= value.length()`。
  - 返回值是新数组，不与源数组共享 payload 存储。
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

## 3. 维护规则

- 修改 runtime contract API 列表时，先改 `src/runtime/feng_runtime_contract.inc`。
- 若 API 需要新的 helper 实现，落在 `src/runtime/feng_runtime_contract.c` 或对应 runtime 实现文件中。
- 然后同步更新本文、标准库 `@runtime` 声明以及相关回归测试。
