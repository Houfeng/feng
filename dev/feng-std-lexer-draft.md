# Feng 标准库 Lexer 设计草案

> 本文记录基于 Feng 实现的词法分析器（Lexer）设计，服务于自定义注解 Token 流变换和未来自举。
> **状态**：草案阶段，尚未实现。
> **目标**：在 `std/src/compiler/Lexer/` 中实现完整的 Feng 词法分析器，作为自定义注解 pre-Parse 阶段的基础设施。

---

## 0 背景与定位

### 为什么需要 Feng 版 Lexer

1. **自定义注解近需**：`feng-custom-annotation-draft-2.md` 确定了 pre-Parse 阶段做 Token 流变换的路线，注解 handler 需要操作 Token 流，要求 Feng 层有 Lexer（产生 Token）和 Token 解析能力
2. **自举远期目标**：Feng 计划自举（~2 年后），Lexer 是编译器前端核心组件
3. **std/src/compiler/ 已预留目录**：`Lexer/`、`Parser/` 目录已创建，`FengToken.ff` 已占位

### 当前 C 版状态

- `src/lexer/lexer.c` (867 行) + `src/lexer/token.c` + `src/lexer/token.h`
- 完整实现了 Feng 语言的词法分析
- Token 定义：30 关键字、5 保留字、8 内建注解（含 `@value`）、43 种运算符/分隔符

### 与 C 版的关系

Feng 版 Token 类型与 C 版**语义等价但结构不同**——按 Feng 风格设计，不机械翻译 C union。C 版编译器继续作为当前主力编译器使用；Feng 版首先服务于自定义注解，远期服务于自举。

---

## 1 设计决策

| 决策项 | 结论 | 理由 |
|--------|------|------|
| 交付范围 | Lexer + Parser 整体规划，分两阶段；本文覆盖阶段一 | Lexer 先交付解锁注解 |
| Token 与 C 版关系 | 语义等价，结构按 Feng 风格 | 不机械翻译 C union |
| Token 值存储 | union type（`i64 \| f64 \| bool \| NoValue`） | tag + slot 性能可忽略，类型安全 |
| Token 类型形式 | `@value` type | 零 heap/RC 压力；~72 bytes memcpy 远快于 malloc + RC |
| StringBuilder | 同步实现 | Lexer 字符串扫描依赖 |
| 序列化预留 | 先实现 Token 后再考虑 | |

### Token @value type 性能分析

- Token 共 8 字段：`kind`(enum) + `lexeme`(string ptr) + `location`(3 ints) + `value`(tag+slot) + `annotationKind`(enum) + `leadingDoc`(string ptr) + `errorMessage`(string ptr) + `errorCode`(string ptr)
- 其中 string 是指针，不深拷贝
- @value 每次传递约 ~9 word（~72 bytes on 64-bit）memcpy，L1 cache 内亚 ns 级
- 对比普通 type：每次创建需 1 次 heap 分配 + 多次 RC 操作（创建、存入数组、传递、释放），ns 级别 × 数万次
- 结论：@value 明显更优

---

## 2 模块结构

```
std/src/text/
└── StringBuilder.ff         # 字符串构建器（Lexer 前置依赖）

std/src/compiler/
└── Lexer/
    ├── FengToken.ff         # Token 类型定义
    ├── FengLexer.ff         # Lexer 实现
    └── TokenStream.ff       # Token 流封装（服务注解变换）
```

---

## 3 StringBuilder

**文件**：`std/src/text/StringBuilder.ff`
**模块**：`std.text`

### 职责

高效的字符串构建，支持逐字节/逐段追加。Lexer 的字符串扫描和数字扫描依赖此组件。

### 设计

```feng
open module std.text;

/**
 * 高效的字符串构建器。
 * 底层用可变 byte[] 缓冲，支持逐字节和逐段追加，最终产出 string。
 */
open type StringBuilder {
  seal var buffer: byte[!];
  seal var count: int;

  /** 创建空构建器，初始容量 64 */
  open func StringBuilder();

  /** 创建指定初始容量的构建器 */
  open func StringBuilder(capacity: int);

  /** 追加单个字节 */
  open func append(ch: byte);

  /** 追加字符串 */
  open func append(text: string);

  /** 追加字节数组的前 length 个字节 */
  open func append(bytes: byte[], length: int);

  /** 产出最终字符串（拷贝 buffer 的前 count 字节） */
  open func toString(): string;

  /** 清空内容，保留缓冲区可重用 */
  open func clear();

  /** 当前已追加的字节数 */
  open func length(): int;
}
```

### 实现要点

- 底层 `byte[!]`（可写数组）+ `count` + 自动扩容
- 扩容策略：容量不足时倍增（`newCapacity = oldCapacity * 2`），初始容量 64
- `toString()` 拷贝 `buffer[0..count]` 产出新 string
- `clear()` 仅重置 `count = 0`，不释放缓冲区

---

## 4 Token 类型定义

**文件**：`std/src/compiler/Lexer/FengToken.ff`
**模块**：`std.compiler`

### 4.1 TokenKind 枚举

语义与 C 版 `FengTokenKind` 完全对齐。

```feng
open module std.compiler;

/**
 * Token 种类枚举。
 * 值与 C 版编译器 FengTokenKind 一一对应。
 */
open enum TokenKind {
  // 特殊
  EOF = 0,
  ERROR = 1,

  // 字面量与标识符
  IDENTIFIER = 2,
  ANNOTATION = 3,
  INTEGER = 4,
  FLOAT = 5,
  STRING = 6,
  BOOL = 7,

  // 关键字（与 C 版 FENG_TOKEN_KW_* 一一对应）
  KW_TYPE = 8,
  KW_ENUM = 9,
  KW_SPEC = 10,
  KW_FIT = 11,
  KW_EXTERN = 12,
  KW_FUNC = 13,
  KW_LET = 14,
  KW_VAR = 15,
  KW_STATIC = 16,
  KW_OPEN = 17,
  KW_SEAL = 18,
  KW_SELF = 19,
  KW_MODULE = 20,
  KW_IMPORT = 21,
  KW_AS = 22,
  KW_IF = 23,
  KW_ELSE = 24,
  KW_MATCH = 25,
  KW_WHILE = 26,
  KW_FOR = 27,
  KW_IN = 28,
  KW_BREAK = 29,
  KW_CONTINUE = 30,
  KW_TRY = 31,
  KW_CATCH = 32,
  KW_UNKNOWN = 33,
  KW_THROW = 34,
  KW_RETURN = 35,
  KW_DEFER = 36,
  KW_VOID = 37,

  // 分隔符与运算符
  LPAREN = 38,
  RPAREN = 39,
  LBRACE = 40,
  RBRACE = 41,
  LBRACKET = 42,
  RBRACKET = 43,
  COMMA = 44,
  COLON = 45,
  SEMICOLON = 46,
  DOT = 47,
  PLUS = 48,
  MINUS = 49,
  STAR = 50,
  SLASH = 51,
  PERCENT = 52,
  PLUS_ASSIGN = 53,
  MINUS_ASSIGN = 54,
  STAR_ASSIGN = 55,
  SLASH_ASSIGN = 56,
  PERCENT_ASSIGN = 57,
  ASSIGN = 58,
  NOT = 59,
  LT = 60,
  LE = 61,
  GT = 62,
  GE = 63,
  EQ = 64,
  NE = 65,
  AND_AND = 66,
  OR_OR = 67,
  AMP = 68,
  AMP_ASSIGN = 69,
  PIPE = 70,
  PIPE_ASSIGN = 71,
  CARET = 72,
  CARET_ASSIGN = 73,
  SHL = 74,
  SHL_ASSIGN = 75,
  SHR = 76,
  SHR_ASSIGN = 77,
  ARROW = 78,
  TILDE = 79,
  ELLIPSIS = 80
}
```

### 4.2 AnnotationKind 枚举

与 C 版 `FengAnnotationKind` 对齐。

```feng
/**
 * 注解种类枚举。
 * 与 C 版 FengAnnotationKind 对齐。
 */
open enum AnnotationKind {
  NONE = 0,
  CUSTOM = 1,
  ABI = 2,
  CDECL = 3,
  STDCALL = 4,
  FASTCALL = 5,
  RUNTIME = 6,
  ITERABLE = 7,
  ITERATOR = 8,
  VALUE = 9
}
```

### 4.3 TokenValue union type

```feng
/**
 * Token 无值标记。
 * 用于关键字、运算符、标识符、字符串、注解等不携带语义值的 Token。
 */
open type NoValue {}

/**
 * Token 值类型——union type（tag + slot 布局）。
 *
 * - i64     → INTEGER Token 的整数值
 * - f64     → FLOAT Token 的浮点值
 * - bool    → BOOL Token 的布尔值
 * - NoValue → 其他所有 Token（不携带语义值）
 */
open spec TokenValue: i64 | f64 | bool | NoValue;
```

### 4.4 SourceLocation

```feng
/**
 * 源码位置信息。
 * 记录 Token 在源文件中的位置，用于错误报告和注解变换。
 */
@value
open type SourceLocation {
  /** 字节偏移量（从 0 开始） */
  open let offset: int;
  /** 行号（从 1 开始） */
  open let line: int;
  /** 列号（从 1 开始） */
  open let column: int;
}
```

### 4.5 Token

```feng
/**
 * Token 实例——词法分析的最小单元。
 *
 * @value type：栈分配、值语义、零 heap/RC 压力。
 * 每次传递约 ~9 word（~72 bytes on 64-bit）memcpy，远低于 malloc + RC 开销。
 *
 * lexeme 存储 Token 的原始文本（string 引用，不拷贝源码）。
 * value 存储解析后的语义值（仅 INTEGER/FLOAT/BOOL 有值，其他为 NoValue）。
 */
@value
open type Token {
  /** Token 种类 */
  open let kind: TokenKind;
  /** 原始文本（引用源码字符串的子串） */
  open let lexeme: string;
  /** 源码位置 */
  open let location: SourceLocation;
  /** 语义值（仅 INTEGER/FLOAT/BOOL 有意义） */
  open let value: TokenValue;
  /** 注解种类（仅 ANNOTATION Token 有意义） */
  open let annotationKind: AnnotationKind;
  /** 文档注释（`/** ... *​/`，无则为空字符串） */
  open let leadingDoc: string;
  /** 错误信息（仅 ERROR Token 有意义） */
  open let errorMessage: string;
  /** 错误码（仅 ERROR Token 有意义，如 "LE0001"） */
  open let errorCode: string;
}
```

### 4.6 辅助函数

```feng
/**
 * 判断 TokenKind 是否为关键字。
 */
open func isKeyword(kind: TokenKind): bool;

/**
 * 获取 TokenKind 的名称字符串（如 "EOF"、"IDENTIFIER"、"KW_TYPE"）。
 */
open func tokenKindName(kind: TokenKind): string;

/**
 * 获取 AnnotationKind 的名称字符串。
 */
open func annotationKindName(kind: AnnotationKind): string;
```

---

## 5 Lexer 实现

**文件**：`std/src/compiler/Lexer/FengLexer.ff`
**模块**：`std.compiler`

### 5.1 设计

```feng
/**
 * Feng 词法分析器。
 *
 * 将源码字符串转换为 Token 流。
 * 逐字节扫描 + 回溯前瞻，与 C 版 lexer（src/lexer/lexer.c）行为等价。
 *
 * 用法示例：
 *   let lexer = FengLexer(source, "example.ff");
 *   var token = lexer.next();
 *   while token.kind != TokenKind.EOF {
 *     // 处理 token
 *     token = lexer.next();
 *   }
 */
open type FengLexer {
  seal let source: byte[];
  seal let sourceLength: int;
  seal let path: string;
  seal var pos: int;
  seal var line: int;
  seal var column: int;
  seal var pendingDoc: string;
  seal var pendingDocLineBreaks: int;
  seal var peeked: Token;
  seal var hasPeeked: bool;

  /** 从源码字符串创建 Lexer */
  open func FengLexer(source: string, path: string);

  /** 从字节数组创建 Lexer */
  open func FengLexer(source: byte[], length: int, path: string);

  /** 获取下一个 Token（消费） */
  open func next(): Token;

  /** 前瞻下一个 Token（不消费） */
  open func peek(): Token;

  /** 一次性产出全部 Token（含 EOF） */
  open func tokenize(): Token[];

  /** 获取源码文件路径 */
  open func path(): string;
}
```

### 5.2 核心流程

```
next()
  → 如果有 peeked，返回 peeked 并清除 hasPeeked
  → skipWhitespaceAndComments()
  → 记录 startOffset/startLine/startColumn
  → 如果 atEnd，返回 EOF Token
  → 读取当前字符并 advance
  → 按字符分发：
    - 标识符首字符（字母/_）→ scanIdentifierOrKeyword()
    - 数字 → scanNumber()
    - '@' → scanAnnotation()
    - '"' → scanString()
    - '`' → scanRawString()
    - 运算符/分隔符 → 直接返回对应 Token（部分需要前瞻，如 `+` vs `+=`、`<` vs `<=`）
    - 其他 → ERROR Token (LE0007)
  → 附加 pendingDoc（如有）
```

### 5.3 Trivia 处理（空白与注释）

`skipWhitespaceAndComments()` 跳过以下 trivia：

- **水平空白**：空格 `0x20`、制表符 `\t`、垂直制表 `\v`、换页 `\f`
- **换行**：`\n`、`\r\n`、`\r`（换行后更新 line/column，并检查 pendingDoc 是否需要清除）
- **行注释**：`//` 到行尾（清除 pendingDoc）
- **块注释**：`/* ... */`（未终止报错 LE0006；普通块注释清除 pendingDoc）
- **文档注释**：`/** ... */`（记录到 pendingDoc，附加到下一个 Token）

**文档注释附加规则**（与 C 版一致）：
- `/** ... */` 记录为 pendingDoc
- 如果文档注释与下一个 Token 之间隔了 ≥ 2 个换行，丢弃 pendingDoc
- `//` 行注释和普通 `/* */` 块注释会清除 pendingDoc

### 5.4 各扫描方法

#### scanIdentifierOrKeyword

- 首字符：`[a-zA-Z_]`
- 后续字符：`[a-zA-Z0-9_]`
- 扫描完整标识符后，查关键字表判断是关键字还是 IDENTIFIER
- 关键字查找策略：先用 match 表达式 + 线性扫描实现正确性，后续可优化为哈希表

**关键字表（30 个）**：
`type`, `enum`, `spec`, `fit`, `extern`, `func`, `let`, `var`, `static`, `open`, `seal`, `self`, `module`, `import`, `as`, `if`, `else`, `match`, `while`, `for`, `in`, `break`, `continue`, `try`, `catch`, `unknown`, `throw`, `return`, `defer`, `void`

**保留字检查**：如果标识符匹配保留字（`class`, `struct`, `const`, `export`, `prop`），产出 ERROR Token（LE0001）。

#### scanNumber

- 前缀识别：`0x`/`0X`（十六进制）、`0b`/`0B`（二进制）、`0o`/`0O`（八进制），否则十进制
- 数字间允许 `_` 分隔（如 `1_000_000`），但 `_` 不得出现在首位或末位
- 十进制支持小数点 `.` 和指数 `e`/`E`（可选正负号），产出 FLOAT Token
- 其他进制仅支持整数
- 解析值：整数用 `parseIntegerValue()`，浮点用 `parseFloatValue()`
- 无效数字字面量 → ERROR Token（LE0002）

#### scanString

- 双引号 `"` 开始，到下一个未转义的 `"` 结束
- 支持转义序列：`\n \t \r \\ \" \0 \xHH \u{HHHH}`
  - `\x` 后必须跟 2 个十六进制数字（否则 LE0004）
  - `\u{...}` 支持 1-6 个十六进制数字（否则 LE0004）
  - 其他 `\?` → LE0004（invalid string escape）
- 未终止 → ERROR Token（LE0003）

#### scanRawString

- 反引号 `` ` `` 开始，到下一个单个 `` ` `` 结束
- ` `` `（两个反引号）转义为单个 `` ` ``
- 不处理转义序列，所有字符原样保留
- 未终止 → ERROR Token（LE0003）

#### scanAnnotation

- `@` 后必须紧跟标识符首字符（否则 LE0005）
- 扫描 `@` + 标识符 → ANNOTATION Token
- 查内建注解表：匹配则设 `annotationKind` 为对应值，否则设为 `CUSTOM`

**内建注解表（8 个）**：
`abi`, `cdecl`, `stdcall`, `fastcall`, `runtime`, `iterable`, `iterator`, `value`

#### 运算符与分隔符

部分运算符需要前瞻（peek 下一字符）：

| 首字符 | 可能产出 | 前瞻逻辑 |
|--------|---------|---------|
| `+` | PLUS, PLUS_ASSIGN | 看下一个是否为 `=` |
| `-` | MINUS, MINUS_ASSIGN, ARROW | 看下一个是否为 `=` 或 `>` |
| `*` | STAR, STAR_ASSIGN | 看下一个是否为 `=` |
| `/` | SLASH, SLASH_ASSIGN | 看下一个是否为 `=`（`//` 和 `/*` 已在 trivia 处理） |
| `%` | PERCENT, PERCENT_ASSIGN | 看下一个是否为 `=` |
| `=` | ASSIGN, EQ | 看下一个是否为 `=` |
| `!` | NOT, NE | 看下一个是否为 `=` |
| `<` | LT, LE, SHL, SHL_ASSIGN | 看下一个是否为 `=` 或 `<`（再下一个 `=`） |
| `>` | GT, GE, SHR, SHR_ASSIGN | 看下一个是否为 `=` 或 `>`（再下一个 `=`） |
| `&` | AMP, AMP_ASSIGN, AND_AND | 看下一个是否为 `=` 或 `&` |
| `\|` | PIPE, PIPE_ASSIGN, OR_OR | 看下一个是否为 `=` 或 `\|` |
| `^` | CARET, CARET_ASSIGN | 看下一个是否为 `=` |
| `.` | DOT, ELLIPSIS | 看下一个两个是否都是 `.` |

### 5.5 错误码

与 C 版完全一致：

| 错误码 | 场景 | 错误文案 |
|--------|------|---------|
| LE0001 | 标识符匹配保留字 | reserved word cannot be used as an identifier |
| LE0002 | 无效数字字面量 | invalid numeric literal |
| LE0003 | 字符串/原始字符串未终止 | unterminated string literal / unterminated raw string literal |
| LE0004 | 无效转义序列 | invalid string escape / invalid \x escape |
| LE0005 | @ 后无标识符 | expected annotation name after '@' |
| LE0006 | 块注释未终止 | unterminated block comment |
| LE0007 | 无法识别的字符 | unexpected character |

### 5.6 与 C 版对应关系

| C 版函数 | Feng 版对应 | 说明 |
|----------|------------|------|
| `feng_lexer_init` | `FengLexer.FengLexer()` | 构造器 |
| `feng_lexer_next` | `FengLexer.next()` | 核心接口 |
| `feng_lexer_peek` | `FengLexer.peek()` | 前瞻 |
| `skip_whitespace_and_comments` | 内部 `skipTrivia()` | trivia 跳过 |
| `scan_identifier_or_keyword` | 内部 `scanIdentifierOrKeyword()` | 标识符/关键字 |
| `scan_number` | 内部 `scanNumber()` | 数字 |
| `scan_string` | 内部 `scanString()` | 普通字符串 |
| `scan_raw_string` | 内部 `scanRawString()` | 原始字符串 |
| `scan_annotation` | 内部 `scanAnnotation()` | 注解 |
| `parse_integer_slice` | 内部 `parseIntegerValue()` | 整数值解析 |
| `parse_float_slice` | 内部 `parseFloatValue()` | 浮点值解析 |
| `feng_lookup_keyword` | 内部关键字查找 | match + 线性扫描 |
| `feng_lookup_builtin_annotation` | 内部注解查找 | match + 线性扫描 |

---

## 6 TokenStream

**文件**：`std/src/compiler/Lexer/TokenStream.ff`
**模块**：`std.compiler`

### 6.1 设计

```feng
/**
 * Token 流封装。
 *
 * 为自定义注解的 pre-Parse 阶段提供 Token 流操作能力：
 * - 切片（slice）：提取声明的 Token 范围
 * - 替换（replace）：handler 输出替换 @X(...) target 范围
 * - 声明边界查找（findDeclarationEnd）：骨架找 target
 */
open type TokenStream {
  seal var tokens: Token[];

  /** 从 Token 数组创建 */
  open func TokenStream(tokens: Token[]);

  /** 从 Lexer 创建（自动 tokenize） */
  open static func fromLexer(lexer: FengLexer): TokenStream;

  // ---- 基础操作 ----

  /** Token 数量 */
  open func length(): int;

  /** 按索引获取 Token */
  open func at(index: int): Token;

  // ---- 注解变换操作 ----

  /**
   * 提取 Token 子范围 [start, end)。
   * 用于骨架找 target 时提取声明的 Token 范围。
   */
  open func slice(start: int, end: int): TokenSpan;

  /**
   * 替换 Token 子范围 [start, end) 为新的 Token 数组。
   * 用于注解 handler 输出替换整个 @X(...) target 范围。
   * 替换后 Token 流长度可能变化。
   */
  open func replace(start: int, end: int, replacement: Token[]): void;

  /**
   * 从指定位置向前查找声明边界。
   *
   * 算法：
   * 1. 从 startIndex 向前扫描
   * 2. 追踪花括号嵌套深度
   * 3. 遇到 '{' 深度 +1，遇到 '}' 深度 -1
   * 4. 深度归零后的下一个位置即为声明结束
   * 5. 对于无花括号的声明（如 `spec Foo: int | string;`），以分号为界
   *
   * 返回声明结束的索引（不含）。
   */
  open func findDeclarationEnd(startIndex: int): int;
}

/**
 * Token 流子范围视图。
 *
 * 持有对原 Token 数组的引用和范围索引，
 * 用于注解 handler 接收 target Token 范围。
 */
open type TokenSpan {
  /** 底层 Token 数组引用 */
  open let tokens: Token[];
  /** 起始索引（含） */
  open let start: int;
  /** 结束索引（不含） */
  open let end: int;

  /** 子范围长度 */
  open func length(): int;

  /** 按相对索引获取 Token */
  open func at(index: int): Token;

  /** 转为独立 Token 数组（拷贝） */
  open func toArray(): Token[];
}
```

### 6.2 findDeclarationEnd 算法细节

骨架找 target 是 pre-Parse 阶段的核心操作，需要识别声明的 Token 范围：

1. **有花括号的声明**（`type`, `spec`, `fit`, `func` 带体）：
   - 从 `@annotation` Token 后的声明关键字开始
   - 扫描到 `{` 后追踪嵌套深度
   - 匹配的 `}` 之后即为声明结束

2. **无花括号的声明**（`spec Foo: int | string;`, `extern func foo(): void;`）：
   - 以分号 `;` 为界

3. **绑定声明**（`let x: int = expr;`）：
   - 以分号 `;` 为界

4. **边界情况**：
   - 注解参数中的括号（`@annotation(arg)`）需要追踪圆括号深度，不混淆
   - 字符串和原始字符串中的花括号不计入深度

---

## 7 实施顺序

| 步骤 | 内容 | 前置 | 产出文件 |
|------|------|------|---------|
| 1 | StringBuilder 实现 | 无 | `std/src/text/StringBuilder.ff` |
| 2 | Token 类型定义 | 无 | `std/src/compiler/Lexer/FengToken.ff` |
| 3 | Lexer 实现 | 1, 2 | `std/src/compiler/Lexer/FengLexer.ff` |
| 4 | TokenStream 实现 | 2, 3 | `std/src/compiler/Lexer/TokenStream.ff` |
| 5 | Lexer 测试 | 1-4 | fcts 测试 |

---

## 8 验证方案

### 8.1 单元测试

用 fcts 测试框架，对 C 版 lexer 测试用例（`test/lexer/test_lexer.c`）编写对应 Feng 测试：

- 关键字识别（30 个关键字 + 5 个保留字报错）
- 标识符扫描（普通标识符 + 下划线开头）
- 数字扫描（十进制、十六进制、二进制、八进制、浮点数、带 `_` 分隔）
- 字符串扫描（普通字符串、转义序列、原始字符串）
- 注解扫描（内建注解 + 自定义注解 + `@` 后无标识符报错）
- 运算符和分隔符（全部 43 种）
- Trivia 处理（空白、行注释、块注释、文档注释附加规则）
- 错误处理（全部 7 个错误码场景）

### 8.2 对比验证

同一源码输入，比较 C 版和 Feng 版产出的 Token 序列：
- TokenKind 一致
- lexeme 一致
- 位置信息（offset, line, column）一致
- 语义值（integer, floating, boolean）一致
- annotationKind 一致

### 8.3 注解模拟

模拟 `@platform` 条件编译场景，验证 TokenStream 的 `slice`/`replace`/`findDeclarationEnd` 正确性。

---

## 9 与自定义注解的集成

### pre-Parse 阶段流程（引自 draft-2）

```
源码 → [C 版 Lex] → C Token 流 → [序列化 bytes] → [Feng 反序列化] → TokenStream
  → [注解 handler 变换] → TokenStream → [序列化 bytes] → [C 版反序列化] → C Token 流 → [Parse]
```

### 集成接口

- **TokenStream.slice/replace**：直接服务注解 handler 的 Token 流变换
- **FengLexer**：注解 handler 内部可用于解析注解参数子语法（如 `@platform(target == "linux")` 的条件表达式）
- **Token ↔ bytes 序列化**：后续 phase 2 实现，当前不预留接口

---

## 10 开放问题

1. **关键字查找优化时机**：当前方案用 match + 线性扫描，何时优化为哈希表？建议阶段一先用简单方案，性能测试后再决定
2. **Token 的 lexeme 字段**：当前方案用 `string` 引用源码子串。Feng 的 `string` 是否支持零拷贝切片（子串引用）？如果不支持，需要改用 `byte[]` + offset + length
3. **`@value` type 的 `open let` 字段**：已确认支持，如有问题是 Bug 需要修复
4. **文档注释存储**：C 版用指针引用源码中的文档注释文本。Feng 版如果 string 不支持零拷贝切片，文档注释可能需要拷贝
