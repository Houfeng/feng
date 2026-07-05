# Feng 标准库 Parser 设计草案

> 本文记录基于 Feng 实现的语法分析器（Parser）设计方向，服务于未来自举。
> **状态**：草案阶段，尚未实现。依赖阶段一（Lexer）完成后启动。
> **目标**：在 `std/src/compiler/Parser/` 中实现完整的 Feng 语法分析器，作为自举编译器的前端。
> **前置**：`feng-std-lexer-draft.md`（阶段一：Lexer + Token + TokenTransformer）。

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
└── parser/
    ├── FengAstNodes.ff      # AST 节点类型定义（已实现）
    ├── FengParser.ff        # Parser 实现（递归下降）
    └── FengAstDumper.ff     # AST 调试输出
```

**模块**：`std.compiler.parser`（与 FengAstNodes.ff 一致）

---

## 3 AST 节点类型

**文件**：`std/src/compiler/parser/FengAstNodes.ff`
**模块**：`std.compiler.parser`

### 3.1 整体设计思路

C 版 AST 用 `struct + enum + union` 模式（如 `FengExpr` 有 `FengExprKind` + 大 union）。Feng 版选择 **spec union + 独立 type** 的结构：

```feng
open spec Expression: IdentifierExpr
  | BooleanLiteralExpr
  | IntegerLiteralExpr
  // ... 共 22 种
  ;

open type IdentifierExpr {
  let token: FengToken;
  let name: StringSpan;
}
```

**选择依据**：

1. **类型安全**：每种节点独立定义，构造时只能填充该节点的字段，不存在"误读其它 kind 字段"的可能
2. **消除 kind 枚举**：spec union 自带判别（tag），无需手写 ExprKind/StmtKind/DeclKind 枚举
3. **token 就近原则**：每个 variant 自带 `token: FengToken`，消除 C 版中"wrapper token + body token 重复"的问题
4. **语义与语法分离**：Parser 只产出 AST，语义分析阶段的数据（推断类型、resolved callable 等）不进入 AST 节点
5. **自举友好**：直接面向 Feng 类型系统，不需要做 enum+union 的二次转换

**类型数量**：22 种 Expression + 15 种 Statement + 6 种 ModuleMemberBody + 4 种 TypeMemberBody + 3 种 TypeReference + 3 种 SpecBody + 2 种 Binding + 辅助类型 = 约 60+ 个 type/spec 定义。每个定义 5-15 行，可读性和可维护性均可接受。

### 3.2 基础类型

```feng
/**
 * 可见性枚举类型
 * 语义阶段 resolve 默认值：
 * - 模块/顶层声明无 open/seal 关键字 → Seal
 * - 类型成员无关键字 → Open
 */
open enum Visibility {
  Default = 0,
  Seal = 1,
  Open = 2
}

/**
 * 可变性枚举类型
 * 语义阶段 resolve 默认值：
 * - 函数参数可省略，无 let/var 关键字 → Let
 * - 绑定/字段必须显式指定 let/var
 */
open enum Mutability {
  Default = 0,
  Let = 1,
  Var = 2
}
```

**命名说明**：

- 使用 `StringSpan`（来自 `std.text`）引用源码文本段，不引入独立的 Slice 类型
- 使用 `FengToken`（来自 `std.compiler.lexer`）承载词法单元（含 FengLocation）
- 使用 `FengTokenKind` 表示运算符、关键字等词法单元类型

### 3.3 命名与路径

```feng
/**
 * 多段命名标识符，用于类型引用、模块路径等
 *
 * segments 字段：
 * - segments[0] 为最左侧标识符，segments[n] 为最右侧标识符
 * - segments 数组长度至少为 1
 */
open type SegmentedName {
  let token: FengToken;
  let fullName: StringSpan;
  let segments: StringSpan[];
}
```

**用途**：类型引用 `NamedTypeRef`、模块声明 `ModuleFile.moduleName`、import 声明 `ModuleImport.name`、注解 `Annotation.name`。

### 3.4 TypeReference（类型引用）

类型位置仅允许标识符链（如 `std.text.String`），Parser 直接识别为多段 segments 数组。

```feng
/** 命名类型引用：int, std.text.String, Map<K, V> */
open type NamedTypeRef {
  let token: FengToken;
  let name: SegmentedName;
  let parameterTypeRefs: TypeReference[];
}

/** 指针类型引用：int*, string* */
open type PointerTypeRef {
  let token: FengToken;
  let targetTypeRef: TypeReference;
}

/** 数组类型引用：int[], int[!] */
open type ArrayTypeRef {
  let token: FengToken;
  let elementTypeRef: TypeReference;
  let elementWritable: bool;
}

/** 类型引用，支持命名类型、指针类型、数组类型 */
open spec TypeReference: NamedTypeRef
  | PointerTypeRef
  | ArrayTypeRef;
```

**示例**：

- `int` → `NamedTypeRef { name: ["int"], parameterTypeRefs: [] }`
- `Map<K, V>` → `NamedTypeRef { name: ["Map"], parameterTypeRefs: [NamedTypeRef("K"), NamedTypeRef("V")] }`
- `string*` → `PointerTypeRef { targetTypeRef: NamedTypeRef("string") }`
- `int[]` → `ArrayTypeRef { elementTypeRef: NamedTypeRef("int"), elementWritable: false }`
- `int[!]` → `ArrayTypeRef { elementTypeRef: NamedTypeRef("int"), elementWritable: true }`

### 3.5 Binding（绑定）

绑定区分简单绑定和解构绑定两种形式，用 spec union 区分：

```feng
/** 简单绑定：let x: int = 5; */
open type SimpleBinding {
  let token: FengToken;
  let name: StringSpan;
  let mutability: Mutability;
  let typeRef: Option<TypeReference>;
  let initializer: Option<Expression>;
  let annotations: Annotation[];
}

/** 解构绑定：let (a, b) = (1, 2); */
open type DestructureBinding {
  let token: FengToken;
  let mutability: Mutability;
  let names: StringSpan[];
  let typeRef: Option<TypeReference>;
  let initializer: Option<Expression>;
  let annotations: Annotation[];
}

/** 绑定联合类型，用于顶层/成员/局部绑定 */
open spec Binding: SimpleBinding | DestructureBinding;
```

**设计说明**：

- 使用 `Option<T>` 替代 `hasX: bool` + `x: T` 的组合，避免"无值时字段仍有垃圾数据"的问题
- `typeRef` 和 `initializer` 均为可选，符合语法实际（两者至少出现一个，但 Parser 不强制校验）
- SimpleBinding 包含 `annotations` 字段，支持 `@value let x = ...` 等注解形式

### 3.6 Expression（表达式）

共 22 种表达式，每种为独立 type，通过 spec union 聚合：

```feng
open spec Expression: IdentifierExpr
  | BooleanLiteralExpr
  | IntegerLiteralExpr
  | FloatLiteralExpr
  | StringLiteralExpr
  | ArrayLiteralExpr
  | TupleLiteralExpr
  | ObjectLiteralExpr
  | ArrayNewExpr
  | GenericTargetExpr
  | CallExpr
  | MemberAccessExpr
  | IndexAccessExpr
  | UnaryExpr
  | BinaryExpr
  | LambdaExpr
  | CastExpr
  | MatchOperatorExpr
  | IfExpr
  | MatchExpr
  | TryExpr
  | RangeExpr;
```

**各 variant 定义**：

```feng
open type IdentifierExpr {
  let token: FengToken;
  let name: StringSpan;
}

open type BooleanLiteralExpr {
  let token: FengToken;
  let value: bool;
}

open type IntegerLiteralExpr {
  let token: FengToken;
  let value: i64;
  let raw: StringSpan;
}

open type FloatLiteralExpr {
  let token: FengToken;
  let value: f64;
  let raw: StringSpan;
}

open type StringLiteralExpr {
  let token: FengToken;
  let value: StringSpan;
}

open type UnaryExpr {
  let token: FengToken;
  let operator: FengTokenKind;
  let operand: Expression;
}

open type CastExpr {
  let token: FengToken;
  let targetType: TypeReference;
  let value: Expression;
}

open type BinaryExpr {
  let token: FengToken;
  let operator: FengTokenKind;
  let left: Expression;
  let right: Expression;
}

/**
 * 函数/方法调用表达式
 *
 * 泛型具化调用：
 * - 语法：`foo<int, string>(arg1, arg2)`
 * - 表示：callee 为 GenericTargetExpr { target: IdentifierExpr, argumentTypeRefs: [...] }
 *
 * 方法调用：
 * - 语法：`obj.method(arg1)`
 * - 表示：callee 为 MemberAccessExpr
 */
open type CallExpr {
  let token: FengToken;
  let callee: Expression;
  let arguments: Expression[];
}

open type ArrayLiteralExpr {
  let token: FengToken;
  let items: Expression[];
}

open type TupleLiteralExpr {
  let token: FengToken;
  let items: Expression[];
}

open type ObjectLiteralField {
  let token: FengToken;
  let name: StringSpan;
  let value: Expression;
}

open type ObjectLiteralExpr {
  let token: FengToken;
  let target: Expression;
  let fields: ObjectLiteralField[];
}

open type ArrayNewExpr {
  let token: FengToken;
  let elementTypeRef: TypeReference;
  let size: Expression;
}

/**
 * 泛型具化表达式：对目标显式指定类型实参
 *
 * 用途：
 * - 表示 `identifier<TypeArgs>` 语法结构
 * - 可作为 CallExpr 的 callee，或独立存在
 */
open type GenericTargetExpr {
  let token: FengToken;
  let target: Expression;
  let argumentTypeRefs: TypeReference[];
}

/**
 * 成员访问表达式：obj.field, std.foo.test
 *
 * 多段路径解析策略：
 * - 语法：`foo.bar.xyz` 按 `.` 左结合嵌套解析
 * - AST：MemberAccess(MemberAccess(Identifier("foo"), "bar"), "xyz")
 * - 语义阶段逐层消歧：从最左标识符查作用域，按解析结果切换查找策略
 */
open type MemberAccessExpr {
  let token: FengToken;
  let target: Expression;
  let member: StringSpan;
}

open type IndexAccessExpr {
  let token: FengToken;
  let target: Expression;
  let index: Expression;
}

open spec LambdaBody: Expression | Block;

open type LambdaExpr {
  let token: FengToken;
  let parameters: Parameter[];
  let body: LambdaBody;
  let returnTypeRef: Option<TypeReference>;
}

open type RangeExpr {
  let token: FengToken;
  let start: Expression;
  let end: Expression;
}

open spec MatchTarget: IntegerLiteralExpr
  | StringLiteralExpr
  | BooleanLiteralExpr
  | MemberAccessExpr
  | TypeReference
  | SimpleBinding;

/**
 * 匹配运算符表达式：value match pattern1 | pattern2 | ...
 *
 * 解析策略：贪婪消费所有 `|` 分隔的匹配目标
 */
open type MatchOperatorExpr {
  let token: FengToken;
  let value: Expression;
  let targets: MatchTarget[];
}
```

### 3.7 Block（代码块）

```feng
open type Block {
  let token: FengToken;
  let statements: Statement[];
}
```

### 3.8 Statement（语句）

共 15 种语句（含 ForEachStmt），每种为独立 type，通过 spec union 聚合：

```feng
open spec Statement: BlockStmt
  | BindingStmt
  | AssignmentStmt
  | ExpressionStmt
  | TryStmt
  | IfStmt
  | MatchStmt
  | WhileStmt
  | ForStmt
  | ForEachStmt
  | ReturnStmt
  | ThrowStmt
  | BreakStmt
  | ContinueStmt
  | DeferStmt;
```

**各 variant 定义**：

```feng
open type BlockStmt {
  let token: FengToken;
  let block: Block;
}

open type BindingStmt {
  let token: FengToken;
  let binding: Binding;
}

/**
 * 赋值语句，支持简单赋值和复合赋值
 * operator: = / += / -= / *= / /= 等
 */
open type AssignmentStmt {
  let token: FengToken;
  let operator: FengTokenKind;
  let target: Expression;
  let value: Expression;
}

open type ExpressionStmt {
  let token: FengToken;
  let expression: Expression;
}

open type TryStmt {
  let token: FengToken;
  let body: Expression;
  let catchClauses: CatchClause[];
  let catchAllBlock: Option<Block>;
}

open type IfStmt {
  let token: FengToken;
  let clauses: IfClause[];
  let elseBlock: Block;
}

open type MatchStmt {
  let token: FengToken;
  let target: Expression;
  let clauses: MatchClause[];
  let elseBlock: Block;
}

open type WhileStmt {
  let token: FengToken;
  let condition: Expression;
  let body: Block;
}

open spec ForInit: SimpleBinding | AssignmentStmt;
open spec ForUpdate: Expression | AssignmentStmt;

open type ForStmt {
  let token: FengToken;
  let init: ForInit;
  let condition: Expression;
  let update: ForUpdate;
  let body: Block;
}

open type ForEachStmt {
  let token: FengToken;
  let binding: Binding;
  let iterable: Expression;
  let body: Block;
}

open type ReturnStmt {
  let token: FengToken;
  let value: Expression;
}

open type ThrowStmt {
  let token: FengToken;
  let value: Expression;
}

open type BreakStmt {
  let token: FengToken;
}

open type ContinueStmt {
  let token: FengToken;
}

open type DeferStmt {
  let token: FengToken;
  let body: Block;
}
```

**设计说明**：

- ForStmt（三段式 `for init; cond; update`）和 ForEachStmt（`for x in expr`）分离为两种独立 type
- ForInit 支持 SimpleBinding（`for let i = 0`）或 AssignmentStmt（`for i = 0`）
- ForUpdate 支持 Expression（`for ...; ...; i++`）或 AssignmentStmt（`for ...; ...; i += 1`）
- TryStmt 的 `catchAllBlock: Option<Block>` 用于 `catch { ... }` 无类型捕获子句

### 3.9 辅助 AST 类型

```feng
/** 泛型参数节点，类型约束可省略 */
open type GenericParameter {
  let token: FengToken;
  let name: StringSpan;
  let constraintTypeRef: Option<TypeReference>;
}

/** 函数参数节点，引用 SimpleBinding，支持可变参数 */
open type Parameter {
  let binding: SimpleBinding;
  let isVariadic: bool;
}

/**
 * 函数节点，支持泛型参数、参数列表、返回类型、函数体、外部函数标记
 */
open type Function {
  let token: FengToken;
  let name: StringSpan;
  let genericParameters: GenericParameter[];
  let parameters: Parameter[];
  let returnTypeRef: Option<TypeReference>;
  let annotations: Annotation[];
  let body: Block;
}

/**
 * 注解节点：@test, @std.compiler.deprecated, @test(args)
 *
 * name 字段（SegmentedName）：
 * - 简单注解：@test → segments: ["test"]
 * - 限定名注解：@std.compiler.deprecated → segments: ["std", "compiler", "deprecated"]
 *
 * arguments 字段：
 * - 注解参数列表，如 @test("reason", 2024) 中的参数
 * - 无参数时为空数组
 */
open type Annotation {
  let token: FengToken;
  let name: SegmentedName;
  let arguments: Expression[];
}

/** if 分支 */
open type IfClause {
  let token: FengToken;
  let condition: Expression;
  let body: Block;
}

/** Match Targets 的分支辅助结构 */
open type MatchClause {
  let token: FengToken;
  let targets: MatchTarget[];
  let body: Block;
}

/** try/catch 子句 */
open type CatchClause {
  let token: FengToken;
  let binding: SimpleBinding;
  let body: Block;
}
```

**设计说明**：

- `CatchClause` 使用 `SimpleBinding` 替代"name + type"组合，统一绑定语义
- `Parameter` 复用 `SimpleBinding`（含 mutability、typeRef、initializer），减少重复定义
- `Function` 独立为 type，被 Function 声明、TypeMethod、TypeConstructor、TypeFinalizer 复用

### 3.10 Type（类型声明）

```feng
open type TypeField {
  let token: FengToken;
  let binding: SimpleBinding;
}

open type TypeMethod {
  let token: FengToken;
  let function: Function;
}

open type TypeConstructor {
  let token: FengToken;
  let function: Function;
}

open type TypeFinalizer {
  let token: FengToken;
  let function: Function;
}

open spec TypeMemberBody: TypeField
  | TypeMethod
  | TypeConstructor
  | TypeFinalizer;

/**
 * 结构对应语法的层次关系：
 * [open|seal] [static] <declaration>
 * ─────────  ──────── ────────────
 * visibility isStatic   body
 */
open type TypeMember {
  let token: FengToken;
  let visibility: Visibility;
  let isStatic: bool;
  let comment: StringSpan;
  let body: TypeMemberBody;
}

open type Type {
  let token: FengToken;
  let name: StringSpan;
  let genericParameters: GenericParameter[];
  let members: TypeMember[];
  let specs: TypeReference[];
  let annotations: Annotation[];
  let isTuple: bool;
  let isValue: bool;
}
```

**设计说明**：

- `TypeMember` 是 wrapper，承载 visibility、isStatic、doc comment 等修饰符
- `TypeMemberBody` 是 spec union，区分 Field/Method/Constructor/Finalizer
- `TypeField` 复用 `SimpleBinding`，字段声明即绑定
- `TypeMethod`/`TypeConstructor`/`TypeFinalizer` 均复用 `Function`，仅语义不同

### 3.11 Enum（枚举声明）

```feng
open type EnumItem {
  let token: FengToken;
  let name: StringSpan;
  let value: Option<int>;
  let annotations: Annotation[];
}

open type Enum {
  let token: FengToken;
  let name: StringSpan;
  let annotations: Annotation[];
  let items: EnumItem[];
}
```

**设计说明**：

- 使用 `Option<int>` 替代 `hasExplicitValue: bool` + `explicitValue: i64`，避免"无显式值时 explicitValue 含垃圾数据"的问题
- 枚举项支持注解（如 `@deprecated`）

### 3.12 Spec（契约声明）

```feng
/**
 * 对象契约：定义类型需实现的成员集合
 * spec Named { let name: string; func greet(): string; }
 */
open type ObjectSpec {
  let token: FengToken;
  let genericParameters: GenericParameter[];
  let members: TypeMember[];
  let parentTypeRefs: TypeReference[];
}

/**
 * 可调用契约：定义函数签名
 * spec Predicate<T>: (T) -> bool;
 */
open type CallableSpec {
  let token: FengToken;
  let genericParameters: GenericParameter[];
  let parameters: Parameter[];
  let returnTypeRef: TypeReference;
}

/**
 * 联合契约：定义可选类型集合
 * spec Option<T>: Some<T> | None;
 */
open type UnionSpec {
  let token: FengToken;
  let memberTypeRefs: TypeReference[];
}

open spec SpecBody: ObjectSpec
  | CallableSpec
  | UnionSpec;

/** 契约声明节点 */
open type Spec {
  let token: FengToken;
  let name: StringSpan;
  let annotations: Annotation[];
  let body: SpecBody;
}
```

### 3.13 Fit（适配器声明）

```feng
/**
 * 适配器声明：fit TargetType: Spec1, Spec2 { ... }
 *
 * members 字段：
 * - Some(members): 有实现体，包含适配方法/字段
 * - None: 无实现体（仅声明适配关系）
 */
open type Fit {
  let token: FengToken;
  let targetTypeRef: TypeReference;
  let specTypeRefs: TypeReference[];
  let members: Option<TypeMember[]>;
}
```

**设计说明**：

- 使用 `Option<TypeMember[]>` 替代 `hasBody: bool` + `members: TypeMember[]`

### 3.14 ModuleFile（模块文件，Parser 输出）

Parser 的输出是 `ModuleFile`（对应单个 `.ff` 文件），而非 `Module`（对应跨多文件的语义模块）。

```feng
open type ModuleBinding {
  let token: FengToken;
  let binding: SimpleBinding;
}

open type ModuleFunction {
  let token: FengToken;
  let isExtern: bool;
  let function: Function;
}

open spec ModuleMemberBody: ModuleBinding
  | ModuleFunction
  | Type
  | Enum
  | Spec
  | Fit;

open type ModuleMember {
  let token: FengToken;
  let visibility: Visibility;
  let body: ModuleMemberBody;
}

/**
 * 模块导入声明：import std.text; import std.text as txt;
 *
 * alias 字段：
 * - Some(alias): 有别名，如 import std.text as txt
 * - None: 无别名，如 import std.text
 */
open type ModuleImport {
  let token: FengToken;
  let name: SegmentedName;
  let alias: Option<StringSpan>;
}

open type ModuleFile {
  let token: FengToken;
  /** 当前文件的路径 */
  let path: string;
  /** import 作用于文件，而非整个模块 */
  let imports: ModuleImport[];
  let moduleVisibility: Visibility;
  let moduleName: SegmentedName;
  let moduleMembers: ModuleMember[];
}
```

**设计说明**：

- `ModuleFile` 是 Parser 的直接输出，对应单个 `.ff` 文件
- `ModuleMember` 是 wrapper，承载 visibility 修饰符
- `ModuleMemberBody` 包含 6 种成员：ModuleBinding、ModuleFunction、Type、Enum、Spec、Fit
- `ModuleFunction` 单独包装，增加 `isExtern` 标记（`extern func`）
- `ModuleImport` 使用 `SegmentedName` 表示模块路径（如 `std.text`）

### 3.15 Module / Program（语义阶段）

语义分析阶段将多个 `ModuleFile` 合并为 `Module`，所有模块聚合为 `Program`：

```feng
open type Module {
  /** 模块可见性 */
  let visibility: Visibility;
  /** 模块名称 */
  let name: SegmentedName;

  /**
   * 模块可分布在多个文件，但 import 作用于文件，而非整个模块
   * 语义阶段，根据 path 合并为同一个模块，同时需检查可见性声明一致
   */
  let files: ModuleFile[];
}

open type Program {
  let modules: Module[];
}
```

**与 C 版对比**：

- C 版 `FengProgram` 实际是单文件（对应 Feng 版 `ModuleFile`），命名不准确
- Feng 版明确区分：`ModuleFile`（语法层，单文件）→ `Module`（语义层，跨文件）→ `Program`（所有模块）
- 主流语言惯例：`Program` 通常对应"整个编译单元/项目"，而非单文件

---

## 4 Parser 实现

**文件**：`std/src/compiler/parser/FengParser.ff`
**模块**：`std.compiler.parser`

### 4.1 设计

```feng
/**
 * Feng 语法分析器。
 *
 * 递归下降式解析器，与 C 版 parser.c 行为等价。
 * 输入 Token 流（来自 FengLexer），输出 ModuleFile AST。
 *
 * 衔接方式：当前阶段 Parser 直接接受 FengLexer（流式消费 + 内部前瞻缓冲，
 * 对齐主流编译器设计）。未来若引入只读 TokenStream 概念（封装
 * next/peek/lookback 等流式读取，不提供 replace 等变换方法），再考虑
 * 增加 stream 入参。是否引入 TokenStream 到时根据实际需求决定，当前不预设。
 *
 * 用法示例：
 *   let lexer = FengLexer(source, "example.ff");
 *   let parser = FengParser(lexer);
 *   let moduleFile = parser.parse();
 */
open type FengParser {
  seal let lexer: FengLexer;
  seal var current: FengToken;
  seal var errors: ParseError[];

  /** 从 Lexer 创建 Parser */
  open func FengParser(lexer: FengLexer);

  /**
   * 解析完整源文件。
   * 返回 ModuleFile AST；如果有语法错误，通过 errors() 获取错误列表。
   */
  open func parse(): ModuleFile;

  /** 获取解析过程中收集的错误 */
  open func errors(): ParseError[];
}

/** 解析错误 */
open type ParseError {
  open let token: FengToken;
  open let code: string;
  open let message: string;
}
```

### 4.2 递归下降结构

Parser 的核心是递归下降，按优先级从低到高组织表达式解析：

```
parse()
  → parseModuleDeclaration()          // module ...;
  → parseModuleImports()              // import ...; (多条)
  → parseModuleMembers()              // 模块成员序列
    → parseAnnotations()              // @annotation 序列
    → parseModuleMember()
      → parseTypeDecl()               // type Name { ... }
      → parseEnumDecl()               // enum Name { ... }
      → parseSpecDecl()               // spec Name { ... } / spec Name(): ...; / spec Name: ... | ...;
      → parseFitDecl()                // fit Type: Spec { ... }
      → parseFunctionDecl()           // func name(...) { ... }
      → parseModuleBindingDecl()      // let/var name: Type = expr;

parseExpression()                     // 表达式入口（返回 Expression spec）
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
| `feng_parse_source` | `FengParser.parse()` | 入口（返回 ModuleFile） |
| `parse_expression` | `parseExpression()` | 表达式（返回 Expression spec） |
| `parse_assignment` | `parseAssignment()` | 赋值 |
| `parse_or` ... `parse_primary` | 对应优先级层方法 | 优先级链 |
| `parse_type_ref` | `parseTypeReference()` | 类型引用（返回 TypeReference spec） |
| `parse_annotations` | `parseAnnotations()` | 注解序列 |
| `parse_declaration` | `parseDeclaration()` | 声明分发（返回 ModuleMember） |
| `parse_block` | `parseBlock()` | 代码块 |
| `parse_statement` | `parseStatement()` | 语句分发（返回 Statement spec） |

---

## 5 AstDumper

**文件**：`std/src/compiler/parser/FengAstDumper.ff`
**模块**：`std.compiler.parser`

### 5.1 设计

```feng
/**
 * AST 调试输出器。
 *
 * 将 AST 以缩进文本形式输出，格式与 C 版 dump.c 对齐。
 * 用于测试验证和调试。
 */
open type FengAstDumper {
  /** 将 ModuleFile AST 输出为文本 */
  open static func dump(moduleFile: ModuleFile): string;

  /** 将单个 Expression AST 输出为文本 */
  open static func dumpExpr(expr: Expression): string;
}
```

### 5.2 输出格式

与 C 版 `dump.c` 对齐，使用缩进表示层级（以单文件 ModuleFile 为单位）：

```
ModuleFile: example.ff
  Module: std.example (open)
  Import: std
  Import: std.text
  TypeDecl: User (open)
    Field: name (string) (let)
    Method: display() -> string
      Return
        MemberAccess: self.name
```

---

## 6 实施顺序

| 步骤 | 内容 | 前置 | 产出文件 |
|------|------|------|---------|
| 6 | AST 节点类型定义 | 阶段一完成 | `std/src/compiler/parser/FengAstNodes.ff`（已完成） |
| 7 | Parser 核心框架 | 6 | `std/src/compiler/parser/FengParser.ff` |
| 8 | 表达式解析（优先级链） | 7 | 同上 |
| 9 | 语句解析 | 7 | 同上 |
| 10 | 声明解析（type/enum/spec/fit/func） | 7-9 | 同上 |
| 11 | FengAstDumper | 6 | `std/src/compiler/parser/FengAstDumper.ff` |
| 12 | Parser 测试 | 7-11 | fcts 测试 |

---

## 7 验证方案

### 7.1 AST dump 对比

- 对 C 版 parser 测试用例（`test/parser/test_parser.c`）中的每个测试输入
- 分别用 C 版 parser + dump.c 和 Feng 版 parser + FengAstDumper 产出 AST dump
- diff 比较两者输出是否一致

### 7.2 错误码对比

- 同样的语法错误输入，比较 C 版和 Feng 版产出的 ParseError（错误码 + 错误信息 + 位置）

### 7.3 完整源文件解析

- 用 std 库中的 `.ff` 文件作为输入（如 `std/src/text/String.ff`）
- 比较 C 版和 Feng 版的解析结果（ModuleFile 结构）

---

## 8 开放问题

1. **Parser 的错误恢复策略**：C 版 parser 的错误恢复策略比较复杂，Feng 版是否需要完全复制？还是可以先实现简单版本（遇错即停），后续再增强？
2. **泛型参数歧义的回溯机制**：`<` 的歧义解析需要回溯能力（保存/恢复解析器状态）。Feng 的 Token 前瞻机制是否足够支持？
3. **Parser 是否需要支持增量解析**：远期如果用于 IDE/LSP 场景，可能需要增量解析能力。当前方案不支持增量解析
4. **AST 节点是否用 @value type**：Expression 用普通 type（引用语义），因为 AST 节点形成树形结构，引用更自然；但大量小节点（如 IdentifierExpr、IntegerLiteralExpr）是否有 GC 压力？
5. **FengToken / FengTokenKind 类型对齐**：AST 节点已统一使用 `FengToken` / `FengTokenKind`（来自 `std.compiler.lexer`），与 Lexer 草案保持一致
6. **内建注解识别**：Lexer 草案已移除 `AnnotationKind`（注解分类是 Parser 职责）。Feng 版 `Annotation` 节点不含 `builtinKind` 字段，注解分类在语义阶段通过 `name.segments` 查找完成
7. **语义阶段产出 Program**：Parser 输出 `ModuleFile`（单文件），语义阶段需将多个 `ModuleFile` 合并为 `Module`（按模块名分组），并聚合为 `Program`。此合并逻辑的设计待细化
