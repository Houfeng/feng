# 泛型复合类型实例创建修复方案

> 状态：待审批
> 日期：2026-06-08
> 关联规范：[dev/feng-generics-aggregate-optimize.md](./feng-generics-aggregate-optimize.md)、[dev/feng-value-model-delivered.md](./feng-value-model-delivered.md)

## 1. 问题

### 1.1 现象

```
feng: panic: feng_aggregate: unknown forwarded slot kind 6 in 'JsonPayload'
```

### 1.2 根因

共享泛型方法体中，含泛型参数字段的复合类型的 C struct 为未特化形态——泛型参数字段为 `void*`（8 字节）。当泛型参数具体化为 by-value aggregate（如 `JsonPayload` = 40 字节）时，`sizeof(未特化 struct)` 不等于实际实例大小，导致内存截断和数据损坏。

### 1.3 问题本质

共享体中缺乏含泛型字段的复合类型的**具体化描述符**，导致两个相互关联的问题：

**问题一：大小错误**。实例创建、栈分配、数组 stride 均使用未特化大小（`sizeof(未特化 struct)`），当泛型参数为 by-value aggregate 时偏小，导致内存截断。

**问题二：生命周期描述符缺失**。含泛型字段的复合类型（如 `MapEntity<K,V>`）作为数组元素或局部变量时，共享体只有未特化描述符（`managed_slots = NULL`，偏移基于未特化布局），无法正确驱动 retain/release，导致内存泄漏或 UAF。

两个问题同根同源：共享体无法获得与具体化类型参数对应的正确描述符。

### 1.4 为何共享体无法持有具体化描述符

共享体编译时 K/V 未知，调用点（Wrapper）编译时 K/V 具体化，但共享体已编译为二进制——Wrapper 无法将静态生成的具体化描述符"注入"进已编译的共享体。这是二进制分发 + 泛型共享体模型的根本约束，与 Swift 的 `swift_initStructMetadata` 面临的约束相同。

### 1.5 受影响的代码路径（`src/codegen/codegen.c`）

1. **数组创建**（~L15448）：`feng_array_new(NULL, sizeof(未特化struct), false, n)` — element_size 错误，element_aggregate 使用未特化描述符
2. **Tuple 局部变量**（~L9667）：`struct <未特化形态> _tuple; memset(...)` — 栈变量太小
3. **数组元素写入**（~L18376）：`((未特化元素类型*)data)[idx] = value` — stride 错误
4. **数组元素读取 / for-in**（~L20526）：stride 错误
5. **Wrapper `_ret_buf`**（~L28443）：接收返回值的缓冲区按未特化大小分配，偏小
6. **托管对象创建**：共享体中若创建含泛型字段的托管对象，分配大小错误

## 2. 设计方案

### 2.1 核心思路：编译期描述符树

**Wrapper（调用点）在编译时已知所有具体类型**，因此由 Wrapper 静态生成具体化描述符（全在 `.rodata`，零运行时开销）。共享体通过描述符在运行时访问所有依赖。

类型级泛型参数描述符（如 `Container<K,V>` 的 `_K`、`_V`）收归到 `FengTypeDescriptor`/`FengAggregateDescriptor` 的 `reified_generic_params` 中，由 Wrapper 在描述符中静态生成。方法级泛型参数（如 `map<U>()` 的 `_U`）和独立泛型函数的泛型参数仍以 `FengGenericParamDescriptor*` 函数参数传入（调用点才能确定，无法收归到描述符）。字段偏移（`_field_offsets`）和具体化依赖（`reified_agg_deps`/`reified_type_deps`）收归到类型/函数描述符中。

**路径一：类型实例方法（有 `self`）**

`self->_hdr.desc` 即是当前实例的具体化 `FengTypeDescriptor`，永远可靠。原先通过函数参数传入的 `_field_offsets` 和类型级泛型参数 `_K`、`_V` 均收归到 `FengTypeDescriptor` 中；共享体在运行时通过 `self->_hdr.desc->reified_generic_params[i]` 访问类型级泛型参数，通过 `->reified_field_offsets[i]`、`->reified_agg_deps[i]`、`->reified_type_deps[i]` 访问所有具体化信息。共享体仅保留 `_self` 参数，移除 `_field_offsets`、`_K`、`_V`。

**路径二：类型静态方法（无 `self`）**

Wrapper 将具体化 `FengTypeDescriptor*` 作为额外参数 `_type_desc` 传入共享体，共享体通过 `_type_desc->reified_generic_params[i]` 访问类型级泛型参数，通过 `_type_desc->reified_agg_deps[i]`、`->reified_type_deps[i]` 访问所有具体化信息。类型级泛型参数 `_K`、`_V` 不再作为独立函数参数。

**路径三：独立泛型函数（无 `self`）**

Wrapper 为本次具体化静态生成一个 **`FengFunctionDescriptor`**（`static const`），传入共享体时命名为 `_desc`。共享体通过 `_desc->reified_agg_deps[i]`、`->reified_type_deps[i]` 在运行时访问所有具体化信息。泛型参数 `_K`、`_V` 保持为独立函数参数。

**路径四：fit 泛型方法（有 `self`，类型外部函数）**

fit 方法本质上是类型外部函数（类似独立函数的语法糖）。若 fit 方法本身有函数级泛型参数，或 fit 目标类型有类型级泛型参数，则需在语义阶段收集待具体化的依赖，在发码阶段生成 `FengFunctionDescriptor`。处理逻辑与路径三（独立泛型函数）非常接近：Wrapper 为本次具体化静态生成 `FengFunctionDescriptor`，传入共享体时命名为 `_desc`，共享体通过 `_desc->reified_agg_deps[i]`、`_desc->reified_type_deps[i]` 在运行时访问 fit 方法体内的具体化依赖。函数级泛型参数以 `FengGenericParamDescriptor*` 函数参数传入。与路径三唯一的区别是：fit 方法有 `self`，可通过 `self->_hdr.desc->reified_generic_params[i]` 访问 fit 目标类型的类型级泛型参数，无需将类型级泛型参数作为独立函数参数传入。

路径一、二中，类型级泛型参数收归到描述符的 `reified_generic_params` 中，共享体不再接收 `_K`、`_V` 函数参数；路径三中，独立泛型函数的泛型参数仍以 `FengGenericParamDescriptor*` 函数参数传入（调用点确定，无法收归）；路径四中，fit 方法的类型级泛型参数从 `self->_hdr.desc->reified_generic_params` 获取（与路径一一致），函数级泛型参数和方法体内的具体化依赖通过 `FengFunctionDescriptor`（`_desc`）传入（与路径三一致）。方法级泛型参数（如 `Container<K>.map<U>()` 中的 `U`）同样以 `FengGenericParamDescriptor*` 函数参数传入，由 Wrapper 在调用点生成。

`FengTypeDescriptor.reified_agg_deps[]` 和 `reified_type_deps[]` 的索引在整个类型范围内**全局稳定**：codegen 收集该类型所有方法（实例方法 + 静态方法）中全部依赖（成员字段 + 各方法体局部依赖），按依赖排序 key（见 §2.3）字典序升序分配全局唯一索引，所有方法共享同一 `FengTypeDescriptor`，各方法按编译期确定的固定索引访问各自所需的描述符。含方法级泛型的方法，其涉及方法级类型参数的依赖使用独立索引（在类型级依赖之后追加）。

新增 `FengFunctionDescriptor` 描述独立泛型函数的具体化依赖（详见 §2.3）。生命周期三大类不变。Aggregate walker 不变。不新增 runtime 函数。

**核心处理流程**

原则是：严格分层，每层负责每层的工作，然后交由下一阶段处理。

1. 语义阶段分析并收集 "泛型类型、独立泛泛函数、fit 泛型方法" 的待具体化依赖。
   - 泛型类型具体化依赖，收集来源包括：类型级泛型参数及方法参数和返回值中的待具化泛型类型，方法体内的待具体化类型，包括成员方法和静态方法，以及成员初始化中的待具化泛型类型
   - 独立泛泛函数具体化依赖，收集来源包括：函数参数及返回值中的待具化泛型类型、方法体内的待具化泛型类型。 fit 方法的具体化依赖和独立函数一致
2. 泛型类型及方法、独立函数、fit 泛方法，本身的泛型参数，目前语义阶段的分析应该已经是具备的，再确认一下即可
3. 第 1/2 步，除了基于 AST 分析和收集本包信息，还需基于导入的外包信息分析，目标是交给下一阶段处理时是完整的
4. 语义阶段完成后，泛型参数信息、具体化依赖，应该是完整的，然后交给符号表导出和发码阶段生成代码
5. 符号表导出，将本包各泛型类型、独立泛型函数、fit 泛方法的具体化依赖导出到符号表（泛型参数目前应该是有的）
6. 发码阶段，根据语义阶段收集到的具体化依赖，生成具化的描述符 `FengTypeDescriptor`、`FengFunctionDescriptor`，如果有依赖则级连生成具化的描述符

### 2.2 语义阶段具体化依赖与符号表导出

语义阶段为每个泛型声明（泛型类型、独立泛型函数、fit 泛型方法）收集待具体化依赖，存入 `FengSemanticAnalysis` 的侧表。符号表导出阶段将依赖信息填入 `FengSymbolDeclView`，再由 ft 序列化写出（ft 二进制格式详见 §2.8）。

#### 2.2.1 语义阶段数据结构

新增结构定义于 `src/semantic/semantic.h`：

```c
/* 具体化依赖的分类：aggregate（tuple/struct by-value）或 managed（type 托管对象）。 */
typedef enum FengReifiedDepKind {
    FENG_REIFIED_DEP_KIND_AGGREGATE = 0,
    FENG_REIFIED_DEP_KIND_MANAGED
} FengReifiedDepKind;

/* 单条具体化依赖记录。
 * type_ref 指向 AST 中的泛型类型引用（如 Foo<int,V>），
 * 其类型参数可含 TYPE_REF_TYPE_PARAM 节点（引用 owner 的泛型参数）。 */
typedef struct FengReifiedDep {
    FengReifiedDepKind kind;
    const FengTypeRef *type_ref;
} FengReifiedDep;

/* 一个泛型声明的全部具体化依赖。
 * owner_decl 标识所属泛型声明：
 *   - 泛型类型：FENG_DECL_TYPE（聚合该类型所有方法的依赖）
 *   - 独立泛型函数：FENG_DECL_FUNCTION
 *   - fit 泛型方法：fit 下的方法声明
 * deps 数组按收集顺序追加，同一类型引用不重复记录。 */
typedef struct FengReifiedDepSet {
    const FengDecl *owner_decl;
    FengReifiedDep *deps;
    size_t dep_count;
    size_t dep_capacity;
} FengReifiedDepSet;
```

`FengSemanticAnalysis` 新增侧表：

```c
typedef struct FengSemanticAnalysis {
    /* ... 已有字段不变 ... */
    FengReifiedDepSet *reified_dep_sets;
    size_t reified_dep_set_count;
    size_t reified_dep_set_capacity;
} FengSemanticAnalysis;
```

配套 API（声明于 `src/semantic/semantic.h`，实现于新文件 `src/semantic/reified_deps.c`）：

```c
/* 获取或创建 owner_decl 的具体化依赖集。 */
FengReifiedDepSet *feng_semantic_get_or_create_reified_dep_set(
    FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl);

/* 向依赖集追加一条具体化依赖。 */
bool feng_semantic_reified_dep_set_append(
    FengReifiedDepSet *dep_set,
    FengReifiedDepKind kind,
    const FengTypeRef *type_ref);

/* 查找 owner_decl 的具体化依赖集，不存在时返回 NULL。 */
const FengReifiedDepSet *feng_semantic_lookup_reified_dep_set(
    const FengSemanticAnalysis *analysis,
    const FengDecl *owner_decl);
```

收集规则（对应 §2.1 核心处理流程第 1 步）：

| 泛型声明类型 | 收集来源 |
|------|------|
| 泛型类型 | 成员字段类型、方法参数及返回值类型、方法体内局部使用的泛型类型、成员初始化表达式中的泛型类型、静态方法体内的泛型类型 |
| 独立泛型函数 | 函数参数及返回值类型、函数体内局部使用的泛型类型 |
| fit 泛型方法 | 与独立泛型函数一致：方法参数及返回值类型、方法体内局部使用的泛型类型 |

**收集约束**：仅收集**含当前声明的泛型参数**的泛型类型引用——即 `type_ref` 的类型参数树中至少存在一个 `TYPE_REF_TYPE_PARAM` 引用当前声明的泛型参数。已完全具体化的类型引用（如 `Foo<int,string>`）无需运行时具体化，不记录。

#### 2.2.2 符号表导出结构

`FengSymbolDeclView`（`src/symbol/internal.h`）新增字段，承载从语义阶段传递到符号表导出的具体化依赖信息：

```c
struct FengSymbolDeclView {
    /* ... 已有字段不变 ... */
    FengSymbolTypeView **reified_agg_deps;    /* aggregate 类型依赖列表 */
    size_t reified_agg_dep_count;
    FengSymbolTypeView **reified_type_deps;   /* managed 类型依赖列表 */
    size_t reified_type_dep_count;
};
```

`export.c` 从 `FengSemanticAnalysis` 的 `FengReifiedDepSet` 读取依赖，按 `kind` 分类填入 `reified_agg_deps`（`FENG_REIFIED_DEP_KIND_AGGREGATE`）和 `reified_type_deps`（`FENG_REIFIED_DEP_KIND_MANAGED`）。每个 `FengSymbolTypeView*` 为 `FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC` 类型视图，完整保留基础类型名和类型参数信息（含 `TYPE_PARAM_REF` 引用）。

`ft_write.c` 将每条依赖序列化为 ATTRS 节中的 `FengSymbolFtAttrRecord`（attr kind 定义见 §2.8）；`ft_read.c` 反序列化回 `FengSymbolDeclView` 字段。ft 二进制格式详见 §2.8。

### 2.3 描述符结构

#### `FengFunctionDescriptor`（新增）

独立泛型函数的具体化描述符，Wrapper 静态生成，传入共享体时命名 `_desc`，承载函数体内的具体化依赖。泛型参数（`_K`、`_V`）不在此描述符中，仍以独立 `FengGenericParamDescriptor*` 函数参数传入。

```c
typedef struct FengFunctionDescriptor {
    const char *name;
    /* 本次调用中直接使用的各具体化 aggregate（tuple）类型的描述符，
     * 按排序 key（见下方"依赖排序 key 规则"）字典序升序排列。
     * [约束] 严禁用于生命周期管理（retain/release/destroy）；唯一用途是在运行时
     *        找到具体化描述符，完成大小计算、栈分配、数组 stride、创建实例等操作。 */
    size_t reified_agg_deps_count;
    const FengAggregateDescriptor *const *reified_agg_deps;
    /* 本次调用中需要创建实例的各具体化 managed（type）类型的描述符，
     * 按排序 key 字典序升序排列。
     * [约束] 严禁用于生命周期管理（retain/release/destroy）；唯一用途是在运行时
     *        找到具体化描述符以创建实例。生命周期管理由 ARC 指针操作负责，与此无关。 */
    size_t reified_type_deps_count;
    const FengTypeDescriptor *const *reified_type_deps;
} FengFunctionDescriptor;
```

#### `FengAggregateDescriptor`（扩展）

```c
typedef struct FengAggregateDescriptor {
    /* ... 已有字段不变 ... */

    /* 具体化泛型参数（类型级），按声明顺序排列（index 0 = 第一个泛型参数）。
     * 由 Wrapper 在描述符中静态生成。aggregate 自身不直接访问，
     * 供持有该描述符的外部代码读取泛型参数信息（如传递给其他函数调用）。
     * 非泛型 aggregate 时为 0/NULL。 */
    size_t reified_generic_params_count;
    const FengGenericParamDescriptor *const *reified_generic_params;

    /* 保留字段，当前版本始终为 0 / NULL。
     * 预留给未来非托管结构体（non-managed struct）场景：
     * 若非托管 struct 内含泛型 aggregate 字段且无法通过 managed_slots.nested 表达，
     * 届时启用这三组字段，语义与 FengTypeDescriptor 上的同名字段一致。
     * 当前所有 aggregate 类型的内部嵌套依赖均由 managed_slots[i].nested 链式引用。
     * [约束] 届时同样严禁用于生命周期管理（retain/release/destroy），
     *        唯一用途是在运行时找到具体化描述符，完成大小计算等操作。 */
    size_t reified_field_offset_count;                                   /* 当前恒为 0 */
    const size_t *reified_field_offsets;                                  /* 当前恒为 NULL */
    size_t reified_agg_deps_count;                                       /* 当前恒为 0 */
    const struct FengAggregateDescriptor *const *reified_agg_deps;       /* 当前恒为 NULL */
    size_t reified_type_deps_count;                                      /* 当前恒为 0 */
    const struct FengTypeDescriptor *const *reified_type_deps;           /* 当前恒为 NULL */
} FengAggregateDescriptor;
```

#### `FengTypeDescriptor`（扩展）

```c
typedef struct FengTypeDescriptor {
    /* ... 已有字段不变 ... */

    /* 具体化泛型参数（类型级），按声明顺序排列（index 0 = 第一个泛型参数）。
     * 由 Wrapper 在描述符中静态生成，替代原先由 Wrapper 传入的
     * FengGenericParamDescriptor* 函数参数。共享体通过
     * reified_generic_params[i] 获取各类型级泛型参数的描述信息。
     * 非泛型类型时为 0/NULL。 */
    size_t reified_generic_params_count;
    const FengGenericParamDescriptor *const *reified_generic_params;

    /* 具体化字段偏移（原共享体函数参数 _field_offsets），
     * 按字段声明顺序排列（index 0 = 第一个字段）。
     * 共享体通过 reified_field_offsets[i] 获取各字段在具体化 struct 中的偏移，
     * 替代原先由 Wrapper 传入的 const size_t* 函数参数。
     * 无字段时为 0/NULL。 */
    size_t reified_field_offset_count;
    const size_t *reified_field_offsets;

    /* 具体化泛型类型的全局依赖（所有方法共享，索引全局稳定）：
     * reified_agg_deps: 该类型所有方法中使用的具体化 aggregate（tuple）类型的描述符，
     *           包含成员字段的 aggregate 依赖和各方法体内局部的 aggregate 依赖
     * reified_type_deps: 该类型所有方法中需要创建新实例的具体化 managed（type）类型的描述符；
     *            托管成员字段的 ARC（retain/release）操作无需此描述符，仅"创建新实例"场景使用
     * 索引规则：按排序 key（见下方"依赖排序 key 规则"）字典序升序在 codegen 阶段全局分配，
     *           同类型不同方法访问各自所需索引，索引语义跨方法一致不变。
     * [约束] reified_agg_deps/reified_type_deps 严禁用于生命周期管理（retain/release/destroy）；
     *        唯一用途是声明静态描述符间依赖关系，使方法和成员在运行时正确找到具体化描述符，
     *        完成大小计算、栈分配、数组 stride、创建实例等操作。
     *        生命周期管理由 managed_slots（FengAggregateDescriptor）和 ARC 指针操作负责，与此无关。 */
    size_t reified_agg_deps_count;
    const struct FengAggregateDescriptor *const *reified_agg_deps;
    size_t reified_type_deps_count;
    const struct FengTypeDescriptor *const *reified_type_deps;
} FengTypeDescriptor;
```

类型级泛型参数（`_K`、`_V`）收归到 `FengTypeDescriptor`/`FengAggregateDescriptor` 的 `reified_generic_params` 中，不再作为独立函数参数传入类型方法共享体。方法级泛型参数和独立泛型函数的泛型参数仍以 `FengGenericParamDescriptor*` 函数参数传入。

访问方式对比：

| 场景 | 具体化描述符来源 | 类型级泛型参数 | 方法级泛型参数 | 字段偏移 | aggregate 依赖 | managed 依赖 |
|------|------|------|------|------|------|------|
| 类型实例方法 | `_td`（`self->_hdr.desc`） | `_td->reified_generic_params[i]` | N/A | `_td->reified_field_offsets[i]` | `_td->reified_agg_deps[i]` | `_td->reified_type_deps[i]` |
| 类型实例方法（有方法级泛型） | `_td`（`self->_hdr.desc`） | `_td->reified_generic_params[i]` | `_U`（函数参数） | `_td->reified_field_offsets[i]` | `_td->reified_agg_deps[i]` | `_td->reified_type_deps[i]` |
| 类型静态方法 | `_type_desc`（Wrapper 传入） | `_type_desc->reified_generic_params[i]` | N/A | N/A | `_type_desc->reified_agg_deps[i]` | `_type_desc->reified_type_deps[i]` |
| 类型静态方法（有方法级泛型） | `_type_desc`（Wrapper 传入） | `_type_desc->reified_generic_params[i]` | `_U`（函数参数） | N/A | `_type_desc->reified_agg_deps[i]` | `_type_desc->reified_type_deps[i]` |
| 独立泛型函数 | `_desc`（Wrapper 传入） | N/A（函数参数 `_K`, `_V`） | N/A | N/A | `_desc->reified_agg_deps[i]` | `_desc->reified_type_deps[i]` |

> **重要约束**：`reified_agg_deps`/`reified_type_deps` 两组字段**严禁直接用于生命周期管理**（retain/release/destroy）。它们的唯一用途是声明静态描述符之间的依赖关系，使类型的方法和成员能在运行时正确找到并使用相应具体化描述符，完成大小计算、栈分配、数组 stride、创建实例等操作。生命周期管理仍由 `managed_slots`（`FengAggregateDescriptor`）和 ARC 指针操作（`FengTypeDescriptor`）负责，与这两个字段无关。

#### 依赖排序 key 规则

`reified_agg_deps[]`/`reified_type_deps[]` 的索引按各依赖类型的**排序 key** 字典序升序分配。

排序 key 的生成规则（递归定义）：

- **根模式**（作为排序 key 的顶层）：`TypeName__p1__p2__...`，参数间以 `__`（双下划线）分隔
- **参数模式**（作为另一个类型参数出现时）：`TypeName_p1_p2_...`，参数间以 `_`（单下划线）分隔
- 每个参数 `pi` 递归以**参数模式**展开
- **`UserType` 自身的泛型参数**（即当前类型/函数声明中的类型参数）以其在声明列表中的**0-based 索引**表示为 `T0`、`T1`、`T2`……，不使用参数名（防止仅因重命名泛型参数导致排序 key 变化而破坏兼容性）

示例（`UserType<K,V>` 内，`K` 为 `T0`、`V` 为 `T1`，依赖 `Foo<int,K>`、`Bar<V>`、`Xyz<Foo<int,K>,Bar<V>>`）：

| 类型 | 泛型参数替换 | 排序 key（根模式） |
|------|------|------|
| `Foo<int,K>` | `K` → `T0` | `Foo__int__T0` |
| `Bar<V>` | `V` → `T1` | `Bar__T1` |
| `Xyz<Foo<int,K>,Bar<V>>` | `K` → `T0`，`V` → `T1` | `Xyz__Foo_int_T0__Bar_T1` |

`Foo<int,K>` 作为 `Xyz` 的参数时展开为参数模式 `Foo_int_T0`；`Bar<V>` 展开为 `Bar_T1`。

三者按字典序：`Bar__T1` < `Foo__int__T0` < `Xyz__Foo_int_T0__Bar_T1`，分别占 `reified_agg_deps[0]`、`reified_agg_deps[1]`、`reified_agg_deps[2]`。

### 2.4 Wrapper 静态生成具体化描述符

Wrapper 在编译时按以下步骤生成：

1. 遍历共享体内所有含泛型字段的 aggregate/managed 类型（成员字段 + 方法体局部使用），代入具体类型参数，为每个类型生成具体化描述符
2. 生成某 aggregate 描述符的 `managed_slots` 时，若某字段的类型仍含未特化泛型参数，则触发链式物化：递归对该类型执行步骤 1，直到所有字段均为非泛型叶子类型（类型图无环，DFS 必然终止）；内层描述符直接作为外层 `managed_slots` 中对应 slot 的 `nested` 指针（不经过 `reified_agg_deps`/`reified_type_deps`）
3. 按依赖排序 key（见 §2.3）字典序升序为所有依赖分配**全局稳定索引**，填入 `reified_agg_deps[]` / `reified_type_deps[]`

上述步骤均为**编译期逻辑**：静态描述符本身及描述符间的依赖关系，在同包内依赖 AST 生成，跨包依赖符号表（ft，见 §2.8）生成；生成结果以 `static const` 形式写入目标文件，运行时只读取，不动态构造。

**链式物化无需特殊检测**：触发条件就是"生成某个 `FENG_SLOT_NESTED_AGGREGATE` 的 `nested` 指针时，该字段的类型仍含泛型参数"，codegen 在生成每个字段 slot 时递归走该逻辑即可。

#### 场景一：独立泛型函数

**示例**（`func process<K, V>()` 内直接使用 `MapEntity<K,V>`，`K=Foo`（托管），`V=Bar`（aggregate））：

```c
/* MapEntity<Foo, Bar> 的具体化 managed slots */
static const FengManagedSlotDescriptor MapEntity__K_Foo__V_Bar__slots[] = {
    { offsetof(struct MapEntity__K_Foo__V_Bar, item1), FENG_SLOT_POINTER, NULL },
    { offsetof(struct MapEntity__K_Foo__V_Bar, item2), FENG_SLOT_NESTED_AGGREGATE, &Bar__desc },
};
/* MapEntity<Foo, Bar> 具体化泛型参数 */
static const FengGenericParamDescriptor *MapEntity__K_Foo__V_Bar__generic_params[] = {
    &(const FengGenericParamDescriptor){ FENG_VALUE_MANAGED_POINTER, &Foo__type_desc, &Foo__witness },
    &(const FengGenericParamDescriptor){ FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &Bar__agg_desc, NULL },
};
/* MapEntity<Foo, Bar> 具体化描述符（reified_agg_deps/reified_type_deps 保留字段当前恒为 0/NULL） */
static const FengAggregateDescriptor MapEntity__K_Foo__V_Bar__desc = {
    .name = "MapEntity", .size = sizeof(struct MapEntity__K_Foo__V_Bar),
    .managed_slot_count = 2, .managed_slots = MapEntity__K_Foo__V_Bar__slots,
    .reified_generic_params_count = 2, .reified_generic_params = MapEntity__K_Foo__V_Bar__generic_params,
};
/* process<Foo, Bar> 函数具体化描述符，reified_agg_deps[0] = MapEntity<Foo,Bar> */
static const FengAggregateDescriptor *process__K_Foo__V_Bar__reified_agg_deps[] = {
    &MapEntity__K_Foo__V_Bar__desc,
};
static const FengFunctionDescriptor process__K_Foo__V_Bar__desc = {
    .name = "process",
    .reified_agg_deps_count = 1, .reified_agg_deps = process__K_Foo__V_Bar__reified_agg_deps,
    .reified_type_deps_count = 0, .reified_type_deps = NULL,
};
/* 调用共享体：_desc 在最前，_K/_V 保持为独立函数参数 */
process__G__K__V(
    &process__K_Foo__V_Bar__desc,  /* _desc */
    &(const FengGenericParamDescriptor){ FENG_VALUE_MANAGED_POINTER, &Foo__type_desc, &Foo__witness },   /* _K */
    &(const FengGenericParamDescriptor){ FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &Bar__agg_desc, NULL }, /* _V */
    ...);
```

共享体内访问：`_desc->reified_agg_deps[0]->size` 在运行时给出 `MapEntity<Foo,Bar>` 的正确大小。

#### 场景二：泛型类型的方法

普通方法虽无方法级泛型，但共享体中仍需具体化信息：**访问自身成员字段**需要具体化后的偏移（`reified_field_offsets`），**使用类型级泛型参数**（如在方法体内创建 `Boo<K>` 局部变量、操作 `Foo<int,V>` 成员）需要从 `self->_hdr.desc` 读取对应的 `reified_agg_deps`/`reified_type_deps`。

**示例**（`type Container<K,V>` 有成员字段 `entry: Foo<int,V>`（tuple），方法体内局部使用 `Boo<K>`（tuple））：

```c
/* 具体化 aggregate 描述符 */
static const FengAggregateDescriptor Boo__K_Foo__desc = {
    .name = "Boo", .size = sizeof(struct Boo__K_Foo),
    .managed_slot_count = ..., .managed_slots = ...,
};
static const FengAggregateDescriptor Foo__int__Bar__desc = {
    .name = "Foo", .size = sizeof(struct Foo__int__Bar),
    .managed_slot_count = ..., .managed_slots = ...,
};
/* Container<Foo,Bar> 的具体化字段偏移 */
static const size_t Container__K_Foo__V_Bar__reified_field_offsets[] = {
    offsetof(struct Container__K_Foo__V_Bar, entry),
    /* ... 其他字段 ... */
};
/* Container<Foo,Bar> 的具体化泛型参数 */
static const FengGenericParamDescriptor *Container__K_Foo__V_Bar__generic_params[] = {
    &(const FengGenericParamDescriptor){ FENG_VALUE_MANAGED_POINTER, &Foo__type_desc, &Foo__witness },
    &(const FengGenericParamDescriptor){ FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &Bar__agg_desc, NULL },
};
/* Container<Foo,Bar> 的 FengTypeDescriptor：
 * reified_agg_deps 按全局稳定排序 key 顺序覆盖该类型所有方法（实例方法+静态方法）的 aggregate 依赖 */
static const FengAggregateDescriptor *Container__K_Foo__V_Bar__reified_agg_deps[] = {
    &Boo__K_Foo__desc,      /* reified_agg_deps[0]：Boo<K>（按类型名排序在 Foo 之前） */
    &Foo__int__Bar__desc,   /* reified_agg_deps[1]：成员字段 entry: Foo<int,V> */
};
static const FengTypeDescriptor Container__K_Foo__V_Bar__type_desc = {
    .name = "Container", .size = sizeof(struct Container__K_Foo__V_Bar),
    /* ... 其他字段 ... */
    .reified_generic_params_count = 2, .reified_generic_params = Container__K_Foo__V_Bar__generic_params,
    .reified_field_offset_count = 1, .reified_field_offsets = Container__K_Foo__V_Bar__reified_field_offsets,
    .reified_agg_deps_count = 2, .reified_agg_deps = Container__K_Foo__V_Bar__reified_agg_deps,
    .reified_type_deps_count = 0, .reified_type_deps = NULL,
};
/* 调用方法共享体：类型级泛型参数在 self->_hdr.desc->reified_generic_params 中，无需函数参数传递 */
Container_method__G__K__V(
    self,
    ...);
```

方法共享体内访问（索引由 codegen 在编译期按全局稳定顺序确定）：

```c
const FengTypeDescriptor *_td = ((FengManagedHeader *)_self)->desc;

/* 类型级泛型参数（原函数参数 _K、_V，现从描述符获取） */
const FengGenericParamDescriptor *_K = _td->reified_generic_params[0];
const FengGenericParamDescriptor *_V = _td->reified_generic_params[1];

/* 字段访问（原函数参数 _field_offsets，现从描述符获取） */
void *_entry_ptr = (char *)_self + _td->reified_field_offsets[0];

/* 方法体内局部 Boo<K>（tuple）：reified_agg_deps[0]，大小在运行时读取 */
const FengAggregateDescriptor *_boo_desc = _td->reified_agg_deps[0];
_Alignas(max_align_t) char _boo_mem[_boo_desc->size];
feng_aggregate_default_init(_boo_mem, _boo_desc);

/* 成员字段 entry（Foo<int,V> 是 tuple）：reified_agg_deps[1]，字段偏移从 reified_field_offsets 获取 */
const FengAggregateDescriptor *_entry_desc = _td->reified_agg_deps[1];
feng_aggregate_assign(_entry_ptr, &new_entry, _entry_desc);
```

**成员字段为托管对象（type）时**：ARC 操作（retain/release）无需描述符；若需在方法体内**创建该托管类型新实例**，则通过 `_td->reified_type_deps[i]` 获取其具体化 `FengTypeDescriptor`。

**每种具体化（如 `Container<Foo,Bar>`）生成一份 `FengTypeDescriptor`**，所有方法的共享体共享该描述符，各自按编译期已知的固定索引访问。

#### 场景三：泛型类型的泛型方法

**示例**（`type Container<K,V>` 的方法 `map<U>(fn: (V) -> U) -> [U]`，调用 `Container<Foo,Bar>.map<Baz>(...)`）：

```c
/* 调用方法共享体：类型级 _K/_V 在 self->_hdr.desc->reified_generic_params 中，仅方法级 _U 为函数参数 */
Container_map__G__K__M__U(
    self,
    &(const FengGenericParamDescriptor){ FENG_VALUE_MANAGED_POINTER, &Baz__type_desc, &Baz__witness },   /* _U（方法级） */
    ...);
```

方法共享体内访问：

```c
const FengTypeDescriptor *_td = ((FengManagedHeader *)_self)->desc;

/* 类型级泛型参数从描述符获取 */
const FengGenericParamDescriptor *_K = _td->reified_generic_params[0];
const FengGenericParamDescriptor *_V = _td->reified_generic_params[1];
/* _K->kind / _K->descriptor / _K->witness 等访问方式与原有代码一致 */

/* 字段偏移和类型级依赖从描述符获取（与场景二一致） */
void *_entry_ptr = (char *)_self + _td->reified_field_offsets[0];
const FengAggregateDescriptor *_boo_desc = _td->reified_agg_deps[0];

/* 方法级泛型参数 _U 从函数参数获取（调用点确定，无法收归到描述符） */
/* _U->kind / _U->descriptor 等访问方式与原有代码一致 */
```

### 2.5 共享体 ABI 变更

类型级泛型参数（`_K`、`_V` 等）收归到 `FengTypeDescriptor` 的 `reified_generic_params` 中，不再作为类型方法的函数参数。独立泛型函数的泛型参数和方法级泛型参数仍以函数参数传入。字段偏移（`_field_offsets`）和具体化依赖（`reified_agg_deps`/`reified_type_deps`）收归到描述符中。独立泛型函数新增 `_desc`（`FengFunctionDescriptor*`）参数；类型静态方法新增 `_type_desc`（`FengTypeDescriptor*`）参数；类型实例方法不新增参数（通过 `self->_hdr.desc` 获取），并移除原有 `_field_offsets`、`_K`、`_V` 参数。

**独立泛型函数**——保持 `_K`、`_V` 参数，新增 `_desc`：

```c
/* 旧签名 */
void process__G__K__V(const FengGenericParamDescriptor *_K,
                      const FengGenericParamDescriptor *_V, ...)

/* 新签名：_desc 在最前，_K、_V 不变 */
void process__G__K__V(const FengFunctionDescriptor *_desc,
                      const FengGenericParamDescriptor *_K,
                      const FengGenericParamDescriptor *_V, ...)
```

**泛型类型的实例方法共享体（无方法级泛型）**——仅保留 `_self`，移除 `_field_offsets`、`_K`、`_V`（均从 `self->_hdr.desc` 获取）。共享体内通过 `((FengManagedHeader *)_self)->desc->reified_generic_params[i]` 访问类型级泛型参数，支持方法体内使用 `K`、`V` 进行类型判断、传递给其他泛型调用等操作：

```c
/* 旧签名 */
void Container_method__G__K__V(void *_self, const size_t *_field_offsets,
                                const FengGenericParamDescriptor *_K,
                                const FengGenericParamDescriptor *_V, ...)
/* 新签名：_field_offsets/_K/_V 均从 self->_hdr.desc 获取 */
void Container_method__G__K__V(void *_self, ...)

/* 共享体内访问类型级泛型参数（即使方法本身无方法级泛型） */
const FengTypeDescriptor *_td = ((FengManagedHeader *)_self)->desc;
const FengGenericParamDescriptor *_K = _td->reified_generic_params[0];
const FengGenericParamDescriptor *_V = _td->reified_generic_params[1];
/* _K/_V 可用于：kind 判断、size 获取、传递给其他泛型函数调用、数组创建等 */
```

**泛型类型的静态方法共享体（无方法级泛型）**——移除 `_K`、`_V`，新增 `_type_desc`（承载 `reified_generic_params`/`reified_agg_deps`/`reified_type_deps`）。共享体内通过 `_type_desc->reified_generic_params[i]` 访问类型级泛型参数，支持方法体内使用 `K`、`V`：

```c
/* 旧签名 */
void Container_static_method__G__K__V(const FengGenericParamDescriptor *_K,
                                       const FengGenericParamDescriptor *_V, ...)
/* 新签名：_type_desc 承载所有类型级信息，_K/_V 从 _type_desc->reified_generic_params 获取 */
void Container_static_method__G__K__V(const FengTypeDescriptor *_type_desc, ...)

/* 共享体内访问类型级泛型参数（即使方法本身无方法级泛型） */
const FengGenericParamDescriptor *_K = _type_desc->reified_generic_params[0];
const FengGenericParamDescriptor *_V = _type_desc->reified_generic_params[1];
/* _K/_V 可用于：kind 判断、size 获取、传递给其他泛型函数调用、数组创建等 */
```

**含方法级泛型参数时**——类型级泛型从描述符获取，仅方法级泛型以独立 `FengGenericParamDescriptor*` 参数传入：

```c
/* 实例方法：Container<K>.map<U>()——类型级 _K 从 self->_hdr.desc 获取，仅传方法级 _U */
void Container_map__G__K__M__U(void *_self,
                                const FengGenericParamDescriptor *_U, ...)

/* 静态方法：Container<K>.make<U>()——类型级 _K 从 _type_desc 获取，仅传方法级 _U */
void Container_make__G__K__M__U(const FengTypeDescriptor *_type_desc,
                                 const FengGenericParamDescriptor *_U, ...)
```

### 2.6 共享体内使用具体化描述符

以下各路径中，`reified_generic_params` / `reified_agg_deps` / `reified_type_deps` 的索引 `i` 均由 codegen 在**编译期**确定（泛型参数按声明顺序，依赖按排序 key 字典序），不是运行时计算的值。描述符来源因路径而异：

- 类型实例方法：`_td = ((FengManagedHeader *)_self)->desc`，通过 `_td->reified_generic_params[i]` 访问类型级泛型参数，通过 `_td->reified_agg_deps[i]` / `_td->reified_type_deps[i]` 访问具体化依赖
- 类型静态方法：`_type_desc`（Wrapper 传入），通过 `_type_desc->reified_generic_params[i]` / `_type_desc->reified_agg_deps[i]` / `_type_desc->reified_type_deps[i]` 访问
- 独立泛型函数：`_desc`（Wrapper 传入），泛型参数为独立函数参数，通过 `_desc->reified_agg_deps[i]` / `_desc->reified_type_deps[i]` 访问具体化依赖

取到具体化描述符后，后续操作完全一致。以下示例以类型实例方法（`_td`）为例。

#### 2.6.1 修复数组创建（~L15448）

```c
const FengAggregateDescriptor *_ed = _td->reified_agg_deps[i];
FengArray *_arr = feng_array_new_kinded(
    FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, _ed, NULL, _ed->size, _n);
```

#### 2.6.2 修复局部变量（~L9667）

```c
const FengAggregateDescriptor *_ed = _td->reified_agg_deps[i];
void *_mem = alloca(_ed->size);
memset(_mem, 0, _ed->size);
feng_aggregate_default_init(_mem, _ed);
```

#### 2.6.3 修复数组元素写入（~L18376）

```c
void *_slot = (char *)feng_array_data(_arr) + _idx * _td->reified_agg_deps[i]->size;
feng_aggregate_assign(_slot, _src, _td->reified_agg_deps[i]);
```

#### 2.6.4 修复数组元素读取 / for-in（~L20526）

```c
void *_elem = (char *)feng_array_data(_arr) + _fidx * _td->reified_agg_deps[i]->size;
feng_aggregate_assign(_iter_mem, _elem, _td->reified_agg_deps[i]);
```

取到描述符后后续操作完全一致，无特判。**tuple 与其他 by-value aggregate 均走同一路径，无需额外处理**；tuple 是否需要生命周期管理由描述符的 `managed_slots` 决定，walker 自动分派。

#### 2.6.5 修复 Wrapper 返回值缓冲区（~L28443）

字段偏移在未特化和具体化 struct 间一致（padding 机制保证）。直接传 `&ret_tmp`（具体化类型，大小正确），去掉原先按未特化大小分配的临时缓冲区。

#### 2.6.6 修复托管对象创建

需创建某个泛型托管类型新实例时，通过 `reified_type_deps[i]` 获取其具体化 `FengTypeDescriptor`：

```c
const FengTypeDescriptor *_node_desc = _td->reified_type_deps[i];
FengObject *_obj = feng_obj_alloc(_node_desc->size, _node_desc);
```

### 2.8 跨包泛型依赖信息（ft 扩展）

同包内 Wrapper 可直接从 AST 语义信息收集目标 type/func 的依赖列表。跨包时只有 ft，需在 ft 中记录每个泛型 type/func 的依赖元数据，供调用方 codegen 读取并按排序 key 分配索引。

#### ft 现有能力

- **TYPS** 节已有 `NAMED_GENERIC`（kind=6）：`string_ref`=全限定基础名，`elem_start`=TSEQ 起始，`elem_count`=类型参数数量——已能完整表达 `Foo<int,K>`
- **ATTRS** 节：`symbol_id` + `kind`（16 位）+ `value0/1/2`（各 32 位），可挂在任意符号上

#### 新增两个 attr kind（`src/symbol/internal.h`）

```c
typedef enum FengSymbolAttrKind {
    /* ... 已有 1–8 不变 ... */
    FENG_SYMBOL_ATTR_GENERIC_AGG_DEP  = 9,   /* 泛型 aggregate 依赖 */
    FENG_SYMBOL_ATTR_GENERIC_TYPE_DEP = 10,  /* 泛型 managed 依赖 */
} FengSymbolAttrKind;
```

两个 attr 的字段布局（`FengSymbolFtAttrRecord`）：

| 字段 | 含义 |
|------|------|
| `symbol_id` | 泛型 type 的 sym id（`SYM_KIND_TYPE`）或 func 的 sym id（`SYM_KIND_TOP_FN` / `SYM_KIND_METHOD`） |
| `kind` | `FENG_SYMBOL_ATTR_GENERIC_AGG_DEP`（9）或 `FENG_SYMBOL_ATTR_GENERIC_TYPE_DEP`（10） |
| `value0` | `TYPS.id`（`NAMED_GENERIC` 记录，包含完整类型名和参数信息） |
| `value1` | 0（保留） |
| `value2` | 0（保留） |

排序 key 由读取方从 `TYPS` 记录按 §2.3 规则推导，不存 ft，保持 ft 紧凑。

每个泛型依赖对应一条 attr 记录，多个依赖对应多条记录，Wrapper 读取后按 sort key 字典序排列，分配 `reified_agg_deps[]`/`reified_type_deps[]` 索引。

### 2.7 不新增 Runtime 函数

本方案不引入任何新的 runtime 函数和 runtime 源文件：

- `feng_generic_aggregate_instance_size`：不需要（大小直接来自 `desc->size`）
- `feng_generic_type_instance_size`：不需要
- `feng_generic_desc_resolve` / 全局缓存：不需要（全部静态，`.rodata`）
- `feng_generic_array_new`：不需要（直接调用 `feng_array_new_kinded`）

新增一个结构体 `FengFunctionDescriptor`（详见 §2.3），用于独立泛型函数的具体化依赖传递。扩展现有 `FengTypeDescriptor`（新增 `reified_generic_params`/`reified_field_offsets`/`reified_agg_deps`/`reified_type_deps` 四组字段）和 `FengAggregateDescriptor`（新增 `reified_generic_params` 一组字段，以及保留字段 `reified_field_offsets`/`reified_agg_deps`/`reified_type_deps`，当前恒为 0/NULL）。

## 3. 不变量

1. **生命周期三大类不变**
2. **Aggregate walker 不变**
3. **已有非泛型代码路径不变**：无具体化描述符需求的共享体行为完全等同于原先
4. **`FengAggregateDescriptor` 向后兼容**：新增保留字段在末尾，count 字段默认为 0、指针字段默认为 NULL
5. **零运行时开销**：所有具体化描述符均为 `static const`，全在 `.rodata`

## 4. 涉及文件

| 文件 | 变更 |
|------|------|
| `src/runtime/feng_runtime.h` | 新增 `FengFunctionDescriptor` 结构体（独立泛型函数描述符）；`FengTypeDescriptor` 新增 `reified_generic_params_count`/`reified_generic_params`/`reified_field_offset_count`/`reified_field_offsets`/`reified_agg_deps_count`/`reified_agg_deps`/`reified_type_deps_count`/`reified_type_deps` 八个字段；`FengAggregateDescriptor` 新增 `reified_generic_params_count`/`reified_generic_params` 两个字段和 `reified_field_offset_count`/`reified_field_offsets`/`reified_agg_deps_count`/`reified_agg_deps`/`reified_type_deps_count`/`reified_type_deps` 六个保留字段（当前恒为 0/NULL） |
| `src/codegen/codegen.c` | Wrapper 生成具体化描述符树（`FengFunctionDescriptor`/`FengTypeDescriptor`/`FengAggregateDescriptor` 静态实例含 `reified_generic_params`/`reified_agg_deps`/`reified_type_deps`）；实例方法共享体移除 `_field_offsets`、`_K`、`_V` 参数改从 `self->_hdr.desc` 获取；静态方法共享体移除 `_K`、`_V` 参数新增 `_type_desc`（`FengTypeDescriptor*`）参数；独立函数共享体新增 `_desc`（`FengFunctionDescriptor*`）参数（泛型参数仍为函数参数）；修复 6 个创建路径（§2.6.1–§2.6.6）；读取 ft ATTRS 中 `GENERIC_AGG_DEP`/`GENERIC_TYPE_DEP` 支持跨包特化 |
| `src/symbol/internal.h` | `FengSymbolAttrKind` 新增 `FENG_SYMBOL_ATTR_GENERIC_AGG_DEP = 9`、`FENG_SYMBOL_ATTR_GENERIC_TYPE_DEP = 10` |
| `src/symbol/ft_write.c` | 写出泛型 type/func 的 aggregate 和 managed 依赖 attr 记录 |
| `src/symbol/ft_read.c` | 解析新 attr kind，填入对应符号的依赖列表 |

## 5. 验证

```bash
feng run examples/hello_world --keep-ir
# 预期: START / OBJ_CREATED / OBJ_SET / JV_CREATED / JV: {"a":1}

feng test
# 全量回归
```

## 6. 开发任务

### 6.1 Runtime 头文件变更

- [x] `src/runtime/feng_runtime.h`：新增 `FengFunctionDescriptor` 结构体定义
- [x] `src/runtime/feng_runtime.h`：`FengTypeDescriptor` 新增 `reified_generic_params_count`/`reified_generic_params`/`reified_field_offset_count`/`reified_field_offsets`/`reified_agg_deps_count`/`reified_agg_deps`/`reified_type_deps_count`/`reified_type_deps` 八个字段
- [x] `src/runtime/feng_runtime.h`：`FengAggregateDescriptor` 新增 `reified_generic_params_count`/`reified_generic_params` 两个字段及六个保留字段（恒为 0/NULL）
- [x] 更新所有现有 `FengTypeDescriptor` 静态实例（补充新字段初始化为 0/NULL）
- [x] 更新所有现有 `FengAggregateDescriptor` 静态实例（补充新字段初始化为 0/NULL）
- [x] 编译验证：`feng test` 全量回归通过（纯结构扩展，行为不变）

### 6.2 符号表 ft 扩展

- [ ] `src/symbol/internal.h`：`FengSymbolAttrKind` 新增 `FENG_SYMBOL_ATTR_GENERIC_AGG_DEP = 9`、`FENG_SYMBOL_ATTR_GENERIC_TYPE_DEP = 10`
- [ ] `src/symbol/ft_write.c`：泛型 type/func 导出时写出 aggregate 和 managed 依赖 attr 记录
- [ ] `src/symbol/ft_read.c`：解析新 attr kind，填入对应符号的依赖列表
- [ ] 编译验证：`feng test` 全量回归通过

### 6.3 Codegen——共享体 ABI 变更

- [ ] 类型实例方法共享体：移除 `_field_offsets`、类型级 `_K`/`_V` 参数，改从 `self->_hdr.desc` 获取
- [ ] 类型静态方法共享体：移除类型级 `_K`/`_V` 参数，新增 `_type_desc`（`FengTypeDescriptor*`）参数
- [ ] 独立泛型函数共享体：新增 `_desc`（`FengFunctionDescriptor*`）参数（保留 `_K`/`_V` 参数）
- [ ] 含方法级泛型的方法：类型级泛型从描述符获取，仅方法级泛型保留为函数参数
- [ ] 编译验证：`feng test` 全量回归通过

### 6.4 Codegen——Wrapper 生成具体化描述符树

- [ ] 实现依赖收集：遍历泛型 type 所有方法（实例+静态）收集 aggregate/managed 依赖
- [ ] 实现排序 key 生成逻辑（§2.3 规则）
- [ ] 实现全局稳定索引分配（按排序 key 字典序升序）
- [ ] Wrapper 生成具体化 `FengAggregateDescriptor`（含 `managed_slots` 链式物化）
- [ ] Wrapper 生成具体化 `FengTypeDescriptor`（含 `reified_generic_params`/`reified_field_offsets`/`reified_agg_deps`/`reified_type_deps`）
- [ ] Wrapper 生成 `FengFunctionDescriptor`（独立泛型函数场景）
- [ ] 编译验证：`feng test` 全量回归通过

### 6.5 Codegen——修复六个创建路径

- [ ] §2.6.1 修复数组创建：使用 `reified_agg_deps[i]->size` 替代 `sizeof(未特化struct)`
- [ ] §2.6.2 修复局部变量栈分配：VLA 或 alloca + `desc->size`
- [ ] §2.6.3 修复数组元素写入：stride 使用具体化描述符大小
- [ ] §2.6.4 修复数组元素读取/for-in：stride 使用具体化描述符大小
- [ ] §2.6.5 修复 Wrapper `_ret_buf`：按具体化类型大小分配
- [ ] §2.6.6 修复托管对象创建：通过 `reified_type_deps[i]` 获取具体化 `FengTypeDescriptor`
- [ ] 编译验证：`feng test` 全量回归通过

### 6.6 跨包特化支持

- [ ] Wrapper codegen 读取 ft ATTRS 中 `GENERIC_AGG_DEP`/`GENERIC_TYPE_DEP`，推导排序 key 并分配索引
- [ ] 跨包调用场景生成正确的具体化描述符树
- [ ] 编译验证：`feng test` 全量回归通过

### 6.7 端到端验证

- [ ] `feng run examples/hello_world --keep-ir` 输出符合预期
- [ ] `feng test` 全量回归通过
- [ ] 检查生成的 C 代码中描述符为 `static const`，确认零运行时开销
