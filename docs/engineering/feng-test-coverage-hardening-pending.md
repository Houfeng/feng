# Feng 测试覆盖补齐计划

> 状态：待 Review，尚未实施
>
> 盘点基线：2026-08-24

## 1 目标

本文基于当前规范、工程文档和实际测试代码，规划下一轮可独立验收、可分步交付的测试补齐工作。
本文只规划测试证据，不定义或改变语言、工具链、包格式、ABI 或运行时语义。

权威行为分别以以下主规范为准：

- [表达式与运算规范](../specifications/feng-expression.md)
- [函数规范](../specifications/feng-function.md)
- [模块规范](../specifications/feng-module.md)
- [ABI 互操作规范](../specifications/feng-interop.md)
- [对象生命周期规范](../specifications/feng-lifetime.md)
- [编译器错误码](../specifications/feng-error-codes.md)
- [包分发规范](../specifications/feng-package.md)
- [符号表规范](../specifications/feng-symbol-table.md)

## 2 测试归属原则

### 2.1 优先使用 FCTS

以下用例必须优先放入 `fcts/`：

- 合法 Feng 程序的语法、语义和可观察运行结果；
- 同包、跨模块及现有 `fcts_lib -> fcts_bin` 两包结构能够表达的跨包行为；
- 仅以“程序成功编译并运行”即可证明的合法能力；
- 不依赖测试进程环境控制、损坏制品、内部 AST/Codegen 结构或额外原生夹具的行为。

成功行为不得因为在 compiler test 中较易构造而降级为只做 C 单元测试。compiler test 可以补充阶段、
诊断或结构证据，但不能替代可由 FCTS 直接观察的语言行为。

### 2.2 不适合 FCTS 的用例放入 `test/`

| 测试目标 | 目录 | 原因 |
| --- | --- | --- |
| token 分类与词法错误码 | `test/lexer/` | 需要直接检查 token 和 `LE` 诊断 |
| 语法拒绝、诊断位置与 AST 结构 | `test/parser/` | 非法程序不能进入 FCTS；AST 不可由 Feng 程序观察 |
| 类型拒绝、名称决议、精确 `AE` 诊断 | `test/semantic/` | FCTS 不承载预期编译失败程序 |
| C 输出、ABI wrapper、descriptor、witness、无额外间接层 | `test/codegen/` | 属于实现结构和性能约束，不是语言输出 |
| ARC/循环回收算法和并发内部状态 | `test/runtime/` | 需要直接控制阈值、描述符和引用计数 |
| `.ft` 二进制读写与损坏输入 | `test/symbol/` | 需要直接构造内部制品 |
| ZIP/`.fb` 路径与损坏输入 | `test/archive/` | 需要构造恶意或不完整归档 |
| 多包、多平台、原生 C 夹具和进程环境 | `test/cli/` | 超出现有 FCTS 两包及单进程边界 |

`test/smoke/` 保留已有回归。本轮若一个新成功行为能够进入 FCTS，不再新增等价 smoke；只有必须比对
独立进程 stdout、exit code、动态链接或宿主 C ABI 的场景才使用 `test/` 下的进程级测试。

### 2.3 用例约束

- 不修改已有用例的语义和断言；每个补齐项新增独立测试入口或独立表驱动 case。
- 一个用例只证明一个主要缺口，避免前置错误遮蔽目标诊断。
- 以非等价语义、lowering、ABI 或制品校验分支为单位，不做类型、参数数量和嵌套深度的笛卡尔积。
- 若新增用例暴露产品缺陷，当前步骤立即停止；先记录最小复现、规范依据、根因方向、ABI/性能影响，
  经人工决定后另行修复，不删除或弱化合法用例绕过问题。
- 若修复需要增加 runtime ABI、运行时操作、分配、间接层或查找，必须先由人工决策。

## 3 当前覆盖基线与结论

### 3.1 基线事实

按当前源码静态盘点：

- FCTS 共有 898 个 `test(...)` 块，入口覆盖基础类型、表达式、流程、函数、异常、defer、tuple、
  value type、spec、fit、union、intersection、mixin、泛型、方法值、跨包恢复和生命周期等能力；
- `std/std_test` 共有 601 个 `test(...)` 块，属于标准库专项测试，不由本计划迁移到 FCTS；
- compiler tests 中，`test_semantic.c`、`test_codegen.c`、`test_cli.c` 已分别形成约 847、210、182 个
  `static test_*` 入口；`test_archive.c` 目前只有 4 个入口；
- 最近的泛型、spec、value method、静态方法值和组合覆盖已有多轮专门 hardening 文档与 FCTS；
- runtime 已直接覆盖普通 ARC、循环回收、外部根、终结器、复活、部分复活、阈值触发和多线程压力；
- CLI 已有跨包泛型环回收、generic finalizer 环和多 provider 菱形依赖的端到端测试。

上述数字只描述规模，不等价于规范覆盖率。是否补用例仍以“规范分支是否有直接且非等价证据”为准。

### 3.2 本轮优先结论

当前最值得优先补齐的直接证据是：

1. 表达式求值顺序目前有结果断言，但普通二元运算和短路测试缺少足以观察副作用次数与顺序的证据；
2. 模块级绑定的惰性初始化已有 Codegen 结构测试，但非泛型模块绑定缺少系统的 FCTS 可观察行为矩阵；
3. 模块别名的 FCTS 主要集中在 union match，未直接覆盖公开函数、绑定、type、enum、spec 的统一别名面；
4. 许多 compiler tests 检查错误消息片段，但没有统一保证规范中的稳定诊断码、位置和错误数量；
5. `.ft` 正常 round-trip 很丰富，但 ZIP/`.fb` 的损坏、路径安全和多平台打包原子失败证据相对薄弱；
6. ABI 合法/非法类型矩阵在 Semantic/Codegen 中较完整，但真正由 C 调回 Feng callback、按值传递
   `@abi type` 的宿主边界行为仍需要独立原生夹具验证。

## 4 分步交付

每一步都是独立交付单元。前一步专项测试及沙箱外 `make test` 全部通过后，才进入下一步。

### Step 1：表达式求值与函数推导测试

#### 子交付文档

| 子交付 | 范围 | 实施文档 | 状态 |
| --- | --- | --- | --- |
| D1A | 表达式求值顺序 | [Feng 表达式求值顺序 FCTS 补齐实施文档](./feng-test-expression-evaluation-order-implementation-pending.md) | 已完成 |
| D1B | 函数返回类型推导 | [Feng 函数返回类型推导测试补齐实施文档](./feng-test-function-return-inference-implementation-pending.md) | 待 Review |

D1A、D1B 的用例实施细节、Todo、问题和交付记录仅在各自实施文档中维护；本文的用例表只保留范围
索引与交付顺序。

#### 目标

把 [表达式规范 §4](../specifications/feng-expression.md) 和
[函数规范](../specifications/feng-function.md) 中已经生效、但当前主要由静态分析或无副作用结果间接证明的规则，
补成直接测试证据；合法行为优先进入 FCTS，非法返回类型冲突进入 Semantic。

#### 新增用例

| 编号 | 测试归属与行为 | 直接断言 |
| --- | --- | --- |
| EVAL01 | 二元运算左右操作数求值顺序 | 两侧分别追加不同标记，结果与标记顺序同时正确 |
| EVAL02 | 普通函数的多个参数求值顺序 | 参数严格从左到右且各执行一次；target 规则复用已有直接证据 |
| EVAL03 | `false && rhs`、`true || rhs` | RHS 副作用计数保持 0 |
| EVAL04 | `true && rhs`、`false || rhs` | RHS 副作用计数恰为 1 |
| EVAL05 | `if` 表达式只执行选中分支 | 未选分支计数为 0，选中分支为 1 |
| EVAL06 | 成员、下标、调用的后缀链 | base、index、argument、invoke 的观察顺序与各自次数正确；callee 固定复用已有直接证据 |
| FUNC01-A～D | 顶层函数：无 `return`、仅 `return;`、多个一致、多个冲突 | A～C 运行结果正确；D 编译拒绝 |
| FUNC02-A～D | 普通实例方法：同一四形态矩阵 | A～C 运行结果正确；D 编译拒绝 |
| FUNC03-A～D | 普通静态方法：同一四形态矩阵 | A～C 运行结果正确；D 编译拒绝 |
| FUNC04-A～D | `fit` 实例方法：同一四形态矩阵 | A～C 运行结果正确；D 编译拒绝 |
| FUNC05-A～D | `fit` 静态方法：同一四形态矩阵 | A～C 运行结果正确；D 编译拒绝 |
| FUNC06-A～D | 块 Lambda：同一四形态矩阵 | A～C 运行结果正确；D 编译拒绝 |
| FUNC07 | 跨包公开函数使用多个一致返回 | `fcts_lib -> fcts_bin` 恢复推导签名并执行两条路径 |
| FUNC08 | 块 Lambda 返回上下文与外层函数推导隔离 | Lambda 与外层函数分别返回预期类型和值 |

#### 文件建议

- 新增 `fcts/fcts_bin/src/test_evaluation_order.ff`；
- 新增 `fcts/fcts_bin/src/test_function_inference.ff`；
- 在 `fcts_lib` 新增 FUNC07 所需的最小 provider 文件；
- 在 `test/semantic/test_semantic.c` 新增五个冲突 case；FUNC01-D 复用既有用例，不修改其内容与断言；
- 仅在 `fcts/fcts_bin/src/main.ff` 登记两个新入口，不改已有入口或断言。

#### 不重复项

- callable 返回值立即调用、callee 先于参数固定，已有
  `test_callable_result_immediate_invocation.ff`，本步只补普通表达式与多参数顺序；
- 泛型控制流与 descriptor-sized 值已有组合 hardening，不再重复泛型排列；
- 单个有值 `return` 已有基础证据，不作为交叉列；多个一致返回覆盖同一有值推导并额外验证类型统一；
- 数值字面量适配已有 Semantic/Codegen 矩阵，本步不复制相同位置矩阵。

### Step 2：模块绑定与导入行为 FCTS

#### 目标

为 [模块规范 §4、§5、§7](../specifications/feng-module.md) 增加直接运行证据。该步骤是本计划的核心
FCTS 补齐项；先完成 Step 1 作为低风险切片，再进入本步骤。

#### 新增用例

| 编号 | FCTS 行为 | 直接断言 |
| --- | --- | --- |
| MOD01 | 同文件模块绑定按首次访问惰性初始化 | 未访问计数为 0；首次访问后为 1；重复访问仍为 1 |
| MOD02 | 声明顺序相反的模块绑定依赖 | 被依赖绑定先完成，最终值与调用次数正确 |
| MOD03 | 同一模块多文件绑定依赖 | 结果不依赖文件排列，每个初始化器只执行一次 |
| MOD04 | 跨模块、跨包公开 `let`/`var` | 短名与别名读取命中同一 canonical slot；`var` 写入前完成初始化 |
| MOD05 | 模块绑定初始化循环 | 按规范观察初始化中的默认值，两个初始化器均只完成一次 |
| MOD06 | 非泛型 type 静态 `let`/`var` 惰性初始化 | 读取、直接写入、复合赋值均不重复执行初始化器 |
| MOD07 | 静态字段之间及静态字段到模块绑定的依赖 | 依赖链按首次访问触发且结果稳定 |
| MOD08 | import alias 的统一公开面 | 同一别名实际使用函数、绑定、type、enum、spec，全部行为正确 |
| MOD09 | 完整模块路径类型引用无需 import | 构造、参数、返回、数组元素中至少选一个非等价运行路径 |
| MOD10 | 未使用的 import 同名碰撞与文件级 import 隔离 | 整个 FCTS 成功编译运行；无关文件不受另一文件 import 影响 |

#### 文件建议

- 新增 `fcts/fcts_bin/src/test_module_binding.ff`；
- 新增 `fcts/fcts_bin/src/test_module_import_surface.ff`；
- 在 `fcts_lib` 中按模块职责拆分最小 provider，跨文件场景不得合并回单文件；
- 在 FCTS 主入口登记新增测试函数。

#### compiler test 补充边界

以下失败行为不进入 FCTS：

- 使用裸名触发 import/import、import/local 二义性；
- alias 与本地符号、其他 alias、导入短名的急切冲突；
- 非公开模块或非公开成员访问。

当前 Semantic 已有上述矩阵。本步只在其缺少精确诊断码、错误位置或单一错误数量断言时，按 Step 3
新增独立 case，不修改已有用例。

### Step 3：稳定诊断码与阶段归属 compiler tests

#### 目标

compiler tests 不只证明“失败”，还要证明失败发生在正确阶段，并输出权威规范规定的稳定诊断码。

#### Step 3A：建立可达诊断清单

以错误码规范和源码实际 emit 点双向核对，给每个诊断标记：

- 用户输入可达：必须有 Lexer/Parser/Semantic/Codegen 中恰当层级的直接 case；
- 前一阶段应拦截：测试前一阶段的目标错误码，不强行制造后端非法状态；
- 内部不变量：只在已有公共测试 seam 能安全构造时测试，不为触发诊断增加生产代码开口；
- 已消解或历史码：不得新增测试固化。

清单同时记录“规范码 -> emit 点 -> 直接测试入口”。未完成该清单前，不按错误码数字机械批量造用例。

#### Step 3B：Lexer 与 Parser

- 每个当前有效且用户可达的 `LE`/`SE` 至少一个独立输入；
- 断言精确 code、起始 token/行列和唯一主要错误；
- 合法对偶若对应独立解析分支，增加 AST kind、关键字段和 source span 断言；
- 以表驱动新增 case，不改已有 parser 用例的断言。

#### Step 3C：Semantic

按以下批次交付，每批均可独立回归：

1. 绑定、函数、表达式、流程、异常；
2. type、enum、tuple、array、value type；
3. module、visibility、import、package surface；
4. spec、fit、union、intersection；
5. ABI、extern、指针与生命周期编译期约束；
6. 泛型只补当前既有 hardening 未映射到精确诊断码的分支。

每个 case 断言：精确 `AE` code、目标 token 位置、错误数量，以及必要但不过度绑定实现文案的关键实体名。
同一输入若规范要求多个独立诊断，显式断言 code 集合；否则保持一例一错。

#### Step 3D：Codegen

[Codegen 错误码迁移表](../specifications/feng-error-codes-ce.md) 中：

- “回到 AE”的项目由 Semantic case 证明，不再构造非法语义结果触发 CE；
- “消解”的项目不新增测试；
- “回到 IE”的项目只做现有 seam 下可安全构造的不变量测试；
- 仅“继续 CE”的用户可达后端约束需要精确 code 测试。

同时为 Step 1、Step 2 新增行为核对既有 Codegen 分支映射；只有发现非等价空白时才新增结构测试。

### Step 4：C ABI 真实边界补齐

#### 目标

区分“Feng 内部直接调用标注了 `@abi` 的函数”和“实际穿越宿主 C ABI”。优先把无需额外原生夹具的
合法行为放入 FCTS；必须由 C 调用或返回的行为放入 `test/cli/` 的临时工程。

#### FCTS 用例

| 编号 | 行为 | 归属理由 |
| --- | --- | --- |
| ABI01 | 调用约定中的库名/C symbol 名来自跨模块公开 `let` 字面量绑定 | 可使用现有 `libc` 函数观察，放 FCTS |
| ABI02 | `&string`、`&` ABI-compatible byte array 在 extern 调用期间有效 | 可使用 `strlen`、`memset` 等稳定 libc 入口观察，放 FCTS |
| ABI03 | 同类型数据指针与 `Foo*` 的 `==`/`!=` 和字段/参数/返回继续传递 | 不需要调用指针本身，放 FCTS |

FCTS 不使用未规范的 runtime 私有 ABI 作为夹具，不为测试新增 runtime helper。

#### `test/cli/` 原生夹具

构造一个最小 C 静态库和 Feng 临时项目，覆盖：

| 编号 | 行为 | 必须使用原生夹具的原因 |
| --- | --- | --- |
| ABI04 | C 实际读取并写回 Feng 传入的标量指针 | 验证指针不是仅在 Feng 内部流转 |
| ABI05 | 有字段对象形式 `@abi type` 按值传入 C、由 C 按值返回 Feng | 需要真实验证 payload ABI，而非只检查生成 C 文本 |
| ABI06 | C 接收 `Foo*` 并反向调用顶层 `@abi func`，精确断言参数、返回值和调用次数 | Feng 语言不能直接调用 `Foo*`，必须由 C 触发 callback |

原生夹具只使用项目已有宿主 C 编译流程，在工程 `temp/` 或动态测试目录中生成并执行，不在
`/tmp`、`/private/tmp` 中执行产物。

#### 结构与负向证据

- Semantic 继续负责不兼容字段、非法 pointer source、borrow 逃逸、异常越过 ABI 边界等拒绝；
- Codegen 负责 wrapper 签名、payload 提取/重建、调用约定属性和零额外 wrapper 的结构事实；
- 只补未映射分支，不重复当前已有 ABI 矩阵。

### Step 5：`.ft`、`.fb` 与 ZIP 制品健壮性

#### 目标

补齐正常 round-trip 之外的损坏输入、安全边界和发布原子性。该步骤不适合 FCTS，全部进入 `test/`。

#### Step 5A：ZIP 与 FM（`test/archive/`）

| 编号 | 新增 case |
| --- | --- |
| ART01 | entry path 空串、绝对路径、`.`、`..`、空 segment、反斜杠、文件尾 `/` 的表驱动拒绝 |
| ART02 | reader 面对预制恶意 `../`、绝对路径或反斜杠 entry 时拒绝读取/解压到目标根外 |
| ART03 | 截断 central directory、损坏 CRC、未知 compression method 的明确失败与资源清理 |
| ART04 | 审计重复 entry、file/directory 同路径碰撞的现状；若主规范未定义，先提交人工决策，不直接固化行为 |
| ART05 | FM 未闭合字符串、非法转义、非法 key/value、CRLF 和末行无换行等已定义边界 |

ART02 必须验证目标根外哨兵文件未创建或未被覆盖，不能只断言返回 false。

#### Step 5B：`.ft`（`test/symbol/`）

在已有 bad magic、正常 round-trip、重复 bundle module 和坏 symbol entry 基础上补：

- 不支持的格式主版本；
- header、字符串表、声明表和关联表的逐段截断；
- count/offset 越界、索引指向不存在字符串或声明；
- 重复模块身份、重复公开声明身份和不完整泛型声明事实；
- 失败后 provider/cache 不保留半加载模块，下一次合法加载可成功。

不为每个字段偏移造一个 case；按共享 bounds-check 分支选择最小代表。

#### Step 5C：`.fb` 与多平台发布（`test/cli/`）

| 编号 | 新增 case |
| --- | --- |
| ART11 | manifest 声明 `feng`/platform，但缺对应 `lib/<platform>`，消费与打包均拒绝 |
| ART12 | 多平台公开 `.ft` 事实不等价时 pack 整体失败 |
| ART13 | 多平台普通资源同包路径内容不一致时 pack 整体失败 |
| ART14 | 任一平台构建或完整性校验失败时不生成新的部分 `.fb` |
| ART15 | 已有旧 `.fb` 时失败不得以半成品覆盖旧制品 |
| ART16 | `.fb` 中缺 `feng.fm`、缺 `mod/`、manifest 与目录平台集合不一致时明确拒绝 |

ART14、ART15 同时检查临时文件清理和目标制品状态，落实包规范的整体发布原子性。

### Step 6：最终映射核对与收口

前五步完成后，建立以下双向映射：

1. 稳定规范条款 -> FCTS 或 compiler/制品直接测试；
2. Lexer/Parser/Semantic/Codegen/Runtime/Symbol/Archive/CLI 非等价分支 -> 直接测试；
3. 稳定诊断码 -> emit 点 -> 精确 code 测试。

只对没有直接证据且不与现有路径等价的空白增加最小 case。最终文档记录新增用例、发现的问题、专项
命令和全量回归结果，然后将状态改为“已完成”。

## 5 暂不优先补齐的范围

### 5.1 泛型与 spec 排列组合

现有工程已经完成泛型组合、高级组合、跨特性、剩余结构、generic spec、object-form spec 完整面等多轮
专项核对。本轮不继续增加：

- 同一泛型路径换一种标量、托管类型或嵌套深度；
- 已覆盖 callable/spec/union/intersection 位置的笛卡尔积；
- 仅为提高 FCTS 数量而复制同一 provider/consumer 结构。

仅当 Step 6 分支映射确认存在非等价空白时再补。

### 5.2 ARC 与循环回收基础矩阵

runtime 和 CLI 已覆盖普通 ARC、环、外部根、终结器、复活、部分复活、数组 storage、阈值、多线程、
跨包泛型环及 generic finalizer 环。本轮不重复这些拓扑。后续只为新发现的生命周期缺陷增加最小回归。

### 5.3 标准库

标准库行为继续放在 `std/std_test/`。本计划不把标准库 API 正确性迁移到 FCTS，也不以 FCTS 替代
`std-tests`。若后续需要补标准库用例，应单独形成标准库覆盖审计文档。

### 5.4 LSP 与 DAP 的横向扩展

当前 `test_cli` 已有较丰富的 hover、completion、definition、references、rename、diagnostics、断点、
stack、variables 和 evaluate 覆盖。本轮不做协议排列扩展；新用例继续以规范新增或实际缺陷驱动。

### 5.5 草案能力

未生效草案不进入本轮正向 FCTS。若草案转为稳定规范，应先更新主规范和对应实现计划，再增加测试。

## 6 分步验收与交付物

| 交付 | 主要目录 | 验收重点 |
| --- | --- | --- |
| D1：求值与函数推导 | `fcts/` | 副作用次数、顺序、返回值；FCTS 全绿 |
| D2：模块绑定与导入 | `fcts/` | 惰性一次、依赖、循环、跨文件/包、alias 面 |
| D3：稳定诊断契约 | `test/lexer`、`parser`、`semantic`、`codegen` | code、位置、数量、阶段归属 |
| D4：真实 C ABI | `fcts/`、`test/cli`、必要的 `test/codegen` | 真正跨 C 边界，而非仅 Feng 内部直调 |
| D5：制品健壮性 | `test/archive`、`test/symbol`、`test/cli` | 损坏输入、路径安全、多平台一致性、原子失败 |
| D6：映射收口 | 本文 | 规范与实现分支均有直接证据或明确非目标 |

每个交付必须：

1. 先确认本步骤不改变规范；若发现规范不完整，停止并交由人工决策；
2. 只新增当前步骤的测试，不顺带修改无关实现或既有测试；
3. 运行对应专项测试；
4. 在 Codex 沙箱外执行完整 `make test`；
5. 记录专项与全量结果后再进入下一步。

## 7 Review 决策点

实施前需要人工确认：

1. 是否同意按 D1 -> D6 的顺序推进；
2. 是否批准为 D1、D2 在 `fcts/fcts_bin/src/main.ff` 仅新增测试入口登记；
3. 是否同意 D3 先做诊断清单，再按可达性补 case，而不是追求所有历史 CE 数字都有测试；
4. 是否同意 D4 在 `test/cli` 动态构造最小 C 静态库验证真实 callback 和 `@abi type` 按值 ABI；
5. 是否同意 D5 优先处理归档路径安全与发布原子性，再处理更细的 `.ft` 字段级损坏矩阵。
