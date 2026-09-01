# Feng 语言正确性用例补齐实施文档

> 状态：G01～G09 已交付；G10～G25 待 Review
>
> 所属总计划：[Feng 测试覆盖补齐计划](./feng-test-coverage-hardening-pending.md)
>
> 独立交付组：G01～G25
>
> 盘点基线：2026-08-26

## 1 文档定位

本文是 Feng 下一轮“语言本身的行为、语义与语法正确性”测试补齐实施清单。

测试按单一测试重点拆成 G01～G25 共 25 个独立交付组。每组内部包含多个用例 TODO，并独立完成：

- 规范和现有测试基线确认；
- 本组用例补齐；
- 本组专项验收；
- 在 Codex 沙箱外执行本组独立的 `make test` 全量回归；
- 本组问题闭环和独立交付记录。

其他组或最终收口阶段的回归结果不能代替本组全量回归。各组可以分别实施、验收和提交。

语言规则只引用 `docs/specifications/` 下的主规范，本文不定义或改变语言语义。所属总计划在本文通过
Review 后只保留分组范围、实施顺序、状态和本文链接；用例 TODO 与验收记录只在本文维护，详细问题
记录只在第 30 节链接的对应问题文件中维护。

本文默认只授权新增测试和必要的测试辅助代码，不概括授权修改既有测试、产品实现、runtime 私有 ABI、
公开 ABI、`.ft` 格式或语言规范。测试或 Review 暴露上述变更需求时，必须先按第 30 节在对应文件中
记录问题；只有在人工明确批准且对应组 TODO 写明范围后，必要变更才进入该组交付。任何新增运行时
开销仍须单独明确批准，不能由“修复问题”概括授权。

## 2 依据、边界与基线

### 2.1 权威规范

- [模块规范](../specifications/feng-module.md)；
- [变量绑定与作用域规范](../specifications/feng-binding.md)；
- [表达式与运算规范](../specifications/feng-expression.md)；
- [类型规范](../specifications/feng-type.md)；
- [流程控制规范](../specifications/feng-flow.md)；
- [enum 规范](../specifications/feng-enum.md)；
- [tuple 规范](../specifications/feng-tuple.md)；
- [字符串转义规范](../specifications/feng-string-escape.md)；
- [反引号字符串规范](../specifications/feng-string-raw.md)；
- [错误码规范](../specifications/feng-error-codes.md)。

实施结果不能由主规范唯一确定时，必须先按第 30 节在对应文件中记录问题并暂停本组，由人工决定是否
补充规范。不得在测试或实现中自行选择行为。

### 2.2 当前覆盖基线

2026-08-26 静态盘点结果：

- FCTS 共有 935 个 `test(...)` 块，`fcts_bin` 主入口登记 84 个测试函数；
- `test_semantic.c`、`test_parser.c`、`test_lexer.c`、`test_codegen.c`、`test_runtime.c` 分别约有
  861、134、21、208、79 个 `static test_*` 入口；
- 表达式求值顺序和函数返回类型推导专项已经完成，不重复 EVAL01～EVAL06、FUNC01～FUNC08；
- 泛型、object-form `spec`、intersection、union、method value、异常、`defer`、终结器和 ARC 已有
  多轮专项，本次只补稳定规范仍缺少的直接证据。

以上数字只表示规模，不表示规范覆盖率。每组开始前必须重新确认本组相关测试现状和工作区状态。

### 2.3 测试层级

- 合法程序的可观察行为放入 `fcts/`，必须真实编译并运行；
- 非法词法输入放入 Lexer test；
- 非法语法结构放入 Parser test；
- 名称绑定、类型、控制流等非法语义放入 Semantic test；
- Compiler test 关心诊断码、位置、数量、AST、IR 等编译器证据；
- 只有 Codegen 文本、Semantic 接受或“程序未崩溃”不能代替语言行为断言。

### 2.4 非目标

- 已充分覆盖特性的同构类型、嵌套深度和组合排列；
- 标准库 API、LSP、DAP、格式化器和调试协议；
- C ABI 宿主边界、`.ft` / `.fb` / ZIP 制品和发布流程；
- 规范明确为未定义行为的动态整数除零、取模除零和越界移位；
- 没有稳定跨平台结果的浮点平台差异；
- 纯性能基准、模糊测试和随机测试基础设施建设。

## 3 分组原则

### 3.1 合法语言行为组

G01～G10 分别验证一个稳定、可观察的语言行为重点。每项 TODO 必须通过真实编译和运行提供直接证据。

### 3.2 稳定诊断组

G11～G25 分别验证一个词法、语法或语义诊断重点。每项 TODO 先完成稳定诊断码到现有测试的映射：

- 已有直接证据时，在交付记录中引用测试函数，不重复新增；
- 缺少直接证据时，新增最小非法程序和必要的合法邻界用例；
- 用户输入不可达或非稳定诊断不为了数量强行造例；
- 每个非法用例至少检查诊断码、token、行列、数量和阶段；
- 规范不承诺诊断文本稳定时，不锁定完整文本。

### 3.3 独立交付规则

一个组只有同时满足以下条件才可标记为“已交付”：

- 本组全部用例 TODO 已完成，或记录了已有直接证据；
- 本组专项测试通过；
- 本组完成后在 Codex 沙箱外独立执行的 `make test` 通过；
- 本组发现的问题已经关闭，或经人工决定不阻塞交付；
- 本组执行 `git diff --check` 并填写独立交付记录；
- 给出本组建议的英文 commit message，但不自动提交。

## 4 用例 TODO 状态规则

- `[ ]`：待确认或待实施；
- `[x]`：已有直接证据，或新增用例已经通过本组验收；
- 被问题阻塞时保持 `[ ]`，并在用例后引用 `ISSUE-Gxx-NNN`；
- 不适用或不应新增时不得直接删除 TODO，应记录规范依据和人工确认结论。

## 5 G01：惰性初始化语义

### 5.1 测试重点

验证模块级绑定和类型 static 绑定在首次读写与依赖关系中的惰性初始化行为，并核对初始化循环保持
自然栈溢出的既定边界。

预期新增 `test_module_binding_semantics.ff`、必要的最小 `fcts_lib` 提供端文件和主入口登记。

### 5.2 用例 TODO

- [x] MOD01：同文件模块绑定首次读取只初始化一次；
- [x] MOD02：模块绑定首次直接写入前完成初始化；
- [x] MOD03：模块绑定复合写入前完成初始化且只执行一次；
- [x] MOD04：声明顺序相反的模块绑定依赖按实际读取初始化；
- [x] MOD05：同模块多文件绑定依赖保持同一初始化语义；
- [x] MOD06：跨模块或跨包首次读取触发目标绑定初始化；
- [x] MOD07：初始化循环不作特殊处理，实际进入循环后等同普通函数无限递归并自然耗尽调用栈；
- [x] MOD08：static `let` / `var` 复用模块级绑定的延迟初始化规则，重复访问同一静态绑定只初始化一次；
- [x] MOD09：static 绑定读取另一 static 绑定时按依赖初始化；
- [x] MOD10：static 绑定读取模块绑定时按依赖初始化；
- [x] MOD11：同一泛型闭合类型实例的 static 绑定重复访问时只初始化一次；
- [x] MOD12：同一泛型类型的不同闭合类型实例分别拥有独立 static 存储和初始化状态；
- [x] MOD13：修改一个泛型闭合类型实例的 static `var`，不影响其他闭合类型实例的对应绑定。

MOD08 不按所属类型是否泛型区分延迟初始化规则。MOD11～MOD13 验证人工已经确定的泛型 static 存储
身份：同一闭合类型实例共享同一 static 存储；不同闭合类型实例拥有彼此独立的 static 存储、初始化状态
和可变状态。该规则已经补入类型主规范，见 `ISSUE-G01-001`。

MOD07 会有意终止进程，不接入常规 FCTS 可执行入口；在 `test/cli/test_cli.c` 中由父测试进程编译最小
用例，再以隔离子进程执行。父进程只验收子进程未成功返回：既允许操作系统直接以信号终止，也允许
sanitizer 接管致命信号后以非零状态退出，同时排除 `execl` 失败。具体平台信号或退出码不属于语言
行为。

### 5.3 独立验收与交付 TODO

- [x] 核对模块、绑定和类型规范，确认 MOD11～MOD13 已有直接证据，其余用例需要新增或隔离验证；
- [x] 独立运行 G01，核对初始化次数、跟踪顺序和最终值，并复核 MOD07 的隔离运行结果；
- [x] 在 Codex 沙箱外为 G01 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G01 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 5.4 独立交付记录

- 状态：已交付
- 实际文件与用例：
  - `test_module_binding_semantics.ff`：新增 MOD01～MOD06、MOD08～MOD10；
  - `module_binding_semantics_provider_a.ff`、`module_binding_semantics_provider_b.ff`：提供 MOD05 同模块
    多文件依赖夹具；
  - `fcts_lib/src/g01_binding/g01_binding.ff`：提供 MOD06 跨包绑定夹具；
  - `main.ff`：登记 G01 常规 FCTS 入口；
  - 既有 `test_generic_static_binding.ff`：`cross-package generic static initializer runs once` 与 aggregate
    initializer 用例覆盖 MOD11 和 MOD12 初始化状态，`closed generic static states are isolated` 覆盖 MOD12
    存储隔离和 MOD13 可变状态隔离；
  - `test/cli/test_cli.c`：新增 MOD07 隔离子进程自动化用例，不接入常规 FCTS 入口；子进程禁用
    core dump 并限制自身栈空间，父进程只验收其未成功返回，并显式排除 `execl` 失败。
- 本组专项结果：修复后沙箱外完整 `test_cli` 在原生路径和模拟 Linux UBSan 非零退出的路径下均
  通过；MOD07 最小用例成功编译，隔离子进程自然耗尽调用栈后分别由信号终止和 sanitizer 非零退出，
  父测试进程继续完成其余 CLI 测试
- 本组沙箱外 `make test`：修复后通过；UBSan 与 normal 全量阶段均完成，FCTS 均为 976/976，smoke
  均为 91/91，std 测试均为 601/601，性能约束、增量构建与发布脚本检查通过
- 问题：`ISSUE-G01-001`、`ISSUE-G01-002`、`ISSUE-G01-003` 均已关闭
- 建议 commit message：`test: cover lazy initialization semantics`

## 6 G02：模块名称解析语义

### 6.1 测试重点

验证 import alias、完整模块路径、同一模块级绑定的存储身份、惰性名称碰撞和文件级 import 隔离的合法行为。

完整模块路径无需先 import,可访问公开模块中的全部公开顶层声明：`type`、`enum`、`spec`、顶层
`func` 和模块级 `let` / `var`。本组逐类提供直接运行证据，不把“类型路径可用”替代为“所有顶层路径
均可用”。

实际新增 `test_module_import_semantics.ff`、必要的多文件辅助用例和最小 `fcts_lib` 导出声明。

### 6.2 用例 TODO

- [x] IMP01：无别名 import 后使用短名调用公开顶层函数；
- [x] IMP02：alias 调用公开顶层函数；
- [x] IMP03：alias 与完整模块路径访问同一公开模块级 `var` 时指向同一存储槽；
- [x] IMP04：alias 引用导出类型；
- [x] IMP05：alias 引用导出 enum；
- [x] IMP06：alias 引用导出 spec；
- [x] IMP07：无 import 时使用完整模块路径调用公开顶层函数；
- [x] IMP08：无 import 时使用完整模块路径取得公开顶层函数值并调用；
- [x] IMP09：无 import 时使用完整模块路径引用并构造公开 `type`；
- [x] IMP10：无 import 时使用完整模块路径引用公开 `enum`；
- [x] IMP11：无 import 时使用完整模块路径引用公开 `spec`；
- [x] IMP12：无 import 时使用完整模块路径读取公开模块级 `let`；
- [x] IMP13：无 import 时使用完整模块路径读写公开模块级 `var`；
- [x] IMP14：未使用的同名 import 保持合法；
- [x] IMP15：未使用 import 与同名局部声明的惰性碰撞保持合法；
- [x] IMP16：同模块多文件的 import 不泄漏到其他文件；
- [x] IMP17：alias 与短名访问同一公开模块级绑定时共享状态；
- [x] IMP18：无别名 import 后使用短名引用并构造公开 `type`；
- [x] IMP19：无别名 import 后使用短名引用公开 `enum`；
- [x] IMP20：无别名 import 后使用短名引用公开 `spec`；
- [x] IMP21：无别名 import 后使用短名读取公开模块级 `let`；
- [x] IMP22：无别名 import 后使用短名读写公开模块级 `var`；
- [x] IMP23：alias 读取公开模块级 `let`；
- [x] IMP24：alias 读写公开模块级 `var`；
- [x] IMP25：alias 取得公开顶层函数值并调用；
- [x] IMP26：无别名 import 后使用短名取得公开顶层函数值并调用；
- [x] IMP27：完整模块路径首段与局部值同名时,局部值优先按普通成员链解析。

碰撞真正被引用后的非法行为不属于本组，归 G22。

### 6.3 独立验收与交付 TODO

- [x] 核对模块规范并确认 IMP01～IMP27 没有等价直接证据；
- [x] 独立运行 G02，核对 alias、完整路径、状态共享、惰性碰撞和文件隔离；
- [x] 在 Codex 沙箱外为 G02 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G02 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 6.4 独立交付记录

- 状态：已交付
- 实际文件与用例：
  - `docs/specifications/feng-module.md`：统一定义三种名称访问形式、声明身份和局部值优先规则；
  - 中英文模块手册：同步面向用户的名称访问与局部值优先说明；
  - `docs/engineering/feng-use-optimize.md`、`feng-module-optimize-dev.md` 与覆盖计划：消除过期或互相
    冲突的实现状态描述；
  - `src/semantic/analyzer.c`：统一 alias 与完整模块路径的公开顶层成员解析,并保持局部值优先；
  - `src/codegen/codegen.c`：统一恢复完整路径函数、模块级绑定和 enum 的提供方声明；
  - `test/semantic/test_semantic.c`：把既有 `qualified_path` 正例修正为真正的完整模块路径；
  - `test/codegen/test_codegen.c`：新增无 import 的完整路径函数、enum 与模块级绑定生成测试；
  - `fcts_lib/src/g02_import/`：提供公开顶层声明和惰性碰撞夹具；
  - `module_import_*_semantics.ff` 与 `test_module_import_semantics.ff`：实现 IMP01～IMP27；
  - `main.ff`：登记 G02 FCTS 入口。
- 本组专项结果：Semantic tests、Codegen tests 均通过；完整 FCTS 969/969；
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成,FCTS 均为 969/969,smoke 均为
  91/91,Runtime、CLI、标准库、性能约束、增量构建与发布脚本检查均通过；
- 问题：`ISSUE-G02-001`、`ISSUE-G02-002` 均已关闭；
- 交付结论：三种名称访问形式已对全部公开顶层声明形成直接行为证据,实现缺口已修复,未增加运行时
  查找或分派,未变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- 建议 commit message：`feat: support full-path module member access`

## 7 G03：赋值求值顺序

### 7.1 测试重点

验证普通赋值和复合赋值的左侧定位顺序、右侧求值顺序、单次求值和最终写回位置。

预期新增 `test_assignment_evaluation_semantics.ff`。

### 7.2 用例 TODO

- [x] ASN01：普通索引赋值按 base、index、右值顺序各求值一次，最后写回已定位元素；
- [x] ASN02：普通成员赋值按 receiver、右值顺序各求值一次，最后写回已定位成员；
- [x] ASN03：索引算术复合赋值按 base、index、旧值读取、右值、写回顺序执行，base 和 index 各求值一次；
- [x] ASN04：成员算术复合赋值按 receiver、旧值读取、右值、写回顺序执行，receiver 只求值一次；
- [x] ASN05：带副作用索引或 receiver 的位运算复合赋值满足相同顺序与单次求值规则；
- [x] ASN06：右值改变 index 后，仍写回此前已定位的索引位置；
- [x] ASN07：右值重新绑定 base 或 receiver 后，仍写回此前已定位的原数组或原对象。

每项同时断言事件轨迹、调用次数和最终状态，不能只检查最终数值。

### 7.3 独立验收与交付 TODO

- [x] 核对表达式规范，并确认不重复 EVAL01～EVAL06；
- [x] 独立运行 G03，核对左侧定位、右值顺序、求值次数和写回位置；
- [x] 在 Codex 沙箱外为 G03 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G03 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 7.4 独立交付记录

- 状态：已交付
- 实际文件与用例：
  - `fcts/fcts_bin/src/test_assignment_evaluation_semantics.ff`：新增 ASN01～ASN07，覆盖事件轨迹、
    调用次数、旧值读取、目标定位、右值重绑和语句级受管临时值 LIFO 释放；
  - `fcts/fcts_bin/src/main.ff`：登记 G03 FCTS 入口；
  - `src/codegen/codegen.c`：增加编译期绑定可变性与 managed identity 稳定性事实，只为无法证明
    稳定，且后续求值或最终写回可能使其失效的 borrowed managed 目标建立语句级强引用；
  - `test/codegen/test_codegen.c`：验证 `let`、`self`、不可变成员链、已有拥有型临时结果与纯标量
    写回不新增目标保护，并验证不稳定成员、索引及 aggregate 写回目标使用既有 cleanup 链；
  - [G01～G09 问题归档](./feng-language-conformance-coverage-hardening-issues/g01-g09.md)：记录
    `ISSUE-G03-001` 的发现、决策、修复和验收结果。
- 本组专项结果：Codegen tests 通过；完整 FCTS 976/976；ASN01～ASN07 全部通过；ASN07 同时验证
  右值临时对象在目标写回后立即终结；
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 976/976、smoke 均为
  91/91、std 均为 601/601，Runtime、CLI、性能约束、增量构建与发布脚本检查均通过；
- 问题：`ISSUE-G03-001` 已关闭；
- 交付结论：普通与复合赋值的左侧定位、单次求值和写回语义已有直接行为证据；右值重绑缺陷已按
  证明驱动方案修复，稳定路径不新增 ARC，且未变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- 建议 commit message：`fix: preserve assignment targets across RHS rebinding`

## 8 G04：动态整数运行时语义

### 8.1 测试重点

验证动态整数运算的静默回绕，以及有符号和无符号右移的运行时行为。

预期新增 `test_integer_runtime_semantics.ff`。输入必须经动态绑定或函数参数产生，避免只验证常量折叠。

本组不得为语言行为增加运行时开销：不得新增溢出检查、条件分支、辅助函数调用或 runtime 接口；
有符号回绕和两类右移必须保持与对应固定宽度原生整数运算等价的 lowering。

### 8.2 用例 TODO

- [x] INT01：动态 `u8` 加法静默回绕；
- [x] INT02：动态 `u8` 减法静默回绕；
- [x] INT03：动态 `i8` 加法静默回绕；
- [x] INT04：动态 `i8` 减法静默回绕；
- [x] INT05：动态 `i32` 乘法回绕；
- [x] INT06：动态 `u32` 乘法回绕；
- [x] INT07：负 `i32` 动态右移为算术右移；
- [x] INT08：`u32` 动态右移为逻辑右移。

不构造动态除零、动态取模除零和越界移位等未定义行为。
本组不包含表达式主规范 §3.1 所列的有符号整数 `MIN / -1` 与 `MIN % -1` 未定义行为。

### 8.3 独立验收与交付 TODO

- [x] 核对表达式规范、固定目标位宽和合法移位范围；
- [x] 独立运行 G04，确认全部结果由运行时计算并核对回绕值和补位；
- [x] 核对生成代码和优化后机器码，确认未新增溢出检查、条件分支、辅助函数调用或 runtime 接口；
- [x] 在 Codex 沙箱外为 G04 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G04 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 8.4 独立交付记录

- 状态：已完成
- 实际文件与用例：
  - `fcts/fcts_bin/src/test_integer_runtime_semantics.ff`：以函数参数构造动态输入，补齐 INT01～INT08；
  - `fcts/fcts_bin/src/main.ff`：将 G04 行为测试接入完整 FCTS；
  - `src/codegen/codegen.c`：定宽整数 `+`、`-`、`*` 统一采用忽略溢出标志的编译器内建 lowering，
    避免 C 有符号溢出 UB 和窄整数提升，不新增运行时判断、辅助函数或 runtime 接口；
  - `test/codegen/test_codegen.c`：验证整数回绕 lowering 的生成结构，并编译生成 C。
- 本组专项结果：完整 FCTS 984/984；严格 UBSan 专项以 `halt_on_error=1` 运行，984/984 且无整数
  溢出报告；Codegen tests 通过；macOS ARM64 `-O2` 机器码中加、减、乘及两类右移分别收敛为
  `add`、`sub`、`mul`、`asr`、`lsr`，未出现新增溢出检查、条件分支、辅助函数调用或 runtime 接口；
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 984/984、smoke 均为
  91/91，std、Runtime、CLI、性能约束、增量构建与发布脚本检查均通过；
- 问题：`ISSUE-G04-001`、`ISSUE-G04-002`、`ISSUE-G04-003` 均已关闭；
- 交付结论：INT01～INT08 均已有动态行为证据；整数回绕实现不再依赖 C 有符号溢出 UB，优化后保持
  对应固定宽度原生整数指令，且未变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- 建议 commit message：`fix: guarantee zero-cost wrapping integer codegen`

## 9 G05：对象构造顺序

### 9.1 测试重点

验证字段声明初始化、构造函数和对象字面量字段写入的三阶段顺序及最终覆盖行为；补充验证完整构造
写法、隐式公开无参构造、显式构造对默认构造的取消，以及 `seal` 构造在 type 内外和真实跨包边界的
可用性。

预期新增一个 FCTS 行为文件和最小库侧提供端。

### 9.2 用例 TODO

- [x] CTOR01：单个声明字段初始化器、构造函数、字面量字段按三阶段执行；
- [x] CTOR02：对象字面量字段按书写顺序写入；
- [x] CTOR03：对于 `Type(arg1(), arg2()) { field: fieldValue() }`，先从左到右求值构造实参
  `arg1()`、`arg2()`，再执行构造函数体，构造函数返回后才求值 `fieldValue()` 并写入 `field`；
- [x] CTOR04：`Type {}` 与同类型无参数构造语义一致；
- [x] CTOR05：`Type(args) { field: value }` 中，构造函数先写入 `self.field`，对象字面量阶段再写入
  同一 `var` 实例成员，最终成员值为 `value`；
- [x] CTOR06：跨包提供端的字段初始化和构造顺序一致。

规范不定义多个声明字段初始化器之间的相对顺序，因此 CTOR01 只使用一个带副作用的声明初始化器。

CTOR03 只验证“构造实参表达式 -> 构造函数体 -> 对象字面量字段值表达式及写入”的相对顺序；该用例
不使用带副作用的声明字段初始化器，不据此定义构造实参与声明字段初始化器之间的相对顺序。

CTOR05 中的“覆盖”只表示同一对象的 `var` 实例成员先由构造函数写入、再由对象字面量写入，不表示
外层 `let` / `var` 变量重新绑定。对象字面量可以完成尚未绑定的 `let` 成员，但不得覆盖已经由声明
初始化器或构造函数完成绑定的 `let` 成员；后者必须在编译期报错，不属于 CTOR05 的正向行为用例。

#### 9.2.1 同包 `let` 三阶段正向用例

- [x] LET01：`let` 成员在成员声明初始化阶段首次显式绑定，构造完成后保持该值；
- [x] LET02：无声明初始化器的 `let` 成员在构造函数阶段首次显式绑定，构造完成后保持该值；
- [x] LET03：前两个阶段均未绑定的 `let` 成员可在对象字面量阶段首次显式绑定；
- [x] LET04：三个阶段均未显式绑定的 `let` 成员在构造结束后保留类型默认零值。

#### 9.2.2 同包 `let` 三阶段负向用例

- [x] LET05：声明初始化阶段已绑定后，构造函数再次绑定必须报错；
- [x] LET06：声明初始化阶段已绑定后，对象字面量再次绑定必须报错；
- [x] LET07：同一构造函数内对同一 `let` 成员绑定两次必须报错；
- [x] LET08：当前选中的构造函数已绑定后，对象字面量再次绑定必须报错；
- [x] LET09：三个构造阶段结束后，无论成员在声明、构造函数、对象字面量阶段完成绑定，还是始终只
  保留默认零值，任何再次绑定或赋值都必须报错。

#### 9.2.3 跨包 `let` 三阶段正向用例

- [x] LET10：通过真实包依赖分别构造“声明阶段绑定、构造函数阶段绑定、consumer 对象字面量阶段
  绑定、三个阶段均未绑定”的公开 `let` 成员，并得到对应显式值或默认零值。

#### 9.2.4 跨包 `let` 三阶段负向用例

- [x] LET11：经过真实 `.ft` 往返后，provider 声明阶段或当前选中构造函数已经绑定的公开 `let`
  成员，在 consumer 对象字面量中再次绑定必须报错；
- [x] LET12：经过真实 `.ft` 往返后，无论公开 `let` 成员在三个阶段中的哪个阶段完成绑定，或始终
  只保留默认零值，consumer 在构造结束后的任何再次绑定或赋值都必须报错。

LET05～LET09 属于编译器诊断测试；LET11～LET12 必须由实际写出并重新读取 `.ft` 的测试提供证据，
不能仅以同一进程中的源码 AST 或内存符号图代替跨包边界。

#### 9.2.5 本包构造写法与可用性正向用例

- [x] CTOR07：未显式声明任何构造函数时，公开隐式无参构造允许 `Type()`、`Type() {}`、
  `Type() { field: value }`、`Type {}` 与 `Type { field: value }`，并执行一次统一构造流程；
- [x] CTOR08：显式公开无参或有参构造分别允许 `Type()`、`Type(args)` 及其对象字面量后缀形式；只有
  无参构造允许 `Type { ... }` 简写，选中的显式构造只执行一次；
- [x] CTOR09：type 自身的实例方法和静态方法均可调用该 type 的 `seal` 无参或有参构造，并得到完整
  构造后的实例。

#### 9.2.6 本包构造写法与可用性负向用例

- [x] CTOR10：未显式声明任何构造函数时，隐式默认构造只接受零参数；`Type(arg)` 与
  `Type(arg) { ... }` 必须报错；
- [x] CTOR11：只显式声明有参构造后不再存在默认无参构造；`Type()`、`Type() {}` 与 `Type {}`
  必须分别报错；
- [x] CTOR12：同 module 或同包但位于所属 type 外部的代码，不得调用该 type 的 `seal` 无参或有参
  构造；直接调用、显式对象字面量后缀和无参简写均必须报错。

#### 9.2.7 真实跨包构造写法与可用性正向用例

- [x] CTOR13：经过真实 package-public `.ft` 往返后，未显式声明构造函数的 provider type 仍通过
  公开隐式无参构造支持 CTOR07 的全部无参写法；
- [x] CTOR14：经过真实 package-public `.ft` 往返后，consumer 可通过公开无参或有参构造使用
  `Type()`、`Type(args)`、对应对象字面量后缀，以及仅适用于无参构造的 `Type { ... }` 简写；
- [x] CTOR15：provider type 的实例方法和静态方法均可使用自身 `seal` 构造；consumer 不能直接调用
  该构造，但可调用公开方法取得并使用 provider 已完成构造的实例。

#### 9.2.8 真实跨包构造写法与可用性负向用例

- [x] CTOR16：provider type 未显式声明任何构造函数时，经过真实 package-public `.ft` 往返后得到的
  隐式默认构造仍只接受零参数；consumer 的 `Type(arg)` 与 `Type(arg) { ... }` 必须报错；
- [x] CTOR17：provider 只显式声明公开有参构造时，consumer 不得重新获得默认无参构造；
  `Type()`、`Type() {}` 与 `Type {}` 必须分别报错；
- [x] CTOR18：provider 的全部构造均为 `seal` 时，`.ft` 必须保留“已显式声明构造”的事实；consumer
  不得获得隐式无参构造，也不得调用匹配的 `seal` 构造，直接调用、对象字面量后缀和无参简写均必须
  报错；
- [x] CTOR19：provider 同时存在 `open` 与 `seal` 构造重载时，consumer 只允许选择匹配的 `open`
  候选；`seal` 候选不得参与外部重载决议、遮蔽公开候选或导致重新生成默认无参构造。

CTOR13～CTOR19 必须由实际写出并重新读取 package-public `.ft` 的测试提供证据。只把 provider 与
consumer 源码 AST 一起交给同一进程语义分析，不属于真实跨包验收。

### 9.3 独立验收与交付 TODO

- [x] 核对类型和表达式规范，确认三阶段和未定义顺序边界；
- [x] 独立运行 G05，核对事件轨迹、参数求值、字段覆盖、`let` 三阶段绑定和跨包一致性；
- [x] 在 Codex 沙箱外为 G05 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G05 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。
- [x] Review 并决策 `ISSUE-G05-004` 的 package-public `.ft` 构造声明保留方案；
- [x] 在主符号表规范中定义 package-public `.ft` 必须保留全部显式构造声明及其原可见性；
- [x] 补齐 CTOR07～CTOR19，并修复真实跨包构造可用性缺口；
- [x] 独立运行 G05 补充用例，覆盖本包/跨包、正向/负向和真实 `.ft` 往返；
- [x] 在 Codex 沙箱外为 G05 补充交付独立执行 `make test`；
- [x] 执行 `git diff --check`，更新问题、专项结果、全量结果和交付结论。

### 9.4 独立交付记录

- 状态：已完成
- 实际文件与用例：
  - `fcts/fcts_bin/src/test_object_construction_order.ff`：覆盖 CTOR01～CTOR09、CTOR13～CTOR15、同包
    LET01～LET04 和跨包 LET10，逐项断言事件轨迹、调用次数、构造写法、owner 内 `seal` 构造能力、
    各阶段 `let` 最终值和默认零值；
  - `fcts/fcts_lib/src/g05_construction/g05_construction.ff`：提供跨包三阶段构造、隐式默认构造、公开
    构造重载、owner 内 `seal` 构造入口以及 LET10 四种公开 `let` 成员状态；
  - `fcts/fcts_bin/src/main.ff`：将 G05 行为测试接入完整 FCTS；
  - `src/parser/parser.h`、`src/semantic/analyzer.c`、`src/codegen/codegen.c`：在对象字面量 AST 保存语义
    阶段唯一选中的构造函数，并让直接 `Type { ... }` 与显式 `Type() { ... }` 复用同一构造调用路径；
  - `src/symbol/ft_write.c`：package-public `.ft` 为已收录 type 保留全部显式构造声明，包括原可见性为
    `seal` 的构造，不增加独立状态标记；
  - `test/codegen/test_codegen.c`：验证引用类型与 `@value` 类型的直接空字面量均生成且只生成一次已选
    无参构造函数调用，并编译生成 C；
  - `test/semantic/test_semantic.c`：覆盖同包 LET05～LET09 和 CTOR10～CTOR12，验证阶段间重复绑定、
    构造结束后赋值、隐式默认构造参数限制、显式有参构造取消默认构造及 type 外 `seal` 构造拒绝；
  - `test/symbol/test_symbol.c`：实际写出并重新读取 provider `.ft`，覆盖跨包 LET11～LET12 和
    CTOR13～CTOR19；同时验证完整构造集合、原可见性、公开成员查询过滤以及 consumer 构造候选；
  - `docs/specifications/feng-symbol-table.md`：定义 package-public `.ft` 的完整显式构造集合、可见性、
    签名依赖闭包、隐式默认构造判定和公开查询过滤规则；
  - [G01～G09 问题归档](./feng-language-conformance-coverage-hardening-issues/g01-g09.md)：记录
    `ISSUE-G05-001`～`ISSUE-G05-004` 的发现、分析、决策和验收状态。
- 本组专项结果：Semantic tests、Symbol tests、Codegen tests 均通过；完整 FCTS 1004/1004；
  CTOR01～CTOR19、LET01～LET12 全部通过；引用类型、`@value` 类型、同包与真实 `.ft` 跨包路径的
  构造阶段行为、构造可用性和诊断一致；
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 1004/1004、smoke 均为
  91/91、std 均为 601/601，Runtime、CLI、性能约束、增量构建与发布脚本检查均通过；
- 问题：`ISSUE-G05-001`、`ISSUE-G05-002` 已关闭；`ISSUE-G05-003` 经专项复验和第二次完整
  `make test` 均未复现，记录为一次性测试环境产物缺失，未据此修改产品代码；
  `ISSUE-G05-004` 已按人工确认方案修复并关闭；
- 交付结论：CTOR01～CTOR19 和 LET01～LET12 已形成完整行为与诊断证据；直接对象字面量简写会执行
  语义阶段已选中的显式无参构造函数；`let` 可在三个构造阶段中的任一阶段首次绑定，首次绑定后不可
  在后续阶段再次绑定，构造结束后无论是否显式绑定都不可再次赋值，同包与真实 `.ft` 跨包边界一致。
  package-public `.ft` 现以完整显式构造集合决定是否存在隐式默认构造，同时仅把可访问构造用于调用
  候选；本轮不增加运行时判断、分支、辅助调用或 runtime 接口，不变更 runtime 私有 ABI、公开 ABI
  或 `.ft` wire 格式，仅修正 package-public 声明内容选择；
- 建议 commit message：`fix: preserve explicit constructors in package metadata`

## 10 G06：字符串字面量语义

### 10.1 测试重点

验证双引号转义字符串和反引号原始字符串的最终字节、长度、索引和比较行为。

预期新增 `test_string_literal_semantics.ff`。非法字符串归 Lexer 诊断组 G11。

### 10.2 用例 TODO

- [x] STR01：`\n`、`\r`、`\t` 分别产生 `0x0A`、`0x0D`、`0x09`；
- [x] STR02：`"A\0B"` 的最终字节为 `0x41, 0x00, 0x42`，逻辑长度为 `3`，内部 NUL 不终止
  索引或比较；
- [x] STR03：`\xNN` 的小写、大小写混合及 `0x00`、`0x1B`、`0x7F`、`0x80`、`0xFF` 边界值
  均产生规定的单字节；
- [x] STR04：普通字符、常用转义、`\xNN`、NUL、反斜线和引号连续出现时，最终字节顺序与各自
  独立解码一致；
- [x] STR05：`\"` 和 `\\` 分别产生单个双引号和反斜线，并可与对应原始字符串按内容相等；
- [x] STR06：双引号字符串的 `length()`、`at()`、`getByte()`、`==` 与 `!=` 全部基于同一最终字节
  序列；其中 `at()` / `getByte()` 使用字节偏移，不使用字符串 `value[index]` 下标；
- [x] STR07：`"\x1b1"` 只消费 `\x1b` 的两位十六进制数字，最终字节为 `0x1B, 0x31`，生成 C
  不得把后续 `1` 贪婪并入同一个转义；
- [x] RAW01：反引号字符串将 `\n`、`\t`、`\x1b` 保留为反斜线和后续普通字符，不执行 `\` 转义；
- [x] RAW02：反引号字符串原样保留双引号，并与对应双引号转义字符串按内容相等；
- [x] RAW03：反引号字符串保留真实 LF、空格和制表符，逻辑长度与逐字节结果精确匹配；
- [x] RAW04：原始字符串的 `length()`、`at()`、`getByte()`、`==` 与 `!=` 全部基于处理后的最终
  字节序列，并可与相同字节的双引号字符串互相比较；
- [x] RAW05：原始字符串中的两个连续反引号产生一个字面量反引号；
- [x] RAW06：空原始字符串的逻辑长度为 `0`，并与 `""` 按内容相等；
- [x] RAW07：包含 CRLF 的内存源码经过 Lexer、Parser 和 Codegen 后仍保留 `0x0D, 0x0A`，不得
  归一化为 LF。

含 NUL 的用例不能依赖 C 风格终止字符串判断。

STR01～STR07、RAW01～RAW06 由新增 FCTS 行为文件覆盖；RAW07 由直接传入 CRLF 源码字节的 Codegen
测试覆盖，避免仓库检出时的文本换行转换干扰证据。`at()` / `getByte()` 仅作为观察字面量最终字节的
既有标准库能力，本组不新增字符串下标语法或新的运行时接口。

### 10.3 独立验收与交付 TODO

- [x] 对齐内建类型规范与两份字符串字面量规范，并为各用例列出精确期望字节；
- [x] 修复 `ISSUE-G06-001`，让所有字符串字面量共用长度感知的 C 字节编码，并验证生成 C 可编译；
- [x] 独立运行 G06，核对长度、逐字节值、索引和比较结果；
- [x] 在 Codex 沙箱外为 G06 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G06 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 10.4 独立交付记录

- 状态：已交付
- 实际文件与用例：
  - `docs/specifications/feng-builtin-type.md`、`docs/specifications/feng-string-escape.md`、
    `docs/specifications/feng-string-raw.md`：收敛字符串核心语义、两类字面量入口、显式字节长度和
    字节偏移访问规则；
  - `src/codegen/codegen.c`：新增统一的显式长度 C 字节字面量编码，非安全 ASCII 使用固定三位
    八进制转义，字符串表初始化和既有文本入口共用该实现；
  - `fcts/fcts_bin/src/test_string_literal_semantics.ff`、`fcts/fcts_bin/src/main.ff`：新增
    STR01～STR07、RAW01～RAW06 的 13 个独立行为用例；
  - `test/codegen/test_codegen.c`：新增 NUL、控制字节、高位字节、转义边界和内存 CRLF 源码的
    RAW07 结构与生成 C 编译用例；
  - [G01～G09 问题归档](./feng-language-conformance-coverage-hardening-issues/g01-g09.md)：记录
    `ISSUE-G06-001`～`ISSUE-G06-004` 的发现、分析、修复和验收结果。
- 本组专项结果：Codegen tests 通过；完整 FCTS 1017/1017，STR01～STR07、RAW01～RAW06 全部
  通过；RAW07 的生成结果精确保留 CRLF 并在 `-Werror` 下编译通过
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 1017/1017、smoke 均为
  91/91、std 测试均为 601/601；性能约束、增量构建、发布脚本及其余单元与 CLI 测试全部通过
- 问题：`ISSUE-G06-001`～`ISSUE-G06-004` 均已关闭
- 交付结论：转义字符串与原始字符串的最终字节、长度、访问和比较已有直接行为证据；生成 C 能无
  歧义地保存内部 NUL、控制字节、高位字节和 CRLF。修复只改变编译期文本编码，不增加运行时判断、
  分支、调用或数据，不变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式
- 建议 commit message：`fix: preserve exact string literal bytes`

## 11 G07：循环绑定实例与闭包捕获语义

### 11.1 测试重点

验证三段式 `for` 与 `for/in` 的绑定实例化次数、跨轮共享或隔离、`let` / `var` 可变性，以及引用捕获
后的可观察结果。三段式 `for` 采用已确认的 C# 模型：外部绑定与初始化子句绑定在整个循环中各自共享
同一实例。三段式 `for` 循环体内执行到的局部声明与 `for/in` 循环变量均按轮产生新绑定；`for/in`
循环变量等同于在每轮进入循环体前执行一次相应的局部声明。

规范依据统一引用 [流程控制规范](../specifications/feng-flow.md)第 6 节；闭包捕获方式引用
[函数规范](../specifications/feng-function.md)与[生命周期规范](../specifications/feng-lifetime.md)，迭代器
协议路径引用[迭代器规范](../specifications/feng-iterator.md)。本文不另行定义这些语义。

### 11.2 用例 TODO

#### 11.2.1 `for/in` 逐轮绑定

- [x] LOOP-BIND01：`for/in var` 修改循环变量不改变源数组；
- [x] LOOP-BIND02：`for/in var` 修改循环变量不改变后续轮值；
- [x] LOOP-BIND03：`for/in let` 被闭包捕获时每轮绑定独立；循环结束后调用闭包，结果分别等于各轮
  初始元素值；
- [x] LOOP-BIND04：先形成捕获 `for/in var` 循环变量的闭包，再在同轮修改该变量；循环结束后调用
  闭包，结果分别等于各轮修改后的值，并确认不同轮不共享绑定；
- [x] LOOP-BIND05：同一轮的多个闭包捕获同一 `for/in var` 绑定时共享修改结果，不同轮的闭包组彼此
  隔离。

#### 11.2.2 三段式 `for` 绑定

- [x] LOOP-BIND06：三段式 `for` 的非空初始化子句只执行一次；
- [x] LOOP-BIND07：三段式 `for` 引用循环外 `var` 时，各轮闭包共享该外部绑定；循环结束后调用闭包
  均读取最终值；
- [x] LOOP-BIND08：三段式 `for` 初始化子句声明 `var` 时，各轮闭包共享该初始化绑定；循环结束后
  调用闭包均读取最终值；
- [x] LOOP-BIND09：三段式 `for` 循环体内声明并捕获 `let` 快照时，每轮闭包分别读取各轮快照；
- [x] LOOP-BIND10：三段式 `for` 循环体内声明 `var`、先形成闭包再同轮修改时，每轮闭包分别读取
  该轮修改后的值，不同轮不共享绑定。

#### 11.2.3 统一 lowering 与性能证据

- [x] LOOP-BIND11：通过 `@iterable` / `@iterator` 协议进入 `for/in` 时，循环变量捕获结果与数组路径
  相同，合法 `let` / `var` 捕获均不得触发 `CE0102`；
- [x] LOOP-BIND12：未捕获的标量 `for/in let` / `for/in var` 不生成 capture cell、`feng_object_new`、
  运行时捕获判断或捕获导致的额外 ARC；
- [x] LOOP-BIND13：Codegen 正确记录 `for/in` 循环变量的 `let` / `var` 稳定性；可证明稳定的 `let`
  链路允许消除 assignment owner guard，`var` 链路保持必要的保守保护。

#### 11.2.4 `for/in` tuple 解构

- [x] LOOP-BIND14：`for let (a, b) in items` 按位置解构具名 tuple 元素，两个非空位置均为不可变的
  逐轮绑定；
- [x] LOOP-BIND15：`for var (a, b) in items` 的各分量可在本轮独立修改，修改不得写回源 tuple，
  也不得影响下一轮初始值；
- [x] LOOP-BIND16：tuple 解构空位只跳过对应位置且不创建名称；嵌套模式、单位置模式、位置数不匹配
  及非 tuple 元素分别在 Parser 或 Semantic 阶段报错；
- [x] LOOP-BIND17：数组与 `@iterable` / `@iterator` 协议路径支持相同的 tuple 解构语义；
- [x] LOOP-BIND18：tuple 解构分量被闭包捕获时，各分量分别遵循逐轮绑定身份；先捕获再修改同轮
  `var` 分量时，闭包读取修改后的本轮值，不同轮不共享；
- [x] LOOP-BIND19：未捕获 tuple 解构直接从本轮元素各分量建立绑定，不物化或复制完整 tuple，不
  增加运行时模式判断、堆分配或空位专属 ARC；只有实际捕获的非空分量使用既有 capture cell。

除 LOOP-BIND05 的同轮共享检查外，捕获用例必须在循环结束后调用闭包，避免只证明循环内即时值。
LOOP-BIND12 只排除捕获模型带来的新增开销，不排除元素类型按生命周期规范本来就需要的
retain/release，也不排除遍历期间保护源集合存活的既有 ARC。

### 11.3 已知问题与实现 TODO

- [x] 按 `ISSUE-G07-001` 修复合法 `for/in` 循环变量捕获触发 `CE0102`；数组与迭代器协议路径必须
  复用同一通用循环绑定 lowering，不得增加用例特判；
- [x] 保留 `CE0102` 对编译器内部不一致的防御作用，但合法源码不得再到达该诊断；全局错误码迁移
  不属于 G07；
- [x] 按 `ISSUE-G07-002` 为所有 `for/in` 循环绑定登记实际 `let` / `var` 元数据，恢复已有的编译期
  稳定性证明和 ARC 消除能力；
- [x] 捕获判断必须在编译期完成；未捕获路径保持零新增运行时分支、调用、分配和 ARC；
- [x] 被实际捕获的逐轮绑定复用普通闭包 capture-cell 与 ARC 机制，不新增 runtime 私有 ABI、公开
  ABI 或 `.ft` 格式。
- [x] Parser 在既有显式 `let` / `var` 循环绑定入口接受 tuple 模式，并复用普通一级解构的结构与
  语法约束；绑定关键字仍不可省略；
- [x] Semantic 复用普通 tuple 解构的类型、位置数和局部名称检查，不为数组或迭代器协议添加特判；
- [x] Codegen 在数组与迭代器协议共用的循环绑定 lowering 中按分量直接建绑定；不得先合成完整 tuple
  局部，不得增加运行时判断、分配、冗余复制或 ARC；
- [x] Parser dump、名称作用域及编译器辅助路径完整识别 tuple 模式中的每个非空名称；不变更 runtime
  私有 ABI、公开 ABI 或 `.ft` 格式。

### 11.4 独立验收与交付 TODO

- [x] 核对流程控制、绑定、函数、生命周期和迭代器主规范中的循环绑定模型；
- [x] 先完成并复验 `ISSUE-G07-001`、`ISSUE-G07-002`，再补齐全部 G07 用例；
- [x] 独立运行 G07，核对源集合、跨轮共享或隔离、同轮修改、逃逸闭包和迭代器协议结果；
- [x] 运行 G07 Codegen 专项，核对合法捕获不再触发 `CE0102`、未捕获路径零新增开销以及
  `let` / `var` 稳定性元数据；
- [x] 在 Codex 沙箱外为 G07 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G07 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。
- [x] 独立运行 LOOP-BIND14～LOOP-BIND19，核对 tuple 解构、非法模式、数组与迭代器、闭包身份和
  源值不变；
- [x] 运行追加 Codegen 专项，核对无完整 tuple 临时、无运行时模式判断、空位无绑定专属 ARC，且仅
  实际捕获的分量创建 capture cell；
- [x] 在 Codex 沙箱外为 G07 追加交付独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G07 追加问题并更新独立交付记录。

### 11.5 独立交付记录

- 状态：已交付（含 LOOP-BIND14～LOOP-BIND19 追加交付）
- 实际文件与用例：
  - `docs/specifications/feng-flow.md`、`docs/specifications/feng-tuple.md` 与中英文流程控制手册：收敛
    `for/in` 一级 tuple 解构的语法、逐轮绑定、空位、非法模式与性能约束；
  - `src/parser/parser.c`、`src/parser/parser.h`、`src/parser/dump.c`、`test/parser/test_parser.c`：在显式
    `let` / `var` 入口解析并输出 tuple 模式，保持三段式 `for` 消歧，并覆盖空位与非法语法；
  - `src/semantic/analyzer.c`、`test/semantic/test_semantic.c`：复用普通 tuple 解构检查，覆盖类型、位置
    数、可变性与局部名称；
  - `src/codegen/codegen.c`、`test/codegen/test_codegen.c`：数组与迭代器协议共用逐轮绑定 lowering，按
    非空分量直接建立普通局部或既有 capture cell，并覆盖固定布局、描述符尺寸布局、空位、闭包与
    `defer` 名称收集；
  - `fcts/fcts_bin/src/test_loop_binding.ff`：新增 LOOP-BIND14～LOOP-BIND18 的运行行为用例；
  - [G01～G09 问题归档](./feng-language-conformance-coverage-hardening-issues/g01-g09.md)：记录并关闭
    `ISSUE-G07-001`～`ISSUE-G07-012`。
- 本组专项结果：Parser、Semantic、Codegen tests 与生成 C 编译通过；完整 FCTS 1053/1053，
  LOOP-BIND01～LOOP-BIND18 全部通过；LOOP-BIND19 的结构断言覆盖数组与迭代器、固定与描述符尺寸
  布局、空位、按分量捕获及 `defer` 辅助路径，确认未合成完整 tuple 临时且只处理实际命名分量
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 1053/1053、smoke 均为
  90/90、std 测试均为 601/601；性能约束、增量构建、发布、安装及其余单元与 CLI 测试全部通过
- 问题：`ISSUE-G07-001`～`ISSUE-G07-012` 均已关闭
- 交付结论：显式 `for let (a, b) in items` 与 `for var (a, b) in items` 已完整交付；绑定关键字省略仍
  不支持。数组与迭代器协议共用通用逐轮绑定 lowering；固定布局直接访问字段，描述符尺寸布局复用
  既有 reified field offset，空位不创建绑定。编译期完成模式与捕获选择；除实际分量绑定按其类型本来
  需要的生命周期操作外，不新增运行时模式判断、分支、堆分配、完整 tuple 复制或空位 ARC。只有实际
  捕获的非空分量复用既有 capture-cell/ARC 机制；未变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式
- 建议 commit message：`feat: support tuple destructuring in for-in loops`

## 12 G08：循环控制转移语义

### 12.1 测试重点

验证 `continue`、`break` 和嵌套循环对更新表达式及控制目标的行为。

### 12.2 用例 TODO

- [x] LOOP-CTRL01：三段式 `for` 执行 `continue` 后，跳过本轮剩余循环体，更新子句恰好执行一次，
  随后再判断下一轮条件；同时断言循环体、更新和最终控制变量结果，避免只以“不死循环”间接证明；
- [x] LOOP-CTRL02：三段式 `for` 中两个不同分支分别汇入 `continue` 时，每条实际路径均只执行一次
  更新子句；同时核对两个分支的命中次数和未跳过路径结果；
- [x] LOOP-CTRL03：三段式 `for` 执行 `break` 后立即退出，当前轮不再执行更新子句；以循环外控制变量
  和独立更新计数直接证明；
- [x] LOOP-CTRL04：嵌套三段式 `for` 的 `continue` 只作用于最近一层；内层继续执行自身更新，外层本轮
  剩余语句与外层更新均按正常路径执行；
- [x] LOOP-CTRL05：嵌套三段式 `for` 的 `break` 只退出最近一层；内层当前轮不执行自身更新，外层本轮
  剩余语句与后续各轮继续执行。

### 12.3 独立验收与交付 TODO

- [x] 按 `ISSUE-G08-001` 补齐流程控制主规范中的更新步骤和最近循环规则，并同步中英文用户手册；
- [x] 独立运行 G08，同时核对事件次数、事件顺序和最终结果；
- [x] 在 Codex 沙箱外为 G08 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G08 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 12.4 独立交付记录

- 状态：已交付
- 实际文件与用例：
  - `docs/specifications/feng-flow.md` 与中英文流程控制手册：明确三段式 `for` 的更新顺序、三种循环
    的 `continue` 目标、`break` 跳过待执行步骤以及无标签的最近循环规则；
  - `fcts/fcts_bin/src/test_loop_control.ff`：新增 LOOP-CTRL01～LOOP-CTRL05，使用循环外控制变量、
    独立更新计数、循环体与尾部路径次数及访问顺序直接验证控制转移；
  - `fcts/fcts_bin/src/main.ff`：登记独立 G08 FCTS 入口；
  - 本文：细化 G08 用例与验收条件；
  - [G01～G09 问题归档](./feng-language-conformance-coverage-hardening-issues/g01-g09.md)：记录并关闭
    `ISSUE-G08-001`～`ISSUE-G08-003`。
- 本组专项结果：完整 FCTS 1058/1058；LOOP-CTRL01～LOOP-CTRL05 全部通过，其中 LOOP-CTRL01 以
  最后一个合法索引上的边界轮 `continue` 直接区分“先更新、后判断”与错误顺序，其余用例分别证明
  多个 `continue` 路径只执行一次更新、`break` 跳过更新，以及嵌套 `continue` / `break` 只作用于
  最近一层循环
- 本组沙箱外 `make test`：边界用例修正后重新通过；UBSan 与 normal 两阶段均完成，FCTS 均为
  1058/1058、smoke 均为 90/90、std 测试均为 601/601；性能约束、增量构建、发布、安装及其余单元
  与 CLI 测试全部通过
- 问题：`ISSUE-G08-001`～`ISSUE-G08-003` 均已关闭
- 交付结论：Feng 的无标签单层 `break` / `continue` 行为已由主规范和真实运行用例直接锁定，与当前
  产品实现一致；本组未修改产品代码，不增加运行时开销，也未变更 runtime 私有 ABI、公开 ABI 或
  `.ft` 格式
- 建议 commit message：`test: cover loop control transfer semantics`

## 13 G09：enum 默认值语义

### 13.1 测试重点

验证所有枚举项均显式指定值的 enum，其类型默认值仍为声明顺序中的第一项，不依赖第一项的底层
`int` 值是否为 `0`、正数或负数。

Feng 的 enum 只允许“全部省略值”或“全部指定值”，不支持 C 式部分指定。本组新增的显式取值
enum 必须为每个枚举项显式赋值；部分指定属于编译期错误，不进入默认值运行语义。全隐式 enum 从
`0` 开始递增且默认值为首项的行为已有 FCTS 直接证据，本组不重复新增同义用例。

### 13.2 用例 TODO

- [x] ENUM01：全显式取值 enum 的首项为正的非零值时，函数内无初始化器的 `let` / `var` 均以
  首项为默认值；
- [x] ENUM02：全显式取值 enum 的首项为负值时，函数内无初始化器的 `let` / `var` 均以首项为
  默认值；
- [x] ENUM03：模块级无初始化器的 enum `let` / `var` 首次访问后均保持全显式 enum 的首项默认值；
- [x] ENUM04：对象中无初始化器的 enum `let` / `var` 字段均以全显式 enum 的首项为默认值。

### 13.3 独立验收与交付 TODO

- [x] 核对 enum、绑定和字段默认值规范，并确认全隐式、全显式与禁止混用三类声明规则的已有直接
  证据；
- [x] 独立运行 G09，核对默认初始化后的 enum 成员和值；
- [x] 在 Codex 沙箱外为 G09 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G09 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 13.4 独立交付记录

- 状态：已交付
- 实际文件与用例：
  - 本文：明确 G09 的显式取值夹具必须为全部枚举项显式赋值；
  - [G01～G09 问题归档](./feng-language-conformance-coverage-hardening-issues/g01-g09.md)：记录并关闭
    `ISSUE-G09-001`～`ISSUE-G09-002`；
  - `fcts/fcts_bin/src/test_enum_default_semantics.ff`：新增两个全显式取值 enum，并实现
    ENUM01～ENUM04；
  - `fcts/fcts_bin/src/main.ff`：登记 G09 FCTS 入口。
- 本组专项结果：首次 `make fcts-tests` 因既有依赖调试 sidecar 一次性缺失而中止；同一命令复验
  通过，完整 FCTS 1062/1062，ENUM01～ENUM04 均实际执行；
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两个干净阶段均完成，FCTS 均为 1062/1062、
  smoke 均为 90/90、std 测试均为 601/601；性能约束、增量构建、发布、安装、bundled package、
  预构建工具链及其余单元与 CLI 测试全部通过；
- 问题：`ISSUE-G09-001`～`ISSUE-G09-002` 均已关闭；
- 交付结论：全显式取值 enum 在首项底层值为正非零或负数时，函数内、模块级及对象字段的无初始化器
  `let` / `var` 均直接取声明顺序中的首项，不错误退化为底层 `0`；本组未修改产品代码、既有测试或
  enum 主规范，不增加运行时开销，也未变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- 建议 commit message：`test: cover explicit enum default values`

## 14 G10：tuple 解构求值语义

### 14.1 测试重点

验证 tuple 解构右侧只求值一次，并验证跳过位置和可变绑定不会触发重复求值。

### 14.2 用例 TODO

- [ ] TUP01：函数返回 tuple 的解构右侧只求值一次；
- [ ] TUP02：含一个跳过位置的解构仍只调用工厂一次；
- [ ] TUP03：含多个跳过位置的解构仍只调用工厂一次；
- [ ] TUP04：`var` 解构出的绑定可独立修改且不重新求值右侧。

### 14.3 独立验收与交付 TODO

- [ ] 核对 tuple 和表达式规范，确认现有解构用例是否缺少副作用右侧；
- [ ] 独立运行 G10，核对工厂调用次数、分量值、跳过位置和绑定独立性；
- [ ] 在 Codex 沙箱外为 G10 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G10 问题；
- [ ] 填写本组实际文件、专项结果、全量结果和交付结论。

### 14.4 独立交付记录

- 状态：待实施
- 实际文件与用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: cover tuple destructuring evaluation`

## 15 G11：Lexer 稳定诊断

### 15.1 测试重点

验证用户输入可达的稳定 Lexer 诊断及其精确位置，不混入 Parser 或 Semantic 错误。

### 15.2 用例 TODO

- [ ] LEX01：非法转义序列的稳定诊断；
- [ ] LEX02：不完整十六进制转义的稳定诊断；
- [ ] LEX03：未闭合双引号字符串的稳定诊断；
- [ ] LEX04：未闭合反引号字符串的稳定诊断；
- [ ] LEX05：未闭合块注释的稳定诊断；
- [ ] LEX06：每类非法输入的最小合法邻界 token 序列。

### 15.3 独立验收与交付 TODO

- [ ] 建立稳定 `LE` 码到现有 Lexer test 的映射，确认真实缺口；
- [ ] 独立运行 G11，核对诊断码、token、行列、数量和 Lexer 阶段归属；
- [ ] 在 Codex 沙箱外为 G11 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G11 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 15.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close lexer diagnostic coverage gaps`

## 16 G12：Parser 稳定诊断

### 16.1 测试重点

验证用户输入可达的稳定 Parser 诊断和合法邻界 AST，不混入 Lexer 或 Semantic 错误。

### 16.2 用例 TODO

- [ ] PARSE01：模块声明结构错误；
- [ ] PARSE02：import 声明结构错误；
- [ ] PARSE03：修饰符或声明结构错误；
- [ ] PARSE04：对象字面量结构错误；
- [ ] PARSE05：tuple 结构错误；
- [ ] PARSE06：循环结构错误；
- [ ] PARSE07：易混淆结构对应的合法邻界 AST。

### 16.3 独立验收与交付 TODO

- [ ] 建立稳定 Parser `SE` 码到现有 Parser test 的映射，确认真实缺口；
- [ ] 独立运行 G12，核对诊断码、token、行列、数量、阶段和合法 AST；
- [ ] 在 Codex 沙箱外为 G12 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G12 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 16.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close parser diagnostic coverage gaps`

## 17 G13：名称绑定诊断

### 17.1 测试重点

验证名称声明、查找、遮蔽和可变性规则产生的稳定 Semantic 诊断。

### 17.2 用例 TODO

- [ ] BIND01：未声明名称引用；
- [ ] BIND02：同一作用域重复绑定；
- [ ] BIND03：不可变绑定被重新赋值；
- [ ] BIND04：名称超出词法作用域后被引用；
- [ ] BIND05：允许遮蔽时的合法邻界程序；
- [ ] BIND06：不同命名空间同名时的合法或非法边界。

### 17.3 独立验收与交付 TODO

- [ ] 建立本领域稳定诊断码到现有 Semantic test 的映射；
- [ ] 独立运行 G13，核对诊断码、位置、数量、阶段和绑定上下文；
- [ ] 在 Codex 沙箱外为 G13 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G13 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 17.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close name binding diagnostic gaps`

## 18 G14：表达式诊断

### 18.1 测试重点

验证运算、成员访问、索引、调用和赋值表达式产生的稳定 Semantic 诊断。

### 18.2 用例 TODO

- [ ] EXPR01：运算符操作数类型不合法；
- [ ] EXPR02：赋值左侧不是可写位置；
- [ ] EXPR03：成员访问目标或成员不合法；
- [ ] EXPR04：索引目标或索引类型不合法；
- [ ] EXPR05：被调用表达式不可调用；
- [ ] EXPR06：复合赋值的类型关系不合法；
- [ ] EXPR07：对应运算的最小合法邻界程序。

### 18.3 独立验收与交付 TODO

- [ ] 建立本领域稳定诊断码到现有 Semantic test 的映射；
- [ ] 独立运行 G14，核对诊断码、位置、数量、阶段和表达式上下文；
- [ ] 在 Codex 沙箱外为 G14 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G14 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 18.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close expression diagnostic gaps`

## 19 G15：函数诊断

### 19.1 测试重点

验证函数声明、参数、调用和返回规则产生的稳定 Semantic 诊断。

### 19.2 用例 TODO

- [ ] FUNC-D01：调用实参数量不匹配；
- [ ] FUNC-D02：调用实参类型不匹配；
- [ ] FUNC-D03：返回值类型不匹配；
- [ ] FUNC-D04：要求返回值的路径缺少返回；
- [ ] FUNC-D05：无返回值函数错误返回值；
- [ ] FUNC-D06：对应函数规则的最小合法邻界程序。

### 19.3 独立验收与交付 TODO

- [ ] 建立本领域稳定诊断码到现有 Semantic test 的映射，并排除 FUNC01～FUNC08 的重复项；
- [ ] 独立运行 G15，核对诊断码、位置、数量、阶段和函数上下文；
- [ ] 在 Codex 沙箱外为 G15 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G15 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 19.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close function diagnostic gaps`

## 20 G16：流程控制诊断

### 20.1 测试重点

验证条件、循环及控制转移规则产生的稳定 Semantic 诊断。

### 20.2 用例 TODO

- [ ] FLOW01：`break` 出现在不允许的上下文；
- [ ] FLOW02：`continue` 出现在不允许的上下文；
- [ ] FLOW03：条件表达式类型不合法；
- [ ] FLOW04：`for/in` 迭代目标不合法；
- [ ] FLOW05：循环绑定形式不合法；
- [ ] FLOW06：嵌套流程控制的最小合法邻界程序。

### 20.3 独立验收与交付 TODO

- [ ] 建立本领域稳定诊断码到现有 Semantic test 的映射；
- [ ] 独立运行 G16，核对诊断码、位置、数量、阶段和流程上下文；
- [ ] 在 Codex 沙箱外为 G16 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G16 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 20.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close control-flow diagnostic gaps`

## 21 G17：异常诊断

### 21.1 测试重点

验证抛出、捕获、重新抛出和异常声明规则产生的稳定 Semantic 诊断。

### 21.2 用例 TODO

- [ ] EXC01：抛出值不满足异常规则；
- [ ] EXC02：catch 声明或捕获类型不合法；
- [ ] EXC03：重新抛出出现在不允许的上下文；
- [ ] EXC04：函数异常声明与实现不一致；
- [ ] EXC05：调用方未满足异常传播要求；
- [ ] EXC06：嵌套捕获和传播的最小合法邻界程序。

### 21.3 独立验收与交付 TODO

- [ ] 建立本领域稳定诊断码到现有 Semantic test 的映射，避免重复已有异常专项；
- [ ] 独立运行 G17，核对诊断码、位置、数量、阶段和异常上下文；
- [ ] 在 Codex 沙箱外为 G17 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G17 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 21.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close exception diagnostic gaps`

## 22 G18：普通类型与值诊断

### 22.1 测试重点

验证非泛型普通类型关系、对象和值规则产生的稳定 Semantic 诊断。

### 22.2 用例 TODO

- [ ] TYPE01：绑定初始化值与声明类型不兼容；
- [ ] TYPE02：普通赋值两侧类型不兼容；
- [ ] TYPE03：显式类型转换不合法；
- [ ] TYPE04：对象构造参数或字段值不兼容；
- [ ] TYPE05：实例成员与 static 成员访问方式错误；
- [ ] TYPE06：对应类型关系的最小合法邻界程序。

### 22.3 独立验收与交付 TODO

- [ ] 建立本领域稳定诊断码到现有 Semantic test 的映射；
- [ ] 独立运行 G18，核对诊断码、位置、数量、阶段和类型上下文；
- [ ] 在 Codex 沙箱外为 G18 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G18 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 22.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close ordinary type diagnostic gaps`

## 23 G19：enum 诊断

### 23.1 测试重点

验证 enum 声明、成员和值关系产生的稳定 Semantic 诊断。

### 23.2 用例 TODO

- [ ] ENUM-D01：enum 成员重复声明；
- [ ] ENUM-D02：enum 显式成员值不合法；
- [ ] ENUM-D03：不同 enum 类型之间错误赋值；
- [ ] ENUM-D04：enum 与底层值之间错误转换；
- [ ] ENUM-D05：不存在的 enum 成员访问；
- [ ] ENUM-D06：对应 enum 规则的最小合法邻界程序。

### 23.3 独立验收与交付 TODO

- [ ] 建立 enum 稳定诊断码到现有 Semantic test 的映射；
- [ ] 独立运行 G19，核对诊断码、位置、数量、阶段和 enum 上下文；
- [ ] 在 Codex 沙箱外为 G19 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G19 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 23.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close enum diagnostic gaps`

## 24 G20：tuple 诊断

### 24.1 测试重点

验证 tuple 构造、访问、类型关系和解构规则产生的稳定 Semantic 诊断。

### 24.2 用例 TODO

- [ ] TUP-D01：tuple 分量数量不匹配；
- [ ] TUP-D02：tuple 分量类型不匹配；
- [ ] TUP-D03：tuple 解构目标数量不匹配；
- [ ] TUP-D04：tuple 解构目标形式不合法；
- [ ] TUP-D05：tuple 分量访问不合法；
- [ ] TUP-D06：含跳过位置解构的最小合法邻界程序。

### 24.3 独立验收与交付 TODO

- [ ] 建立 tuple 稳定诊断码到现有 Semantic test 的映射；
- [ ] 独立运行 G20，核对诊断码、位置、数量、阶段和 tuple 上下文；
- [ ] 在 Codex 沙箱外为 G20 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G20 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 24.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close tuple diagnostic gaps`

## 25 G21：数组与索引诊断

### 25.1 测试重点

验证数组声明、元素类型、维度和索引规则产生的稳定 Semantic 诊断。

### 25.2 用例 TODO

- [ ] ARRAY01：数组元素初始化类型不兼容；
- [ ] ARRAY02：数组赋值的元素类型或维度不兼容；
- [ ] ARRAY03：数组索引类型不合法；
- [ ] ARRAY04：多维数组索引数量不合法；
- [ ] ARRAY05：数组长度或维度声明不合法；
- [ ] ARRAY06：边界合法索引和多维访问的邻界程序。

### 25.3 独立验收与交付 TODO

- [ ] 建立数组和索引稳定诊断码到现有 Semantic test 的映射；
- [ ] 独立运行 G21，核对诊断码、位置、数量、阶段和数组上下文；
- [ ] 在 Codex 沙箱外为 G21 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G21 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 25.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close array diagnostic gaps`

## 26 G22：模块可见性诊断

### 26.1 测试重点

验证模块路径、import、alias、导出、可见性和文件级隔离产生的稳定 Semantic 诊断。

### 26.2 用例 TODO

- [ ] MODULE01：引用不存在的模块或包；
- [ ] MODULE02：引用模块中不存在的导出成员；
- [ ] MODULE03：访问不可见成员；
- [ ] MODULE04：惰性名称碰撞被实际引用后产生歧义；
- [ ] MODULE05：import alias 冲突被实际使用后产生诊断；
- [ ] MODULE06：在未 import 的同模块其他文件中使用短名被拒绝；
- [ ] MODULE07：完整模块路径访问的最小合法邻界程序。

### 26.3 独立验收与交付 TODO

- [ ] 建立模块领域稳定诊断码到现有 Semantic 和跨包测试的映射；
- [ ] 独立运行 G22，核对错误来源文件、诊断码、位置、数量、阶段和隔离边界；
- [ ] 在 Codex 沙箱外为 G22 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G22 问题；
- [ ] 填写本组映射、实际新增用例、专项结果和全量结果。

### 26.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: close module visibility diagnostic gaps`

## 27 G23：spec 适配诊断

### 27.1 测试重点

验证 spec 声明及其 fit 适配关系产生的稳定 Semantic 诊断。

### 27.2 用例 TODO

- [ ] SPEC01：spec 成员声明不合法；
- [ ] SPEC02：fit 缺少必需成员；
- [ ] SPEC03：fit 成员签名与 spec 要求不兼容；
- [ ] SPEC04：fit 目标类型不合法；
- [ ] SPEC05：重复或冲突 fit 关系；
- [ ] SPEC06：满足全部要求的最小合法邻界程序。

### 27.3 独立验收与交付 TODO

- [ ] 建立 spec 与 fit 稳定诊断码到现有测试的映射，禁止同构排列；
- [ ] 独立运行 G23，核对诊断码、位置、数量、阶段和 fit 上下文；
- [ ] 在 Codex 沙箱外为 G23 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G23 问题；
- [ ] 填写“不新增”依据或实际新增用例、专项结果和全量结果。

### 27.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: audit spec and fit diagnostics`

## 28 G24：复合类型诊断

### 28.1 测试重点

验证 union 和 intersection 所构成的复合类型在声明、类型关系和成员访问中产生的稳定 Semantic 诊断。

### 28.2 用例 TODO

- [ ] COMPOSITE01：union 声明或组成类型不合法；
- [ ] COMPOSITE02：intersection 声明或组成类型不合法；
- [ ] COMPOSITE03：值不满足 union 类型关系；
- [ ] COMPOSITE04：值不满足 intersection 类型关系；
- [ ] COMPOSITE05：复合类型成员访问不合法；
- [ ] COMPOSITE06：对应复合类型关系的最小合法邻界程序。

### 28.3 独立验收与交付 TODO

- [ ] 建立 union 与 intersection 稳定诊断码到现有测试的映射，禁止同构排列；
- [ ] 独立运行 G24，核对诊断码、位置、数量、阶段和复合类型上下文；
- [ ] 在 Codex 沙箱外为 G24 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G24 问题；
- [ ] 填写“不新增”依据或实际新增用例、专项结果和全量结果。

### 28.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: audit union and intersection diagnostics`

## 29 G25：泛型诊断

### 29.1 测试重点

验证泛型声明、实例化、约束和推断产生的稳定 Semantic 诊断。

### 29.2 用例 TODO

- [ ] GENERIC01：泛型参数重复或声明不合法；
- [ ] GENERIC02：泛型实参数量不匹配；
- [ ] GENERIC03：泛型实参不满足约束；
- [ ] GENERIC04：泛型参数无法完成推断；
- [ ] GENERIC05：显式实例化目标不合法；
- [ ] GENERIC06：对应泛型约束和推断的最小合法邻界程序。

### 29.3 独立验收与交付 TODO

- [ ] 建立泛型稳定诊断码到现有测试的映射，区分声明、实例化、约束和推断阶段；
- [ ] 独立运行 G25，核对诊断码、位置、数量、阶段和实例化上下文；
- [ ] 在 Codex 沙箱外为 G25 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G25 问题；
- [ ] 填写“不新增”依据或实际新增用例、专项结果和全量结果。

### 29.4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: audit generic diagnostics`

## 30 实施问题记录

### 30.1 问题记录文件

G01～G09 的既有问题集中归档；从 G10 起，每个交付组使用一个独立问题记录文件。详细问题正文只在
对应文件中维护，本文只维护文件索引以及统一的编号、状态、模板和处理规则。

- [G01～G09（已交付）问题记录](./feng-language-conformance-coverage-hardening-issues/g01-g09.md)；
- [G10 问题记录](./feng-language-conformance-coverage-hardening-issues/g10.md)；
- [G11 问题记录](./feng-language-conformance-coverage-hardening-issues/g11.md)；
- [G12 问题记录](./feng-language-conformance-coverage-hardening-issues/g12.md)；
- [G13 问题记录](./feng-language-conformance-coverage-hardening-issues/g13.md)；
- [G14 问题记录](./feng-language-conformance-coverage-hardening-issues/g14.md)；
- [G15 问题记录](./feng-language-conformance-coverage-hardening-issues/g15.md)；
- [G16 问题记录](./feng-language-conformance-coverage-hardening-issues/g16.md)；
- [G17 问题记录](./feng-language-conformance-coverage-hardening-issues/g17.md)；
- [G18 问题记录](./feng-language-conformance-coverage-hardening-issues/g18.md)；
- [G19 问题记录](./feng-language-conformance-coverage-hardening-issues/g19.md)；
- [G20 问题记录](./feng-language-conformance-coverage-hardening-issues/g20.md)；
- [G21 问题记录](./feng-language-conformance-coverage-hardening-issues/g21.md)；
- [G22 问题记录](./feng-language-conformance-coverage-hardening-issues/g22.md)；
- [G23 问题记录](./feng-language-conformance-coverage-hardening-issues/g23.md)；
- [G24 问题记录](./feng-language-conformance-coverage-hardening-issues/g24.md)；
- [G25 问题记录](./feng-language-conformance-coverage-hardening-issues/g25.md)。

### 30.2 编号与状态

问题编号采用 `ISSUE-Gxx-NNN`，必须归属一个发现组。问题影响其他组时，在问题正文中记录关联组，
不改变各组独立验收责任。

状态使用：

- 待确认；
- 待决策；
- 待修复；
- 待验证；
- 已关闭；
- 非问题。

分类使用：

- 规范歧义；
- 产品缺陷；
- 测试基础设施；
- 既有测试问题；
- 性能风险；
- ABI 或格式风险。

### 30.3 单个问题记录模板

#### ISSUE-Gxx-NNN：问题标题

##### 归属

- 发现组：
- 关联组：
- 发现用例：

##### 现象与结论

- 最小复现：
- 实际结果：
- 规范依据与预期结果：
- 结论及根因：

##### 决策与实施

- 建议方案：
- 是否涉及既有测试修改：
- 是否涉及运行时性能：
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：
- 人工决策与批准范围：
- 实际变更：

##### 验收与关闭

- 本组专项复验结果：
- 本组沙箱外 `make test` 结果：
- 关闭依据：

### 30.4 问题处理规则

1. 规范不能唯一决定预期时，标记“待决策”并暂停相关用例；
2. 修改既有测试、产品实现、ABI、格式或增加运行时开销前，必须取得明确人工批准；
3. 问题关闭后必须重新执行发现组的专项和全量回归；
4. 一个问题跨组时，各受影响组仍须分别完成专项、全量回归和独立交付。

## 31 最终反向映射与整体收口

G01～G25 均独立交付后，再执行以下整体核对：

- [ ] 从主规范稳定条款反向查找直接测试证据，记录规范位置、组号、用例号和测试文件；
- [ ] 从稳定诊断码反向查找直接 compiler test，记录阶段、测试函数和断言维度；
- [ ] 检查新增 FCTS 入口均已登记、每个测试实际执行且没有孤立文件；
- [ ] 检查每组自身的独立交付记录是否完整；
- [ ] 检查每组是否分别记录了沙箱外 `make test`，没有复用其他组结果；
- [ ] 收敛所属总计划，只保留各组范围、顺序、状态和本文链接。

最终核对不是第 26 个交付组，也不能替代任何组的专项或全量回归。如果发现缺口，必须重新打开归属组，
在该组内补齐后重新执行该组专项、沙箱外 `make test` 和独立交付。

## 32 Review 清单

- [ ] 每个组是否只有一个明确的测试重点；
- [ ] 每个组是否包含多个可执行、可验收的用例 TODO；
- [ ] G01～G10 的合法行为是否都属于稳定规范且可观察；
- [ ] G11～G25 的诊断分组边界是否清晰；
- [ ] 诊断 TODO 是否应继续细化或删除不属于稳定规范的候选项；
- [ ] 是否接受诊断组在映射完整时以“零新增用例 + 完整映射记录”交付；
- [ ] 是否确认每组必须独立执行专项、沙箱外 `make test` 和交付；
- [ ] 问题记录字段和需要人工决策的边界是否完整；
- [ ] 是否按 G01 → G25 顺序实施，或由人工另行指定优先级。
