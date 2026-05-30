# Feng 联合类型待开发项

> 本文档用于拆解 union-form 从规范到实现的施工顺序、边界与验收口径。
> [docs/feng-union-type.md](../docs/feng-union-type.md) 是 union-form 的专项规范；本文只写开发步骤与 TODO，不重复规范定义。

## 1. 当前前提

- union-form 语法已确定为 `spec Name: A | B | C;`，不引入新关键字，不允许 `{}` 块体。
- union-form 的成员收窄已确定复用 `if 目标值 { ... }` 条件匹配语法，不引入独立 `is`。
- union-form 的值级基线表示已确定为：一个 `tag`、一个 `inline value`、一个托管指针槽位。
- 当前阶段不支持 `union -> common spec` 投影，即使全部 member 都满足同一个 object-form `spec`。
- 首版不为 `all-trivial union` 或 `all-managed-pointer union` 再开顶层值模型特例；统一走 `aggregate-with-managed-slots`。
- **已确认**：union-form 的 if-match 类型匹配与 union-form 主体必须在同一轮交付；若 union-form 可声明但 if-match 类型匹配未落地，则 union 值在收窄前完全不可操作，不得作为独立里程碑先行完成。
- **已确认**：union-form 必须进入 `.ft` 符号表；writer、reader 与 imported-module 三段须在同一轮完成，不得延迟。
- **已确认**：union-form 可作为泛型类型参数约束（`T: UnionSpec`）；在泛型声明体内，未经 if-match 收窄的参数值不允许做成员访问、方法调用或 `==` / `!=` 比较；收窄后按 narrowed member 的规则操作；union-form 约束不产生额外的 witness 物化，与 object-form `spec` 约束不同。
- **已确认**：union-form 默认零值取归一化后第一个 member 的默认零值；若该 member 无合法默认零值，则该 union-form 亦无合法默认零值。

## 2. 分步 TODO

### 2.1 规范并入

- [ ] 将 union-form 的主定义并入 `docs/feng-spec.md`。
- [ ] 在 `docs/feng-language.md` 中补 union-form 的总览入口，只做摘要与引用，不重复细则。
- [ ] 在 `docs/feng-type.md` 中补 union-form 的类型系统位置与引用。
- [ ] 在 `docs/feng-expression.md` 中补 union-form 显式转换边界的引用关系。
- [ ] 复核 `docs/` 中涉及 `if 目标值 { ... }`、`spec`、显式转换的交叉引用，避免重复定义与冲突措辞。

验收口径：

- `docs/` 中 union-form 语义只有一个主定义来源。
- 流程控制、表达式、类型系统文档只保留协作规则与引用。

### 2.2 Parser / AST

- [ ] 扩展 `FengSpecForm`，新增 `UNION`。
- [ ] 为 union-form 增加独立成员集合承载，例如 `union_members`，不复用 object-form 的 `parent_specs` 或块体成员结构。
- [ ] 解析 `spec Name: TypeRef ('|' TypeRef)+;`。
- [ ] 拒绝 union-form 上的 `{}` 块体。
- [ ] 在语法阶段拒绝 `void` 作为 union member。
- [ ] 扩展 `FengMatchLabelKind`，新增类型 member 标签形式（区别于现有的 `VALUE` 与 `RANGE`）。
- [ ] 扩展 `FengMatchLabel`，为类型 member 标签承载对应的 `TypeRef`，而非字面量表达式。
- [ ] 在 `peek_match_body` 与 `parse_match_label_atom` 中识别类型名标签形式，并与字面量/区间标签稳定区分；禁止两种标签模式在同一 `if 目标值 { ... }` 中混用。

验收口径：

- parser 能稳定区分 object-form、callable-form、union-form。
- 非法语法给出明确诊断，不落到模糊错误。

### 2.3 成员归一化与类型记录

- [ ] 解析 union member 后，拍平嵌套 union-form。
- [ ] 去重并保持声明顺序，形成归一化成员集合。
- [ ] 记录归一化后的第一个 member，供默认零值与 codegen 使用。
- [ ] 为后续语义层和 codegen 暴露稳定的 union member 元信息。

验收口径：

- 嵌套 union 与重复 member 的行为在语义层唯一确定。
- 默认零值所依赖的首 member 可被稳定查询。

### 2.4 进入站点与显式转换语义

- [ ] 实现赋值、变量初始化、传参、返回等 union 进入站点的可接受性检查。
- [ ] 实现“精确 member 优先”的 active member 选择规则。
- [ ] 实现重叠 `spec` member 的进入冲突诊断，禁止按声明顺序兜底。
- [ ] 只允许开发者先显式转换到目标 `spec` member，再写入 union-form。
- [ ] 禁止 `union -> common spec` 投影。
- [ ] 复核 object-form `spec` 的向上转换资格与 union member 选择之间的衔接。

验收口径：

- `UserType | Named` 这类组合在精确命中时不误报冲突。
- 两个重叠 `spec` 同时接纳同一源值时，诊断稳定且不发生隐式 member 选择。

### 2.5 `if` 收窄与条件匹配

> **交付阻断依赖**：本节与 §2.2 Parser/AST 扩展、§2.3 成员归一化属于同一轮交付。若 union-form 已可声明但 if-match 类型匹配未落地，则 union 值在收窄前完全不可操作，不得作为独立里程碑先行完成。

- [ ] 扩展 `if 目标值 { ... }` 的语义分析，使其支持 union member 标签。
- [ ] 禁止在同一次 union member 匹配中混用字面量/区间标签与 union member 标签。
- [ ] 实现单 member 分支、多个 member 分支与 `else` 分支的收窄结果计算。
- [ ] 实现“匹配只检查当前 active member，不自动向上转换后再命中”的规则。
- [ ] 在未收窄到单一 member 时，禁止成员访问、方法调用与 `==` / `!=`。

验收口径：

- 分支内的收窄结果与 [docs/feng-union-type.md](../docs/feng-union-type.md) 一致。
- 对仍是 union 子集的分支，诊断明确指出还需继续收窄。

### 2.6 基于现有 runtime 的布局接入与生命周期

#### 2.6.1 C 值布局草案

每个 union-form spec 在 codegen 阶段生成一个对应的 C struct。**三个域的结构是固定的，对所有 union spec 一律如此**，不随 member 种类变化；变化的只有 struct 名称（按 spec 全限定名生成）和 `inline_v` 的宽度（见下）：

```c
/* union-form spec 的统一值布局。
 * 三个域对所有 union spec 固定存在，不随 member 类型变化；
 * "FengUnion_SpecName" 仅为示意名，codegen 按 spec 全限定名生成实际名称。 */
typedef struct {
    uint32_t  tag;          /* 固定存在。active member 在归一化列表中的序号（0-based） */
    uintptr_t inline_v;     /* 固定存在。inline payload slot；宽度见下方说明 */
    void     *managed_ptr;  /* 固定存在。托管指针槽位；active member 不需要托管引用时必须为 NULL */
} FengUnion_SpecName;
```

三个域的固定性说明：
- `tag`：绝对固定，所有 union spec 相同，类型为 `uint32_t`。
- `managed_ptr`：固定为单根 `void *`，**不随 member 种类增减**。不需要托管引用的 variant 令其为 `NULL`，aggregate walker 对 `NULL` 槽位自动 skip。
- `inline_v`：域本身固定存在，但**宽度是待决项**（见 §2.6.3 待决项 1）。首版基线为 `uintptr_t`（一个 pointer-sized word）；若某个 spec 含宽度超过 pointer 的 trivial member，codegen 可为该 spec 生成更宽的 `inline_v`，仍属于同一布局形状，不引入新的域。

**为什么 `inline_v` 不用 C `union`？**

codegen 是 `inline_v` 的唯一读写方，它在发码时已经通过语义信息（当前 active member 类型）知道应该如何强转，不需要 C union 的 named field（`.as_int`、`.as_ptr` 等）。改用 C union 会带来三个问题而没有收益：

- 每个 union spec 要生成一个不同的匿名 C union 类型，codegen 复杂度上升；
- `feng_aggregate_retain` / `feng_aggregate_release` 等 walker 完全不感知 `inline_v` 内部——trivial 字节对它透明，用 C union 还是 `uintptr_t` 对 runtime 无差别；
- object-form spec 的 `witness` 是 `const void *` 转 `uintptr_t` 存入，若 `inline_v` 同时含 `int32_t`、`bool` 等 named field，语义混杂，不如单一 word 直接强转清晰。

#### 2.6.2 各 member 种类的 slot 分配策略

| member 种类 | `inline_v` 存什么 | `managed_ptr` 存什么 |
|---|---|---|
| trivial（`int`、`bool` 等，≤ pointer size） | 值本身 | `NULL` |
| managed-pointer（用户定义托管对象） | 不用（`0`） | 对象的托管指针 |
| object-form `spec` fat value（`{subject, witness}`） | `witness`（const 静态指针，trivial）| `subject`（托管对象指针）|
| aggregate member（嵌套 aggregate） | 不用（`0`） | 首版：装箱后的托管指针 |

说明：
- object-form `spec` 的 fat value 恰好能拆入两个 slot：`subject` 是托管引用，进 `managed_ptr`；`witness` 是 const 静态指针（trivial），进 `inline_v`。无需额外装箱。
- aggregate member 首版走装箱路径，managed_ptr 指向堆上分配的 aggregate 对象；后续若有内联优化需求，作为独立专项处理。
- 切换 active member 时，必须先 release 旧 `managed_ptr` 并清零，再写入新值；`inline_v` 亦需整体覆盖，不得留有上一 member 的残余。

**关键不变量**：`managed_ptr` 在 active member 不需要托管引用时必须为 `NULL`。现有 aggregate walker 对 `NULL` 槽位自动 skip（retain / release 均为 no-op），因此不需要为 union-form 特殊改动 `src/runtime/` 的 walker 实现。

#### 2.6.3 描述符草案

每个 union-form spec 在 codegen 阶段生成一个静态 `FengAggregateDescriptor`，复用现有 aggregate 通用路径：

```c
/* managed_ptr 槽位描述（偏移由 codegen 按 struct 布局确定） */
static const FengManagedSlotDescriptor kUnionSpecName_slots[] = {
    { .offset = offsetof(FengUnion_SpecName, managed_ptr),
      .kind   = FENG_SLOT_POINTER,
      .nested = NULL },
};

/* union-form spec 描述符 */
static const FengAggregateDescriptor kUnionSpecName_desc = {
    .name               = "pkg.SpecName",           /* debug only */
    .size               = sizeof(FengUnion_SpecName),
    .default_init       = &kUnionSpecName_default_init, /* 见下 */
    .managed_slot_count = 1,
    .managed_slots      = kUnionSpecName_slots,
    .equal_fn           = NULL,  /* union 不直接支持相等性，必须先收窄 */
};
```

`default_init` 策略取决于归一化后第一个 member 的类型：
- 第一个 member 为 trivial → `FENG_DEFAULT_ZERO_BYTES`（`tag=0`，`inline_v=0`，`managed_ptr=NULL` 即为合法零值）。
- 第一个 member 需要托管引用 → `FENG_DEFAULT_INIT_FN`，init_fn 负责正确构造该 member 的零值并写入对应 slot。

**待决项**（供 Review 决策）：
1. `inline_v` 宽度是否首版固定 `uintptr_t`，还是按 spec 各自生成精确宽度？固定宽度实现更简单，但对宽度不足的 trivial member（如某些扩展整数类型）需要额外处理。
2. aggregate member 首版是否强制装箱？若 aggregate 本身 size 较小，可在 inline 部分直接嵌入而非装箱，但这要求 `inline_v` 宽度灵活。首版建议：先强制装箱，后续按需优化。
3. 是否复用 `FengAggregateDescriptor` 还是新增 `FengUnionDescriptor`？现阶段 `FengAggregateDescriptor` 已可完整描述 union 布局与生命周期，建议首版直接复用，不新增类型。

#### 2.6.4 TODO

- [ ] 在 codegen / descriptor 层固定 union-form 的值布局：一个 `tag`、一个 `inline value`、一个托管指针槽位。
- [ ] 定义 `tag` 到 active member 的映射与必要的描述符元信息。
- [ ] 定义默认零值时如何初始化 `tag`、`inline value` 与托管指针槽位，并决定走 `FENG_DEFAULT_ZERO_BYTES` 还是 `FENG_DEFAULT_INIT_FN`。
- [ ] 明确未使用托管指针槽位的 variant 在运行时始终视为空。
- [ ] 复用现有 `feng_aggregate_retain`、`feng_aggregate_release`、`feng_aggregate_assign`、`feng_aggregate_take` 与 `feng_aggregate_default_init` 完成 union 生命周期，不新增 runtime 通用 API。
- [ ] 验证现有 aggregate walker 在该布局下即可正确处理复制、销毁与托管扫描。
- [ ] 验证该实现不引入第四类顶层值模型，也不需要改动 `src/runtime/` 的通用实现。

验收口径：

- 同一 union 值在构造、复制、销毁、扫描路径上的行为一致。
- runtime 不需要重新搜索“当前对象还满足哪些 spec”。
- `src/runtime/` 无需为 union-form 新增专用生命周期分支或新 API。

### 2.7 Codegen

- [ ] 为 union 进入站点发出固定布局构造代码。
- [ ] 为 `if` 收窄发出 `tag` 判别与分支内直接访问代码。
- [ ] 为默认零值发出“首个归一化 member 的零值”构造代码。
- [ ] 处理 object-form `spec` 作为 union member 时的 payload / witness 发码。
- [ ] 明确拒绝当前阶段不支持的 `union -> common spec` 投影路径。

验收口径：

- 分支内访问成本收敛为“一次收窄 + 具体 member 直接访问”。
- 不产生多余的运行时搜索、候选比较或回退逻辑。

### 2.8 `.ft` 符号表接入

- [ ] 在 `src/symbol/ft_internal.h` 中新增 `FENG_SYMBOL_FT_TYPE_KIND_SPEC_UNION = 10U` 常量。
- [ ] 在 `src/symbol/ft_write.c` 中为 union-form spec 序列化 TYPS（`SPEC_UNION` kind）和 TSEQ（各归一化 member 类型的顺序列表，每个元素 `name_str = 0`）。
- [ ] 在 `src/symbol/ft_read.c` 中增加 `SPEC_UNION` 读取路径，把 member 类型从 TSEQ 读出并恢复到 `FengSymbolDeclView`。
- [ ] 在 `src/symbol/export.c` 中让 spec decl 导出时能区分 object / callable / union 三种 form，union-form 的 member list 正确写入 TSEQ。
- [ ] 在 `src/symbol/imported_module.c` 中从 `.ft` 恢复 union-form spec 时，保留 member 列表并正确标记 spec form。
- [ ] 验证 union-form spec 在跨包场景下可被 consumer 作为泛型约束识别与消费。

验收口径：

- union-form spec 的 round-trip（write → read → imported-module 恢复）测试通过。
- 跨包 consumer 能识别 union-form、读取 member 列表，并在泛型约束场景下正确拒绝未收窄操作。

### 2.9 诊断与测试

- [ ] 补 parser 用例：合法语法、非法块体、非法 `void` member、嵌套 union。
- [ ] 补 semantic 用例：member 归一化、默认零值、进入冲突、显式转换、未收窄访问/比较报错。
- [ ] 补 codegen / runtime 用例：`tag` 判别、零值构造、复制、销毁、托管扫描。
- [ ] 补 smoke 用例：基础 union、`spec` member、多个 member 分支、`else` 收窄。
- [ ] 每轮落地后执行全量回归。

验收口径：

- 新增测试能覆盖语法、语义、codegen、runtime 四层。
- `make test` 全量通过。

## 3. 当前明确不做

- [ ] 不在当前阶段支持 `union -> common spec` 投影。
- [ ] 不在当前阶段为 `all-trivial union` 或 `all-managed-pointer union` 单独做另一套顶层值分类特化。
- [ ] 不在当前阶段把 C ABI 兼容面作为 union-form 的前置约束。

## 4. 建议执行顺序

1. 先更新文档（`dev/`、`docs/`），把已确认的 4 条边界写实后再动代码。
2. 补 parser / AST：扩展 `FengSpecForm` + union member 存储 + if-match 类型 member 标签承载，三者**必须同一 PR/步骤** 完成（相互依赖）。
3. 补语义层成员归一化、进入站点与 if-match 类型收窄（同一步骤，不可分离）。
4. 补泛型约束支持：union-form 可作为约束、收窄前禁止操作。
5. 补 `.ft` 符号表接入：writer、reader、imported-module 三段同步完成。
6. 补 codegen 与 runtime 布局接入。
7. 补测试与回归，覆盖 parser / semantic / symbol / codegen / smoke 四层，执行全量回归。

## 5. 交付约束

- 每一阶段先改 `docs/` 或 `dev/` 中对应规范，再落代码，最后补测试。
- 所有实现必须遵守已确认的固定布局与语义边界，不得在编码阶段临时改语义。
- 若后续要放开 `union -> common spec` 投影或 runtime 特化，必须先更新规范，再进入实现。
