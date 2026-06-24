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

> 内建类型数量：从 `12` 个类型名 + `5` 个别名，变更为 `12` 个类型名 + `4` 个别名（移除 `long`，新增 `uint`）。

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
| `symbol/export.c` | `canonical_builtin_name()` |
| `semantic/analyzer.c` | `is_builtin_type_name()` / `canonical_builtin_type_name()` |
| `semantic/spec_relations.c` | `rel_builtin_canonical_name()` |
| `semantic/spec_witnesses.c` | 同类 if/else 链 |
| `codegen/codegen.c` | `k_builtin_types[]` / `cg_is_builtin_named_fit_target()` |
| `cli/lsp/runtime.c` | 多处硬编码别名判断 |

### 6.2 目标

在语义分析入口处统一归一所有 type_ref 中的别名为标准名，后续阶段（codegen、symbol export、LSP）只看到 `i32`/`i64`/`u8`/`f32`/`f64` 等标准名，不再各自处理别名。

### 6.3 为什么放在语义分析阶段而非语法分析阶段

| 维度 | 语法分析（Parser） | 语义分析（Semantic） |
| --- | --- | --- |
| 职责 | 只关心语法结构，不感知类型是否存在 | 关心类型含义，别名归一属于语义判断 |
| 平台配置 | 需引入平台信息，增加耦合 | 编译上下文（目标平台）天然可用 |
| 可扩展性 | 未来用户自定义类型别名会进一步膨胀 | 用户自定义别名天然属于语义层 |
| 实现效果 | AST 中只有标准名 | 归一后 AST 中同样只有标准名 |

结论：别名归一放在语义分析阶段，职责清晰，可扩展，实现效果一致。

### 6.4 实现方案

```
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

- `semantic/spec_relations.c`：`rel_builtin_canonical_name()` 中的别名分支
- `semantic/spec_witnesses.c`：同类别名 if/else 链
- `codegen/codegen.c`：`k_builtin_types[]` 中的别名字段、`cg_is_builtin_named_fit_target()` 中的别名项
- `symbol/export.c`：`canonical_builtin_name()` 中的别名条目
- `cli/lsp/runtime.c`：多处 `name == "int" || name == "i32"` 硬编码
- LSP hover 显示标准名称，无需回溯原始别名

## 7. 影响范围

- 规范文档：`docs/feng-language.md`（别名表与关键字说明）、`docs/feng-builtin-type.md`
- 编译器：语义分析入口新增统一别名归一逻辑，下游 6 个文件的别名代码移除
- 现有代码：使用 `long` 的源码需迁移为 `i64`
- 测试：全量回归，重点关注使用 `long` 别名的测试用例

## 8. 决策记录

- **2026-06-24**：草案提出
- **2026-06-24**：决策——`float`/`double` 保持固定宽度，仅移除 `long`，新增 `uint`，`int` 改为平台相关
- **2026-06-24**：决策——别名归一收至语义分析入口，Parser 不感知别名，后续阶段只看到标准名
