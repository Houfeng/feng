# Feng import / 类型引用优化实施记录

## 1 文档定位

- 模块名称访问的行为规则只在 [Feng 语言模块规范](../specifications/feng-module.md#4-模块导入规则) 中定义；本文仅记录类型引用链路的历史实施结果。
- 本文聚焦类型位置，不重复定义普通表达式成员访问、lambda 语法或 LSP 补全行为。
- 源码语法使用 `import`；编译器内部保留的 `use` 命名属于实现细节，不代表另一种语言语法。

## 2 实施范围

本轮历史实施覆盖命名类型及其数组形式在三种模块名称访问入口下的 Parser、Semantic 和 Codegen 链路：

- 无别名 import 后的短名类型；
- import alias 限定类型；
- 无需 import 的完整模块路径类型。

短名和 alias 都必须由当前文件的 import 建立；完整模块路径不依赖 import。该约束来自模块主规范，本文不另行定义。

## 3 实施结果

### 3.1 Parser

`src/parser/parser.c` 的命名类型解析接受多段路径，因此短名、alias 限定名和完整模块路径在语法层均可形成类型引用。

### 3.2 Semantic

`src/semantic/analyzer.c` 按当前文件可见短名、当前文件 import alias 或公开模块完整路径解析命名类型。完整路径指向外部包时，分析预注入阶段会按类型引用加载目标公开模块。

### 3.3 Codegen

`src/codegen/codegen.c` 已按 Semantic 分析得到的模块和声明身份处理多段命名类型，不再保留“仅支持单段类型名”的限制。数组类型沿用其元素类型的同一解析结果。

### 3.4 端到端结论

以下类型引用已经端到端支持：

- `Type`；
- `alias.Type` 与 `alias.Type[]`；
- `module.path.Type` 与 `module.path.Type[]`。

对应实现与测试由 commit `da6b4fb8`（`feat: support full-path reference type & module alias reference type`）交付。

## 4 测试证据

- `test/semantic/test_semantic.c` 锁定短名、alias 限定名、无需 import 的完整模块路径及未导入短名等规则边界；
- `test/codegen/test_codegen.c` 锁定 alias 和完整模块路径类型及其数组形式能够生成并编译 C；
- G02 模块名称解析语义进一步在 FCTS 中分别验证 `type`、`enum`、`spec`、顶层 `func`、模块级 `let` / `var` 的三种名称访问入口。

## 5 当前状态

类型引用优化已完成。后续若模块名称访问规则发生变化，应先修改模块主规范，再分别更新实现、compiler tests 与 FCTS；本文只同步实施状态，不建立第二份行为定义。
