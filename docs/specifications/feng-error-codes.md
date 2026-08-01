# Feng 编译器错误码

## LE - 词法错误

[词法错误](./feng-error-codes-le.md)

## SE - 语法错误

[语法错误](./feng-error-codes-se.md)

## AE - 语义错误

| 错误码 | 用途 | 错误文案 |
|--------|------|----------|
| **AE0001** | 注解使用错误 | @runtime only applies to top-level extern func declarations |
| **AE0002** | 注解使用错误 | function '%.*s' cannot use @runtime unless it is declared extern |
| **AE0003** | 注解使用错误 | function '%.*s' cannot combine @runtime with @abi |
| **AE0004** | 注解使用错误 | function '%.*s' cannot combine @runtime with C ABI target annotations |
| **AE0005** | 语义检查错误 | unknown annotation '@%.*s' is not supported |
| **AE0006** | 语义检查错误 | function '%.*s' cannot be marked as @abi because extern functions use target annotations to define external ABI semantics |
| **AE0007** | 调用目标无效 | extern function '%.*s' must use exactly one of '@cdecl', '@stdcall', or '@fastcall' with a library argument, an optional C function name, and an optional fixed parameter count |
| **AE0008** | 语义检查错误 | extern function annotation '@%.*s' library argument must be a string literal or a visible let binding initialized directly with a string literal |
| **AE0009** | 语义检查错误 | extern function annotation '@%.*s' C function name argument must be a string literal or a visible let binding initialized directly with a string literal |
| **AE0010** | 语义检查错误 | extern function annotation '@%.*s' fixed parameter count must be an integer literal |
| **AE0011** | 语义检查错误 | extern function annotation '@%.*s' fixed parameter count %lld is out of range for function with %zu parameters |
| **AE0012** | 符号未定义 | type '%.*s' is not defined |
| **AE0013** | 联合类型约束违反 | union-form spec '%.*s' forms a cycle through its member list |
| **AE0014** | 成员不存在 | union-form spec members cannot be 'void' |
| **AE0015** | 联合类型约束违反 | union-form spec '%.*s' must have at least one member |
| **AE0016** | 元组类型错误 | tuple literal requires a named tuple target type, got '%s' |
| **AE0017** | 类型不匹配 | tuple literal has %zu element(s) but tuple type '%s' expects %zu |
| **AE0020** | 除零错误 | %s by zero in %s '%s' expression |
| **AE0021** | 运算符使用错误 | unsupported compound assignment operator '%s' |
| **AE0022** | 不可变绑定赋值 | cannot assign to immutable binding '%.*s' |
| **AE0024** | 联合类型约束违反 | binary operator '%s' requires union-form operands to be narrowed to a single member first |
| **AE0031** | 分支结构错误 | %s branch block must end with an expression statement |
| **AE0033** | 分支结构错误 | if expressions require an else branch |
| **AE0035** | 返回语句错误 | catch clause must end with a result expression, 'return', or 'throw' |
| **AE0038** | 模式匹配错误 | match range label endpoints must be integer literals or 'let' bindings to integer literals |
| **AE0039** | 模式匹配错误 | match range label endpoints must be integer values |
| **AE0040** | 模式匹配错误 | match range label requires low <= high, got %lld and %lld |
| **AE0041** | 模式匹配错误 | match label must be a literal or a 'let' binding to a literal |
| **AE0042** | 模式匹配错误 | match label overlaps with an earlier label and is unreachable |
| **AE0043** | 联合类型约束违反 | union-form match labels must be union member types or 'else' |
| **AE0044** | 成员不存在 | type label '%s' is not a member of the target union-form spec |
| **AE0045** | 联合类型约束违反 | union-form spec metadata is unavailable |
| **AE0046** | 联合类型约束违反 | union match label overlaps with an earlier label and is unreachable |
| **AE0047** | 联合类型约束违反 | union match branch lists the same member more than once |
| **AE0048** | 模式匹配错误 | match expressions require an else branch |
| **AE0049** | 模式匹配错误 | match expression branches must have the same type, got '%s' and '%s' |
| **AE0055** | 返回语句错误 | %s body must use 'return;' without a value |
| **AE0056** | 调用目标无效 | expression '%s' is a borrowed data pointer formed by '&'; borrowed data pointers are only guaranteed valid for the current extern call and cannot be returned; retain the original owner instead of returning the raw pointer |
| **AE0059** | 枚举定义错误 | enum '%.*s' cannot mix explicit and implicit item values |
| **AE0060** | 重复定义 | enum '%.*s' has duplicate item name '%.*s' |
| **AE0061** | 重复定义 | enum '%.*s' has duplicate item value %lld |
| **AE0062** | 语义检查错误 | @%s can only be applied to methods |
| **AE0063** | 注解使用错误 | type '%.*s' has multiple @iterable methods |
| **AE0064** | 注解使用错误 | type '%.*s' has multiple @iterator methods |
| **AE0065** | 注解使用错误 | type '%.*s' cannot have both @iterable and @iterator |
| **AE0066** | 注解使用错误 | @iterable method must take no parameters |
| **AE0067** | 返回语句错误 | return type of @iterable method has no @iterator method |
| **AE0068** | 注解使用错误 | @iterator method must take no parameters |
| **AE0069** | 返回语句错误 | @iterator method must return a named tuple type of the form (bool, E) |
| **AE0070** | 泛型使用错误 | generic type '%.*s' cannot declare a finalizer |
| **AE0071** | 语义检查错误 | type '%.*s' is marked as @abi and cannot declare a finalizer |
| **AE0072** | 语义检查错误 | type '%.*s' declares more than one finalizer |
| **AE0073** | 语义检查错误 | '%s' cannot appear directly inside an 'if' expression block; |
| **AE0074** | 异常捕获错误 | '%s' cannot appear directly inside a catch block; |
| **AE0075** | 语义检查错误 | '%s' statement is only allowed inside a 'while' or 'for' loop |
| **AE0077** | 异常抛出错误 | throw statement requires a non-void expression |
| **AE0078** | 调用目标无效 | call target '%s' does not accept an existing array at a variadic argument position; pass elements individually |
| **AE0079** | 调用目标无效 | call target '%s' has no function type overload accepting %zu argument(s) |
| **AE0080** | 调用目标无效 | expression '%s' is not callable |
| **AE0081** | 访问权限不足 | static member '%.*s' of type '%.*s' is not accessible from the current module |
| **AE0082** | 成员不存在 | type '%.*s' has no static member '%.*s' |
| **AE0083** | 枚举定义错误 | enum '%.*s' has no item '%.*s' |
| **AE0084** | 静态成员访问错误 | static member '%.*s' must be accessed through its type |
| **AE0085** | 联合类型约束违反 | union-form constrained value must be narrowed to a single member before accessing member '%.*s' |
| **AE0086** | 成员不存在 | type '%s' has no member '%.*s' |
| **AE0087** | 联合类型约束违反 | union-form spec '%.*s' must be narrowed to a single member before accessing member '%.*s' |
| **AE0088** | 成员不存在 | spec '%.*s' is callable-form and has no member '%.*s' |
| **AE0089** | 成员不存在 | spec '%.*s' has no member '%.*s' |
| **AE0090** | 成员不存在 | type '%.*s' has no member '%.*s' |
| **AE0091** | 访问权限不足 | fit body cannot access private member '%.*s' of target type '%.*s' |
| **AE0092** | 访问权限不足 | member '%.*s' of type '%.*s' is not accessible from the current module |
| **AE0093** | 语义检查错误 | object literal field '%.*s' repeats final binding of let member '%.*s' already completed by declaration initializer |
| **AE0094** | 语义检查错误 | object literal field '%.*s' repeats final binding of let member '%.*s' already completed by constructor '%.*s' |
| **AE0095** | 成员不存在 | let member '%.*s' cannot be directly assigned outside constructors |
| **AE0096** | 语义检查错误 | constructor assignment repeats final binding of let member '%.*s' already completed by declaration initializer |
| **AE0097** | 语义检查错误 | constructor assignment repeats final binding of let member '%.*s' more than once in constructor '%.*s' |
| **AE0098** | 类型不匹配 | array literal element at index %zu does not match expected type '%s' |
| **AE0099** | 语义检查错误 | assignment target '%s' is not writable |
| **AE0101** | 调用目标无效 | lambda expression in %s requires an explicit callable-form spec target type |
| **AE0102** | 调用目标无效 | argument %zu expression '%s' is a borrowed data pointer formed by '&'; borrowed data pointers are only guaranteed valid for the current extern call and cannot be passed to non-extern callable '%s'; retain the original owner and form the pointer at the extern boundary |
| **AE0103** | 调用目标无效 | object literal field '%.*s' cannot store borrowed data pointer expression '%s'; borrowed data pointers are only guaranteed valid for the current extern call; retain the original owner instead of caching the raw pointer |
| **AE0104** | 调用目标无效 | assignment target '%s' cannot store borrowed data pointer expression '%s'; borrowed data pointers are only guaranteed valid for the current extern call; retain the original owner instead of caching the raw pointer |
| **AE0105** | 类型不匹配 | expression '%s' has multiple overloads matching expected ABI function pointer type '%s' |
| **AE0106** | 类型不匹配 | expression '%s' has multiple overloads matching expected function type '%s' |
| **AE0107** | 类型不匹配 | expression '%s' does not match expected function type '%s' |
| **AE0108** | 联合类型约束违反 | expression '%s' matches multiple members of union-form spec '%s'; use an explicit cast to select the target member |
| **AE0109** | 类型不匹配 | expression '%s' does not match expected type '%s' |
| **AE0110** | 重复定义 | duplicate method signature '%.*s' in type '%.*s' |
| **AE0111** | 返回语句错误 | method overloads in type '%.*s' cannot differ only by return type: '%.*s' |
| **AE0112** | 重载解析失败 | variadic method overload conflicts with existing method '%.*s' in type '%.*s' |
| **AE0113** | 重复定义 | duplicate method signature '%.*s' in fit target '%s' |
| **AE0114** | 返回语句错误 | method overloads in fit target '%s' cannot differ only by return type: '%.*s' |
| **AE0115** | fit 实现错误 | variadic method overload conflicts with existing method '%.*s' in fit target '%s' |
| **AE0116** | 调用目标无效 | object-form spec '%.*s' cannot be marked as @abi; @abi only applies to type declarations and callable-form spec |
| **AE0117** | 调用目标无效 | spec '%.*s' cannot use calling convention annotations |
| **AE0118** | 联合类型约束违反 | union-form spec '%.*s' cannot be marked as @abi; union values use compiler-managed aggregate layout |
| **AE0119** | 调用目标无效 | type '%.*s' cannot use calling convention annotations |
| **AE0120** | 调用目标无效 | type '%.*s' cannot be marked as @abi because calling convention annotations do not apply to type declarations |
| **AE0121** | 语义检查错误 | type '%.*s' cannot be marked as @abi because parameter '%.*s' uses non-ABI-stable type '%s' |
| **AE0122** | 返回语句错误 | type '%.*s' cannot be marked as @abi because return type '%s' is not ABI-stable |
| **AE0123** | 语义检查错误 | type '%.*s' cannot be marked as @abi because field '%.*s' uses non-ABI-stable type '%s' |
| **AE0124** | 调用目标无效 | %s '%.*s' cannot use calling convention annotations unless it is marked as @abi or declared extern |
| **AE0125** | 语义检查错误 | %s '%.*s' cannot be marked as @abi because extern functions declare imported C symbols |
| **AE0126** | 调用目标无效 | %s '%.*s' cannot be marked as @abi because it uses more than one calling convention annotation |
| **AE0127** | 调用目标无效 | %s '%.*s' cannot be marked as @abi because calling convention annotations on @abi declarations must not take library arguments |
| **AE0128** | 语义检查错误 | %s '%.*s' cannot be marked as @abi because parameter '%.*s' uses non-ABI-stable type '%s' |
| **AE0129** | 返回语句错误 | %s '%.*s' cannot be marked as @abi because return type '%s' is not ABI-stable |
| **AE0130** | 语义检查错误 | %s '%.*s' cannot be marked as @abi because uncaught exceptions must not cross the @abi ABI boundary |
| **AE0131** | 语义检查错误 | extern function '%.*s' parameter '%.*s' type '%s' is not C ABI-stable |
| **AE0132** | 返回语句错误 | extern function '%.*s' return type '%s' is not C ABI-stable |
| **AE0133** | 语义检查错误 | constructor '%.*s' cannot use ABI annotations |
| **AE0134** | 语义检查错误 | finalizer '~%.*s' cannot use ABI annotations |
| **AE0135** | 重载解析失败 | expression '%s' requires an explicit target function type to resolve overloads |
| **AE0136** | 语义检查错误 | expression '%s' requires an explicit target Foo* type to form an ABI function pointer |
| **AE0137** | 语义检查错误 | expression '%s' cannot form an ABI function pointer; ABI function pointers can only be formed from top-level @abi functions with an explicit Foo* target type |
| **AE0138** | 语义检查错误 | empty array literal requires an explicit target array type |
| **AE0139** | 元组类型错误 | tuple literal requires an explicit named tuple target type |
| **AE0140** | 语义检查错误 | %s '%.*s' is not an object type and cannot be constructed |
| **AE0141** | 语义检查错误 | type '%.*s' has no constructor accepting %zu argument(s) |
| **AE0142** | 访问权限不足 | type '%.*s' has multiple accessible constructors matching %zu argument(s); argument types are ambiguous |
| **AE0143** | 访问权限不足 | type '%.*s' has no accessible constructor accepting %zu argument(s) |
| **AE0144** | 模式匹配错误 | function '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| **AE0145** | 重载解析失败 | function '%.*s.%.*s' has no overload accepting %zu argument(s) |
| **AE0146** | 模式匹配错误 | static method '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| **AE0147** | 静态成员访问错误 | static method '%.*s.%.*s' has no overload accepting %zu argument(s) |
| **AE0148** | 模式匹配错误 | method '%.*s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| **AE0149** | 重载解析失败 | method '%.*s.%.*s' has no overload accepting %zu argument(s) |
| **AE0150** | 模式匹配错误 | method '%s.%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| **AE0151** | 模式匹配错误 | top-level function '%.*s' has multiple overloads matching %zu argument(s); argument types are ambiguous |
| **AE0152** | 重载解析失败 | top-level function '%.*s' has no overload accepting %zu argument(s) |
| **AE0153** | 语义检查错误 | object literal target '%s' must resolve to an object type |
| **AE0154** | 重复定义 | duplicate object literal field '%.*s' for type '%.*s' |
| **AE0155** | 语义检查错误 | object literal field '%.*s' is not a field of type '%.*s' |
| **AE0156** | 访问权限不足 | object literal field '%.*s' is not accessible for type '%.*s' |
| **AE0157** | 重复定义 | duplicate symbol '%.*s' in module '%.*s' |
| **AE0160** | 语义检查错误 | type parameter '%.*s' cannot take type arguments |
| **AE0161** | 语义检查错误 | unknown type '%s' |
| **AE0162** | 泛型使用错误 | '%.*s' is not a generic type and does not take type arguments |
| **AE0163** | 类型不匹配 | '%.*s' expects %zu type argument(s), but %zu were provided |
| **AE0164** | 异常捕获错误 | type 'unknown' is only valid as a catch clause type |
| **AE0165** | 返回语句错误 | type 'void' is only valid as a function return type |
| **AE0166** | 语义检查错误 | module alias '%.*s' cannot be used as a type by itself; use '%.*s.Name' |
| **AE0167** | 语义检查错误 | module alias '%.*s' does not export public name '%.*s' from module '%s' |
| **AE0168** | 异常抛出错误 | unknown catch value '%.*s' can only be used in 'throw %.*s' |
| **AE0169** | 语义检查错误 | module alias '%.*s' must be accessed as '%.*s.name' |
| **AE0170** | 符号未定义 | undefined identifier '%.*s' |
| **AE0171** | 语义检查错误 | 'self' is only available inside type methods and constructors |
| **AE0172** | 语义检查错误 | array size must be an integer expression |
| **AE0173** | 泛型使用错误 | explicit generic target '%s' cannot be used as a value expression |
| **AE0174** | 元组类型错误 | destructuring binding initializer must be a tuple |
| **AE0175** | 元组类型错误 | destructuring binding has %zu position(s) but tuple initializer has %zu |
| **AE0176** | 元组类型错误 | destructuring binding has %zu position(s) but tuple literal has %zu |
| **AE0177** | 语义检查错误 | destructuring bindings cannot use a single type annotation |
| **AE0178** | 模式匹配错误 | catch clause matching any exception must be the last catch clause |
| **AE0180** | 注解使用错误 | type '%s' is not iterable (no @iterable or @iterator method found) |
| **AE0181** | 语义检查错误 | type parameter '%.*s' shadows an outer type parameter with the same name |
| **AE0182** | 规约定义错误 | spec '%.*s' parent spec list must contain only spec types but found '%s' |
| **AE0183** | 规约定义错误 | object-form spec parent list can only contain object-form specs |
| **AE0184** | 规约定义错误 | spec '%.*s' lists '%.*s' more than once in its parent spec list |
| **AE0185** | 循环依赖 | spec '%.*s' forms a cycle through its parent spec list |
| **AE0186** | 规约定义错误 | object-form spec '%.*s' cannot declare a constructor |
| **AE0187** | 规约定义错误 | object-form spec '%.*s' cannot declare a finalizer |
| **AE0188** | 规约定义错误 | type '%.*s' is missing field '%.*s' required by spec '%.*s' |
| **AE0189** | 类型不匹配 | type '%.*s' field '%.*s' mutability does not match spec '%.*s' (expected '%s') |
| **AE0190** | 模式匹配错误 | type '%.*s' field '%.*s' type '%s' does not match spec '%.*s' field type '%s' |
| **AE0191** | 模式匹配错误 | type '%.*s' method '%.*s' signature does not match spec '%.*s' |
| **AE0192** | 规约定义错误 | type '%.*s' is missing method '%.*s' required by spec '%.*s' |
| **AE0193** | fit 实现错误 | type '%.*s' has multiple visible implementations of method '%.*s' required by spec '%.*s' (one or more fits and/or the type itself) |
| **AE0194** | fit 实现错误 | fit target has multiple visible implementations of method '%.*s' required by spec '%.*s' |
| **AE0195** | 返回语句错误 | type '%.*s' satisfies specs '%.*s' and '%.*s' which both declare method '%.*s' with the same parameters but different return types |
| **AE0196** | 模式匹配错误 | method overloads in type '%.*s' may both match the same arguments under visible contract relations: '%.*s' |
| **AE0197** | 模式匹配错误 | method overloads in fit target '%s' may both match the same arguments under visible contract relations: '%.*s' |
| **AE0198** | 模式匹配错误 | function overloads may both match the same arguments under visible contract relations: '%.*s' |
| **AE0199** | 规约定义错误 | type '%.*s' declared spec list must contain only spec types but found '%s' |
| **AE0200** | 规约定义错误 | type '%.*s' declared spec list can only contain object-form specs |
| **AE0201** | 规约定义错误 | type '%.*s' lists '%.*s' more than once in its declared spec list |
| **AE0202** | fit 实现错误 | fit with spec clause requires a body; |
| **AE0203** | fit 实现错误 | fit spec '%s' could not be resolved |
| **AE0204** | fit 实现错误 | fit specs list can only contain object-form specs |
| **AE0205** | fit 实现错误 | fit target must be a concrete type but found '%s' |
| **AE0206** | fit 实现错误 | fit target for generic type '%.*s' must reference all target type parameters directly |
| **AE0207** | fit 实现错误 | fit target for generic type '%.*s' must use target type parameter '%.*s' at position %zu |
| **AE0208** | fit 实现错误 | fit target type '%.*s' is not generic |
| **AE0209** | fit 实现错误 | fit specs list must contain only spec types but found '%s' |
| **AE0210** | fit 实现错误 | fit lists '%.*s' more than once in its specs clause |
| **AE0211** | 元组类型错误 | type parameter '%.*s': tuple type cannot be used as a constraint; use a spec constraint |
| **AE0212** | 规约定义错误 | type parameter '%.*s': constraint must be a spec, not a type |
| **AE0221** | 重复定义 | duplicate import alias '%.*s' in the same file |
| **AE0222** | 语义检查错误 | import target module '%s' was not found in current compilation input |
| **AE0223** | 返回语句错误 | function '%.*s' requires an explicit return type because its return type could not be inferred |
| **AE0224** | 返回语句错误 | method '%.*s.%.*s' requires an explicit return type because its return type could not be inferred |
| **AE0225** | 重复定义 | duplicate 'main' entry: target 'bin' requires exactly one 'main(args: string[])' across all programs |
| **AE0226** | 语义检查错误 | target 'bin' requires a 'main(args: string[])' entry function but none was found |
| **AE0227** | 语义检查错误 | 'main' entry must have signature 'main(args: string[])' |
| **AE0228** | 返回语句错误 | 'main' entry must return void |


## CE - 发码错误

| 错误码 | 用途 | 错误文案 |
|--------|------|----------|
| **CE0001** | 调试信息 | codegen: missing debug display type |
| **CE0002** | 调试信息 | codegen: missing debug source mapping for '%s' |
| **CE0003** | 泛型实例化 | codegen: generic constraint for '%.*s' must be a spec supported by codegen |
| **CE0004** | 泛型实例化 | codegen: internal: generic constraint spec was not registered |
| **CE0005** | 泛型实例化 | codegen: no reified_type_dep found for generic type method call |
| **CE0006** | 泛型实例化 | codegen: cg_rtd_expr_for_type called on non-generic type |
| **CE0007** | 泛型实例化 | codegen: no reified_type_dep found for generic cross-type call |
| **CE0008** | 聚合描述符缺失 | codegen: missing aggregate descriptor for capture cell |
| **CE0009** | 聚合描述符缺失 | codegen: missing aggregate default-init rule for capture cell |
| **CE0010** | 外部接口/ABI | codegen: extern annotation %s argument must resolve to a string literal |
| **CE0011** | 外部接口/ABI | codegen: malformed extern annotation string literal |
| **CE0012** | 外部接口/ABI | codegen: unknown extern annotation string escape '\\%c' |
| **CE0013** | 外部接口/ABI | codegen: extern annotation %s argument is missing |
| **CE0014** | 外部接口/ABI | codegen: extern annotation %s argument must be a string literal or visible let binding |
| **CE0015** | 外部接口/ABI | codegen: extern annotation %s argument must be a string literal or visible let binding initialized directly with a string literal |
| **CE0016** | 枚举处理 | codegen: out of memory tracking emitted enum |
| **CE0017** | 泛型实例化 | codegen: internal: generic constraint spec moved before it could be indexed |
| **CE0018** | 泛型实例化 | codegen: internal: generic constraint spec index is out of range |
| **CE0019** | 类型推断 | codegen: unsupported builtin inferred type for global binding '%.*s' |
| **CE0020** | 类型推断 | codegen: inferred type for global binding '%.*s' is missing its declaration |
| **CE0021** | 类型推断 | codegen: unsupported declared inferred type for global binding '%.*s' |
| **CE0022** | 类型推断 | codegen: module-level binding requires an explicit type or initializer |
| **CE0023** | 类型推断 | codegen: missing semantic type fact for inferred global binding '%.*s'; add an explicit type if this persists |
| **CE0024** | 规约处理 | codegen: coercion target did not resolve to a concrete spec instance |
| **CE0025** | 联合类型处理 | codegen: union coercion target did not resolve to a concrete union-form spec |
| **CE0026** | 可调用对象 | codegen: invalid callable function-value request |
| **CE0027** | 可调用对象 | codegen: callable value source function was not registered |
| **CE0028** | 可调用对象 | codegen: invalid callable method-value request |
| **CE0029** | 泛型实例化 | codegen: generic type '%.*s' expects %zu type argument(s), got %zu |
| **CE0030** | 泛型实例化 | codegen: generic spec '%.*s' expects %zu type argument(s), got %zu |
| **CE0031** | 泛型实例化 | codegen: generic type/spec instance '%.*s<...>' was not registered |
| **CE0032** | 未知类型 | codegen: unknown type '%.*s' |
| **CE0033** | 外部接口/ABI | codegen: this pointee type does not support ABI pointer lowering |
| **CE0034** | 未知类型 | codegen: unknown type reference kind |
| **CE0035** | 类型推断 | codegen: unsupported builtin inferred type for field |
| **CE0036** | 代码生成 | codegen: inferred field type is missing its declaration |
| **CE0037** | 代码生成 | codegen: unsupported declared inferred field type |
| **CE0038** | 类型推断 | codegen: field '%.*s' requires an explicit type or semantic inferred type |
| **CE0039** | 规约处理 | codegen: spec member kind not supported (Step 4b-α only handles fields/methods) |
| **CE0040** | 外部接口/ABI | codegen: @runtime extern func '%.*s' is not declared by runtime contract |
| **CE0041** | 泛型实例化 | codegen: generic extern func '%.*s' parameter '%.*s' does not lower to a single external surface |
| **CE0042** | 泛型实例化 | codegen: generic extern func '%.*s' return type does not lower to a single external surface |
| **CE0043** | 代码生成 | codegen: type already declares a finalizer |
| **CE0044** | 联合类型处理 | codegen: union-form spec has no normalized members |
| **CE0045** | 泛型实例化 | codegen: union-form spec member layout requires a concrete type argument |
| **CE0046** | 规约处理 | codegen: spec parent did not resolve to an object-form spec |
| **CE0047** | 规约处理 | codegen: recursive spec parent registration detected |
| **CE0048** | fit 实现 | codegen: fit spec did not resolve to a known user spec |
| **CE0049** | fit 实现 | codegen: fit target did not resolve to a builtin/array type |
| **CE0050** | fit 实现 | codegen: fit target is missing |
| **CE0051** | fit 实现 | codegen: only named or array fit targets are supported |
| **CE0052** | fit 实现 | codegen: fit target did not resolve to a known user type |
| **CE0053** | fit 实现 | codegen: fit target type '%.*s' is not a known user type |
| **CE0054** | fit 实现 | codegen: only methods are supported in fit bodies |
| **CE0055** | 聚合描述符缺失 | codegen: union-form aggregate member is missing a descriptor |
| **CE0056** | 联合类型处理 | codegen: union-form spec has no members |
| **CE0057** | 聚合描述符缺失 | codegen: union default aggregate member is missing a descriptor |
| **CE0058** | 泛型实例化 | codegen: missing generic descriptor for tuple field '%s' |
| **CE0059** | 泛型实例化 | codegen: tuple field '%s' generic initializer type mismatch |
| **CE0060** | 聚合描述符缺失 | codegen: missing aggregate descriptor for tuple field '%s' |
| **CE0061** | 聚合描述符缺失 | codegen: missing aggregate default-init rule for tuple field '%s' |
| **CE0062** | 元组处理 | codegen: internal tuple aggregate ownership state was not materialized |
| **CE0063** | 元组处理 | codegen: tuple aggregate field initializer type mismatch |
| **CE0064** | 元组处理 | codegen: tuple literal requires a named tuple target type |
| **CE0065** | 元组处理 | codegen: tuple literal arity does not match target tuple type '%s' |
| **CE0066** | 泛型实例化 | codegen: tuple type '%s' requires reified layout but no |
| **CE0067** | 元组处理 | codegen: tuple spec coercion is missing box metadata |
| **CE0068** | 聚合描述符缺失 | codegen: missing tuple aggregate descriptor for spec coercion |
| **CE0069** | 元组处理 | codegen: tuple cast source must be a tuple value |
| **CE0070** | 元组处理 | codegen: tuple cast arity mismatch |
| **CE0071** | 赋值/绑定 | codegen: cannot assign to immutable imported binding '%.*s' |
| **CE0072** | 赋值/绑定 | codegen: compound assignment requires a numeric binding type |
| **CE0073** | 赋值/绑定 | codegen: unsupported compound assignment operator |
| **CE0074** | 聚合描述符缺失 | codegen: missing aggregate descriptor for assignment |
| **CE0075** | 聚合描述符缺失 | codegen: missing aggregate descriptor for spec return |
| **CE0076** | 构造器/初始化 | codegen: constructor arguments require a resolved user-defined constructor |
| **CE0077** | 构造器/初始化 | codegen: wrong argument count for constructor '%s' (expected %zu, got %zu) |
| **CE0078** | 字面量处理 | codegen: malformed string literal |
| **CE0079** | 字面量处理 | codegen: invalid \\x escape: expected 2 hex digits |
| **CE0080** | 字面量处理 | codegen: invalid \\x escape: expected hex digit |
| **CE0081** | 字面量处理 | codegen: unknown string escape '\\%c' |
| **CE0082** | 代码生成 | codegen: unsupported literal kind |
| **CE0083** | 代码生成 | codegen: cannot apply numeric op to non-numeric operands |
| **CE0084** | 规约处理 | codegen: spec equality requires aggregate spec operands |
| **CE0085** | 聚合描述符缺失 | codegen: tuple type has no aggregate descriptor for equality |
| **CE0086** | 代码生成 | codegen: unsupported binary operator |
| **CE0087** | 代码生成 | codegen: && / \|\| require bool operands |
| **CE0088** | 代码生成 | codegen: ordering comparisons require numeric operands |
| **CE0089** | 代码生成 | codegen: unsupported float modulo operation |
| **CE0090** | 代码生成 | codegen: unary '&' is missing an operand type |
| **CE0091** | 外部接口/ABI | codegen: this ABI-compatible array element type does not support data-pointer lowering |
| **CE0092** | 外部接口/ABI | codegen: this operand type does not support ABI pointer formation; only string, ABI scalar, fielded @abi value, and ABI-compatible array operands are allowed here |
| **CE0093** | 代码生成 | codegen: unsupported unary operator |
| **CE0094** | 代码生成 | codegen: '!' requires bool operand |
| **CE0095** | 代码生成 | codegen: '~' requires integer operand |
| **CE0096** | 代码生成 | codegen: unary +/- requires numeric operand |
| **CE0097** | 可调用对象 | codegen: lambda capture was not lowered to a capture cell |
| **CE0098** | 可调用对象 | codegen: lambda argument name overflow |
| **CE0099** | 可调用对象 | codegen: callable lambda coercion is missing lambda semantic data |
| **CE0100** | 规约处理 | codegen: callable lambda coercion target was not registered as a callable-form spec |
| **CE0101** | 可调用对象 | codegen: callable lambda coercion parameter count mismatch |
| **CE0102** | 可调用对象 | codegen: lambda capture '%.*s' was not lowered to a capture cell |
| **CE0103** | 可调用对象 | codegen: lambda self capture was not lowered to a capture cell |
| **CE0104** | 代码生成 | codegen: identifier '%.*s' not found |
| **CE0105** | 规约处理 | codegen: callable coercion target was not registered as a callable-form spec |
| **CE0106** | 可调用对象 | codegen: only top-level function callable coercions are supported in this step |
| **CE0107** | 外部接口/ABI | codegen: ABI function pointer target was not registered as a callable-form spec |
| **CE0108** | 外部接口/ABI | codegen: ABI function pointers currently support only top-level @abi functions |
| **CE0109** | 外部接口/ABI | codegen: ABI function pointer source function was not registered |
| **CE0110** | 可调用对象 | codegen: callable method coercion requires a member expression |
| **CE0111** | 可调用对象 | codegen: callable method coercion is missing semantic resolution data |
| **CE0112** | 可调用对象 | codegen: callable method coercion source must be an object value |
| **CE0113** | 可调用对象 | codegen: callable method coercion receiver type does not match resolved owner type |
| **CE0114** | 可调用对象 | codegen: callable method coercion source method was not registered |
| **CE0115** | 可调用对象 | codegen: callable-form coercion requires source/target callable signatures to match |
| **CE0116** | 可调用对象 | codegen: callable-form coercion source must be a callable value |
| **CE0117** | 可调用对象 | codegen: callable-form lambda/method coercion not yet supported in this step |
| **CE0118** | 联合类型处理 | codegen: union coercion target member is invalid |
| **CE0119** | 代码生成 | codegen: 'self' used outside of method body |
| **CE0120** | 泛型实例化 | codegen: explicit generic target must be consumed before emission |
| **CE0121** | 元组处理 | codegen: tuple literal requires an explicit named tuple target type |
| **CE0122** | 代码生成 | codegen: expression kind not yet supported in this iteration |
| **CE0123** | 聚合描述符缺失 | codegen: missing aggregate descriptor for variadic element type |
| **CE0124** | 代码生成 | codegen: undefined function '%.*s' |
| **CE0125** | 代码生成 | codegen: too few arguments for variadic function '%.*s' (need at least %zu, got %zu) |
| **CE0126** | 构造器/初始化 | codegen: wrong argument count for '%.*s' (expected %zu, got %zu) |
| **CE0127** | 规约处理 | codegen: callable value call requires a callable-form spec type |
| **CE0128** | 可调用对象 | codegen: too few arguments for variadic callable '%s' (need at least %zu, got %zu) |
| **CE0129** | 可调用对象 | codegen: wrong argument count for callable '%s' (expected %zu, got %zu) |
| **CE0130** | 泛型实例化 | codegen: generic direct call requires a callable-form spec constraint |
| **CE0131** | 规约处理 | codegen: too few arguments for variadic callable constraint '%s' (need at least %zu, got %zu) |
| **CE0132** | 规约处理 | codegen: wrong argument count for callable constraint '%s' (expected %zu, got %zu) |
| **CE0133** | 泛型实例化 | codegen: generic method constraint for '%.*s' must be a spec supported by codegen |
| **CE0134** | 泛型实例化 | codegen: internal: generic type method call missing method type parameters |
| **CE0135** | 泛型实例化 | codegen: method expects %zu type argument(s), got %zu |
| **CE0136** | 泛型实例化 | codegen: wrong argument count for generic method '%s' (expected %zu, got %zu) |
| **CE0137** | 泛型实例化 | codegen: cannot infer type argument %zu for generic method '%s' |
| **CE0138** | 泛型实例化 | codegen: cannot determine concrete generic method return type |
| **CE0139** | 泛型实例化 | codegen: generic type '%.*s' has no method '%.*s' |
| **CE0140** | 构造器/初始化 | codegen: wrong argument count for method '%.*s' (expected %zu, got %zu) |
| **CE0141** | 泛型实例化 | codegen: cannot infer type argument %zu for generic method '%.*s' |
| **CE0142** | 泛型实例化 | codegen: generic static method call requires an active generic descriptor context |
| **CE0143** | 泛型实例化 | codegen: internal: generic static method call missing type parameters |
| **CE0144** | 泛型实例化 | codegen: generic builtin static fit methods are not supported yet |
| **CE0145** | 泛型实例化 | codegen: static method expects %zu type argument(s), got %zu |
| **CE0146** | 泛型实例化 | codegen: too few arguments for variadic generic static method '%s' |
| **CE0147** | 泛型实例化 | codegen: wrong argument count for generic static method '%s' |
| **CE0148** | 泛型实例化 | codegen: cannot infer type argument %zu for generic static method '%s' |
| **CE0149** | 泛型实例化 | codegen: cannot determine generic static method return type |
| **CE0150** | 代码生成 | codegen: resolved static method was not registered |
| **CE0151** | 构造器/初始化 | codegen: wrong argument count for static method '%s' (expected %zu, got %zu) |
| **CE0152** | 代码生成 | codegen: too few arguments for variadic static method '%s' (need at least %zu, got %zu) |
| **CE0153** | 代码生成 | codegen: failed to determine static method return type |
| **CE0154** | 可调用对象 | codegen: imported binding '%.*s' is not callable |
| **CE0155** | 泛型实例化 | codegen: generic method call requires an object-form spec constraint |
| **CE0156** | 规约处理 | codegen: spec '%s' has no method '%.*s' |
| **CE0157** | 规约处理 | codegen: wrong argument count for spec method '%s' (expected %zu, got %zu) |
| **CE0158** | 泛型实例化 | codegen: missing descriptor for generic spec method return |
| **CE0159** | fit 实现 | codegen: builtin/array fit has no method '%.*s' |
| **CE0160** | 构造器/初始化 | codegen: wrong argument count for method '%s' (expected %zu, got %zu) |
| **CE0161** | 泛型实例化 | codegen: builtin fit generic descriptor emission only supports array targets in this phase |
| **CE0162** | fit 实现 | codegen: failed to instantiate builtin fit return type |
| **CE0163** | 代码生成 | codegen: method call on non-object value |
| **CE0164** | 代码生成 | codegen: type '%s' has no method '%.*s' |
| **CE0165** | 代码生成 | codegen: too few arguments for variadic method '%s' (need at least %zu, got %zu) |
| **CE0166** | 代码生成 | codegen: only direct or method calls supported in this iteration |
| **CE0167** | 泛型实例化 | codegen: generic type constructor instance for '%.*s' was not registered |
| **CE0168** | 构造器/初始化 | codegen: unknown type '%.*s' in constructor call |
| **CE0169** | 构造器/初始化 | codegen: resolved constructor for type '%s' was not registered |
| **CE0170** | 枚举处理 | codegen: enum '%.*s' has no item '%.*s' |
| **CE0171** | 泛型实例化 | codegen: generic type '%.*s' has no field '%.*s' |
| **CE0172** | 泛型实例化 | codegen: generic member access requires an object-form spec constraint |
| **CE0173** | 规约处理 | codegen: spec '%s' has no field '%.*s' |
| **CE0174** | 元组处理 | codegen: tuple type '%s' has no field '%.*s' |
| **CE0175** | 代码生成 | codegen: member access on non-object value |
| **CE0176** | 代码生成 | codegen: type '%s' has no field '%.*s' |
| **CE0177** | 模块/程序 | codegen: missing object literal target |
| **CE0178** | 未知类型 | codegen: unknown type '%.*s' in object literal |
| **CE0179** | 泛型实例化 | codegen: generic type object literal instance for '%.*s' was not registered |
| **CE0180** | 构造器/初始化 | codegen: only direct type constructor targets are supported for object literals |
| **CE0181** | 代码生成 | codegen: duplicate field '%s' in object literal |
| **CE0182** | 代码生成 | codegen: empty array literal needs an explicit element type |
| **CE0183** | 聚合描述符缺失 | codegen: missing aggregate descriptor for spec array element |
| **CE0184** | 泛型实例化 | codegen: generic empty array literal requires an active generic descriptor |
| **CE0185** | 代码生成 | codegen: heterogeneous array literal (all elements must share a type) |
| **CE0186** | 聚合描述符缺失 | codegen: missing aggregate descriptor for array-new element type |
| **CE0187** | 泛型实例化 | codegen: generic array-new requires an active generic descriptor |
| **CE0188** | 代码生成 | codegen: indexing requires an array value |
| **CE0189** | 代码生成 | codegen: array index must be an integer |
| **CE0190** | 泛型实例化 | codegen: generic array index requires an active generic descriptor |
| **CE0191** | 规约处理 | codegen: callable-form cast operand must be a callable-form spec value |
| **CE0192** | 类型转换 | codegen: array cast requires the same lowered array type |
| **CE0193** | 类型转换 | codegen: only numeric/bool casts supported in 1A iter 1 |
| **CE0194** | 类型转换 | codegen: cast operand must be numeric/bool |
| **CE0195** | 代码生成 | codegen: if-expression branches yield mismatched types |
| **CE0196** | 聚合描述符缺失 | codegen: missing aggregate descriptor for if/match result slot |
| **CE0197** | 代码生成 | codegen: try/catch branches yield mismatched types |
| **CE0198** | 聚合描述符缺失 | codegen: missing aggregate descriptor for try-expression result |
| **CE0199** | 代码生成 | codegen: if-expression branches must end with an expression statement |
| **CE0200** | 聚合描述符缺失 | codegen: missing aggregate descriptor for if-expression result |
| **CE0201** | 代码生成 | codegen: if-expression condition must be bool |
| **CE0202** | 聚合描述符缺失 | codegen: missing aggregate default-init rule for if-expression result |
| **CE0203** | 模块/程序 | codegen: range labels apply to integer match targets only |
| **CE0204** | 代码生成 | codegen: unknown match label kind |
| **CE0205** | 代码生成 | codegen: match expression requires an else branch |
| **CE0206** | 代码生成 | codegen: match expression else branch must end with an expression statement |
| **CE0207** | 代码生成 | codegen: match branch must end with an expression statement |
| **CE0208** | 聚合描述符缺失 | codegen: missing aggregate descriptor for match expression result |
| **CE0209** | 聚合描述符缺失 | codegen: missing aggregate default-init rule for match expression result |
| **CE0210** | 代码生成 | codegen: match branch has no labels |
| **CE0211** | 模块/程序 | codegen: match target must be integer, bool, string, or enum |
| **CE0212** | 赋值/绑定 | codegen: failed to register unknown catch binding |
| **CE0213** | 赋值/绑定 | codegen: missing catch binding type |
| **CE0214** | 赋值/绑定 | codegen: missing scalar catch binding payload field |
| **CE0215** | 代码生成 | codegen: catch block must produce a try-expression value |
| **CE0216** | 聚合描述符缺失 | codegen: missing aggregate default-init rule for try-expression result |
| **CE0217** | 规约处理 | codegen: spec coercion source type is missing |
| **CE0218** | 规约处理 | codegen: spec coercion references type outside current codegen scope |
| **CE0219** | 规约处理 | codegen: scalar spec coercion has unsupported source kind |
| **CE0220** | 规约处理 | codegen: object-form spec coercion source kind is invalid |
| **CE0221** | 枚举处理 | codegen: enum has no items for default value |
| **CE0222** | 枚举处理 | codegen: out of memory emitting enum default value |
| **CE0223** | 可调用对象 | codegen: cannot produce default value for unresolved callable type |
| **CE0224** | 聚合描述符缺失 | codegen: missing aggregate descriptor for spec array default-zero |
| **CE0225** | 代码生成 | codegen: cannot default-zero an unresolved object type |
| **CE0226** | 构造器/初始化 | codegen: type '%s' contains reference cycles and has no default zero value; provide an explicit initializer |
| **CE0227** | 代码生成 | codegen: cannot produce default value for this type |
| **CE0228** | 聚合描述符缺失 | codegen: missing aggregate descriptor for local binding |
| **CE0229** | 构造器/初始化 | codegen: destructuring binding requires an initializer |
| **CE0230** | 代码生成 | codegen: destructuring arity mismatch |
| **CE0231** | 元组处理 | codegen: destructuring source must be a tuple value |
| **CE0232** | 聚合描述符缺失 | codegen: missing aggregate descriptor for field '%s' |
| **CE0233** | 聚合描述符缺失 | codegen: missing aggregate default-init rule for field '%s' |
| **CE0234** | 构造器/初始化 | codegen: binding without type or initializer not supported |
| **CE0235** | 聚合描述符缺失 | codegen: missing aggregate descriptor for spec local |
| **CE0236** | 聚合描述符缺失 | codegen: missing aggregate default-init rule |
| **CE0237** | 赋值/绑定 | codegen: indexed assignment requires an array value |
| **CE0238** | 赋值/绑定 | codegen: compound indexed assignment requires a numeric element type |
| **CE0239** | 赋值/绑定 | codegen: unsupported compound indexed assignment operator |
| **CE0240** | 泛型实例化 | codegen: missing generic descriptor for array element assignment |
| **CE0241** | 泛型实例化 | codegen: generic array element assignment requires a value with the same generic type parameter |
| **CE0242** | 泛型实例化 | codegen: tuple array element requires reified layout |
| **CE0243** | 聚合描述符缺失 | codegen: missing aggregate descriptor for spec array element write |
| **CE0244** | 赋值/绑定 | codegen: cannot assign to immutable static binding '%s.%s' |
| **CE0245** | 赋值/绑定 | codegen: compound assignment requires a numeric static binding type |
| **CE0246** | 聚合描述符缺失 | codegen: missing aggregate descriptor for static binding write |
| **CE0247** | 泛型实例化 | codegen: compound assignment to generic field requires a concrete numeric field type |
| **CE0248** | 泛型实例化 | codegen: generic field assignment requires a value with the same generic type parameter |
| **CE0249** | 赋值/绑定 | codegen: compound member assignment requires a numeric field type |
| **CE0250** | 赋值/绑定 | codegen: unsupported compound member assignment operator |
| **CE0251** | 泛型实例化 | codegen: missing aggregate descriptor for generic method field write |
| **CE0252** | 泛型实例化 | codegen: generic member assignment requires an object-form spec constraint |
| **CE0253** | 规约处理 | codegen: spec field '%s' is not declared `var` |
| **CE0254** | 规约处理 | codegen: compound spec field assignment requires a numeric field type |
| **CE0255** | 规约处理 | codegen: unsupported compound spec field assignment operator |
| **CE0256** | 赋值/绑定 | codegen: member assignment on non-object value |
| **CE0257** | 聚合描述符缺失 | codegen: missing aggregate descriptor for member assignment |
| **CE0258** | 赋值/绑定 | codegen: only identifier or member assignments supported in this iteration |
| **CE0259** | 赋值/绑定 | codegen: assignment to undefined identifier '%.*s' |
| **CE0260** | 赋值/绑定 | codegen: cannot assign to immutable module binding '%s' |
| **CE0261** | 聚合描述符缺失 | codegen: missing aggregate descriptor for module assignment |
| **CE0262** | 赋值/绑定 | codegen: compound assignment requires a numeric local type |
| **CE0263** | 聚合描述符缺失 | codegen: missing aggregate descriptor for local assignment |
| **CE0264** | 代码生成 | codegen: void function cannot return a value |
| **CE0265** | 代码生成 | codegen: non-void function must return a value |
| **CE0266** | 代码生成 | codegen: if condition must be bool |
| **CE0267** | 代码生成 | codegen: too many nested else-if wrappers |
| **CE0268** | 联合类型处理 | codegen: union-form match label is not a normalized member |
| **CE0269** | 代码生成 | codegen: match branch has no labels |
| **CE0270** | 模块/程序 | codegen: match target must be integer, bool, string, or enum |
| **CE0271** | 代码生成 | codegen: while condition must be bool |
| **CE0272** | 代码生成 | codegen: for condition must be bool |
| **CE0273** | 代码生成 | codegen: @iterable method not found on source type |
| **CE0274** | 代码生成 | codegen: iterator cursor type not found |
| **CE0275** | 代码生成 | codegen: @iterator method not found on cursor type |
| **CE0276** | 元组处理 | codegen: @iterator return type must be a 2-field tuple |
| **CE0277** | 代码生成 | codegen: for/in sequence must be an array |
| **CE0278** | 聚合描述符缺失 | codegen: missing aggregate descriptor for spec for/in element |
| **CE0279** | 代码生成 | codegen: '%s' outside of loop |
| **CE0280** | 代码生成 | codegen: missing exception payload type |
| **CE0281** | 聚合描述符缺失 | codegen: object exception payload is missing a descriptor |
| **CE0282** | 代码生成 | codegen: unsupported exception payload type |
| **CE0283** | 代码生成 | codegen: 'throw' requires a value |
| **CE0284** | 规约处理 | codegen: spec fat value is missing its value struct name |
| **CE0285** | 构造器/初始化 | codegen: missing scalar box constructor for throw payload |
| **CE0286** | 代码生成 | codegen: statement kind not yet supported in this iteration |
| **CE0287** | 赋值/绑定 | codegen: imported public binding surface is missing a type |
| **CE0288** | 模块/程序 | codegen: main must return void or i32 |
| **CE0289** | 模块/程序 | codegen: main must have signature (args: string[]) |
| **CE0290** | 代码生成 | codegen: failed to substitute type params in reifiable dep |
| **CE0291** | 聚合描述符缺失 | codegen: failed to resolve descriptor name for reifiable dep |
| **CE0292** | 泛型实例化 | codegen: generic type argument forwarding requires an active generic descriptor context |
| **CE0293** | 泛型实例化 | codegen: forwarding a generic type argument across a different constraint surface requires a parent-compatible witness surface (G6) |
| **CE0294** | 泛型实例化 | codegen: constrained generic type argument currently requires a concrete user type, concrete spec value, matching outer generic parameter, or concrete builtin type |
| **CE0295** | 泛型实例化 | codegen: constrained generic type argument currently requires a concrete user type, concrete spec value, or matching outer generic parameter (G6) |
| **CE0296** | 泛型实例化 | codegen: trivial generic type argument requires a trivial descriptor |
| **CE0297** | 泛型实例化 | codegen: managed generic type argument requires a type descriptor |
| **CE0298** | 泛型实例化 | codegen: aggregate type as generic type argument not yet supported (missing flatten rule) (G6) |
| **CE0299** | 泛型实例化 | codegen: tuple return requires reified layout but no |
| **CE0300** | 泛型实例化 | codegen: out of memory while emitting generic aggregate return |
| **CE0301** | 泛型实例化 | codegen: missing aggregate descriptor for generic aggregate return |
| **CE0302** | 泛型实例化 | codegen: generic extern call is missing extern metadata |
| **CE0303** | 泛型实例化 | codegen: generic extern type argument count mismatch |
| **CE0304** | 泛型实例化 | codegen: cannot infer generic extern type arguments for '%.*s' from argument %zu |
| **CE0305** | 泛型实例化 | codegen: cannot infer type argument %zu for generic extern '%.*s' |
| **CE0306** | 泛型实例化 | codegen: generic function '%s' expects %zu type argument(s), got %zu |
| **CE0307** | 泛型实例化 | codegen: cannot determine concrete parameter type for generic function '%s' |
| **CE0308** | 泛型实例化 | codegen: cannot infer type argument %zu for generic function '%s' |
| **CE0309** | 代码生成 | codegen: cannot determine concrete return type |
| **CE0310** | 代码生成 | codegen: invalid scalar subject key |
| **CE0311** | 代码生成 | codegen: invalid scalar subject storage kind |
| **CE0312** | 泛型实例化 | codegen: spec value '%s' cannot satisfy generic constraint spec '%s' through mismatched spec forms |
| **CE0313** | 泛型实例化 | codegen: spec value '%s' cannot satisfy generic constraint spec '%s' through callable slot witness adaptation |
| **CE0314** | 泛型实例化 | codegen: spec value '%s' cannot satisfy generic constraint spec '%s' through slot witness adaptation |
| **CE0315** | 规约处理 | codegen: type '%s' is missing an implementation for spec '%s' member '%s' |
| **CE0316** | 规约处理 | codegen: field '%s' on type '%s' does not match spec '%s' field type |
| **CE0317** | fit 实现 | codegen: type '%s' has multiple visible implementations of method '%s' required by spec '%s' (one or more fits and/or the type itself) |
| **CE0318** | 规约处理 | codegen: internal: witness slot count mismatch for non-type subject |
| **CE0319** | 规约处理 | codegen: missing implementation for spec member '%s' |
| **CE0320** | fit 实现 | codegen: non-type subject key currently supports fit-method spec members only |
| **CE0321** | 规约处理 | codegen: enum '%.*s' cannot satisfy spec field '%s' without field support |
| **CE0322** | 规约处理 | codegen: enum '%.*s' is missing an implementation for spec '%s' member '%s' |
| **CE0323** | 规约处理 | codegen: enum '%.*s' has multiple visible implementations of method '%s' required by spec '%s' |
| **CE0324** | fit 实现 | codegen: fit implementation does not match object-form spec coercion source |
| **CE0325** | fit 实现 | codegen: fit method '%s' was not registered |
| **CE0326** | 规约处理 | codegen: witness source type is not registered in current module |
| **CE0327** | 规约处理 | codegen: invalid subject key for object-form spec coercion |
| **CE0328** | 规约处理 | codegen: object-form spec coercion has unknown subject key |
| **CE0329** | 规约处理 | codegen: missing semantic witness for object-form spec coercion |
| **CE0330** | 元组处理 | codegen: internal: witness slot count mismatch for tuple box (%s, %s) |
| **CE0331** | 元组处理 | codegen: tuple type '%s' is missing an implementation for spec '%s' member '%s' |
| **CE0332** | 元组处理 | codegen: internal: tuple fit decl for spec '%s' member '%s' not registered for type '%s' |
| **CE0333** | 元组处理 | codegen: internal: tuple fit method '%s' not found in fit body for type '%s' |
| **CE0334** | 元组处理 | codegen: internal: tuple type '%s' has no field '%s' to satisfy spec '%s' |
| **CE0335** | 元组处理 | codegen: tuple type '%s' cannot satisfy spec method '%s' without a fit method |
| **CE0336** | 元组处理 | codegen: tuple spec member '%s' must be implemented by a fit method |
| **CE0337** | 元组处理 | codegen: tuple spec field '%s' must be satisfied by a tuple field |
| **CE0338** | 元组处理 | codegen: tuple fields are immutable and cannot satisfy var spec field '%s' |
| **CE0339** | 规约处理 | codegen: internal: witness slot count mismatch for (%s, %s) |
| **CE0340** | fit 实现 | codegen: internal: fit decl for spec '%s' member '%s' not registered for type '%s' |
| **CE0341** | fit 实现 | codegen: internal: fit method '%s' not found in fit body for type '%s' |
| **CE0342** | 规约处理 | codegen: internal: type '%s' has no method '%s' to satisfy spec '%s' |
| **CE0343** | 规约处理 | codegen: internal: type '%s' has no field '%s' to satisfy spec '%s' |
| **CE0344** | fit 实现 | codegen: spec field '%s' cannot be satisfied by a fit method |
| **CE0345** | fit 实现 | codegen: internal: fit binding for spec '%s' member '%s' not registered for type '%s' |
| **CE0346** | 规约处理 | codegen: spec method '%s' must be implemented by a method on '%s' (Step 4b-α) |
| **CE0347** | 规约处理 | codegen: spec field '%s' must be satisfied by a field on '%s' |
| **CE0348** | 聚合描述符缺失 | codegen: missing aggregate descriptor for spec field write |
| **CE0349** | 外部接口/ABI | codegen: extern module-level bindings not supported in Phase 1A |
| **CE0350** | 编译器内部错误 | codegen: internal: type not registered |
| **CE0351** | fit 实现 | codegen: internal: fit not registered |
| **CE0352** | 编译器内部错误 | codegen: internal: type shell registration failed without diagnostic |
| **CE0353** | 泛型实例化 | codegen: internal: generic instance collection failed without diagnostic |
| **CE0354** | 编译器内部错误 | codegen: internal: user type member registration failed without diagnostic |
| **CE0355** | 编译器内部错误 | codegen: internal: function pre-registration failed without diagnostic |
| **CE0356** | 编译器内部错误 | codegen: internal: declaration emission failed without diagnostic |
| **CE0357** | 赋值/绑定 | codegen: internal: module binding ensure-init emission failed without diagnostic |
| **CE0358** | 赋值/绑定 | codegen: internal: type static binding ensure-init emission failed without diagnostic |
| **CE0359** | 聚合描述符缺失 | codegen: missing aggregate default-init rule for module binding |
| **CE0360** | 聚合描述符缺失 | codegen: missing aggregate descriptor for module binding |
| **CE0361** | 赋值/绑定 | codegen: internal: module binding ensure-init emitted before registration |
| **CE0362** | 聚合描述符缺失 | codegen: aggregate field has no descriptor (unknown aggregate kind) |
| **CE0363** | 聚合描述符缺失 | codegen: aggregate field has no descriptor symbol (unknown aggregate kind) |
| **CE0364** | 元组处理 | codegen: missing tuple equality function name |
| **CE0365** | 聚合描述符缺失 | codegen: tuple field '%s' has no managed equality descriptor |
| **CE0366** | 聚合描述符缺失 | codegen: tuple field '%s' has no aggregate equality descriptor |
| **CE0367** | 聚合描述符缺失 | codegen: tuple field '%s' has no aggregate descriptor |
| **CE0368** | 元组处理 | codegen: missing tuple field aggregate default initializer |
| **CE0369** | 泛型实例化 | codegen: internal: failed to emit shared generic method '%.*s' |
| **CE0370** | 泛型实例化 | codegen: internal: generic instance method missing origin type |
| **CE0371** | 泛型实例化 | codegen: internal: failed to emit generic method wrapper '%s.%s' |
| **CE0372** | 模块/程序 | codegen: no programs to compile |
| **CE0373** | 模块/程序 | codegen: bin target requires `main` function |


## IE - 基础错误

| 错误码 | 用途 | 错误文案 |
|--------|------|----------|
| **IE0001** | 内存不足 | out of memory |
| **IE0002** | 编译器内部错误 | internal compiler error |
