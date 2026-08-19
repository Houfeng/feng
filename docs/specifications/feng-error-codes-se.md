# SE 语法错误码分段归类方案

## 分段规划

| 段 | 范围 | 语义域 |
|---|------|--------|
| 通用段 | SE00XX | 跨结构的通用语法错误 |
| 绑定段 | SE01XX | let/var 绑定声明 |
| 数组段 | SE02XX | 数组字面量、array-new |
| 类型/元组段 | SE03XX | type 声明、元组类型/字面量 |
| 枚举段 | SE04XX | enum 声明及成员 |
| 函数/Lambda段 | SE05XX | func 声明、lambda、参数列表、构造器/finalizer |
| Spec段 | SE06XX~SE07XX | spec 多种 form，预占两段 |
| Fit段 | SE08XX | fit 声明及实现 |
| 模块/Import段 | SE09XX | module/import 声明 |
| 表达式段 | SE10XX | 对象字面量等表达式结构 |
| 分支段 | SE11XX | if/match |
| 循环段 | SE12XX | for/while |
| 注解段 | SE13XX | 注解使用约束 |
| 异常处理段 | SE14XX | try/catch/throw |

## SE00XX - 通用段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0001 | 缺失分号 | break/continue/return/throw/try/expression statements and local bindings/top-level bindings/import/module/tuple type/spec callable/fit declarations without body must end with ';' |
| SE0002 | 缺失结构定义标识符 | expected a binding name、expected a function name after 'func'、expected a method name after 'func'、expected a spec name after 'spec'、expected a type name、expected an enum name、expected an enum item name、expected a parameter name、expected a type parameter name、expected an alias name after 'as'、expected an object literal field name、expected an identifier after '.' in a qualified name/member access |
| SE0003 | 非法结构定义标识符 | expected top-level declaration、expected type member declaration、expected spec member declaration |
| SE0004 | 类型转换错误 | expected '(' to start cast expression、expected ')' after cast type |
| SE0005 | 分组表达式字符缺失 | expected '(' to start grouped expression、expected ')' to close grouped expression |
| SE0006 | 表达式项缺失 | expected expression term: identifier, literal, call |

## SE01XX - 绑定段 (let/var)

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0101 | 绑定缺少类型或初始化器 | binding declarations require a type annotation or an initializer |
| SE0102 | 顶层绑定必须以 let/var 开头 | top-level bindings must start with 'let' or 'var' |
| SE0103 | 局部绑定必须以 let/var 开头 | local bindings must start with 'let' or 'var' |
| SE0104 | 解构绑定位置数不合法 | destructuring bindings require 0 or 2 to 8 positions、destructuring bindings support at most 8 positions |
| SE0105 | 解构绑定不支持嵌套 | nested destructuring bindings are not supported |
| SE0106 | 解构绑定位置必须是标识符或空槽 | destructuring positions must be identifiers or empty slots |
| SE0107 | 解构绑定不能使用单一类型注解 | destructuring bindings cannot use a single type annotation |
| SE0108 | 解构绑定需要初始化器 | destructuring bindings require an initializer |
| SE0109 | 解构绑定上下文不合法 | destructuring is not valid in this binding context |
| SE0110 | 解构绑定字符缺失 | expected '(' to start destructuring binding、expected ')' to close destructuring binding、expected ',' between destructuring positions |

## SE02XX - 数组段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0201 | array-new 缺少类型名 | array-new segment '[:expr]' requires a type name |
| SE0202 | 数组字符缺失 | expected '[' to start array literal、expected ']' to close array literal、expected ']' after array size in '[:expr]'、expected ']' to close index expression |

## SE03XX - 类型/元组段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0301 | type 声明缺少主体 | type declarations require '{...}' after the optional spec list |
| SE0302 | type 字段缺少冒号 | type field declarations require ':' after the field name |
| SE0303 | type 字段必须以 let/var 开头 | type fields must start with 'let' or 'var' |
| SE0304 | type 方法/构造器必须以 func 开头 | type methods and constructors must start with 'func' |
| SE0305 | type 成员不能使用 extern func | type members cannot use 'extern func'; use 'func' for methods or 'let'/'var' for fields |
| SE0306 | extern type 仅支持字段 | extern type object form only supports fields; methods require a non-extern type |
| SE0307 | type 成员注解后必须紧跟字段或方法 | type member annotations must be followed immediately by a field or method; remove the trailing ';' |
| SE0308 | 元组类型元素数不合法 | tuple type declarations require 0 or 2 to 8 elements、tuple type declarations support at most 8 elements |
| SE0309 | 元组字面量元素数超限 | tuple literals support at most 8 elements |
| SE0310 | 元组字面量逗号后缺少表达式 | tuple literals require an expression after ',' |
| SE0311 | 对象字面量字段缺少冒号 | expected ':' after object literal field name |
| SE0312 | 类型/元组/对象字符缺失 | expected '(' to start tuple type declaration、expected ')' to close tuple type declaration、expected ')' to close tuple literal、expected '{' to start object literal、expected '}' to close object literal/type body |
| SE0313 | 成员展开声明形式不合法 | member mix declarations must use '...: Type;', '...: Type = Construction;', or '... = Construction;' |

## SE04XX - 枚举段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0401 | enum 不能标记 extern | enum declarations cannot be marked 'extern' |
| SE0402 | enum 不支持注解 | enum declarations do not support annotations in the current phase、enum items do not support annotations |
| SE0403 | enum 不支持泛型参数 | enum declarations do not support generic parameters |
| SE0404 | enum 不能声明父 spec | enum declarations cannot declare parent specs |
| SE0405 | enum 不能声明 callable 签名 | enum declarations cannot declare callable signatures |
| SE0406 | enum 至少需要一个成员 | enum declarations must declare at least one item |
| SE0407 | enum 仅允许成员名和整数字面量初始化器 | enum declarations only allow item names and optional integer literal initializers |
| SE0408 | enum 成员初始化器必须是整数字面量 | enum item initializer must be an integer literal、enum item initializer must be a single integer literal |
| SE0409 | enum 不允许尾部逗号 | enum declarations do not allow a trailing ',' after the last item |
| SE0410 | enum 字符缺失 | enum declarations require '{...}' after the enum name、expected '}' to close enum body |

## SE05XX - 函数/Lambda段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0501 | 顶层函数必须以 func 开头 | top-level function declarations must start with 'func' |
| SE0502 | 可变参数必须是最后一个 | variadic parameter must be the last parameter |
| SE0503 | extern 函数不能使用可变参数 | extern function declarations cannot use variadic parameters |
| SE0504 | 参数名后缺少冒号 | expected ':' after parameter name in parameter list |
| SE0505 | 构造器不能有非 void 返回类型 | constructor must not declare a non-void return type |
| SE0506 | 构造器名称必须匹配类型名 | constructor name must match the enclosing type name |
| SE0507 | 构造器不能声明为 static | constructors cannot be declared 'static' |
| SE0508 | finalizer 名称必须匹配类型名 | finalizer name must match the enclosing type name |
| SE0509 | finalizer 不能声明参数 | finalizer must not declare any parameters |
| SE0510 | finalizer 返回类型必须省略或为 void | finalizer return type must be omitted or ': void' |
| SE0511 | finalizer 不能声明为 static | finalizers cannot be declared 'static' |
| SE0512 | finalizer 不能通过 .~ 直接调用 | finalizer cannot be invoked directly via '.~' |
| SE0513 | lambda 必须使用 -> 或块体 | lambda expressions must use '->' before a single-expression body or '{' for a block body |
| SE0514 | 多行 lambda 必须使用块形式 | multi-line lambda body must omit '->' and use the block form '(params) { ... }' |
| SE0515 | 函数/参数字符缺失 | expected '(' to start parameter list、expected ')' to close parameter list/argument list |
| SE0516 | lambda 表达式结构不完整 | expected '(' to start grouped expression, cast, or lambda（lambda 部分） |
| SE0517 | extern 函数声明形式不合法 | extern function declarations must end with ';' and cannot have a body '{...}' |
| SE0518 | 函数/finalizer 声明缺少函数体 | function declarations must provide a body '{...}'、type finalizers must provide a body '{...}' |

## SE06XX - Spec段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| ~~SE0601~~ | (失效) 原用于禁止 spec 成员显式声明 open；自 object-form spec 同时支持显式 open 与 seal 后失效，后续按需复用 | spec members cannot declare 'open'; omit 'open' or use 'seal' |
| ~~SE0602~~ | (失效) 原用于禁止 spec 中声明 static 成员;自 spec 静态成员支持后,该错误码失效,后续按需复用 | spec members cannot be declared 'static' |
| SE0603 | spec 字段不能有初始化器 | spec field declarations cannot have an initializer |
| SE0604 | spec 方法签名必须有返回类型 | spec method signatures must declare a return type |
| SE0605 | spec 方法签名不能有函数体 | spec method signatures must end with ';' and cannot have a body '{...}' |
| SE0606 | spec 方法/callable 参数不能用 let/var | spec method parameters cannot use 'let' or 'var' modifiers、spec callable parameters cannot use 'let' or 'var' modifiers |
| SE0607 | spec callable 需要冒号 | spec callable declarations require ':' before the return type |
| SE0608 | spec object 需要主体 | spec object declarations require '{...}' after the optional spec list |
| SE0609 | union-form spec 成员不能是 void | union-form spec members cannot be 'void' |
| SE0610 | spec 字符缺失 | expected '>' to close type argument list/type parameter list、expected '}' to close spec body |

## SE08XX - Fit段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0801 | fit 不能使用 extern | 'extern' cannot be applied to a 'fit' declaration |
| SE0802 | fit 不能使用注解 | annotations cannot be applied to 'fit' declarations |
| SE0803 | fit 不能使用 seal | fit declarations cannot use 'seal' |
| SE0804 | fit 必须包含 spec 列表或函数体 | fit declarations must include a spec list, a body block, or both |
| SE0805 | fit 块方法必须提供函数体 | fit block methods must provide a body '{...}' |
| SE0806 | fit 字符缺失 | expected '}' to close fit body |
| SE0807 | fit 不能声明字段 | fit blocks cannot declare 'let'/'var' fields（含 static let/static var） |
| SE0808 | fit 成员必须以 func 开头 | fit static members must be declared with 'func'、fit block members must start with 'func' |

## SE09XX - 模块/Import段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE0901 | 源文件必须以 module 声明开头 | source file must begin with module declaration |
| SE0902 | module/import 缺少路径 | expected a module path after 'module'、expected a module path after 'import' |

## SE10XX - 表达式段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE1001 | 对象字面量字段缺少冒号 | expected ':' after object literal field name |
| SE1002 | 对象字面量字符缺失 | expected '{' to start object literal、expected '}' to close object literal |
| SE1003 | 预打包变参数组位置不合法 | prepacked variadic forwarding is only allowed before the final call argument、prepacked variadic forwarding must be the last call argument |

> 注：算术/关系/逻辑运算符的语法错误在当前源码中未出现独立的 SE 错误码（这些在语义阶段 AE 中检查），SE10XX 目前仅覆盖对象字面量相关。后续如有新增可直接扩展。

## SE11XX - 分支段 (if/match)

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE1101 | if 条件后必须使用块 | if expressions must use '{...}' after the condition |
| SE1102 | if 表达式需要 else 分支 | if expressions require an 'else' branch |
| SE1103 | match 表达式需要 else 分支 | match expressions require an 'else' branch |
| SE1104 | match 不能有多个 else 分支 | match expression cannot declare more than one 'else' branch |
| SE1105 | match 标签必须是字面量或命名常量 | match label must be an integer, string, bool literal or named constant |
| SE1106 | 分支字符缺失 | expected '{' after if condition、expected '}' to close if block/match body/the true branch of if expression |
| SE1107 | if 表达式结构不完整 | expected expression term 中 if-expression 部分 |

## SE12XX - 循环段 (for/while)

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE1201 | for 初始化器后缺少分号 | for statements require ';' after the initializer |
| SE1202 | for 条件后缺少分号 | for statements require ';' after the condition |

## SE13XX - 注解段

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE1301 | 注解后必须紧跟声明 | annotation must be followed immediately by a declaration; remove the trailing ';' |
| SE1302 | 注解参数列表字符缺失 | expected ')' to close annotation argument list |

## SE14XX - 异常处理段 (try/catch/throw)

| 新错误码 | 用途 | 涵盖的旧消息 |
|----------|------|-------------|
| SE1401 | try 至少需要一个 catch 子句 | 'try' requires at least one 'catch' clause |
| SE1402 | catch 必须绑定异常名称 | catch clauses must bind an exception name |
| SE1403 | catch 必须包含类型注解 | catch clauses must include a ': Type' annotation |
| SE1404 | try 表达式结构不完整 | expected expression term 中 try-expression 部分 |

## 统计

| 段 | 错误码数量 |
|----|-----------|
| SE00XX 通用 | 6 |
| SE01XX 绑定 | 10 |
| SE02XX 数组 | 2 |
| SE03XX 类型/元组 | 13 |
| SE04XX 枚举 | 10 |
| SE05XX 函数/Lambda | 18 |
| SE06XX Spec | 10 |
| SE08XX Fit | 8 |
| SE09XX 模块/Import | 2 |
| SE10XX 表达式 | 3 |
| SE11XX 分支 | 7 |
| SE12XX 循环 | 2 |
| SE13XX 注解 | 2 |
| SE14XX 异常处理 | 4 |
| **合计** | **97** |
