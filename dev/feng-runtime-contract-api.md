# Feng Runtime Contract API

## 1. 范围与权威来源

- 本文只说明当前允许通过 `@runtime extern fn` 声明使用的 runtime contract API，以及每个 API 的用途。
- 允许列表的唯一权威来源是 `src/runtime/feng_runtime_contract.inc`；本文是解释性索引，不是第二份白名单定义。
- 若 future 变更新增、删除或重命名 runtime contract API，必须先修改 `src/runtime/feng_runtime_contract.inc`，再同步更新本文。

当前 contract API 共 4 个：

1. `feng_string_utf8_length`
2. `feng_string_from_utf8_bytes`
3. `feng_array_length_i64`
4. `feng_array_slice`

## 2. API 清单

### 2.1 `feng_string_utf8_length`

- C 符号：`int64_t feng_string_utf8_length(FengString *value)`
- Feng 声明形态：`extern fn feng_string_utf8_length(value: string): long;`
- 用途：读取 `string` 的逻辑长度，并把 runtime 内部的 `size_t` 长度转换成 Feng 侧稳定使用的 `long`（`i64`）。
- 语义说明：返回的是 UTF-8 字节长度，不是 Unicode code point 数量，也不是显示宽度。
- 主要使用点：标准库 `std/src/builtin/string.ff` 用它实现 `string.length()`。
- 边界说明：实现对 `NULL` 做了容错并返回 `0`，但 contract 的正常使用语义仍然是“对一个合法 `string` 值取长度”。若内部长度无法装入 `int64_t`，runtime 直接失败。

### 2.2 `feng_string_from_utf8_bytes`

- C 符号：`FengString *feng_string_from_utf8_bytes(FengArray *value, int64_t length)`
- Feng 声明形态：`extern fn feng_string_from_utf8_bytes(value: byte[], length: long): string;`
- 用途：把一个字节数组的前 `length` 个字节复制成新的 `string`，用于“字节输入 -> Feng 字符串值”的 runtime bridge。
- 语义说明：
  - `length` 必须满足 `0 <= length <= value.length()`。
  - 返回值是新的 Feng `string` 值；当 `length == 0` 时，返回共享空字符串单例。
  - 当前实现只做按字节复制，不在这里额外执行 UTF-8 合法性校验。
- 主要使用点：标准库 `std/src/io/input.ff` 用它把输入字节数组转换为 `string`。
- 边界说明：若 `length` 为负或超过数组长度，runtime 直接失败。

### 2.3 `feng_array_length_i64`

- C 符号：`int64_t feng_array_length_i64(const FengArray *value)`
- Feng 声明形态：`extern fn feng_array_length_i64<T>(value: T[]): long;`
- 用途：读取数组当前层的元素个数，并把 runtime 内部的 `size_t` 长度转换成 Feng 侧 `long`（`i64`）。
- 语义说明：返回的是“元素个数”，不是字节数，也不是递归展开后的总元素数。
- 主要使用点：标准库 `std/src/builtin/array.ff` 用它实现数组 `length()`。
- 边界说明：实现对 `NULL` 做了容错并返回 `0`，但 contract 的正常使用语义仍然是“对一个合法数组值取长度”。若数组长度无法装入 `int64_t`，runtime 直接失败。

### 2.4 `feng_array_slice`

- C 符号：`FengArray *feng_array_slice(const FengArray *value, int64_t start, int64_t end)`
- Feng 声明形态：`extern fn feng_array_slice<T>(value: T[], start: long, end: long): T[];`
- 用途：复制数组的一个右开区间子段 `[start, end)`，并返回一个新的 `FengArray`。适用于调用方需要“独立数组值”而不是共享视图的场景。
- 语义说明：
  - `start`、`end` 必须非负，且满足 `start <= end <= value.length()`。
  - 返回值是新数组，不与源数组共享 payload 存储。
  - 对元素的复制策略由源数组的元素类别决定：
    - trivial 元素按字节复制；
    - managed pointer 元素逐槽 retain；
    - aggregate 元素逐元素走 `feng_aggregate_assign`。
  - 因而返回数组对托管子元素拥有独立持有权。
- 当前状态：已进入 runtime contract 白名单，但标准库中尚未发现现有使用点。
- 边界说明：这是一条“复制型数组 helper”，不等同于语言层 `std.collections` 的 `slice(start, length) -> Span<T>` 只读视图语义；后者共享底层数组，这里返回的是新数组。

## 3. 维护规则

- 修改 runtime contract API 列表时，先改 `src/runtime/feng_runtime_contract.inc`。
- 若 API 需要新的 helper 实现，落在 `src/runtime/feng_runtime_contract.c` 或对应 runtime 实现文件中。
- 然后同步更新本文、标准库 `@runtime` 声明以及相关回归测试。
