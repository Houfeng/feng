# Feng 联合类型待开发项

> 本文档用于拆解 union-form 从规范到实现的施工顺序、边界与验收口径。
> [docs/feng-union-type.md](../docs/feng-union-type.md) 是 union-form 的专项规范；本文只写开发步骤与 TODO，不重复规范定义。

## 1. 当前前提

- union-form 语法已确定为 `spec Name: A | B | C;`，不引入新关键字，不允许 `{}` 块体。
- union-form 的成员收窄已确定复用 `if 目标值 { ... }` 条件匹配语法，不引入独立 `is`。
- union-form 的值级基线表示已确定为：一个 `tag`、一个运行时转发槽 `_fwd`、一个 inline payload 区域。
- 当前阶段不支持 `union -> common spec` 投影，即使全部 member 都满足同一个 object-form `spec`。
- 首版不为 `all-trivial union` 或 `all-managed-pointer union` 再开顶层值模型特例；统一走 `aggregate-with-managed-slots`。
- **已确认**：union-form 的 if-match 类型匹配与 union-form 主体必须在同一轮交付；若 union-form 可声明但 if-match 类型匹配未落地，则 union 值在收窄前完全不可操作，不得作为独立里程碑先行完成。
- **已确认**：union-form 必须进入 `.ft` 符号表；writer、reader 与 imported-module 三段须在同一轮完成，不得延迟。
- **已确认**：union-form 可作为泛型类型参数约束（`T: UnionSpec`）；在泛型声明体内，未经 if-match 收窄的参数值不允许做成员访问、方法调用或 `==` / `!=` 比较；收窄后按 narrowed member 的规则操作；union-form 约束不产生额外的 witness 物化，与 object-form `spec` 约束不同。
- **已确认**：union-form 默认零值取归一化后第一个 member 的默认零值；若该 member 无合法默认零值，则该 union-form 亦无合法默认零值。
- **已确认**：当 `if` 目标表达式静态类型为 union-form 时，整个 match body 只能使用 union member 类型匹配与 `else`，不得混入字面量值匹配或区间匹配。

## 2. 分步 TODO

### 2.1 规范并入

- [x] 将 union-form 的主定义并入 `docs/feng-spec.md`。
- [x] 在 `docs/feng-language.md` 中补 union-form 的总览入口，只做摘要与引用，不重复细则。
- [x] 在 `docs/feng-type.md` 中补 union-form 的类型系统位置与引用。
- [x] 在 `docs/feng-expression.md` 中补 union-form 显式转换边界的引用关系。
- [x] 复核 `docs/` 中涉及 `if 目标值 { ... }`、`spec`、显式转换的交叉引用，避免重复定义与冲突措辞。

验收口径：

- `docs/` 中 union-form 语义只有一个主定义来源。
- 流程控制、表达式、类型系统文档只保留协作规则与引用。

### 2.2 Parser / AST

- [x] 扩展 `FengSpecForm`，新增 `UNION`。
- [x] 为 union-form 增加独立成员集合承载，例如 `union_members`，不复用 object-form 的 `parent_specs` 或块体成员结构。
- [x] 解析 `spec Name: TypeRef ('|' TypeRef)+;`。
- [x] 拒绝 union-form 上的 `{}` 块体。
- [x] 在语法阶段拒绝 `void` 作为 union member。
- [x] 扩展 `FengMatchLabelKind`，新增类型 member 标签形式（区别于现有的 `VALUE` 与 `RANGE`）。
- [x] 扩展 `FengMatchLabel`，为类型 member 标签承载对应的 `TypeRef`，而非字面量表达式。
- [x] 在 `peek_match_body` 与 `parse_match_label_atom` 中识别类型名标签候选，并保留足够 AST 信息供语义层按目标类型选择 union type match 或普通 value/range match。

验收口径：

- parser 能稳定区分 object-form、callable-form、union-form。
- 非法语法给出明确诊断，不落到模糊错误。

### 2.3 成员归一化与类型记录

- [x] 解析 union member 后，拍平嵌套 union-form。
- [x] 去重并保持声明顺序，形成归一化成员集合。
- [x] 记录归一化后的第一个 member，供默认零值与 codegen 使用。
- [x] 为后续语义层和 codegen 暴露稳定的 union member 元信息。

验收口径：

- 嵌套 union 与重复 member 的行为在语义层唯一确定。
- 默认零值所依赖的首 member 可被稳定查询。

### 2.4 进入站点与显式转换语义

- [x] 实现赋值、变量初始化、传参、返回等 union 进入站点的可接受性检查。
- [x] 实现“精确 member 优先”的 active member 选择规则。
- [x] 实现重叠 `spec` member 的进入冲突诊断，禁止按声明顺序兜底。
- [x] 只允许开发者先显式转换到目标 `spec` member，再写入 union-form。
- [x] 禁止 `union -> common spec` 投影。
- [x] 复核 object-form `spec` 的向上转换资格与 union member 选择之间的衔接。

验收口径：

- `UserType | Named` 这类组合在精确命中时不误报冲突。
- 两个重叠 `spec` 同时接纳同一源值时，诊断稳定且不发生隐式 member 选择。

### 2.5 `if` 收窄与条件匹配

> **交付阻断依赖**：本节与 §2.2 Parser/AST 扩展、§2.3 成员归一化属于同一轮交付。若 union-form 已可声明但 if-match 类型匹配未落地，则 union 值在收窄前完全不可操作，不得作为独立里程碑先行完成。

- [x] 扩展 `if 目标值 { ... }` 的语义分析，使其支持 union member 标签。
- [x] 当 `if` 目标表达式静态类型为 union-form 时，强制整个 match body 进入 union member 类型匹配模式，只允许 union member 类型标签与 `else`。
- [x] 在 union member 类型匹配模式中拒绝字面量值标签与区间标签；value/range match 仅保留给非 union 目标值。
- [x] 实现单 member 分支、多个 member 分支与 `else` 分支的收窄结果计算。
- [x] 实现“匹配只检查当前 active member，不自动向上转换后再命中”的规则。
- [x] 在未收窄到单一 member 时，禁止成员访问、方法调用与 `==` / `!=`。

验收口径：

- 分支内的收窄结果与 [docs/feng-union-type.md](../docs/feng-union-type.md) 一致。
- 对仍是 union 子集的分支，诊断明确指出还需继续收窄。

### 2.6 基于现有 runtime 的布局接入与生命周期

#### 2.6.1 C 值布局草案

每个 union-form spec 在 codegen 阶段生成一个对应的 C struct。首版布局固定为 `tag + _fwd + payload`；变化的只有 struct 名称和 payload 宽度/对齐，均由归一化 member 集合决定：

```c
typedef struct {
    uint32_t tag;                    /* active member 在归一化列表中的 0-based 序号 */
    FengManagedSlotDescriptor _fwd;  /* 当前 active payload 的生命周期转发描述 */
    union {
        /* codegen 按归一化 member 生成足够容纳各 member 的 inline payload */
    } payload;
} FengUnion_SpecName;
```

布局不变量：
- `tag` 是语言层 active member identity；if-match 的类型分支只依据 `tag` 判别。
- `_fwd` 是 runtime 生命周期描述，不参与语言层 member 判别；它只告诉 aggregate walker 当前应如何 retain / release / assign / take / 扫描 payload。
- `_fwd.offset` 始终相对当前 union aggregate 值的基地址，通常指向 `payload` 内当前 active member 的起始偏移。
- payload 必须 inline 容纳归一化 member 中最大的值表示；aggregate member 首版不装箱。
- 切换 active member 时，必须先按旧 `_fwd` 释放旧 payload，再覆盖 `tag`、`_fwd` 与新 payload；不得遗留上一 member 的托管槽位状态。

#### 2.6.2 各 member 种类的 `_fwd` 策略

| member 种类 | payload 存什么 | `_fwd` 写什么 |
|---|---|---|
| trivial（`int`、`bool` 等） | 值本身 | `{ .offset = payload_offset, .kind = FENG_SLOT_NONE, .nested = NULL }` |
| managed-pointer（用户定义托管对象） | 对象托管指针 | `{ .offset = payload_offset, .kind = FENG_SLOT_POINTER, .nested = NULL }` |
| object-form `spec` fat value（`{subject, witness}`） | spec fat value 的完整 inline 表示 | 指向该 spec fat value 内部 managed slot 的描述，通常为 `FENG_SLOT_NESTED_AGGREGATE` |
| aggregate member（tuple / inline aggregate 等） | aggregate 的完整 inline 表示 | `{ .offset = payload_offset, .kind = FENG_SLOT_NESTED_AGGREGATE, .nested = &member_desc }` |

关键点：
- `_fwd.kind = FENG_SLOT_NONE` 表示当前 payload 没有需要 runtime 管理的托管槽位。
- `_fwd.kind = FENG_SLOT_POINTER` 表示 payload 起始处是一根托管指针。
- `_fwd.kind = FENG_SLOT_NESTED_AGGREGATE` 表示 payload 起始处是一个可由 `nested` 描述的 aggregate 值。
- runtime 已支持 `FENG_SLOT_FORWARD`，union 自身的静态 aggregate descriptor 只需包含一个指向 `_fwd` 字段的 `FENG_SLOT_FORWARD` 槽位。
- 不允许 `_fwd.kind` 再写成 `FENG_SLOT_FORWARD`；嵌套 forward 是 runtime 防御性 panic 场景。

#### 2.6.3 描述符草案

每个 union-form spec 在 codegen 阶段生成一个静态 `FengAggregateDescriptor`，复用现有 aggregate 通用路径：

```c
static const FengManagedSlotDescriptor kUnionSpecName_slots[] = {
    { .offset = offsetof(FengUnion_SpecName, _fwd),
      .kind = FENG_SLOT_FORWARD,
      .nested = NULL },
};

static const FengAggregateDescriptor kUnionSpecName_desc = {
    .name = "pkg.SpecName",
    .size = sizeof(FengUnion_SpecName),
    .default_init = &kUnionSpecName_default_init,
    .managed_slot_count = 1,
    .managed_slots = kUnionSpecName_slots,
    .equal_fn = NULL,
};
```

`default_init` 策略取决于归一化后第一个 member 的类型：
- `tag` 固定初始化为 `0`。
- payload 按第一个 member 的默认零值规则初始化。
- `_fwd` 按第一个 member 的生命周期分类写入 `NONE` / `POINTER` / `NESTED_AGGREGATE`。
- 若第一个 member 无合法默认零值，则该 union-form 也无合法默认零值。

#### 2.6.4 方案讨论结论

- 不能只保留 `_fwd + payload` 而省略 `tag`。`_fwd` 只能表达生命周期路径，无法区分两个同生命周期分类但语义 member 不同的 active variant。
- 不采用固定单根 `managed_ptr`。该方案会迫使 aggregate member 装箱，破坏 inline 值模型，也会把 object-form `spec` 与 aggregate payload 拆成特例。
- 不新增 `FengUnionDescriptor`。现有 `FengAggregateDescriptor + FENG_SLOT_FORWARD` 已能覆盖生命周期；union member 列表、tag 映射和收窄规则由 semantic/codegen 元数据负责。
- value/range match 与 union type match 在语义上必须分离。union 目标未收窄前没有统一值比较入口，因此 union target 的 match body 只能写 member type label 与 `else`。
- 异常 cleanup 链需要支持“按 aggregate descriptor 清理一个 by-value aggregate”这一通用能力。原因是 union-form 的 active payload 由 `_fwd` 动态决定，不能在编译期展开成固定 `void **` 指针槽；该扩展面向所有 aggregate local，不新增 union 专用 runtime 分支，也不改变三类顶层值模型。
- cleanup 链中的 frame marker 必须同时满足 `slot == NULL` 且 `aggregate_desc == NULL`；aggregate cleanup node 的 `slot` 也为 `NULL`，但其 `aggregate_desc` 非空，释放到 frame marker 时不得被误判为 marker。

#### 2.6.5 TODO

- [x] 在 codegen / descriptor 层固定 union-form 的值布局：一个 `tag`、一个 `_fwd`、一个 inline payload 区域。
- [x] 定义 `tag` 到 active member 的映射与必要的描述符元信息。
- [x] 定义默认零值时如何初始化 `tag`、`_fwd` 与 payload。
- [x] 明确无托管槽位的 variant 在运行时写入 `FENG_SLOT_NONE`。
- [x] 复用 `feng_aggregate_retain`、`feng_aggregate_release`、`feng_aggregate_assign`、`feng_aggregate_take` 与 `feng_aggregate_default_init` 完成 union 生命周期；异常路径通过通用 aggregate cleanup 节点调用同一 descriptor，不新增 union 专用 API。
- [x] 验证 aggregate walker 经由 `FENG_SLOT_FORWARD` 在该布局下即可正确处理复制、销毁与托管扫描。
- [x] 验证该实现不引入第四类顶层值模型，也不需要在 `src/runtime/` 中新增 union 专用生命周期分支。

验收口径：

- 同一 union 值在构造、复制、销毁、扫描路径上的行为一致。
- runtime 不需要重新搜索“当前对象还满足哪些 spec”。
- `src/runtime/` 无需为 union-form 新增专用生命周期分支；通用 cleanup 链扩展只依赖 `FengAggregateDescriptor`。

### 2.7 Codegen

- [x] 为 union 进入站点发出固定布局构造代码。
- [x] 为 `if` 收窄发出 `tag` 判别与分支内直接访问代码。
- [x] 为默认零值发出“首个归一化 member 的零值”构造代码。
- [x] 处理 object-form `spec` 作为 union member 时的 payload / witness 发码。
- [x] 明确拒绝当前阶段不支持的 `union -> common spec` 投影路径。

验收口径：

- 分支内访问成本收敛为“一次收窄 + 具体 member 直接访问”。
- 不产生多余的运行时搜索、候选比较或回退逻辑。

### 2.8 `.ft` 符号表接入

- [x] 在 `src/symbol/ft_internal.h` 中新增 `FENG_SYMBOL_FT_TYPE_KIND_SPEC_UNION = 10U` 常量。
- [x] 在 `src/symbol/ft_write.c` 中为 union-form spec 序列化 TYPS（`SPEC_UNION` kind）和 TSEQ（各归一化 member 类型的顺序列表，每个元素 `name_str = 0`）。
- [x] 在 `src/symbol/ft_read.c` 中增加 `SPEC_UNION` 读取路径，把 member 类型从 TSEQ 读出并恢复到 `FengSymbolDeclView`。
- [x] 在 `src/symbol/export.c` 中让 spec decl 导出时能区分 object / callable / union 三种 form，union-form 的 member list 正确写入 TSEQ。
- [x] 在 `src/symbol/imported_module.c` 中从 `.ft` 恢复 union-form spec 时，保留 member 列表并正确标记 spec form。
- [x] 验证 union-form spec 在跨包场景下可被 consumer 作为泛型约束识别与消费。

验收口径：

- union-form spec 的 round-trip（write → read → imported-module 恢复）测试通过。
- 跨包 consumer 能识别 union-form、读取 member 列表，并在泛型约束场景下正确拒绝未收窄操作。

### 2.9 诊断与测试

- [x] 补 parser 用例：合法语法、非法块体、非法 `void` member、嵌套 union。
- [x] 补 semantic 用例：member 归一化、默认零值、进入冲突、显式转换、未收窄访问/比较报错。
- [x] 补 codegen / runtime 用例：`tag` 判别、零值构造、复制、销毁、托管扫描。
- [x] 补 smoke 用例：基础 union、`spec` member、多个 member 分支、`else` 收窄。
- [x] 每轮落地后执行全量回归。

验收口径：

- 新增测试能覆盖语法、语义、codegen、runtime 四层。
- `make test` 全量通过。

### 2.10 泛型 union-form 成员实例化与收窄

- [ ] 在语义层保留 `FengUnionSpecInfo` 的声明态 member（允许 `T` 这类 type param 引用），并在 usage site 基于具体 type args 做成员替换后再匹配。
- [ ] 保持无泛型 union-form 的既有匹配路径不变；仅在目标 union spec 含 type param 且调用点提供 type args 时启用替换。
- [ ] 在 union `if` 类型标签匹配路径中应用同一替换规则，确保 `Result<int>` 可用 `int` 标签命中而非 `T`。
- [ ] 在单 member 收窄时写入替换后的具体类型，避免分支内局部变量仍保留为未替换 type param。
- [ ] 在 codegen 的 generic spec instance 路径中为 union-form 构建具体 `union_member_types`，保证 `Result<int>` 与 `Result<string>` 各自持有具体 member 映射。

验收口径：

- `spec Result<T>: Error | T;` 在 `Result<int>`、`Result<string>` 场景下的赋值、参数匹配、`if` 收窄均按具体类型工作。
- 无泛型 union-form 行为与现有测试结果保持一致。
- 全量回归通过。

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
