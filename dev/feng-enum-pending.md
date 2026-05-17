# Feng enum 待开发项

> 本文档用于拆解 enum 从现有规范进入实现的施工顺序、边界与验收口径。
> [docs/feng-enum.md](../docs/feng-enum.md) 是 enum 的专项规范；本文只写开发步骤与 TODO，不重复规范定义。

## 1. 当前前提

- enum 公开语义已经收敛到 [docs/feng-enum.md](../docs/feng-enum.md)，当前阶段只支持简单的 int enum：无关联值、无字段、无方法、无泛型。
- 当前规则已明确：只允许“全隐式取值”或“全显式取值”，禁止混合取值。
- lexer 侧已经把 `enum` 识别为关键字；首版实现不需要新增关键字扫描逻辑，但需要补回归测试。
- parser / AST 目前没有 enum 顶层声明种类；`FengDeclKind` 只有 binding / type / spec / fit / function。
- symbol table 目前没有 enum 声明种类；包表导入/导出、provider 查询视图和 LSP 仍只围绕 `type` / `spec` / `fit` / `fn` 等已支持声明。
- 现有值模型只有 `TRIVIAL` / `MANAGED_POINTER` / `AGGREGATE` 三类；首版 enum 应作为“具名的 `int` 标量”进入 `TRIVIAL` 路径，不新增 runtime 值模型或专用 runtime API。
- ABI 侧已收敛：enum 在 ABI 边界视为与 `int` 相同的 ABI 标量，可直接进入 `extern fn`、顶层 `@abi fn`、callable-form `@abi spec` 与 `@abi type` 字段位置。
- 现有测试入口已经具备分层落地条件：`test_lexer`、`test_parser`、`test_semantic`、`test_codegen`、`test_symbol`、`test_cli` 以及最终 `make test`。

## 2. 分步 TODO

### 2.1 Parser / AST

- [ ] 在 `src/parser/parser.h` 中新增 enum 顶层声明种类，例如 `FENG_DECL_ENUM`。
- [ ] 为 enum 增加独立 AST 载体，例如 `enum_items` 与“是否显式赋值/显式字面量值”记录；不要复用 `type_decl.members`。
- [ ] 在 `src/parser/parser.c` 中为 `parse_declaration` 增加 `enum` 入口。
- [ ] 解析 `enum Name { Item, Item2 }` 与 `enum Name { Item = 1, Item2 = 2 }`。
- [ ] `=` 右侧在 parser 阶段只接受整数字面量，不接受一般表达式。
- [ ] 在 parser 阶段拒绝 enum 块体中的字段、方法、构造函数等非枚举项成员。
- [ ] 更新 AST dump / free 路径，确保 enum 不泄漏、调试输出可读。

验收口径：

- parser 能稳定区分 `type` / `spec` / `fit` / `fn` / `enum`。
- 合法的“全隐式”与“全显式” enum 能成功建树。
- 非法的成员块体与 `= 1 + 2` 这类非字面量初始化在 parser 阶段直接报错。

建议验证：

- [ ] 新增 `test/parser/test_parser.c` 用例后，运行 `make build/bin/test_parser && build/bin/test_parser`。
- [ ] 补一个 `test/lexer/test_lexer.c` 回归，确认 `enum` 仍是关键字 token。

### 2.2 语义建模与值归一化

- [ ] 在语义层为 enum 建立独立声明语义，进入模块级命名空间与重复声明检查。
- [ ] 把 enum 类型建模为“独立命名类型，但底层存储固定为 `int`”。
- [ ] 为 enum 项建立稳定的声明顺序与底层值归一化结果。
- [ ] 对“全隐式” enum，按声明顺序计算 `0, 1, 2, ...`。
- [ ] 对“全显式” enum，直接采用字面量值。
- [ ] 拒绝同一 enum 内混用显式赋值与隐式取值。
- [ ] 拒绝重复枚举项名称。
- [ ] 拒绝重复底层值。
- [ ] 记录第一个枚举项，供默认值与后续 codegen 使用。
- [ ] 在 `src/semantic/value_kind.c` 或等价辅助层把 enum 归类为 `FENG_SEMANTIC_VALUE_TRIVIAL`。

验收口径：

- enum 在语义层是独立类型，不被当成普通 `type` 或 `int` 别名直接混用。
- 归一化后每个枚举项都有唯一、稳定的底层 `int` 值。
- 默认值所依赖的“首个枚举项”可以被稳定查询。

建议验证：

- [ ] 新增 `test/semantic/test_semantic.c` 用例覆盖：全隐式、全显式、混用报错、重复名称、重复值。
- [ ] 运行 `make build/bin/test_semantic && build/bin/test_semantic`。

### 2.3 类型系统、表达式与默认值

- [ ] 让类型引用解析能把 enum 名称解析为合法类型目标。
- [ ] 实现 `EnumName.ItemName` 的语义解析，保证它解析为枚举项值，而不是普通类型成员访问。
- [ ] 无初始值的 enum 绑定默认取第一个枚举项。
- [ ] 允许 `enum -> int` 的显式转换。
- [ ] 禁止 `int -> enum` 的显式转换。
- [ ] 禁止 `enum` 与 `int` 之间的隐式转换。
- [ ] 仅允许同一 enum 类型之间直接做 `==` / `!=`。
- [ ] 禁止不同 enum 之间直接赋值、比较与转换。
- [ ] 禁止 enum 直接参与算术与顺序比较；需要时必须先显式转换为 `int`。
- [ ] 让 enum 能出现在变量、参数、返回值、成员、数组元素等普通类型位置，并沿用 trivial 值复制路径。
- [ ] 让 enum 能直接出现在 `extern fn`、顶层 `@abi fn`、callable-form `@abi spec` 与 `@abi type` 字段位置，并按 `int` ABI 标量规则校验。
- [ ] 若一元 `&` 已支持基础标量取址，则补 enum 取址规则，使 `&enum_value` 的 ABI 行为与 `&int_value` 一致。

验收口径：

- `let x: Status = Status.Ok;` 这类基本写法通过。
- `let x: int = (int)Status.Ok;` 通过；`let x: Status = (Status)1;` 报错。
- `Status.Ok == Status.NotFound` 合法；`Status.Ok < Status.NotFound` 非法，除非先转成 `int`。
- `extern fn use_status(s: Status): void;`、`@abi fn export_status(): Status` 与 `@abi type Box { var status: Status; }` 这类 ABI surface 合法并按 `int` 标量处理。

建议验证：

- [ ] 在 `test/semantic/test_semantic.c` 中新增类型检查、默认值、转换、比较、赋值用例。
- [ ] 复跑 `make build/bin/test_semantic && build/bin/test_semantic`。

### 2.4 Codegen

- [ ] 为 enum 选择稳定的 C 落地表示。
- [ ] 不要把 Feng enum 的值存储直接依赖到 C 原生 `enum` 类型宽度；首版应固定走 `int32_t` / Feng `int` 对应的稳定表示。
- [ ] 为 enum 声明生成稳定的 C 名称与枚举项常量符号。
- [ ] 为 `EnumName.ItemName` 发出对应常量值。
- [ ] 为 enum 默认值发出“首个枚举项底层值”。
- [ ] 让 enum 在局部变量、参数、返回值、数组、对象字段中都走 trivial copy 路径。
- [ ] 复核 enum 到 `int` 的显式转换发码，避免引入多余 runtime 调用。
- [ ] 在 ABI surface 上把 enum 按 `int32_t` / Feng `int` 的固定标量表示发码，不单独生成另一套 ABI layout。
- [ ] 复核 enum 进入 `extern fn`、顶层 `@abi fn`、callable-form `@abi spec` 与 `@abi type` 字段位置时的 C surface 一致性。

验收口径：

- enum 发码不新增 runtime 生命周期分支。
- 生成的 C 表示对目标平台保持固定 32 位有符号整型语义，不依赖编译器的 C enum 实现细节。
- enum 在复制、返回、数组存储上与 `int` 具有相同的 trivial 行为。
- enum 在 ABI 参数、返回值、函数签名与 `@abi type` 字段位置上与 `int` 使用同一 C surface。

建议验证：

- [ ] 新增 `test/codegen/test_codegen.c` 用例，覆盖 enum 声明、枚举项引用、默认值、显式 cast。
- [ ] 运行 `make build/bin/test_codegen && build/bin/test_codegen`。
- [ ] 增加一个最小 CLI 直编/运行用例，确认 end-to-end 发码通过。

### 2.5 Symbol Table / 包导出 / 导入查询 / LSP

- [ ] 在 `src/symbol/symbol.h` 中为 enum 增加声明种类；若需要，也为 enum item 增加独立声明种类或等价的稳定属性表达。
- [ ] 在 `src/symbol/ft_write.c` / `ft_read.c` / `export.c` / `provider.c` 中补齐 enum 的导出、读取与查询视图接入。
- [ ] 导出 enum 的名称、可见性、声明顺序、各枚举项名称及其底层值。
- [ ] 让跨模块 / 跨包 `use` 后的 enum 类型引用与枚举项访问可被正确解析。
- [ ] 把 enum 纳入 imported-module 的重名冲突检查与可见性查询。
- [ ] 让 LSP completion / hover / definition 至少能识别 enum 声明与 `Enum.` 后的枚举项候选。
- [ ] 若 `.ft` 格式需要新增 decl kind / attr kind / relation kind，先回写 `docs/feng-symbol-table.md`，再落代码。

验收口径：

- 当前项目源码模块中的 enum 可以跨文件、跨模块引用。
- `.fb` 公开包中的 enum 可以被 consumer 读取并参与语义分析。
- `Enum.` 位置至少能返回稳定的枚举项候选，不退化为普通成员猜测。

建议验证：

- [ ] 新增 `test/symbol/test_symbol.c` 用例覆盖导出/导入 enum 与枚举项值读取。
- [ ] 新增 `test/semantic/test_semantic.c` 跨模块 / 跨导入用例。
- [ ] 运行 `make build/bin/test_symbol && build/bin/test_symbol`。
- [ ] 运行 `make build/bin/test_cli && build/bin/test_cli` 或补对应 CLI 项目级用例。

### 2.6 回归与收尾

- [ ] 复核所有涉及“顶层声明种类”“公开声明查询视图”“类型引用”的实现分支，补上 enum。
- [ ] 复核 CLI parse / semantic / build / check 的诊断文本，确保 enum 错误信息清晰、不退化成“unknown type”。
- [ ] 若实现过程中发现规范仍有缺口，先回写 `docs/feng-enum.md` 或相关权威文档，再继续编码。
- [ ] 每个阶段都以“先文档、后代码、再测试”的顺序落地，不修改已有测试语义，只新增覆盖。
- [ ] 阶段完成后执行全量回归。

验收口径：

- 窄测试先通过，再执行全量 `make test` 通过。
- enum 功能进入主线后，不引入新的 runtime API 和额外值模型分支。

建议验证：

- [ ] 阶段性执行对应窄测试。
- [ ] 最终执行 `make test`。

## 3. 当前明确不做

- [ ] 不支持关联值、payload、字段、方法、构造函数、终结器。
- [ ] 不支持 enum 泛型。
- [ ] 不支持显式值与隐式值混用。
- [ ] 不支持 `int -> enum` 显式转换或任何 `enum <-> int` 隐式转换。
- [ ] 不引入新的 runtime 对象表示、runtime API 或额外值模型分类。

## 4. 建议执行顺序

1. 先完成 Parser / AST，并补 `test_parser` 与 lexer 回归。
2. 再完成语义层的值归一化、默认值、转换和比较规则，并补 `test_semantic`。
3. 再完成 codegen，把 enum 固定落到 trivial `int32_t` 路径，并补 `test_codegen` / 最小 CLI 端到端用例。
4. 最后补 symbol table、包导入导出与 LSP / CLI 查询视图，并补 `test_symbol` / `test_cli`。
5. 每一阶段稳定后都执行全量 `make test`。

## 5. 交付约束

- 所有实现必须以 [docs/feng-enum.md](../docs/feng-enum.md) 为准，不得在编码阶段临时放宽为“类 C 混合取值”。
- enum 首版必须保持“具名的 int 标量”定位，不得偷渡成托管对象、fat value 或 ABI 特判对象。
- 若符号表格式、导入查询模型或 LSP 展示需要新事实，先更新对应文档，再进入实现。
- 若后续要支持 payload enum、位标志语义或可配置底层类型，必须另开规范，不在本待开发项中顺手扩展。
