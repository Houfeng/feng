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

Feng 版面向更好的设计，不机械对齐 C 版——Lexer 只管词法切分（含空白和注释），语义解析（数值、注解分类）交给 Parser（详见 `feng-std-parser-draft.md`）。C 版编译器继续作为当前主力编译器使用；Feng 版首先服务于自定义注解，远期服务于自举。

---

## 1 设计决策

| 决策项 | 结论 | 理由 |
|--------|------|------|
| 交付范围 | Lexer + Parser 整体规划，分两阶段；本文覆盖阶段一 | Lexer 先交付解锁注解 |
| Token 与 C 版关系 | 面向更好的设计，不机械对齐 C 版 | Lexer 只管词法切分，语义解析交给 Parser |
| Token 类型形式 | `@value` type | 避免每个 token 堆分配；64 bytes memcpy 远快于 malloc + RC |
| StringBuilder | 同步实现（详见 `feng-std-string-builder-dev.md`） | Lexer 字符串扫描依赖 |
| 序列化预留 | 先实现 Token 后再考虑 | |
| 文档注释关联 | Lexer 不处理 pendingDoc，仅发射 CommentDoc Token | 关联语义属 Parser 职责，Lexer 保持纯词法切分 |

### Token @value type 性能分析

FengToken 字段布局（@value 类型字段内联展开）：

```
FengToken — 8 words (64 bytes on 64-bit)
├── kind: FengTokenKind             1 word   (enum/int)
├── value: StringSpan               3 words  (origin:string + start:int + end:int)
├── source: FengSource              1 word   (reference)
└── location: FengLocation    3 words  (offset:int + line:int + column:int)
```

- StringSpan.origin（`seal` 私有）持有源码 string 的引用，与 FengSource.content 共享同一 string 实例，保证 token value 指向的源码子串不会被释放
- FengSource 为托管引用，与 StringSpan.origin 共享同一 string 实例，提供 path 和字节级/子串访问 API（`at`/`slice`/`clone`/`length`/`extractLineSnippet`）；content 字段私有化（`seal`），外部不直接访问
- 两个引用字段（StringSpan.origin、FengSource）的 RC 成本仅为原子计数加减，同一文件所有 token 共享同一实例，开销可忽略

**@value vs 普通 type 对比**：

| | @value type | 普通 type |
|---|---|---|
| 分配 | 栈分配，零 heap 开销 | 每个 token 1 次 heap 分配（malloc） |
| 传递 | 64 bytes memcpy（L1 cache ~1-2ns） | 指针传递 + RC 增减 |
| 释放 | 自动，无 GC/RC 扫描 | 每个 token 1 次 heap 释放 + 字段 RC 释放 |
| 总计（10 万 token） | ~10 万次 memcpy | ~10 万次 malloc + ~40 万次 RC 操作 + ~10 万次 free |

- 结论：@value 明显更优，核心收益是避免每个 token 的堆分配

---

## 2 模块结构

```
std/src/text/
└── StringBuilder.ff         # 字符串构建器（Lexer 前置依赖）

std/src/compiler/
├── Common/
│   └── FengCompileError.ff  # 编译错误（Lexer/Parser 共用）
└── Lexer/
    ├── FengTokenKind.ff      # Token 种类枚举
    ├── FengSource.ff         # 源文件信息
    ├── FengLocation.ff       # 源码位置信息（@value type）
    ├── FengToken.ff         # Token 类型定义
    ├── FengTokenUtil.ff     # Token 分类工具（静态方法）
    ├── FengLexer.ff         # Lexer 实现
    └── FengTokenTransformer.ff  # Token 变换器（服务注解 pre-Parse 变换）
```

---

## 2b Common 类型

**目录**：`std/src/compiler/Common/`
**模块**：`std.compiler.common`

### FengCompileError

Lexer 和 Parser 共用的编译错误类型，词法错误码前缀 `LE`，语法错误码前缀 `PE`。
词法/语法错误不混入 Token 流，遇到错误直接抛出 `FengCompileError`，由调用方捕获处理。
自包含 path/line/column，不依赖 `FengLocation` 或 `FengSource`。

```feng
open module std.compiler.common;

/**
 * 编译错误。
 *
 * Lexer 和 Parser 共用，通过 code 前缀区分来源：
 * - LE00xx：词法错误
 * - PE00xx：语法错误（将来）
 *
 * throw 会装箱，但异常路径 RC 开销可忽略。
 */
open type FengCompileError {
  /** 错误码（如 "LE0001"） */
  let code: string;
  /** 错误描述 */
  let message: string;
  /** 源文件路径 */
  let path: string;
  /** 行号（从 1 开始） */
  let line: int;
  /** 列号（从 1 开始） */
  let column: int;
  /** 出错行的源代码片段 */
  let snippet: string;
}
```

---

## 3 Token 类型定义

**目录**：`std/src/compiler/Lexer/`
**模块**：`std.compiler.lexer`

### 3.1 FengTokenKind 枚举

按类别分段编号，区间判断代替逐个枚举匹配。特殊/空白/注释合并在 0xx 并分子段，标识符、字面量、关键字、分隔符、运算符各占独立百段。
完整拼写命名，类别前缀区分。

```feng
open module std.compiler.lexer;

/**
 * Token 种类枚举。
 *
 * 按类别分段编号，便于区间判断分类：
 * - isTrivia(kind): kind < 100（特殊/空白/注释，含子区间）
 * - isIdentifier(kind): kind >= 200 && kind < 300
 * - isLiteral(kind): kind >= 300 && kind < 400
 * - isKeyword(kind): kind >= 400 && kind < 500
 * - isPunctuation(kind): kind >= 500 && kind < 600
 * - isOperator(kind): kind >= 600 && kind < 700
 */
open enum FengTokenKind {
  // ---- 特殊/空白/注释 (0xx) ----
  // 特殊 (0-19)
  SpecialEndOfFile = 0,
  // 空白 (20-39)
  WhitespaceSpace = 20,
  WhitespaceNewline = 21,
  // 注释 (40-59)
  CommentLine = 40,
  CommentBlock = 41,
  CommentDoc = 42,

  // ---- 标识符 (2xx) ----
  Identifier = 200,

  // ---- 字面量 (3xx) ----
  LiteralInteger = 300,
  LiteralFloat = 301,
  LiteralString = 302,
  LiteralRawString = 303,
  LiteralBool = 304,

  // ---- 关键字 (4xx) ----
  KeywordType = 400,
  KeywordEnum = 401,
  KeywordSpec = 402,
  KeywordFit = 403,
  KeywordExtern = 404,
  KeywordFunc = 405,
  KeywordLet = 406,
  KeywordVar = 407,
  KeywordStatic = 408,
  KeywordOpen = 409,
  KeywordSeal = 410,
  KeywordSelf = 411,
  KeywordModule = 412,
  KeywordImport = 413,
  KeywordAs = 414,
  KeywordIf = 415,
  KeywordElse = 416,
  KeywordMatch = 417,
  KeywordWhile = 418,
  KeywordFor = 419,
  KeywordIn = 420,
  KeywordBreak = 421,
  KeywordContinue = 422,
  KeywordTry = 423,
  KeywordCatch = 424,
  KeywordUnknown = 425,
  KeywordThrow = 426,
  KeywordReturn = 427,
  KeywordDefer = 428,
  KeywordVoid = 429,

  // ---- 分隔符 (5xx) ----
  PunctuationLeftParen = 500,
  PunctuationRightParen = 501,
  PunctuationLeftBrace = 502,
  PunctuationRightBrace = 503,
  PunctuationLeftBracket = 504,
  PunctuationRightBracket = 505,
  PunctuationComma = 506,
  PunctuationColon = 507,
  PunctuationSemicolon = 508,
  PunctuationDot = 509,
  PunctuationEllipsis = 510,

  // ---- 运算符 (6xx) ----
  // 算术 (600-609)
  OperatorPlus = 600,
  OperatorMinus = 601,
  OperatorStar = 602,
  OperatorSlash = 603,
  OperatorPercent = 604,
  // 赋值 (610-619)
  OperatorAssign = 610,
  OperatorPlusAssign = 611,
  OperatorMinusAssign = 612,
  OperatorStarAssign = 613,
  OperatorSlashAssign = 614,
  OperatorPercentAssign = 615,
  // 比较 (620-629)
  OperatorLess = 620,
  OperatorLessEqual = 621,
  OperatorGreater = 622,
  OperatorGreaterEqual = 623,
  OperatorEqual = 624,
  OperatorNotEqual = 625,
  // 逻辑 (630-639)
  OperatorNot = 630,
  OperatorLogicalAnd = 631,
  OperatorLogicalOr = 632,
  // 位运算 (640-649)
  OperatorBitwiseAnd = 640,
  OperatorBitwiseOr = 641,
  OperatorBitwiseXor = 642,
  OperatorBitwiseNot = 643,
  OperatorShiftLeft = 644,
  OperatorShiftRight = 645,
  // 位运算赋值 (650-659)
  OperatorBitwiseAndAssign = 650,
  OperatorBitwiseOrAssign = 651,
  OperatorBitwiseXorAssign = 652,
  OperatorShiftLeftAssign = 653,
  OperatorShiftRightAssign = 654,
  // 其他 (660-669)
  OperatorArrow = 660,
  OperatorAt = 661
}
```

**区间总览**：

| 区间 | 类别 | 前缀 | 已用 | 预留 |
|------|------|------|------|------|
| 0xx | 特殊/空白/注释 | `Special`/`Whitespace`/`Comment` | 6 | 94 |
| 2xx | 标识符 | `Identifier` | 1 | 99 |
| 3xx | 字面量 | `Literal` | 5 | 95 |
| 4xx | 关键字 | `Keyword` | 30 | 70 |
| 5xx | 分隔符 | `Punctuation` | 11 | 89 |
| 6xx | 运算符 | `Operator` | 31 | 69 |

### 3.2 FengSource

```feng
/**
 * 源文件信息。
 * 普通类型（堆分配、引用语义），由 Lexer 创建，每个 Token 持有引用。
 * 源文件数量少、生命周期长，所有引用同一源文件的 Token 共享同一实例，
 * 避免每 Token 复制文件内容。
 *
 * content 私有化（seal），外部不直接访问，统一通过 length/at/slice/clone
 * 等公开 API 获取字节或子串。字节访问的优化点收敛在本类型内部，便于
 * 未来调整（如缓存 bytes 视图、提供无边界检查的 raw 访问），不影响 Lexer。
 */
open type FengSource {
  /** 源文件路径 */
  let path: string;
  /** 源文件内容（私有，外部通过公开 API 访问） */
  seal let content: string;

  /** 从字符串内容创建 */
  func FengSource(content: string, path: string) {
    self.path = path;
    self.content = content;
  }
  /** 从字节数组创建（解码为字符串） */
  func FengSource(content: byte[], path: string) {
    self.path = path;
    self.content = string.fromUtf8Bytes(content);
  }

  /** 源文件字节长度 */
  func length(): int;
  /** 获取指定字节偏移的字节值 */
  func at(index: int): byte;
  /** 获取指定字节范围的 StringSpan 视图（零拷贝） */
  func slice(start: int, end: int): StringSpan;
  /** 克隆指定字节范围的字符串（独立拷贝） */
  func clone(start: int, end: int): string;

  /**
   * 提取指定字节偏移所在行的源码片段（不含行尾换行）。
   * 用于 Lexer/Parser 构造 FengCompileError 时填充 snippet 字段。
   * 错误报告为冷路径，按需将 content 转为字节访问，不常驻缓存。
   * 偏移越界时夹到 [0, length] 区间后定位行边界。
   */
  func extractLineSnippet(offset: int): string;
}
```

### 3.3 FengLocation

```feng
/**
 * 源码位置信息。
 * 记录 Token 在源文件中的位置，用于错误报告和注解变换。
 */
@value
open type FengLocation {
  /** 字节偏移量（从 0 开始） */
  let offset: int;
  /** 行号（从 1 开始） */
  let line: int;
  /** 列号（从 1 开始） */
  let column: int;
}
```

### 3.4 FengToken

```feng
/**
 * Token 实例——词法分析的最小单元。
 *
 * @value type：栈分配、值语义、零 heap/RC 压力。
 * value 使用 StringSpan 引用源码子串，避免拷贝。
 *
 * Lexer 只负责词法切分，不解析语义值。
 * 数值解析（integer/float/bool）和注解分类由 Parser 阶段处理。
 */
@value
open type FengToken {
  /** Token 种类 */
  let kind: FengTokenKind;
  /** 原始文本（引用源码字符串的子串） */
  let value: StringSpan;
  /** Token 所在的源文件 */
  let source: FengSource;
  /** 源码位置 */
  let location: FengLocation;
}
```

### 3.5 FengTokenUtil

基于区间判断的静态工具方法，无需逐个枚举匹配：

```feng
/**
 * Token 工具类。
 * 提供基于 FengTokenKind 区间编号的分类判断和名称查询，
 * 所有方法均为静态方法，通过区间比较实现 O(1) 判断。
 */
open type FengTokenUtil {
  /** 判断是否为 trivia（0xx：特殊/空白/注释） */
  open static func isTrivia(kind: FengTokenKind): bool;

  /** 判断是否为标识符（2xx 区间） */
  open static func isIdentifier(kind: FengTokenKind): bool;

  /** 判断是否为字面量（3xx 区间） */
  open static func isLiteral(kind: FengTokenKind): bool;

  /** 判断是否为关键字（4xx 区间） */
  open static func isKeyword(kind: FengTokenKind): bool;

  /** 判断是否为分隔符（5xx 区间） */
  open static func isPunctuation(kind: FengTokenKind): bool;

  /** 判断是否为运算符（6xx 区间） */
  open static func isOperator(kind: FengTokenKind): bool;

  /** 判断是否为空白（0xx 子区间：20-39） */
  open static func isWhitespace(kind: FengTokenKind): bool;

  /** 判断是否为注释（0xx 子区间：40-59） */
  open static func isComment(kind: FengTokenKind): bool;

  /** 获取 FengTokenKind 的名称字符串（如 "KeywordType"、"OperatorPlus"） */
  open static func kindName(kind: FengTokenKind): string;
}
```

---

## 4 Lexer 实现

**文件**：`std/src/compiler/Lexer/FengLexer.ff`
**模块**：`std.compiler.lexer`

### 4.1 设计

```feng
/**
 * Feng 词法分析器。
 *
 * 将源码字符串转换为 Token 流。
 * 逐字节扫描 + 回溯前瞻，与 C 版 lexer（src/lexer/lexer.c）行为等价。
 *
 * 用法示例：
 *   let source = FengSource(content, "example.ff");
 *   var lexer = FengLexer(source);
 *   var token = lexer.next();
 *   while token.kind != FengTokenKind.SpecialEndOfFile {
 *     // 处理 token
 *     token = lexer.next();
 *   }
 */
open type FengLexer {
  seal let source: FengSource;
  /** 字节长度（构造时一次性缓存，避免热路径重复调用 source.length()） */
  seal let length: int;
  seal var pos: int;
  seal var line: int;
  seal var column: int;
  seal var peeked: FengToken;
  seal var hasPeeked: bool;

  /** 从 FengSource 创建 Lexer */
  open func FengLexer(source: FengSource);

  /**
   * 获取下一个 Token（消费）。
   * @throws FengCompileError 遇到词法错误时抛出
   */
  open func next(): FengToken;

  /**
   * 前瞻下一个 Token（不消费）。
   * @throws FengCompileError 遇到词法错误时抛出
   */
  open func peek(): FengToken;

  /**
   * 一次性产出全部 Token（含 EOF）。
   * 内部循环调用 scanNext()，遇错即抛。
   * @throws FengCompileError 遇到词法错误时抛出
   */
  open func tokenize(): FengToken[];
}
```

**字节访问设计**：Lexer 不持有源码 bytes 副本，所有字节级访问（`peekChar`/`peekCharAt`/`advance`/`scanNumber` 中的 `source.at(idx)`）与子串构造（`makeToken` 中的 `source.slice(start, end)`）均通过 FengSource 公开 API 完成。`length` 在构造时一次性缓存，避免热路径重复调用 `source.length()`。此设计的优化点收敛在 FengSource，未来若需在内部加 bytes 视图缓存或无边界检查的 raw 访问，Lexer 无需变更。

**内部方法拆分原则**：FengLexer 内部应尽可能将逻辑拆解为多个功能完整的小方法，避免巨型方法。每个方法职责单一、边界清晰，便于理解、测试和维护。

内部方法拆分如下（均为 `seal` 私有方法）：

| 方法 | 职责 |
|------|------|
| `scanNext()` | 核心调度：记录起始位置，按首字符分发到各扫描方法，产出 Token |
| `scanWhitespace()` | 扫描连续同类水平空白或换行 |
| `scanLineComment()` | 扫描 `//` 到行尾 |
| `scanBlockOrDocComment()` | 扫描 `/* ... */`，区分 `CommentBlock` 与 `CommentDoc` |
| `scanIdentifierOrKeyword()` | 扫描标识符并查关键字表，含保留字检查 |
| `scanNumber()` | 扫描数字字面量（各进制、浮点、`_` 分隔） |
| `scanString()` | 扫描双引号字符串，处理转义序列 |
| `scanRawString()` | 扫描反引号原始字符串 |
| `scanOperatorOrPunctuation()` | 扫描运算符和分隔符，含前瞻逻辑 |
| `advance()` | 前进一字节，维护 pos/line/column |
| `peekChar()` / `peekCharAt()` | 前瞻当前/指定偏移的字节，不消费 |
| `makeToken()` | 从起始位置和当前 pos 构造 FengToken |
| `throwError()` | 构造并抛出 FengCompileError |

`scanNext()` 只做分发，各 `scan*` 方法内部逻辑自包含，不再嵌套巨型分支。

### 4.2 核心流程

```
next()
  → 如果有 peeked，返回 peeked 并清除 hasPeeked
  → 调用 scanNext()

scanNext()
  → 记录 startOffset/startLine/startColumn
  → 如果 atEnd，返回 SpecialEndOfFile Token
  → 读取当前字符，分发到对应扫描方法：
    - 空白字符 → scanWhitespace()
    - '/' + '/' → scanLineComment()
    - '/' + '*' → scanBlockOrDocComment()
    - 标识符首字符（字母/_）→ scanIdentifierOrKeyword()
    - 数字 → scanNumber()
    - '"' → scanString()
    - '`' → scanRawString()
    - 运算符/分隔符首字符 → scanOperatorOrPunctuation()
    - 其他 → throwError() (LE0007)
```

### 4.3 Trivia 处理（空白与注释）

Lexer 将空白和注释作为独立 Token 发射，不跳过。Parser 在构建 AST 时过滤 trivia Token。

**空白扫描**：
- **水平空白**：空格 `0x20`、制表符 `\t`、垂直制表 `\v`、换页 `\f` → `WhitespaceSpace`
- **换行**：`\n`、`\r\n`、`\r` → `WhitespaceNewline`（更新 line/column）
- 连续同类空白合并为一个 Token

**注释扫描**：
- **行注释**：`//` 到行尾 → `CommentLine`
- **块注释**：`/* ... */` → `CommentBlock`（未终止抛出 FengCompileError LE0006）
- **文档注释**：`/** ... */` → `CommentDoc`

Lexer 不维护文档注释与目标声明的关联。`CommentDoc` 仅作为普通 trivia Token 发射，"附加到下一个非 trivia Token" 的语义交给 Parser 阶段处理。

### 4.4 各扫描方法

#### scanIdentifierOrKeyword

- 首字符：`[a-zA-Z_]`
- 后续字符：`[a-zA-Z0-9_]`
- 扫描完整标识符后，查关键字表判断是关键字还是 IDENTIFIER
- 关键字查找策略：先用 match 表达式 + 线性扫描实现正确性，后续可优化为哈希表

**关键字表（30 个）**：
`type`, `enum`, `spec`, `fit`, `extern`, `func`, `let`, `var`, `static`, `open`, `seal`, `self`, `module`, `import`, `as`, `if`, `else`, `match`, `while`, `for`, `in`, `break`, `continue`, `try`, `catch`, `unknown`, `throw`, `return`, `defer`, `void`

**保留字检查**：如果标识符匹配保留字（`class`, `struct`, `const`, `export`, `prop`），抛出 FengCompileError（LE0001）。

#### scanNumber

- 前缀识别：`0x`/`0X`（十六进制）、`0b`/`0B`（二进制）、`0o`/`0O`（八进制），否则十进制
- 数字间允许 `_` 分隔（如 `1_000_000`），但 `_` 不得出现在首位或末位
- 十进制支持小数点 `.` 和指数 `e`/`E`（可选正负号），产出 FLOAT Token
- 其他进制仅支持整数
- Lexer 只扫描数字文本格式，不解析数值；数值解析由 Parser 处理
- 无效数字字面量 → 抛出 FengCompileError（LE0002）

#### scanString

- 双引号 `"` 开始，到下一个未转义的 `"` 结束
- 支持转义序列：`\n \t \r \\ \" \0 \xHH`
  - `\x` 后必须跟 2 个十六进制数字（否则 LE0004）
  - 其他 `\?` → LE0004（invalid string escape）
- 未终止 → 抛出 FengCompileError（LE0003）

#### scanRawString

- 反引号 `` ` `` 开始，到下一个单个 `` ` `` 结束
- ` `` `（两个反引号）转义为单个 `` ` ``
- 不处理转义序列，所有字符原样保留
- 未终止 → 抛出 FengCompileError（LE0003）

#### 运算符与分隔符

部分运算符需要前瞻（peek 下一字符）：

| 首字符 | 可能产出 | 前瞻逻辑 |
|--------|---------|---------|
| `+` | OperatorPlus, OperatorPlusAssign | 看下一个是否为 `=` |
| `-` | OperatorMinus, OperatorMinusAssign, OperatorArrow | 看下一个是否为 `=` 或 `>` |
| `*` | OperatorStar, OperatorStarAssign | 看下一个是否为 `=` |
| `/` | OperatorSlash, OperatorSlashAssign | 看下一个是否为 `=`（`//` 和 `/*` 已在 trivia 处理） |
| `%` | OperatorPercent, OperatorPercentAssign | 看下一个是否为 `=` |
| `=` | OperatorAssign, OperatorEqual | 看下一个是否为 `=` |
| `!` | OperatorNot, OperatorNotEqual | 看下一个是否为 `=` |
| `<` | OperatorLess, OperatorLessEqual, OperatorShiftLeft, OperatorShiftLeftAssign | 看下一个是否为 `=` 或 `<`（再下一个 `=`） |
| `>` | OperatorGreater, OperatorGreaterEqual, OperatorShiftRight, OperatorShiftRightAssign | 看下一个是否为 `=` 或 `>`（再下一个 `=`） |
| `&` | OperatorBitwiseAnd, OperatorBitwiseAndAssign, OperatorLogicalAnd | 看下一个是否为 `=` 或 `&` |
| `\|` | OperatorBitwiseOr, OperatorBitwiseOrAssign, OperatorLogicalOr | 看下一个是否为 `=` 或 `\|` |
| `^` | OperatorBitwiseXor, OperatorBitwiseXorAssign | 看下一个是否为 `=` |
| `.` | PunctuationDot, PunctuationEllipsis | 看下一个两个是否都是 `.` |
| `@` | OperatorAt | 无前瞻，直接产出 |

### 4.5 错误码

基于 C 版，移除 LE0005（注解识别移至 Parser）：

| 错误码 | 场景 | 错误文案 |
|--------|------|---------|
| LE0001 | 标识符匹配保留字 | reserved word cannot be used as an identifier |
| LE0002 | 无效数字字面量 | invalid numeric literal |
| LE0003 | 字符串/原始字符串未终止 | unterminated string literal / unterminated raw string literal |
| LE0004 | 无效转义序列 | invalid string escape / invalid \x escape |
| LE0006 | 块注释未终止 | unterminated block comment |
| LE0007 | 无法识别的字符 | unexpected character |

### 4.6 与 C 版对应关系

| C 版函数 | Feng 版对应 | 说明 |
|----------|------------|------|
| `feng_lexer_init` | `FengLexer.FengLexer()` | 构造器 |
| `feng_lexer_next` | `FengLexer.next()` | 核心接口 |
| `feng_lexer_peek` | `FengLexer.peek()` | 前瞻 |
| `skip_whitespace_and_comments` | 内部 `scanWhitespace()`/`scanComment()` | 发射为 Token，不再跳过 |
| `scan_identifier_or_keyword` | 内部 `scanIdentifierOrKeyword()` | 标识符/关键字 |
| `scan_number` | 内部 `scanNumber()` | 数字 |
| `scan_string` | 内部 `scanString()` | 普通字符串 |
| `scan_raw_string` | 内部 `scanRawString()` | 原始字符串 |
| `feng_lookup_keyword` | 内部关键字查找 | match + 线性扫描 |

---

## 5 FengTokenTransformer

**文件**：`std/src/compiler/Lexer/FengTokenTransformer.ff`
**模块**：`std.compiler.lexer`

### 5.1 设计

FengTokenTransformer 是基于 `FengToken[]` 的可变变换器，服务注解 pre-Parse 阶段：

- **职责**：接收已 lex 的 Token 数组，提供 `slice`/`replace`/`findDeclarationEnd` 等查找与变换操作
- **消费方**：注解 handler 的 pre-Parse 变换
- **与 TokenStream 概念的区分**：FengTokenTransformer 专注"变换"；未来若 Parser 需要流式读取接口，另立只读 `TokenStream` 概念（封装 `next`/`peek`/`lookback`，**不提供 `replace` 等变换方法**），与 FengTokenTransformer 职责严格分离。当前阶段不引入 TokenStream，Parser 衔接方式见 `feng-std-parser-draft.md`

```feng
/**
 * Token 变换器。
 *
 * 基于 FengToken[] 的可变变换器，为自定义注解的 pre-Parse 阶段提供 Token 操作能力：
 * - 切片（slice）：提取声明的 Token 范围
 * - 替换（replace）：handler 输出替换 @X(...) target 范围
 * - 声明边界查找（findDeclarationEnd）：骨架找 target
 *
 * 与 TokenStream 概念区分：FengTokenTransformer 专注变换（基于已 lex 的 Token 数组，
 * 支持随机访问与修改）；未来若引入只读 TokenStream（封装 next/peek/lookback 等
 * 流式读取），其职责仅为 Parser 提供 Token 流读取接口，不提供 replace 等变换方法。
 */
open type FengTokenTransformer {
  seal var tokens: FengToken[];

  /** 从 Token 数组创建 */
  open func FengTokenTransformer(tokens: FengToken[]);

  /** 从 Lexer 创建（自动 tokenize） */
  open static func fromLexer(lexer: FengLexer): FengTokenTransformer;

  // ---- 基础操作 ----

  /** Token 数量 */
  open func length(): int;

  /** 按索引获取 Token */
  open func at(index: int): FengToken;

  // ---- 注解变换操作 ----

  /**
   * 提取 Token 子范围 [start, end)。
   * 用于骨架找 target 时提取声明的 Token 范围。
   * 返回 std.collections.Span<FengToken>，复用标准库泛型视图类型。
   */
  open func slice(start: int, end: int): Span<FengToken>;

  /**
   * 替换 Token 子范围 [start, end) 为新的 Token 数组。
   * 用于注解 handler 输出替换整个 @X(...) target 范围。
   * 替换后 Token 数组长度可能变化。
   */
  open func replace(start: int, end: int, replacement: FengToken[]): void;

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
```

**设计说明**：`slice` 方法返回 `Span<FengToken>`（`std.collections.Span` 泛型实例），复用标准库已有的只读切片视图类型，无需引入额外的 `TokenSpan` 类型。`Span<T>` 提供 `length()`、`at()`、`toArray()` 等方法，满足注解 handler 对 Token 子范围的所有访问需求。

### 5.2 findDeclarationEnd 算法细节

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

## 6 实施顺序

| 步骤 | 内容 | 前置 | 产出文件 |
|------|------|------|---------|
| 1 | StringBuilder 实现（详见 `feng-std-string-builder-dev.md`） | 无 | `std/src/text/StringBuilder.ff` |
| 2 | FengCompileError 类型定义 | 无 | `std/src/compiler/Common/FengCompileError.ff` |
| 3 | Token 类型定义 | 2 | `std/src/compiler/Lexer/FengToken.ff` |
| 4 | Lexer 实现 | 1, 2, 3 | `std/src/compiler/Lexer/FengLexer.ff` |
| 5 | FengTokenTransformer 实现 | 3, 4 | `std/src/compiler/Lexer/FengTokenTransformer.ff` |
| 6 | Lexer 测试 | 1-5 | fcts 测试 |

---

## 7 验证方案

### 7.1 单元测试

用 fcts 测试框架，对 C 版 lexer 测试用例（`test/lexer/test_lexer.c`）编写对应 Feng 测试：

- 关键字识别（30 个关键字 + 5 个保留字报错）
- 标识符扫描（普通标识符 + 下划线开头）
- 数字扫描（十进制、十六进制、二进制、八进制、浮点数、带 `_` 分隔）
- 字符串扫描（普通字符串、转义序列、原始字符串）
- 运算符和分隔符（全部 43 种，含 OperatorAt）
- Trivia 发射（空白 Token、行注释、块注释、文档注释）
- 错误处理（全部 6 个错误码场景）

### 7.2 对比验证

同一源码输入，比较 C 版和 Feng 版产出的 Token 序列：
- FengTokenKind 一致
- value 一致
- 位置信息（offset, line, column）一致

### 7.3 注解模拟

模拟 `@platform` 条件编译场景，验证 FengTokenTransformer 的 `slice`/`replace`/`findDeclarationEnd` 正确性。

---

## 8 与自定义注解的集成

### pre-Parse 阶段流程（引自 draft-2）

```
源码 → [C 版 Lex] → C Token 流 → [序列化 bytes] → [Feng 反序列化] → FengTokenTransformer
  → [注解 handler 变换] → FengTokenTransformer → [序列化 bytes] → [C 版反序列化] → C Token 流 → [Parse]
```

### 集成接口

- **FengTokenTransformer.slice/replace**：直接服务注解 handler 的 Token 流变换
- **FengLexer**：注解 handler 内部可用于解析注解参数子语法（如 `@platform(target == "linux")` 的条件表达式）
- **Token ↔ bytes 序列化**：后续 phase 2 实现，当前不预留接口

---

## 9 开放问题

1. **关键字查找优化时机**：当前方案用 match + 线性扫描，何时优化为哈希表？建议阶段一先用简单方案，性能测试后再决定
2. **Token 的 value 字段**：已确定使用 `StringSpan`（引用源码子串，零拷贝）。`FengSource.content`（`seal` 私有）为托管引用类型，生命周期由引用计数管理；外部通过 `at`/`slice`/`clone` 等公开 API 访问字节或子串
3. **`@value` type 的 `open let` 字段**：已确认支持，如有问题是 Bug 需要修复
