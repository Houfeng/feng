# G25 用例与验收证据

## 1 范围与断言口径

本文记录 [G25 实施文档](./feng-language-conformance-coverage-hardening-g25.md) 的反向证据，
不另行定义语言规则。下表“主规范”默认指
[泛型主规范](../specifications/feng-generics-draft.md)；错误码以
[AE 主规范](../specifications/feng-error-codes-ae.md) 为准。

- `S`：[test/semantic/test_g25.c](../../test/semantic/test_g25.c)。表中前缀匹配该文件的
  case ID；`g25_check_source` 对非法源码检查完整、无序诊断集合的码、文件、token 字节与长度、
  行列、数量和 Semantic 失败，正例要求完整分析成功且零诊断。不是仅检查消息或数量下限。
- `P`：[test/parser/test_g25.c](../../test/parser/test_g25.c)。检查源码的 token 消费、AST
  类型层级与后缀归属；不把 Parser 通过当作 Semantic 或运行行为通过。
- `C`：[test/codegen/test_g25.c](../../test/codegen/test_g25.c)。检查真实源码产生的 C 可编译，
  非法闭合 pointee 保持 `CE0033`；public／workspace 两种 FT profile 均在释放 provider AST
  后加载 consumer。约束反例检查 Semantic 单码与 token，完整位置证据由 `S` 提供。
- `F`：[test_g25_generic_boundaries.ff](../../fcts/fcts_bin/src/test_g25_generic_boundaries.ff)。
  新增 10 个真实行为测试，统一由主入口登记；跨包声明在
  [lib_g25_generic_boundaries.ff](../../fcts/fcts_lib/src/test/lib_g25_generic_boundaries.ff)。
- `G`：[既有 test_generic.ff](../../fcts/fcts_bin/src/test_generic.ff)。仅引用有可观察结果的
  既有用例，不以其中的 `assert(true)` 关闭行为交付项。

## 2 GENERIC01～06 反向矩阵

### GENERIC01：类型参数名称与声明 identity

| 项目 | 主规范条款／稳定码 | 具体证据与断言 |
|---|---|---|
| 01-A | §4～6 参数名称唯一；AE1017 | `S:01A-*` 覆盖 type、func、object/callable spec、type/fit 方法；重复列表在第二个名称单码失败，不同名配对零诊断。 |
| 01-B | §4～6 owner／方法名称域；AE1017 | `S:01B-shadow/distinct`；`F` 的 owner／方法测试用 string 与 int 区分参数槽。 |
| 01-C | §4～6 特殊成员限制；AE0316 | `S:01C-constructor/finalizer/owner`：两个方法级泛参反例与只使用 owner 参数的构造／终结器正例。 |
| 01-D | spec 主规范 §5；AE0331 | 既有 [g23_declaration_diagnostics](../../test/semantic/test_g23.c) 的实例／静态方法泛参两例精确诊断；`S:01A-distinct-spec` 与 `F` 泛型 spec owner 运行配对。 |
| 01-E | §4～6 type/spec identity；AE0213 | `S:01E-*`：改参数名或约束仍重复；`S:02D-*` 与 `F` 两种声明顺序的 type/spec arity 选择配对。 |
| 01-F | §4 category 引用；AE0004 | `S:01F-category`：type 与 spec 即使 arity 不同仍发生 name-only 冲突。 |
| 01-G | §4～6 callable 签名；AE0508/0801，AE0509/0510/0802/0803 | `S:01G-*` 覆盖顶层、type/fit 方法的泛参重命名、约束变化、嵌套 alpha-equivalence、返回与变参冲突；不同槽与 arity 正例由 `S`、`C`、`F` 配对。 |
| 01-H | §6 声明期检查；同 01-A～G | `S:01A-*`、`01E-*`、`01G-*` 非法程序无需调用即有唯一声明诊断；不以调用二义性关闭声明检查。 |

### GENERIC02：类型实参数量与精确目标解析

| 项目 | 主规范条款／稳定码 | 具体证据与断言 |
|---|---|---|
| 02-A | §4～6 精确类型 arity；AE1015 | `S:02A-*`：字段、参数、返回、父 spec、type 契约、fit target、嵌套与数组；真实双模块的 alias/qualified 反例和合法引用。 |
| 02-B | §4～6 构造目标；AE1015 | `S:02B-constructor-few/many`：定位构造目标且无 AE0313/0315 次生诊断；`G` 的 generic type constructor 返回字段 99。 |
| 02-C | §4～6 显式 callable；AE1015 | `S:02C-*`：顶层、实例、静态、fit 实例/静态和真实模块函数的过少／过多类型实参。 |
| 02-D | §4～6 精确 identity；AE1015 | `S:02D-*` 对两个声明顺序核对可用 arity 消息；`19-spec-arity-*` 覆盖 direct/fit/parent 关系；`F` 连续使用 type/spec/callable 两种 arity 并比较结果 10/11 或 1/2；`C` 验证不同 arity/槽的 C 名称不碰撞及跨包消费。 |
| 02-E | §4～6 约束引用；AE1015 | `S:02E-constraint-arity`：约束自身 arity 错误只报 AE1015，不追加非 spec 或不满足诊断。 |
| 02-F | §6 与 AE 诊断口径 | `S` 的通用完整集合检查覆盖上述反例；`F` 的 exact type/callable、exact spec 两项用结果而非编译成功证明选路。 |

### GENERIC03：约束声明、满足与候选筛选

| 项目 | 主规范条款／稳定码 | 具体证据与断言 |
|---|---|---|
| 03-A | §4～6 约束解析；AE1013 | `S:03A-unknown`：未知约束只有名称根因，无 AE0709。 |
| 03-B | §4～6 spec-only；AE0709 | `S:03B-*`：object、tuple、enum、builtin、array、pointer 六类非 spec；G20 两条精确断言仅按批准迁移错误码。 |
| 03-C | §4～6 owner 约束；AE0710 | `S:03C-*`：type/object spec/callable spec owner 与开放 callable 约束；`20-callable-owner-compatible` 正例。既有 test_fit_enum_satisfies_generic_constraint 验证直接 fit 后零诊断；下方 spec owner／method constraint FCTS 核对具体 owner、父约束和 fit 支持的闭合调用结果。 |
| 03-D | §4～6 开放参数转传；AE0710 | `S:03D-none/weaker/unrelated/parent-none` 拒绝；`same/strong` 成功。`G` 的 generic parent constraint forwarding 返回 item，child spec 的推导／显式／传递父结果分别可观察。 |
| 03-E | §4～6 候选约束；AE0512 | `S:03E-*` 及 `20-callable-constraint-mismatch`；`F` unsatisfied constraints 测试分别选中返回 2 的另一泛型候选与返回 73 的受约束候选；`G` 的 constraint unsatisfied 测试选中 non-generic。 |
| 03-F | §4～6 约束体能力；AE0306/1008/0512 | `S:03F-*` 对字段、实例／静态成员、参数不匹配、无约束和 callable 调用分别配对；`G` 的 generic constraint witness field 真实改名并调用方法，静态 requirement 由下方既有 factory 测试验证，callable requirement 由下方 wide constraint 测试验证。 |
| 03-G | 复合类型主规范 | 直接引用 [G24 COMPOSITE23～28、34～35](./feng-language-conformance-coverage-g24-evidence.md) 的逐项证据；G25 不复制复合关系矩阵。 |
| 03-H | §4～6 不变性；AE1003/0512/0522/1023 | `S:03H-*`：Box 的双向赋值、调用、cast；Producer/Consumer 的返回／参数方向与相同实例正例；拒绝以父子契约合并不同泛型实参。 |
| 03-I | §4 完整指针实参准入；各消费入口既有码 | `S:g25_pointer_constraint_diagnostics` 的 14 个类型配对 × 11 个入口，共 154 例；`C` 两种持久化 FT profile；`F` 本包／跨包 union 显式指针成员的推导、显式调用保持同一指针。约束实现未增加统一禁指针分支。 |
| 03-J | §4 开放 pointee；AE0333 | `S:g25_open_pointer_diagnostics` 共 240 例，包含 12 种组合 × 13 个声明位置、1～64 层连续深度及消费／独立错误边界。`Box<T*>[1]` 保留 AE0333+AE1016；`Box<T*>[:1]` 只有 AE0333；`Box<int>[1]` 只有 AE1016；`Box<int*>[:1]` 成功。`C` 和 `F` 的合法闭合指针整体作为 T 仍通过。 |

### GENERIC04：类型参数推导

| 项目 | 主规范条款／稳定码 | 具体证据与断言 |
|---|---|---|
| 04-A | §4 递归推导；不匹配 AE0512 | `S:04A-*` 覆盖多实参、整槽、重复槽、数组层级／可写性、具名 identity、开放转传与八层嵌套；`04E-*` 深层冲突配对；`P` 验证相邻结束符不改变 AST。`F` 前四项分别检查 41、51/52、61、原指针及共享可写数组的 83/82。 |
| 04-B | §4 owner／方法推导 | `S:04A-owner-method/static/fit`；`F` instance static and fit methods 测试保持 owner 字符串、六种显式／推导调用均返回 91，不串方法 int 槽。 |
| 04-C | §4 目标类型推导；AE0525 | `S:04C-missing/target`；既有 factory target inference 的绑定目标测试调用 provider 并检查字段和构造次数。 |
| 04-D | §4 全部参数闭合；AE0525 | `S:04D-partial/middle/explicit`：定位调用目标，消息锁定首个缺失槽索引，显式补齐成功。 |
| 04-E | §4 绑定一致性；AE0512 | `S:04E-*` 覆盖直接、深层、跨层重复槽及方法；冲突无候选拒绝，有 fallback 正常。`F` conflicting nested sources 的不同类型返回 2，相同类型返回 1。 |
| 04-F | §4～6 重载优先级；AE0511 | `S:04F-ambiguous/deep-ambiguous`；`G` overload: non-generic exact match preferred over generic 断言 non-generic-exact。 |
| 04-G | §4～6 callable 来源需显式闭合；AE0522/1023 | `S:04G-*`；既有 callable value reification 的 top-level 与 explicit generic instance method values 用赋值/cast 后调用结果配对；`F` 最后一项核对真实别名导入来源的两种 callable 值均返回 101。 |
| 04-H | §4～6 裸 owner arity 0；AE0315/0512 | `S:04H-bare-constructor/bare-static`；`G` arity overload: bare constructor selects arity zero regardless of declaration order 的构造与静态结果均为 2。 |

### GENERIC05：显式泛型 target 合法性

| 项目 | 主规范条款／稳定码 | 具体证据与断言 |
|---|---|---|
| 05-A | §4～6 类型参数；AE1012 | `S:05A-type-param/construct-param`：类型位置和构造消费均定位参数名。 |
| 05-B | §4～6 类型实参解析；AE1013 | `S:05B-*` 的普通／嵌套实参、callable value 与 cast，单根因不追加调用或目标不兼容。 |
| 05-C | §4～6 非泛型目标；AE1014 | `S:05C-*` 覆盖 type、构造、顶层、实例、静态、fit、真实模块函数。 |
| 05-D | §4～6 target 消费；AE1016 | `S:05D-expression/binding/postfix`；开放指针矩阵另保留独立的非法索引错误，不按已有诊断数量跳过索引验证。 |
| 05-E | §4～6 类型位置裸名；AE0006 | `S:05E-bare-type/bare-spec`，与显式 target 错误分开。 |
| 05-F | §4～6 合法消费形式 | `G` 的 generic function、generic type constructor、generic object literal、generic array creation；`F` 顶层／模块、实例／静态／fit、正确 `[:1]` 创建和 callable 赋值/cast 均有结果断言；`P` 仅补语法与 AST 证据。 |

### GENERIC06：最小合法邻界行为

| 项目 | 主规范条款 | 具体证据与断言 |
|---|---|---|
| 06-A | §4～6 声明与 identity | `F` owner／方法、exact type/callable 和 exact spec；`G` generic spec、generic type with generic method、arity overload 的两种泛型类型分别调用闭合成员并核对结果。 |
| 06-B | §4 推导与显式调用 | `F` ordinary arrays、nested named arrays、instance static and fit methods：显式／推导结果一致，receiver 与 owner 字段值未改变。 |
| 06-C | §4 约束使用 | `G` generic constraint、generic constraint witness field、generic parent constraint forwarding；既有 callable generic constraint maps a wide aggregate value；union/intersection 沿用 03-G 引用。 |
| 06-D | §4～6 callable 值 | 既有 top-level callable values close scalar and explicit casts 的四个结果为 40/41/42/43；`F` imported nested inference and explicit callable values agree 的赋值、cast 均返回 101。 |
| 06-E | §4～6 arity 与声明顺序 | `F` exact type and callable arity is independent of declaration order 与 exact spec arity is independent of declaration order：每种顺序连续使用两种 identity，分别断言 10/11 和 1/2。 |
| 06-F | 实施文档 §4.6 | `F` 共 10 个 test，主入口登记一次，执行日志中十项均出现且通过；所有新增 test 均执行具体结果断言，没有条件分支绕开断言。 |

## 3 既有行为证据索引

仅引用相关测试，不修改它们的输入或期望：

- [test_generic_spec_owner_constraint.ff](../../fcts/fcts_bin/src/test_generic_spec_owner_constraint.ff)：
  `same-package object form forwards strong constraints`、
  `package object constraint metadata survives FT recovery` 核对 owner 的父约束与完整字段；
  `package callable owner constraint closes normally` 调用闭合 callback 返回 package-callable。
- [test_generic_owner_method_constraint.ff](../../fcts/fcts_bin/src/test_generic_owner_method_constraint.ff)：
  `same-package generic type and fit owners` 同时使用 type 直接声明和 fit 声明的契约，
  推导／显式调用返回 11/12、静态调用返回 13/14、方法值返回 15/16，嵌套 owner 返回 17/18；
  `package generic type and fit owners` 保留相应跨包断言。
- [test_generic_factory_target_inference.ff](../../fcts/fcts_bin/src/test_generic_factory_target_inference.ff)：
  `binding target infers imported top-level type arguments` 核对 42、宽值字段和构造次数；
  `binding target infers imported method type arguments` 核对 51/52 与各一次构造；
  `local and imported contracts share static member semantics` 核对 10/11 与返回值字段。
- [test_reified_value_callable.ff](../../fcts/fcts_bin/src/test_reified_value_callable.ff)：
  `callable generic constraint maps a wide aggregate value` 在泛型体内真实调用约束对象，
  返回的 number 为 53 且字符串字段与 marker 正确；
  `callable-form generic constraint preserves its concrete value` 返回 57。
- [test_generic_callable_value_reification.ff](../../fcts/fcts_bin/src/test_generic_callable_value_reification.ff)：
  `top-level callable values close scalar and explicit casts`、
  `explicit generic instance method values close through targets`、
  `explicit generic fit method forms a callable value` 等保留全部既有结果断言。

## 4 验收执行记录

- 完整 Semantic 专项 `g25-open-pointer-corrected-semantic.log`：退出码 0；G25 的 168 个基础
  源码、7 个独立诊断、7 个真实双模块、154 个指针约束、240 个开放指针边界全部通过，共 576 例。
- `g25-array-new-fcts-ubsan.log`：补齐 `std.collections` 导入后，UBSan FCTS 1374/1374
  通过，失败 0、跳过 0，包含 `F` 的十项。记录关联 ISSUE-G25-029～030。
- 2026-09-10 最终在沙箱外独立执行 `make test`，日志 `g25-delivery-regression.log`，
  退出码 0。两阶段均从根构建目录清理后重新构建，未跳过失败用例。

| 套件／门禁 | UBSan 阶段 | 常规阶段 |
|---|---|---|
| Archive、Lexer、Parser、Semantic、Runtime | 全部通过 | 全部通过 |
| Codegen、Debug、CLI、CLI Paths、Symbol | 全部通过 | 全部通过 |
| G25 Semantic 完整源码矩阵（包含在 Semantic 中） | 576/576 | 576/576 |
| Smoke | 91/91 | 91/91 |
| 标准库 | 604/604，失败 0、跳过 0 | 604/604，失败 0、跳过 0 |
| FCTS（含 G25 新增 10 项） | 1374/1374，失败 0、跳过 0 | 1374/1374，失败 0、跳过 0 |
| CLI 直接／项目模式、init 内置包 | 全部通过 | 全部通过 |
| 性能约束 | 全部通过 | 全部通过 |
| 增量构建、发布／安装／回滚、macOS 最终处理、内置包发布、工具链预构建 | Makefile 不在此阶段运行 | 全部通过 |

原生 Debug 测试成功时不打印汇总，以该命令正常退出且后续命令完成为依据。
CLI 负向用例的预期错误输出不表示套件失败。macOS 的标准 sanitizer 目标只运行 UBSan，
本次不声称运行了 ASan。

最终核对：`src/` 无 AE0232／AE0233／AE0304 发码；既有测试修改严格限于 G25 §2.3 批准项。
产品变更仅涉及 Parser、Semantic 与 callable 符号编码，没有 runtime、公开／私有 ABI、
`.ft` 格式或新增运行时操作；provider／consumer 的生成符号需通过重建同步。
`git diff --check` 通过。全量通过后只更新本组验收文档，不自动暂存或提交。
