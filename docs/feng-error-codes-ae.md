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
| 11 | 分支/匹配段 | if/match 分支完备性与标签约束 |
| 12 | 循环段 | 循环上下文语义限制 |
| 13 | 注解/ABI段 | 注解适用域、ABI 稳定性与边界 |
| 14 | 异常处理段 | try/catch/throw 语义 |

## 00 通用段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0001 | 基础符号与类型存在性 | AE0012 | type '%.*s' is not defined |
| AE0001 | 基础符号与类型存在性 | AE0170 | undefined identifier '%.*s' |
| AE0003 | 语义上下文限制 | AE0171 | 'self' is only available inside type methods and constructors |

## 01 绑定段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0101 | 绑定可写性约束 | AE0022 | cannot assign to immutable binding '%.*s' |
| AE0101 | 绑定可写性约束 | AE0093 | object literal field '%.*s' repeats final binding of let member '%.*s' already completed by declaration initializer |
| AE0101 | 绑定可写性约束 | AE0094 | object literal field '%.*s' repeats final binding of let member '%.*s' already completed by constructor '%.*s' |
| AE0101 | 绑定可写性约束 | AE0095 | let member '%.*s' cannot be directly assigned outside constructors |
| AE0101 | 绑定可写性约束 | AE0096 | constructor assignment repeats final binding of let member '%.*s' already completed by declaration initializer |
| AE0101 | 绑定可写性约束 | AE0097 | constructor assignment repeats final binding of let member '%.*s' more than once in constructor '%.*s' |
| AE0101 | 绑定可写性约束 | AE0099 | assignment target '%s' is not writable |
| AE0108 | 解构绑定语义 | AE0174 | destructuring binding initializer must be a tuple |
| AE0108 | 解构绑定语义 | AE0175 | destructuring binding has %zu position(s) but tuple initializer has %zu |
| AE0108 | 解构绑定语义 | AE0176 | destructuring binding has %zu position(s) but tuple literal has %zu |
| AE0108 | 解构绑定语义 | AE0177 | destructuring bindings cannot use a single type annotation |

## 02 数组段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0201 | 数组元素类型约束 | AE0098 | array literal element at index %zu does not match expected type '%s' |
| AE0202 | 数组尺寸表达式类型约束 | AE0172 | array size must be an integer expression |
| AE0203 | 空数组目标类型约束 | AE0138 | empty array literal requires an explicit target array type |

## 03 类型/元组段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0301 | 元组目标与形态约束 | AE0016 | tuple literal requires a named tuple target type, got '%s' |
| AE0301 | 元组目标与形态约束 | AE0017 | tuple literal has %zu element(s) but tuple type '%s' expects %zu |
| AE0301 | 元组目标与形态约束 | AE0139 | tuple literal requires an explicit named tuple target type |
| AE0304 | 泛型约束中的元组限制 | AE0211 | type parameter '%.*s': tuple type cannot be used as a constraint; use a spec constraint |
| AE0305 | 静态成员解析与访问方式约束 | AE0081 | static member '%.*s' of type '%.*s' is not accessible from the current module |
| AE0305 | 静态成员解析与访问方式约束 | AE0082 | type '%.*s' has no static member '%.*s' |
| AE0305 | 静态成员解析与访问方式约束 | AE0084 | static member '%.*s' must be accessed through its type |
| AE0306 | 实例成员解析与可见性约束 | AE0086 | type '%s' has no member '%.*s' |
| AE0306 | 实例成员解析与可见性约束 | AE0090 | type '%.*s' has no member '%.*s' |
| AE0306 | 实例成员解析与可见性约束 | AE0091 | fit body cannot access private member '%.*s' of target type '%.*s' |
| AE0306 | 实例成员解析与可见性约束 | AE0092 | member '%.*s' of type '%.*s' is not accessible from the current module |
| AE0312 | 构造语义与可见性 | AE0140 | %s '%.*s' is not an object type and cannot be constructed |
| AE0312 | 构造语义与可见性 | AE0141 | type '%.*s' has no constructor accepting %zu argument(s) |
| AE0312 | 构造语义与可见性 | AE0142 | type '%.*s' has multiple accessible constructors matching %zu argument(s); argument types are ambiguous |
| AE0312 | 构造语义与可见性 | AE0143 | type '%.*s' has no accessible constructor accepting %zu argument(s) |
| AE0316 | finalizer 与成员生命周期约束 | AE0070 | generic type '%.*s' cannot declare a finalizer |
| AE0316 | finalizer 与成员生命周期约束 | AE0071 | type '%.*s' is marked as @abi and cannot declare a finalizer |
| AE0316 | finalizer 与成员生命周期约束 | AE0072 | type '%.*s' declares more than one finalizer |
| AE0319 | 可迭代协议闭包 | AE0063 | type '%.*s' has multiple @iterable methods |
| AE0319 | 可迭代协议闭包 | AE0064 | type '%.*s' has multiple @iterator methods |
| AE0319 | 可迭代协议闭包 | AE0065 | type '%.*s' cannot have both @iterable and @iterator |
| AE0319 | 可迭代协议闭包 | AE0066 | @iterable method must take no parameters |
| AE0319 | 可迭代协议闭包 | AE0067 | return type of @iterable method has no @iterator method |
| AE0319 | 可迭代协议闭包 | AE0068 | @iterator method must take no parameters |
| AE0319 | 可迭代协议闭包 | AE0069 | @iterator method must return a named tuple type of the form (bool, E) |
| AE0319 | 可迭代协议闭包 | AE0180 | type '%s' is not iterable (no @iterable or @iterator method found) |

## 04 枚举段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0401 | 枚举定义一致性 | AE0059 | enum '%.*s' cannot mix explicit and implicit item values |
| AE0401 | 枚举定义一致性 | AE0060 | enum '%.*s' has duplicate item name '%.*s' |
| AE0401 | 枚举定义一致性 | AE0061 | enum '%.*s' has duplicate item value %lld |
| AE0404 | 枚举成员访问合法性 | AE0083 | enum '%.*s' has no item '%.*s' |

## 05 函数/Lambda段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0501 | 返回语句值形态约束 | AE0055 | %s body must use 'return;' without a value |
| AE0502 | void 类型使用位置约束 | AE0165 | type 'void' is only valid as a function return type |
| AE0503 | 返回类型推断失败需显式标注 | AE0223 | function '%.*s' requires an explicit return type because its return type could not be inferred |
| AE0503 | 返回类型推断失败需显式标注 | AE0224 | method '%.*s.%.*s' requires an explicit return type because its return type could not be inferred |
| AE0505 | 可调用目标合法性 | AE0078 | call target '%s' does not accept an existing array at a variadic argument position; pass elements individually |
| AE0505 | 可调用目标合法性 | AE0079 | call target '%s' has no function type overload accepting %zu argument(s) |
| AE0505 | 可调用目标合法性 | AE0080 | expression '%s' is not callable |
| AE0508 | 重载声明冲突 | AE0110 | duplicate method signature '%.*s' in type '%.*s' |
| AE0508 | 重载声明冲突 | AE0111 | method overloads in type '%.*s' cannot differ only by return type: '%.*s' |
| AE0508 | 重载声明冲突 | AE0112 | variadic method overload conflicts with existing method '%.*s' in type '%.*s' |
| AE0511 | 调用重载解析 | AE0144 | function '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0511 | 调用重载解析 | AE0145 | function '%.*s.%.*s' has no overload accepting %zu argument(s) |
| AE0511 | 调用重载解析 | AE0146 | static method '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0511 | 调用重载解析 | AE0147 | static method '%.*s.%.*s' has no overload accepting %zu argument(s) |
| AE0511 | 调用重载解析 | AE0148 | method '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0511 | 调用重载解析 | AE0149 | method '%.*s.%.*s' has no overload accepting %zu argument(s) |
| AE0511 | 调用重载解析 | AE0150 | method '%s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0511 | 调用重载解析 | AE0151 | top-level function '%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| AE0511 | 调用重载解析 | AE0152 | top-level function '%.*s' has no overload accepting %zu argument(s) |
| AE0520 | lambda 目标类型与边界 | AE0101 | lambda expression in %s requires an explicit callable-form spec target type |
| AE0521 | 函数类型匹配与消歧约束 | AE0105 | expression '%s' has multiple overloads matching expected ABI function pointer type '%s' |
| AE0521 | 函数类型匹配与消歧约束 | AE0106 | expression '%s' has multiple overloads matching expected function type '%s' |
| AE0521 | 函数类型匹配与消歧约束 | AE0107 | expression '%s' does not match expected function type '%s' |
| AE0521 | 函数类型匹配与消歧约束 | AE0135 | expression '%s' requires an explicit target function type to resolve overloads |

## 06/07 Spec段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0601 | union-form spec 基础定义 | AE0013 | union-form spec '%.*s' forms a cycle through its member list |
| AE0601 | union-form spec 基础定义 | AE0014 | union-form spec members cannot be 'void' |
| AE0601 | union-form spec 基础定义 | AE0015 | union-form spec '%.*s' must have at least one member |
| AE0604 | union-form 收窄与匹配 | AE0024 | binary operator '%s' requires union-form operands to be narrowed to a single member first |
| AE0604 | union-form 收窄与匹配 | AE0043 | union-form match labels must be union member types or 'else' |
| AE0604 | union-form 收窄与匹配 | AE0044 | type label '%s' is not a member of the target union-form spec |
| AE0604 | union-form 收窄与匹配 | AE0045 | union-form spec metadata is unavailable |
| AE0604 | union-form 收窄与匹配 | AE0046 | union match label overlaps with an earlier label and is unreachable |
| AE0604 | union-form 收窄与匹配 | AE0047 | union match branch lists the same member more than once |
| AE0604 | union-form 收窄与匹配 | AE0085 | union-form constrained value must be narrowed to a single member before accessing member '%.*s' |
| AE0604 | union-form 收窄与匹配 | AE0087 | union-form spec '%.*s' must be narrowed to a single member before accessing member '%.*s' |
| AE0604 | union-form 收窄与匹配 | AE0108 | expression '%s' matches multiple members of union-form spec '%s'; use an explicit cast to select the target member |
| AE0613 | spec 继承图合法性 | AE0182 | spec '%.*s' parent spec list must contain only spec types but found '%s' |
| AE0613 | spec 继承图合法性 | AE0183 | object-form spec parent list can only contain object-form specs |
| AE0613 | spec 继承图合法性 | AE0184 | spec '%.*s' lists '%.*s' more than once in its parent spec list |
| AE0613 | spec 继承图合法性 | AE0185 | spec '%.*s' forms a cycle through its parent spec list |
| AE0613 | spec 继承图合法性 | AE0199 | type '%.*s' declared spec list must contain only spec types but found '%s' |
| AE0613 | spec 继承图合法性 | AE0200 | type '%.*s' declared spec list can only contain object-form specs |
| AE0613 | spec 继承图合法性 | AE0201 | type '%.*s' lists '%.*s' more than once in its declared spec list |
| AE0620 | object-form spec 声明限制 | AE0186 | object-form spec '%.*s' cannot declare a constructor |
| AE0620 | object-form spec 声明限制 | AE0187 | object-form spec '%.*s' cannot declare a finalizer |
| AE0701 | spec 满足关系一致性 | AE0188 | type '%.*s' is missing field '%.*s' required by spec '%.*s' |
| AE0701 | spec 满足关系一致性 | AE0189 | type '%.*s' field '%.*s' mutability does not match spec '%.*s' (expected '%s') |
| AE0701 | spec 满足关系一致性 | AE0190 | type '%.*s' field '%.*s' type '%s' does not match spec '%.*s' field type '%s' |
| AE0701 | spec 满足关系一致性 | AE0191 | type '%.*s' method '%.*s' signature does not match spec '%.*s' |
| AE0701 | spec 满足关系一致性 | AE0192 | type '%.*s' is missing method '%.*s' required by spec '%.*s' |
| AE0706 | 可见关系下重载二义性 | AE0195 | type '%.*s' satisfies specs '%.*s' and '%.*s' which both declare method '%.*s' with the same parameters but different return types |
| AE0706 | 可见关系下重载二义性 | AE0196 | method overloads in type '%.*s' may both match the same arguments under visible contract relations: '%.*s' |
| AE0706 | 可见关系下重载二义性 | AE0198 | function overloads may both match the same arguments under visible contract relations: '%.*s' |
| AE0709 | 类型参数约束形态 | AE0212 | type parameter '%.*s': constraint must be a spec, not a type |

## 08 Fit段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0801 | fit 重载与实现冲突 | AE0113 | duplicate method signature '%.*s' in fit target '%s' |
| AE0801 | fit 重载与实现冲突 | AE0114 | method overloads in fit target '%s' cannot differ only by return type: '%.*s' |
| AE0801 | fit 重载与实现冲突 | AE0115 | variadic method overload conflicts with existing method '%.*s' in fit target '%s' |
| AE0801 | fit 重载与实现冲突 | AE0193 | type '%.*s' has multiple visible implementations of method '%.*s' required by spec '%.*s' (one or more fits and/or the type itself) |
| AE0801 | fit 重载与实现冲突 | AE0194 | fit target has multiple visible implementations of method '%.*s' required by spec '%.*s' |
| AE0801 | fit 重载与实现冲突 | AE0197 | method overloads in fit target '%s' may both match the same arguments under visible contract relations: '%.*s' |
| AE0807 | fit 声明与目标约束 | AE0202 | fit with spec clause requires a body; |
| AE0807 | fit 声明与目标约束 | AE0203 | fit spec '%s' could not be resolved |
| AE0807 | fit 声明与目标约束 | AE0204 | fit specs list can only contain object-form specs |
| AE0807 | fit 声明与目标约束 | AE0205 | fit target must be a concrete type but found '%s' |
| AE0807 | fit 声明与目标约束 | AE0206 | fit target for generic type '%.*s' must reference all target type parameters directly |
| AE0807 | fit 声明与目标约束 | AE0207 | fit target for generic type '%.*s' must use target type parameter '%.*s' at position %zu |
| AE0807 | fit 声明与目标约束 | AE0208 | fit target type '%.*s' is not generic |
| AE0807 | fit 声明与目标约束 | AE0209 | fit specs list must contain only spec types but found '%s' |
| AE0807 | fit 声明与目标约束 | AE0210 | fit lists '%.*s' more than once in its specs clause |

## 09 模块/Import/入口段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE0901 | 模块别名使用与导出访问约束 | AE0166 | module alias '%.*s' cannot be used as a type by itself; use '%.*s.Name' |
| AE0901 | 模块别名使用与导出访问约束 | AE0167 | module alias '%.*s' does not export public name '%.*s' from module '%s' |
| AE0901 | 模块别名使用与导出访问约束 | AE0169 | module alias '%.*s' must be accessed as '%.*s.name' |
| AE0902 | import 声明解析约束 | AE0221 | duplicate import alias '%.*s' in the same file |
| AE0902 | import 声明解析约束 | AE0222 | import target module '%s' was not found in current compilation input |
| AE0906 | 模块符号唯一性 | AE0157 | duplicate symbol '%.*s' in module '%.*s' |
| AE0907 | bin 入口规则 | AE0225 | duplicate 'main' entry: target 'bin' requires exactly one 'main(args: string[])' across all programs |
| AE0907 | bin 入口规则 | AE0226 | target 'bin' requires a 'main(args: string[])' entry function but none was found |
| AE0907 | bin 入口规则 | AE0227 | 'main' entry must have signature 'main(args: string[])' |
| AE0907 | bin 入口规则 | AE0228 | 'main' entry must return void |

## 10 表达式段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1001 | 算术零除约束 | AE0020 | %s by zero in %s '%s' expression |
| AE1002 | 复合赋值运算符支持约束 | AE0021 | unsupported compound assignment operator '%s' |
| AE1003 | 表达式期望类型匹配约束 | AE0109 | expression '%s' does not match expected type '%s' |
| AE1004 | 对象字面量语义 | AE0153 | object literal target '%s' must resolve to an object type |
| AE1004 | 对象字面量语义 | AE0154 | duplicate object literal field '%.*s' for type '%.*s' |
| AE1004 | 对象字面量语义 | AE0155 | object literal field '%.*s' is not a field of type '%.*s' |
| AE1004 | 对象字面量语义 | AE0156 | object literal field '%.*s' is not accessible for type '%.*s' |
| AE1008 | 成员访问语义 | AE0088 | spec '%.*s' is callable-form and has no member '%.*s' |
| AE1008 | 成员访问语义 | AE0089 | spec '%.*s' has no member '%.*s' |
| AE1010 | ABI 函数指针形成 | AE0136 | expression '%s' requires an explicit target Foo* type to form an ABI function pointer |
| AE1010 | ABI 函数指针形成 | AE0137 | expression '%s' cannot form an ABI function pointer; ABI function pointers can only be formed from top-level @abi functions with an explicit Foo* target type |
| AE1012 | 泛型表达式语义 | AE0160 | type parameter '%.*s' cannot take type arguments |
| AE1012 | 泛型表达式语义 | AE0161 | unknown type '%s' |
| AE1012 | 泛型表达式语义 | AE0162 | '%.*s' is not a generic type and does not take type arguments |
| AE1012 | 泛型表达式语义 | AE0163 | '%.*s' expects %zu type argument(s), but %zu were provided |
| AE1012 | 泛型表达式语义 | AE0173 | explicit generic target '%s' cannot be used as a value expression |
| AE1012 | 泛型表达式语义 | AE0181 | type parameter '%.*s' shadows an outer type parameter with the same name |

## 11 分支/匹配段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1101 | if 分支完备性 | AE0031 | %s branch block must end with an expression statement |
| AE1101 | if 分支完备性 | AE0033 | if expressions require an else branch |
| AE1103 | match 标签合法性 | AE0038 | match range label endpoints must be integer literals or 'let' bindings to integer literals |
| AE1103 | match 标签合法性 | AE0039 | match range label endpoints must be integer values |
| AE1103 | match 标签合法性 | AE0040 | match range label requires low <= high, got %lld and %lld |
| AE1103 | match 标签合法性 | AE0041 | match label must be a literal or a 'let' binding to a literal |
| AE1103 | match 标签合法性 | AE0042 | match label overlaps with an earlier label and is unreachable |
| AE1108 | match 结果一致性 | AE0048 | match expressions require an else branch |
| AE1108 | match 结果一致性 | AE0049 | match expression branches must have the same type, got '%s' and '%s' |
| AE1110 | 分支上下文语义限制 | AE0073 | '%s' cannot appear directly inside an 'if' expression block; |

## 12 循环段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1201 | 循环上下文语义 | AE0075 | '%s' statement is only allowed inside a 'while' or 'for' loop |

## 13 注解/ABI段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1301 | runtime/注解目标与组合约束 | AE0001 | @runtime only applies to top-level extern func declarations |
| AE1301 | runtime/注解目标与组合约束 | AE0002 | function '%.*s' cannot use @runtime unless it is declared extern |
| AE1301 | runtime/注解目标与组合约束 | AE0003 | function '%.*s' cannot combine @runtime with @abi |
| AE1301 | runtime/注解目标与组合约束 | AE0004 | function '%.*s' cannot combine @runtime with C ABI target annotations |
| AE1302 | 未知注解 | AE0005 | unknown annotation '@%.*s' is not supported |
| AE1303 | extern 导入职责与 @abi 暴露职责冲突 | AE0006 | function '%.*s' cannot be marked as @abi because extern functions use target annotations to define external ABI semantics |
| AE1315 | 注解适用目标限制（方法限定） | AE0062 | @%s can only be applied to methods |
| AE1304 | @abi 适用声明类型限制 | AE0116 | object-form spec '%.*s' cannot be marked as @abi; @abi only applies to type declarations and callable-form spec |
| AE1305 | 调用约定注解适用位置限制 | AE0117 | spec '%.*s' cannot use calling convention annotations |
| AE1304 | @abi 适用声明类型限制 | AE0118 | union-form spec '%.*s' cannot be marked as @abi; union values use compiler-managed aggregate layout |
| AE1305 | 调用约定注解适用位置限制 | AE0119 | type '%.*s' cannot use calling convention annotations |
| AE1304 | @abi 适用声明类型限制 | AE0120 | type '%.*s' cannot be marked as @abi because calling convention annotations do not apply to type declarations |
| AE1305 | 调用约定注解适用位置限制 | AE0124 | %s '%.*s' cannot use calling convention annotations unless it is marked as @abi or declared extern |
| AE1303 | extern 导入职责与 @abi 暴露职责冲突 | AE0125 | %s '%.*s' cannot be marked as @abi because extern functions declare imported C symbols |
| AE1306 | 调用约定注解组合/参数规则 | AE0126 | %s '%.*s' cannot be marked as @abi because it uses more than one calling convention annotation |
| AE1306 | 调用约定注解组合/参数规则 | AE0127 | %s '%.*s' cannot be marked as @abi because calling convention annotations on @abi declarations must not take library arguments |
| AE1305 | 调用约定注解适用位置限制 | AE0133 | constructor '%.*s' cannot use ABI annotations |
| AE1305 | 调用约定注解适用位置限制 | AE0134 | finalizer '~%.*s' cannot use ABI annotations |
| AE1307 | extern 调用约定声明完整性 | AE0007 | extern function '%.*s' must use exactly one of '@cdecl', '@stdcall', or '@fastcall' with a library argument, an optional C function name, and an optional fixed parameter count |
| AE1308 | extern 注解参数值约束 | AE0008 | extern function annotation '@%.*s' library argument must be a string literal or a visible let binding initialized directly with a string literal |
| AE1308 | extern 注解参数值约束 | AE0009 | extern function annotation '@%.*s' C function name argument must be a string literal or a visible let binding initialized directly with a string literal |
| AE1308 | extern 注解参数值约束 | AE0010 | extern function annotation '@%.*s' fixed parameter count must be an integer literal |
| AE1309 | extern 固定参数个数范围约束 | AE0011 | extern function annotation '@%.*s' fixed parameter count %lld is out of range for function with %zu parameters |
| AE1310 | 参数 ABI 稳定性约束 | AE0121 | type '%.*s' cannot be marked as @abi because parameter '%.*s' uses non-ABI-stable type '%s' |
| AE1311 | 返回 ABI 稳定性约束 | AE0122 | type '%.*s' cannot be marked as @abi because return type '%s' is not ABI-stable |
| AE1312 | 字段 ABI 稳定性约束 | AE0123 | type '%.*s' cannot be marked as @abi because field '%.*s' uses non-ABI-stable type '%s' |
| AE1310 | 参数 ABI 稳定性约束 | AE0128 | %s '%.*s' cannot be marked as @abi because parameter '%.*s' uses non-ABI-stable type '%s' |
| AE1311 | 返回 ABI 稳定性约束 | AE0129 | %s '%.*s' cannot be marked as @abi because return type '%s' is not ABI-stable |
| AE1313 | @abi 异常跨边界约束 | AE0130 | %s '%.*s' cannot be marked as @abi because uncaught exceptions must not cross the @abi ABI boundary |
| AE1310 | 参数 ABI 稳定性约束 | AE0131 | extern function '%.*s' parameter '%.*s' type '%s' is not C ABI-stable |
| AE1311 | 返回 ABI 稳定性约束 | AE0132 | extern function '%.*s' return type '%s' is not C ABI-stable |
| AE1314 | 借用指针生命周期越界 | AE0056 | expression '%s' is a borrowed data pointer formed by '&'; borrowed data pointers are only guaranteed valid for the current extern call and cannot be returned; retain the original owner instead of returning the raw pointer |
| AE1314 | 借用指针生命周期越界 | AE0102 | argument %zu expression '%s' is a borrowed data pointer formed by '&'; borrowed data pointers are only guaranteed valid for the current extern call and cannot be passed to non-extern callable '%s'; retain the original owner and form the pointer at the extern boundary |
| AE1314 | 借用指针生命周期越界 | AE0103 | object literal field '%.*s' cannot store borrowed data pointer expression '%s'; borrowed data pointers are only guaranteed valid for the current extern call; retain the original owner instead of caching the raw pointer |
| AE1314 | 借用指针生命周期越界 | AE0104 | assignment target '%s' cannot store borrowed data pointer expression '%s'; borrowed data pointers are only guaranteed valid for the current extern call; retain the original owner instead of caching the raw pointer |

## 14 异常处理段

| 新错误码 | 用途 | 原错误码 | 原错误文案 |
|---|---|---|---|
| AE1401 | catch 子句结果语义 | AE0035 | catch clause must end with a result expression, 'return', or 'throw' |
| AE1402 | throw 表达式值约束 | AE0077 | throw statement requires a non-void expression |
| AE1403 | catch 类型专用约束 | AE0164 | type 'unknown' is only valid as a catch clause type |
| AE1404 | unknown catch 值重抛约束 | AE0168 | unknown catch value '%.*s' can only be used in 'throw %.*s' |
| AE1405 | catch 块语句上下文约束 | AE0074 | '%s' cannot appear directly inside a catch block; |
| AE1406 | catch 分支顺序约束 | AE0178 | catch clause matching any exception must be the last catch clause |

## 说明

- 本版为“语法主归属”分段；与语法文档阅读顺序一致或接近。
- 数组段独立归于 02，元组语义统一并入 03 类型/元组段。
- 当一条错误同时涉及多个维度（例如 type + 注解 + ABI）时，按用户最先感知的写法归段。
