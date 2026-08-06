# Feng mixin LSP Hover 优化开发方案

> 状态：已实施
>
> 日期：2026-08-06
>
> 关联文档：
>
> - [`feng-type.md`](../specifications/feng-type.md)：定义 `...` 成员展开的正式语言语义。
> - [`feng-function.md`](../specifications/feng-function.md)：定义 `@mixable` 与生成 wrapper 的正式语言语义。
> - [`feng-cli.md`](../specifications/feng-cli.md)：定义 LSP Hover 的对外行为。
> - [`feng-mixin-draft.md`](feng-mixin-draft.md)：记录 mixin 的整体设计和实现过程。
> - [`feng-lsp-hover-optimize.md`](feng-lsp-hover-optimize.md)：记录通用 Hover 展示模型。

本文只描述 mixin 语法位置的 LSP Hover 优化、实现步骤和验收范围，不重新定义
`...`、字段展开或 `@mixable` wrapper 的语言语义。正式对外行为在实施前收敛到
`feng-cli.md`；若本文与正式规范冲突，以正式规范为准。

## 1. 背景

以下成员展开声明同时包含一个展开指令、一个显式来源类型和一个来源构造表达式：

```feng
...: LibView = LibView(9) {
    mutableValue: 12,
    label: "literal"
};
```

当前 Hover 存在两个问题：

1. 光标位于 `...` 时，LSP 可能把它识别成某一个生成字段或 wrapper，只显示该成员的
   单个签名。
2. 光标位于 `...` 右侧的显式类型或 `=` 后的来源构造表达式时，LSP 没有完整复用
   普通类型引用、调用表达式和对象字面量的 Hover 查询路径。

这些问题只属于 LSP AST 查询和展示，不改变 mixin 的编译语义、目标内存布局、代码
生成、运行时行为或二进制格式。

## 2. 当前原因

### 2.1 生成成员共享 `...` 的 token

语义展开生成的字段、静态 wrapper 和实例 wrapper 都使用对应
`FengTypeMixinDecl.token` 作为生成位置，并通过 `mixin_origin` 指回所属的展开指令。

当前 `find_decl_token_hit_member()` 会把普通成员的 token 当作声明位置。遍历目标最终
成员表时，第一个共享 `...` token 的生成成员会抢先命中，所以 Hover 的结果表现为
一个字段或方法，并可能受生成成员顺序影响。

生成成员在目标源码中没有独立的手写声明位置，因此不能再通过共享的 `...` token
参与普通成员声明命中。成员使用位置仍应通过现有成员访问解析，Definition 仍通过
`mixin_source_member` 返回来源声明。

### 2.2 普通 AST 查询没有遍历 mixin 声明

原始 mixin 声明保存在 `type_decl.mixins`，不属于 `type_decl.members`。现有以下查询主要
遍历普通成员，尚未完整遍历 `FengTypeMixinDecl.source_type` 和
`FengTypeMixinDecl.source_constructor`：

- 类型引用命中；
- 构造调用命中；
- 普通表达式命中；
- 对象字面量字段命中；
- 持久符号缓存对应的类型与表达式回退查询。

因此，右侧源码虽然已经存在于 AST 中，却没有进入与普通手写类型和表达式相同的
Hover 解析路径。

## 3. 目标与非目标

### 3.1 目标

1. Hover `...` 时，一次显示该指令最终实际生成的全部字段、静态 wrapper 和实例
   wrapper。
2. Hover 显式来源类型时，与普通手写类型引用的 Hover 完全一致。
3. Hover 来源构造表达式中的任意已有可查询位置时，与把同一表达式写在普通绑定初值
   中完全一致。
4. 同包、跨源码模块、外部 `.fb` 包、当前成功分析和持久符号缓存回退路径保持一致。
5. 不触发同步语义分析、构建或磁盘读取，不增加 Feng 程序的运行时开销。

### 3.2 非目标

本次优化不负责：

- 修改 `...` 或 `@mixable` 的语言语义；
- 改变生成成员集合、成员顺序、冲突规则或可见性；
- 为 `...` 发明新的 Feng 声明语法、标题或类型类别；
- 在 Hover 中解释 mixin 原理、构造阶段或 wrapper 调用链；
- 修改 `.ft`、`.fb`、运行时 ABI 或代码生成；
- 为语义分析失败的源码推测一个并不存在的最终展开结果。

## 4. `...` 的 Hover 展示

### 4.1 展示内容

光标位于 `...` token 时，Hover 的 Feng 代码块直接列出该指令最终实际生成的成员，
不添加标题、来源说明或其他文本。例如：

```feng
var mutableValue: int
let label: string
static func draw(target: Widget, area: Area): void
func draw(area: Area): void
```

该列表必须同时包含：

- 实际生成的实例字段；
- 实际生成的静态 wrapper；
- 实际生成的实例 wrapper。

静态 wrapper 必须显示 `static`，使其与删除第一个参数后生成的实例 wrapper 明确区分。
不得使用 `member mix: LibView` 等非 Feng 语法作为代码块内容，也不需要额外显示“来源：
LibView”等上下文提示；用户的光标已经位于明确的 `...` 上。

本次 Hover 只汇总成员签名，不聚合多个来源成员的文档注释。

### 4.2 数据来源

展示集合必须来自目标类型完成 mixin 展开后的最终成员表，并满足：

```text
member.mixin_origin == 当前 FengTypeMixinDecl
```

不能直接枚举来源类型的公开成员。只有目标最终成员表才能准确体现：

- 目标显式成员优先后实际跳过的字段和静态 wrapper；
- 目标泛型参数替换后的最终字段及方法签名；
- 可见 `fit` 提供并实际生成的 `@mixable` wrapper；
- 多层 mix 后归属于当前 `...` 的传递生成成员；
- 重载、可见性和后续普通成员规则处理后的实际结果。

只显示 `mixin_origin` 等于当前指令的成员，不显示目标显式成员、其他 `...` 生成的成员
或来源类型中未参与本次生成的成员。展示顺序与目标最终成员表中的顺序一致。

当没有可展示的实际生成成员时，不输出虚构签名或额外解释文本。

### 4.3 查询优先级

`...` 的复合 Hover 必须在普通生成成员声明命中之前执行。与此同时，普通成员声明命中
应忽略带有 `mixin_origin` 的生成成员 token，避免同一个 `...` 再次被识别为某一个
字段或 wrapper。

该规则还避免同文件中的来源成员名称被另一个类型的生成成员错误抢占。生成成员本身
没有目标源码声明标识符；其普通使用点仍由成员访问表达式解析，不依赖生成 token。

完整成员集合只从与当前文档匹配的成功语义分析读取。当前文本尚未成功分析时，不根据
来源表面成员自行推测冲突过滤、泛型替换或可见 `fit` 结果。

## 5. 右侧类型和构造表达式的 Hover

### 5.1 显式来源类型

以下 `LibView` 是用户书写的普通类型位置：

```feng
...: LibView;
...: LibView = LibView();
```

`source_type` 及其泛型实参必须进入现有类型引用查询，直接复用普通类型引用的目标解析、
签名格式化、文档注释和跨包 symbol provider。不得为 mixin 类型位置生成专用 Hover
文本。

### 5.2 来源构造表达式

`source_constructor` 的完整表达式树必须进入现有普通查询路径，包括：

- 构造目标及显式泛型实参；
- 构造函数调用；
- 构造参数中的标识符、成员访问和嵌套调用；
- 对象字面量字段名；
- 对象字面量字段值中的普通表达式；
- 已有字面量 Hover 支持的位置。

例如 `LibView(9)` 上的 Hover 结果必须与以下普通手写表达式中同一位置的结果一致：

```feng
let source = LibView(9) {
    mutableValue: 12,
    label: "literal"
};
```

“与手写一致”表示复用同一个解析和格式化实现，而不是分别维护两套输出相似的逻辑。

### 5.3 推导形式不能把合成类型当作源码类型位置

以下形式没有用户书写的 `source_type` 位置：

```feng
... = LibView(9);
```

语义阶段可以为后续展开合成 `source_type`，但 LSP 不得把该合成节点作为独立的类型
Hover 命中。否则合成类型可能先于调用表达式命中，使 `LibView(9)` 不再与普通构造
调用保持一致。

因此：

- 只有非推导形式查询显式 `source_type`；
- 推导形式中的构造目标只通过 `source_constructor` 查询。

### 5.4 对象字面量 owner 使用通用构造解析

对于 `LibView(9) { ... }`，对象字面量 target 是调用表达式。若普通对象字面量字段
Hover 只对标识符 target 调用 `resolve_expr_target()`，就无法得到构造结果类型。

该问题应在普通对象字面量 owner 解析中统一修复，复用现有构造目标或表达式静态类型
解析；mixin 只把 `source_constructor` 接入同一遍历。禁止增加只识别 mixin 来源表达式
的对象字面量特判。

## 6. 实现设计

### 6.1 `...` 复合 Hover

在 `src/cli/lsp/service.c` 中增加只读的 mixin Hover 查询，职责为：

1. 根据光标 offset 在当前 `type` 的 `type_decl.mixins` 中定位精确的 `...` token；
2. 在与当前文档匹配的成功分析中取得目标最终成员表；
3. 按 `mixin_origin` 精确筛选当前指令的生成成员；
4. 使用现有字段和方法签名格式化能力逐项输出；
5. 对静态 wrapper 补充 Feng 表层语法中的 `static` 标记；
6. 继续复用现有 Markdown/plaintext 协商和 JSON 转义。

该查询应在调用普通 `resolve_target_at()` 之前完成。无需把 mixin 指令伪装成普通成员，
也无需为 References、Rename 等符号操作创造可重命名目标。

### 6.2 排除生成成员的伪声明命中

调整普通 AST 成员声明命中条件：

- 手写成员继续按自身 token 和名称命中；
- `mixin_origin != NULL` 的生成成员不得按共享 token 命中；
- 成员访问表达式、Completion 和 Definition 的现有生成成员查询保持不变。

### 6.3 将 mixin 原始节点接入普通遍历

`FENG_DECL_TYPE` 的相关遍历需要在普通成员之外处理每个原始 mixin 声明：

- 类型引用遍历：处理非推导形式的 `source_type`，并处理
  `source_constructor` 中显式泛型实参等类型引用；
- 调用遍历：处理 `source_constructor` 中的构造调用和嵌套调用；
- 表达式遍历：处理 `source_constructor` 中的标识符、成员访问及嵌套表达式；
- 对象字段遍历：处理 `source_constructor` 中的对象字面量字段名；
- symbol/cache 类型查询：提供与 AST 成功分析路径一致的外部类型回退。

应复用现有递归函数，不复制表达式种类分派。

### 6.4 缓存与并发边界

`...` 的最终成员集合只读取现有 immutable published analysis，并继续受当前文档指纹和
最后一次成功分析规则约束。查询期间沿用现有 `analysis_mutex` 生命周期，不保存超出
session 生命周期的 `mixin_origin` 指针。

右侧显式类型和构造表达式属于原始 AST，可继续使用：

1. 与当前文档匹配的成功分析；
2. 当前文本 parse 回退；
3. 持久 symbol provider 回退。

外部来源类型通过已有 `.ft` provider 解析；本优化不要求 `.ft` 保存 consumer 源码中的
原始 mixin 指令，也不新增符号格式字段。

## 7. 性能与安全

本优化不改变编译产物和 Feng 程序运行路径，因此没有运行时性能开销。

Hover 请求的新增工作限于：

- 在当前类型的 mixin 声明中定位一次 token；
- 命中 `...` 时线性扫描一次目标最终成员表；
- 在普通 AST 查询中增加与 mixin 声明数量成正比的只读遍历。

不得为 Hover 触发同步项目分析、依赖加载、构建或磁盘 I/O。不得创建来源对象、执行
构造表达式或重新运行 mixin 展开。

## 8. 测试范围

新增 LSP 协议测试，不修改已有测试用例。至少覆盖：

1. Hover `...` 同时显示生成字段、静态 wrapper 和实例 wrapper。
2. 静态 wrapper 显示 `static`，实例 wrapper 删除第一个 `target` 参数。
3. 不出现额外标题、来源说明或 `member mix` 等伪 Feng 语法。
4. 私有字段、静态字段、未标注静态方法和普通实例方法不出现在展开 Hover 中。
5. 被目标显式成员规则跳过的字段或静态 wrapper 不出现在列表中。
6. 多个 `...` 分别只显示各自 `mixin_origin` 对应的最终成员。
7. 泛型来源显示替换后的目标字段及 wrapper 签名。
8. 多层 mix 和可见 `fit` 的生成成员归属正确。
9. `...: Source;` 和 `...: Source = Source();` 的显式类型 Hover 与普通类型位置一致。
10. `...: Source = Source();` 与 `... = Source();` 的构造调用 Hover 与普通绑定初值
    一致，推导形式不被合成 `source_type` 抢占。
11. 构造参数、对象字面量字段名和字段值中的标识符、成员访问、调用及字面量 Hover
    与普通手写表达式一致。
12. 同包、跨源码模块和外部 `.fb` 来源类型均可查询。
13. Markdown 与 plaintext 客户端得到内容等价的成员列表。
14. 生成成员的普通使用点 Hover 和 Definition 保持现有行为，`...` 不再误命中某一个
    生成成员。

所有非文档变更完成后，必须在 Codex 沙箱外执行 `make test` 全量回归。

## 9. 分步 TODO

### 9.1 规范

- [x] 在 `docs/specifications/feng-cli.md` 收敛 `...` 复合 Hover 以及右侧普通 Hover
  复用规则。

### 9.2 查询与展示

- [x] 增加 `...` token 的精确定位和复合 Hover 构建。
- [x] 按当前 `mixin_origin` 输出实际生成的字段、静态 wrapper 和实例 wrapper。
- [x] 静态 wrapper 签名显示 `static`，且不添加标题或来源说明。
- [x] 排除生成成员共享 token 对普通成员声明命中的干扰。

### 9.3 右侧普通 Hover 复用

- [x] 将显式 `source_type` 接入 AST 与 symbol/cache 类型引用查询。
- [x] 将 `source_constructor` 接入普通类型、调用、表达式和对象字段遍历。
- [x] 推导形式忽略仅由语义阶段合成的 `source_type` Hover 命中。
- [x] 在普通对象字面量 owner 解析中统一支持构造调用 target。

### 9.4 测试与回归

- [x] 新增第 8 节所列 LSP 协议测试。
- [x] 验证查询不触发同步分析、构建或磁盘 I/O。
- [x] 在 Codex 沙箱外执行 `make test` 全量回归。

## 10. 验收标准

全部满足以下条件后，本优化方可视为完成：

- Hover `...` 只显示该指令最终实际生成的全部字段、静态 wrapper 和实例 wrapper；
- 输出没有标题、来源说明或任何伪 Feng 语法，静态 wrapper 明确显示 `static`；
- 显式来源类型与普通类型位置的 Hover 相同；
- 来源构造表达式与普通手写构造表达式的 Hover 相同；
- 推导形式不会因合成 `source_type` 改变构造目标的 Hover；
- 构造调用后的对象字面量字段使用通用 owner 解析，不存在 mixin 专用特判；
- 当前项目、跨模块和外部包查询结果一致；
- 生成成员普通使用点的 Hover、Completion 和 Definition 不回退；
- 无编译产物、ABI 或运行时行为变化；
- 新增测试及 `make test` 全量回归通过。
