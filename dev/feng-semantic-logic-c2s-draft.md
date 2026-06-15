# 发码模块语义逻辑向语义模块迁移方案

> 状态：待 Review  
> 日期：2026-06-15  
> 关联文档：[CE 错误码分段归类](../docs/feng-error-codes-ce.md)

## 1. 背景

当前 CE（发码/codegen）模块承担了 **154 个**本应由 AE（语义分析）阶段拦截的校验职责。这些校验分布在 `src/codegen/codegen.c`（33048 行）的各发码函数中，以 `cg_fail(cg, tok, "CExxxx", "...")` 形式触发。

### 1.1 问题

- **诊断滞后**：错误在发码阶段才暴露，用户拿到的报错上下文不如语义阶段精确
- **职责混乱**：codegen 应在"所有语义约束已满足"的前提下安心发码，而不是重复做语义校验
- **维护负担**：语义规则分散在 `analyzer.c`（23423 行）和 `codegen.c` 两处，同一类检查可能两端都有变体

### 1.2 现状

| 模块 | 当前错误码数 | 主要职责 |
|---|---|---|
| AE（`src/semantic/`） | 183 | 类型推断、符号解析、spec 满足性 |
| CE（`src/codegen/`） | 373 | C 代码发射，含 154 个语义校验 |

迁移完成后预期：

| 模块 | 预期错误码数 |
|---|---|
| AE | ~337 |
| CE | ~219 |

## 2. 迁移原则

1. **只搬不改**：校验逻辑从 codegen 搬到 semantic，判断条件和错误语义保持不变，仅适配 AE 的数据结构
2. **AE 先加后删**：先在 AE 中补上校验，确认测试通过后再从 codegen 中移除旧检查
3. **逐批回归**：每批迁移完成后跑全量回归测试，确保无回归
4. **错误码不变**：迁移的错误码仍使用原 CE 编号，待后续统一替换时再改为 AE 编号

## 3. 模块结构

### 3.1 AE（语义分析）

```
src/semantic/
├── analyzer.c              # 主分析器：resolve_expr / resolve_block / resolve_stmt
├── spec_coercion_sites.c   # spec coercion 站点收集
├── spec_witnesses.c        # spec witness 生成
├── spec_equalities.c       # spec 相等性
├── spec_relations.c        # spec 继承关系
├── spec_member_accesses.c  # spec 成员访问
├── spec_default_bindings.c # spec 默认绑定
├── type_facts.c            # 类型事实
├── enum_infos.c            # 枚举信息
├── union_infos.c           # union 信息
├── cyclic.c                # 循环依赖检测
├── reifiable_deps.c        # reifiable 依赖
└── value_kind.c            # 值种类
```

**关键入口函数**：
- `resolve_expr(context, expr, allow_self)` — 表达式语义分析
- `resolve_block(context, block, allow_self)` — 块语义分析
- `resolve_expr_owner_type(context, ...)` — 表达式所有者类型推断
- `resolve_expr_callable_value(context, ...)` — 可调用值解析

**错误报告**：`resolver_append_error(context, token, "AExxxx", format_message("..."))`

### 3.2 CE（发码/codegen）

```
src/codegen/
├── codegen.c     # 主发码器：cg_emit_expr / cg_emit_stmt / cg_emit_block
├── codegen.h     # 接口定义
├── mapping.c     # 错误码映射辅助
└── mapping.h
```

**关键入口函数**：
- `cg_emit_expr(cg, expr, out)` — 表达式发码
- `cg_emit_stmt(cg, stmt)` — 语句发码
- `cg_emit_block(cg, block)` — 块发码
- `cg_emit_call(cg, expr, out)` — 调用发码
- `cg_emit_member(cg, expr, out)` — 成员访问发码
- `cg_emit_cast(cg, expr, out)` — 类型转换发码

**错误报告**：`cg_fail(cg, tok, "CExxxx", "codegen: ...")`

## 4. 待迁移检查项分类

### 4.1 P1：基础类型检查（37 码）

AE 已有完整的 AST 遍历和类型信息，在对应节点补上校验即可。

#### 4.1.1 运算符操作数类型（8 码）

| 错误码 | 检查内容 | codegen 位置 | AE 挂载点 |
|---|---|---|---|
| CE0083 | 数值运算操作数必须为 numeric | `cg_emit_binary` / `cg_emit_unary` | `resolve_expr` 中 binary/unary 分支 |
| CE0087 | `&&`/`||` 操作数必须为 bool | `cg_emit_binary` | 同上 |
| CE0088 | 有序比较操作数必须为 numeric | `cg_emit_binary` | 同上 |
| CE0089 | float 不支持 modulo | `cg_emit_binary` | 同上 |
| CE0090 | 一元 `&` 操作数类型缺失 | `cg_emit_unary` | 同上 |
| CE0094 | `!` 操作数必须为 bool | `cg_emit_unary` | 同上 |
| CE0095 | `~` 操作数必须为 integer | `cg_emit_unary` | 同上 |
| CE0096 | 一元 `+`/`-` 操作数必须为 numeric | `cg_emit_unary` | 同上 |

#### 4.1.2 赋值目标/可变性（10 码）

| 错误码 | 检查内容 | codegen 位置 | AE 挂载点 |
|---|---|---|---|
| CE0071 | imported binding 不可赋值 | `cg_emit_assign` | `resolve_expr` 中 assign 分支 |
| CE0072 | 复合赋值要求 numeric 类型 | `cg_emit_assign` | 同上 |
| CE0073 | 不支持的复合赋值运算符 | `cg_emit_assign` | 同上 |
| CE0244 | static binding 不可赋值 | `cg_emit_assign` | 同上 |
| CE0245 | static binding 复合赋值要求 numeric | `cg_emit_assign` | 同上 |
| CE0249 | 成员复合赋值要求 numeric 字段 | `cg_emit_assign` | 同上 |
| CE0250 | 不支持的成员复合赋值运算符 | `cg_emit_assign` | 同上 |
| CE0256 | 成员赋值目标必须为对象 | `cg_emit_assign` | 同上 |
| CE0260 | module binding 不可赋值 | `cg_emit_assign` | 同上 |
| CE0262 | local binding 复合赋值要求 numeric | `cg_emit_assign` | 同上 |

#### 4.1.3 类型转换（6 码）

| 错误码 | 检查内容 | codegen 位置 | AE 挂载点 |
|---|---|---|---|
| CE0069 | tuple cast 源值必须为 tuple | `cg_emit_cast` | `resolve_expr` 中 cast 分支 |
| CE0070 | tuple cast 元素数量匹配 | `cg_emit_cast` | 同上 |
| CE0192 | array cast 降级类型一致性 | `cg_emit_cast` | 同上 |
| CE0194 | cast 操作数必须为 numeric/bool | `cg_emit_cast` | 同上 |
| CE0241 | 泛型数组元素赋值同类型参数 | `cg_emit_assign` | 同上 |
| CE0248 | 泛型字段赋值同类型参数 | `cg_emit_assign` | 同上 |

#### 4.1.4 对象字面量/构造/解构（10 码）

| 错误码 | 检查内容 | codegen 位置 | AE 挂载点 |
|---|---|---|---|
| CE0043 | finalizer 重复声明 | `cg_emit_type_finalizer` | `resolve_decl` 中 type 分支 |
| CE0076 | 构造器参数要求已解析 | `cg_emit_call` | `resolve_expr` 中 call 分支 |
| CE0077 | 构造器参数数量 | `cg_emit_call` | 同上 |
| CE0126 | 构造参数数量（变体） | `cg_emit_call` | 同上 |
| CE0168 | 构造调用目标类型未知 | `cg_emit_call` | 同上 |
| CE0178 | 对象字面量目标类型未知 | `cg_emit_object_literal` | `resolve_expr` 中 object_literal 分支 |
| CE0181 | 对象字面量字段重复 | `cg_emit_object_literal` | 同上 |
| CE0229 | 解构绑定要求初始化器 | `cg_emit_binding` | `resolve_binding` |
| CE0230 | 解构绑定元素数量匹配 | `cg_emit_binding` | 同上 |
| CE0234 | 绑定缺少类型或初始化器 | `cg_emit_binding` | 同上 |

#### 4.1.5 Extern 注解（2 码）

| 错误码 | 检查内容 | codegen 位置 | AE 挂载点 |
|---|---|---|---|
| CE0010 | extern 注解参数必须解析为字符串 | `cg_resolve_extern_annotation` | `resolve_decl` 中 extern 分支 |
| CE0013 | extern 注解参数缺失 | `cg_resolve_extern_annotation` | 同上 |

#### 4.1.6 Union match 标签（1 码）

| 错误码 | 检查内容 | codegen 位置 | AE 挂载点 |
|---|---|---|---|
| CE0268 | union match 标签必须为规范化成员 | `cg_emit_match_expr` | `resolve_expr` 中 match 分支 |

### 4.2 P2：计数与存在性检查（50 码）

AE 已有符号表和类型成员信息，查找操作直接复用。

#### 4.2.1 参数数量（18 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0122 | 泛型方法参数数量 | `resolve_expr` call 分支 |
| CE0125 | 变参函数最小参数数量 | 同上 |
| CE0128 | variadic callable 最小参数数量 | 同上 |
| CE0129 | callable 参数数量 | 同上 |
| CE0130 | 泛型方法调用 object-form spec | 同上 |
| CE0131 | variadic callable constraint 最小参数数量 | 同上 |
| CE0132 | callable constraint 参数数量 | 同上 |
| CE0140 | 方法参数数量（两个变体） | 同上 |
| CE0145 | 泛型静态方法类型实参数量 | 同上 |
| CE0146 | 泛型静态方法变参最小参数数量 | 同上 |
| CE0147 | 泛型静态方法参数数量 | 同上 |
| CE0151 | 静态方法参数数量 | 同上 |
| CE0152 | 变参静态方法最小参数数量 | 同上 |
| CE0154 | imported binding 不可调用 | 同上 |
| CE0156 | spec 方法不存在 | `resolve_expr` member 分支 |
| CE0157 | spec 方法参数数量 | `resolve_expr` call 分支 |
| CE0160 | 方法参数数量（变体） | 同上 |
| CE0165 | 变参方法最小参数数量 | 同上 |

#### 4.2.2 成员存在性（13 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0104 | 标识符解析缺失 | `resolve_expr` identifier 分支 |
| CE0119 | self 上下文缺失 | `resolve_expr` self 分支 |
| CE0124 | 函数解析缺失 | `resolve_expr` call 分支 |
| CE0163 | 方法调用目标必须为对象 | `resolve_expr` call 分支 |
| CE0164 | 类型方法不存在 | `resolve_expr` call/member 分支 |
| CE0170 | 枚举项不存在 | `resolve_expr` member 分支 |
| CE0171 | spec 字段不存在 | `resolve_expr` member 分支 |
| CE0173 | spec 字段不存在（变体） | 同上 |
| CE0174 | tuple 字段不存在 | `resolve_expr` member 分支 |
| CE0175 | 成员访问目标必须为对象 | `resolve_expr` member 分支 |
| CE0176 | 类型字段不存在 | `resolve_expr` member 分支 |
| CE0259 | 赋值目标标识符未定义 | `resolve_expr` assign 分支 |
| CE0288 | main 返回类型约束 | `resolve_decl` function 分支 |

#### 4.2.3 简单控制流（17 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0195 | if 表达式分支类型一致性 | `resolve_expr` if_expr 分支 |
| CE0197 | try/catch 分支类型一致性 | `resolve_expr` try_expr 分支 |
| CE0199 | if 表达式分支结果语句约束 | `resolve_expr` if_expr 分支 |
| CE0201 | if 表达式条件必须为 bool | 同上 |
| CE0205 | match 表达式要求 else 分支 | `resolve_expr` match_expr 分支 |
| CE0206 | match else 分支结果语句约束 | 同上 |
| CE0207 | match 分支结果语句约束 | 同上 |
| CE0210 | match 分支必须有标签 | 同上 |
| CE0211 | match 目标必须为 integer/bool/string | 同上 |
| CE0215 | catch 块必须产生值 | `resolve_expr` try_expr 分支 |
| CE0264 | void 函数不能返回值 | `resolve_stmt` return 分支 |
| CE0265 | non-void 函数必须返回值 | `resolve_block` 尾部检查 |
| CE0266 | if 语句条件必须为 bool | `resolve_stmt` if 分支 |
| CE0269 | if-match 分支必须有标签 | `resolve_stmt` if_match 分支 |
| CE0270 | if-match 目标类型约束 | 同上 |
| CE0271 | while 条件必须为 bool | `resolve_stmt` while 分支 |
| CE0272 | for 条件必须为 bool | `resolve_stmt` for 分支 |

#### 4.2.4 迭代协议（2 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0273 | for/in 源类型缺少 @iterable 方法 | `resolve_stmt` for_in 分支 |
| CE0277 | for/in 序列必须为数组 | 同上 |

### 4.3 P3：泛型验证（16 码）

AE 已有泛型定义信息，部分类型推断能力已存在。

#### 4.3.1 泛型参数数量（7 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0029 | 泛型类型实参数量 | `resolve_type_ref` |
| CE0030 | 泛型 spec 实参数量 | 同上 |
| CE0135 | 泛型方法类型实参数量 | `resolve_expr` call 分支 |
| CE0145 | 泛型静态方法类型实参数量 | 同上 |
| CE0303 | 泛型 extern 类型实参数量 | `resolve_expr` extern call 分支 |
| CE0306 | 泛型函数类型实参数量 | `resolve_expr` call 分支 |
| CE0312 | spec 值满足泛型约束时 spec form 不匹配 | `resolve_expr` 泛型约束检查 |

#### 4.3.2 泛型类型推断（5 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0137 | 泛型方法类型实参推断失败 | `resolve_expr` call 分支 |
| CE0141 | 泛型方法类型实参推断失败（变体） | 同上 |
| CE0148 | 泛型静态方法类型实参推断失败 | 同上 |
| CE0304 | 泛型 extern 类型实参从参数推断失败 | `resolve_expr` extern call 分支 |
| CE0305 | 泛型 extern 类型实参推断失败 | 同上 |

#### 4.3.3 泛型约束匹配（4 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0130 | 泛型直接调用要求 callable-form spec | `resolve_expr` call 分支 |
| CE0134 | 泛型方法调用要求 object-form spec | 同上 |
| CE0139 | 泛型成员访问要求 object-form spec | `resolve_expr` member 分支 |
| CE0150 | 泛型成员赋值要求 object-form spec | `resolve_expr` assign 分支 |

### 4.4 P4：Spec 满足性（22 码）

AE 已有 spec 定义和 witness 生成逻辑，需补全满足性校验。

#### 4.4.1 Spec 基本满足（9 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0047 | spec 父项递归注册检测 | `spec_relations.c` |
| CE0084 | spec 相等性要求聚合 spec 操作数 | `spec_equalities.c` |
| CE0315 | type 对 spec 成员实现缺失 | `resolve_decl` type 分支 |
| CE0319 | spec 成员实现缺失 | 同上 |
| CE0321 | enum 对 spec 字段满足缺少字段支持 | `resolve_decl` enum 分支 |
| CE0322 | enum 对 spec 成员实现缺失 | 同上 |
| CE0323 | enum 对 spec 方法多个可见实现 | 同上 |
| CE0325 | spec 成员实现缺失（变体） | `resolve_decl` type 分支 |
| CE0344 | spec 字段不能由 fit 方法满足 | 同上 |

#### 4.4.2 Spec 字段约束（6 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0253 | spec 字段必须声明 var 才可写 | `resolve_expr` assign 分支 |
| CE0254 | spec 字段复合赋值要求 numeric | 同上 |
| CE0255 | 不支持的 spec 字段复合赋值运算符 | 同上 |
| CE0316 | type 字段与 spec 字段类型不匹配 | `resolve_decl` type 分支 |
| CE0347 | spec 字段满足载体约束 | 同上 |
| CE0346 | spec 方法实现载体约束 | 同上 |

#### 4.4.3 Tuple spec 满足（7 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0331 | tuple 对 spec 成员实现缺失 | `resolve_decl` tuple 分支 |
| CE0334 | tuple 对 spec 字段满足缺失 | 同上 |
| CE0335 | tuple 对 spec 方法满足需 fit 方法 | 同上 |
| CE0336 | tuple spec 成员需由 fit 方法实现 | 同上 |
| CE0337 | tuple spec 字段需由 tuple 字段满足 | 同上 |
| CE0338 | tuple 不可变字段不能满足 var spec 字段 | 同上 |
| CE0344 | spec 字段不能由 fit 方法满足 | 同上 |

### 4.5 P5：Callable/Fit 语义（20 码）

#### 4.5.1 Callable coercion（11 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0100 | callable-form spec 目标注册缺失 | `resolve_expr` coercion 分支 |
| CE0101 | callable lambda coercion 参数数量 | 同上 |
| CE0105 | callable coercion 目标注册缺失 | 同上 |
| CE0110 | callable method coercion 要求 member expr | 同上 |
| CE0112 | callable method coercion 源值必须为对象 | 同上 |
| CE0113 | callable method coercion receiver 类型不匹配 | 同上 |
| CE0115 | callable-form 源目标签名匹配 | 同上 |
| CE0116 | callable-form 源值类型约束 | 同上 |
| CE0127 | callable value 调用要求 callable-form spec | 同上 |
| CE0128 | variadic callable 最小参数数量 | 同上 |
| CE0129 | callable 参数数量 | 同上 |

#### 4.5.2 Fit 解析（7 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0048 | fit spec 用户 spec 解析失败 | `resolve_decl` fit 分支 |
| CE0049 | fit 目标 builtin/array 解析失败 | 同上 |
| CE0050 | fit 目标缺失 | 同上 |
| CE0052 | fit 目标用户类型解析失败（两个变体） | 同上 |
| CE0053 | fit 目标类型非已知用户类型 | 同上 |
| CE0054 | fit body 仅支持方法成员 | 同上 |
| CE0159 | builtin/array fit 方法不存在 | 同上 |

#### 4.5.3 Spec 相等性（2 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0084 | spec 相等性要求聚合 spec 操作数 | `spec_equalities.c` |
| CE0317 | type 对 spec 方法多个可见 fit 实现 | `resolve_decl` type 分支 |

### 4.6 P6：跨模块/阶段（6 码）

| 错误码 | 检查内容 | AE 挂载点 |
|---|---|---|
| CE0006 | 模块级绑定类型或初始化器缺失 | `resolve_binding` |
| CE0008 | 类型解析缺失 | `resolve_type_ref` |
| CE0012 | 字段声明式推断类型不支持 | `resolve_decl` field 分支 |
| CE0013 | 字段类型或推断事实缺失 | 同上 |
| CE0154 | imported binding 不可调用 | `resolve_expr` call 分支 |
| CE0315 | spec coercion 引用越界当前 codegen scope | `spec_coercion_sites.c` |

## 5. 迁移步骤

每批迁移按以下步骤执行：

```
1. 文档  → 更新本文档标记已迁移项
2. AE    → 在 analyzer.c（或对应子模块）补上校验
3. 测试  → 确认触发 AE 错误的测试用例存在
4. CE    → 从 codegen.c 移除旧校验（保留 cg_fail 但标记 unreachable）
5. 回归  → 全量回归测试
6. 提交  → 每批一个 commit
```

## 6. 风险与注意事项

1. **AE 与 CE 类型表示差异**：AE 使用 `FengTypeRef` / `InferredExprType`，CE 使用 `CGType` / `CGTypeKind`，迁移时需要映射判断逻辑而非直接搬代码
2. **codegen 上下文信息**：部分校验在 codegen 中依赖发码上下文（如当前正在发射的函数类型），AE 中需通过 `ResolveContext` 获取等价信息
3. **错误消息一致性**：迁移后错误码不变，但前缀从 `codegen:` 变为 `semantic:`，需同步更新测试中的预期输出
4. **性能影响**：AE 增加校验不影响运行时性能（编译期开销），但需确认不会显著拖慢编译速度
