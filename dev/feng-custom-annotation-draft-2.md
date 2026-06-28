# Feng 自定义注解设计草案 2

> 本文记录自定义注解系统的新一轮设计：基于 Token 流变换 + 注册机制。
> 与 `feng-custom-annotation-draft.md`（AST 数组路线）不同，本草案是当前讨论结论。
> **状态**：草案阶段，尚未实现，仅讨论。
> **核心目标**：phase 1（`@platform` 内建）→ phase 2（自定义注解）迁移无浪费。

---

## 0 背景与定位

### 两类注解的边界

Feng 的注解分两类，边界明确：

- **内建语义注解**（`@abi`、`@runtime`、`@iterable` 等）：在语义阶段跑，有符号表，能解析常量绑定。自定义注解**无法**实现这类——它们依赖语义信息。
- **自定义注解**（用户可定义）：在 pre-Parse 阶段跑，无符号表，做 Token 流变换。

### 时序约束

自定义注解**必须在 Parse 之前完成处理**。原因：

- `@platform` 这类注解会声明在同名的绑定或类型上，如果拖到语义阶段才处理，会引起重名声明错误或污染重载决策。
- 因此注解的输入输出都是 **Token 流**，不是 AST——Parse 及之后阶段完全不感知自定义注解，该报错就报错。

### 近期与远期

- **远期**：Feng 计划自举，自定义注解是远期计划（Feng 当前不够稳定）。
- **近期**：需要条件编译能力。如果不实现编译期注解，条件编译很难做；先引入一种简单宏在将来会成为包袱。
- **phase 1**：先提供临时内建 `@platform` 注解解决条件编译近需。
- **phase 2**：`@platform` 迁移为由自定义注解在 std 中实现（算是 breakchange，但自定义注解需要先 import，影响可控）。

### 设计原则：将来没有任何浪费

预 Parse 阶段的核心逻辑（扫描 Token 数组、解析 `@name`、骨架找 target、调注册函数、替换 Token 范围）一次写定。phase 1 和 phase 2 之间只是注册的函数实现不同：

- phase 1：注册的是 C 函数（`@platform` 等内建）。
- phase 2：注册的是 C wrapper（内部 dlopen .so、调 Feng 实现的注解）。

注册机制、handler 签名、流水线、错误处理都不变。

---

## 1 总体架构

### 阶段位置

```
Lex → 预 Parse 注解阶段 → Parse → Semantic → Codegen
```

预 Parse 注解阶段插入在 Lex 和 Parse 之间。

### 预 Parse 阶段流程

```
扫描 Token 数组
  → 发现 @name
  → 调用 resolver 把 name 解析成全名
  → 用全名查注册表
  → 调 handler（透传 ctx）
  → 用输出替换 Token 范围
  → 进入下一个注解
```

Parse 及之后阶段完全不感知自定义注解。

---

## 2 注册机制

### 统一注册表

注册表 key 是注解全名。两类来源：

- **内建**（`@platform`）：编译器启动时注册 C 函数，key 是短名（`platform`），不需要符号解析。
- **自定义**（将来）：.fb 加载后注册 C wrapper（内部 dlopen .so + bytes 序列化），key 是全名（`foo.bar.async`）。

### handler 签名（讨论用，非最终实现）

```c
typedef int (*FengAnnotationHandler)(
    const FengAnnotationContext *ctx,        /* 编译上下文载体 */
    const FengToken *arg_tokens,             /* @X(...) 括号内的 Token */
    size_t arg_token_count,
    const FengToken *target_tokens,          /* 被注解的声明 Token 范围 */
    size_t target_token_count,
    FengToken **out_tokens,                  /* 替换整个 @X(...) target 范围的输出 */
    size_t *out_token_count,
    FengAnnotationError *out_error
);
```

- `arg_tokens`：注解参数 Token（`@X(...)` 括号内，无参时为空）。
- `target_tokens`：被注解的声明的完整 Token 范围。
- `out_tokens`：替换整个 `@X(...) target` 范围的输出。返回空表示丢弃 target，返回 `target_tokens` 原样表示注解是 no-op 标记。
- `ctx`：编译上下文，见 §5。

`@platform` 和自定义注解的 wrapper 签名完全一致，预 Parse 阶段不区分。

---

## 3 多注解流水线

### 顺序规则

允许同一 target 上多个注解：`@a @b func foo() {}`（`@b` 近、`@a` 远）。

- 由近到远依次跑，前一个的输出是后一个的输入。
- `@b` 先跑：入参 = 原始 target，出参 = B，替换 `@b(...) target` 段。
- `@a` 后跑：入参 = B，出参 = A，替换 `@a(...) B` 段。
- 最终 = `a(b(target))`，标准函数复合。

### 边界情况

- `@b` 把 target 丢掉（出参为空）时，`@a` 拿到空 target，由 `@a` 的 handler 自决是报错还是跟着丢。
- 预 Parse 阶段逐个注解顺序消解，每次替换后 Token 流就变了，下一个注解看到的是上一步产物。

---

## 4 ctx：编译上下文载体

### 定位

ctx 不是"build 变量表"，它是个通用的编译上下文载体：

- 核心编译器拥有 ctx，在调 handler 时透传。
- ctx 的字段集是开放的：`target_os`、`target_arch`、`profile`、`feature_*`、将来可能还有 `module_path` 等。
- `@platform` 的 handler 从 ctx 读 build 变量；其他 handler 可读其他字段。
- CLI 参数（交叉编译的 `--target` 等）在编译器启动时填入 ctx。

### build 变量表的位置

build 变量表**不是核心编译器的一部分**。它是 `@platform` handler 内部的查表逻辑，表的数据来源是 ctx。核心编译器只负责把上下文信息装进 ctx 透传，不关心 `@platform` 怎么用。

---

## 5 错误处理（三层）

### 入口保证

lexer 输出的 Token 流一定合法，第一个 handler 拿到的是合法输入。

### 过程自检

每个 handler 拿到上一步输出，可以做自检。发现问题有两种策略：

- **直接报错**（fail fast）：归因到本注解，错误信息精确。
- **原地返回**：把问题留给最终检查。

鼓励能自检的就自检。

### 出口总检

流水线跑完后，预 Parse 阶段做一次**强制**健全性检查（花括号/方括号配对、关键字位置等）：

- 不合法就报"注解阶段产出非法 Token 流"并归因到注解阶段。
- 绝不让破损 Token 进 Parse（否则 Parse 会报看似无关的语法错误）。
- 不需要精确归因到哪个 handler——职责是兜底。

---

## 6 `@name` 解析（resolver 钩子）

### 注册与解析分离

- **注册**：不需要 import 信息。.fb 加载后把包里所有 `@annotation` 标记的函数都注册进注册表（每个挂一个 C wrapper）。注册表是"这个 .fb 提供了哪些注解"的索引，和当前文件有没有 import 无关。
- **解析**：在 pre-Parse 流水线里看到 `@async`（短名）时，需要解析成 `foo.bar.async`（全名）才能查注册表。这一步需要当前文件的 import 信息 + .ft 符号表。

注册和调用都不关心 `@name` 怎么解析成全名，只有解析这一步关心。resolver 是个**可插拔的钩子**。

### phase 1：平凡 resolver

只有内建（`@platform`）。resolver 是平凡的——短名直接当全名查内建注册项。不扫 import，不处理别名，零复杂度。

### phase 2：完整 resolver（开放问题）

自定义注解由 Feng 实现、需要 import，意味着真正编译前需要一轮"annotation 符号解析"（不是完整 Semantic）。这轮解析的范围被收得很窄：

- 骨架扫描当前文件的 `import` 语句（只找 `import X.Y.Z;` 模式，不构建 AST）。
- 用 import 表 + .ft 把短名解析成全名。
- **不分析当前文件的绑定、类型、表达式**——这些仍然留给真正的 Semantic 阶段。

这是为"自定义注解短名必须能用"付出的必然代价。对内建这条路径完全跳过。

### phase 2 解析策略未定

phase 2 的 resolver 有两个候选，现在不拍板：

- **(a) 完整复刻 Semantic 的 import 解析逻辑**：骨架扫描所有 import 形式（`import X.Y.Z;`、`import X.Y.Z as Z;`）+ 别名表 + 冲突检测。代价是和 Semantic 的解析逻辑保持同步，有发散风险。
- **(b) 自定义注解强制全名**：`@foo.bar.async` 形式，绕开整个解析环节。对"短名必须能用"的 breakchange，但只对自定义注解生效，内建不受影响。

因为框架把 resolver 隔离了，phase 1 落地时不需要现在就拍这个板。phase 2 无论选 (a) 还是 (b)，注册机制、handler 签名、流水线、错误处理都不变，只是换个 resolver 实现。

---

## 7 参数限制

### pre-Parse 时序的必然代价

自定义注解在 pre-Parse 跑，无符号表，**只能接受字面量参数**，无法解析常量绑定。内建语义注解在语义阶段跑，可接受字面量及常量绑定。

这与"内建是语义注解、自定义是语法注解"的边界一致：

- 语义注解（`@abi` 等）依赖符号表——自定义注解本来就不能实现这类。
- 语法注解用途上字面量参数通常够用（`@platform(target == "linux")`、`@async(timeout = 1000)` 都是字面量）。

### arg_tokens 透传原始 Token

预 Parse 阶段不做"args 必须是字面量"的全局校验，因为"字面量"的边界本身由 handler 定义（`@platform` 就要接受标识符 `target` 当变量名）。各 handler 自行决定接受哪种子语法：

- `@platform` 的 handler 接受 C1 子语法（`name == "str"`、`&&`/`||`/`!`），其中标识符当作 build 变量**名**查表，不是值引用。
- 未来某个自定义注解的 handler 可以有自己的 arg 子语法，但永远无法把标识符解析成常量绑定的**值**——这是硬约束，写进文档。

---

## 8 phase 1 落地项

phase 1 范围：`@platform` 作为内建 C 函数注册，解决条件编译近需。

1. **预 Parse 阶段框架**：扫描 Token 数组、骨架找 target Token 范围、调用 handler、替换 Token 范围。
2. **注册表**：内建启动时注册 C 函数。
3. **`@platform` handler**：C1 条件解析 + ctx 查表 + 保留/丢弃 target。
4. **ctx 结构**：CLI 填充 `target_os`/`target_arch`/`profile`/`feature_*` 等。
5. **出口总检**：花括号/方括号配对、关键字位置等健全性检查。
6. **平凡 resolver**：短名直查内建注册项。

`@platform` 的 C1 条件语法由其 handler 自解析，不复用编译器表达式解析器（那是 Parse 阶段能力）。

---

## 9 phase 2 留位

phase 2 不实现，只保证 phase 1 的接口不挡路：

1. **C wrapper**：dlopen .so + Token↔bytes 序列化 + 调 `__feng_annotation_main__`。
2. **.fb 的 `anno/<platform>/` 层**：预编译的 .so 放置位置。
3. **`@annotation` 内建标记**：把普通函数标记为注解，标记信息进 .ft。
4. **自定义注解 resolver**：短名/全名/`import as` 解析（策略 a/b 待定）。

### phase 2 的执行模型（已定）

- 自定义注解的实际实现在一个单独的 .fb 包中。
- .fb 编译时根据 `@annotation` 收集本包内的注解，生成注解动态库放到 `anno/<platform>/`。
- 每个 (包, 架构) 只有一个动态库，调用哪个注解通过 C ABI 入口的全名参数传入，可执行文件内通过全名分发到不同注解逻辑。
- 注解按正常符号解析，底层用动态库是实现机制，用户不感知。
- C ABI 入口形如：`__feng_annotation_main__(fullname, request_bytes, request_len, out_response_bytes, out_response_len)` + `__feng_annotation_release_response`。
- 编译器不感知复杂类型，所以编译器↔.so 之间用字节流传输；std 层（.so 内）负责 bytes ↔ Feng TokenStream 的解析。

---

## 10 待定开放问题

1. **phase 2 自定义注解的 `@name` 解析策略**：(a) 完整 import 解析 / (b) 强制全名。
2. **`@platform` 的 C1 条件语法精确闭环**：支持的运算符、字面量类型、变量名集合。
3. **ctx 里 build 变量集合**：`target_os`/`target_arch`/`profile`/`feature_*` 的闭环清单，变量集合是编译器内置固定还是允许包声明扩展。
4. **CLI 参数怎么进 ctx**：`--target`、`-Dfeature_*` 等参数的解析与填充。
5. **.fb 的 `anno/<platform>/` 层结构**（phase 2）：.so 命名、版本、缓存策略。
6. **bytes 序列化格式**（phase 2）：Token 编码细节、format_version 字段。
7. **骨架找 target 的实现**：识别声明起始关键字（`func`/`let`/`type`/`struct`/`module` 等）、正确追踪花括号/缩进层级的具体算法。
8. **注解缓存策略**（phase 2）：相同输入是否缓存 handler 输出，缓存放在哪。
