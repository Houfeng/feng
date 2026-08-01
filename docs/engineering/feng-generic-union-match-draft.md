# 泛型共享体内 Union Match + Narrowing 设计草案

> **状态**：草案（未最终确定，未实施）
> **日期**：2026-07-07
> **关联**：[feng-generics-delivered.md](./feng-generics-delivered.md)、[feng-runtime-generics-delivered.md](./feng-runtime-generics-delivered.md)、[feng-value-model-delivered.md](./feng-value-model-delivered.md)

---

## 1 背景

### 1.1 当前能力边界

Union-form spec 作为泛型约束已实现，泛型共享体可通过 `FengGenericParamDescriptor` 对 T 做值级操作（copy / retain / release / size）：

```feng
spec SimpleUnion: int | string;

func identity_union<T: SimpleUnion>(value: T): T {
    return value;  // 通过 descriptor 做 memcpy + retain/release，正常工作
}
```

但共享体**不能对 T 做 match + narrowing**：

```feng
spec Greetable { func greet(): string; }
spec GreetableOrString: Greetable | string;

func process<T: GreetableOrString>(v: T): string {
    return match v {                        // 编译失败
        let g: Greetable { g.greet() }
        let s: string { s }
        else { "else" }
    };
}
```

### 1.2 根因

codegen 的 match 实现有两条路径：

| 路径 | 条件 | 泛型参数 T |
|---|---|---|
| Union match | `tgt.type->kind == CG_TYPE_SPEC && form == FENG_SPEC_FORM_UNION` | `CG_TYPE_GENERIC_PARAM`，不满足 |
| Scalar/enum match | `tk == BOOL / STRING / integer` | 不满足 |

两条路径均要求 target 类型在编译期确定。泛型参数 T 在不同实例化点的值表示可能完全不同：

```feng
foo<GreetableOrString>(v1)  // T 有 tag + payload（int | string）
foo<Greetable>(v2)          // T 有 subject + witness（无 tag）
foo<string>(v3)             // T 是 managed pointer（无 tag、无 payload）
```

共享体编译一次，无法生成适用于所有实例化的 tag 检查代码。

### 1.3 根本矛盾

共享体只通过 `FengGenericParamDescriptor` 获得**值级**信息（size、slots），不获得**结构级**信息（tag 含义、成员类型、narrowing 路径）。

---

## 2 方案：扩展 Union 描述符

### 2.1 核心思路

在 union spec 的 `FengAggregateDescriptor` 中增加**成员元信息数组**，使共享体在运行时能通过 descriptor 获取：

- 每个成员的 tag 值
- 每个成员的值类型（trivial / managed pointer / aggregate）
- 每个成员的 payload 偏移和大小
- 嵌套 descriptor（用于递归处理嵌套 union 或 object-form spec）

### 2.2 新增结构

```c
typedef struct FengUnionMemberDescriptor {
    uint32_t tag;                          // 该成员对应的 tag 值
    FengValueKind value_kind;              // TRIVIAL / MANAGED_POINTER / AGGREGATE
    size_t payload_offset;                 // payload 在 union value struct 内的偏移
    size_t payload_size;                   // payload 大小
    const void *nested_descriptor;         // 按 value_kind 转型的具体 descriptor
} FengUnionMemberDescriptor;
```

在 `FengAggregateDescriptor` 中增加 union 扩展字段（非 union 类型为零）：

```c
typedef struct FengAggregateDescriptor {
    /* ... 现有字段 ... */
    const char *name;
    size_t size;
    const FengManagedSlotDescriptor *managed_slots;
    size_t managed_slot_count;
    /* ... */

    /* union-form spec 扩展（其他类型为零） */
    size_t union_member_count;
    const FengUnionMemberDescriptor *union_members;
} FengAggregateDescriptor;
```

### 2.3 生成示例

```feng
spec GreetableOrString: Greetable | string;
```

```c
static const FengUnionMemberDescriptor FengSpecUnionMembers__GreetableOrString[] = {
    {
        .tag = 0,
        .value_kind = FENG_VALUE_AGGREGATE,
        .payload_offset = offsetof(struct FengSpecValue__GreetableOrString, payload.m0),
        .payload_size = sizeof(struct FengSpecValue__Greetable),
        .nested_descriptor = &FengSpecAgg__Greetable,
    },
    {
        .tag = 1,
        .value_kind = FENG_VALUE_MANAGED_POINTER,
        .payload_offset = offsetof(struct FengSpecValue__GreetableOrString, payload.m1),
        .payload_size = sizeof(FengString *),
        .nested_descriptor = NULL,
    },
};

static const FengAggregateDescriptor FengSpecAgg__GreetableOrString = {
    /* ... 现有字段 ... */
    .union_member_count = 2,
    .union_members = FengSpecUnionMembers__GreetableOrString,
};
```

### 2.4 共享体代码生成

codegen 为共享体内的 match 生成基于 descriptor 的运行时分发：

```c
// match v { let g: Greetable { g.greet() } let s: string { s } else { "else" } }
const FengAggregateDescriptor *_desc_T = _param_T->descriptor;
uint32_t _tag = *(uint32_t *)((char *)&v + 0);  // tag 始终在 offset 0

if (_desc_T->union_member_count > 0 && _tag == _desc_T->union_members[0].tag) {
    // Greetable 分支：从 payload 提取 { subject, witness }
    char *_payload = (char *)&v + _desc_T->union_members[0].payload_offset;
    void *_subject = *(void **)(_payload + offsetof(FengSpecValue__Greetable, subject));
    const struct FengSpecWitness__Greetable *_w =
        *(const struct FengSpecWitness__Greetable **)(_payload + offsetof(FengSpecValue__Greetable, witness));
    _ifv = _w->greet(_subject);
} else if (_desc_T->union_member_count > 1 && _tag == _desc_T->union_members[1].tag) {
    // string 分支
    _ifv = *(FengString **)((char *)&v + _desc_T->union_members[1].payload_offset);
    _ifv = feng_retain(_ifv);
} else {
    _ifv = feng_string_new("else", 4);
}
```

---

## 3 关键设计决策

### 3.1 tag 偏移

所有 union spec 的 tag 固定在 offset 0（`uint32_t`），这是现有布局的不变量。共享体可安全读取。

### 3.2 嵌套 union 的 tag 访问

嵌套 union（如 `NestedGreetable: GreetableOrString | int`）的外层 tag 在 offset 0，内层 tag 在 payload 内部。通过 `nested_descriptor` 递归：

```c
// 匹配到 GreetableOrString 分支后，进一步检查内层 tag
const FengAggregateDescriptor *_inner =
    (const FengAggregateDescriptor *)_desc_T->union_members[0].nested_descriptor;
uint32_t _inner_tag = *(uint32_t *)((char *)&v + _desc_T->union_members[0].payload_offset);
```

链式 match（`NestedGreetable -> GreetableOrString -> Greetable`）需要逐层 descriptor 递归。

### 3.3 Narrowing 绑定变量的表示

narrowing 后的绑定变量（如 `let g: Greetable`）是 payload 的**直接引用**（零拷贝）还是**拷贝**？

| 方案 | 优点 | 缺点 |
|---|---|---|
| 引用 | 零开销 | 需要确保生命周期不超过 match 分支 |
| 拷贝 | 安全 | 需要 retain，aggregate 需要完整 init |

待确定。

### 3.4 Object-form spec 成员的 witness 来源

当 union 成员是 object-form spec 时，payload 本身就是 `{ subject, witness }` 对。witness 已在值中存储，descriptor 只需提供偏移即可定位。descriptor 不需要额外存储 witness 指针。

### 3.5 ABI 兼容性

`FengAggregateDescriptor` 增加字段是 ABI 变更。需评估：
- 现有所有生成 `FengAggregateDescriptor` 的 codegen 路径需补零
- 运行时 walker 不读新字段，向后兼容

---

## 4 代价评估

### 4.1 编译期

- 每个 union spec 多生成一个 `FengUnionMemberDescriptor[]` 常量数组
- codegen 需新增共享体 match 路径（从 descriptor 生成 tag 检查、payload 提取）
- 语义分析需支持泛型参数上的 match label 验证

### 4.2 运行时

- 描述符体积增大（每个 union 成员约 32 字节），但为编译期常量，**零运行时计算开销**
- 共享体 match 通过 descriptor 间接寻址，比具体类型的 match 多一层指针跳转
- 不使用 match 的泛型函数（如 `identity_union`）不受影响

### 4.3 复杂度

- `FengAggregateDescriptor` 结构变复杂，需要文档和测试覆盖
- 嵌套 union 的链式 match 需要递归 descriptor 遍历，实现复杂度较高

---

## 5 待定事项

1. **narrowing 绑定变量**：引用还是拷贝？
2. **链式 match 语法**：`T -> InnerUnion -> LeafType` 在共享体内是否支持？
3. **else 分支语义**：当 T 的具体实例化不在 label 列表中时，是否一律走 else？
4. **call-form spec 成员**：callable-form spec 作为 union 成员时的 match + 调用如何处理？
5. **是否需要扩展 `FengGenericParamDescriptor`**：当前 `witness` 字段为 NULL（union spec 无方法），是否需要用于传递 union 特定信息？
6. **性能基准**：descriptor 间接寻址 vs 具体类型 match 的性能差异



===============================================================================
-------------------------------------------------------------------------------

当前描述符结构

现在 union spec 的 FengAggregateDescriptor 只有 ARC 所需的最少信息：

// 只有一个 slot：_fwd（运行时动态转发）
static const FengManagedSlotDescriptor FengSpecAggSlots__SimpleUnion[] = {
    { offsetof(struct FengSpecValue__SimpleUnion, _fwd), FENG_SLOT_FORWARD, NULL },
};

static const FengAggregateDescriptor FengSpecAgg__SimpleUnion = {
    .name = "SimpleUnion",
    .size = sizeof(struct FengSpecValue__SimpleUnion),
    .managed_slots = FengSpecAggSlots__SimpleUnion,
    .managed_slot_count = 1,
    ...
};

这只够做 retain/release，不够做 match。

如果加入每个成员的描述

扩展 descriptor，加入 union 成员元信息：

typedef struct FengUnionMemberDescriptor {
    uint32_t tag;                          // 该成员对应的 tag 值
    FengValueKind value_kind;              // TRIVIAL / MANAGED_POINTER / AGGREGATE
    size_t payload_offset;                 // payload 内偏移
    size_t payload_size;                   // payload 大小
    const void *nested_descriptor;         // 嵌套 descriptor（按 value_kind 转型）
    const void *witness;                   // object-form spec 成员时的 witness 表
} FengUnionMemberDescriptor;

在 FengAggregateDescriptor 中增加 union 特定字段：

typedef struct FengAggregateDescriptor {
    ... // 现有字段
    // union 扩展（仅 union-form spec 填充，其他类型为零）
    size_t union_member_count;
    const FengUnionMemberDescriptor *union_members;
} FengAggregateDescriptor;

共享体代码生成

有了这个信息，共享体内的 match 就可以工作：

func foo<T: GreetableOrString>(v: T): string {
  return match v {
    let g: Greetable { g.greet() }
    let s: string { s }
    else { "else" }
  };
}

codegen 可以生成：

// 共享体内 match
const FengAggregateDescriptor *_desc = _param_T->descriptor;
uint32_t _tag = *(uint32_t *)((char *)&v + 0);  // tag 始终在 offset 0

if (_tag == _desc->union_members[0].tag) {  // Greetable 分支
    // 从 member descriptor 取 witness
    const FengSpecWitness__Greetable *_w =
        (const FengSpecWitness__Greetable *)_desc->union_members[0].witness;
    void *_subject = *(void **)((char *)&v + _desc->union_members[0].payload_offset
                                + offsetof(FengSpecValue__Greetable, subject));
    // 通过 witness 调用方法
    FengString *_result = _w->greet(_subject);
} else if (_tag == _desc->union_members[1].tag) {  // string 分支
    FengString *_result = *(FengString **)((char *)&v + _desc->union_members[1].payload_offset);
}

需要解决的关键问题

1. tag 偏移的统一

当前所有 union spec 的 tag 都在 offset 0，这点可以依赖。但嵌套 union 的 tag 在外层 union 的 payload 内，共享体需要知道如何到达内层 tag。

方案：member descriptor 的 nested_descriptor 本身如果是 union，也有 union_members，形成递归链。

2. narrowing 后的绑定变量类型

let g: Greetable 收窄后，g 的类型在共享体中是 T 的一部分，不是完整的 FengSpecValue__Greetable。需要定义：narrowing 后的绑定变量是 payload 的直接引用（零拷贝），还是一个拷贝？

- 引用：payload 偏移 + 大小即可，零开销
- 拷贝：需要 nested_descriptor 做 retain

3. witness 表的可用性

当 label 匹配到 object-form spec 成员时，member descriptor 中有 witness。但这个 witness 是 "Greetable 作为 Greetable" 的 identity witness，不是具体类型（如 GreeterUser）的 witness。

// union_members[0].witness 是 identity witness 还是 GreeterUser 的 witness？
// 值结构中已经存了 witness 指针：payload.m0 = { subject, witness }
// 所以可以直接从 payload 中取，不需要 descriptor 提供

实际上，object-form spec 作为 union 成员时，payload 就是 { subject, witness } 对，witness 已经在值里了。descriptor 只需要知道偏移和大小。

4. 运行时开销

每个 union spec 的 descriptor 多了 union_members 数组，是编译期常量，零运行时开销。但 descriptor 体积增大了——对于不使用 match 的泛型函数（如 identity_union），这些信息是冗余的。

结论

这个方向是可行的。核心收益是：union 描述符从"只够做 ARC"升级为"够用 match + 方法调用"，泛型共享体获得了对 union 值结构的运行时内省能力。

代价：
- 扩展 FengAggregateDescriptor 结构（ABI 变更）
- 每个 union spec 多生成一个 FengUnionMemberDescriptor[] 常量数组
- codegen 共享体 match 路径需要新实现（从 descriptor 生成 tag 检查、payload 提取、witness 调用）

不需要：
- 改变值的内存布局（tag + _fwd + payload 不变）
- 改变 ARC 逻辑（walker 仍走 FENG_SLOT_FORWARD）
- 改变 witness 表结构（object-form spec 的 witness 已在 payload 中）
