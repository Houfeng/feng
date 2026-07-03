# Feng 标准库 Parser 设计草案

> 本文记录基于 Feng 实现的语法分析器（Parser）设计方向，服务于未来自举。
> **状态**：草案阶段，尚未实现。依赖阶段一（Lexer）完成后启动。
> **目标**：在 `std/src/compiler/Parser/` 中实现完整的 Feng 语法分析器，作为自举编译器的前端。
> **前置**：`feng-std-lexer-draft.md`（阶段一：Lexer + Token + TokenStream）。

---

## 0 背景与定位

### 为什么需要 Feng 版 Parser

1. **自举核心组件**：Parser 是编译器前端的核心，将 Token 流转换为 AST，是自举的必经之路
2. **与 Lexer 配套**：阶段一交付了 Feng 版 Lexer，Parser 是其天然下游
3. **验证语言完备性**：用 Feng 实现自己的 Parser 是对语言表达力的终极验证

### 当前 C 版状态

- `src/parser/parser.c` (5032 行) + `src/parser/parser.h` + `src/parser/dump.c`
- 完整实现了 Feng 语言的递归下降语法分析
- AST 结构：Expr (22 种)、Stmt (14 种)、Decl (6 种)、TypeRef (3 种)、TypeMember (4 种)

### 与自定义注解的关系

自定义注解在 pre-Parse 阶段处理（Token 流变换），**不需要 Parser**。Parser 主要服务于自举目标。但 Parser 的 AST 类型设计需要与 C 版保持语义等价，以确保自举时行为一致。

---

## 1 Parser 从 Lexer 承接的职责

以下能力原在 C 版 Lexer 中实现，Feng 版按职责划分移至 Parser：

### 1.1 数值解析

Lexer 只扫描数字文本并产出 `LiteralInteger`/`LiteralFloat`/`LiteralBool` Token，不解析语义值。Parser 在构建 AST 时将 lexeme 解析为具体数值：

| C 版函数 | 功能 | Parser 对应 |
|----------|------|------------|
| `parse_integer_slice` | 解析整数文本为 `i64`（含十六进制/二进制/八进制/`_` 分隔） | 内部工具函数 |
| `parse_float_slice` | 解析浮点文本为 `f64`（含指数/`_` 分隔） | 内部工具函数 |

### 1.2 注解识别

Lexer 产出独立的 `OperatorAt` 和 `LiteralIdentifier` Token。Parser 在解析注解序列时组合二者，并查表分类内建/自定义注解：

| C 版函数 | 功能 | Parser 对应 |
|----------|------|------------|
| `feng_lookup_builtin_annotation` | 查表判断注解是内建还是自定义 | `parseAnnotations()` 内部 |
| `scan_annotation` | Lexer 整体扫描 `@name` | Parser 组合 `OperatorAt` + `LiteralIdentifier` |

**内建注解表（8 个）**：
`abi`, `cdecl`, `stdcall`, `fastcall`, `runtime`, `iterable`, `iterator`, `value`

### 1.3 LE0005 错误

C 版 LE0005（`expected annotation name after '@'`）原由 Lexer 在扫描 `@` 时检查。Feng 版由 Parser 在组合注解时检查。

### 1.4 Trivia 过滤

Lexer 将空白和注释作为独立 Token 发射（`WhitespaceSpace`、`WhitespaceNewline`、`CommentLine`、`CommentBlock`、`CommentDoc`）。Parser 在构建 AST 时跳过 trivia Token。

文档注释关联由 Lexer 处理（`CommentDoc` Token 的 `leadingDoc` 字段），Parser 可直接使用。

---

## 2 模块结构

```
std/src/compiler/
└── Parser/
    ├── AstNode.ff           # AST 节点类型定义
    ├── FengParser.ff        # Parser 实现（递归下降）
    └── AstDumper.ff         # AST 调试输出
```

---

## 3 AST 节点类型

**文件**：`std/src/compiler/Parser/AstNode.ff`
**模块**：`std.compiler`

### 3.1 整体设计思路

C 版 AST 用 `struct + enum + union` 模式（如 `FengExpr` 有 `FengExprKind` + 大 union）。Feng 版有两个设计选项：

**选项 A：spec + 多个 type（面向对象风格）**

```feng
spec Expr {
  let location: SourceLocation;
}

type IdentifierExpr: Expr { ... }
type IntLiteralExpr: Expr { ... }
type BinaryExpr: Expr { ... }
// ... 22 种表达式类型
```

- 优点：类型安全、每种节点独立定义、可扩展
- 缺点：节点种类多时类型数量爆炸（22 种 Expr + 14 种 Stmt + 6 种 Decl = 42+ 种类型）；需要引用语义（堆分配）

**选项 B：type + enum + union（与 C 版同构）**

```feng
type Expr {
  let kind: ExprKind;
  let location: SourceLocation;
  let value: ExprValue;  // union type
}

spec ExprValue: IdentifierExprData | IntLiteralData | BinaryExprData | ...;
```

- 优点：与 C 版结构对应、验证等价性更直接
- 缺点：union 成员多时（22 种）slot 大小由最大成员决定，可能浪费空间

**建议**：采用**选项 A（spec + 多个 type）**。理由：
- Feng 的 spec/fit 机制天然适合这种多态节点结构
- 每种节点独立定义，可读性和可维护性更好
- 自举时直接面向 Feng 类型系统，不需要再做转换
- 虽然类型数量多，但每种类型的定义都很清晰

### 3.2 基础类型

```feng
open module std.compiler;

/** 可见性 */
open enum Visibility {
  DEFAULT = 0,
  PRIVATE = 1,
  PUBLIC = 2
}

/** 可变性 */
open enum Mutability {
  DEFAULT = 0,
  LET = 1,
  VAR = 2
}

/** 切片（引用源码中的文本段） */
@value
open type Slice {
  open let text: string;
}
```

### 3.3 TypeRef（类型引用）

```feng
/** 类型引用种类 */
open enum TypeRefKind {
  NAMED = 0,
  POINTER = 1,
  ARRAY = 2
}

/**
 * 类型引用——出现在类型标注位置的 AST 节点。
 *
 * 示例：
 * - `int`         → NAMED(segments=["int"])
 * - `Map<K, V>`   → NAMED(segments=["Map"], typeArgs=[NAMED("K"), NAMED("V")])
 * - `string*`     → POINTER(inner=NAMED("string"))
 * - `int[]`       → ARRAY(inner=NAMED("int"))
 * - `int[!]`      → ARRAY(inner=NAMED("int"), elementWritable=true)
 */
open type TypeRef {
  open let location: SourceLocation;
  open let kind: TypeRefKind;
  open let arrayElementWritable: bool;
  // NAMED
  open let segments: Slice[];
  open let typeArgs: TypeRef[];
  // POINTER / ARRAY
  open let inner: TypeRef;
}
```

### 3.4 Expr（表达式）

```feng
/** 表达式种类 */
open enum ExprKind {
  IDENTIFIER = 0,
  SELF = 1,
  BOOL = 2,
  INTEGER = 3,
  FLOAT = 4,
  STRING = 5,
  ARRAY_LITERAL = 6,
  TUPLE_LITERAL = 7,
  OBJECT_LITERAL = 8,
  GENERIC_TARGET = 9,
  CALL = 10,
  MEMBER = 11,
  INDEX = 12,
  UNARY = 13,
  BINARY = 14,
  LAMBDA = 15,
  CAST = 16,
  IF = 17,
  MATCH = 18,
  TRY = 19,
  ARRAY_NEW = 20,
  MATCH_OP = 21
}

/**
 * 表达式 AST 节点。
 *
 * 使用 type + enum + 可选字段的方式表达 22 种表达式。
 * 不同类型的表达式通过 kind 区分，按 kind 读取对应字段。
 *
 * 设计说明：
 * 这里选择"扁平 type"而非"spec + 22 个子 type"，原因：
 * 1. 表达式是 AST 中数量最多的节点类型，扁平结构减少类型实例数
 * 2. 与 C 版结构对应，验证等价性更直接
 * 3. 后续可重构为 spec + 子 type（如果需要更好的类型安全）
 */
open type Expr {
  open let kind: ExprKind;
  open let location: SourceLocation;
  // 语义分析填充的类型（Parser 阶段为空）
  open let inferredType: TypeRef;

  // ---- 按 kind 使用的字段 ----

  // IDENTIFIER: identifierName
  open let identifierName: Slice;

  // BOOL: boolValue
  // INTEGER: intValue
  // FLOAT: floatValue
  // STRING: stringValue
  open let boolValue: bool;
  open let intValue: i64;
  open let floatValue: f64;
  open let stringValue: Slice;

  // ARRAY_LITERAL / TUPLE_LITERAL: items
  open let items: Expr[];

  // OBJECT_LITERAL: objectTarget + objectFields
  open let objectTarget: Expr;
  open let objectFields: ObjectFieldInit[];

  // GENERIC_TARGET: genericTarget + typeArgs
  open let genericTarget: Expr;
  open let typeArgs: TypeRef[];

  // CALL: callee + callArgs + explicitTypeArgs
  open let callee: Expr;
  open let callArgs: Expr[];
  open let explicitTypeArgs: TypeRef[];

  // MEMBER: memberObject + memberName
  open let memberObject: Expr;
  open let memberName: Slice;

  // INDEX: indexObject + indexExpr
  open let indexObject: Expr;
  open let indexExpr: Expr;

  // UNARY: unaryOp + operand
  open let unaryOp: TokenKind;
  open let operand: Expr;

  // BINARY: binaryOp + left + right
  open let binaryOp: TokenKind;
  open let left: Expr;
  open let right: Expr;

  // LAMBDA: params + lambdaBody / lambdaBlockBody + isBlockBody
  open let params: Parameter[];
  open let lambdaBody: Expr;
  open let lambdaBlockBody: Block;
  open let isBlockBody: bool;

  // CAST: castType + castValue
  open let castType: TypeRef;
  open let castValue: Expr;

  // IF: condition + thenBlock + elseBlock
  open let condition: Expr;
  open let thenBlock: Block;
  open let elseBlock: Block;

  // MATCH: matchTarget + branches + matchElse
  open let matchTarget: Expr;
  open let branches: MatchBranch[];
  open let matchElse: Block;

  // MATCH_OP: matchOpTarget + matchOpLabels + binding info
  open let matchOpTarget: Expr;
  open let matchOpLabels: MatchLabel[];
  open let hasBinding: bool;
  open let bindingName: Slice;
  open let bindingMutability: Mutability;

  // TRY: tryBody + catchClauses
  open let tryBody: Expr;
  open let catchClauses: TryCatchClause[];

  // ARRAY_NEW: elementTypeName + arraySize
  open let elementTypeName: TypeRef;
  open let arraySize: Expr;
}
```

**设计说明**：

这里对 Expr 采用了**扁平 type**（而非 spec + 22 个子 type），原因：
- Expr 是 AST 中数量最多的节点类型，一个中等大小的函数可能有几十个 Expr 节点
- 扁平结构可以直接用 `@value` type（如果后续需要），避免 22 种子类型的堆分配
- 与 C 版 union 结构直接对应，验证等价性更直接
- 未使用字段保持默认值（null/0/false），空间上有些浪费但可接受

### 3.5 辅助 AST 类型

```feng
/** 参数定义 */
open type Parameter {
  open let token: Token;
  open let mutability: Mutability;
  open let name: Slice;
  open let typeRef: TypeRef;
  open let isVariadic: bool;
}

/** 泛型类型参数 */
open type TypeParam {
  open let token: Token;
  open let name: Slice;
  open let constraint: TypeRef;
}

/** 注解 */
open type Annotation {
  open let token: Token;
  open let name: Slice;
  open let builtinKind: AnnotationKind;
  open let args: Expr[];
}

/** 对象字段初始化 */
open type ObjectFieldInit {
  open let token: Token;
  open let name: Slice;
  open let value: Expr;
}

/** match 分支标签 */
open enum MatchLabelKind {
  VALUE = 0,
  RANGE = 1,
  TYPE = 2
}

open type MatchLabel {
  open let token: Token;
  open let kind: MatchLabelKind;
  open let value: Expr;
  open let rangeLow: Expr;
  open let rangeHigh: Expr;
  open let typeRef: TypeRef;
}

/** match 分支 */
open type MatchBranch {
  open let token: Token;
  open let labels: MatchLabel[];
  open let body: Block;
  open let hasBinding: bool;
  open let bindingName: Slice;
  open let bindingMutability: Mutability;
}

/** try/catch 子句 */
open type TryCatchClause {
  open let token: Token;
  open let name: Slice;
  open let typeRef: TypeRef;
  open let body: Block;
}
```

### 3.6 Stmt（语句）

```feng
/** 语句种类 */
open enum StmtKind {
  BLOCK = 0,
  BINDING = 1,
  ASSIGN = 2,
  EXPR = 3,
  TRY = 4,
  IF = 5,
  MATCH = 6,
  WHILE = 7,
  FOR = 8,
  RETURN = 9,
  THROW = 10,
  BREAK = 11,
  CONTINUE = 12,
  DEFER = 13
}

/** 代码块 */
open type Block {
  open let token: Token;
  open let statements: Stmt[];
}

/** 语句 */
open type Stmt {
  open let kind: StmtKind;
  open let location: SourceLocation;

  // BLOCK
  open let block: Block;

  // BINDING
  open let binding: Binding;

  // ASSIGN: assignOp + assignTarget + assignValue
  open let assignOp: TokenKind;
  open let assignTarget: Expr;
  open let assignValue: Expr;

  // EXPR
  open let expr: Expr;

  // IF: ifClauses + ifElse
  open let ifClauses: IfClause[];
  open let ifElse: Block;

  // MATCH
  open let matchTarget: Expr;
  open let matchBranches: MatchBranch[];
  open let matchElse: Block;

  // WHILE
  open let whileCondition: Expr;
  open let whileBody: Block;

  // FOR（三段式和 for/in 共用）
  open let isForIn: bool;
  open let forBody: Block;
  open let forInit: Stmt;
  open let forCondition: Expr;
  open let forUpdate: Stmt;
  open let forIterBinding: Binding;
  open let forIterExpr: Expr;

  // RETURN / THROW
  open let returnOrThrowValue: Expr;

  // DEFER
  open let deferBlock: Block;
}

/** 绑定声明（let/var） */
open type Binding {
  open let token: Token;
  open let mutability: Mutability;
  open let name: Slice;
  open let typeRef: TypeRef;
  open let initializer: Expr;
  open let isDestructure: bool;
  open let destructureNames: Slice[];
}

/** if 分支 */
open type IfClause {
  open let token: Token;
  open let condition: Expr;
  open let block: Block;
}
```

### 3.7 TypeMember（类型成员）

```feng
/** 类型成员种类 */
open enum TypeMemberKind {
  FIELD = 0,
  METHOD = 1,
  CONSTRUCTOR = 2,
  FINALIZER = 3
}

/** 可调用签名（函数/方法/构造器共用） */
open type CallableSignature {
  open let token: Token;
  open let name: Slice;
  open let typeParams: TypeParam[];
  open let params: Parameter[];
  open let returnType: TypeRef;
  open let body: Block;
}

/** 类型成员 */
open type TypeMember {
  open let token: Token;
  open let kind: TypeMemberKind;
  open let visibility: Visibility;
  open let isStatic: bool;
  open let docComment: Slice;
  open let annotations: Annotation[];

  // FIELD
  open let fieldMutability: Mutability;
  open let fieldName: Slice;
  open let fieldType: TypeRef;
  open let fieldInitializer: Expr;

  // METHOD / CONSTRUCTOR / FINALIZER
  open let callable: CallableSignature;
}
```

### 3.8 Decl（声明）

```feng
/** 声明种类 */
open enum DeclKind {
  GLOBAL_BINDING = 0,
  TYPE = 1,
  ENUM = 2,
  SPEC = 3,
  FIT = 4,
  FUNCTION = 5
}

/** spec 形式 */
open enum SpecForm {
  OBJECT = 0,
  CALLABLE = 1,
  UNION = 2
}

/** 枚举项 */
open type EnumItem {
  open let token: Token;
  open let name: Slice;
  open let hasExplicitValue: bool;
  open let explicitValue: i64;
}

/** import 声明 */
open type UseDecl {
  open let token: Token;
  open let segments: Slice[];
  open let alias: Slice;
  open let hasAlias: bool;
}

/** 顶层声明 */
open type Decl {
  open let token: Token;
  open let kind: DeclKind;
  open let visibility: Visibility;
  open let isExtern: bool;
  open let docComment: Slice;
  open let annotations: Annotation[];

  // GLOBAL_BINDING
  open let binding: Binding;

  // TYPE
  open let typeName: Slice;
  open let typeParams: TypeParam[];
  open let typeMembers: TypeMember[];
  open let declaredSpecs: TypeRef[];
  open let isTuple: bool;
  open let isValue: bool;

  // ENUM
  open let enumName: Slice;
  open let enumItems: EnumItem[];

  // SPEC
  open let specName: Slice;
  open let specTypeParams: TypeParam[];
  open let specForm: SpecForm;
  open let parentSpecs: TypeRef[];
  // object-form
  open let specMembers: TypeMember[];
  // callable-form
  open let specCallableParams: Parameter[];
  open let specCallableReturn: TypeRef;
  // union-form
  open let specUnionMembers: TypeRef[];

  // FIT
  open let fitTarget: TypeRef;
  open let fitSpecs: TypeRef[];
  open let fitMembers: TypeMember[];
  open let fitHasBody: bool;

  // FUNCTION
  open let functionSig: CallableSignature;
}
```

### 3.9 Program（程序）

```feng
/** 解析后的完整程序 */
open type Program {
  open let path: string;
  open let moduleToken: Token;
  open let moduleVisibility: Visibility;
  open let moduleSegments: Slice[];
  open let uses: UseDecl[];
  open let declarations: Decl[];
}
```

---

## 4 Parser 实现

**文件**：`std/src/compiler/Parser/FengParser.ff`
**模块**：`std.compiler`

### 4.1 设计

```feng
/**
 * Feng 语法分析器。
 *
 * 递归下降式解析器，与 C 版 parser.c 行为等价。
 * 输入 Token 流（来自 FengLexer 或 TokenStream），输出 AST（Program）。
 *
 * 用法示例：
 *   let lexer = FengLexer(source, "example.ff");
 *   let parser = FengParser(lexer);
 *   let program = parser.parse();
 */
open type FengParser {
  seal let lexer: FengLexer;
  seal var current: Token;
  seal var errors: ParseError[];

  /** 从 Lexer 创建 Parser */
  open func FengParser(lexer: FengLexer);

  /** 从 TokenStream 创建 Parser */
  open func FengParser(stream: TokenStream);

  /**
   * 解析完整源文件。
   * 返回 Program AST；如果有语法错误，通过 errors() 获取错误列表。
   */
  open func parse(): Program;

  /** 获取解析过程中收集的错误 */
  open func errors(): ParseError[];
}

/** 解析错误 */
open type ParseError {
  open let token: Token;
  open let code: string;
  open let message: string;
}
```

### 4.2 递归下降结构

Parser 的核心是递归下降，按优先级从低到高组织表达式解析：

```
parse()
  → parseModuleDeclaration()          // module ...;
  → parseUseDeclarations()            // import ...; (多条)
  → parseTopLevelDeclarations()       // 顶层声明序列
    → parseAnnotations()              // @annotation 序列
    → parseDeclaration()
      → parseTypeDecl()               // type Name { ... }
      → parseEnumDecl()               // enum Name { ... }
      → parseSpecDecl()               // spec Name { ... } / spec Name(): ...; / spec Name: ... | ...;
      → parseFitDecl()                // fit Type: Spec { ... }
      → parseFunctionDecl()           // func name(...) { ... }
      → parseGlobalBindingDecl()      // let/var name: Type = expr;

parseExpression()                     // 表达式入口
  → parseAssignment()                 // = += -= *= /= %= &= |= ^= <<= >>=
    → parseOr()                       // ||
      → parseAnd()                    // &&
        → parseBitOr()                // |
          → parseBitXor()             // ^
            → parseBitAnd()           // &
              → parseEquality()       // == !=
                → parseComparison()   // < <= > >=
                  → parseShift()      // << >>
                    → parseAdditive() // + -
                      → parseMultiplicative() // * / %
                        → parseUnary()       // ! - ~
                          → parseCast()      // (Type) expr
                            → parsePostfix() // call, member, index
                              → parsePrimary() // 字面量, 标识符, self, 分组, if, match, try, lambda, array new
```

### 4.3 关键解析策略

#### 表达式优先级

与 C 版 parser.c 一致的优先级层次（从低到高）：
1. 赋值（`=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`）— 右结合
2. 逻辑或 `||`
3. 逻辑与 `&&`
4. 位或 `|`
5. 位异或 `^`
6. 位与 `&`
7. 相等 `==` `!=`
8. 比较 `<` `<=` `>` `>=`
9. 移位 `<<` `>>`
10. 加法 `+` `-`
11. 乘法 `*` `/` `%`
12. 一元 `!` `-` `~`
13. 类型转换 `(Type) expr`
14. 后缀：调用 `()`、成员 `.`、下标 `[]`
15. 基本：字面量、标识符、`self`、分组 `()`、`if` 表达式、`match` 表达式、`try` 表达式、lambda、`T[:n]`

#### 泛型参数歧义

`<` 既可能是比较运算符，也可能是泛型参数开始。解析策略：
- 在 postfix 表达式中遇到 `<` 时，尝试解析为泛型参数
- 如果解析成功（匹配到 `>`），作为 `GENERIC_TARGET` 节点
- 如果解析失败，回溯为比较运算符
- 需要 C 版 parser 中的 `parse_type_args` 逻辑（包括 `>>` 拆分为两个 `>` 的处理）

#### Lambda 解析

Lambda 语法：`(params) => expr` 或 `(params) => { block }`
- 遇到 `(` 时需要前瞻判断是分组表达式还是 Lambda
- 判断依据：参数列表的形式（`name: Type` 模式）

#### match 运算符

`expr match { branches }` 和 `expr match pattern => expr` 两种形式
- 前者是 match 表达式（match_expr）
- 后者是 match 运算符（match_op）

### 4.4 错误恢复

- 解析错误产出 `ParseError` 并记录到错误列表
- 错误码体系与 C 版一致（`SE*` 前缀）
- 错误后尝试跳过到下一个同步点（`;`、`}`、声明关键字）继续解析
- 最终返回部分 AST + 错误列表

### 4.5 与 C 版对应关系

| C 版函数 | Feng 版对应 | 说明 |
|----------|------------|------|
| `feng_parse_source` | `FengParser.parse()` | 入口 |
| `parse_expression` | `parseExpression()` | 表达式 |
| `parse_assignment` | `parseAssignment()` | 赋值 |
| `parse_or` ... `parse_primary` | 对应优先级层方法 | 优先级链 |
| `parse_type_ref` | `parseTypeRef()` | 类型引用 |
| `parse_annotations` | `parseAnnotations()` | 注解序列 |
| `parse_declaration` | `parseDeclaration()` | 声明分发 |
| `parse_block` | `parseBlock()` | 代码块 |
| `parse_statement` | `parseStatement()` | 语句分发 |

---

## 5 AstDumper

**文件**：`std/src/compiler/Parser/AstDumper.ff`
**模块**：`std.compiler`

### 5.1 设计

```feng
/**
 * AST 调试输出器。
 *
 * 将 AST 以缩进文本形式输出，格式与 C 版 dump.c 对齐。
 * 用于测试验证和调试。
 */
open type AstDumper {
  /** 将 Program AST 输出为文本 */
  open static func dump(program: Program): string;

  /** 将单个 Expr AST 输出为文本 */
  open static func dumpExpr(expr: Expr): string;
}
```

### 5.2 输出格式

与 C 版 `dump.c` 对齐，使用缩进表示层级：

```
Program: example.ff
  Module: std.example (open)
  Import: std
  Import: std.text
  TypeDecl: User (open)
    Field: name (string) (let)
    Method: display() -> string
      Return
        Member: self.name
```

---

## 6 实施顺序

| 步骤 | 内容 | 前置 | 产出文件 |
|------|------|------|---------|
| 6 | AST 节点类型定义 | 阶段一完成 | `std/src/compiler/Parser/AstNode.ff` |
| 7 | Parser 核心框架 | 6 | `std/src/compiler/Parser/FengParser.ff` |
| 8 | 表达式解析（优先级链） | 7 | 同上 |
| 9 | 语句解析 | 7 | 同上 |
| 10 | 声明解析（type/enum/spec/fit/func） | 7-9 | 同上 |
| 11 | AstDumper | 6 | `std/src/compiler/Parser/AstDumper.ff` |
| 12 | Parser 测试 | 7-11 | fcts 测试 |

---

## 7 验证方案

### 7.1 AST dump 对比

- 对 C 版 parser 测试用例（`test/parser/test_parser.c`）中的每个测试输入
- 分别用 C 版 parser + dump.c 和 Feng 版 parser + AstDumper 产出 AST dump
- diff 比较两者输出是否一致

### 7.2 错误码对比

- 同样的语法错误输入，比较 C 版和 Feng 版产出的 ParseError（错误码 + 错误信息 + 位置）

### 7.3 完整源文件解析

- 用 std 库中的 `.ff` 文件作为输入（如 `std/src/text/String.ff`）
- 比较 C 版和 Feng 版的解析结果

---

## 8 开放问题

1. **Expr 的扁平 type vs spec + 子 type**：当前方案用扁平 type，但如果后续需要更好的类型安全性，可能需要重构为 spec + 子 type。这个决策可以在实现过程中根据实际体验调整
2. **Parser 的错误恢复策略**：C 版 parser 的错误恢复策略比较复杂，Feng 版是否需要完全复制？还是可以先实现简单版本（遇错即停），后续再增强？
3. **泛型参数歧义的回溯机制**：`<` 的歧义解析需要回溯能力（保存/恢复解析器状态）。Feng 的 Token 前瞻机制是否足够支持？
4. **Parser 是否需要支持增量解析**：远期如果用于 IDE/LSP 场景，可能需要增量解析能力。当前方案不支持增量解析
5. **AST 节点是否用 @value type**：Expr 用普通 type（引用语义），因为 AST 节点形成树形结构，引用更自然；但大量小节点（如 Identifier、IntLiteral）是否有 GC 压力？
6. **Token/FengToken 类型名对齐**：AST 节点中引用的 `Token` 和 `TokenKind` 类型需更新为 Lexer 草案中的 `FengToken` 和 `FengTokenKind`（含新的区间分段命名：`KeywordType`、`OperatorPlus` 等）
7. **AnnotationKind 定义位置**：Lexer 草案已移除 `AnnotationKind`（注解分类是 Parser 职责）。Parser AST 的 `Annotation` 节点引用了 `AnnotationKind`，需在此草案中定义
