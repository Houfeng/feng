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
| CE0269 | match 分支必须有标签 | `resolve_stmt` match 分支 |
| CE0270 | match 目标类型约束 | 同上 |
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

## 6. 类型系统映射表

CE 使用 `CGType` / `CGTypeKind`，AE 使用 `FengTypeRef` / `InferredExprType`。迁移时需要按以下映射转换判断逻辑：

| CE 判断 | AE 等价 |
|---|---|
| `cgtype_is_numeric(k)` | `inferred_expr_type_is_numeric(t)` |
| `cgtype_is_integer(k)` | `inferred_expr_type_is_integer(t)` |
| `cgtype_is_float(k)` | `inferred_expr_type_is_float(t)` |
| `k == CG_TYPE_BOOL` | `inferred_expr_type_is_bool(t)` |
| `k == CG_TYPE_STRING` | `inferred_expr_type_is_string(t)` |
| `type_ref_is_void(ref)` | `inferred_expr_type_is_void(t)` |
| `cgtype_is_aggregate(t)` | 通过 `InferredExprType.kind == FENG_INFERRED_EXPR_TYPE_DECL` + `decl` 判断 |
| `cgtype_is_tuple_user(t)` | `inferred_expr_type_is_enum(context, t)` 或 type_decl 判断 |
| `cgtype_is_managed(t)` | AE 层暂无直接等价，需通过 type_decl 属性判断 |

> **关键差异**：CE 的类型是已解析的 `CGTypeKind` 枚举值，可直接比较；AE 的类型是 `InferredExprType`，可能是 `DECL`（指向 `FengDecl`）、`TYPE_REF`（指向 `FengTypeRef`）、`BUILTIN`（内建类型名）或 `LAMBDA`，需要通过对应的 `inferred_expr_type_is_*` 辅助函数间接判断。

## 7. AE 可用的检查 API

### 7.1 类型判断函数

位于 `src/semantic/analyzer.c`：

| 函数签名 | 行号 | 说明 |
|---|---|---|
| `inferred_expr_type_is_numeric(InferredExprType)` | 5287 | 判断是否为数值类型 |
| `inferred_expr_type_is_integer(InferredExprType)` | 5313 | 判断是否为整数类型 |
| `inferred_expr_type_is_float(InferredExprType)` | 5319 | 判断是否为浮点类型 |
| `inferred_expr_type_is_bool(InferredExprType)` | 5324 | 判断是否为 bool |
| `inferred_expr_type_is_string(InferredExprType)` | 5330 | 判断是否为 string |
| `inferred_expr_type_is_void(InferredExprType)` | 5113 | 判断是否为 void |
| `inferred_expr_type_is_known(InferredExprType)` | 538 | 判断类型是否已解析 |
| `inferred_expr_type_is_enum(context, InferredExprType)` | 5293 | 判断是否为枚举类型 |
| `inferred_expr_types_equal(context, left, right)` | — | 判断两个类型是否相等 |
| `type_ref_is_void(const FengTypeRef *)` | 992 | 判断 type_ref 是否为 void |
| `type_ref_is_unknown(const FengTypeRef *)` | 999 | 判断 type_ref 是否未解析 |
| `type_ref_equals(left, right)` | 1011 | 判断两个 type_ref 是否相等 |
| `builtin_type_name_is_numeric(FengSlice)` | 2223 | 按内建名称判断是否数值 |
| `builtin_type_name_is_integer(FengSlice)` | 2230 | 按内建名称判断是否整数 |

### 7.2 表达式类型推断

| 函数 | 说明 |
|---|---|
| `infer_expr_type(context, expr)` | 推断表达式的类型，返回 `InferredExprType` |
| `resolve_expr_owner_type(context, expr, &decl, &module)` | 获取表达式的所有者类型 |
| `resolve_expr_callable_value(context, expr)` | 解析可调用值 |

### 7.3 已有的校验函数

AE 中**已存在**的校验逻辑，部分与 CE 重叠：

| 函数 | 行号 | 覆盖的 CE 错误码 |
|---|---|---|
| `validate_binary_expr(context, expr)` | 5856 | CE0083/CE0087/CE0088/CE0089（通过 AE0030 报告） |
| `unary_expr_type_is_valid(op, type)` | 5527 | CE0094/CE0095/CE0096 |
| `binary_expr_types_are_valid(context, op, left, right)` | 5543 | CE0083/CE0087/CE0088 |
| `validate_division_or_modulo_rhs_zero(...)` | — | CE0089 (float modulo) |

> **重要发现**：AE 的 `validate_binary_expr` 已覆盖 CE0083/CE0087/CE0088 的检查，使用统一的 AE0030 错误码报告。这意味着对这些错误码，迁移工作 = **仅从 codegen 中移除冗余检查**，无需在 AE 中新增代码。

## 8. 构建与测试命令

```bash
# 编译
make cli               # 编译 CLI（含 lexer → parser → semantic → codegen）
make runtime           # 编译 runtime 静态库

# 单元测试
make test              # 全量回归（含所有单元测试 + smoke + cli-tests）

# 按模块运行测试
./build/bin/test_semantic     # 仅语义测试
./build/bin/test_codegen      # 仅发码测试

# Smoke 测试
make smoke             # test/smoke/phase1a/*.ff + *.expected

# CLI 集成测试
make cli-tests         # test/cli/ 下的集成测试
make cli-project-tests # 项目级集成测试
make std-tests         # 标准库测试
```

## 9. 错误报告模式对比

### CE（codegen）

```c
// 在 codegen.c 中
static bool cg_fail(CG *cg, FengToken token, const char *code, const char *fmt, ...);

// 调用方式
return cg_fail(cg, tok, "CE0083", "codegen: cannot apply numeric op to non-numeric operands");
return cg_fail(cg, tok, "CE0029", "codegen: generic type '%.*s' expects %zu type argument(s), got %zu",
               (int)name.length, name.data, expected, actual);
```

### AE（semantic）

```c
// 在 analyzer.c 中
static bool resolver_append_error(ResolveContext *context, FengToken token,
                                   const char *code, char *message);

// format_message 负责格式化（类似 sprintf，返回 malloc'd 字符串）
static char *format_message(const char *fmt, ...);

// 调用方式
return resolver_append_error(
    context,
    expr->token,
    "AE0030",
    format_message("binary operator '%s' requires operands of the same numeric type, got '%s' and '%s'",
                   operator_name, left_type_name, right_type_name));
```

### 关键差异

| 维度 | CE | AE |
|---|---|---|
| 上下文 | `CG *cg` | `ResolveContext *context` |
| Token | `FengToken token` | `FengToken token`（相同） |
| 消息格式化 | `cg_fail` 内嵌 variadic | `format_message` 独立函数 |
| 消息前缀 | `codegen: ...` | 无前缀（直接写消息） |
| 返回值 | `false`（终止发码） | `false`（终止分析） |

## 10. 迁移示例：CE0083（数值运算操作数类型约束）

### 10.1 现状分析

**CE 中的检查**（`codegen.c:11041`）：
```c
// codegen.c:11017-11042 — cg_numeric_common_type
static bool cg_numeric_common_type(CG *cg, FengToken tok,
                                    CGTypeKind lk, CGTypeKind rk,
                                    CGTypeKind *out_common) {
    // ... 类型提升逻辑 ...
    if (cgtype_is_integer(lk) && cgtype_is_integer(rk)) {
        // ... 整数提升 ...
        return true;
    }
    return cg_fail(cg, tok, "CE0083",
        "codegen: cannot apply numeric op to non-numeric operands");
}
```

**AE 中的检查**（`analyzer.c:5856-5954`）：
```c
// analyzer.c:5856 — validate_binary_expr
static bool validate_binary_expr(ResolveContext *context, const FengExpr *expr) {
    InferredExprType left_type = infer_expr_type(context, expr->as.binary.left);
    InferredExprType right_type = infer_expr_type(context, expr->as.binary.right);

    if (binary_expr_types_are_valid(context, expr->as.binary.op, left_type, right_type)) {
        return true;  // 类型合法
    }

    // 类型不合法，报告 AE0030
    return resolver_append_error(context, expr->token, "AE0030",
        format_message("binary operator '%s' requires operands of the same numeric type, ..."));
}
```

### 10.2 结论

AE 的 `validate_binary_expr` 已完整覆盖 CE0083 的检查（通过 `binary_expr_types_are_valid` 判断，失败时以 AE0030 报告）。

### 10.3 迁移操作

**Step 1：确认 AE 已覆盖**

`validate_binary_expr` 在 `resolve_expr` 的 `FENG_EXPR_BINARY` 分支中被调用（`analyzer.c:1620`），所有二元表达式在语义阶段已经过类型校验。

**Step 2：从 codegen 移除冗余检查**

将 `cg_numeric_common_type` 中的 `cg_fail` 替换为 unreachable 断言：

```c
// Before (codegen.c:11041):
return cg_fail(cg, tok, "CE0083",
    "codegen: cannot apply numeric op to non-numeric operands");

// After:
assert(false && "AE should have rejected non-numeric operands");
return false;
```

**Step 3：验证测试**

```bash
make test  # 确认所有测试通过
```

AE 的 `test_semantic.c` 中已有覆盖二元运算符类型错误的测试用例（通过 AE0030 触发）。

### 10.4 迁移分类

CE0083 属于"**AE 已覆盖，仅需删除 CE 冗余**"类别。类似的还有：
- CE0087（`&&`/`||` bool 约束）→ AE0030 已覆盖
- CE0088（有序比较 numeric 约束）→ AE0030 已覆盖
- CE0094（`!` bool 约束）→ `unary_expr_type_is_valid` 已覆盖
- CE0095（`~` integer 约束）→ `unary_expr_type_is_valid` 已覆盖
- CE0096（一元 +/- numeric 约束）→ `unary_expr_type_is_valid` 已覆盖

## 11. codegen 错误精确定位索引

### 11.1 实际覆盖情况

文档规划了 149 个"回到AE"错误码，但经核实 **codegen.c 中仅实现了 48 个**，其余 101 个为规划态（尚未在 codegen 中发码）。

| 状态 | 数量 | 迁移策略 |
|---|---|---|
| codegen.c 已实现 | 48 | 从 codegen 移除 → AE 补上（或确认 AE 已覆盖则直接删除） |
| codegen.c 未实现 | 101 | 直接在 AE 中实现（无需从 codegen 迁移） |

### 11.2 codegen.c 已实现的 48 个错误码（精确行号）

| 错误码 | codegen.c 行号 | 用途 |
|---|---|---|
| CE0006 | 2181 | 模块级绑定类型或初始化器缺失 |
| CE0008 | 3004 | 类型解析缺失 |
| CE0012 | 3661 | 字段声明式推断类型不支持 |
| CE0013 | 3680 | 字段类型或推断事实缺失 |
| CE0014 | 3690 | 标识符解析缺失 |
| CE0015 | 3704 | self 上下文缺失 |
| CE0108 | 12123 | 泛型类型实参数量约束 |
| CE0109 | 12128 | 泛型 spec 实参数量约束 |
| CE0118 | 12425 | 泛型直接调用 callable-form spec 约束要求 |
| CE0121 | 12555 | 泛型方法类型实参数量约束 |
| CE0122 | 12564 | 泛型方法参数数量约束 |
| CE0123 | 12605 | 泛型方法类型实参推断失败 |
| CE0125 | 12776 | 泛型类型方法不存在约束 |
| CE0129 | 13077 | 泛型静态方法类型实参数量约束 |
| CE0130 | 13158 | 泛型静态方法变参最小实参数量约束 |
| CE0131 | 13168 | 泛型静态方法参数数量约束 |
| CE0132 | 13176 | 泛型静态方法类型实参推断失败 |
| CE0134 | 13476 | 泛型方法调用 object-form spec 约束要求 |
| CE0137 | 13532 | 泛型类型构造实例注册缺失 |
| CE0138 | 13624 | 泛型类型字段不存在约束 |
| CE0139 | 13770 | 泛型成员访问 object-form spec 约束要求 |
| CE0145 | 14113 | 泛型数组元素赋值同类型参数约束 |
| CE0147 | 14128 | 泛型字段复合赋值具体数值类型约束 |
| CE0148 | 14199 | 泛型字段赋值同类型参数约束 |
| CE0150 | 14485 | 泛型成员赋值 object-form spec 约束要求 |
| CE0162 | 15079 | 泛型 extern 类型实参数量约束 |
| CE0163 | 15112 | 泛型 extern 类型实参从参数推断失败 |
| CE0164 | 15169, 15185 | 泛型 extern 类型实参推断失败 |
| CE0165 | 15205 | 泛型函数类型实参数量约束 |
| CE0167 | 15369 | 泛型函数类型实参推断失败 |
| CE0168 | 15376 | spec 值满足泛型约束时 spec form 不匹配 |
| CE0304 | 23992 | spec 父项递归注册检测 |
| CE0305 | 24005 | spec 相等性要求聚合 spec 操作数 |
| CE0307 | 24322 | callable value 调用需要 callable-form spec 类型 |
| CE0308 | 24345 | variadic callable constraint 最小实参数量约束 |
| CE0309 | 24512 | callable constraint 参数数量约束 |
| CE0310 | 25145, 25158 | spec 方法不存在约束 |
| CE0311 | 25167 | spec 字段不存在约束 |
| CE0312 | 25268 | spec 方法参数数量约束 |
| CE0318 | 25584, 25914 | spec 字段可写性前置约束 |
| CE0319 | 25592, 25922 | spec 字段复合赋值数值类型约束 |
| CE0320 | 25600, 25931 | spec 字段复合赋值运算符不支持 |
| CE0322 | 25674 | type 对 spec 成员实现缺失 |
| CE0323 | 25684 | type 字段与 spec 字段类型不匹配 |
| CE0325 | 25702, 25979 | spec 成员实现缺失 |
| CE0326 | 25862 | enum 对 spec 字段满足缺少字段支持 |
| CE0327 | 25869 | enum 对 spec 成员实现缺失 |
| CE0328 | 25875 | enum 对 spec 方法存在多个可见实现 |

### 11.3 codegen.c 未实现的 101 个错误码（直接在 AE 中实现）

| 错误码 | 用途 |
|---|---|
| CE0401 | fit spec 用户 spec 解析失败 |
| CE0402 | fit 目标 builtin/array 解析失败 |
| CE0403 | fit 目标缺失 |
| CE0405 | fit 目标用户类型解析失败 |
| CE0406 | fit body 仅支持方法成员 |
| CE0407 | builtin/array fit 方法不存在约束 |
| CE0507 | callable lambda coercion 参数数量不匹配 |
| CE0511 | callable method coercion 需要 member expression |
| CE0513 | callable method coercion 源值必须为对象 |
| CE0514 | callable method coercion receiver 类型不匹配 |
| CE0516 | callable-form 源目标签名匹配约束 |
| CE0517 | callable-form 源值类型约束 |
| CE0519 | variadic callable 最小实参数量约束 |
| CE0520 | callable 参数数量约束 |
| CE0521 | imported binding 不可调用 |
| CE0601 | extern 注解参数字符串字面量解析要求 |
| CE0604 | extern 注解参数缺失 |
| CE0705 | union-form match 标签规范化成员约束 |
| CE0707 | 枚举项不存在约束 |
| CE0712 | tuple 字面量命名目标类型要求 |
| CE0713 | tuple 字面量元素数量匹配约束 |
| CE0715 | tuple cast 源值类型约束 |
| CE0716 | tuple cast 元素数量匹配约束 |
| CE0717 | tuple 字段不存在约束 |
| CE0718 | 解构源 tuple 值要求 |
| CE0719 | iterator 返回 tuple 形态约束 |
| CE0721 | tuple 对 spec 成员实现缺失 |
| CE0724 | tuple 对 spec 字段满足缺失 |
| CE0725 | tuple 对 spec 方法满足需 fit 方法 |
| CE0726 | tuple spec 成员需由 fit 方法实现 |
| CE0727 | tuple spec 字段需由 tuple 字段满足 |
| CE0728 | tuple 不可变字段不能满足 var spec 字段 |
| CE0730 | 空数组元素类型要求 |
| CE0731 | 数组字面量元素同类型约束 |
| CE0732 | 数组索引目标类型约束 |
| CE0733 | 数组索引整数约束 |
| CE0734 | array cast 降级类型一致性约束 |
| CE0736 | 类型转换操作数类型约束 |
| CE0801 | imported binding 不可变赋值约束 |
| CE0802 | 绑定复合赋值数值类型约束 |
| CE0803 | 绑定复合赋值运算符不支持 |
| CE0807 | 索引赋值目标数组类型约束 |
| CE0808 | 索引复合赋值元素数值类型约束 |
| CE0809 | 索引复合赋值运算符不支持 |
| CE0810 | static binding 不可变赋值约束 |
| CE0811 | static binding 复合赋值数值类型约束 |
| CE0812 | 成员复合赋值字段数值类型约束 |
| CE0813 | 成员复合赋值运算符不支持 |
| CE0814 | 成员赋值目标对象类型约束 |
| CE0816 | 赋值目标标识符未定义 |
| CE0817 | module binding 不可变赋值约束 |
| CE0818 | local binding 复合赋值数值类型约束 |
| CE0901 | finalizer 重复声明 |
| CE0902 | 构造器解析前置约束 |
| CE0903 | 构造目标参数数量约束 |
| CE0904 | 构造调用目标类型未知 |
| CE0907 | 对象字面量目标类型未知 |
| CE0909 | 对象字面量字段重复 |
| CE0913 | 解构绑定初始化器缺失 |
| CE0914 | 解构绑定元素数量匹配约束 |
| CE0915 | 绑定类型或初始化器缺失 |
| CE1006 | 数值运算操作数类型约束 |
| CE1008 | 短路逻辑运算 bool 操作数约束 |
| CE1009 | 有序比较数值类型约束 |
| CE1010 | float modulo 不支持 |
| CE1011 | 一元 `&` 操作数类型缺失 |
| CE1013 | 逻辑非 bool 操作数约束 |
| CE1014 | 按位非 integer 操作数约束 |
| CE1015 | 一元正负 numeric 操作数约束 |
| CE1017 | 函数解析缺失 |
| CE1018 | 变参函数最小实参数量约束 |
| CE1019 | 方法参数数量约束 |
| CE1020 | 静态方法参数数量约束 |
| CE1021 | 变参静态方法最小实参数量约束 |
| CE1023 | 方法调用目标对象类型约束 |
| CE1024 | 类型方法不存在约束 |
| CE1025 | 变参方法最小实参数量约束 |
| CE1027 | 成员访问目标对象类型约束 |
| CE1028 | 类型字段不存在约束 |
| CE1029 | if 表达式分支类型一致性约束 |
| CE1030 | try 表达式分支类型一致性约束 |
| CE1031 | if 表达式分支结果语句约束 |
| CE1032 | if 表达式条件 bool 约束 |
| CE1033 | match 区间标签目标整型约束 |
| CE1035 | match 表达式 else 分支存在性约束 |
| CE1036 | match 表达式 else 分支结果语句约束 |
| CE1037 | match 分支结果语句约束 |
| CE1038 | match 分支标签存在性约束 |
| CE1039 | match 目标类型约束 |
| CE1040 | catch 块结果值约束 |
| CE1041 | void 函数返回值禁用 |
| CE1042 | non-void 函数返回值必需 |
| CE1043 | if 语句条件 bool 约束 |
| CE1045 | match 分支标签存在性约束 |
| CE1046 | match 目标类型约束 |
| CE1047 | while 条件 bool 约束 |
| CE1048 | for 条件 bool 约束 |
| CE1049 | for/in 源类型 @iterable 方法缺失 |
| CE1052 | for/in 序列数组类型约束 |
| CE1053 | 循环控制语句上下文约束 |
| CE1056 | throw 值缺失 |

## 12. 风险与注意事项

1. **AE 已覆盖的检查优先删除**：如第 10 节示例所示，部分 CE 检查在 AE 中已有等价实现（如 AE0030 覆盖 CE0083/CE0087/CE0088）。这类迁移仅需从 codegen 中删除冗余代码，成本极低
2. **AE 未覆盖的检查需新增**：对于 AE 中尚无对应校验的错误码，需在 `analyzer.c` 的对应 `resolve_expr` / `resolve_stmt` 分支中补上检查
3. **类型表示差异**：见第 6 节映射表，迁移时需将 `CGTypeKind` 判断转换为 `inferred_expr_type_is_*` 调用
4. **codegen 上下文信息**：部分校验在 codegen 中依赖发码上下文（如当前正在发射的函数类型），AE 中需通过 `ResolveContext` 获取等价信息
5. **错误码保持不变**：迁移后错误码仍使用原 CE 编号，仅报告阶段从 codegen 变为 semantic。测试中如果断言了错误码，无需修改
6. **性能影响**：AE 增加校验不影响运行时性能（编译期开销），但需确认不会显著拖慢编译速度
