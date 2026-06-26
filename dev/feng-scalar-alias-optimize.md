# 标量类型别名优化

> 状态：草案  
> 日期：2026-06-24  
> 关联规范：[docs/feng-language.md](../docs/feng-language.md)、[docs/feng-builtin-type.md](../docs/feng-builtin-type.md)

## 1. 背景

Feng 的标量类型体系以固定宽度类型为基础，所有标量类型均为 `ixx`/`uxx`/`fxx` 形式的确定宽度类型（如 `i32`、`u64`、`f32`）。`int`、`long`、`float`、`double` 等**不是独立类型**，仅是编译期别名，解析后直接替换为对应的固定宽度类型，不引入新的类型实体。

当前别名采用固定宽度映射：

| 别名 | 映射 |
| --- | --- |
| `int` | `i32` |
| `long` | `i64` |
| `byte` | `u8` |
| `float` | `f32` |
| `double` | `f64` |

问题：

- `long` 语义模糊（具体多长？），增加认知负担
- `int` 固定为 `i32`，在 64 位平台上与 C ABI 的 `int`/`intptr_t` 对齐时需频繁做显式宽度转换
- 缺少平台相关的无符号整数别名

## 2. 方案

### 2.1 取消 `long`

`long`（`i64` 别名）：移除，使用者直接写 `i64`。

理由：`long` 语义不精确，移除后命名空间更干净，且不影响表达能力。`double` 与 `float` 作为配对别名保留（见 2.4 节）。

### 2.2 `int` 改为平台相关别名

编译时根据目标平台映射：

- 32 位平台 → `i32`
- 64 位平台 → `i64`

### 2.3 新增 `uint` 平台相关别名

与 `int` 对称：

- 32 位平台 → `u32`
- 64 位平台 → `u64`

### 2.4 `float` / `double` 保持固定

- `float` → `f32`（固定，不变）
- `double` → `f64`（固定，不变）

理由：所有主流语言的浮点类型均为固定宽度（IEEE 754），不存在整数那样"机器字长影响运算效率"的问题。`float` 与 `double` 作为配对别名保留，与 C 的 `float`/`double` 语义对齐。

## 3. 主流语言参照

| 语言 | 默认整数 | 平台相关类型 | 固定宽度类型 | 浮点 |
| --- | --- | --- | --- | --- |
| C/C++ | `int`（平台相关） | `int`、`long`、`intptr_t`、`size_t` 等 | `int32_t`、`int64_t`、`uint32_t` 等（C99/C++11 起） | `float`、`double`（事实固定） |
| Go | `int`（平台相关） | `int`、`uint`、`uintptr` | `int8`~`int64`、`uint8`~`uint64` | `float32`、`float64`（固定） |
| Swift | `Int`（平台相关） | `Int`、`UInt` | `Int8`~`Int64`、`UInt8`~`UInt64` | `Float`（32 位）、`Double`（64 位）（固定） |
| Rust | 无默认，需显式指定 | `isize`、`usize` | `i8`~`i128`、`u8`~`u128` | `f32`、`f64`（固定） |
| Zig | 无默认，需显式指定 | `isize`、`usize`、`c_int`、`c_long` 等 | `i8`~`i128`、`u8`~`u128` | `f16`、`f32`、`f64`、`f80`、`f128`（固定） |
| Java | `int`（固定 32 位） | 无 | `byte`、`short`、`int`、`long` | `float`、`double`（固定） |
| C# | `int`（固定 32 位） | `nint`、`nuint`（C# 9 起） | `byte`、`short`、`int`、`long` | `float`、`double`（固定） |
| Kotlin | `Int`（固定 32 位） | 无 | `Byte`、`Short`、`Int`、`Long` | `Float`、`Double`（固定） |

Go、Swift 的设计模式一致：默认整数类型（`int`/`Int`）匹配机器字长，同时提供完整固定宽度类型族；浮点类型则始终固定宽度。Feng 定位为兼顾脚本语言简洁性与 C 语言高效性，且与 C ABI 紧密互操作，采用平台相关 `int` 与 Go/Swift 路线一致。

> **Feng 与其他语言的关键差异**：Go 的 `int` 与 `int32`/`int64` 是不同的独立类型（需显式转换）；Swift 的 `Int` 与 `Int32`/`Int64` 同理。Feng 的 `int` 仅是别名，编译期直接替换为 `i32` 或 `i64`，不引入新的类型实体，不存在"别名类型与底层类型之间的转换"问题。

## 4. 变更后别名表

| 别名 | 映射 | 性质 |
| --- | --- | --- |
| `int` | `i32` 或 `i64` | 平台相关（变更） |
| `uint` | `u32` 或 `u64` | 平台相关（新增） |
| `byte` | `u8` | 固定（不变） |
| `float` | `f32` | 固定（不变） |
| `double` | `f64` | 固定（不变） |

> 内建类型数量：从 `12` 个类型名 + `5` 个别名，变更为 `12` 个类型名 + `5` 个别名（移除 `long`，新增 `uint`，总量不变）。

## 5. 已决问题：`float` / `double` 是否平台相关

**结论：保持固定宽度，不做平台相关化。**

理由：

- 所有主流语言（C/C++、Go、Swift、Rust、Zig、Java、C#、Kotlin）的浮点类型均为固定宽度，这是行业共识
- IEEE 754 已统一浮点表示，不存在整数那样"机器字长影响运算效率"的问题
- C 的 `float`/`double` 事实上跨平台固定为 32/64 位，Feng 保持一致有利于 ABI 互操作
- Apple 的 `CGFloat`（平台相关浮点）被视为历史包袱，Swift 生态已逐步推荐直接用 `Double`
- `float` 与 `double` 是配对别名，保留其一就应保留另一个

## 6. 架构收敛：别名归一收至语义分析入口

### 6.1 现状问题

当前别名映射散布在 6 个文件中，各自维护一份 if/else 链或查找表：

| 文件 | 函数/表 |
| --- | --- |
| `src/semantic/analyzer.c` | `is_builtin_type_name()` / `canonical_builtin_type_name()` |
| `src/semantic/spec_relations.c` | `rel_builtin_canonical_name()` |
| `src/semantic/spec_witnesses.c` | `canonical_builtin_type_name_local()` |
| `src/codegen/codegen.c` | `k_builtin_types[]` / `cg_is_builtin_named_fit_target()`（注：`k_builtin_types[]` 自 2026-04-28 初始创建起即含有 `uint → CG_TYPE_U32` 条目，系 codegen 创建时的预留；但 2026-05-11 添加 `cg_is_builtin_named_fit_target()` 时已刻意排除 `uint`，且 semantic 层从未识别 `uint`，因此该条目为不可达死代码） |
| `src/symbol/export.c` | `canonical_builtin_name()` |
| `src/cli/lsp/runtime.c` | 两处硬编码别名判断（3717-3720 行、4120-4123 行），仅检查别名（`int`/`long`/`byte`/`float`/`double`/`bool`/`string`/`void`），未包含标准名（`i8`~`i64`/`u8`~`u64`/`f32`/`f64`），归一后别名检查变为死代码，标准名检查仍缺失 |

### 6.2 目标

在语义分析入口处统一归一所有 type_ref 中的别名为标准名，后续阶段（codegen、symbol export、LSP）只看到 `i32`/`i64`/`u32`/`u64`/`u8`/`f32`/`f64` 等标准名，不再各自处理别名。codegen 不感知 `int`/`uint`/`byte`/`float`/`double` 等别名，其 `k_builtin_types[]` 仅保留标准名条目。

### 6.3 为什么放在语义分析阶段而非语法分析阶段

| 维度 | 语法分析（Parser） | 语义分析（Semantic） |
| --- | --- | --- |
| 职责 | 只关心语法结构，不感知类型是否存在 | 关心类型含义，别名归一属于语义判断 |
| 平台配置 | 需引入平台信息，增加耦合 | 编译上下文（目标平台）天然可用 |
| 可扩展性 | 未来用户自定义类型别名会进一步膨胀 | 用户自定义别名天然属于语义层 |
| 实现效果 | AST 中只有标准名 | 归一后 AST 中同样只有标准名 |

结论：别名归一放在语义分析阶段，职责清晰，可扩展，实现效果一致。

### 6.4 实现方案

```text
Parser → AST（type_ref 中保留用户写的原始名称，如 int、byte）
  ↓
Semantic 入口 → 统一遍历 AST，将所有 type_ref 中的别名替换为标准名（一次性归一）
  ↓
后续所有阶段 → 只看到标准名，别名逻辑全部移除
```

归一逻辑集中为一个函数（示意）：

```c
/* 平台相关别名在归一阶段根据编译目标决定映射 */
static const char *canonical_builtin_type_name(FengSlice name, PlatformTarget target) {
    if (slice_eq(name, "int"))  return target == PLATFORM_64 ? "i64" : "i32";
    if (slice_eq(name, "uint")) return target == PLATFORM_64 ? "u64" : "u32";
    if (slice_eq(name, "byte"))  return "u8";
    if (slice_eq(name, "float")) return "f32";
    if (slice_eq(name, "double")) return "f64";
    /* i8/i16/i32/i64/u16/u32/u64/f32/f64/bool/string/void 直接返回自身 */
    ...
}
```

### 6.5 可清理的下游代码

归一收敛后，以下别名相关逻辑可移除：

- `src/semantic/spec_relations.c`：`rel_builtin_canonical_name()` 中的别名分支
- `src/semantic/spec_witnesses.c`：`canonical_builtin_type_name_local()` 中的别名分支
- `src/semantic/analyzer.c`：`is_builtin_type_name()` 的 `builtin_names[]` 中的别名条目（归一后只看到标准名，别名条目为死代码）
- `src/codegen/codegen.c`：`k_builtin_types[]` 中的别名字段（含 `uint` 死代码条目）及 `cg_is_builtin_named_fit_target()` 中的别名项。归一后 codegen 不再感知任何别名，`k_builtin_types[]` 仅保留标准名（`i32`/`i64`/`u32`/`u64` 等）
- `src/symbol/export.c`：`canonical_builtin_name()` 中的别名条目
- `src/cli/lsp/runtime.c`：两处硬编码（3717-3720 行、4120-4123 行）替换为调用 `is_builtin_type_name()`（或提供等价导出函数），统一覆盖所有内建名（标准名 + 别名）；归一并移除别名条目后，该调用自然只匹配标准名。此举同时修复现状中标准名（`i8`~`i64`/`u8`~`u64`/`f32`/`f64`）未被识别为内建类型的遗漏
- LSP hover 显示标准名称，无需回溯原始别名

### 6.6 已决问题：AST 归一后原始名称不保留

**结论：归一后 AST 中 type_ref 仅保留标准名，不保留用户书写的原始别名。**

影响：编译器错误诊断（如类型不匹配）将显示标准名（如 `i64`），而非用户写的别名（如 `long`）。这是可接受的取舍，Go、Rust 等语言在类型推断中也采用同一策略——别名在编译早期归一，诊断统一使用标准名。

### 6.7 已决问题：移除 `long` 后的错误处理策略

**结论：移除 `long` 后，编译器按"未知类型名"处理，不添加专门的迁移提示。**

理由：

- 添加专门的 `long` 迁移提示属于临时 workaround，违反"不做特殊处理"原则
- `long` 移除后变为普通标识符，可用于变量或类型命名，专门报错反而限制了标识符空间
- 迁移期由人工完成标准库与测试代码的批量替换，不依赖编译器诊断引导

### 6.8 已决问题：字面量类型推导保留别名

**结论：语义分析内部构造的 `InferredExprType.builtin_name` 保留别名（如 `"int"`、`"double"`、`"byte"`），不归一为标准名。**

语义分析在推导字面量类型时硬编码使用别名：

| 表达式类型 | 代码 | 含义 |
| --- | --- | --- |
| 整数字面量 | `inferred_expr_type_builtin("int")` | 默认类型为 `int` |
| 浮点字面量 | `inferred_expr_type_builtin("double")` | 默认类型为 `double` |
| `&string` 指针元素 | `inferred_expr_type_builtin("byte")` | 元素类型为 `byte` |

这些 `builtin_name` 后续通过 `canonical_builtin_type_name()` 归一为标准名。保留别名是正确的：

- 规范要求"整数字面量默认推导为 `int`"，而非"推导为 `i32`"
- Task 6 使 `int` 平台相关后，`canonical_builtin_type_name("int", platform)` 自然返回平台匹配的 `i32` 或 `i64`，字面量类型自动跟随平台
- `"double"` 和 `"byte"` 为固定别名，归一结果始终为 `f64` 和 `u8`，不受平台影响

因此，AST 预遍历归一仅处理用户书写的 type_ref，语义分析内部构造的 `InferredExprType` 保持别名不变。

> **实现注意**：`inferred_expr_type_builtin_canonical_name()` 内部调用 `canonical_builtin_type_name()`（analyzer.c:5668），Task 3 后需同步获取平台信息（通过 `ResolveContext` 或显式参数），确保 `"int"` 等别名的归一结果跟随平台。

## 7. 影响范围

- 规范文档：`docs/feng-language.md`（别名表与关键字说明）、`docs/feng-builtin-type.md`（别名表、映射规则，以及正文中引用 `long` 的描述如 `string.length()` 返回类型）
- 编译器：语义分析入口新增统一别名归一逻辑，下游 6 个文件的别名代码移除；codegen `k_builtin_types[]` 仅保留标准名条目，不再感知别名；LSP `runtime.c` 两处硬编码替换为 `is_builtin_type_name()` 调用，同时修复标准名未被识别为内建类型的现有遗漏
- 标准库（`std/`）：约 639 处 `long` 使用需迁移为 `i64`，涉及 `stdio.ff`、`SystemInfo.ff`、`MemoryInfo.ff`、`TestContext.ff` 等文件
- 兼容性测试（`fcts/`）：约 23 处 `long` 使用需迁移
- 编译器测试（`test/`）：3 处 `.ff` 文件中的 `long` 使用需迁移；C 测试代码（`test_lexer.c`、`test_cli.c` 等）中的 `long` 为 C 语言类型，不受影响
- 清理 codegen `k_builtin_types[]` 中自初始提交即存在的 `uint → CG_TYPE_U32` 预留死代码（归一收敛时一并处理）

## 8. 分步任务

> 别名归一收敛为基础任务，完成后别名的增删改只需修改一处。每步内部遵循"先规范（文档）再实现（代码）后测试"原则，文档与实现始终对齐。

### Task 1：别名归一收敛（基础，纯重构，无行为变更）

- [x] 确认 `src/semantic/analyzer.c` 中 `canonical_builtin_type_name()` 为唯一归一入口
- [x] 在语义分析开始时遍历 AST，将所有 type_ref 中的别名替换为标准名（归一后 AST 仅保留标准名，不保留原始别名）。需覆盖的位置：函数签名（参数类型、返回类型）、变量/常量声明的类型注解、struct/enum/type_decl 中的字段类型、spec 声明与 impl 中的类型引用、泛型实参（type_args）、extern 声明的类型签名、强转表达式的目标类型
- [x] 移除 `src/semantic/analyzer.c` 中 `is_builtin_type_name()` 的 `builtin_names[]` 中的别名条目（归一后只看到标准名，别名条目为死代码）
- [x] 移除 `src/semantic/spec_relations.c` 中 `rel_builtin_canonical_name()` 的别名分支
- [x] 移除 `src/semantic/spec_witnesses.c` 中 `canonical_builtin_type_name_local()` 的别名分支
- [x] 移除 `src/codegen/codegen.c` 中 `k_builtin_types[]` 的别名字段（含 `uint` 死代码条目）及 `cg_is_builtin_named_fit_target()` 的别名项；归一后 codegen 仅感知标准名，不再包含任何别名
- [x] 移除 `src/symbol/export.c` 中 `canonical_builtin_name()` 的别名条目
- [x] 将 `src/cli/lsp/runtime.c` 中两处硬编码别名判断（3717-3720 行、4120-4123 行）替换为调用 `is_builtin_type_name()`（或提供等价导出函数），归一并移除别名条目后自然只匹配标准名；同时修复现状中标准名未被识别为内建类型的遗漏
- [x] 全量回归测试，确认无行为变更

### Task 2：移除 `long` 别名

- [x] 更新 `docs/feng-language.md`：别名表移除 `long`
- [x] 更新 `docs/feng-builtin-type.md`：别名表与映射规则移除 `long`，正文中引用 `long` 的描述改为 `i64`（如 `string.length()` 返回类型）
- [x] 迁移标准库（`std/`，约 639 处）中使用 `long` 的代码为 `i64`
- [x] 迁移兼容性测试（`fcts/`，约 23 处）中使用 `long` 的代码为 `i64`
- [x] 迁移编译器测试（`test/`，3 处 `.ff` 文件）中使用 `long` 的代码为 `i64`
- [x] 从集中别名表中移除 `long` → `i64` 条目
- [x] 全量回归测试

### Task 3：编译器平台位宽映射能力（无行为变更）

> 为 `canonical_builtin_type_name()` 增加平台参数，但当前阶段 `int` 仍固定映射为 `i32`，行为不变。此步骤仅建立基础设施。

- [x] `canonical_builtin_type_name()` 新增平台参数（通过 `ResolveContext` 或显式参数）
- [x] 实现平台位宽检测函数（如 `feng_get_host_pointer_size()`），根据当前宿主机返回指针位宽（`sizeof(void *)`），并注释：未来支持交叉编译时，需要通过编译选项传入目标平台位宽（核心编译器不直接读取 CLI 参数，已有 `FengSemanticAnalyzeOptions` 机制）
- [x] `FengSemanticAnalyzeOptions` 新增 `size_t pointer_size` 字段，语义分析入口由调用方（CLI 层）填入宿主机位宽；`pointer_size` 在语义分析入口存入 `ResolveContext`
- [x] `inferred_expr_type_builtin_canonical_name()` 内部调用 `canonical_builtin_type_name()` 时同步传入平台信息
- [x] 全量回归测试（`int` 仍固定映射 `i32`，应无行为变更）

### Task 4：新增 `uint` 平台相关别名（验证 Task 3 基础设施）

> `uint` 是全新别名，无现有代码依赖，正好验证 Task 3 的平台位宽映射是否正确。

- [x] 更新 `docs/feng-language.md`：别名表新增 `uint`（平台相关说明）
- [x] 更新 `docs/feng-builtin-type.md`：别名表与映射规则新增 `uint`
- [x] 集中别名表新增 `uint` → `u32`（32 位）或 `u64`（64 位）；仅需修改 `canonical_builtin_type_name()`，codegen 等下游阶段无需变更
- [x] 全量回归测试

### 已废弃任务

此为 旧 Task 5，已废弃（原本的处理也已 Revert）

```md
### 旧 Task 5：迁移现有 `int` 用法为 `i32`（纯代码迁移，无行为变更）

> 消除代码对 `int` 当前语义（固定 32 位）的依赖。Task 6 改变 `int` 含义后，已有代码不受影响。

- [ ] 迁移标准库（`std/`）中使用 `int` 的代码为 `i32`
- [ ] 迁移兼容性测试（`fcts/`）中使用 `int` 的代码为 `i32`
- [ ] 迁移编译器测试（`test/`）中使用 `int` 的代码为 `i32`
- [ ] 全量回归测试
```

### Task 5：std 中的数组长度、字符串长度、容器 size 改为 `int` (后续将和平台位宽一致)

- [ ] feng_runtime_contract.inc 中 Array/String 相关 API 的 `int64_t` 改为 `intptr_t`（含 `feng_array_length_i64`、`feng_array_slice`、`feng_string_utf8_length`、`feng_string_from_utf8_bytes`、`feng_string_slice`、`feng_string_slice_bytes`、`feng_string_range_equal`）
- [ ] 更新受 feng_runtime_contract.inc 变更影响的 c 实现
- [ ] feng_array_length_i64 改名为 feng_array_get_length
- [ ] Array 的 `length()`、`at()`、`indexOf()`、`clone()` 等方法的 `i64` 改为 `int`（含 `feng_array_slice` 的 `start`/`length` 参数），及所有级联调用代码更新
- [ ] 全量回归测试（测试代码如果能通过就不要动），通过后将已完成任务 TODO 标记为完成，等下一步指令
- [ ] String 的 `length()` 等方法的 `i64` 改为 `int`（含 `feng_string_*` 系列参数），及所有级联调用代码更新
- [ ] 全量回归测试（测试代码如果能通过就不要动），通过后将已完成任务 TODO 标记为完成，等下一步指令
- [ ] Map 的 size 改为 int 类型，及所有级联代码更新
- [ ] 全量回归测试（测试代码如果能通过就不要动），通过后将已完成任务 TODO 标记为完成，等下一步指令
- [ ] List 的 size 改为 int 类型，及所有级联代码更新
- [ ] 全量回归测试（测试代码如果能通过就不要动），通过后将已完成任务 TODO 标记为完成，等下一步指令
- [ ] Set 的 size 改为 int 类型，及所有级联代码更新
- [ ] 全量回归测试（测试代码如果能通过就不要动），通过后将已完成任务 TODO 标记为完成，等下一步指令

### Task 6：`int` 改为平台相关别名

- [ ] 更新 `docs/feng-language.md`：别名表中 `int` 标注为平台相关
- [ ] 更新 `docs/feng-builtin-type.md`：`int` 映射规则改为平台相关，整数字面量默认类型描述更新（当前为"推导为 `int`（即 `i32`）"，`int` 平台相关后需同步更新描述）
- [ ] `canonical_builtin_type_name()` 中 `int` 映射从固定 `i32` 改为平台相关：32 位 → `i32`，64 位 → `i64`
- [ ] 全量回归测试
- [ ] 通过后将当前任务 TODO 标记为完成，等下一步指令

### Task 7：语义优化——识别应使用平台相关 `int` 的 `i32` 用法

- [ ] 分析 std 中所有应该明确使用平台位宽的地方，列出清单由人工决策
- [ ] 根据人工决策，进行代码变更
- [ ] 全量回归测试
- [ ] 通过后将当前任务 TODO 标记为完成，等下一步指令

## 9. 决策记录

- **2026-06-24**：草案提出
- **2026-06-24**：决策——`float`/`double` 保持固定宽度，仅移除 `long`，新增 `uint`，`int` 改为平台相关
- **2026-06-24**：决策——别名归一收至语义分析入口，Parser 不感知别名，后续阶段只看到标准名
- **2026-06-24**：决策——AST 归一后仅保留标准名，不保留用户书写的原始别名；编译器诊断统一使用标准名
- **2026-06-24**：决策——移除 `long` 后按"未知类型名"处理，不添加专门的迁移提示（避免临时 workaround）
- **2026-06-24**：发现 codegen `k_builtin_types[]` 自 2026-04-28 初始提交即含有 `uint → CG_TYPE_U32` 预留条目（codegen 创建时提前写入，semantic 层从未识别，为不可达死代码）；`cg_is_builtin_named_fit_target()`（2026-05-11 添加）已刻意排除 `uint`。归一收敛后 codegen 不感知任何别名，该条目及所有别名条目均从 `k_builtin_types[]` 中彻底删除
- **2026-06-24**：决策——规范文档更新拆入各任务中，每步内部遵循"先规范再实现"原则，文档与实现始终对齐
- **2026-06-24**：决策——语义分析内部的 `InferredExprType.builtin_name` 保留别名（`"int"`、`"double"`、`"byte"`），不归一为标准名；AST 预遍历归一仅处理用户书写的 type_ref，字面量类型通过 `canonical_builtin_type_name()` 延迟归一，自然跟随平台
- **2026-06-24**：发现 LSP `runtime.c` 两处硬编码（3717-3720 行、4120-4123 行）仅检查别名（`int`/`long`/`byte`/`float`/`double`/`bool`/`string`/`void`），未包含标准名（`i8`~`i64`/`u8`~`u64`/`f32`/`f64`），存在标准名未被识别为内建类型的遗漏。归一收敛时替换为调用 `is_builtin_type_name()`，统一覆盖所有内建名，同时修复该遗漏
- **2026-06-24**：决策——平台位宽信息通过 `FengSemanticAnalyzeOptions.pointer_size` 传入，语义分析入口存入 `ResolveContext`，`canonical_builtin_type_name()` 及 `inferred_expr_type_builtin_canonical_name()` 通过 `ResolveContext` 获取平台信息，保持核心编译器不直接依赖 CLI 参数
- **2026-06-24**：决策——Task 2 迁移顺序调整：先迁移所有 `long` 用法为 `i64`，最后才移除别名表条目。迁移期间 `long` 仍为合法别名，编译器可正常编译和验证，避免中间状态不可编译
- **2026-06-24**：决策——Task 3~7 五步拆分：先建平台位宽映射基础设施（Task 3），用新增 `uint` 验证（Task 4），再迁移所有 `int` → `i32`（Task 5），然后切换 `int` 为平台相关（Task 6），最后审计识别应平台相关的 `i32` 改回 `int`（Task 7）。Feng 的字面量贴合策略（`integer_literal_fits_canonical_target`）保证有目标类型注解的字面量不受默认推导变化影响
- **2026-06-26**：决策——Task 5/6/7 重构：旧 Task 5（迁移所有 `int` → `i32`）废弃。新 Task 5 将 std 中数组长度、字符串长度、容器 size 从 `i64`/`i32` 改为 `int`，runtime contract 对应 `int64_t` 改为 `intptr_t`，每种类型独立"改代码→回归测试"循环推进。Task 6 移除旧 Task 5 前提，仅保留切换 `int` 为平台相关的核心步骤。Task 7 从"审计旧 Task 5 迁移的 `i32`"调整为"分析 std 中所有应使用平台位宽的地方"，由人工决策后实施
