# Feng 联合类型待开发项

> 本文档用于拆解 union-form 从规范到实现的施工顺序、边界与验收口径。
> [docs/specifications/feng-union-type.md](../specifications/feng-union-type.md) 是 union-form 的专项规范；本文只写开发步骤与 TODO，不重复规范定义。

## 1. 当前前提

- union-form 语法已确定为 `spec Name: A | B | C;`，不引入新关键字，不允许 `{}` 块体。
- union-form 的成员收窄已确定复用 `match 目标值 { ... }` 条件匹配语法，不引入独立 `is`。
- union-form 的值级布局及静态生命周期表统一见[联合主规范 §4.4](../specifications/feng-union-type.md#44-union-form-的运行时基线表示)。
- 当前阶段不支持 `union -> common spec` 投影，即使全部 member 都满足同一个 object-form `spec`。
- 首版不为 `all-trivial union` 或 `all-managed-pointer union` 再开顶层值模型特例；统一走 `aggregate-with-managed-slots`。
- **已确认**：union-form 的 match 类型匹配与 union-form 主体必须在同一轮交付；若 union-form 可声明但 match 类型匹配未落地，则 union 值在收窄前完全不可操作，不得作为独立里程碑先行完成。
- **已确认**：union-form 必须进入 `.ft` 符号表；writer、reader 与 imported-module 三段须在同一轮完成，不得延迟。
- **已确认**：union-form 可作为泛型类型参数约束（`T: UnionSpec`）；在泛型声明体内，未经 match 收窄的参数值不允许做成员访问、方法调用或 `==` / `!=` 比较；收窄后按 narrowed member 的规则操作；union-form 约束不产生额外的 witness 物化，与 object-form `spec` 约束不同。
- **已确认**：union-form 默认零值取归一化后第一个 member 的默认零值；若该 member 无合法默认零值，则该 union-form 亦无合法默认零值。
- **已确认**：当 `match` 目标表达式静态类型为 union-form 时，整个 match body 只能使用 union member 类型匹配与 `else`，不得混入字面量值匹配或区间匹配。

## 2. 分步 TODO

### 2.1 规范并入

- [x] 将 union-form 的主定义并入 `docs/specifications/feng-spec.md`。
- [x] 在 `docs/specifications/feng-language.md` 中补 union-form 的总览入口，只做摘要与引用，不重复细则。
- [x] 在 `docs/specifications/feng-type.md` 中补 union-form 的类型系统位置与引用。
- [x] 在 `docs/specifications/feng-expression.md` 中补 union-form 显式转换边界的引用关系。
- [x] 复核 `docs/` 中涉及 `match 目标值 { ... }`、`spec`、显式转换的交叉引用，避免重复定义与冲突措辞。

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
- [x] 旧版重叠 `spec` member 进入检查已实施；现行路径选择以联合主规范 §3.8.1 为准，
  后续迁移与验收见 G24 ISSUE-G24-018。
- [x] active member 选定后，先复用既有转换生成叶子成员值，再组装 union-form payload。
- [x] 禁止 `union -> common spec` 投影。
- [x] 复核 object-form `spec` 的向上转换资格与 union member 选择之间的衔接。

验收口径：

- `UserType | Named` 这类组合在精确命中时不误报冲突。
- 两个重叠 `spec` 同时接纳同一源值时，诊断稳定且不发生隐式 member 选择。

### 2.5 `match` 收窄与条件匹配

> **交付阻断依赖**：本节与 §2.2 Parser/AST 扩展、§2.3 成员归一化属于同一轮交付。若 union-form 已可声明但 match 类型匹配未落地，则 union 值在收窄前完全不可操作，不得作为独立里程碑先行完成。

- [x] 扩展 `match 目标值 { ... }` 的语义分析，使其支持 union member 标签。
- [x] 当 `match` 目标表达式静态类型为 union-form 时，强制整个 match body 进入 union member 类型匹配模式，只允许 union member 类型标签与 `else`。
- [x] 在 union member 类型匹配模式中拒绝字面量值标签与区间标签；value/range match 仅保留给非 union 目标值。
- [x] 实现单 member 分支、多个 member 分支与 `else` 分支的收窄结果计算。
- [x] 实现“匹配只检查当前 active member，不自动向上转换后再命中”的规则。
- [x] 在未收窄到单一 member 时，禁止成员访问、方法调用与 `==` / `!=`。

验收口径：

- 分支内的收窄结果与 [docs/specifications/feng-union-type.md](../specifications/feng-union-type.md) 一致。
- 对仍是 union 子集的分支，诊断明确指出还需继续收窄。

### 2.6 基于现有 runtime 的布局接入与生命周期

布局、成员表与默认值的唯一规则见[联合主规范 §4.4](../specifications/feng-union-type.md#44-union-form-的运行时基线表示)。
通用按值选择机制见[值模型说明 §3.2.1](feng-value-model-delivered.md#321-按值选择活动-slot)。
从实例转发槽迁移到静态成员表的施工及验证状态见[布局优化方案](feng-union-layout-performance-optimize.md)。

已交付的语言行为继续保持：

- 构造、复制、移动、销毁与扫描均只处理当前 active member。
- 保留既有 aggregate API；异常 cleanup 节点通过相同描述符清理，不新增 union 专用入口。
- cleanup frame marker 同时满足 `slot == NULL` 且 `aggregate_desc == NULL`，不能将 aggregate 节点误判为 marker。
- 共享泛型函数体与跨包调用使用具化点的具体描述符；`match` 的成员身份仍由 tag 决定。
- runtime 不搜索对象满足的 spec，不增加第四类顶层值模型。

### 2.7 Codegen

- [x] 为 union 进入站点发出固定布局构造代码。
- [x] 为 `match` 收窄发出 `tag` 判别与分支内直接访问代码。
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
- [ ] 在 union `match` 类型标签匹配路径中应用同一替换规则，确保 `Result<int>` 可用 `int` 标签命中而非 `T`。
- [ ] 在单 member 收窄时写入替换后的具体类型，避免分支内局部变量仍保留为未替换 type param。
- [ ] 在 codegen 的 generic spec instance 路径中为 union-form 构建具体 `union_member_types`，保证 `Result<int>` 与 `Result<string>` 各自持有具体 member 映射。

验收口径：

- `spec Result<T>: Error | T;` 在 `Result<int>`、`Result<string>` 场景下的赋值、参数匹配、`match` 收窄均按具体类型工作。
- 无泛型 union-form 行为与现有测试结果保持一致。
- 全量回归通过。

## 3. 当前明确不做

- [ ] 不在当前阶段支持 `union -> common spec` 投影。
- [ ] 不在当前阶段为 `all-trivial union` 或 `all-managed-pointer union` 单独做另一套顶层值分类特化。
- [ ] 不在当前阶段把 C ABI 兼容面作为 union-form 的前置约束。

## 4. 建议执行顺序

1. 先更新文档（`dev/`、`docs/`），把已确认的 4 条边界写实后再动代码。
2. 补 parser / AST：扩展 `FengSpecForm` + union member 存储 + match 类型 member 标签承载，三者**必须同一 PR/步骤** 完成（相互依赖）。
3. 补语义层成员归一化、进入站点与 match 类型收窄（同一步骤，不可分离）。
4. 补泛型约束支持：union-form 可作为约束、收窄前禁止操作。
5. 补 `.ft` 符号表接入：writer、reader、imported-module 三段同步完成。
6. 补 codegen 与 runtime 布局接入。
7. 补测试与回归，覆盖 parser / semantic / symbol / codegen / smoke 四层，执行全量回归。

## 5. 交付约束

- 每一阶段先改 `docs/` 或 `dev/` 中对应规范，再落代码，最后补测试。
- 所有实现必须遵守已确认的固定布局与语义边界，不得在编码阶段临时改语义。
- 若后续要放开 `union -> common spec` 投影或 runtime 特化，必须先更新规范，再进入实现。
