# AE 语义错误码分段归类方案（语法对齐版）

## 归类原则

- 主归属按“用户写法所在语法结构”确定，优先与语法分段阅读路径一致。
- 同一错误可能同时涉及注解、类型、调用等多个语义维度；文档仅给出一个主归属，避免重复。
- 目标是“面向用户可快速定位”，不是反映编译器内部实现模块边界。
- 错误码完整格式固定为：`AE` + 两位段编码 + 两位段内编码（例如 `AE0001`）。
- “同类错误共码”判定同时满足三点：冲突对象一致、冲突根因一致、建议修复动作一致；任一不一致则拆分新错误码。
- 例：`throw statement requires a non-void expression` 与 `type 'unknown' is only valid as a catch clause type` 不同类，必须拆分新错误码。

## 分段规划

| 段编码 | 段 | 语义域（主归属） |
|---|---|---|
| 00 | 通用段 | 跨结构的基础语义约束 |
| 01 | 绑定段 | let/var/解构/可写性 |
| 02 | 数组段 | array 相关语义检查 |
| 03 | 类型/元组段 | type/tuple 成员、构造器、可见性、约束与生命周期 |
| 04 | 枚举段 | enum 一致性与成员合法性 |
| 05 | 函数/Lambda段 | 函数签名、调用、重载、返回、lambda |
| 06/07 | Spec段 | spec 定义与满足关系 |
| 08 | Fit段 | fit 目标、spec 列表、实现冲突 |
| 09 | 模块/Import/入口段 | import/module/main 入口 |
| 10 | 表达式段 | 对象字面量、成员访问、类型匹配 |
| 11 | 分支/匹配段 | match 分支完备性与标签约束 |
| 12 | 循环段 | 循环上下文语义限制 |
| 13 | 注解/ABI段 | 注解适用域、ABI 稳定性与边界 |
| 14 | 异常处理段 | try/catch/throw 语义 |

## 00 通用段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0001 | 基础符号与类型存在性 | AE0012 | type '%.*s' is not defined |
| AE0001 | 基础符号与类型存在性 | AE0170 | undefined identifier '%.*s' |
| AE0003 | 语义上下文限制 | AE0171 | 'self' is only available inside type methods and constructors |
| AE0004 | 跨 category name-only 冲突 | (新增) | '%.*s' conflicts with an existing visible name in a different category |
| AE0005 | import 引入名称二义/多义(惰性,使用处报错) | (新增) | '<name>' is ambiguous: imported from '<module1>' and '<module2>'; use a fully-qualified path or import alias to disambiguate |
| AE0005 | import 引入名称二义/多义(惰性,使用处报错) | (新增) | '<name>' is ambiguous: defined in current module and also imported from '<module>'; use a fully-qualified path or import alias to disambiguate |
| AE0005 | import 引入名称二义/多义(惰性,使用处报错) | (新增) | '<name>' is ambiguous: imported from multiple modules (<module1>, <module2>, ...); use a fully-qualified path or import alias to disambiguate |
| AE0006 | 使用点裸名引用泛型 | (新增) | '%.*s' is a generic type and requires type arguments |

## 01 绑定段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0101 | 不可变绑定赋值约束 | AE0022 | cannot assign to immutable binding '%.*s' |
| AE0102 | let 成员重复完成赋值约束 | AE0093 | object literal field '%.*s' repeats final binding of let member '%.*s' already completed by declaration initializer |
| AE0102 | let 成员重复完成赋值约束 | AE0094 | object literal field '%.*s' repeats final binding of let member '%.*s' already completed by constructor '%.*s' |
| AE0103 | let 成员构造期外赋值约束 | AE0095 | let member '%.*s' cannot be directly assigned outside constructors |
| AE0102 | let 成员重复完成赋值约束 | AE0096 | constructor assignment repeats final binding of let member '%.*s' already completed by declaration initializer |
| AE0102 | let 成员重复完成赋值约束 | AE0097 | constructor assignment repeats final binding of let member '%.*s' more than once in constructor '%.*s' |
| AE0104 | 赋值目标可写性约束 | AE0099 | assignment target '%s' is not writable |
| AE0105 | 同一词法值作用域绑定名称唯一性约束 | （新增） | duplicate binding '%.*s' in the same scope |
| AE0108 | 解构初始化元组要求 | AE0174 | destructuring binding initializer must be a tuple |
| AE0109 | 解构位置数量匹配约束 | AE0175 | destructuring binding has %zu position(s) but tuple initializer has %zu |
| AE0109 | 解构位置数量匹配约束 | AE0176 | destructuring binding has %zu position(s) but tuple literal has %zu |
| AE0110 | 解构类型标注位置约束 | AE0177 | destructuring bindings cannot use a single type annotation |

## 02 数组段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0201 | 数组元素类型约束 | AE0098 | array literal element at index %zu does not match expected type '%s' |
| AE0202 | 数组尺寸表达式类型约束 | AE0172 | array size must be an integer expression |
| AE0203 | 空数组目标类型约束 | AE0138 | empty array literal requires an explicit target array type |

## 03 类型/元组段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0301 | 元组目标命名类型约束 | AE0016 | tuple literal requires a named tuple target type, got '%s' |
| AE0302 | 元组元素数量匹配约束 | AE0017 | tuple literal has %zu element(s) but tuple type '%s' expects %zu |
| AE0301 | 元组目标命名类型约束 | AE0139 | tuple literal requires an explicit named tuple target type |
| AE0304 | 泛型约束中的元组限制 | AE0211 | type parameter '%.*s': tuple type cannot be used as a constraint; use a spec constraint |
| AE0305 | 静态成员 type 可见性约束 | AE0081 | static member '%.*s' of type '%.*s' is not accessible from the current type scope |
| AE0309 | 静态成员不存在约束 | AE0082 | type '%.*s' has no static member '%.*s' |
| AE0310 | 静态成员访问方式约束 | AE0084 | static member '%.*s' must be accessed through its type |
| AE0306 | 实例成员不存在约束 | AE0086 | type '%s' has no member '%.*s' |
| AE0306 | 实例成员不存在约束 | AE0090 | type '%.*s' has no member '%.*s' |
| AE0307 | fit 私有成员访问约束 | AE0091 | fit body cannot access private member '%.*s' of target type '%.*s' |
| AE0308 | 实例成员 type 可见性约束 | AE0092 | member '%.*s' of type '%.*s' is not accessible from the current type scope |
| AE0312 | 构造目标对象类型约束 | AE0140 | %s '%.*s' is not an object type and cannot be constructed |
| AE0313 | 构造器参数匹配缺失约束 | AE0141 | type '%.*s' has no constructor accepting %zu argument(s) |
| AE0314 | 构造器重载二义性约束 | AE0142 | type '%.*s' has multiple accessible constructors matching %zu argument(s); argument types are ambiguous |
| AE0315 | 构造器可见性约束 | AE0143 | type '%.*s' has no accessible constructor accepting %zu argument(s) |
| AE0316 | 构造函数/终结器方法级泛参禁用 | (新增) | constructor '%.*s' cannot declare type parameters / finalizer '~%.*s' cannot declare type parameters |
| AE0317 | @abi 类型 finalizer 禁用 | AE0071 | type '%.*s' is marked as @abi and cannot declare a finalizer |
| AE0318 | finalizer 唯一性约束 | AE0072 | type '%.*s' declares more than one finalizer |
| AE0319 | @iterable 方法唯一性约束 | AE0063 | type '%.*s' has multiple @iterable methods |
| AE0320 | @iterator 方法唯一性约束 | AE0064 | type '%.*s' has multiple @iterator methods |
| AE0321 | @iterable 与 @iterator 互斥约束 | AE0065 | type '%.*s' cannot have both @iterable and @iterator |
| AE0322 | @iterable 参数个数约束 | AE0066 | @iterable method must take no parameters |
| AE0323 | @iterable 返回迭代协议约束 | AE0067 | return type of @iterable method has no @iterator method |
| AE0324 | @iterator 参数个数约束 | AE0068 | @iterator method must take no parameters |
| AE0325 | @iterator 返回类型约束 | AE0069 | @iterator method must return a named tuple type of the form (bool, E) |
| AE0326 | 类型可迭代性缺失约束 | AE0180 | type '%s' is not iterable (no @iterable or @iterator method found) |
| AE0327 | 声明签名可见性一致性约束 | (新增) | declaration '%s' has effective visibility '%s' but type '%s' has narrower effective visibility '%s' |
| AE0328 | 成员展开来源具体类型约束 | (新增) | member mix source must resolve to a concrete object type but found '%s' |
| AE0329 | 成员展开来源构造约束 | (新增) | member mix initializer must be an object construction expression |
| AE0330 | 成员展开依赖无环约束 | (新增) | member mix expansion for type '%.*s' forms a cycle through source type '%.*s' |
| AE0331 | object-form spec 方法级泛参禁用 | (新增) | object-form spec method '%.*s' cannot declare type parameters |
| AE0332 | 默认零值有限性约束 | CE0226 | type '%s' has no finite default zero value; provide an initializer、construction of type '%s' requires a non-terminating default zero value for field '%.*s'; provide a field declaration initializer |

## 04 枚举段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0401 | 枚举显隐式赋值混用约束 | AE0059 | enum '%.*s' cannot mix explicit and implicit item values |
| AE0402 | 枚举项名称唯一性约束 | AE0060 | enum '%.*s' has duplicate item name '%.*s' |
| AE0403 | 枚举项数值唯一性约束 | AE0061 | enum '%.*s' has duplicate item value %lld |
| AE0404 | 枚举成员访问合法性 | AE0083 | enum '%.*s' has no item '%.*s' |

## 05 函数/Lambda段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0501 | 返回语句值形态约束 | AE0055 | %s body must use 'return;' without a value |
| AE0501 | 非 `void` callable 空返回约束 | AE0057 | return statement does not match expected type '%s' |
| AE0501 | `void` callable 带值返回约束 | AE1003（仅 `return expr;` 产生点） | return statement does not match expected type 'void' |
| AE0502 | void 类型使用位置约束 | AE0165 | type 'void' is only valid as a function return type |
| AE0503 | 返回类型推断失败需显式标注 | AE0223 | function '%.*s' requires an explicit return type because its return type could not be inferred |
| AE0503 | 返回类型推断失败需显式标注 | AE0224 | method '%.*s.%.*s' requires an explicit return type because its return type could not be inferred |
| AE0504 | callable 返回类型推断一致性约束 | AE0058 | callable '%.*s' has conflicting inferred return types '%s' and '%s' |
| AE0505 | 变参位置数组传递约束 | AE0078 | call target '%s' does not accept an existing array at a variadic argument position; use explicit '...array' forwarding for the complete variadic group |
| AE0506 | 可调用签名缺失约束 | AE0079 | call target '%s' has no function type overload accepting %zu argument(s) |
| AE0507 | 表达式可调用性约束 | AE0080 | expression '%s' is not callable |
| AE0508 | 方法签名重复声明约束 | AE0110 | duplicate method signature '%.*s' in type '%.*s' |
| AE0508 | object-form spec 方法签名重复声明约束 | （新增模板） | duplicate method signature '%.*s' in object-form spec '%.*s' |
| AE0508 | 顶层函数签名重复声明约束 | AE0218 | duplicate function signature '%.*s' |
| AE0509 | 仅返回类型差异的重载禁用 | AE0111 | method overloads in type '%.*s' cannot differ only by return type: '%.*s' |
| AE0509 | object-form spec 仅返回类型差异的重载禁用 | （新增模板） | method overloads in object-form spec '%.*s' cannot differ only by return type: '%.*s' |
| AE0509 | 顶层函数仅返回类型差异的重载禁用 | AE0219 | function overloads cannot differ only by return type: '%.*s' |
| AE0510 | 变参方法重载冲突约束 | AE0112 | variadic method overload conflicts with existing method '%.*s' in type '%.*s' |
| AE0510 | object-form spec 变参方法重载冲突约束 | （新增模板） | variadic method overload conflicts with existing method '%.*s' in object-form spec '%.*s' |
| AE0510 | 顶层变参函数重载冲突约束 | AE0220 | variadic function overload conflicts with existing overload '%.*s' |
| AE0511 | 调用重载二义性约束 | AE0144 | function '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0512 | 调用重载缺失匹配约束 | AE0145 | function '%.*s.%.*s' has no overload accepting %zu argument(s) |
| AE0511 | 调用重载二义性约束 | AE0146 | static method '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0512 | 调用重载缺失匹配约束 | AE0147 | static method '%.*s.%.*s' has no overload accepting %zu argument(s) |
| AE0512 | 静态约束 requirement 缺失 | （新增） | constraint spec has no static method '%.*s' accessible on type parameter '%.*s' |
| AE0513 | 字段-方法同名冲突约束 | AE0513 | field '%.*s' and method '%.*s' in type '%.*s' cannot share the same name within the same conflict surface (static or instance) |
| AE0514 | 同成员面字段名唯一性约束 | （新增） | duplicate field '%.*s' in %s '%.*s' within the %s member surface |
| AE0515 | 非 `void` callable 正常落尾约束 | （新增） | non-void callable '%.*s' can reach the end of its body without returning a value |
| AE0515 | 非 `void` 块 Lambda 正常落尾约束 | （新增） | non-void block lambda can reach the end of its body without returning a value |
| AE0511 | 调用重载二义性约束 | AE0148 | method '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0512 | 调用重载缺失匹配约束 | AE0149 | method '%.*s.%.*s' has no overload accepting %zu argument(s) |
| AE0511 | 调用重载二义性约束 | AE0150 | method '%s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0511 | 调用重载二义性约束 | AE0151 | top-level function '%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0512 | 调用重载缺失匹配约束 | AE0152 | top-level function '%.*s' has no overload accepting %zu argument(s) |
| AE0520 | lambda 目标类型与边界 | AE0101 | lambda expression in %s requires an explicit callable-form spec target type |
| AE0521 | 目标可调用类型重载消歧约束 | AE0105 | expression '%s' has multiple overloads matching expected ABI function pointer type '%s' |
| AE0521 | 目标可调用类型重载消歧约束 | AE0106 | expression '%s' has multiple overloads matching expected function type '%s' |
| AE0522 | 目标函数类型匹配约束（包括方法值来源） | AE0107 | expression '%s' does not match expected function type '%s' |
| AE0523 | 目标函数类型显式标注约束 | AE0135 | expression '%s' requires an explicit target function type to form a callable value; target type must be a callable-form spec |
| AE0524 | 预打包变参数组转发约束 | AE0524 | prepacked variadic forwarding requires a variadic call target、prepacked variadic forwarding must begin at the first variadic argument position、prepacked variadic argument must match the target readonly variadic array type |
| AE0525 | 泛型调用类型实参推导完整性 | （新增） | cannot infer type argument %zu for generic callable '%.*s'; provide an explicit type argument or a target type |

`AE0521`、`AE0522` 与 `AE0523` 同样适用于 object-form/intersection-form `spec` 实例
方法引用、受这两种 `spec` 约束的泛型值实例方法引用、`Type.method` 具体静态方法引用和
`T: ObjectSpec` 或 `T: IntersectionSpec` 的 `T.method` 静态 requirement 引用：分别表示
目标 callable 下仍存在多个匹配来源、来源签名不匹配，以及缺少形成 callable value
所需的明确 callable-form
`spec` 目标。泛型静态方法缺失显式方法类型实参继续使用既有显式泛型 target 诊断，不由
目标 callable 反向推导。

`T: ObjectSpec` 与 `T: IntersectionSpec` 的静态方法直接调用共用 `AE0511` / `AE0512`
重载诊断。intersection-form 约束先按 requirement 原声明执行访问过滤；只有本来匹配但
不可访问的 `seal` requirement 时使用 `AE0708`，不可访问候选不得被误报为重载缺失。

## 06/07 Spec段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0601 | union-form 成员列表环依赖约束 | AE0013 | union-form spec '%.*s' forms a cycle through its member list |
| AE0602 | union-form 成员类型限制 | AE0014 | union-form spec members cannot be 'void' |
| AE0603 | union-form 最少成员约束 | AE0015 | union-form spec '%.*s' must have at least one member |
| AE0604 | union-form 收窄前置约束 | AE0024 | binary operator '%s' requires union-form operands to be narrowed to a single member first |
| AE0605 | union-form match 标签成员合法性 | AE0043 | union-form match labels must be union member types or 'else' |
| AE0605 | union-form match 标签成员合法性 | AE0044 | type label '%s' is not a member of the target union-form spec |
| AE0606 | union-form 元数据可用性约束 | AE0045 | union-form spec metadata is unavailable |
| AE0607 | union-form match 标签冲突约束 | AE0046 | union match label overlaps with an earlier label and is unreachable |
| AE0607 | union-form match 标签冲突约束 | AE0047 | union match branch lists the same member more than once |
| AE0604 | union-form 收窄前置约束 | AE0085 | union-form constrained value must be narrowed to a single member before accessing member '%.*s' |
| AE0604 | union-form 收窄前置约束 | AE0087 | union-form spec '%.*s' must be narrowed to a single member before accessing member '%.*s' |
| AE0608 | union-form 目标成员消歧约束 | AE0108 | expression '%s' matches multiple members of union-form spec '%s'; use an explicit cast to select the target member |
| AE0613 | spec 父列表形态约束 | AE0182 | spec '%.*s' parent spec list must contain only spec types but found '%s' |
| AE0613 | spec 父列表形态约束 | AE0183 | object-form spec parent list can only contain object-form specs |
| AE0614 | spec 父列表唯一性与无环约束 | AE0184 | spec '%.*s' lists '%.*s' more than once in its parent spec list |
| AE0614 | spec 父列表唯一性与无环约束 | AE0185 | spec '%.*s' forms a cycle through its parent spec list |
| AE0615 | type 声明 spec 列表形态约束 | AE0199 | type '%.*s' declared spec list must contain only spec types but found '%s' |
| AE0615 | type 声明 spec 列表形态约束 | AE0200 | type '%.*s' declared spec list can only contain object-form specs |
| AE0616 | type 声明 spec 列表唯一性约束 | AE0201 | type '%.*s' lists '%.*s' more than once in its declared spec list |
| AE0620 | object-form spec 声明限制 | AE0187 | object-form spec '%.*s' cannot declare a finalizer |
| AE0621 | intersection-form 成员形态约束 | — | intersection-form spec '%.*s' members must be spec types but found '%s' |
| AE0621 | intersection-form 成员形态约束 | — | intersection-form spec '%.*s' members must be object-form or intersection-form specs |
| AE0622 | union-form 成员禁止 intersection-form | — | union-form spec '%.*s' cannot have intersection-form spec '%.*s' as a member |
| AE0701 | spec 字段存在性约束 | AE0188 | type '%.*s' is missing field '%.*s' required by spec '%.*s' |
| AE0702 | spec 字段可写性一致性约束 | AE0189 | type '%.*s' field '%.*s' mutability does not match spec '%.*s' (expected '%s') |
| AE0703 | spec 字段类型一致性约束 | AE0190 | type '%.*s' field '%.*s' type '%s' does not match spec '%.*s' field type '%s' |
| AE0704 | spec 方法签名一致性约束 | AE0191 | type '%.*s' method '%.*s' signature does not match spec '%.*s' |
| AE0705 | spec 方法存在性约束 | AE0192 | type '%.*s' is missing method '%.*s' required by spec '%.*s' |
| AE0706 | 可见关系下重载二义性 | AE0195 | type '%.*s' satisfies specs '%.*s' and '%.*s' which both declare method '%.*s' with the same parameters but different return types |
| AE0706 | 可见关系下重载二义性 | AE0196 | method overloads in type '%.*s' may both match the same arguments under visible contract relations: '%.*s' |
| AE0706 | 可见关系下重载二义性 | AE0198 | function overloads may both match the same arguments under visible contract relations: '%.*s' |
| AE0706 | object-form spec 可见关系下重载二义性 | （新增模板） | method overloads in object-form spec '%.*s' may both match the same arguments under visible contract relations: '%.*s' |
| AE0707 | spec 实现成员可见性兼容约束 | (新增) | type '%.*s' member '%.*s' has visibility 'seal' and cannot satisfy public member required by spec '%.*s' |
| AE0708 | spec seal 成员访问域约束 | (新增) | seal member '%.*s' of spec '%.*s' is only accessible from a type or fit implementation that satisfies that spec |
| AE0709 | 类型参数约束形态 | AE0212 | type parameter '%.*s': constraint must be a spec, not a type |

## 08 Fit段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0801 | fit 方法签名重复声明约束 | AE0113 | duplicate method signature '%.*s' in fit target '%s' |
| AE0802 | fit 仅返回类型差异重载禁用 | AE0114 | method overloads in fit target '%s' cannot differ only by return type: '%.*s' |
| AE0803 | fit 变参方法重载冲突约束 | AE0115 | variadic method overload conflicts with existing method '%.*s' in fit target '%s' |
| AE0804 | fit 可见实现冲突约束 | AE0193 | type '%.*s' has multiple visible implementations of method '%.*s' required by spec '%.*s' (one or more fits and/or the type itself) |
| AE0804 | fit 可见实现冲突约束 | AE0194 | fit target has multiple visible implementations of method '%.*s' required by spec '%.*s' |
| AE0805 | fit 可见关系下重载二义性约束 | AE0197 | method overloads in fit target '%s' may both match the same arguments under visible contract relations: '%.*s' |
| AE0807 | fit spec 子句体约束 | AE0202 | fit with spec clause requires a body; |
| AE0808 | fit spec 解析约束 | AE0203 | fit spec '%s' could not be resolved |
| AE0809 | fit specs 列表成员类型约束 | AE0204 | fit specs list can only contain object-form specs |
| AE0811 | fit 目标具体类型约束 | AE0205 | fit target must be a concrete type but found '%s' |
| AE0812 | fit 泛型目标参数引用完整性约束 | AE0206 | fit target for generic type '%.*s' must reference all target type parameters directly |
| AE0812 | fit 泛型目标参数引用完整性约束 | AE0207 | fit target for generic type '%.*s' must use target type parameter '%.*s' at position %zu |
| AE0813 | fit 目标泛型形态约束 | AE0208 | fit target type '%.*s' is not generic |
| AE0809 | fit specs 列表成员类型约束 | AE0209 | fit specs list must contain only spec types but found '%s' |
| AE0810 | fit specs 列表唯一性约束 | AE0210 | fit lists '%.*s' more than once in its specs clause |

## 09 模块/Import/入口段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0901 | 模块别名类型位误用约束 | AE0166 | module alias '%.*s' cannot be used as a type by itself; use '%.*s.Name' |
| AE0903 | 模块导出名称缺失约束 | AE0167 | module alias '%.*s' does not export public name '%.*s' from module '%s' |
| AE0904 | 模块别名成员访问语法约束 | AE0169 | module alias '%.*s' must be accessed as '%.*s.name' |
| AE0902 | import 声明解析约束 | AE0221 | duplicate import alias '%.*s' in the same file |
| AE0902 | import 声明解析约束 | AE0222 | import target module '%s' was not found in current compilation input |
| AE0906 | 模块符号唯一性(已废弃,本次优化后不再产生新错误,由 AE0005 在使用处惰性覆盖) | AE0157 | duplicate symbol '%.*s' in module '%.*s' |
| AE0907 | bin 入口唯一性约束 | AE0225 | duplicate 'main' entry: target 'bin' requires exactly one 'main(args: string[])' across all programs |
| AE0908 | bin 入口存在性约束 | AE0226 | target 'bin' requires a 'main(args: string[])' entry function but none was found |
| AE0909 | bin 入口参数签名约束 | AE0227 | 'main' entry must have signature 'main(args: string[])' |
| AE0910 | bin 入口返回类型约束 | AE0228 | 'main' entry must return void |

## 10 表达式段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1001 | 算术零除约束 | AE0020 | %s by zero in %s '%s' expression |
| AE1002 | 复合赋值运算符支持约束 | AE0021 | unsupported compound assignment operator '%s' |
| AE1003 | 表达式期望类型匹配约束 | AE0109 | expression '%s' does not match expected type '%s' |
| AE1004 | 对象字面量目标对象类型约束 | AE0153 | object literal target '%s' must resolve to an object type |
| AE1005 | 对象字面量字段唯一性约束 | AE0154 | duplicate object literal field '%.*s' for type '%.*s' |
| AE1006 | 对象字面量字段存在性约束 | AE0155 | object literal field '%.*s' is not a field of type '%.*s' |
| AE1007 | 对象字面量字段可见性约束 | AE0156 | object literal field '%.*s' is not accessible for type '%.*s' |
| AE1008 | 成员访问语义 | AE0088 | spec '%.*s' is callable-form and has no member '%.*s' |
| AE1008 | 成员访问语义 | AE0089 | spec '%.*s' has no member '%.*s' |
| AE1009 | infix match binding pattern 类型约束 | (新增) | infix match binding requires all labels to be union member type patterns |
| AE1010 | ABI 函数指针形成 | AE0136 | expression '%s' requires an explicit target Foo* type to form an ABI function pointer |
| AE1010 | ABI 函数指针形成 | AE0137 | expression '%s' cannot form an ABI function pointer; ABI function pointers can only be formed from top-level @abi functions with an explicit Foo* target type |
| AE1012 | 类型参数类型实参使用约束 | AE0160 | type parameter '%.*s' cannot take type arguments |
| AE1013 | 泛型目标类型可解析性约束 | AE0161 | unknown type '%s' |
| AE1014 | 非泛型类型实参误用约束 | AE0162 | '%.*s' is not a generic type and does not take type arguments |
| AE1015 | 泛型类型实参数量约束 | AE0163 | '%.*s' expects %zu type argument(s), but %zu were provided |
| AE1016 | 显式泛型目标值位使用约束 | AE0173 | explicit generic target '%s' cannot be used as a value expression |
| AE1017 | 类型参数命名遮蔽约束 | AE0181 | type parameter '%.*s' shadows an outer type parameter with the same name |
| AE1018 | 一元运算符操作数类型约束 | AE0234、AE0235、AE0236 | unary operator '%s' requires a numeric operand, got '%s'、unary operator '%s' requires an integer operand, got '%s'、unary operator '%s' requires a bool operand, got '%s' |
| AE1019 | 二元运算符操作数类型约束 | AE0030 | binary operator '%s' requires operands of the same numeric or string type, got '%s' and '%s'、binary operator '%s' requires operands of the same numeric type, got '%s' and '%s'、binary operator '%s' requires operands of the same type, got '%s' and '%s'、binary operator '%s' requires bool operands, got '%s' and '%s'、binary operator '%s' requires operands of the same integer type, got '%s' and '%s' |
| AE1020 | 复合赋值操作数类型约束 | AE0023 | compound assignment operator '%s' requires operands of the same numeric type, got '%s' and '%s'、compound assignment operator '%s' requires operands of the same integer type, got '%s' and '%s' |
| AE1021 | 索引目标数组类型约束 | AE0052 | index expression target must have array type, got '%s' |
| AE1022 | 索引操作数整数类型约束 | AE0053 | index expression requires an integer operand, got '%s' |
| AE1023 | 显式转换资格约束 | AE0051 | cast from '%s' to '%s' is not allowed |

## 11 分支/匹配段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1101 | if 分支完备性 | AE0031 | %s branch block must end with an expression statement on every normally completing path, or every reachable path must exit through return/throw |
| AE1101 | if 分支完备性 | AE0033 | if expressions require an else branch |
| AE1103 | match 区间标签端点整型约束 | AE0038 | match range label endpoints must be integer literals or 'let' bindings to integer literals |
| AE1103 | match 区间标签端点整型约束 | AE0039 | match range label endpoints must be integer values |
| AE1104 | match 区间标签顺序约束 | AE0040 | match range label requires low <= high, got %lld and %lld |
| AE1105 | match 单值标签字面量约束 | AE0041 | match label must be a literal or a 'let' binding to a literal |
| AE1106 | match 标签重叠不可达约束 | AE0042 | match label overlaps with an earlier label and is unreachable |
| AE1107 | match enum 目标标签形式约束 | (新增) | match enum target requires enum item reference label of the form 'EnumName.ItemName' |
| AE1109 | match enum 目标标签类型一致性约束 | (新增) | match label references enum '%.*s' but target type is enum '%.*s' |
| AE1111 | match enum 目标区间标签禁用约束 | (新增) | match enum target does not support range labels |
| AE1108 | match 结果一致性 | AE0048 | match expressions require an else branch |
| AE1108 | match 结果一致性 | AE0049 | match expression branches must have the same type, got '%s' and '%s' |
| AE1102 | if 条件 bool 约束 | AE0032、AE0054（if 产生点） | if expression condition must have type 'bool', got '%s'、if statement condition must have type 'bool', got '%s' |
| AE1110 | 表达式结果分支循环控制转移边界 | AE0073、（扩展至 match 表达式） | break / continue cannot target a loop outside an if / match expression result branch |

## 12 循环段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1201 | 循环上下文语义 | AE0075 | '%s' statement is only allowed inside a 'while' or 'for' loop |
| AE1202 | 循环条件 bool 约束 | AE0054（while / for 产生点） | while statement condition must have type 'bool', got '%s'、for statement condition must have type 'bool', got '%s' |

## 13 注解/ABI段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1301 | @runtime extern 适用前提约束 | AE0001 | @runtime only applies to top-level extern func declarations |
| AE1301 | @runtime extern 适用前提约束 | AE0002 | function '%.*s' cannot use @runtime unless it is declared extern |
| AE1320 | @runtime 与 ABI 注解互斥约束 | AE0003 | function '%.*s' cannot combine @runtime with @abi |
| AE1320 | @runtime 与 ABI 注解互斥约束 | AE0004 | function '%.*s' cannot combine @runtime with C ABI target annotations |
| AE1302 | 未知注解 | AE0005 | unknown annotation '@%.*s' is not supported |
| AE1303 | extern 导入职责与 @abi 暴露职责冲突 | AE0006 | function '%.*s' cannot be marked as @abi because extern functions use target annotations to define external ABI semantics |
| AE1315 | 注解适用目标限制（方法限定） | AE0062 | @%s can only be applied to methods |
| AE1304 | @abi 适用声明类型限制 | AE0116 | object-form spec '%.*s' cannot be marked as @abi; @abi only applies to type declarations and callable-form spec |
| AE1305 | 调用约定注解声明类型禁用 | AE0117 | spec '%.*s' cannot use calling convention annotations |
| AE1304 | @abi 适用声明类型限制 | AE0118 | union-form spec '%.*s' cannot be marked as @abi; union values use compiler-managed aggregate layout |
| AE1305 | 调用约定注解声明类型禁用 | AE0119 | type '%.*s' cannot use calling convention annotations |
| AE1304 | @abi 适用声明类型限制 | AE0120 | type '%.*s' cannot be marked as @abi because calling convention annotations do not apply to type declarations |
| AE1316 | 调用约定注解声明前提约束 | AE0124 | %s '%.*s' cannot use calling convention annotations unless it is marked as @abi or declared extern |
| AE1303 | extern 导入职责与 @abi 暴露职责冲突 | AE0125 | %s '%.*s' cannot be marked as @abi because extern functions declare imported C symbols |
| AE1306 | 调用约定注解组合/参数规则 | AE0126 | %s '%.*s' cannot be marked as @abi because it uses more than one calling convention annotation |
| AE1306 | 调用约定注解组合/参数规则 | AE0127 | %s '%.*s' cannot be marked as @abi because calling convention annotations on @abi declarations must not take library arguments |
| AE1317 | 构造器与终结器 ABI 注解禁用 | AE0133 | constructor '%.*s' cannot use ABI annotations |
| AE1317 | 构造器与终结器 ABI 注解禁用 | AE0134 | finalizer '~%.*s' cannot use ABI annotations |
| AE1307 | extern 调用约定声明完整性 | AE0007 | extern function '%.*s' must use exactly one of '@cdecl', '@stdcall', or '@fastcall' with a library argument, an optional C function name, and an optional fixed parameter count |
| AE1308 | extern 库参数字面量约束 | AE0008 | extern function annotation '@%.*s' library argument must be a string literal or a visible let binding initialized directly with a string literal |
| AE1321 | extern C 函数名参数字面量约束 | AE0009 | extern function annotation '@%.*s' C function name argument must be a string literal or a visible let binding initialized directly with a string literal |
| AE1322 | extern 固定参数个数字面量约束 | AE0010 | extern function annotation '@%.*s' fixed parameter count must be an integer literal |
| AE1309 | extern 固定参数个数范围约束 | AE0011 | extern function annotation '@%.*s' fixed parameter count %lld is out of range for function with %zu parameters |
| AE1310 | @abi 参数稳定性约束 | AE0121 | type '%.*s' cannot be marked as @abi because parameter '%.*s' uses non-ABI-stable type '%s' |
| AE1311 | @abi 返回稳定性约束 | AE0122 | type '%.*s' cannot be marked as @abi because return type '%s' is not ABI-stable |
| AE1312 | 字段 ABI 稳定性约束 | AE0123 | type '%.*s' cannot be marked as @abi because field '%.*s' uses non-ABI-stable type '%s' |
| AE1310 | @abi 参数稳定性约束 | AE0128 | %s '%.*s' cannot be marked as @abi because parameter '%.*s' uses non-ABI-stable type '%s' |
| AE1311 | @abi 返回稳定性约束 | AE0129 | %s '%.*s' cannot be marked as @abi because return type '%s' is not ABI-stable |
| AE1313 | @abi 异常跨边界约束 | AE0130 | %s '%.*s' cannot be marked as @abi because uncaught exceptions must not cross the @abi ABI boundary |
| AE1318 | extern C 参数稳定性约束 | AE0131 | extern function '%.*s' parameter '%.*s' type '%s' is not C ABI-stable |
| AE1319 | extern C 返回稳定性约束 | AE0132 | extern function '%.*s' return type '%s' is not C ABI-stable |
| AE1314 | 借用指针返回逃逸约束 | AE0056 | expression '%s' is a borrowed data pointer formed by '&'; borrowed data pointers are only guaranteed valid for the current extern call and cannot be returned; retain the original owner instead of returning the raw pointer |
| AE1323 | 借用指针非 extern 传参逃逸约束 | AE0102 | argument %zu expression '%s' is a borrowed data pointer formed by '&'; borrowed data pointers are only guaranteed valid for the current extern call and cannot be passed to non-extern callable '%s'; retain the original owner and form the pointer at the extern boundary |
| AE1324 | 借用指针对象字段存储逃逸约束 | AE0103 | object literal field '%.*s' cannot store borrowed data pointer expression '%s'; borrowed data pointers are only guaranteed valid for the current extern call; retain the original owner instead of caching the raw pointer |
| AE1325 | 借用指针赋值存储逃逸约束 | AE0104 | assignment target '%s' cannot store borrowed data pointer expression '%s'; borrowed data pointers are only guaranteed valid for the current extern call; retain the original owner instead of caching the raw pointer |
| AE1329 | @mixable 无参数约束 | (新增) | @mixable annotation does not accept arguments |
| AE1330 | @mixable 适用目标约束 | (新增) | @mixable can only be applied to static methods declared in a type or fit block, or seal instance fields declared in a type |
| AE1331 | @mixable 首参数存在性约束 | (新增) | @mixable static method '%.*s' must declare at least one parameter |
| AE1332 | @mixable 首参数非变参约束 | (新增) | @mixable static method '%.*s' first parameter cannot be variadic |
| AE1333 | @mixable 首参数 object-form spec 约束 | (新增) | @mixable static method '%.*s' first parameter must use an object-form spec |
| AE1334 | @mixable 来源类型名义声明 spec 约束 | (新增) | type '%.*s' must nominally declare spec '%.*s' before declaring @mixable static method '%.*s' |
| AE1335 | @mixable 目标类型名义声明 spec 约束 | (新增) | type '%.*s' must nominally declare spec '%.*s' before mixing @mixable static method '%.*s' |
| AE1336 | @friend 参数约束 | (新增) | @friend annotation requires at least one concrete friend type / @friend argument must resolve to a concrete type |
| AE1337 | @friend 适用目标约束 | (新增) | @friend can only be applied to explicitly seal fields or ordinary methods declared in a type, object-form spec, or fit block / constructors and finalizers cannot use @friend |
| AE1338 | @friend 签名可见性约束 | (新增) | member '%.*s' exposes type '%s' that is not accessible to friend type '%s' / @friend member '%.*s' exposes type '%s' that is not accessible from fit module '%s' |

## 14 异常处理段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1401 | catch 子句结果语义 | AE0035 | catch clause must produce a final result expression on every normally completing path or exit every reachable path through return/throw |
| AE1402 | throw 表达式值约束 | AE0077 | throw statement requires a non-void expression |
| AE1403 | catch 类型专用约束 | AE0164 | type 'unknown' is only valid as a catch clause type |
| AE1404 | unknown catch 值重抛约束 | AE0168 | unknown catch value '%.*s' can only be used in 'throw %.*s' |
| AE1405 | catch 结果分支循环控制转移边界 | AE0074 | break / continue cannot target a loop outside a try expression catch result branch |
| AE1406 | catch 分支顺序约束 | AE0178 | catch clause matching any exception must be the last catch clause |

## 说明

- 本版为“语法主归属”分段；与语法文档阅读顺序一致或接近。
- 数组段独立归于 02，元组语义统一并入 03 类型/元组段。
- 当一条错误同时涉及多个维度（例如 type + 注解 + ABI）时，按用户最先感知的写法归段。
