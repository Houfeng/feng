# CE 发码错误码分段归类方案（责任域对齐版）

## 归类原则

- 主归属按发码责任链与后端阶段确定，优先与 codegen 实际责任边界一致。
- 同一错误可能同时涉及泛型、规约、描述符、调用、控制流等多个维度；文档仅给出一个主归属，避免重复。
- 目标是便于定位发码点、后续替换旧码以及收敛同类诊断，不是复用旧总表里的粗粒度“用途”标签。
- 错误码完整格式固定为：`CE` + 两位段编码 + 两位段内编码（例如 `CE0001`）。
- “同类错误共码”判定同时满足三点：冲突对象一致、失败根因一致、建议修复动作一致；任一不一致则拆分新错误码。
- 对仅有占位符形式差异、但错误对象与修复动作完全一致的旧错误码，本版合并为同一个新错误码。

## 分段规划

| 段编码 | 段 | 语义域（主归属） |
|---|---|---|
| 00 | 通用段 | 调试、内部状态、类型解析与类型推断 |
| 01 | 泛型段 | 泛型实例化、约束、描述符上下文与实参推断 |
| 02 | 聚合描述符段 | aggregate descriptor、default-init rule 与等价元数据 |
| 03 | 规约/见证段 | spec、witness、coercion、contract 满足与 callable-form spec |
| 04 | Fit段 | fit 目标、fit 实现、fit 注册与 fit 冲突 |
| 05 | 可调用对象段 | callable value、lambda/method coercion 与 callable 默认值 |
| 06 | ABI/Extern段 | extern 注解、@runtime、ABI lowering 与 ABI function pointer |
| 07 | 聚合值段 | union、tuple、array、enum 与 cast 语义 |
| 08 | 绑定/赋值段 | binding、assignment、member write 与 catch binding |
| 09 | 构造/初始化段 | constructor、object literal、default value 与 destructuring init |
| 10 | 表达式/控制流段 | 运算、调用、成员访问、if/match/loop/throw 等表达式发码 |
| 11 | 模块/程序段 | program 输入与 main/bin 入口约束 |

## 00 通用段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0001 | 调试显示类型缺失 | CE0001 | codegen: missing debug display type | 回到IE |
| CE0002 | 调试源码映射缺失 | CE0002 | codegen: missing debug source mapping for '%s' | 回到IE |
| CE0003 | 全局绑定内建推断类型不支持 | CE0019 | codegen: unsupported builtin inferred type for global binding '%.*s' | 消解 |
| CE0004 | 全局绑定推断类型声明缺失 | CE0020 | codegen: inferred type for global binding '%.*s' is missing its declaration | 回到IE |
| CE0005 | 全局绑定声明式推断类型不支持 | CE0021 | codegen: unsupported declared inferred type for global binding '%.*s' | 回到IE |
| CE0006 | 模块级绑定类型或初始化器缺失 | CE0022 | codegen: module-level binding requires an explicit type or initializer | 回到AE |
| CE0007 | 全局绑定推断类型事实缺失 | CE0023 | codegen: missing semantic type fact for inferred global binding '%.*s'; add an explicit type if this persists | 回到IE |
| CE0008 | 类型解析缺失 | CE0032 | codegen: unknown type '%.*s' | 回到AE |
| CE0009 | 类型引用种类未知 | CE0034 | codegen: unknown type reference kind | 回到IE |
| CE0010 | 字段内建推断类型不支持 | CE0035 | codegen: unsupported builtin inferred type for field | 消解 |
| CE0011 | 字段推断类型声明缺失 | CE0036 | codegen: inferred field type is missing its declaration | 消解 |
| CE0012 | 字段声明式推断类型不支持 | CE0037 | codegen: unsupported declared inferred field type | 回到AE |
| CE0013 | 字段类型或推断事实缺失 | CE0038 | codegen: field '%.*s' requires an explicit type or semantic inferred type | 回到AE |
| CE0014 | 标识符解析缺失 | CE0104 | codegen: identifier '%.*s' not found | 回到AE |
| CE0015 | self 上下文缺失 | CE0119 | codegen: 'self' used outside of method body | 回到AE |
| CE0016 | 静态方法注册缺失 | CE0150 | codegen: resolved static method was not registered | 回到IE |
| CE0017 | 具体返回类型确定失败 | CE0309 | codegen: cannot determine concrete return type | 消解 |
| CE0018 | 标量 subject key 无效 | CE0310 | codegen: invalid scalar subject key | 回到IE |
| CE0019 | 标量 subject storage kind 无效 | CE0311 | codegen: invalid scalar subject storage kind | 回到IE |
| CE0020 | 类型注册缺失 | CE0350 | codegen: internal: type not registered | 回到IE |
| CE0021 | 类型壳注册无诊断失败 | CE0352 | codegen: internal: type shell registration failed without diagnostic | 回到IE |
| CE0022 | 用户类型成员注册无诊断失败 | CE0354 | codegen: internal: user type member registration failed without diagnostic | 回到IE |
| CE0023 | 函数预注册无诊断失败 | CE0355 | codegen: internal: function pre-registration failed without diagnostic | 回到IE |
| CE0024 | 声明发射无诊断失败 | CE0356 | codegen: internal: declaration emission failed without diagnostic | 回到IE |

## 01 泛型段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0101 | 泛型约束 spec 支持约束 | CE0003 | codegen: generic constraint for '%.*s' must be a spec supported by codegen | 消解 |
| CE0102 | 泛型约束 spec 注册缺失 | CE0004 | codegen: internal: generic constraint spec was not registered | 回到IE |
| CE0103 | 泛型 type 方法 reified 依赖缺失 | CE0005 | codegen: no reified_type_dep found for generic type method call | 回到IE |
| CE0104 | 非泛型类型 reified 描述符误用 | CE0006 | codegen: cg_rtd_expr_for_type called on non-generic type | 回到IE |
| CE0105 | 泛型跨类型调用 reified 依赖缺失 | CE0007 | codegen: no reified_type_dep found for generic cross-type call | 回到IE |
| CE0106 | 泛型约束 spec 索引前移动错误 | CE0017 | codegen: internal: generic constraint spec moved before it could be indexed | 回到IE |
| CE0107 | 泛型约束 spec 索引越界 | CE0018 | codegen: internal: generic constraint spec index is out of range | 回到IE |
| CE0108 | 泛型类型实参数量约束 | CE0029 | codegen: generic type '%.*s' expects %zu type argument(s), got %zu | 回到AE |
| CE0109 | 泛型 spec 实参数量约束 | CE0030 | codegen: generic spec '%.*s' expects %zu type argument(s), got %zu | 回到AE |
| CE0110 | 泛型 type/spec 实例注册缺失 | CE0031 | codegen: generic type/spec instance '%.*s<...>' was not registered | 回到IE |
| CE0111 | 泛型 extern 参数单一外部表面约束 | CE0041 | codegen: generic extern func '%.*s' parameter '%.*s' does not lower to a single external surface | 继续CE |
| CE0112 | 泛型 extern 返回单一外部表面约束 | CE0042 | codegen: generic extern func '%.*s' return type does not lower to a single external surface | 继续CE |
| CE0113 | union-form 成员布局具体类型实参要求 | CE0045 | codegen: union-form spec member layout requires a concrete type argument | 继续CE |
| CE0114 | tuple 字段泛型描述符缺失 | CE0058 | codegen: missing generic descriptor for tuple field '%s' | 消解 |
| CE0115 | tuple 字段泛型初始化类型不匹配 | CE0059 | codegen: tuple field '%s' generic initializer type mismatch | 消解 |
| CE0116 | tuple 类型 reified 布局缺失 | CE0066 | codegen: tuple type '%s' requires reified layout but no | 消解 |
| CE0117 | 显式泛型目标消费前置约束 | CE0120 | codegen: explicit generic target must be consumed before emission | 消解 |
| CE0118 | 泛型直接调用 callable-form spec 约束要求 | CE0130 | codegen: generic direct call requires a callable-form spec constraint | 回到AE |
| CE0119 | 泛型方法约束 spec 支持约束 | CE0133 | codegen: generic method constraint for '%.*s' must be a spec supported by codegen | 消解 |
| CE0120 | 泛型 type 方法类型参数缺失 | CE0134 | codegen: internal: generic type method call missing method type parameters | 回到IE |
| CE0121 | 泛型方法类型实参数量约束 | CE0135 | codegen: method expects %zu type argument(s), got %zu | 回到AE |
| CE0122 | 泛型方法参数数量约束 | CE0136 | codegen: wrong argument count for generic method '%s' (expected %zu, got %zu) | 回到AE |
| CE0123 | 泛型方法类型实参推断失败 | CE0137 | codegen: cannot infer type argument %zu for generic method '%s' | 回到AE |
| CE0123 | 泛型方法类型实参推断失败 | CE0141 | codegen: cannot infer type argument %zu for generic method '%.*s' | 回到AE |
| CE0124 | 泛型方法具体返回类型确定失败 | CE0138 | codegen: cannot determine concrete generic method return type | 消解 |
| CE0125 | 泛型类型方法不存在约束 | CE0139 | codegen: generic type '%.*s' has no method '%.*s' | 回到AE |
| CE0126 | 泛型静态方法描述符上下文要求 | CE0142 | codegen: generic static method call requires an active generic descriptor context | 消解 |
| CE0127 | 泛型静态方法类型参数缺失 | CE0143 | codegen: internal: generic static method call missing type parameters | 回到IE |
| CE0128 | 泛型 builtin static fit 方法暂不支持 | CE0144 | codegen: generic builtin static fit methods are not supported yet | 消解 |
| CE0129 | 泛型静态方法类型实参数量约束 | CE0145 | codegen: static method expects %zu type argument(s), got %zu | 回到AE |
| CE0130 | 泛型静态方法变参最小实参数量约束 | CE0146 | codegen: too few arguments for variadic generic static method '%s' | 回到AE |
| CE0131 | 泛型静态方法参数数量约束 | CE0147 | codegen: wrong argument count for generic static method '%s' | 回到AE |
| CE0132 | 泛型静态方法类型实参推断失败 | CE0148 | codegen: cannot infer type argument %zu for generic static method '%s' | 回到AE |
| CE0133 | 泛型静态方法具体返回类型确定失败 | CE0149 | codegen: cannot determine generic static method return type | 消解 |
| CE0134 | 泛型方法调用 object-form spec 约束要求 | CE0155 | codegen: generic method call requires an object-form spec constraint | 回到AE |
| CE0135 | 泛型 spec 方法返回描述符缺失 | CE0158 | codegen: missing descriptor for generic spec method return | 消解 |
| CE0136 | builtin fit 泛型描述符发射目标限制 | CE0161 | codegen: builtin fit generic descriptor emission only supports array targets in this phase | 消解 |
| CE0137 | 泛型类型构造实例注册缺失 | CE0167 | codegen: generic type constructor instance for '%.*s' was not registered | 回到AE |
| CE0138 | 泛型类型字段不存在约束 | CE0171 | codegen: generic type '%.*s' has no field '%.*s' | 回到AE |
| CE0139 | 泛型成员访问 object-form spec 约束要求 | CE0172 | codegen: generic member access requires an object-form spec constraint | 回到AE |
| CE0140 | 泛型类型对象字面量实例注册缺失 | CE0179 | codegen: generic type object literal instance for '%.*s' was not registered | 回到IE |
| CE0141 | 泛型空数组字面量描述符上下文要求 | CE0184 | codegen: generic empty array literal requires an active generic descriptor | 消解 |
| CE0142 | 泛型 array-new 描述符上下文要求 | CE0187 | codegen: generic array-new requires an active generic descriptor | 消解 |
| CE0143 | 泛型数组索引描述符上下文要求 | CE0190 | codegen: generic array index requires an active generic descriptor | 消解 |
| CE0144 | 泛型数组元素赋值描述符缺失 | CE0240 | codegen: missing generic descriptor for array element assignment | 消解 |
| CE0145 | 泛型数组元素赋值同类型参数约束 | CE0241 | codegen: generic array element assignment requires a value with the same generic type parameter | 回到AE |
| CE0146 | tuple 数组元素 reified 布局要求 | CE0242 | codegen: tuple array element requires reified layout | 消解 |
| CE0147 | 泛型字段复合赋值具体数值类型约束 | CE0247 | codegen: compound assignment to generic field requires a concrete numeric field type | 回到AE |
| CE0148 | 泛型字段赋值同类型参数约束 | CE0248 | codegen: generic field assignment requires a value with the same generic type parameter | 回到AE |
| CE0149 | 泛型方法字段写入聚合描述符缺失 | CE0251 | codegen: missing aggregate descriptor for generic method field write | 消解 |
| CE0150 | 泛型成员赋值 object-form spec 约束要求 | CE0252 | codegen: generic member assignment requires an object-form spec constraint | 回到AE |
| CE0151 | 泛型类型实参转发描述符上下文要求 | CE0292 | codegen: generic type argument forwarding requires an active generic descriptor context | 消解 |
| CE0152 | 泛型类型实参跨约束面转发 witness 兼容性约束 | CE0293 | codegen: forwarding a generic type argument across a different constraint surface requires a parent-compatible witness surface (G6) | 消解 |
| CE0153 | 受约束泛型实参候选范围约束（含 builtin） | CE0294 | codegen: constrained generic type argument currently requires a concrete user type, concrete spec value, matching outer generic parameter, or concrete builtin type | 消解 |
| CE0154 | 受约束泛型实参候选范围约束（G6） | CE0295 | codegen: constrained generic type argument currently requires a concrete user type, concrete spec value, or matching outer generic parameter (G6) | 消解 |
| CE0155 | 平凡泛型实参 trivial descriptor 要求 | CE0296 | codegen: trivial generic type argument requires a trivial descriptor | 消解 |
| CE0156 | 托管泛型实参 type descriptor 要求 | CE0297 | codegen: managed generic type argument requires a type descriptor | 消解 |
| CE0157 | 聚合类型泛型实参 flatten 规则缺失 | CE0298 | codegen: aggregate type as generic type argument not yet supported (missing flatten rule) (G6) | 消解 |
| CE0158 | tuple 返回 reified 布局要求 | CE0299 | codegen: tuple return requires reified layout but no | 消解 |
| CE0159 | 泛型聚合返回发射内存不足 | CE0300 | codegen: out of memory while emitting generic aggregate return | 回到IE |
| CE0160 | 泛型聚合返回描述符缺失 | CE0301 | codegen: missing aggregate descriptor for generic aggregate return | 消解 |
| CE0161 | 泛型 extern 调用 extern 元数据缺失 | CE0302 | codegen: generic extern call is missing extern metadata | 回到IE |
| CE0162 | 泛型 extern 类型实参数量约束 | CE0303 | codegen: generic extern type argument count mismatch | 回到AE |
| CE0163 | 泛型 extern 类型实参从参数推断失败 | CE0304 | codegen: cannot infer generic extern type arguments for '%.*s' from argument %zu | 回到AE |
| CE0164 | 泛型 extern 类型实参推断失败 | CE0305 | codegen: cannot infer type argument %zu for generic extern '%.*s' | 回到AE |
| CE0165 | 泛型函数类型实参数量约束 | CE0306 | codegen: generic function '%s' expects %zu type argument(s), got %zu | 回到AE |
| CE0166 | 泛型函数具体参数类型确定失败 | CE0307 | codegen: cannot determine concrete parameter type for generic function '%s' | 消解 |
| CE0167 | 泛型函数类型实参推断失败 | CE0308 | codegen: cannot infer type argument %zu for generic function '%s' | 回到AE |
| CE0168 | spec 值满足泛型约束时 spec form 不匹配 | CE0312 | codegen: spec value '%s' cannot satisfy generic constraint spec '%s' through mismatched spec forms | 回到AE |
| CE0169 | spec 值满足泛型约束时 callable slot witness 适配失败 | CE0313 | codegen: spec value '%s' cannot satisfy generic constraint spec '%s' through callable slot witness adaptation | 消解 |
| CE0170 | spec 值满足泛型约束时 slot witness 适配失败 | CE0314 | codegen: spec value '%s' cannot satisfy generic constraint spec '%s' through slot witness adaptation | 消解 |
| CE0171 | 泛型实例收集无诊断失败 | CE0353 | codegen: internal: generic instance collection failed without diagnostic | 回到IE |
| CE0172 | 共享泛型方法发射失败 | CE0369 | codegen: internal: failed to emit shared generic method '%.*s' | 回到IE |
| CE0173 | 泛型实例方法 origin type 缺失 | CE0370 | codegen: internal: generic instance method missing origin type | 回到IE |
| CE0174 | 泛型方法包装发射失败 | CE0371 | codegen: internal: failed to emit generic method wrapper '%s.%s' | 回到IE |

## 02 聚合描述符段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0201 | capture cell 聚合描述符缺失 | CE0008 | codegen: missing aggregate descriptor for capture cell | 消解 |
| CE0202 | capture cell 默认初始化规则缺失 | CE0009 | codegen: missing aggregate default-init rule for capture cell | 消解 |
| CE0203 | union-form 聚合成员描述符缺失 | CE0055 | codegen: union-form aggregate member is missing a descriptor | 消解 |
| CE0204 | union 默认聚合成员描述符缺失 | CE0057 | codegen: union default aggregate member is missing a descriptor | 消解 |
| CE0205 | tuple 字段聚合描述符缺失 | CE0060 | codegen: missing aggregate descriptor for tuple field '%s' | 消解 |
| CE0205 | tuple 字段聚合描述符缺失 | CE0367 | codegen: tuple field '%s' has no aggregate descriptor | 消解 |
| CE0206 | tuple 字段默认初始化规则缺失 | CE0061 | codegen: missing aggregate default-init rule for tuple field '%s' | 消解 |
| CE0206 | tuple 字段默认初始化规则缺失 | CE0368 | codegen: missing tuple field aggregate default initializer | 消解 |
| CE0207 | tuple spec coercion 聚合描述符缺失 | CE0068 | codegen: missing tuple aggregate descriptor for spec coercion | 消解 |
| CE0208 | 赋值目标聚合描述符缺失 | CE0074 | codegen: missing aggregate descriptor for assignment | 消解 |
| CE0209 | spec 返回聚合描述符缺失 | CE0075 | codegen: missing aggregate descriptor for spec return | 消解 |
| CE0210 | tuple 相等性聚合描述符缺失 | CE0085 | codegen: tuple type has no aggregate descriptor for equality | 消解 |
| CE0211 | 变参元素类型聚合描述符缺失 | CE0123 | codegen: missing aggregate descriptor for variadic element type | 消解 |
| CE0212 | spec 数组元素描述符缺失 | CE0183 | codegen: missing aggregate descriptor for spec array element | 消解 |
| CE0213 | array-new 元素描述符缺失 | CE0186 | codegen: missing aggregate descriptor for array-new element type | 消解 |
| CE0214 | if/match 结果槽聚合描述符缺失 | CE0196 | codegen: missing aggregate descriptor for if/match result slot | 消解 |
| CE0215 | try 表达式结果聚合描述符缺失 | CE0198 | codegen: missing aggregate descriptor for try-expression result | 消解 |
| CE0216 | if 表达式结果聚合描述符缺失 | CE0200 | codegen: missing aggregate descriptor for if-expression result | 消解 |
| CE0217 | if 表达式结果默认初始化规则缺失 | CE0202 | codegen: missing aggregate default-init rule for if-expression result | 消解 |
| CE0218 | match 表达式结果聚合描述符缺失 | CE0208 | codegen: missing aggregate descriptor for match expression result | 消解 |
| CE0219 | match 表达式结果默认初始化规则缺失 | CE0209 | codegen: missing aggregate default-init rule for match expression result | 消解 |
| CE0220 | try 表达式结果默认初始化规则缺失 | CE0216 | codegen: missing aggregate default-init rule for try-expression result | 消解 |
| CE0221 | spec 数组默认零值描述符缺失 | CE0224 | codegen: missing aggregate descriptor for spec array default-zero | 消解 |
| CE0222 | local binding 聚合描述符缺失 | CE0228 | codegen: missing aggregate descriptor for local binding | 消解 |
| CE0223 | 字段聚合描述符缺失 | CE0232 | codegen: missing aggregate descriptor for field '%s' | 消解 |
| CE0224 | 字段默认初始化规则缺失 | CE0233 | codegen: missing aggregate default-init rule for field '%s' | 消解 |
| CE0225 | spec local 聚合描述符缺失 | CE0235 | codegen: missing aggregate descriptor for spec local | 消解 |
| CE0226 | 聚合默认初始化规则缺失 | CE0236 | codegen: missing aggregate default-init rule | 消解 |
| CE0227 | spec 数组元素写入描述符缺失 | CE0243 | codegen: missing aggregate descriptor for spec array element write | 消解 |
| CE0228 | static binding 写入聚合描述符缺失 | CE0246 | codegen: missing aggregate descriptor for static binding write | 消解 |
| CE0229 | member assignment 聚合描述符缺失 | CE0257 | codegen: missing aggregate descriptor for member assignment | 消解 |
| CE0230 | module assignment 聚合描述符缺失 | CE0261 | codegen: missing aggregate descriptor for module assignment | 消解 |
| CE0231 | local assignment 聚合描述符缺失 | CE0263 | codegen: missing aggregate descriptor for local assignment | 消解 |
| CE0232 | spec for/in 元素描述符缺失 | CE0278 | codegen: missing aggregate descriptor for spec for/in element | 消解 |
| CE0233 | 对象异常载荷描述符缺失 | CE0281 | codegen: object exception payload is missing a descriptor | 消解 |
| CE0234 | reifiable 依赖描述符名称解析失败 | CE0291 | codegen: failed to resolve descriptor name for reifiable dep | 回到IE |
| CE0235 | spec 字段写入聚合描述符缺失 | CE0348 | codegen: missing aggregate descriptor for spec field write | 消解 |
| CE0236 | module binding 默认初始化规则缺失 | CE0359 | codegen: missing aggregate default-init rule for module binding | 消解 |
| CE0237 | module binding 聚合描述符缺失 | CE0360 | codegen: missing aggregate descriptor for module binding | 消解 |
| CE0238 | aggregate field 描述符缺失（未知聚合种类） | CE0362 | codegen: aggregate field has no descriptor (unknown aggregate kind) | 回到IE |
| CE0239 | aggregate field 描述符符号缺失（未知聚合种类） | CE0363 | codegen: aggregate field has no descriptor symbol (unknown aggregate kind) | 回到IE |
| CE0240 | tuple 字段托管相等性描述符缺失 | CE0365 | codegen: tuple field '%s' has no managed equality descriptor | 消解 |
| CE0241 | tuple 字段聚合相等性描述符缺失 | CE0366 | codegen: tuple field '%s' has no aggregate equality descriptor | 消解 |

## 03 规约/见证段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0301 | spec coercion 目标实例解析失败 | CE0024 | codegen: coercion target did not resolve to a concrete spec instance | 回到IE |
| CE0302 | spec 成员种类暂不支持 | CE0039 | codegen: spec member kind not supported (Step 4b-α only handles fields/methods) | 消解 |
| CE0303 | spec 父项 object-form 解析失败 | CE0046 | codegen: spec parent did not resolve to an object-form spec | 回到IE |
| CE0304 | spec 父项递归注册检测 | CE0047 | codegen: recursive spec parent registration detected | 回到AE |
| CE0305 | spec 相等性要求聚合 spec 操作数 | CE0084 | codegen: spec equality requires aggregate spec operands | 回到AE |
| CE0306 | callable-form spec 目标注册缺失 | CE0100 | codegen: callable lambda coercion target was not registered as a callable-form spec | 回到IE |
| CE0306 | callable-form spec 目标注册缺失 | CE0105 | codegen: callable coercion target was not registered as a callable-form spec | 回到IE |
| CE0307 | callable value 调用需要 callable-form spec 类型 | CE0127 | codegen: callable value call requires a callable-form spec type | 回到AE |
| CE0308 | variadic callable constraint 最小实参数量约束 | CE0131 | codegen: too few arguments for variadic callable constraint '%s' (need at least %zu, got %zu) | 回到AE |
| CE0309 | callable constraint 参数数量约束 | CE0132 | codegen: wrong argument count for callable constraint '%s' (expected %zu, got %zu) | 回到AE |
| CE0310 | spec 方法不存在约束 | CE0156 | codegen: spec '%s' has no method '%.*s' | 回到AE |
| CE0311 | spec 字段不存在约束 | CE0173 | codegen: spec '%s' has no field '%.*s' | 回到AE |
| CE0312 | spec 方法参数数量约束 | CE0157 | codegen: wrong argument count for spec method '%s' (expected %zu, got %zu) | 回到AE |
| CE0313 | callable-form cast 源操作数类型约束 | CE0191 | codegen: callable-form cast operand must be a callable-form spec value | 回到IE |
| CE0314 | spec coercion 源类型缺失 | CE0217 | codegen: spec coercion source type is missing | 回到IE |
| CE0315 | spec coercion 引用越界当前 codegen scope | CE0218 | codegen: spec coercion references type outside current codegen scope | 回到IE |
| CE0316 | scalar spec coercion 源 kind 不支持 | CE0219 | codegen: scalar spec coercion has unsupported source kind | 回到IE |
| CE0317 | object-form spec coercion 源 kind 无效 | CE0220 | codegen: object-form spec coercion source kind is invalid | 消解 |
| CE0318 | spec 字段可写性前置约束 | CE0253 | codegen: spec field '%s' is not declared `var` | 回到AE |
| CE0319 | spec 字段复合赋值数值类型约束 | CE0254 | codegen: compound spec field assignment requires a numeric field type | 回到AE |
| CE0320 | spec 字段复合赋值运算符不支持 | CE0255 | codegen: unsupported compound spec field assignment operator | 回到AE |
| CE0321 | spec fat value 值结构名缺失 | CE0284 | codegen: spec fat value is missing its value struct name | 回到IE |
| CE0322 | type 对 spec 成员实现缺失 | CE0315 | codegen: type '%s' is missing an implementation for spec '%s' member '%s' | 回到AE |
| CE0323 | type 字段与 spec 字段类型不匹配 | CE0316 | codegen: field '%s' on type '%s' does not match spec '%s' field type | 回到AE |
| CE0324 | non-type subject witness slot 数量不匹配 | CE0318 | codegen: internal: witness slot count mismatch for non-type subject | 回到IE |
| CE0325 | spec 成员实现缺失 | CE0319 | codegen: missing implementation for spec member '%s' | 回到AE |
| CE0326 | enum 对 spec 字段满足缺少字段支持 | CE0321 | codegen: enum '%.*s' cannot satisfy spec field '%s' without field support | 回到AE |
| CE0327 | enum 对 spec 成员实现缺失 | CE0322 | codegen: enum '%.*s' is missing an implementation for spec '%s' member '%s' | 回到AE |
| CE0328 | enum 对 spec 方法存在多个可见实现 | CE0323 | codegen: enum '%.*s' has multiple visible implementations of method '%s' required by spec '%s' | 回到AE |
| CE0329 | witness 源类型未在当前模块注册 | CE0326 | codegen: witness source type is not registered in current module | 回到IE |
| CE0330 | object-form spec coercion subject key 无效 | CE0327 | codegen: invalid subject key for object-form spec coercion | 回到IE |
| CE0331 | object-form spec coercion subject key 未知 | CE0328 | codegen: object-form spec coercion has unknown subject key | 回到IE |
| CE0332 | object-form spec coercion 语义 witness 缺失 | CE0329 | codegen: missing semantic witness for object-form spec coercion | 回到IE |
| CE0333 | witness slot 数量不匹配（对象/类型对） | CE0339 | codegen: internal: witness slot count mismatch for (%s, %s) | 回到IE |
| CE0334 | 内部 type 满足 spec 时方法缺失 | CE0342 | codegen: internal: type '%s' has no method '%s' to satisfy spec '%s' | 回到IE |
| CE0335 | 内部 type 满足 spec 时字段缺失 | CE0343 | codegen: internal: type '%s' has no field '%s' to satisfy spec '%s' | 回到IE |
| CE0336 | spec 方法实现载体约束 | CE0346 | codegen: spec method '%s' must be implemented by a method on '%s' (Step 4b-α) | 消解 |
| CE0337 | spec 字段满足载体约束 | CE0347 | codegen: spec field '%s' must be satisfied by a field on '%s' | 消解 |

## 04 Fit段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0401 | fit spec 用户 spec 解析失败 | CE0048 | codegen: fit spec did not resolve to a known user spec | 回到AE |
| CE0402 | fit 目标 builtin/array 解析失败 | CE0049 | codegen: fit target did not resolve to a builtin/array type | 回到AE |
| CE0403 | fit 目标缺失 | CE0050 | codegen: fit target is missing | 回到AE |
| CE0404 | fit 目标形态限制（仅 named/array） | CE0051 | codegen: only named or array fit targets are supported | 消解 |
| CE0405 | fit 目标用户类型解析失败 | CE0052 | codegen: fit target did not resolve to a known user type | 回到AE |
| CE0405 | fit 目标用户类型解析失败 | CE0053 | codegen: fit target type '%.*s' is not a known user type | 回到AE |
| CE0406 | fit body 仅支持方法成员 | CE0054 | codegen: only methods are supported in fit bodies | 回到AE |
| CE0407 | builtin/array fit 方法不存在约束 | CE0159 | codegen: builtin/array fit has no method '%.*s' | 回到AE |
| CE0408 | builtin fit 返回类型实例化失败 | CE0162 | codegen: failed to instantiate builtin fit return type | 消解 |
| CE0409 | type 对 spec 方法存在多个可见 fit 实现 | CE0317 | codegen: type '%s' has multiple visible implementations of method '%s' required by spec '%s' (one or more fits and/or the type itself) | 消解 |
| CE0410 | non-type subject 仅支持 fit-method spec 成员 | CE0320 | codegen: non-type subject key currently supports fit-method spec members only | 消解 |
| CE0411 | fit 实现与 object-form spec coercion 源不匹配 | CE0324 | codegen: fit implementation does not match object-form spec coercion source | 回到IE |
| CE0412 | fit 方法注册缺失 | CE0325 | codegen: fit method '%s' was not registered | 回到IE |
| CE0413 | fit 声明注册缺失 | CE0340 | codegen: internal: fit decl for spec '%s' member '%s' not registered for type '%s' | 回到IE |
| CE0414 | fit body 中方法缺失 | CE0341 | codegen: internal: fit method '%s' not found in fit body for type '%s' | 回到IE |
| CE0415 | spec 字段不能由 fit 方法满足 | CE0344 | codegen: spec field '%s' cannot be satisfied by a fit method | 消解 |
| CE0416 | fit 绑定注册缺失 | CE0345 | codegen: internal: fit binding for spec '%s' member '%s' not registered for type '%s' | 回到IE |
| CE0417 | fit 注册缺失 | CE0351 | codegen: internal: fit not registered | 回到IE |

## 05 可调用对象段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0501 | callable 函数值请求无效 | CE0026 | codegen: invalid callable function-value request | 回到IE |
| CE0502 | callable 函数值源函数未注册 | CE0027 | codegen: callable value source function was not registered | 回到IE |
| CE0503 | callable 方法值请求无效 | CE0028 | codegen: invalid callable method-value request | 回到IE |
| CE0504 | lambda capture 未降级为 capture cell | CE0097 | codegen: lambda capture was not lowered to a capture cell | 回到IE |
| CE0505 | lambda 参数名溢出 | CE0098 | codegen: lambda argument name overflow | 回到IE |
| CE0506 | callable lambda coercion 语义数据缺失 | CE0099 | codegen: callable lambda coercion is missing lambda semantic data | 回到IE |
| CE0507 | callable lambda coercion 参数数量不匹配 | CE0101 | codegen: callable lambda coercion parameter count mismatch | 回到AE |
| CE0508 | lambda 命名 capture 未降级 | CE0102 | codegen: lambda capture '%.*s' was not lowered to a capture cell | 回到IE |
| CE0509 | lambda self capture 未降级 | CE0103 | codegen: lambda self capture was not lowered to a capture cell | 回到IE |
| CE0510 | callable coercion 仅支持 top-level function | CE0106 | codegen: only top-level function callable coercions are supported in this step | 消解 |
| CE0511 | callable method coercion 需要 member expression | CE0110 | codegen: callable method coercion requires a member expression | 回到AE |
| CE0512 | callable method coercion 解析数据缺失 | CE0111 | codegen: callable method coercion is missing semantic resolution data | 回到IE |
| CE0513 | callable method coercion 源值必须为对象 | CE0112 | codegen: callable method coercion source must be an object value | 回到AE |
| CE0514 | callable method coercion receiver 类型不匹配 | CE0113 | codegen: callable method coercion receiver type does not match resolved owner type | 回到AE |
| CE0515 | callable method coercion 源方法未注册 | CE0114 | codegen: callable method coercion source method was not registered | 回到IE |
| CE0516 | callable-form 源目标签名匹配约束 | CE0115 | codegen: callable-form coercion requires source/target callable signatures to match | 回到AE |
| CE0517 | callable-form 源值类型约束 | CE0116 | codegen: callable-form coercion source must be a callable value | 回到AE |
| CE0518 | callable-form lambda/method coercion 暂不支持 | CE0117 | codegen: callable-form lambda/method coercion not yet supported in this step | 消解 |
| CE0519 | variadic callable 最小实参数量约束 | CE0128 | codegen: too few arguments for variadic callable '%s' (need at least %zu, got %zu) | 回到AE |
| CE0520 | callable 参数数量约束 | CE0129 | codegen: wrong argument count for callable '%s' (expected %zu, got %zu) | 回到AE |
| CE0521 | imported binding 不可调用 | CE0154 | codegen: imported binding '%.*s' is not callable | 回到AE |
| CE0522 | 未解析 callable type 默认值不可生成 | CE0223 | codegen: cannot produce default value for unresolved callable type | 消解 |

## 06 ABI/Extern段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0601 | extern 注解参数字符串字面量解析要求 | CE0010 | codegen: extern annotation %s argument must resolve to a string literal | 回到AE |
| CE0602 | extern 注解字符串字面量格式错误 | CE0011 | codegen: malformed extern annotation string literal | 消解 |
| CE0603 | extern 注解字符串转义无效 | CE0012 | codegen: unknown extern annotation string escape '\%c' | 消解 |
| CE0604 | extern 注解参数缺失 | CE0013 | codegen: extern annotation %s argument is missing | 回到AE |
| CE0605 | extern 注解参数可见字符串绑定要求 | CE0014 | codegen: extern annotation %s argument must be a string literal or visible let binding | 消解 |
| CE0606 | extern 注解参数直接字符串初始化要求 | CE0015 | codegen: extern annotation %s argument must be a string literal or visible let binding initialized directly with a string literal | 消解 |
| CE0607 | ABI pointee lowering 不支持 | CE0033 | codegen: this pointee type does not support ABI pointer lowering | 继续CE |
| CE0608 | @runtime extern 合同声明缺失 | CE0040 | codegen: @runtime extern func '%.*s' is not declared by runtime contract | 继续CE |
| CE0609 | ABI-compatible array element data-pointer lowering 不支持 | CE0091 | codegen: this ABI-compatible array element type does not support data-pointer lowering | 继续CE |
| CE0610 | ABI pointer formation 操作数类型不支持 | CE0092 | codegen: this operand type does not support ABI pointer formation; only string, ABI scalar, fielded @abi value, and ABI-compatible array operands are allowed here | 继续CE |
| CE0611 | ABI function pointer 目标 callable-form spec 注册缺失 | CE0107 | codegen: ABI function pointer target was not registered as a callable-form spec | 回到IE |
| CE0612 | ABI function pointer 来源形态限制 | CE0108 | codegen: ABI function pointers currently support only top-level @abi functions | 消解 |
| CE0613 | ABI function pointer 源函数注册缺失 | CE0109 | codegen: ABI function pointer source function was not registered | 回到IE |
| CE0614 | extern 模块级绑定暂不支持 | CE0349 | codegen: extern module-level bindings not supported in Phase 1A | 消解 |

## 07 聚合值段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0701 | union coercion 目标实例解析失败 | CE0025 | codegen: union coercion target did not resolve to a concrete union-form spec | 回到IE |
| CE0702 | union-form 规范化成员缺失 | CE0044 | codegen: union-form spec has no normalized members | 回到IE |
| CE0703 | union-form 成员缺失 | CE0056 | codegen: union-form spec has no members | 回到IE |
| CE0704 | union coercion 目标成员无效 | CE0118 | codegen: union coercion target member is invalid | 回到IE |
| CE0705 | union-form match 标签规范化成员约束 | CE0268 | codegen: union-form match label is not a normalized member | 回到AE |
| CE0706 | 枚举发射跟踪内存不足 | CE0016 | codegen: out of memory tracking emitted enum | 回到IE |
| CE0707 | 枚举项不存在约束 | CE0170 | codegen: enum '%.*s' has no item '%.*s' | 回到AE |
| CE0708 | 枚举默认值候选项缺失 | CE0221 | codegen: enum has no items for default value | 回到IE |
| CE0709 | 枚举默认值发射内存不足 | CE0222 | codegen: out of memory emitting enum default value | 回到IE |
| CE0710 | tuple 聚合所有权状态未实体化 | CE0062 | codegen: internal tuple aggregate ownership state was not materialized | 回到IE |
| CE0711 | tuple 聚合字段初始化类型不匹配 | CE0063 | codegen: tuple aggregate field initializer type mismatch | 回到IE |
| CE0712 | tuple 字面量命名目标类型要求 | CE0064 | codegen: tuple literal requires a named tuple target type | 回到AE |
| CE0712 | tuple 字面量命名目标类型要求 | CE0121 | codegen: tuple literal requires an explicit named tuple target type | 回到AE |
| CE0713 | tuple 字面量元素数量匹配约束 | CE0065 | codegen: tuple literal arity does not match target tuple type '%s' | 回到AE |
| CE0714 | tuple spec coercion box 元数据缺失 | CE0067 | codegen: tuple spec coercion is missing box metadata | 回到IE |
| CE0715 | tuple cast 源值类型约束 | CE0069 | codegen: tuple cast source must be a tuple value | 回到AE |
| CE0716 | tuple cast 元素数量匹配约束 | CE0070 | codegen: tuple cast arity mismatch | 回到AE |
| CE0717 | tuple 字段不存在约束 | CE0174 | codegen: tuple type '%s' has no field '%.*s' | 回到AE |
| CE0718 | 解构源 tuple 值要求 | CE0231 | codegen: destructuring source must be a tuple value | 回到AE |
| CE0719 | iterator 返回 tuple 形态约束 | CE0276 | codegen: @iterator return type must be a 2-field tuple | 回到AE |
| CE0720 | tuple box witness slot 数量不匹配 | CE0330 | codegen: internal: witness slot count mismatch for tuple box (%s, %s) | 回到IE |
| CE0721 | tuple 对 spec 成员实现缺失 | CE0331 | codegen: tuple type '%s' is missing an implementation for spec '%s' member '%s' | 回到AE |
| CE0722 | tuple fit 声明注册缺失 | CE0332 | codegen: internal: tuple fit decl for spec '%s' member '%s' not registered for type '%s' | 回到IE |
| CE0723 | tuple fit body 方法缺失 | CE0333 | codegen: internal: tuple fit method '%s' not found in fit body for type '%s' | 回到IE |
| CE0724 | tuple 对 spec 字段满足缺失 | CE0334 | codegen: internal: tuple type '%s' has no field '%s' to satisfy spec '%s' | 回到AE |
| CE0725 | tuple 对 spec 方法满足需 fit 方法 | CE0335 | codegen: tuple type '%s' cannot satisfy spec method '%s' without a fit method | 回到AE |
| CE0726 | tuple spec 成员需由 fit 方法实现 | CE0336 | codegen: tuple spec member '%s' must be implemented by a fit method | 回到AE |
| CE0727 | tuple spec 字段需由 tuple 字段满足 | CE0337 | codegen: tuple spec field '%s' must be satisfied by a tuple field | 回到AE |
| CE0728 | tuple 不可变字段不能满足 var spec 字段 | CE0338 | codegen: tuple fields are immutable and cannot satisfy var spec field '%s' | 回到AE |
| CE0729 | tuple 相等函数名缺失 | CE0364 | codegen: missing tuple equality function name | 回到IE |
| CE0730 | 空数组元素类型要求 | CE0182 | codegen: empty array literal needs an explicit element type | 回到AE |
| CE0731 | 数组字面量元素同类型约束 | CE0185 | codegen: heterogeneous array literal (all elements must share a type) | 回到AE |
| CE0732 | 数组索引目标类型约束 | CE0188 | codegen: indexing requires an array value | 回到AE |
| CE0733 | 数组索引整数约束 | CE0189 | codegen: array index must be an integer | 回到AE |
| CE0734 | array cast 降级类型一致性约束 | CE0192 | codegen: array cast requires the same lowered array type | 回到AE |
| CE0735 | 类型转换种类支持约束 | CE0193 | codegen: only numeric/bool casts supported in 1A iter 1 | 消解 |
| CE0736 | 类型转换操作数类型约束 | CE0194 | codegen: cast operand must be numeric/bool | 回到AE |

## 08 绑定/赋值段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0801 | imported binding 不可变赋值约束 | CE0071 | codegen: cannot assign to immutable imported binding '%.*s' | 回到AE |
| CE0802 | 绑定复合赋值数值类型约束 | CE0072 | codegen: compound assignment requires a numeric binding type | 回到AE |
| CE0803 | 绑定复合赋值运算符不支持 | CE0073 | codegen: unsupported compound assignment operator | 回到AE |
| CE0804 | unknown catch 绑定注册失败 | CE0212 | codegen: failed to register unknown catch binding | 回到IE |
| CE0805 | catch 绑定类型缺失 | CE0213 | codegen: missing catch binding type | 回到IE |
| CE0806 | scalar catch 绑定载荷字段缺失 | CE0214 | codegen: missing scalar catch binding payload field | 回到IE |
| CE0807 | 索引赋值目标数组类型约束 | CE0237 | codegen: indexed assignment requires an array value | 回到AE |
| CE0808 | 索引复合赋值元素数值类型约束 | CE0238 | codegen: compound indexed assignment requires a numeric element type | 回到AE |
| CE0809 | 索引复合赋值运算符不支持 | CE0239 | codegen: unsupported compound indexed assignment operator | 回到AE |
| CE0810 | static binding 不可变赋值约束 | CE0244 | codegen: cannot assign to immutable static binding '%s.%s' | 回到AE |
| CE0811 | static binding 复合赋值数值类型约束 | CE0245 | codegen: compound assignment requires a numeric static binding type | 回到AE |
| CE0812 | 成员复合赋值字段数值类型约束 | CE0249 | codegen: compound member assignment requires a numeric field type | 回到AE |
| CE0813 | 成员复合赋值运算符不支持 | CE0250 | codegen: unsupported compound member assignment operator | 回到AE |
| CE0814 | 成员赋值目标对象类型约束 | CE0256 | codegen: member assignment on non-object value | 回到AE |
| CE0815 | 赋值目标形态支持约束 | CE0258 | codegen: only identifier or member assignments supported in this iteration | 消解 |
| CE0816 | 赋值目标标识符未定义 | CE0259 | codegen: assignment to undefined identifier '%.*s' | 回到AE |
| CE0817 | module binding 不可变赋值约束 | CE0260 | codegen: cannot assign to immutable module binding '%s' | 回到AE |
| CE0818 | local binding 复合赋值数值类型约束 | CE0262 | codegen: compound assignment requires a numeric local type | 回到AE |
| CE0819 | imported public binding 类型缺失 | CE0287 | codegen: imported public binding surface is missing a type | 回到IE |
| CE0820 | module binding ensure-init 发射无诊断失败 | CE0357 | codegen: internal: module binding ensure-init emission failed without diagnostic | 回到IE |
| CE0821 | type static binding ensure-init 发射无诊断失败 | CE0358 | codegen: internal: type static binding ensure-init emission failed without diagnostic | 回到IE |
| CE0822 | module binding ensure-init 发射时序错误 | CE0361 | codegen: internal: module binding ensure-init emitted before registration | 回到IE |

## 09 构造/初始化段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE0901 | finalizer 重复声明 | CE0043 | codegen: type already declares a finalizer | 回到AE |
| CE0902 | 构造器解析前置约束 | CE0076 | codegen: constructor arguments require a resolved user-defined constructor | 回到AE |
| CE0903 | 构造目标参数数量约束 | CE0077 | codegen: wrong argument count for constructor '%s' (expected %zu, got %zu) | 回到AE |
| CE0903 | 构造目标参数数量约束 | CE0126 | codegen: wrong argument count for '%.*s' (expected %zu, got %zu) | 回到AE |
| CE0904 | 构造调用目标类型未知 | CE0168 | codegen: unknown type '%.*s' in constructor call | 回到AE |
| CE0905 | 构造器注册缺失 | CE0169 | codegen: resolved constructor for type '%s' was not registered | 回到IE |
| CE0906 | 对象字面量目标缺失 | CE0177 | codegen: missing object literal target | 回到IE |
| CE0907 | 对象字面量目标类型未知 | CE0178 | codegen: unknown type '%.*s' in object literal | 回到AE |
| CE0908 | 对象字面量目标构造形态限制 | CE0180 | codegen: only direct type constructor targets are supported for object literals | 消解 |
| CE0909 | 对象字面量字段重复 | CE0181 | codegen: duplicate field '%s' in object literal | 回到AE |
| CE0910 | 未解析对象类型默认零值生成失败 | CE0225 | codegen: cannot default-zero an unresolved object type | 消解 |
| CE0911 | 含引用环类型默认零值禁用 | CE0226 | codegen: type '%s' contains reference cycles and has no default zero value; provide an explicit initializer | 继续CE |
| CE0912 | 类型默认值生成失败 | CE0227 | codegen: cannot produce default value for this type | 继续CE |
| CE0913 | 解构绑定初始化器缺失 | CE0229 | codegen: destructuring binding requires an initializer | 回到AE |
| CE0914 | 解构绑定元素数量匹配约束 | CE0230 | codegen: destructuring arity mismatch | 回到AE |
| CE0915 | 绑定类型或初始化器缺失 | CE0234 | codegen: binding without type or initializer not supported | 回到AE |
| CE0916 | throw 标量装箱构造器缺失 | CE0285 | codegen: missing scalar box constructor for throw payload | 消解 |
| CE0917 | 成员展开来源对象发码前提 | CE0377 | codegen: member mix source field is not lowerable、codegen: member mix source construction is not an object value | 回到IE |
| CE0918 | 成员展开来源 reified 布局缺失 | CE0378 | codegen: member mix source requires missing reified layout | 回到IE |
| CE0919 | 成员展开来源字段元数据缺失 | CE0379 | codegen: member mix source field index is missing、codegen: member mix source field is missing | 回到IE |

## 10 表达式/控制流段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE1001 | 字符串字面量格式错误 | CE0078 | codegen: malformed string literal | 消解 |
| CE1002 | 字符串 `\x` 转义长度约束 | CE0079 | codegen: invalid \x escape: expected 2 hex digits | 消解 |
| CE1003 | 字符串 `\x` 转义字符约束 | CE0080 | codegen: invalid \x escape: expected hex digit | 消解 |
| CE1004 | 字符串转义序列不支持 | CE0081 | codegen: unknown string escape '\%c' | 消解 |
| CE1005 | 字面量种类暂不支持 | CE0082 | codegen: unsupported literal kind | 消解 |
| CE1006 | 数值运算操作数类型约束 | CE0083 | codegen: cannot apply numeric op to non-numeric operands | 回到AE |
| CE1007 | 二元运算符不支持 | CE0086 | codegen: unsupported binary operator | 消解 |
| CE1008 | 短路逻辑运算 bool 操作数约束 | CE0087 | codegen: && / &#124;&#124; require bool operands | 回到AE |
| CE1009 | 有序比较数值类型约束 | CE0088 | codegen: ordering comparisons require numeric operands | 回到AE |
| CE1010 | float modulo 不支持 | CE0089 | codegen: unsupported float modulo operation | 回到AE |
| CE1011 | 一元 `&` 操作数类型缺失 | CE0090 | codegen: unary '&' is missing an operand type | 回到AE |
| CE1012 | 一元运算符不支持 | CE0093 | codegen: unsupported unary operator | 消解 |
| CE1013 | 逻辑非 bool 操作数约束 | CE0094 | codegen: '!' requires bool operand | 回到AE |
| CE1014 | 按位非 integer 操作数约束 | CE0095 | codegen: '~' requires integer operand | 回到AE |
| CE1015 | 一元正负 numeric 操作数约束 | CE0096 | codegen: unary +/- requires numeric operand | 回到AE |
| CE1016 | 表达式 kind 暂不支持 | CE0122 | codegen: expression kind not yet supported in this iteration | 消解 |
| CE1017 | 函数解析缺失 | CE0124 | codegen: undefined function '%.*s' | 回到AE |
| CE1018 | 变参函数最小实参数量约束 | CE0125 | codegen: too few arguments for variadic function '%.*s' (need at least %zu, got %zu) | 回到AE |
| CE1019 | 方法参数数量约束 | CE0140 | codegen: wrong argument count for method '%.*s' (expected %zu, got %zu) | 回到AE |
| CE1019 | 方法参数数量约束 | CE0160 | codegen: wrong argument count for method '%s' (expected %zu, got %zu) | 回到AE |
| CE1020 | 静态方法参数数量约束 | CE0151 | codegen: wrong argument count for static method '%s' (expected %zu, got %zu) | 回到AE |
| CE1021 | 变参静态方法最小实参数量约束 | CE0152 | codegen: too few arguments for variadic static method '%s' (need at least %zu, got %zu) | 回到AE |
| CE1022 | 静态方法返回类型确定失败 | CE0153 | codegen: failed to determine static method return type | 消解 |
| CE1023 | 方法调用目标对象类型约束 | CE0163 | codegen: method call on non-object value | 回到AE |
| CE1024 | 类型方法不存在约束 | CE0164 | codegen: type '%s' has no method '%.*s' | 回到AE |
| CE1025 | 变参方法最小实参数量约束 | CE0165 | codegen: too few arguments for variadic method '%s' (need at least %zu, got %zu) | 回到AE |
| CE1026 | 调用表达式形态支持约束 | CE0166 | codegen: only direct or method calls supported in this iteration | 消解 |
| CE1027 | 成员访问目标对象类型约束 | CE0175 | codegen: member access on non-object value | 回到AE |
| CE1028 | 类型字段不存在约束 | CE0176 | codegen: type '%s' has no field '%.*s' | 回到AE |
| CE1029 | if 表达式分支类型一致性约束 | CE0195 | codegen: if-expression branches yield mismatched types | 回到AE |
| CE1030 | try 表达式分支类型一致性约束 | CE0197 | codegen: try/catch branches yield mismatched types | 回到AE |
| CE1031 | if 表达式分支结果语句约束 | CE0199 | codegen: if-expression branches must end with an expression statement | 回到AE |
| CE1032 | if 表达式条件 bool 约束 | CE0201 | codegen: if-expression condition must be bool | 回到AE |
| CE1033 | match 区间标签目标整型约束 | CE0203 | codegen: range labels apply to integer match targets only | 回到AE |
| CE1034 | match 标签 kind 未知 | CE0204 | codegen: unknown match label kind | 消解 |
| CE1035 | match 表达式 else 分支存在性约束 | CE0205 | codegen: match expression requires an else branch | 回到AE |
| CE1036 | match 表达式 else 分支结果语句约束 | CE0206 | codegen: match expression else branch must end with an expression statement | 回到AE |
| CE1037 | match 分支结果语句约束 | CE0207 | codegen: match branch must end with an expression statement | 回到AE |
| CE1038 | match 分支标签存在性约束 | CE0210 | codegen: match branch has no labels | 回到AE |
| CE1039 | match 目标类型约束 | CE0211 | codegen: match target must be integer, bool, string, or enum | 回到AE |
| CE1040 | catch 块结果值约束 | CE0215 | codegen: catch block must produce a try-expression value | 回到AE |
| CE1041 | void 函数返回值禁用 | CE0264 | codegen: void function cannot return a value | 回到AE |
| CE1042 | non-void 函数返回值必需 | CE0265 | codegen: non-void function must return a value | 回到AE |
| CE1043 | if 语句条件 bool 约束 | CE0266 | codegen: if condition must be bool | 回到AE |
| CE1044 | else-if 包装层级过深 | CE0267 | codegen: too many nested else-if wrappers | 消解 |
| CE1045 | match 分支标签存在性约束 | CE0269 | codegen: match branch has no labels | 回到AE |
| CE1046 | match 目标类型约束 | CE0270 | codegen: match target must be integer, bool, string, or enum | 回到AE |
| CE1047 | while 条件 bool 约束 | CE0271 | codegen: while condition must be bool | 回到AE |
| CE1048 | for 条件 bool 约束 | CE0272 | codegen: for condition must be bool | 回到AE |
| CE1049 | for/in 源类型 @iterable 方法缺失 | CE0273 | codegen: @iterable method not found on source type | 回到AE |
| CE1050 | for/in 迭代游标类型缺失 | CE0274 | codegen: iterator cursor type not found | 回到IE |
| CE1051 | for/in 游标类型 @iterator 方法缺失 | CE0275 | codegen: @iterator method not found on cursor type | 回到IE |
| CE1052 | for/in 序列数组类型约束 | CE0277 | codegen: for/in sequence must be an array | 回到AE |
| CE1053 | 循环控制语句上下文约束 | CE0279 | codegen: '%s' outside of loop | 回到AE |
| CE1054 | 异常载荷类型缺失 | CE0280 | codegen: missing exception payload type | 回到IE |
| CE1055 | 异常载荷类型不支持 | CE0282 | codegen: unsupported exception payload type | 消解 |
| CE1056 | throw 值缺失 | CE0283 | codegen: 'throw' requires a value | 回到AE |
| CE1057 | 语句 kind 暂不支持 | CE0286 | codegen: statement kind not yet supported in this iteration | 消解 |
| CE1058 | reifiable 依赖类型参数替换失败 | CE0290 | codegen: failed to substitute type params in reifiable dep | 回到IE |

## 11 模块/程序段

| 新错误码 | 用途 | 原错误码 | 原错误文案 | 未来处理 |
|---|---|---|---|---|
| CE1101 | main 返回类型约束 | CE0288 | codegen: main must return void or i32 | 继续CE |
| CE1102 | main 参数签名约束 | CE0289 | codegen: main must have signature (args: string[]) | 继续CE |
| CE1103 | program 输入缺失 | CE0372 | codegen: no programs to compile | 继续CE |
| CE1104 | bin 目标 main 入口缺失 | CE0373 | codegen: bin target requires `main` function | 继续CE |

## 说明

- 本版按 codegen 责任域分段，不按前端语法结构分段。
- 「未来处理」列仅表示长期收敛方向：`消解` 表示随实现补完或阶段能力扩展而从 CE 中消失；`回到AE` 表示应前移到语义层；`回到IE` 表示本质属于内部不变量或实现缺陷；`继续CE` 表示长期保留为后端责任约束。
- 对仅因占位符写法不同、但语义对象和修复动作相同的旧码已合并共码，例如泛型方法类型实参推断失败、tuple 命名目标类型要求、方法参数数量约束等。
- 对调用对象、约束面或修复动作不同的错误，即使旧“用途”相近，也保持拆码，避免后续替换时误合并。
