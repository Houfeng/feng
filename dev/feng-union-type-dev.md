# Feng 联合类型待开发项

> 本文档用于拆解 union-form 从规范到实现的施工顺序、边界与验收口径。
> [docs/feng-union-type.md](../docs/feng-union-type.md) 是 union-form 的专项规范；本文只写开发步骤与 TODO，不重复规范定义。

## 1. 当前前提

- union-form 语法已确定为 `spec Name: A | B | C;`，不引入新关键字，不允许 `{}` 块体。
- union-form 的成员收窄已确定复用 `if 目标值 { ... }` 条件匹配语法，不引入独立 `is`。
- union-form 的值级基线表示已确定为：一个 `tag`、一个 `inline value`、一个托管指针槽位。
- 当前阶段不支持 `union -> common spec` 投影，即使全部 member 都满足同一个 object-form `spec`。
- 首版不为 `all-trivial union` 或 `all-managed-pointer union` 再开顶层值模型特例；统一走 `aggregate-with-managed-slots`。

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

- [ ] 扩展 `if 目标值 { ... }` 的语义分析，使其支持 union member 标签。
- [ ] 禁止在同一次 union member 匹配中混用字面量/区间标签与 union member 标签。
- [ ] 实现单 member 分支、多个 member 分支与 `else` 分支的收窄结果计算。
- [ ] 实现“匹配只检查当前 active member，不自动向上转换后再命中”的规则。
- [ ] 在未收窄到单一 member 时，禁止成员访问、方法调用与 `==` / `!=`。

验收口径：

- 分支内的收窄结果与 [docs/feng-union-type.md](../docs/feng-union-type.md) 一致。
- 对仍是 union 子集的分支，诊断明确指出还需继续收窄。

### 2.6 基于现有 runtime 的布局接入与生命周期

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

### 2.8 诊断与测试

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

1. 先补 parser / AST 与成员归一化。
2. 再补语义层的进入站点、收窄与诊断。
3. 再落基于现有 runtime 的固定布局接入与 codegen。
4. 最后补齐测试、回归与其余主规范并入。

## 5. 交付约束

- 每一阶段先改 `docs/` 或 `dev/` 中对应规范，再落代码，最后补测试。
- 所有实现必须遵守已确认的固定布局与语义边界，不得在编码阶段临时改语义。
- 若后续要放开 `union -> common spec` 投影或 runtime 特化，必须先更新规范，再进入实现。
