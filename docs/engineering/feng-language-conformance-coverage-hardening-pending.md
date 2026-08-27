# Feng 语言正确性用例补齐实施文档

> 状态：G01～G06 已交付；G07 已完成语义与修复范围 Review、待实施；G08～G25 待 Review
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
Review 后只保留分组范围、实施顺序、状态和本文链接；用例 TODO、问题及验收记录只在本文维护。

本文默认只授权新增测试和必要的测试辅助代码，不概括授权修改既有测试、产品实现、runtime 私有 ABI、
公开 ABI、`.ft` 格式或语言规范。测试或 Review 暴露上述变更需求时，必须先在第 30 节记录问题；只有
在人工明确批准且对应组 TODO 写明范围后，必要变更才进入该组交付。任何新增运行时开销仍须单独明确
批准，不能由“修复问题”概括授权。

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

实施结果不能由主规范唯一确定时，必须先按第 30 节记录问题并暂停本组，由人工决定是否补充规范。
不得在测试或实现中自行选择行为。

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
  - 本文：记录 `ISSUE-G03-001` 的发现、决策、修复和验收结果。
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
  - 本文：记录 `ISSUE-G05-001`～`ISSUE-G05-004` 的发现、分析、决策和验收状态。
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
  - 本文：记录 `ISSUE-G06-001`～`ISSUE-G06-004` 的发现、分析、修复和验收结果。
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
同一实例；循环体内声明的绑定按每次执行独立。`for/in` 循环变量每轮独立。

规范依据统一引用 [流程控制规范](../specifications/feng-flow.md)第 6 节；闭包捕获方式引用
[函数规范](../specifications/feng-function.md)与[生命周期规范](../specifications/feng-lifetime.md)，迭代器
协议路径引用[迭代器规范](../specifications/feng-iterator.md)。本文不另行定义这些语义。

### 11.2 用例 TODO

#### 11.2.1 `for/in` 逐轮绑定

- [ ] LOOP-BIND01：`for/in var` 修改循环变量不改变源数组；
- [ ] LOOP-BIND02：`for/in var` 修改循环变量不改变后续轮值；
- [ ] LOOP-BIND03：`for/in let` 被闭包捕获时每轮绑定独立；循环结束后调用闭包，结果分别等于各轮
  初始元素值；
- [ ] LOOP-BIND04：先形成捕获 `for/in var` 循环变量的闭包，再在同轮修改该变量；循环结束后调用
  闭包，结果分别等于各轮修改后的值，并确认不同轮不共享绑定；
- [ ] LOOP-BIND05：同一轮的多个闭包捕获同一 `for/in var` 绑定时共享修改结果，不同轮的闭包组彼此
  隔离。

#### 11.2.2 三段式 `for` 绑定

- [ ] LOOP-BIND06：三段式 `for` 的非空初始化子句只执行一次；
- [ ] LOOP-BIND07：三段式 `for` 引用循环外 `var` 时，各轮闭包共享该外部绑定；循环结束后调用闭包
  均读取最终值；
- [ ] LOOP-BIND08：三段式 `for` 初始化子句声明 `var` 时，各轮闭包共享该初始化绑定；循环结束后
  调用闭包均读取最终值；
- [ ] LOOP-BIND09：三段式 `for` 循环体内声明并捕获 `let` 快照时，每轮闭包分别读取各轮快照；
- [ ] LOOP-BIND10：三段式 `for` 循环体内声明 `var`、先形成闭包再同轮修改时，每轮闭包分别读取
  该轮修改后的值，不同轮不共享绑定。

#### 11.2.3 统一 lowering 与性能证据

- [ ] LOOP-BIND11：通过 `@iterable` / `@iterator` 协议进入 `for/in` 时，循环变量捕获结果与数组路径
  相同，合法 `let` / `var` 捕获均不得触发 `CE0102`；
- [ ] LOOP-BIND12：未捕获的标量 `for/in let` / `for/in var` 不生成 capture cell、`feng_object_new`、
  运行时捕获判断或捕获导致的额外 ARC；
- [ ] LOOP-BIND13：Codegen 正确记录 `for/in` 循环变量的 `let` / `var` 稳定性；可证明稳定的 `let`
  链路允许消除 assignment owner guard，`var` 链路保持必要的保守保护。

除 LOOP-BIND05 的同轮共享检查外，捕获用例必须在循环结束后调用闭包，避免只证明循环内即时值。
LOOP-BIND12 只排除捕获模型带来的新增开销，不排除元素类型按生命周期规范本来就需要的
retain/release。

### 11.3 已知问题与实现 TODO

- [ ] 按 `ISSUE-G07-001` 修复合法 `for/in` 循环变量捕获触发 `CE0102`；数组与迭代器协议路径必须
  复用同一通用循环绑定 lowering，不得增加用例特判；
- [ ] 保留 `CE0102` 对编译器内部不一致的防御作用，但合法源码不得再到达该诊断；全局错误码迁移
  不属于 G07；
- [ ] 按 `ISSUE-G07-002` 为所有 `for/in` 循环绑定登记实际 `let` / `var` 元数据，恢复已有的编译期
  稳定性证明和 ARC 消除能力；
- [ ] 捕获判断必须在编译期完成；未捕获路径保持零新增运行时分支、调用、分配和 ARC；
- [ ] 被实际捕获的逐轮绑定复用普通闭包 capture-cell 与 ARC 机制，不新增 runtime 私有 ABI、公开
  ABI 或 `.ft` 格式。

### 11.4 独立验收与交付 TODO

- [ ] 核对流程控制、绑定、函数、生命周期和迭代器主规范中的循环绑定模型；
- [ ] 先完成并复验 `ISSUE-G07-001`、`ISSUE-G07-002`，再补齐全部 G07 用例；
- [ ] 独立运行 G07，核对源集合、跨轮共享或隔离、同轮修改、逃逸闭包和迭代器协议结果；
- [ ] 运行 G07 Codegen 专项，核对合法捕获不再触发 `CE0102`、未捕获路径零新增开销以及
  `let` / `var` 稳定性元数据；
- [ ] 在 Codex 沙箱外为 G07 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G07 问题；
- [ ] 填写本组实际文件、专项结果、全量结果和交付结论。

### 11.5 独立交付记录

- 状态：语义和已知问题修复范围已确认，待实施
- 实际文件与用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：`ISSUE-G07-001`、`ISSUE-G07-002` 待修复
- 建议 commit message：`fix: preserve loop binding identity across closures`

## 12 G08：循环控制转移语义

### 12.1 测试重点

验证 `continue`、`break` 和嵌套循环对更新表达式及控制目标的行为。

### 12.2 用例 TODO

- [ ] LOOP-CTRL01：`continue` 仍使循环更新表达式执行一次；
- [ ] LOOP-CTRL02：同一轮多分支汇入 `continue` 时更新表达式仍只执行一次；
- [ ] LOOP-CTRL03：`break` 跳出时不再执行更新表达式；
- [ ] LOOP-CTRL04：嵌套循环的 `continue` 只作用于最近一层；
- [ ] LOOP-CTRL05：嵌套循环的 `break` 只作用于最近一层。

### 12.3 独立验收与交付 TODO

- [ ] 核对流程控制规范中的更新步骤和最近循环规则；
- [ ] 独立运行 G08，同时核对事件次数、事件顺序和最终结果；
- [ ] 在 Codex 沙箱外为 G08 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G08 问题；
- [ ] 填写本组实际文件、专项结果、全量结果和交付结论。

### 12.4 独立交付记录

- 状态：待实施
- 实际文件与用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: cover loop control transfer semantics`

## 13 G09：enum 默认值语义

### 13.1 测试重点

验证 enum 首项具有显式值时，enum 类型默认值仍为首项。

### 13.2 用例 TODO

- [ ] ENUM01：enum 首项显式为正的非零值时，默认值仍为首项；
- [ ] ENUM02：enum 首项显式为负值时，默认值仍为首项；
- [ ] ENUM03：模块级 enum 绑定惰性初始化后保持显式首项默认值；
- [ ] ENUM04：对象字段的 enum 默认初始化保持显式首项默认值。

### 13.3 独立验收与交付 TODO

- [ ] 核对 enum、绑定和字段默认值规范，确认已有用例的直接证据缺口；
- [ ] 独立运行 G09，核对默认初始化后的 enum 成员和值；
- [ ] 在 Codex 沙箱外为 G09 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G09 问题；
- [ ] 填写本组实际文件、专项结果、全量结果和交付结论。

### 13.4 独立交付记录

- 状态：待实施
- 实际文件与用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
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

### 30.1 当前问题

#### ISSUE-G01-001：泛型类型不同闭合实例的 static 存储身份

##### 归属

- 发现组：G01；
- 关联组：无；
- 发现用例：MOD08 Review。

##### 现象与结论

- 现象：原 MOD08 使用“非泛型类型”限定，容易被理解为泛型类型的 static 绑定不满足单次初始化；
- 已确认规则：类型规范规定静态绑定复用模块顶层绑定的延迟初始化规则，没有为泛型类型声明例外；
- 人工决策：同一闭合类型实例共享同一 static 存储；同一泛型类型的不同闭合类型实例拥有彼此独立的
  static 存储、初始化状态和可变状态；
- 当前结论：删除“非泛型”限定，并新增 MOD11～MOD13 直接验证泛型 static 的单次初始化和闭合实例
  隔离；该规则已经写入类型主规范，G01 可以开始实施。

##### 决策与实施

- 状态：已关闭；
- 分类：规范歧义；
- 建议方案：已采纳，在类型主规范中明确 static 存储以闭合类型实例为单位；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：规则已经确定；后续实现若增加运行时开销，仍须单独取得人工批准；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：规则可能影响泛型 static 符号和跨包存储身份，
  若需要变更则须单独取得人工批准；
- 人工决策与批准范围：批准将泛型 static 存储定义为按闭合类型实例独立；本决策不自动批准产品实现、
  runtime 私有 ABI、公开 ABI、`.ft` 格式或性能开销变更；
- 实际变更：`feng-type.md` 已定义泛型 static 的闭合实例存储身份；实施文档已新增 MOD11～MOD13；
  总计划已删除旧 MOD 矩阵并改为引用本实施文档。

##### 验收与关闭

- 本组专项复验结果：MOD11～MOD13 的既有直接证据随 `make fcts-tests` 通过，942/942；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 阶段的 FCTS 均为 942/942；
- 关闭依据：类型主规范已明确闭合实例存储身份，MOD11～MOD13 直接证据和 G01 独立全量回归均通过。

#### ISSUE-G01-002：MOD06 完整模块路径访问在 Semantic 阶段无法解析

##### 归属

- 发现组：G01；
- 关联组：G02；
- 发现用例：MOD06。

##### 现象与结论

- 最小复现：在 `fcts_bin` 中声明 `import fcts_lib.g01_binding;`，随后读取
  `fcts_lib.g01_binding.g01CrossPackageCalls`；
- 实际结果：首次 `make fcts-tests` 在 Semantic 阶段报告 `AE0001: undefined identifier 'fcts_lib'`，
  并产生由未解析类型派生的 `AE0030`、`AE0512`；
- G01 当时的主规范依据与预期结果：模块规范 §4 只明确允许公开 `type` / `enum` 在类型引用位置免
  import 使用完整模块路径，因此 G01 按当时的主规范将新增用例改为短名访问；
- 后续 G02 复核：更早的模块优化工程文档已经把完整模块路径列为全部顶层名称的消歧方式，但对应
  `qualified_path` 测试实际只使用 import alias，主规范、工程文档、测试与实现未收敛；
- 当前规则：经人工确认，公开模块中的全部公开顶层声明均允许通过完整模块路径免 import 访问；G01
  的历史交付结果保持有效，规范和实现缺口归 G02 统一补齐。

##### 决策与实施

- 状态：已关闭；
- 分类：G01 阶段按当时主规范关闭；后续发现的规范不一致、既有测试问题和实现缺口归 G02；
- 建议方案：已按模块规范改为 import 后使用公开绑定短名；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：当前无；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：当前无；
- 人工决策与批准范围：当前不需要；若规范不能唯一确定访问形式，再请求人工决策；
- 实际变更：MOD06 将跨包绑定访问从完整模块路径改为无别名 import 引入的短名。

##### 验收与关闭

- 本组专项复验结果：第二次 `make fcts-tests` 中 MOD06 通过；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 阶段的 FCTS 均为 942/942；
- 关闭依据：修正后的 MOD06 已按 G01 当时的主规范真实编译、运行并通过独立全量回归；后续扩展并
  收敛的完整路径规则由 G02 独立验收，不复用 G01 结果。

#### ISSUE-G01-003：模块绑定初始化循环自然耗尽调用栈

##### 归属

- 发现组：G01；
- 关联组：无；
- 发现用例：MOD07。

##### 现象与结论

- 最小复现：模块绑定 A 的初始化器读取 B，B 的初始化器再读取尚未完成初始化的 A；
- 实际结果：第二次 `make fcts-tests` 中 MOD01～MOD06 通过，执行 MOD07 时进程收到 signal 11；
- 原规范依据与预期结果：发现问题时，模块主规范 §5 规定被循环访问的绑定在初始化表达式完成前保存
  类型默认值，因此原 MOD07 预期 A、B 初始化器各完成一次；
- 根因：模块绑定、非泛型 static 绑定和泛型 static 绑定的 `ensure_init` 都只使用一个布尔状态，且均在
  初始化表达式执行完成后才将状态写为 `true`。初始化器循环重入时，内层访问仍观察到 `false`，因此
  不断再次调用同一 `ensure_init`，最终栈溢出并表现为 signal 11；
- 规范复核：早期 `feng-top-binding-optimize.md` 已将该行为定义为等同普通函数循环调用并自然栈溢出；
  模块主规范中“读取默认值”的规则与该设计及当前实现不一致；
- 人工决策：保持当前实现，初始化循环不作特殊处理，实际进入循环后自然耗尽调用栈；模块主规范按
  当前实现收敛，static 绑定继续复用模块绑定规则；
- 当前结论：signal 11 是隔离用例在当前平台上自然耗尽调用栈的实际表现，不属于产品缺陷；语言不承诺
  具体平台信号或退出码，只规定不检测、不打断、不恢复初始化循环。
- 后续 Linux CI 现象：生成程序带 `-fsanitize=undefined` 时，UBSan 接管栈溢出的致命信号并以非零
  状态退出，父进程因此观察到 `WIFEXITED(status)`，原测试对 `WIFSIGNALED(status)` 的单一断言失败；
  这是测试验收条件与既定语言边界不一致，不是产品实现变化。

##### 决策与实施

- 状态：已关闭；
- 分类：规范歧义；
- 决策方案：保留现有 `ensure_init` 布尔状态和标记位置，不增加循环检测、三态、默认值实体化或异常
  恢复逻辑；
- 是否涉及既有测试修改：初次交付否；Linux CI 修复需要放宽 MOD07 的退出形态断言，已于
  2026-08-26 获得人工明确批准；
- 是否涉及运行时性能：否，保持当前访问和首次初始化开销；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 人工决策与批准范围：已批准保持自然栈溢出行为并按现状更新规范，不批准或要求产品实现变更；
- 实际变更：模块主规范已改为自然栈溢出；MOD07 从常规 FCTS 运行组移除，并由
  `test/cli/test_cli.c` 的隔离子进程用例自动验收。该用例只约束未成功返回，允许平台直接以信号终止
  或由 sanitizer 接管后以非零状态退出，同时排除 `execl` 失败；不约束具体平台信号或退出码。

##### 验收与关闭

- 本组专项复验结果：修复后沙箱外完整 `test_cli` 在原生路径与
  `UBSAN_OPTIONS=abort_on_error=0 FENG_CC=clang FENG_CC_FLAGS=-fsanitize=undefined` 路径下均通过；
  MOD07 子进程分别由信号终止和 sanitizer 非零退出，父测试进程继续完成其余 CLI 测试；
- 执行环境记录：沙箱内运行完整 `test_cli` 时，既有进程用例因沙箱进程限制返回
  `no such process`，失败位置不在 MOD07；按仓库测试约束改为沙箱外运行后完整通过，结论为测试执行
  环境限制，不是 Feng 产品或 MOD07 用例问题；
- 本组沙箱外 `make test` 结果：修复后通过；UBSan 与 normal 全量阶段均完成，FCTS 均为 976/976、
  smoke 均为 91/91、std 测试均为 601/601；
- 关闭依据：模块主规范、早期设计和当前实现已经收敛一致，MOD07 自动化隔离验收、常规 G01 专项及
  全量回归均通过。

#### ISSUE-G02-001：完整模块路径历史验收用例未覆盖其声明目标

##### 归属

- 发现组：G02；
- 关联组：G01、G22；
- 发现用例：既有 `test_lazy_ambiguity_resolved_by_qualified_path` 与 IMP07～IMP13 Review。

##### 现象与结论

- 历史工程文档 `feng-module-optimize-dev.md` 要求 `type`、`enum`、`spec`、`func`、`let` / `var`
  均可使用完整模块路径或 import alias 消歧义，并把
  `test_lazy_ambiguity_resolved_by_qualified_path` 记录为完整路径正例；
- 既有测试实际写成 `import demo.a as a;` 后调用 `a.compute()`，与紧随其后的 alias 正例重复，没有
  使用 `demo.a.compute()`；
- G02 最小实测确认，无 import 和无别名 import 两种情况下，`my.utils.math.add(1, 2)` 当前都在
  `my` 处报告 `AE0001: undefined identifier 'my'`；
- G02 实施时进一步确认，完整路径 enum 已在 Semantic 解析成功，但 Codegen 仍把路径首段当作普通
  标识符，报告 `CE0104: codegen: identifier 'fcts_lib' not found`；
- 原模块主规范只明确规定完整路径类型引用，工程文档、主规范、测试和实现存在不一致；
- 人工决策：公开模块中的全部公开顶层声明均支持三种名称访问形式：import 后短名、import alias
  限定名、无需 import 的完整模块路径；三种形式解析到同一声明和存储身份。

##### 决策与实施

- 状态：已关闭；
- 分类：规范歧义、既有测试问题、产品缺陷；
- 决策方案：以模块主规范统一定义三种形式；Semantic 和 Codegen 复用通用完整模块路径解析，不按
  声明类别添加互相独立的临时特判；
- 是否涉及既有测试修改：是；已人工批准在本次补完整用例并修正名为 `qualified_path`、实际只测
  alias 的既有测试；
- 是否涉及运行时性能：不应涉及；名称查找和路径收敛必须在编译期完成，不增加生成程序的运行时
  查找或分派；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：预期不涉及；若实施审计发现需要变更，必须
  停止并请求人工决策；
- 人工决策与批准范围：已批准对齐相关文档，补齐 compiler tests 与 FCTS，并修复实现缺口；未批准
  增加运行时开销、runtime 私有 ABI、公开 ABI 或 `.ft` 格式变更；
- 实际变更：模块主规范和中英文手册已收敛三种名称访问形式；Semantic 与 Codegen 已统一复用模块
  成员目标解析，完整路径顶层函数、函数值、模块级绑定和 enum 不再按表达式类别各自依赖 alias；既有
  `qualified_path` Semantic 用例已改为真正的完整模块路径；G02 FCTS 已覆盖三种形式及全部公开顶层
  声明类别。

##### 验收与关闭

- 本组专项复验结果：Semantic tests、Codegen tests 均通过；包含 IMP01～IMP27 的完整 FCTS
  969/969；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 两阶段均完成；
- 关闭依据：模块主规范、手册、Semantic、Codegen、compiler tests 与 FCTS 已收敛一致,专项和全量
  回归均通过。

#### ISSUE-G02-002：完整模块路径首段与局部值同名时的解析优先级未定义

##### 归属

- 发现组：G02；
- 关联组：G22；
- 发现用例：完整模块路径表达式解析链路审计。

##### 现象与结论

- 函数和模块级绑定的完整路径使用普通成员表达式语法，例如 `app.service.value`；
- 若当前表达式作用域同时存在名为 `app` 的局部值，该源码既可能表示局部值的连续成员访问，也可能
  表示公开模块 `app.service` 的顶层成员；
- import alias 已有急切重名规则，不存在同类歧义；类型位置也由类型语境直接区分；
- 发现时模块主规范没有定义完整模块路径首段与局部值同名时的解析优先级；
- 发现时的 G02 实现按精确完整模块路径优先解析,与局部值的既有词法作用域规则不一致。

##### 决策与实施

- 人工决策：局部值优先,符合词法作用域,也避免新增依赖后意外改变既有成员表达式语义；需要访问
  被局部值遮蔽的模块时使用 import alias；
- 决策边界：不扩展为文件级顶层声明优先；文件级顶层声明与无别名 import 引入名称继续按既有规则
  在使用时报告二义性；
- 运行时、ABI 与 `.ft` 影响：无,只调整编译期名称解析；
- 状态：已关闭。

##### 验收与关闭

- 实现与用例：Semantic 与 Codegen 的通用模块成员解析均先保留局部值成员链；IMP27 直接验证局部
  值结果未被同名模块路径改绑；
- 本组专项结果：完整 FCTS 969/969；
- 本组沙箱外 `make test`：通过；
- 关闭依据：人工决策已写入模块主规范和中英文手册,实现与 IMP27 一致,专项和全量回归均通过。

#### ISSUE-G03-001：右值重绑 base 或 receiver 后写回了新目标

##### 归属

- 发现组：G03；
- 关联组：无；
- 发现用例：ASN07。

##### 现象与结论

- 最小复现：先以 `holder.activeArray[index]` 或 `holder.activeCell.value` 作为左侧，再在右值
  函数中将 `holder.activeArray` 或 `holder.activeCell` 重新绑定到新引用；
- 实际结果：首次 `make fcts-tests` 中 ASN01～ASN06 通过，ASN07 失败；数组和成员两条路径均
  写入了右值重绑后的新目标，结果为 976 项中 975 通过、1 失败；
- 规范依据与预期结果：表达式主规范规定普通赋值先定位左侧可写位置，再求值右侧，
  最后写入已定位目标；因此应写入原数组和原 receiver；
- 结论及根因：产品缺陷。Codegen 只在 managed base/receiver 是拥有型临时结果时将其
  物化到局部变量；借用型成员读取仍保留包含原绑定的 C 表达式，最终写回时再次读取
  已被右值修改的绑定，没有固定左侧定位阶段观察到的引用值。

##### 决策与实施

- 状态：已关闭；
- 分类：产品缺陷、性能风险；
- 建议方案：通过递归的目标稳定性证明和保守的右值副作用证明先排除无需 ARC 的路径；
  只对无法证明原目标在右值期间仍被稳定持有的 borrowed managed base/receiver，建立
  语句级临时强引用，正常写回后立即释放，异常退出由现有 cleanup 链释放；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：是；稳定 `let`、`self`、递归不可重绑链路、已有拥有型临时结果，
  以及右值可证明不会触发用户代码或改变目标链路时不新增 retain/release；只有无法证明
  安全的路径增加临时强引用开销；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：当前分析为否；
- 人工决策与批准范围：已批准按上述证明驱动方案修复，批准无法证明安全的赋值目标
  增加语句级 retain/release 与现有 cleanup 节点；不批准无条件为所有成员或索引赋值
  增加 ARC，也不批准变更 runtime、ABI 或 `.ft` 格式；
- 收尾审计补充：只证明 index 与右值不改变状态仍不足以覆盖 aggregate 目标槽；现有
  `feng_aggregate_assign` / `feng_aggregate_take` 会在最终字节写入前 release 目标旧槽，旧槽终结器
  可能重绑并释放承载该槽的 receiver。直接 managed pointer 写入会先发布新指针再 release 旧值，
  标量写入不执行用户代码，二者没有该风险；擦除泛型写回可能在运行时选择 aggregate 分支，必须
  保守按 aggregate 处理。该补充仍属于“无法证明安全才保护”的已批准边界，不改变纯标量 RHS
  不新增 ARC 的结论；
- 实际变更：Codegen 为源码绑定记录可变性，并将稳定性沿 `let`、`self`、不可变成员和已有拥有型
  临时结果递归传播；对字面量、局部读取及其一元和二元组合做保守的无状态改写证明。只有 borrowed
  managed 目标无法证明稳定，且 index、右值或最终写回任一环节无法证明不会使目标失效时，才在左侧
  定位阶段保存并 retain 原引用；aggregate 与擦除泛型目标按不安全写回保守处理，标量和直接 managed
  pointer 写回按安全处理。目标写回后，按作用域后缀逆序释放后续右值临时量和目标保护，再移除对应
  编译期元数据；异常路径继续由既有 cleanup 链释放。该顺序符合生命周期主规范中临时值在表达式结束时
  release 的规则，并保持 cleanup 节点严格 LIFO；未新增 runtime 接口，未变更 ABI 或 `.ft` 格式。

##### 验收与关闭

- 本组专项复验结果：Codegen tests 通过；完整 FCTS 976/976，ASN01～ASN07 全部通过；Codegen
  断言同时证明稳定路径、已有拥有型临时结果和不安全因素均可排除的纯标量路径不生成目标保护，
  不稳定成员、索引及 aggregate 写回路径生成保护；ASN07 的终结器断言证明后续 RHS 临时量先于
  目标保护按 LIFO 在赋值语句结束时释放；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 976/976、smoke
  均为 91/91、std 均为 601/601，其余单元、CLI、性能约束、增量构建和发布脚本检查均通过；
- 关闭依据：实现符合表达式与生命周期主规范，ASN07 最小复现已修复，条件 ARC 性能边界已有 Codegen
  直接证据，专项和全量回归均通过。

#### ISSUE-G04-001：有符号整数除法溢出边界与零运行时开销约束

##### 归属

- 发现组：G04；
- 关联组：无；
- 发现用例：G04 边界 Review。

##### 现象与结论

- 现象：既有整数回绕条款未单独说明有符号整数 `MIN / -1` 与 `MIN % -1`；若将其解释为返回
  回绕结果，在操作数无法于编译期确定时，需要为动态有符号 `/` 与 `%` 增加运行时判断或等价
  lowering；
- 实现核对：当前 Codegen 直接生成 C `/` 与 `%`，没有该边界的运行时保护；
- 性能结论：为该边界提供确定回绕结果会影响无法静态排除该输入的动态有符号除法与取模，不符合
  G04 的零新增运行时开销要求；
- 人工决策：保留既有整数回绕条款，只在表达式主规范 §3.1 追加上述边界为未定义行为（UB）的说明；
  G04 不包含该 UB。

##### 决策与实施

- 状态：已关闭；
- 分类：规范歧义、性能风险；
- 决策方案：不增加运行时判断、辅助函数、runtime 接口或目标相关特殊处理；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：否，保持现有生成路径；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 人工决策与批准范围：已批准追加 UB 边界说明并将其排除在 G04 之外；未批准任何产品实现或运行时
  开销变更；
- 实际变更：表达式主规范追加 UB 边界；内建类型规范和中文手册只引用该权威规则；G04 明确不包含
  对应用例。

##### 验收与关闭

- 文档检查：`git diff --check` 通过；
- 测试：仅文档变更，不需要执行回归测试；
- 关闭依据：人工决策已明确语言边界、G04 范围与性能约束，未产生实现或测试变更。

#### ISSUE-G04-002：动态有符号回绕仍使用 C 有符号运算

##### 归属

- 发现组：G04；
- 关联组：无；
- 发现用例：INT05。

##### 现象与结论

- 最小复现：将动态 `i32` 参数 `1073741824` 与 `3` 相乘并返回 `i32`；
- 普通专项结果：G04 与完整 FCTS 均通过，结果为 984/984；
- UBSan 专项结果：INT05 的结果断言仍通过，但生成程序报告
  `signed integer overflow: 1073741824 * 3 cannot be represented in type 'int32_t'`；
- 初步根因：Codegen 直接使用相应 C 有符号类型的 `+`、`-`、`*` 运算，C 将超出表示范围的结果
  视为未定义行为，因此当前平台得到预期位模式不能构成 Feng 静默回绕语义的实现保证；
- 当前结论：产品缺陷，须使用适用于所有有符号整数宽度的通用 lowering 修复，并证明不增加运行时
  检查、分支、辅助函数调用或 runtime 接口。

##### 决策与实施

- 状态：已关闭；
- 分类：产品缺陷、性能风险；
- 建议方案：对全部定宽整数的 `+`、`-`、`*` 统一使用 GNU C / Clang 的
  `__builtin_add_overflow`、`__builtin_sub_overflow`、`__builtin_mul_overflow` 表达式 lowering；结果直接
  写入对应目标类型并忽略溢出标志，不生成运行时判断。该方案同时避开有符号溢出 UB 和窄整数的 C
  整数提升，不按具体值或具体用例增加特判；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：不得增加运行时开销；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：预期均不涉及；若分析结果不同则停止并请求
  人工决策；
- 人工决策与批准范围：已批准 G04 行为实现，但未批准运行时开销、runtime 私有 ABI、公开 ABI 或
  `.ft` 格式变更；
- 实际变更：在整数数值运算的公共生成路径中，将全部定宽整数的 `+`、`-`、`*` 统一 lowering 为
  `__builtin_add_overflow`、`__builtin_sub_overflow`、`__builtin_mul_overflow`，结果直接写入目标整数类型并
  丢弃溢出标志；普通二元运算与复合赋值共用该路径。未增加 runtime 接口、ABI 或 `.ft` 格式变更。

##### 验收与关闭

- 本组专项复验结果：完整 FCTS 984/984；严格 UBSan 专项以 `halt_on_error=1` 运行，984/984 且不再
  报告有符号整数溢出；Codegen tests 通过；macOS ARM64 `-O2` 机器码中对应运算收敛为原生
  `add`、`sub`、`mul`，没有溢出检查、条件分支或辅助函数调用；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 984/984，性能约束
  及其余全量检查通过；
- 关闭依据：通用 lowering 覆盖所有定宽有符号和无符号整数，INT01～INT06 与严格 UBSan 复验通过，
  优化后机器码证明未新增运行时开销，且未引入 runtime、ABI 或 `.ft` 变更。

#### ISSUE-G04-003：零开销 Codegen 结构断言发现函数体含 Feng 符号

##### 归属

- 发现组：G04；
- 关联组：无；
- 发现用例：G04 Codegen 零开销结构测试。

##### 现象与结论

- 现象：首次运行新增 Codegen 测试时，回绕函数体“不包含 `feng_`”的整体断言失败；
- 实际生成体：回绕函数和右移基线函数都包含既有的异常元数据、`feng_frame_push` / `feng_frame_pop`
  以及不可达路径 `feng_panic`；这些是普通 Feng 函数统一生成的序言和收尾，不由整数 lowering 引入；
- 当前结论：测试断言误判，不是产品性能问题。零开销断言应约束本次新增路径不生成条件判断或
  `feng_wrap` 一类运行时辅助函数，并确认编译内建在优化后只保留原生整数运算。

##### 决策与实施

- 状态：已关闭；
- 分类：测试基础设施；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：无；实际运行时调用集合与右移基线一致；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 实际变更：结构测试改为排除新增条件判断和回绕运行时辅助函数，同时保留编译内建与直接右移断言。

##### 验收与关闭

- 本组专项复验结果：Codegen tests 通过；结构断言确认回绕 lowering 不生成新增条件判断或
  `feng_wrap` 辅助函数；优化后机器码确认编译器内建只保留原生整数指令；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 两阶段及性能约束均通过；
- 关闭依据：原失败来自断言将普通 Feng 函数既有的统一序言、收尾和不可达异常路径误认为回绕开销；
  修正后的断言直接约束本次 lowering，生成代码对比和机器码检查均证明未新增运行时调用或分支。

#### ISSUE-G05-001：空对象字面量简写与无参构造的对照断言失败

##### 归属

- 发现组：G05；
- 关联组：无；
- 发现用例：CTOR04。

##### 现象与结论

- 最小复现：分别执行 `G05EmptyLiteralConstruction {}` 与
  `G05EmptyLiteralConstruction()`；类型包含一个带副作用的声明字段初始化器和一个带副作用的无参
  构造函数；
- 首次专项结果：CTOR01、CTOR02、CTOR03、CTOR05、CTOR06 通过，CTOR04 失败；完整 FCTS 为
  989/990；
- 预期：两种写法均执行一次声明字段初始化器和一次无参构造函数，事件轨迹均为 `12`，最终字段值
  相同；
- 实际结果：`G05EmptyLiteralConstruction {}` 的轨迹为 `1`、调用次数为 1、构造字段为 `0`；
  `G05EmptyLiteralConstruction()` 的轨迹为 `12`、调用次数为 2、构造字段为 `20`；
- 根因：语义阶段会为直接对象字面量目标校验并唯一选中无参构造函数，但该选择没有保存在对象字面量
  AST；Codegen 仅在字面量 target 本身是调用表达式时读取构造函数，因此 `Type {}` 路径只执行声明
  字段初始化，没有调用已声明的无参构造函数；
- 当前结论：产品缺陷；应把语义阶段已经完成的构造函数选择作为通用对象字面量元数据传递给
  Codegen，不在 Codegen 中重新执行可见性或重载决议。

##### 决策与实施

- 状态：已关闭；
- 分类：产品缺陷；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：只恢复规范要求但当前缺失的无参构造函数调用，不向已正确的构造路径增加额外
  检查、分支或辅助调用；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 修复方案：对象字面量 AST 保存语义阶段唯一选中的构造函数声明；直接 `Type { ... }` 和
  `Type {}` 的 Codegen 复用该声明定位既有 `UserMethod`，与显式 `Type() { ... }` 共用成员初始化、
  构造函数调用和字面量写入流程；
- 实际变更：对象字面量 AST 新增语义阶段解析结果字段；语义分析在直接目标和调用目标两条路径保存
  唯一选中的构造函数声明；Codegen 统一从该声明定位既有 `UserMethod`，直接 `Type { ... }`、
  `Type {}` 与显式调用字面量共用构造函数调用路径。隐式构造函数仍以空选择表示，行为不变。

##### 验收与关闭

- 本组专项复验结果：Codegen tests 通过；完整 FCTS 990/990，CTOR01～CTOR06 全部通过；CTOR04
  同时覆盖引用类型和 `@value` 类型，两种 `Type {}` 均与 `Type()` 具有相同轨迹、调用次数和字段值；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 两阶段及性能约束均通过；
- 关闭依据：语义阶段选择已被直接传递给 Codegen，直接空对象字面量现会执行且只执行一次显式无参
  构造函数；隐式构造路径未改变，且未新增运行时检查、分支、辅助调用或 ABI/格式变更。

#### ISSUE-G05-002：对象字面量构造函数 Codegen 结构测试未通过语义分析

##### 归属

- 发现组：G05；
- 关联组：无；
- 发现用例：CTOR04 Codegen 结构测试。

##### 现象与结论

- 现象：为直接对象字面量补充的引用类型和值类型 Codegen 结构测试，在
  `feng_semantic_analyze` 阶段返回失败，尚未进入构造函数调用结构断言；
- 实际诊断：两个辅助函数声明为 `open`，但分别返回 module-private 的 `RefProbe` 与 `ValueProbe`，
  因而违反公开声明不得暴露更窄可见性类型的既有规则；
- 当前结论：新增测试源码问题，不是产品回归；辅助函数只在同一测试模块内用于定位生成函数体，无需
  公开。

##### 决策与实施

- 状态：已关闭；
- 分类：测试基础设施；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：否；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 修复方案：移除两个测试辅助函数的 `open`，保持类型与函数均为 module-private；
- 实际变更：两个 Codegen 测试辅助函数改为 module-private，与其返回类型的可见性一致；未修改任何
  既有测试用例。

##### 验收与关闭

- 本组专项复验结果：Codegen tests 通过，引用类型与 `@value` 类型的直接空字面量结构断言及生成 C
  编译均通过；完整 FCTS 990/990；
- 本组沙箱外 `make test` 结果：通过；UBSan 与 normal 两阶段及性能约束均通过；
- 关闭依据：测试辅助函数不再违反既有公开可见性规则，结构测试能够进入并完成目标断言；产品代码
  无需针对该诊断增加特殊处理。

#### ISSUE-G05-003：normal 阶段 FCTS 缺少标准库调试侧文件

##### 归属

- 发现组：G05；
- 关联组：全量回归测试基础设施；
- 发现用例：沙箱外 `make test` 的 normal 阶段 FCTS。

##### 现象与结论

- 现象：normal 阶段 smoke 91/91、CLI 检查及 std 601/601 均通过，随后执行 FCTS 时中止；
- 实际诊断：`feng run ./fcts/fcts_bin` 写入 `fcts_bin.fd` 时，无法读取依赖调试侧文件
  `std/std/build/macos-arm64/lib/libstd.a.fd`，该文件不存在；
- 后续分析：失败后立即执行同一 `make fcts-tests`，标准库侧文件正常重建且 FCTS 990/990；随后从
  干净阶段重新执行完整沙箱外 `make test`，UBSan 与 normal 两阶段均正常生成并读取该侧文件，未在
  同一点或其他阶段复现；
- 当前结论：一次性回归产物缺失，未发现可复现的 G05 产品缺陷或全量回归目标依赖缺口。现有证据
  不足以认定具体外部干扰来源，因此不据此修改产品或构建代码。

##### 决策与实施

- 状态：已关闭；
- 分类：测试基础设施；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：否；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 处理方案：先用同一 FCTS 入口验证侧文件可正常重建，再从干净阶段重跑完整 `make test`；仅在稳定
  复现并获得根因证据后才修改构建实现；
- 实际变更：无；未对不可复现的产物缺失添加重试、特判或其他掩盖性处理。

##### 验收与关闭

- 本组专项复验结果：失败后同一 `make fcts-tests` 立即通过 990/990，依赖侧文件存在且可正常读取；
- 本组沙箱外 `make test` 结果：首次执行停在 normal 阶段 FCTS；第二次从干净阶段完整执行通过，
  UBSan 与 normal 两阶段、性能约束及发布检查均完成；
- 关闭依据：同一入口专项复验和第二次完整干净回归均通过，问题不可复现且没有产品或构建依赖缺陷
  证据；保留本记录用于后续若再次出现时比对，不引入未经证实的实现变更。

#### ISSUE-G05-004：全 `seal` 构造在 package-public `.ft` 中丢失显式构造事实

##### 归属

- 发现组：G05；
- 关联组：符号表导出与真实跨包构造；
- 发现用例：CTOR18。

##### 现象与结论

- 最小复现：provider 导出一个公开 type，该 type 显式声明 `seal func TypeName() {}`；consumer 经过
  真实 package-public `.ft` 导入后直接执行 `TypeName()`；
- 预期：显式 `seal` 构造取消隐式默认构造，且 consumer 不在所属 type 内，不得直接构造；
- 实际结果：provider 与 consumer 的真实包构建均成功，consumer 被错误视为可以使用隐式公开无参
  构造；
- 根因：package-public writer 不收录普通 `seal` 构造；consumer 恢复出的 type 构造成员数为零，
  现有统一构造校验据此判定该 type 未显式声明构造，并允许零参数隐式构造；
- 影响范围：只要 provider type 的全部显式构造均为 `seal`，consumer 就会丢失“该 type 已显式声明
  构造”的事实；即使 provider 只声明 `seal` 有参构造，consumer 仍可能错误得到不存在的隐式公开
  无参构造；
- 当前结论：可复现的产品缺陷；源码 AST 同进程分析能够看到 `seal` 构造并正确拒绝，缺口只发生在
  package-public `.ft` 过滤之后。

##### 决策与实施

- 状态：已关闭；
- 分类：产品缺陷、ABI 或格式风险；
- 是否涉及既有测试修改：否；新增 CTOR07～CTOR19，不修改既有用例；
- 是否涉及运行时性能：否；构造可用性继续在编译期决定，不增加运行时检查、分支或辅助调用；
- 是否涉及 runtime 私有 ABI 或公开 ABI：否；
- 是否涉及 `.ft`：是；推荐改变 package-public 声明选择，但不新增 wire kind、flag、attr、section 或
  格式版本；
- 推荐方案：package-public `.ft` 收录所属公开 type 的全部显式构造声明，包括 `seal` 构造；沿用现有
  constructor kind 和 `public = 0` 可见性事实，只导出签名与编译所需依赖，不导出函数体。consumer
  继续使用同一套“完整构造集合决定是否存在隐式默认构造、可见构造集合参与重载决议”的通用逻辑；
- 不推荐方案：新增独立 `hasExplicitConstructor` 标记。该标记与构造声明集合表达同一事实，会形成
  两套需要保持一致的状态，并要求 Parser、AST、符号图、writer、reader、导入 AST、Semantic 和
  Codegen 分别消费；旧 consumer 忽略新标记后仍会错误编译，因此不能把它作为可静默忽略的同 major
  `.ft` 扩展。若最终选择标记方案，必须另行决策格式版本和旧 consumer 的显式拒绝策略；
- 禁止同时采用“导出全部构造声明”和“新增显式构造标记”；前者已经完整表达语义事实，后者属于冗余
  状态；
- 人工决策：采用推荐方案；全部显式构造进入 package-public `.ft`，保持原可见性，不增加独立标记。
- 实际变更：package-public writer 对构造声明不再按可见性过滤；reader、构造校验和公开成员查询继续
  使用现有 constructor kind、`public` 可见性及签名，无新增 wire 字段、格式版本或运行时逻辑。

##### 验收与关闭

- [x] 人工确认采用“全部显式构造进入 package-public `.ft`，保持原可见性且不增加标记”的方案；
- [x] 主符号表规范先定义构造声明选择、可见性恢复、签名依赖闭包和用户符号查询过滤规则；
- [x] 真实 `.ft` 往返后，consumer 能看到 `seal` 构造事实，但普通成员枚举、补全和调用候选仍不得把
  `seal` 构造暴露为公开 API；
- [x] CTOR07～CTOR19 全部通过，其中 CTOR18 必须覆盖全 `seal` 无参、全 `seal` 有参和全 `seal`
  重载集合；
- [x] G05 专项、沙箱外 `make test` 与 `git diff --check` 全部通过；
- [x] 关闭依据：Symbol tests 通过真实 package-public `.ft` 写入和读取确认全 `seal`、全 `seal` 有参及
  `open + seal` 重载集合完整恢复，公开成员查询不返回 `seal` 构造；Semantic tests 精确确认外部调用
  被拒绝且公开候选正常选择；CTOR07～CTOR19、完整 FCTS 1004/1004 及沙箱外全量 `make test` 均通过。

#### ISSUE-G06-001：生成 C 的十六进制转义贪婪消费后续字符

##### 归属

- 发现组：G06；
- 关联组：无；
- 发现用例：STR02、STR03、STR07、RAW07。

##### 现象与结论

- 最小复现：双引号字面量 `"\x1b1"` 解码后应为 `0x1B, 0x31`；含 NUL 后接十六进制字符的
  `"\0B"` 同样要求两个字节保持独立；
- 当前生成结果：字符串表初始化对控制字节使用 C `\x%02x`，因此上述字节会生成 `"\x1b1"` 或
  `"\x00B"`。C 的十六进制转义会继续消费后续十六进制字符，不能保持 Feng `\xNN` 固定两位的
  语义；`0x80`～`0xFF` 当前还会直接写入生成 C 源文件，结果受宿主源码编码影响；
- 规范依据与预期结果：字符串转义规范规定 `\xNN` 恰好消费两位并可产生 `0x00`～`0xFF` 任意
  字节；原始字符串规范规定 CRLF 等源字节必须原样保留；
- 根因：字符串表初始化使用一套只按字符值决定文本写法的局部循环，没有复用文件顶部已有的 C
  字符串转义抽象；既有抽象又只接受 NUL 结尾文本，不能表达含内部 NUL 的显式长度字节序列；
- 当前结论：产品缺陷。必须使用通用的长度感知字节编码修复，不能针对 `\x1b1`、NUL 或单个高位
  字节增加用例特判。

##### 决策与实施

- 状态：已关闭；
- 分类：产品缺陷；
- 修复方案：增加统一的“字节指针 + 显式长度”C 字符串字面量编码函数；安全 ASCII 直接输出，双引号
  和反斜线使用标准短转义，其余字节统一使用固定三位八进制转义。三位八进制最多消费三位，天然隔离
  后续字符，并保证生成 C 只包含稳定 ASCII；现有 NUL 结尾文本入口作为薄封装复用该函数；
- 是否涉及既有测试修改：否；新增 G06 FCTS 与 Codegen 回归，不改变既有用例；
- 是否涉及运行时性能：否；只改变编译期生成 C 的文本编码，不增加运行时判断、分支、调用或数据；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 实际变更：`src/codegen/codegen.c` 已增加显式长度的 C 字节字面量编码函数；双引号和反斜线使用
  短转义，其他非安全 ASCII 字节使用固定三位八进制转义。字符串表初始化直接传递 AST 中记录的
  字节长度，既有 NUL 结尾文本入口作为薄封装复用同一实现。

##### 验收与关闭

- [x] STR02、STR03、STR07 和 RAW07 的生成 C 使用无贪婪、ASCII 稳定的字节表示；
- [x] 新增 Codegen 测试直接断言 NUL、控制字节、高位字节与 CRLF 的生成形式，并在 `-Werror` 下
  编译生成 C；
- [x] G06 FCTS、Codegen tests、沙箱外 `make test` 与 `git diff --check` 全部通过。

#### ISSUE-G06-002：字符串主规范与后续字面量能力未收敛

##### 归属

- 发现组：G06；
- 关联组：无；
- 发现用例：G06 规范核对。

##### 现象与结论

- 现象：内建类型规范仍写着字符串字面量只能使用双引号，重复列出的转义表缺少 `\xNN`，并把
  `length()` 返回类型写为 `i64`；字符串转义规范末尾还残留一段误写入的 shell 命令文本；
- 历史事实：反引号字符串和 `\xNN` 已分别有专门主规范及实现；提交
  `f234af3f refactor(std,test): migrate length/index/size types from i64/i32 to int` 已将标准库字符串
  `length()`、`at()` 与相关索引参数统一迁移到 `int`，但内建类型规范未同步；
- 当前结论：规范收敛缺口，不需要新增语言决策。内建类型规范只保留字符串核心语义并引用两份字面量
  主规范，返回类型按既有 `int` 迁移事实对齐，同时删除无效 shell 文本。

##### 决策与实施

- 状态：已关闭；
- 分类：规范歧义；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：否；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 实际变更：内建类型规范已引用双引号转义与反引号原始字符串主规范，明确内部 NUL、字节长度、
  内容比较和字节偏移访问，并将 `length()` 返回类型对齐为 `int`；两份字面量规范已明确统一的长度
  感知发码约束，转义规范中的无效 shell 文本已删除。

##### 验收与关闭

- 文档检查：`git diff --check` 通过；
- 关闭依据：所有调整均由既有专门规范、现行标准库签名和已提交迁移事实唯一确定，没有引入新语义。

#### ISSUE-G06-003：长度感知编码接入后重复追加结尾引号

##### 归属

- 发现组：G06；
- 关联组：无；
- 发现用例：实现后静态复核。

##### 现象与结论

- 现象：新增长度感知编码函数已经输出完整的首尾 C 双引号，字符串表初始化调用点仍沿用旧局部循环
  的收尾格式并再次追加一个双引号；
- 影响：若不修复，生成的 `feng_string_literal()` 第一个参数后会出现多余空字符串字面量，调用参数
  分隔位置不符合预期；
- 根因：公共函数的职责从“只编码内容”提升为“输出完整 C 字符串字面量”后，调用点旧收尾未同步
  删除；
- 当前结论：本轮实现接入错误，静态复核已在运行测试前发现；应删除调用点的重复结尾引号，不改变
  公共编码职责。

##### 决策与实施

- 状态：已关闭；
- 分类：产品缺陷；
- 是否涉及既有测试修改：否；
- 是否涉及运行时性能：否；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 实际变更：字符串表初始化调用点已删除旧路径遗留的重复结尾引号，只由统一编码函数输出完整 C
  字节字面量。

##### 验收与关闭

- [x] Codegen 结构断言匹配且生成 C 在 `-Werror` 下编译通过；
- [x] G06 专项与沙箱外完整回归通过。

#### ISSUE-G06-004：整份生成 C 的 ASCII 附加断言失败

##### 归属

- 发现组：G06；
- 关联组：无；
- 发现用例：G06 Codegen 结构测试。

##### 现象与结论

- 现象：NUL、控制字节、高位字节和 CRLF 的固定八进制结构断言均已通过，随后扫描整份生成 C 并
  要求所有字节小于 `0x80` 的附加断言失败；
- 定位结果：首个非 ASCII 字节来自既有固定生成头注释 `Feng generated code — do not edit.` 中的
  Unicode 破折号，不来自目标字符串表；
- 当前结论：本轮新增测试范围过宽。G06 只需证明目标字节在 C 字面量中的编码稳定，不能要求与该
  语义无关的整份生成文件必须是 ASCII。

##### 决策与实施

- 状态：已关闭；
- 分类：测试基础设施；
- 是否涉及既有测试修改：否；仅分析本轮新增断言；
- 是否涉及运行时性能：否；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 实际变更：删除本轮新增的整份生成 C ASCII 扫描；保留对 `0x80`、`0xFF` 固定三位八进制编码的
  精确结构断言以及生成 C 编译检查。

##### 验收与关闭

- [x] 已定位非 ASCII 字节来自既有固定生成头注释；
- [x] 已保留能够直接证明 `0x80` 与 `0xFF` 已安全编码的最小结构断言；
- [x] Codegen tests 与生成 C 编译通过。

#### ISSUE-G07-001：合法 `for/in` 循环变量捕获未降级为 capture cell

##### 归属

- 发现组：G07；
- 关联组：无；
- 发现用例：LOOP-BIND03、LOOP-BIND04、LOOP-BIND05、LOOP-BIND11 的实施前探针。

##### 现象与结论

- 最小复现：在 `for let value in values` 或 `for var value in values` 的循环体内形成直接读取
  `value` 的 Lambda；
- 实际结果：Semantic 接受该捕获，Codegen 随后报告
  `CE0102: codegen: lambda capture 'value' was not lowered to a capture cell`；`let` 与 `var` 结果相同；
- 规范依据与预期结果：流程控制规范规定 `for/in` 每轮产生独立循环绑定，函数和生命周期规范规定
  Lambda 引用捕获已经确定的绑定实例；因此该源码合法，逃逸闭包必须分别保存各轮绑定；
- 根因：数组 `for/in` 与迭代器协议 `for/in` 都由专用 lowering 手工声明并注册循环局部，没有复用
  普通绑定的捕获需求判断和 capture-cell 注册；Lambda emitter 只接受已经准备好的 capture cell，因而
  在合法源码上触发内部阶段不一致防线；
- 当前结论：产品缺陷。必须修复循环绑定 lowering，不能把 `CE0102` 作为用户错误，也不能针对用例
  名称、元素类型或数组路径增加特判。

##### 决策与实施

- 状态：待修复；
- 分类：产品缺陷；
- 建议方案：抽取统一的循环绑定 lowering，输入绑定名称、类型、初始值、所有权和 `let` / `var`
  元数据；在编译期根据捕获事实选择普通局部或 capture cell，并由数组与迭代器协议路径共同复用；
- 是否涉及既有测试修改：否；新增 G07 FCTS 与 Codegen 回归；
- 是否涉及运行时性能：被实际捕获的逐轮绑定复用现有普通闭包 capture-cell/ARC 路径；未捕获路径不得
  新增运行时判断、调用、分配或 ARC；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；不得新增或修改这些边界；
- 人工决策与批准范围：已批准修复合法 `for/in` 捕获并补齐数组、迭代器协议和三段式对照用例；批准
  范围要求未捕获路径零新增开销，不授权修改既有测试、runtime 私有 ABI、公开 ABI、`.ft` 格式或
  全局错误码体系；
- 实际变更：待实施。`CE0102` 可继续保留为编译器内部不一致防线；G07 只保证合法路径不再触发，
  不承担错误码全局迁移。

##### 验收与关闭

- 本组专项复验结果：待实施；
- 本组沙箱外 `make test` 结果：待实施；
- 关闭依据：LOOP-BIND03～LOOP-BIND05、LOOP-BIND11 和对应 Codegen 结构用例全部通过，合法捕获不
  再触发 `CE0102`，且未捕获路径满足 LOOP-BIND12 后方可关闭。

#### ISSUE-G07-002：`for/in` 循环绑定缺失 `let` / `var` 稳定性元数据

##### 归属

- 发现组：G07；
- 关联组：G03；
- 发现用例：LOOP-BIND12、LOOP-BIND13 的实施前静态检查。

##### 现象与结论

- 现象：普通局部绑定在 Codegen 注册后会记录源级 `let` / `var`，而数组与迭代器协议 `for/in` 的
  专用 lowering 只注册名称、C 存储和类型，没有登记循环绑定的可变性；
- 实际结果：Semantic 仍能正确拒绝对 `let` 赋值并允许 `var` 赋值，因此基础语言结果未改变；但
  Codegen 读取循环绑定时把稳定性视为未知，无法把 `let` 作为不可重绑 owner 参与已有的 assignment
  owner guard 消除，可能产生本可证明不需要的 retain/release；
- 规范依据与预期结果：绑定规范明确区分 `let` 与 `var`，流程控制规范要求未捕获循环不得仅为绑定
  模型增加额外 ARC；G03 已建立基于静态稳定性证明消除 owner guard 的通用机制；
- 根因：两条 `for/in` 专用 lowering 绕过普通绑定尾部的 mutability metadata 登记；
- 当前结论：性能风险兼实现缺口。应恢复通用编译期事实，不得通过运行时判断弥补。

##### 决策与实施

- 状态：待修复；
- 分类：性能风险；
- 建议方案：由 `ISSUE-G07-001` 的统一循环绑定 lowering 在所有普通局部与 capture-cell 分支登记实际
  `let` / `var`；Codegen 继续复用既有稳定性传播与 assignment owner guard 机制；
- 是否涉及既有测试修改：否；新增最小 Codegen 结构回归；
- 是否涉及运行时性能：修复只增加编译期元数据，不增加运行时操作，并应减少可证明稳定的 `let`
  链路上的冗余 ARC；`var` 继续保留必要保护；
- 是否涉及 runtime 私有 ABI、公开 ABI 或 `.ft` 格式：否；
- 人工决策与批准范围：已批准在 G07 修复该编译期事实缺口并以性能不退化为验收条件；不授权针对
  单一类型、表达式形状或测试文本增加特判；
- 实际变更：待实施。

##### 验收与关闭

- 本组专项复验结果：待实施；
- 本组沙箱外 `make test` 结果：待实施；
- 关闭依据：LOOP-BIND12、LOOP-BIND13 证明未捕获路径没有捕获开销、`let` 恢复稳定性优化且
  `var` 保留必要 owner guard，并完成 G07 独立全量回归后方可关闭。

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
