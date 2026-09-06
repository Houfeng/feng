# Feng 语言正确性用例补齐实施文档

> 状态：G01～G20 已交付；G21～G25 待 Review
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
- [函数规范](../specifications/feng-function.md)；
- [函数变长参数规范](../specifications/feng-function-variadic.md)；
- [`spec` 规范](../specifications/feng-spec.md)；
- [类型规范](../specifications/feng-type.md)；
- [流程控制规范](../specifications/feng-flow.md)；
- [异常模型规范](../specifications/feng-exception.md)；
- [enum 规范](../specifications/feng-enum.md)；
- [tuple 规范](../specifications/feng-tuple.md)；
- [字符串转义规范](../specifications/feng-string-escape.md)；
- [反引号字符串规范](../specifications/feng-string-raw.md)；
- [AE 语义错误码规范](../specifications/feng-error-codes-ae.md)。

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
`i32` 值是否为 `0`、正数或负数。

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

验证具名 tuple 表达式作为解构右侧时整体只求值一次；验证直接字面量解构只求值非空位置，而字面量
贴合具名 tuple 目标时求值全部元素；同时验证空位、零 tuple 和可变绑定不会引入重复求值或附加
运行时结构。

### 14.2 用例 TODO

- [x] TUP01：无空位解构具名 tuple 工厂结果时，工厂只调用一次，所有分量来自同一个返回值；
- [x] TUP02：含一个空位解构具名 tuple 工厂结果时，工厂仍只调用一次，非空分量值正确；
- [x] TUP03：含多个空位解构具名 tuple 工厂结果时，工厂仍只调用一次，非空分量值正确；
- [x] TUP04：具名 tuple 解构模式全部为空位时，工厂仍完整执行且只执行一次，不产生局部绑定；
- [x] TUP05：零元素具名 tuple 工厂通过 `let () = factory()` 解构时仍只调用一次；
- [x] TUP06：分别从具名 tuple 表达式和直接字面量进行 `var` 解构；各绑定可独立修改，不影响其他
  绑定或原具名 tuple 值，也不重新求值右侧；
- [x] TUP07：无空位直接解构 tuple 字面量时，各元素表达式从左到右各求值一次；
- [x] TUP08：含一个空位直接解构 tuple 字面量时，只按从左到右顺序求值非空位置，空位表达式不
  求值；
- [x] TUP09：含多个空位及全部为空位的直接字面量解构只求值非空位置；全部为空位时不求值任何元素
  表达式；
- [x] TUP10：字面量先贴合具名 tuple 目标时，全部元素表达式从左到右各求值一次；随后以空位解构
  该值不重新求值，也不改变先前已经完成的元素求值；
- [x] TUP11：Codegen 结构证明具名 tuple 工厂只出现一次，直接字面量解构不物化完整 tuple、不生成
  空位绑定或空位表达式调用，且不增加运行时判断、分支或分配。

### 14.3 独立验收与交付 TODO

- [x] 核对 tuple 和表达式规范，确认具名 tuple 表达式、直接字面量、空位以及 `let` / `var` 的
  求值边界已由主规范唯一确定；
- [x] 按人工决策完成 `ISSUE-G10-001` 的规范收敛，并由 TUP08～TUP11 直接验证当前实现；
- [x] 独立运行 TUP01～TUP10，核对事件顺序、调用次数、分量值、空位和绑定独立性；
- [x] 运行 TUP11 Codegen 专项，核对调用次数和直接字面量解构的零附加运行时结构；
- [x] 在 Codex 沙箱外为 G10 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G10 问题；
- [x] 填写本组实际文件、专项结果、全量结果和交付结论。

### 14.4 独立交付记录

- 状态：已交付
- 实际文件与用例：
  - `fcts/fcts_bin/src/test_tuple_destructuring_evaluation.ff`：新增独立 G10 行为测试入口并实现
    TUP01～TUP10；
  - `fcts/fcts_bin/src/main.ff`：登记 G10 FCTS 入口；
  - `test/codegen/test_codegen.c`：新增 TUP11 Codegen 结构测试并登记执行；
  - `docs/engineering/feng-language-conformance-coverage-hardening-issues/g10.md`：记录并关闭
    `ISSUE-G10-001`～`ISSUE-G10-002`。
- 本组专项结果：`make fcts-tests` 通过，完整 FCTS 1072/1072，TUP01～TUP10 均实际执行；
  `make build/bin/test_codegen && build/bin/test_codegen` 通过，TUP11 证明具名 tuple 工厂只调用一次，
  直接字面量解构不生成空位调用、空位绑定、完整 tuple 物化、运行时判断、分支或分配；
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两个干净阶段均完成，FCTS 均为 1072/1072、
  smoke 均为 90/90；std、CLI、性能约束、增量构建、发布、安装、bundled package 和预构建工具链
  检查全部通过；
- 问题：`ISSUE-G10-001`～`ISSUE-G10-002` 均已关闭；
- 交付结论：具名 tuple 表达式整体只求值一次；直接字面量只从左到右求值非空位置；字面量贴合具名
  tuple 目标时全部元素均求值；空位、零 tuple 和 `var` 独立绑定行为均符合主规范。本组未修改产品
  实现或既有测试用例，不增加运行时开销，也未变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- 建议 commit message：`test: cover tuple destructuring evaluation`

## 15 G11：Lexer 稳定诊断

### 15.1 测试重点

验证用户输入可达的稳定 Lexer 诊断及其精确位置，不混入 Parser 或 Semantic 错误。

### 15.2 用例 TODO

- [x] LEX01：`\\u`、`\\U`、`\\a`、`\\b`、`\\f`、`\\v` 及其他不受支持的普通转义在 Lexer
  遇到首个非法转义字符时产生 `LE0004`；
- [x] LEX02：`\\x` 后零位、一位以及第一位或第二位非十六进制字符均产生 `LE0004`；
- [x] LEX03：双引号字符串在 EOF、LF、CRLF 或尾随反斜线处未闭合时产生 `LE0003`；
- [x] LEX04：单行、多行以及以双反引号结尾但没有终止符的反引号字符串产生 `LE0003`；
- [x] LEX05：普通块注释、文档块注释和多行块注释在 EOF 前未闭合时产生 `LE0006`；
- [x] LEX06：为上述每类非法输入补齐最小合法邻界 token 序列，包括全部合法普通转义、完整
  `\\xNN`、闭合双引号字符串、闭合反引号字符串以及闭合块注释后的下一 token；
- [x] LEX07：反向映射 `LE0001`～`LE0007`；已有测试只在诊断码、错误 token、源码片段、行列、
  数量或 Lexer 阶段归属上缺少直接证据时，新增最小用例，不为重复数量修改既有用例；
- [x] LEX08：超过 `u64` 的整数源码字面量产生数字字面量错误 `LE0002`，最大 `u64` 字面量仍是
  合法整数 token，回归 [ISSUE-G11-001](./feng-language-conformance-coverage-hardening-issues/g11.md)。

### 15.3 独立验收与交付 TODO

- [x] 建立稳定 `LE` 码到现有 Lexer test 的映射，确认真实缺口；
- [x] 独立运行 G11，核对诊断码、token、行列、数量和 Lexer 阶段归属；
- [x] 在 Codex 沙箱外为 G11 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G11 问题；
- [x] 填写本组映射、实际新增用例、专项结果和全量结果。

### 15.4 独立交付记录

- 状态：已交付
- 稳定码映射与新增用例：
  - 复核既有 `test_reserved_words_rejected`、`test_error_tokens`、`test_hex_escape_valid`、
    `test_hex_escape_invalid`、`test_raw_string_literals` 和 `test_raw_string_unterminated`，确认其可作为
    基础行为证据，但尚未完整断言稳定诊断码、精确错误 token、行列、诊断数量和 Lexer 阶段归属；
  - 新增 `assert_g11_lexer_error`，统一断言错误码、文案、lexeme、offset、行列、Lexer 状态以及错误后
    紧邻 EOF，从直接 Lexer 调用证明阶段归属，并证明每个最小非法输入只产生一个诊断；
  - 新增 `test_g11_invalid_escape_diagnostics`、`test_g11_unterminated_string_diagnostics`、
    `test_g11_unterminated_raw_string_diagnostics`、`test_g11_unterminated_block_comment_diagnostics`、
    `test_g11_remaining_error_code_mapping` 和 `test_g11_legal_lexer_boundaries`，完成 `LE0001`～`LE0007`
    反向映射、所有计划非法分支及其合法邻界覆盖；未修改既有测试用例；
  - 将超过 `u64` 的整数源码字面量从误用的 `LE0003` 更正为 `LE0002`，并覆盖最大 `u64` 与最大值
    加一两个邻界。
- 本组专项结果：`make build/bin/test_lexer && build/bin/test_lexer` 通过，输出
  `lexer tests passed`；
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两个干净阶段均完成，FCTS 均为 1072/1072、
  smoke 均为 90/90；std、CLI、性能约束、增量构建、发布、安装、bundled package 和预构建工具链
  检查全部通过；首次 normal 回归中的外部瞬时 `SIGKILL` 已记录，独立复验失败目标和完整复跑均
  通过；
- 问题：`ISSUE-G11-001`～`ISSUE-G11-002` 均已关闭；
- 交付结论：G11 已完整覆盖用户输入可达的 `LE0001`～`LE0007` 稳定诊断、精确位置、单诊断数量、
  Lexer 阶段归属及合法邻界。本组仅更正编译器错误路径的静态错误码，不改变合法程序 Lexer 路径，
  不增加生成程序运行时开销，也未变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- 建议 commit message：`test: close lexer diagnostic coverage gaps`

## 16 G12：Parser 稳定诊断

### 16.1 测试重点

以语法结构覆盖完整性为目标，验证 Parser 对合法源码的 AST、对非法源码的稳定拒绝以及易混淆结构的
消歧结果。正向和反向用例必须覆盖当前用户输入可达的主要语法分支；诊断码只是反向用例的稳定属性
之一，不以机械命中全部 `SE` 码为目标，也不为不可达内部防御制造用例。输入必须通过 Lexer，且
Parser 失败时不得混入 Semantic 错误。

### 16.2 用例 TODO

- [x] PARSE01：模块声明与 import 的正向和反向结构；覆盖默认/显式可见性、单段/多段路径、无别名/
  有别名导入，以及缺少 `module`、路径、路径段、别名或分号；
- [x] PARSE02：顶层声明分派与修饰结构；覆盖合法注解、可见性、`extern func`、函数和绑定声明，以及
  孤立注解、非法 `extern` 目标、缺少 `func`、缺少 `let` / `var` 和未知顶层声明；
- [x] PARSE03：普通绑定与 tuple 解构绑定；覆盖 `let` / `var`、显式类型/推导初始化、空槽、零位置和
  2～8 位置，以及缺少名称、类型或初始化器、非法嵌套、非法位置、非法数量、单一类型注解和缺少
  `=`；
- [x] PARSE04：对象形式 type 与 tuple type；覆盖字段、方法、构造、finalizer、静态成员、泛型参数、
  父 spec 和成员展开，以及缺少名称、主体、字段分号、闭合字符、非法成员形式和 tuple 元素数量边界；
- [x] PARSE05：enum、三种 spec form 与 fit；覆盖各合法最小/完整形式，以及空 enum、非法 enum 成员/
  初始化器、spec 字段/方法/callable 约束、union/intersection 终止符、fit 空声明、非法成员和未闭合
  主体；
- [x] PARSE06：函数、参数、泛型参数、变长参数与 lambda；覆盖零/多参数、返回类型、普通/extern
  函数、表达式/block lambda，以及缺少名称、括号、冒号、函数体、非法变长参数位置和 arrow-block
  混用；
- [x] PARSE07：数组、调用、成员、泛型 target 与对象字面量等 postfix 结构；覆盖数组字面量、索引、
  array-new、普通/泛型调用、成员链和有参/无参对象字面量，以及缺少闭合字符、成员名、字段名、冒号、
  字段值和非法 array-new target；
- [x] PARSE08：基础表达式、分组、cast、tuple、unary/binary 和赋值结构；覆盖优先级与结合方向 AST，
  以及缺少操作数、分组/实参/tuple 闭合字符、tuple 尾随逗号元素和非法表达式项；
- [x] PARSE09：`if` / `match` 的 statement 与 expression form；覆盖 `else if`、多标签、range、type/
  chain/binding 标签和结果分支，以及缺少块、闭合字符或 `else`、重复 `else` 和非法标签；
- [x] PARSE10：`while`、三段式 `for`、`for/in`、tuple 解构迭代和 `defer`；覆盖空/非空块、可省略的
  三段式子句，以及缺少两个分号、循环体或 defer block；
- [x] PARSE11：`return`、`throw`、`break`、`continue` 与普通表达式语句终止边界；覆盖要求分号、
  表达式分支末尾允许省略的 `throw`、块末尾 yield，以及非法尾随或缺失分号；
- [x] PARSE12：`try/catch` 的 statement 与 expression form；覆盖匿名/具名 typed catch、多 catch 和
  结果分支，以及缺少 catch、名称、类型、冒号或主体；
- [x] PARSE13：集中覆盖易混淆合法邻界 AST；包括 block/对象字面量、group/tuple/lambda/cast、泛型
  target/关系表达式、index/array-new、`match`/bit-or、三段式 `for`/`for-in`；
- [x] PARSE14：对所有新增反向用例统一核对失败阶段、错误码、文案、错误 token、lexeme、offset、
  行列和空 AST；同时覆盖普通 token 与 EOF 两类错误位置，不修改既有测试用例；
- [x] PARSE15：审计用户不可达的内部防御与失效映射，只记录调用链依据和本组处理结论，不把它们
  计入正反向覆盖率，也不在 G12 重构为 `IE` 或改变 Parser 分派。
- [x] PARSE16：为本组新增的合法 Parser 边界建立 FCTS 行为映射；已有直接行为证据时引用既有 FCTS，
  不重复新增同义用例；本组发现并修复的合法语法缺口必须新增独立 FCTS，不能只以 AST 断言验收。
- [x] PARSE17：直接书写的 `if` 表达式 `else if` 链必须在 FCTS 中覆盖三个结果块分别被选中，并按
  流程控制主规范 §4.1 覆盖完整上下文目标对各块的独立贴合、无目标时有确定类型的非字面量分支
  分别位于 `then` / `else if` / `else` 的推导、全数值字面量的默认推导，以及 `throw` 结果块不参与
  目标选择和贴合。语句形式还必须覆盖简单标识符条件紧邻空 block 的真实执行行为。

### 16.3 独立验收与交付 TODO

- [x] 建立主要语法结构到现有 Parser test 的正向/反向映射，按用户可达分支确认真实缺口；
- [x] 独立运行 G12，核对合法 AST，以及反向用例的诊断码、token、行列、阶段和空 AST；
- [x] 在 Codex 沙箱外为 G12 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G12 问题；
- [x] 填写本组映射、实际新增用例、专项结果和全量结果。
- [x] 独立运行 G12 FCTS 追加用例，核对 `else if` 语句空 block、表达式分支选择及全部 §4.1 贴合/
  推导规则；
- [x] 在 Codex 沙箱外为 G12 追加交付重新执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G12 追加问题并更新独立交付记录。

### 16.4 独立交付记录

- 状态：已交付（含 PARSE16～PARSE17 追加交付）
- 语法结构映射与新增用例：
  - 复核既有 module/top-level、type/static/spec、enum、generic/variadic、tuple/destructure、array/postfix、
    `if` / `match`、loop、try/defer 和 mixin Parser 测试，保留其已有专项 AST 与合法邻界证据；其中
    range/type/chain/binding match 形态继续由既有 `test_match_*` 系列直接覆盖；
  - 在 `test/parser/test_parser.c` 新增统一的 `G12ParserFailureCase` 与断言辅助代码；每个反向源码均
    直接调用 `feng_parse_source`，精确核对 `SE` 阶段、错误码、完整文案、token kind、lexeme、offset、
    行列和空 AST，并同时覆盖普通 token 与 EOF 定位；
  - 新增 `test_g12_module_import_and_top_level_syntax`、
    `test_g12_binding_type_and_callable_syntax`、`test_g12_enum_spec_and_fit_syntax`、
    `test_g12_expression_and_postfix_syntax`、`test_g12_control_flow_and_try_syntax` 和
    `test_g12_ambiguous_legal_ast_boundaries`；合计执行 8 份正向源码和 111 份独立反向源码，覆盖
    PARSE01～PARSE14，未修改任何既有测试用例；
  - PARSE15 审计确认 15 个发码点属于用户输入不可达路径，另有 3 个规范条目当前无实现发码点；按
    `ISSUE-G12-001` 只记录边界，不制造伪用例，也不进行 `SE` / `IE` 或 Parser 分派重构；
  - 新用例发现并修复 `else if` 简单标识符条件误吞空 block，以及直接 `if` 表达式不接受
    `else if` 链两个 Parser Bug；前者统一复用块前表达式解析入口，后者归一化为既有嵌套
    `FENG_EXPR_IF` AST。主规范无需修改。

#### 16.4.1 正向 FCTS 行为映射

Parser 正向源码用于隔离语法结构，允许使用尚未定义的占位类型或函数；FCTS 使用语义完整且可执行的
程序映射同一合法结构，不机械复制 Parser 夹具：

- PARSE01～PARSE03 的 module/import、顶层声明、普通绑定与 tuple 解构，由 `test_module`、
  `test_module_import_semantics`、`test_module_binding_semantics`、`test_binding`、`test_tuple`、
  `test_tuple_destructuring_evaluation`、`test_function` 和 `test_extern` 提供直接行为证据；
- PARSE04～PARSE06 的对象/tuple type、构造与成员、enum、三种 spec form、fit、泛型、变长参数和
  两种 lambda，由 `test_type`、`test_object_construction_order`、`test_enum`、`test_spec`、
  `test_union`、`test_intersection`、`test_fit`、`test_generic`、`test_variadic` 和 `test_lambda` 提供直接
  行为证据；
- PARSE07、PARSE08 和 PARSE13 的 array/postfix、对象字面量、调用与成员链、group/tuple/cast、
  unary/binary/assignment、泛型 target、index/array-new、infix match/bit-or 及两类 `for` 消歧，由
  `test_array`、`test_callable_result_immediate_invocation`、`test_expression`、`test_numeric_literal`、
  `test_assignment_evaluation_semantics`、`test_evaluation_order`、`test_tuple`、`test_flow` 和
  `test_loop_binding` 提供直接行为证据；
- PARSE09～PARSE12 的 `if` / `match`、循环与控制转移、`defer`、`try/catch`、`return` 和 `throw`，由
  `test_flow`、`test_branch_result_fitting`、`test_loop_binding`、`test_loop_control`、`test_defer`、
  `test_exception` 和 `test_try_branch_exception_cleanup` 提供直接行为证据；
- PARSE09 / PARSE13 本次修复的两个缺口由新增 `test_g12_parser_positive_semantics` 独立补齐，不以
  上述宽泛映射替代直接证据。

#### 16.4.2 追加用例与验收

- `fcts/fcts_bin/src/test_g12_parser_positive_semantics.ff` 新增 5 个 FCTS：语句形式以简单标识符条件紧邻
  空 block；直接表达式链分别选择 `then` / `else if` / `else`；完整 union-form 绑定目标、函数参数、
  函数返回、局部赋值和成员赋值目标分别传播到每个结果块；无目标时 `f32` 非字面量锚点分别位于
  三个结果块；全整数与全浮点字面量分别默认推导为 `int` 与 `double`；三个位置的 `throw` 块均不
  参与目标选择和贴合；
- `fcts/fcts_bin/src/main.ff` 只登记上述独立 FCTS 入口，未改写既有测试用例；
- 专项 `make fcts-tests` 通过，FCTS 1077/1077；Parser 专项继续通过并输出 `parser tests passed`；
- 首次追加全量回归的 UBSan 阶段完整通过，normal 阶段一次在 bundled init 子进程处收到系统
  `SIGKILL(9)`；独立 `make init-bundled-packages-test` 随即通过，第二次完整 `make test` 通过，确认
  该失败不可复现，未据此修改产品或测试脚本。

- 本组专项结果：`make build/bin/test_parser && build/bin/test_parser` 通过，输出
  `parser tests passed`；新增 111 份反向源码、全部正向 AST 断言和 5 个追加 FCTS 均实际执行；
- 本组沙箱外 `make test`：通过，退出码 0；UBSan 与 normal 两个干净阶段均完成，FCTS 均为
  1077/1077、smoke 均为 90/90、std 均为 601/601；CLI、性能约束、增量构建、发布、安装、bundled
  package 和预构建工具链检查全部通过；
- 问题：`ISSUE-G12-001`～`ISSUE-G12-006` 均已关闭；
- 交付结论：G12 已覆盖用户输入可达的主要 Parser 正向/反向语法结构、精确诊断属性和易混淆 AST
  边界；所有合法 Parser 结构均已映射到 FCTS 行为证据，新增 `else if` 语法缺口另有直接 FCTS 验收。
  产品实现只修复编译期 Parser，不改变生成程序执行路径，不增加运行时开销，也未变更 runtime 私有
  ABI、公开 ABI、`.ft` 格式或错误码主规范；
- 建议 commit message：`fix(parser): complete else-if parsing and conformance coverage`

## 17 G13：名称绑定诊断

### 17.1 测试重点

验证值名称的声明、查找、同级唯一性、子块屏蔽、词法生命周期和可变性规则。非法程序在 Semantic
test 中核对稳定诊断码、位置、数量和阶段; 所有能够合法编译并形成可观察行为的边界均在 FCTS 中
直接运行,Semantic 正向用例不得替代可进入 FCTS 的行为证据。

同一词法值作用域中的名称不得重复声明; 子级花括号块可以屏蔽父作用域同名绑定。函数、方法和
Lambda 的参数与最外层函数体处于同一词法值作用域。`for` 头、`match` 分支头、传播到 `if` /
`while` 体的 infix `match` 绑定以及 `catch` 子句头位于各自的头部作用域,其花括号 body 是子级块;
body 可屏蔽头部绑定,不同分支或子句可独立复用名称。

本组不重复覆盖 import / alias / 跨模块二义性（归 G22）、函数重载合法性（归 G15）或泛型名称与
arity 身份（归 G25）。模块内同名顶层声明和跨声明类别冲突只建立到现有直接证据的映射,缺口仍由
G13 补齐。

### 17.2 用例 TODO

- [x] BIND01：分别在普通读取、赋值目标和调用目标位置引用未声明的值名称; Semantic 必须在该
  使用位置报告 `AE0001`,不得延迟到 Codegen,也不得由后续可写性或可调用性诊断替代首个错误；
- [x] BIND02：同一普通花括号块中重复声明局部绑定; 覆盖 `let` / `let`、`let` / `var`、`var` /
  `let` 与 `var` / `var`,均在第二个声明名称处产生一个重复绑定诊断,不得把后声明解释为屏蔽；`_`
  是普通标识符而非丢弃符,同一作用域重复声明 `_` 适用相同规则；
- [x] BIND03：同一函数、方法和 Lambda 的参数名称重复,以及参数与最外层函数体局部绑定同名时
  均报错; 在函数体更深一层子块中声明同名绑定则合法并进入 FCTS；
- [x] BIND04：同一普通 tuple 解构模式及同一 `for/in` tuple 解构模式中的非空位置名称重复时
  报错; 解构空位不产生名称,不得与其他位置形成重复；
- [x] BIND05：同一块中先声明普通局部、后由单变量或 tuple 解构再次声明同名名称时在后一个声明
  处报错; 解构一次引入的所有非空名称都必须参与同级唯一性检查；
- [x] BIND06：同一 `if` / `while` 条件经 `&&` 同时传播的多个 infix `match` 绑定名称重复时在
  后一个绑定处报错; `||`、`!` 或语句结束后不传播的名称继续按既有可见性规则处理；
- [x] BIND07：不可变普通局部、默认或显式 `let` 参数、`for/in let`、`match` 默认或显式 `let`
  绑定以及不可变 `catch` 绑定被重新赋值时,Semantic 在赋值目标处报告不可变绑定诊断; `var`
  对应项的合法重新赋值全部进入 FCTS；
- [x] BIND08：名称离开普通子块、三段式 `for`、`for/in`、`match` 分支、`catch` 子句或 infix
  `match` 的可传播范围后再被引用时报告 `AE0001`; 不同 `match` 分支、不同 `catch` 子句以及
  `else` 均不得读取其他分支或条件专属绑定；
- [x] BIND09：在普通子块中覆盖父块绑定,逐项覆盖外层 / 内层 `let` 与 `var` 的四种组合;
  FCTS 必须证明块内解析到最近绑定、内层 `var` 修改不影响外层值,并在离开子块后恢复外层绑定；
- [x] BIND10：在 `for` 体、块形式 `match` 分支体、infix `match` 的 `if` / `while` 体以及
  `catch` body 中声明与头部绑定同名的局部名称; 全部作为合法子块屏蔽进入 FCTS,并证明头部绑定
  在进入屏蔽点前可见、内层绑定在声明后优先；
- [x] BIND11：不同 `match` 分支、不同 `catch` 子句及循环的不同嵌套 body 独立复用同一名称;
  全部进入 FCTS,证明同名绑定不跨兄弟作用域共享值或可变性；
- [x] BIND12：模块级普通绑定重复声明以及模块级不同声明类别同名冲突,映射或补齐现有 Semantic
  直接证据; import / alias 情形留给 G22,函数重载留给 G15,泛型 arity 留给 G25。

### 17.3 独立验收与交付 TODO

- [x] 先按已确认的同级唯一、子块屏蔽和头部 / body 父子作用域更新变量绑定、流程控制与异常主规范,
  并同步中英文用户手册；
- [x] 建立本领域稳定诊断码到现有 Semantic test 的映射,只为没有直接证据的非法边界新增测试；
- [x] 为 BIND03、BIND07、BIND09～BIND11 的全部合法边界建立 FCTS 直接运行证据; 可进入 FCTS
  的正向程序不得只停留在 Semantic test；
- [x] 独立运行 G13 Semantic tests 与 FCTS，核对诊断码、位置、数量、阶段、最近绑定、屏蔽恢复和
  兄弟作用域隔离；
- [x] 在 Codex 沙箱外为 G13 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G13 问题；
- [x] 填写本组映射、实际新增用例、专项结果和全量结果。

### 17.4 独立交付记录

- 状态：已交付
- 稳定码映射与新增用例：
  - BIND01、BIND08 以 `AE0001` 覆盖未声明名称及离开作用域后的读取；赋值目标不再追加派生
    `AE0104`；
  - BIND02～BIND06 新增 `AE0105`，覆盖普通局部、参数、tuple 解构、`for/in` 解构及同一 `&&`
    条件传播的 infix `match` 绑定，并精确断言后声明 token；
  - BIND07 以 `AE0104` 覆盖所有不可变局部绑定形态，FCTS 覆盖对应 `var` 正向行为；
  - BIND09～BIND11 在 FCTS 覆盖四种 `let` / `var` 子块屏蔽组合、所有控制头 / body 父子作用域及
    兄弟作用域隔离；`_` 作为普通可读取标识符同时具有正向屏蔽与反向重复声明证据；
  - BIND12 复用并补齐 `AE0215`、`AE0216`、`AE0217` 的同文件模块级直接证据；模块级绑定实现未
    改动。
- 实际文件：
  - `docs/specifications/feng-binding.md`、`feng-flow.md`、`feng-exception.md`、
    `feng-error-codes-ae.md` 与中英文用户手册：收敛同级唯一、子块屏蔽、参数 / body、控制头 /
    body、`_` 普通标识符及 `AE0105`；
  - `src/parser/parser.h`、`src/parser/parser.c` 与 `src/codegen/codegen.c`：为 tuple、`match`、
    infix `match` 和 `catch` 绑定保留精确声明 token，并在 `for/in` tuple 分量 lowering 中传播；
  - `src/semantic/analyzer.c`：统一源码局部声明检查，统一块形式 union `match` 的父子作用域，并消除
    未声明赋值目标的派生可写性诊断；
  - `test/semantic/test_semantic.c`：新增 BIND01～BIND08、BIND12 的精确反向矩阵；
  - `fcts/fcts_bin/src/test_name_binding_scope.ff`：新增 BIND02～BIND04、BIND07、BIND09～BIND11 的
    正向运行用例；
  - `std/std/src/compiler/parser/FengParser.ff`、`std/std_test/src/test_lexer.ff`：按人工确认移除仅用于
    忽略返回值的重复 `let _`，改用表达式语句。
- 本组专项结果：Parser、Semantic、Codegen tests 均通过；std 601/601；FCTS 1094/1094；
  `git diff --check` 通过。
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 1094/1094、smoke 均为
  90/90、std 测试均为 601/601；性能约束、增量构建、发布、安装及其余单元与 CLI 测试全部通过。
- 性能与兼容性：变更只增加编译期名称检查和 AST 声明位置信息；未增加运行时分支、分配或 ARC，
  未修改 runtime 私有 ABI、公开 ABI 或 `.ft` 格式。
- 问题：[G13 问题记录](./feng-language-conformance-coverage-hardening-issues/g13.md) 中
  `ISSUE-G13-001`～`ISSUE-G13-005` 均已关闭。
- 建议 commit message：`fix(semantic): enforce lexical binding uniqueness and complete G13 coverage`

## 18 G14：表达式诊断

### 18.1 测试重点

验证一元与二元运算、成员访问、索引、调用和赋值结构产生的稳定 Semantic 诊断，并为各非法边界建立
最小合法邻界运行证据。

本组只覆盖表达式结构自身的通用边界：调用参数数量与类型归 G15；普通赋值右值不兼容、显式转换和
static / 实例访问方式归 G18；数组元素、维度和多维关系归 G21；模块、spec、复合类型与泛型的专属
诊断分别归 G22～G25。G04 已覆盖的整数溢出、除模零值、移位范围及明确 UB 不在本组重复。ABI 一元
`&` 及借用边界也不属于 G14。

### 18.2 用例 TODO

- [x] EXPR01：一元 `-`、`!`、`~` 分别拒绝非数值、非 `bool`、非整数操作数；二元 `+ - * / %`、
  `< <= > >=`、`== !=`、`&& ||`、`& | ^ << >>` 的每个运算符拼写均以所属类型族的最小非法
  操作数触发一个稳定 Semantic 诊断，不得延迟到 Codegen；
- [x] EXPR02：字面量、二元表达式和调用结果作为普通或复合赋值左侧时，均在左侧起始 token 报告
  `AE0104`；不可变绑定和只读数组层分别映射 G13、既有数组测试，不在本组重复排列；
- [x] EXPR03：普通对象值访问不存在的实例成员、普通类型访问不存在的 static 成员，分别在成员访问
  位置产生稳定的成员不存在诊断；static / 实例访问方式和可见性专属矩阵留给 G18、G22；
- [x] EXPR04：读取与写入上下文中的非数组索引目标均被拒绝；数组索引分别拒绝 `bool` 与浮点操作数，
  接受不同固定宽度的有符号和无符号整数索引；
- [x] EXPR05：普通非 callable 局部值与返回非 callable 值的计算表达式被立即调用时，均在调用目标处
  报告 `AE0507`；callable 的参数数量、参数类型和变参规则留给 G15；
- [x] EXPR06：数值复合赋值拒绝非数值操作数和不同数值类型，位复合赋值拒绝非整数操作数和不同整数
  类型；对应合法目标贴合覆盖全部 `+= -= *= /= %= &= |= ^= <<= >>=`，左侧只求值一次的行为复用
  已交付赋值求值顺序专项；
- [x] EXPR07：在 FCTS 中直接运行一元、二元、成员、索引、调用、普通赋值和复合赋值的最小合法邻界；
  使用固定宽度数值类型避免 `int` 平台位宽影响，已有直接运行证据只建立映射、不重复新增同构程序。

### 18.3 独立验收与交付 TODO

- [x] 先对齐表达式主规范中的 `int` 平台位宽说明，并把一元、二元、复合赋值与索引旧码收敛到 AE
  表达式段；不得借 G14 开展无关错误码重构；
- [x] 建立本领域稳定诊断码到现有 Semantic test 的映射，只为缺少精确码、token、行列、数量和阶段
  证据的非法边界新增测试；
- [x] 为 EXPR04 与 EXPR07 的合法边界建立或映射 FCTS 直接运行证据；Semantic 接受不能替代运行断言；
- [x] 独立运行 G14，核对诊断码、位置、数量、阶段和表达式上下文；
- [x] 在 Codex 沙箱外为 G14 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G14 问题；
- [x] 填写本组映射、实际新增用例、专项结果和全量结果。

### 18.4 独立交付记录

- 状态：已交付
- 稳定码映射与新增用例：
  - EXPR01：新增 `AE1018` 覆盖 3 种一元运算符，新增 `AE1019` 覆盖 18 种二元运算符拼写；
  - EXPR02：以 `AE0104` 覆盖字面量、二元表达式和调用结果的普通与复合赋值，共 6 个反向程序；
  - EXPR03：分别以 `AE0306`、`AE0309` 覆盖不存在的实例与 static 成员；
  - EXPR04：新增 `AE1021` 覆盖读写位置的非数组目标，新增 `AE1022` 覆盖 `bool` 与浮点索引；FCTS
    直接运行 `i16`、`u8` 索引；
  - EXPR05：以 `AE0507` 覆盖直接非 callable 局部值与计算所得非 callable 值；
  - EXPR06：新增 `AE1020` 覆盖全部 10 种复合赋值拼写及数值、整数宽度不匹配，共 12 个反向程序；
  - EXPR07：新增 3 项 FCTS，直接运行全部合法运算符、成员、索引、直接及计算 callable、普通赋值和
    全部 10 种复合赋值；共新增 47 个最小反向程序和 3 项正向运行用例。
- 实际文件：
  - `docs/specifications/feng-expression.md`、`feng-error-codes-ae.md` 与 `feng-flow.md`：对齐 `int` 平台
    位宽说明，定义 `AE1018`～`AE1022` 并更新关联码引用；
  - `src/semantic/analyzer.c`：将五类既有表达式诊断产生点收敛到新稳定码，不改变接受、拒绝或 lowering；
  - `test/semantic/test_semantic.c`：新增 G14 精确反向矩阵，逐项断言唯一 Semantic 诊断的错误码、token、
    行列和消息片段；
  - `fcts/fcts_bin/src/test_expression_diagnostic_boundaries.ff` 与 `main.ff`：新增并登记 G14 合法邻界运行
    用例；
  - `src/parser/parser.c`、`test/parser/test_parser.c` 与 `docs/specifications/feng-flow.md`：同步已迁移错误码
    的注释和引用。
- 本组专项结果：Parser、Semantic、Codegen tests 均通过；std 601/601；FCTS 1097/1097；
  `git diff --check` 通过。
- 本组沙箱外 `make test`：通过；UBSan 与 normal 两阶段均完成，FCTS 均为 1097/1097、smoke 均为
  90/90、std 测试均为 601/601；性能约束、增量构建、发布、安装及其余单元与 CLI 测试全部通过。
- 性能与兼容性：只变更编译期诊断码、规范与测试；未改变表达式发码，未增加运行时分支、调用、分配
  或 ARC，未修改 runtime 私有 ABI、公开 ABI 或 `.ft` 格式。
- 问题：[G14 问题记录](./feng-language-conformance-coverage-hardening-issues/g14.md) 中
  `ISSUE-G14-001`、`ISSUE-G14-002` 均已关闭。
- 建议 commit message：`fix(semantic): stabilize expression diagnostics and complete G14 coverage`

## 19 G15：函数诊断

### 19.1 测试重点

验证普通函数系统中的声明签名、参数、重载、调用、callable / Lambda 目标类型和返回契约产生的稳定
Semantic 诊断，并为对应合法邻界建立可直接运行的 FCTS 证据。

本组覆盖顶层函数、`type` 中的实例方法与静态方法、`fit` 方法、object-form `spec` 方法签名、
callable-form `spec` 值和块 Lambda。相同规则存在多个声明面时，先映射现有直接证据，只为缺少精确
诊断码、token、行列、数量、阶段或合法运行证据的边界新增测试，不机械排列全部笛卡尔积。

参数缺少类型属于 Parser 语法诊断，参数重名、默认 `let` / 显式 `let` / `var` 可写性属于 G13；
构造函数与终结器的声明、可见性及特殊返回限制属于 G05 和类型规范；import / alias / 完整模块路径的
名称解析属于 G22；spec 满足与适配属于 G23；复合类型参数与返回值属于 G24；泛型 callable 的类型实参
数量、推导与约束属于 G25；`extern func`、C ABI、注解和 runtime 私有 ABI 不属于 G15。上述边界只建立
必要映射，不在本组重复新增同构用例。

### 19.2 用例 TODO

#### 19.2.1 声明签名与参数规则

- [x] FUNC-D01：重复签名、仅返回类型不同、变参与定参覆盖范围重叠以及当前可见契约关系导致的参数
  接受范围重叠，分别覆盖顶层函数、`type` 实例 / 静态方法、`fit` 方法和 object-form `spec` 方法签名；
  非法程序必须在形成冲突的后一个声明名称处产生一个稳定 Semantic 诊断。参数个数或参数类型不同且
  接受范围不重叠的合法重载、实例与 static 两个成员面中的同名方法，以及当前不存在共同满足类型的
  两个 object-form `spec` 参数重载必须继续合法；已有直接证据只建立映射；
- [x] FUNC-D02：函数与 Lambda 参数必须具有显式非 `void` 类型；参数省略 `let` / `var` 时按 `let`
  处理，显式 `let` 与 `var` 均可声明。缺失类型的 Parser 诊断只映射 G12，参数重名及参数赋值只映射
  G13；G15 仅补 `void` 参数位置及函数签名层仍缺少的稳定 Semantic 证据，默认 `let` 读取、显式
  `let` 读取和 `var` 修改的合法行为直接映射 G13 已有 FCTS，不重复新增同构用例；
- [x] FUNC-D02-A：编译目标为 `bin` 时，分别覆盖缺少入口、同包多个入口、`main` 参数数量 / 参数
  类型不等于唯一合法的 `main(args: string[])` 以及显式非 `void` 返回类型，精确映射 `AE0907`～
  `AE0910`；合法入口的可见性和所在 module 可见性不得影响入口资格。编译目标为 `lib` 时，同名
  `main` 按普通顶层函数规则处理，不得误触发 bin 入口诊断；已有入口专项足够时只建立映射；

#### 19.2.2 固定参数与重载调用

- [x] FUNC-D03：对顶层函数、实例方法、静态方法和 callable-form `spec` 值分别构造少于及多于声明
  数量的固定实参调用；普通函数 / 方法重载集合必须在被调用名称处报告“无匹配重载”的稳定诊断，
  callable 值必须在调用目标处报告“目标函数类型不接受该参数数量”的稳定诊断；每个最小程序只产生
  一个 Semantic 诊断，不得延迟到 Codegen；
- [x] FUNC-D03-A：使用三个固定参数分别覆盖第一个、中间和最后一个实参类型不匹配；顶层函数完成三
  个位置矩阵，实例方法、静态方法和 callable 值各至少覆盖一个独立不匹配位置。已具备静态类型的
  `i32` / `u32`、`bool` / `i32` 等不兼容值必须被拒绝；数值字面量及纯字面量常量表达式按目标固定
  宽度数值类型贴合、显式转换后的值以及全部参数精确匹配必须进入 FCTS 直接运行；
- [x] FUNC-D03-B：普通重载调用分别覆盖唯一精确匹配、无匹配和仍存在多个匹配三个结果；合法调用必须
  执行被选中的顶层、实例或 static 实现并断言返回值，非法调用必须区分“无匹配重载”和“多个匹配
  候选”稳定诊断，不得以声明顺序静默选择。若普通声明期重叠检查使某种二义调用在用户程序中不可达，
  必须记录并映射实际可达的 spec / 约束调用证据，不为凑数制造内部不可达输入；

#### 19.2.3 返回语句与路径完整性

- [x] FUNC-D04：已经人工确认，显式声明或经返回类型推导确定为非 `void`、且具有实现 body 的
  callable，所有能够正常到达 body 末尾的分支都必须在此前返回值；编译器必须在 Semantic 阶段
  检查并报错，不得把缺失返回保留到运行时 panic。逃逸当前 callable 的 `throw` 和可证明没有正常
  出口的循环终止相应路径，
  不要求实际返回值；本地捕获继续按 `catch` body 的结果判断，见
  [ISSUE-G15-001](./feng-language-conformance-coverage-hardening-issues/g15.md#issue-g15-001func-d04-要求返回值的路径可落到-callable-末尾)。
  最小反向示例为：

  ```feng
  func choose(flag: bool): i32 {
      if flag {
          return 1;
      }
      // flag == false 时没有执行 return，控制流会到达函数末尾。
  }
  ```

  当前实现会通过 Semantic，并在该路径运行到函数末尾时执行 Codegen 注入的
  `feng_panic("function reached end without return")`；实施后的预期是在命名 callable 的名称 token
  或块 Lambda 的起始 `(` token 产生一个 `AE0515` Semantic 诊断。该规则覆盖非 `void` 顶层函数、
  实例 / 静态 / `fit` 方法和非 `void` 块 Lambda；省略返回类型但存在有值 `return`、因而推导为非
  `void` 的 callable 也适用。反向用例至少覆盖直接落尾、无 `else` 的单分支 `if`、完整
  `if / else if / else` 中一个分支落尾、缺少 `else` 或一个分支落尾的块 `match`、主表达式或本地
  `catch` 正常完成后继续落尾的 `try/catch`，以及顶层函数、普通方法和块 Lambda 三种 callable
  body；正向用例覆盖所有分支返回、一个分支返回而另一分支以逃逸 `throw` 终止、本地捕获后每个可达
  `catch` 均返回或重新抛出、可能退出的循环后存在明确返回、恒真且没有可达 `break` 的循环使函数
  末尾不可达，以及 `void` body 自然结束。`for/in` 始终保留循环后的正常路径；恒真循环的判断严格
  使用函数主规范定义的布尔字面量 `true` 或三段式 `for` 空条件，不扩展为实现自选的常量折叠；
- [x] FUNC-D04-A：显式非 `void` callable 中的空 `return;` 使用 `AE0501`；省略返回类型的同一
  callable 中混用空 `return;` 与有值 `return`、或使用两个互不兼容的有值返回，均使用 `AE0504`；
  诊断位于触发冲突的 `return` token。`return` 表达式与显式声明返回类型不匹配继续使用表达式目标
  类型贴合对应的既有诊断，不得错误归入返回类型推导码。顶层函数、实例方法、静态方法、`fit` 方法
  与块 Lambda 的既有 FUNC01～FUNC08 证据只做反向映射；缺少的“空返回与有值返回混用”和显式返回
  类型不匹配才新增最小程序；
- [x] FUNC-D05：显式 `: void` 的顶层函数、实例 / 静态 / `fit` 方法和目标返回类型为 `void` 的块
  Lambda 均拒绝 `return expr;`，并在 `return` token 产生一个 `AE0501` Semantic 诊断；不得沿用
  普通表达式目标类型不匹配的 `AE1003`。无 `return` 自然结束和显式 `return;` 均合法。构造函数与
  终结器的 `return` 值形态只映射 G05 已有证据，不在 G15 重复；

#### 19.2.4 变长参数调用

- [x] FUNC-D06：映射变参专项已有的顶层函数、实例方法、静态方法、构造函数和 callable-form `spec`
  证据，并只补精确诊断缺口：缺少固定前缀实参、普通变参元素类型不匹配、把既有 `T[]` 当作一个
  普通变参元素、`...expr` 目标不是变参 callable、转发不从第一个变参位置开始、转发表达式不是匹配
  的只读 `T[]`。非法程序必须分别稳定落入参数匹配、`AE0505` 或 `AE0524` 所属根因；合法的零个、
  一个、多个变参元素及预打包数组直接转发必须映射或补齐 FCTS 运行证据；

#### 19.2.5 callable 值与 Lambda 目标类型

- [x] FUNC-D07：未绑定的顶层函数、实例方法、静态方法引用和 Lambda 在普通值位置缺少明确
  callable-form `spec` 目标时产生稳定诊断；同名重载在目标 callable 下仍有多个匹配候选、来源签名
  与目标的参数数量 / 参数类型 / 变参标记 / 返回类型任一不一致时，分别产生目标消歧或签名不匹配
  诊断。非法程序至少覆盖绑定、函数实参和函数返回三个目标位置，并精确断言 `AE0520`～`AE0523`
  中与实际根因对应的码；泛型来源显式闭合和 spec 方法值的专项规则只映射 G23 / G25；
- [x] FUNC-D08：callable-form `spec` 目标下的单表达式 Lambda 与块 Lambda 分别覆盖参数数量、参数
  类型和返回类型贴合；合法目标分别来自显式绑定类型、形参类型、显式函数返回类型和显式转换，均在
  FCTS 中调用并断言结果。块 Lambda 的 `return` 必须只使用自身 callable 上下文，不能继承外层函数
  的返回约束；FUNC06～FUNC08 已有直接证据只建立映射；

#### 19.2.6 合法邻界与稳定诊断收敛

- [x] FUNC-D09：在同一个最小 FCTS 文件中直接运行固定参数顶层函数、实例方法、静态方法、`fit`
  方法、callable 值、单表达式 Lambda、块 Lambda、合法重载选择、显式非 `void` 返回、推导返回、
  `void` 自然结束 / `return;` 和合法变参调用；已有直接运行证据足以唯一证明某项时只记录映射，不
  新增同构程序，Semantic 接受不得替代可进入 FCTS 的运行断言；
- [x] FUNC-D10：审计函数领域仍由实现产生的 `AE0057`、`AE0058`、`AE0218`～`AE0220`。重复顶层
  函数签名、仅返回类型不同和变参覆盖冲突分别迁移为 `AE0508`～`AE0510`；非 `void` callable 的
  空 `return;` 从 `AE0057` 迁移为 `AE0501`，冲突的推导返回类型从 `AE0058` 迁移为 `AE0504`，
  `void` callable 的 `return expr;` 从该产生点当前使用的 `AE1003` 迁移为 `AE0501`，FUNC-D04 的
  正常落尾使用 `AE0515`。同步实现和精确测试时，既有测试只允许修改问题记录明确列出的十条白名单，
  见
  [ISSUE-G15-002](./feng-language-conformance-coverage-hardening-issues/g15.md#issue-g15-002函数诊断仍产生未进入当前-ae-分段规范的旧错误码)。
  G15 可以新增用例；不得借本组修改白名单外的既有测试，不得重排 ABI、注解、构造、spec 满足、
  复合类型或泛型诊断。若白名单外既有用例需要修复，必须暂停该修改并再次取得人工决策。

### 19.3 独立验收与交付 TODO

- [x] 按已经确认的“非 `void` callable 正常落尾必须在编译期报错”结论及函数主规范 §4.1.1，使用
  通用控制流结果分析实现块、`if`、`match`、`throw`、本地 `try/catch`、`while`、三段式 `for`、
  `for/in`、`break` 和 `continue` 的末尾可达性；不得通过新增运行时检查实现，不得把恒真识别扩大
  为未定义的常量折叠；
- [x] 按 `ISSUE-G15-002` 对齐 AE 函数段稳定码；只迁移 G15 直接覆盖的旧码产生点，并严格按问题记录
  的十条白名单替换或新增列明的错误码断言。白名单外只新增用例，不修改其他既有用例，不开展无关
  错误码重构；
- [x] 建立 FUNC-D01～FUNC-D10 到现有 Semantic、Parser 与 FCTS 的逐项映射；每条已有证据必须能
  直接证明对应声明面、调用形态或返回边界，FUNC01～FUNC08、变参、method value 和 spec 专项只
  映射不重复；
- [x] 对映射后仍缺少的非法边界新增最小 Semantic 程序；每个程序至少断言恰好一个诊断、稳定错误码、
  触发 token、行列、来源文件和 Semantic 阶段，消息只锁定规范承诺稳定的必要片段；
- [x] 对所有可以合法编译并形成可观察结果的缺口新增或映射 FCTS；专项执行必须证明新增测试函数已由
  `fcts_bin` 主入口登记并真实运行，不能以 Semantic 正向分析或 Codegen 文本替代；
- [x] 独立运行 Parser、Semantic、Codegen、std 与 FCTS 专项，记录各套件准确通过数量；G15 的返回
  路径实现如影响发码，必须额外核对没有新增运行时分支、调用、分配或 ARC；
- [x] 在 Codex 沙箱外为 G15 独立执行完整 `make test`，记录 UBSan 与 normal 两阶段的 smoke、std、
  FCTS、性能约束及其余回归结果；其他组或此前的全量结果不能替代本组回归；
- [x] 执行 `git diff --check`，逐项关闭或取得人工决定保留 G15 问题，填写稳定码映射、实际新增用例、
  专项结果、全量结果、性能与兼容性结论及英文 commit message 后，才可标记 G15 已交付。

### 19.4 独立交付记录

- 状态：已交付；
- 稳定码映射：重复顶层函数签名由 `AE0218` 迁移为 `AE0508`，仅返回类型不同由 `AE0219` 迁移为
  `AE0509`，顶层变参覆盖冲突由 `AE0220` 迁移为 `AE0510`；非 `void` callable 的空 `return;` 由
  `AE0057` 迁移为 `AE0501`，推导返回冲突由 `AE0058` 迁移为 `AE0504`，`void` callable 的
  `return expr;` 由该产生点的 `AE1003` 迁移为 `AE0501`；新增非 `void` callable 正常落尾诊断
  `AE0515`。fit 变参方法冲突继续使用其既有专用码 `AE0803`；
- 实际新增入口：Semantic 新增 11 个 G15 测试函数并在同一测试二进制入口登记；FCTS 新增
  `test_function_return_paths.ff` 并由 `fcts_bin` 主入口登记；std 新增 `test_async.ff`、`test_parser.ff`
  并由 `std_test` 主入口登记；
- 本组专项结果：Parser `1/1` 测试程序通过，Semantic `1/1` 测试程序通过，Codegen `1/1` 测试程序
  通过，std `604/604` 通过，FCTS `1109/1109` 通过；
- 本组沙箱外 `make test`：2026-09-02 在 `ISSUE-G15-015` 与 `ISSUE-G15-016` 修复后重新执行，
  以退出码 `0` 通过；UBSan 与 normal 两阶段的 smoke 均为 `90/90`、std 均为 `604/604`、FCTS 均为
  `1109/1109`；两阶段的 Parser、Semantic、Codegen、runtime、CLI、symbol、性能约束及其余构建 /
  发布脚本回归全部通过；
- 性能与发码：正常落尾分析和 Lambda 目标返回检查只在编译期执行。没有新增运行时分支、调用、分配
  或 ARC；非 `void` callable 的 Codegen 原有 `feng_panic(...)` 防御兜底按人工决定保持原样，没有
  替换为 builtin，也没有新增发码。`Future.await()` 在尚未实现期间明确
  `throw Error("Future.await is not implemented")`；只有实际调用该占位实现时进入错误路径；
- 问题：实施中发现的 16 项问题均已记录并关闭，见
  [G15 问题记录](./feng-language-conformance-coverage-hardening-issues/g15.md)；
- 建议 commit message：`fix(semantic): stabilize function diagnostics and complete G15 coverage`

#### 19.4.1 FUNC-D01：声明冲突

- 新增 `test_g15_signature_and_return_shape_diagnostics`，精确覆盖顶层函数的重复签名、仅返回类型不同、
  定参与变参覆盖冲突，type 实例方法重复签名、static 方法仅返回类型不同、fit 变参冲突和 object-form
  `spec` 方法重复签名；
- 映射 `test_top_level_overload_overlap_via_fit_rejected`、
  `test_top_level_overload_overlap_via_two_specs_rejected`、
  `test_member_method_overload_overlap_via_fit_rejected`、
  `test_fit_method_overload_conflicts_match_method_rules`、
  `test_object_spec_declared_method_overload_diagnostics` 和
  `test_object_spec_accumulated_method_overload_diagnostics` 覆盖当前可见契约关系导致的接受范围重叠；
- 映射 `test_valid_function_overload_by_parameter_type`、
  `test_top_level_overload_two_specs_no_common_type_accepted` 和
  `test_object_spec_parent_parameter_overload_without_concrete_overlap` 覆盖合法邻界。

#### 19.4.2 FUNC-D02 与 FUNC-D02-A：参数声明与 bin 入口

- 新增 `test_g15_signature_and_return_shape_diagnostics`，分别锁定普通函数参数和 Lambda 参数使用
  `void` 时的 `AE0502`；缺失参数类型继续映射 G12，参数绑定类别、重名和可写性继续映射 G13；
- 新增 `test_g15_main_entry_diagnostics`，精确覆盖缺少 `main`、跨 module 两个 `main`、参数数量、参数
  名称、参数类型和非 `void` 返回类型，并验证默认 / `open` 可见性均可作为 bin 入口；同一不合法
  `main(value: i32): i32` 在 lib 目标下按普通函数合法分析。

#### 19.4.3 FUNC-D03、FUNC-D03-A 与 FUNC-D03-B：固定参数及重载调用

- 新增 `test_g15_fixed_argument_call_diagnostics`，覆盖顶层函数、实例方法、静态方法和 callable 值的
  实参数量过少 / 过多；顶层三参数的首、中、末位置类型错误，以及实例、static、callable 值的独立
  类型错误；普通重载无匹配使用 `AE0512`，callable 值参数不匹配使用 `AE0506`；
- FCTS `test_function_return_paths.ff` 直接运行 `i32` 字面量、纯字面量表达式、显式转换和精确类型值在
  首、中、末三个参数位置的合法目标贴合；
- 唯一匹配与实现选择映射 `test_function.ff` 及既有顶层 / 实例 / static 重载 Semantic 用例；普通
  声明期已经拒绝接受范围重叠，调用期多个候选的实际可达边界映射
  `test_intersection_constrained_static_method_call_diagnostics` 和
  `test_object_spec_upcast_rejects_out_of_scope_conversions` 的 `AE0511`，没有制造内部不可达输入。

#### 19.4.4 FUNC-D04 与 FUNC-D04-A：返回形态和正常落尾

- 新增 `test_g15_named_callable_fallthrough_diagnostics`、
  `test_g15_structured_fallthrough_diagnostics` 和 `test_g15_block_lambda_return_diagnostics`，覆盖显式与
  推导返回类型、顶层 / 实例 / static / fit / 泛型函数、块 Lambda、普通调用、`if`、`match`、
  `try/catch`、`while`、三段式 `for`、`for/in`、`break` 与 `continue` 的反向正常落尾边界；
- 新增 `test_g15_structured_return_acceptance` 和 `test_g15_block_lambda_return_acceptance`，覆盖完整分支、
  逃逸 `throw`、本地 catch 返回 / 重新抛出、恒真无出口循环、可退出循环后的明确返回和嵌套循环局部
  `break`；FCTS `test_function_return_paths.ff` 直接运行对应正向路径；
- 新增空 / 有值返回冲突和显式返回表达式不匹配用例；映射
  `test_explicit_non_void_return_rejects_empty_return` 及顶层、实例、static、fit 与块 Lambda 的六条既有
  推导返回冲突断言。原块式 `match` 正向用例已按人工批准改为反向 `AE0515` 用例。

#### 19.4.5 FUNC-D05：void 返回

- 新增 `test_g15_signature_and_return_shape_diagnostics`、
  `test_g15_void_method_valued_return_diagnostics` 和 `test_g15_block_lambda_return_diagnostics`，覆盖
  `void` 顶层函数、实例 / static / fit 方法和块 Lambda 的 `return expr;`，全部在 `return` token
  稳定产生 `AE0501`；
- FCTS `test_function_return_paths.ff` 直接运行 `void` 函数自然结束与显式 `return;`。

#### 19.4.6 FUNC-D06：变长参数

- 映射 `test_variadic_accepts_zero_one_many_arguments`、
  `test_fixed_and_variadic_parameters_accept_calls`、`test_variadic_rejects_mismatched_element_type`、
  `test_variadic_rejects_existing_array_argument`、`test_prepacked_variadic_calls_are_semantically_valid`、
  `test_prepacked_variadic_rejects_non_variadic_target`、
  `test_prepacked_variadic_rejects_prior_variadic_elements`、
  `test_prepacked_variadic_rejects_non_array_expression`、
  `test_prepacked_variadic_rejects_array_element_mismatch`、
  `test_prepacked_variadic_rejects_writable_array` 和
  `test_callable_variadic_shape_mismatches_are_rejected`；
- 合法运行映射 FCTS `test_variadic.ff` 与 `test_variadic_method_value_shape.ff`；构造函数变参继续映射
  G05，不重复新增同构用例。

#### 19.4.7 FUNC-D07：callable 值目标

- 新增 `test_g15_callable_value_target_diagnostics`，覆盖 Lambda 在绑定和省略返回类型的函数返回位置
  缺少目标时的 `AE0520`，以及 static 方法值缺少目标时的 `AE0523`；
- 映射 `test_untyped_single_callable_references_require_named_target`、顶层函数值与方法值的显式类型、
  参数及返回目标选择 / 不匹配用例，以及 `test_concrete_static_method_values_reject_invalid_sources`；
  后者直接覆盖多个来源匹配同一目标的 `AE0521` 和来源签名不匹配的 `AE0522`。泛型来源闭合继续映射
  G25。

#### 19.4.8 FUNC-D08：Lambda 目标签名

- 新增 `test_g15_lambda_target_signature_diagnostics`，分别覆盖单表达式与块 Lambda 的参数数量、参数
  类型和返回类型不匹配；块 Lambda 的具体 `return` 表达式沿用更精确的 `AE1003`，表达式内部
  `try/catch` 返回也纳入目标检查；正反向矩阵同时覆盖子块局部返回、子块同名遮蔽结束后恢复外层
  局部，以及块 Lambda 返回局部 callable；
- FCTS `test_function_return_paths.ff` 对两种 Lambda 分别直接运行显式绑定、函数形参、显式函数返回
  和显式转换四种目标来源，并运行表达式内部 catch 返回的固定宽度目标贴合、子块词法作用域恢复及
  返回捕获型局部 callable；
- 映射 `test_generic_callable_spec_instance_rejects_lambda_parameter_mismatch`、
  `test_generic_callable_spec_instance_rejects_lambda_return_mismatch` 和
  `test_special_member_block_lambda_value_return_uses_lambda_context`，固定块 Lambda 不继承外层 callable
  返回上下文。

#### 19.4.9 FUNC-D09：合法运行邻界

- 新增 FCTS `test_function_return_paths.ff`，直接运行顶层函数、实例 / static / fit 方法、callable 值、
  单表达式 / 块 Lambda、显式与推导返回、完整分支、异常、循环、固定宽度参数及 `void` 返回；
- 普通合法重载和变参运行分别映射 `test_function.ff`、`test_function_inference.ff`、`test_variadic.ff`、
  `test_lambda.ff` 和既有 method-value FCTS，避免复制已有专项。

#### 19.4.10 FUNC-D10：稳定诊断收敛

- 只迁移 `ISSUE-G15-002` 明确批准的十条既有断言，并新增 G15 精确 Semantic 矩阵；没有修改白名单外
  的既有错误码断言；
- 新增用例全部锁定一个根因诊断、来源文件、错误码、token、行列和必要消息片段。fit 方法 AST 的
  callable token 统一为方法名称，不为 `AE0515` 增加声明面特判。

## 20 G16：流程控制诊断

### 20.1 测试重点

验证条件、循环及控制转移规则产生的稳定 Semantic 诊断，并为能够合法执行的邻界建立或映射 FCTS
直接运行证据。本组同时修复实施前发现的一项窄 Parser 缺口：三段式 `for` 的更新子句不得使用
`let` / `var` 声明绑定。

`if`、块形式 `match` 与带 `catch` 的 `try` 作为表达式使用时，每个结果分支形成控制转移边界：
分支内的 `break` / `continue` 不得作用于该表达式外部的循环。这里的限制按控制目标判断，不只检查
分支块的直接子语句；嵌套普通 block 或普通 `if` 中的跳转如果仍以表达式外部循环为目标，同样非法。
结果分支内部另行声明的 `while`、三段式 `for` 或 `for/in` 则建立新的合法目标，其循环体内的
`break` / `continue` 仍可正常使用。相同结构作为语句使用时不形成该表达式边界。

三段式 `for` 只有初始化子句可以声明 `let` / `var` 绑定；更新子句只能操作此前已经可见的绑定或
执行函数调用，不得引入新绑定。`for/in` 的绑定关键字、一级 tuple 模式和逐轮身份继续遵循已交付
G07；语法形态映射 G12，同级重名和 `let` / `var` 可写性映射 G13，不在本组重复排列。

### 20.2 用例 TODO

#### 20.2.1 `break` / `continue` 上下文

- [x] FLOW01：分别覆盖函数普通块及其嵌套普通 block 中没有任何包围循环的 `break`，以及外层循环体
  中新建块 Lambda 后在 Lambda body 直接使用 `break`；均在 `break` token 产生唯一 `AE1201`，证明
  普通块不建立循环目标、callable 边界不能继承外层循环；
- [x] FLOW01-A：在包围循环中，分别把 `break` 放入值形式 `if` 的结果分支、值形式 `match` 的普通
  分支 / `else` 分支和 `try` 表达式的结果 catch 分支，并使其只能指向表达式外部循环；`if` / `match`
  使用 `AE1110`，catch 使用 `AE1405`，诊断均位于 `break` token。嵌套普通 block 或普通 `if` 不得
  绕过该边界；
- [x] FLOW01-B：映射 defer 直接位置的 `AE1504` 既有证据；在值形式 `if`、`match`、`try` 的结果
  分支内分别新建一个循环并在该内层循环中执行 `break`，全部必须合法并进入 FCTS 直接运行；语句
  形式的 `if` / `match` / `try` 位于循环内时，分支中的 `break` 可以作用于包围该语句的最近循环；
- [x] FLOW02：对 FLOW01、FLOW01-A 和 FLOW01-B 的每个上下文建立对应 `continue` 矩阵；普通块与
  Lambda callable 边界使用 `AE1201`，值形式 `if` / `match` 使用 `AE1110`，结果 catch 使用
  `AE1405`，defer 直接位置映射 `AE1505`；结果分支内新建循环及语句形式分支的合法控制目标必须在
  FCTS 中直接运行。

#### 20.2.2 条件类型

- [x] FLOW03：普通 `if` 的首个条件与 `else if` 条件、直接值形式 `if` 及值形式 `else if` 链的内层
  条件分别使用最小非 `bool` 表达式触发 `AE1102`；每个诊断必须定位实际出错条件表达式的起始 token，
  不能把后续 `else if` 错误定位到首个 `if`；
- [x] FLOW03-A：`while` 与三段式 `for` 的非空条件分别使用最小非 `bool` 表达式触发 `AE1202`，定位
  实际条件表达式起始 token；三段式 `for` 省略条件继续表示恒真，不产生条件类型诊断；
- [x] FLOW03-B：在 FCTS 中直接运行布尔字面量、返回 `bool` 的调用和 infix `match` 结果分别作为
  `if` / `else if`、`while` 与三段式 `for` 条件的合法邻界；不得把数值、字符串或对象视为 truthy。

#### 20.2.3 `for/in` 目标与循环绑定

- [x] FLOW04：内建标量与没有 `@iterable` / `@iterator` 的普通对象分别作为 `for/in` 目标时，在
  迭代表达式起始 token 产生唯一 `AE0326`；数组 `T[]`、`T[!]`、普通 `@iterable` 容器和 self-cursor
  `@iterator` 继续合法，并在 FCTS 中直接运行。迭代协议声明自身的签名诊断只建立既有映射，不混入
  本组目标可迭代性用例；
- [x] FLOW05：`for/in` 一级 tuple 解构分别覆盖“序列元素不是具名 tuple”的 `AE0108` 和“模式位置数
  与元素 tuple 不一致”的 `AE0109`，诊断位于模式绑定 token；零 tuple、带空位及位置数完全匹配的
  模式继续合法。嵌套 / 单位置 / 类型标注 / 省略 `let|var` 等语法错误映射 G12，同一模式重名及
  `let` 分量赋值映射 G13，不重复新增同构诊断；
- [x] FLOW05-A：三段式 `for` 的更新子句以 `let` 或 `var` 开始时，Parser 均在绑定关键字处产生唯一
  `SE1203`，返回空 AST 且不得进入 Semantic；初始化子句中的 `let` / `var`、更新子句对既有绑定的
  普通赋值 / 复合赋值及函数调用继续合法，并建立精确 Parser AST 与 FCTS 运行证据。

#### 20.2.4 合法嵌套邻界

- [x] FLOW06：在 FCTS 中覆盖 `while`、三段式 `for`、数组及迭代器协议 `for/in` 的 `break` /
  `continue`，并覆盖异构嵌套循环只作用于最近一层；已有 G08 / `test_flow.ff` / `test_iterator.ff`
  直接证据足够的行为只建立映射，缺少的表达式结果分支内层循环和语句形式分支外层控制目标才新增；
- [x] FLOW06-A：Semantic 正向矩阵必须同时证明普通嵌套 block 不改变最近循环目标、Lambda callable
  边界内自行声明循环后可使用自己的 `break` / `continue`，以及值表达式结果分支内自行声明的循环
  不受表达式外部控制转移禁令影响；所有可执行边界同步进入 FCTS。

### 20.3 独立验收与交付 TODO

- [x] 按人工确认更新流程控制主规范、SE / AE 分段错误码规范及中英文用户手册，明确表达式结果分支
  的外部循环控制转移边界、内层循环合法邻界和更新子句绑定禁令；
- [x] 将 if 表达式当前产生的旧 `AE0032`、if 语句当前产生的旧 `AE0054` 收敛为 `AE1102`，将
  `while` / 三段式 `for` 当前产生的旧 `AE0054` 收敛为 `AE1202`；只修改这四类 G16 产生点，不开展
  其他旧错误码迁移；
- [x] 为值形式 `match` 的结果分支建立与值形式 `if` 相同的外部循环目标隔离，同时保留结果分支
  内层循环；不得通过禁止整个分支子树中的控制转移实现；
- [x] Parser 使用 `SE1203` 拒绝三段式 `for` 更新子句中的 `let` / `var`，不得影响初始化子句绑定、
  `for/in` 显式绑定或合法更新表达式的 AST；
- [x] 建立 `AE1201`、`AE1110`、`AE1405`、`AE1504`、`AE1505`、`AE1102`、`AE1202`、`AE0326`、
  `AE0108`、`AE0109` 与 `SE1203` 到现有或新增测试的逐项映射；
- [x] 本组按计划新增 Parser、Semantic 与 FCTS 用例；实施中为通用分支结果 Codegen 缺陷增加一项
  Codegen 正向回归，但没有修改既有测试；既有
  `test_break_inside_loop_inside_if_expr_block_is_accepted` 和
  `test_break_inside_loop_inside_try_expr_catch_block_is_accepted` 必须保持正向且继续通过。若实施中发现
  必须修改任何既有用例，先记录问题并再次取得人工决策；
- [x] 每个新增反向程序核对唯一诊断、稳定码、触发 token、行列、来源文件及 Parser / Semantic 阶段；
  每个可执行合法邻界均建立或映射 FCTS 直接运行证据；
- [x] 独立运行 G16 的 Parser、Semantic、Codegen 与 FCTS 专项，核对诊断及实际控制流结果；所有实现
  变更必须只影响编译期，不得新增生成程序的运行时分支、调用、分配或 ARC；
- [x] 在 Codex 沙箱外为 G16 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G16 问题；
- [x] 填写本组映射、实际新增用例、专项结果、全量结果、性能与兼容性结论。

### 20.4 独立交付记录

- 状态：已交付；
- 稳定码映射：值 / 语句形式 `if` 的非 `bool` 条件统一为 `AE1102`，`while` / 三段式 `for` 的
  非 `bool` 条件统一为 `AE1202`；无合法循环目标继续使用 `AE1201`，值形式 `if` / `match` 的外部
  循环控制转移使用 `AE1110`，结果 catch 使用 `AE1405`，defer 映射既有 `AE1504` / `AE1505`；
  非可迭代目标、tuple 解构目标 / 数量错误分别映射 `AE0326`、`AE0108`、`AE0109`；更新子句绑定声明
  新增并锁定 `SE1203`；
- 实际新增入口：Parser 新增 2 个 G16 测试函数，Semantic 新增 5 个 G16 测试函数，Codegen 新增
  1 个共享分支结果测试函数；FCTS 新增 `test_g16_flow_control.ff` 并由 `fcts_bin` 主入口登记，其中
  5 条可执行断言覆盖合法条件、语句 / 值表达式控制目标、更新形态和 Lambda 内层循环；没有修改
  任何既有测试用例；
- 本组专项结果：Parser、Semantic、Codegen 测试程序全部通过；FCTS 为 `1114/1114`，性能约束通过；
- 本组沙箱外 `make test`：2026-09-02 以退出码 `0` 通过；UBSan 与 normal 两阶段的 smoke 均为
  `90/90`、std 均为 `604/604`、FCTS 均为 `1114/1114`；两阶段的 Parser、Semantic、Codegen、
  runtime、CLI、symbol、增量构建、发布脚本、性能约束及其余回归全部通过；
- 性能与兼容性：实现变更仅影响编译期 Parser、Semantic 和 Codegen 收集 / 类型选择；没有新增生成
  程序的运行时分支、调用、分配或 ARC，没有变更 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- 问题：实施中发现的 8 项问题均已记录并关闭，见
  [G16 问题记录](./feng-language-conformance-coverage-hardening-issues/g16.md)；
- 建议 commit message：`fix(compiler): stabilize control-flow diagnostics and complete G16 coverage`

#### 20.4.1 FLOW01、FLOW01-A、FLOW01-B 与 FLOW02：控制转移上下文

- `test_g16_base_loop_control_diagnostics` 锁定普通块、无循环函数体及 Lambda callable 边界的
  `AE1201`；`test_g16_expression_loop_boundary_diagnostics` 锁定值形式 `if` / `match` 的 `AE1110` 与
  结果 catch 的 `AE1405`，同时覆盖嵌套普通 block / 普通 `if` 不能绕过表达式边界；
- `test_g16_control_flow_acceptance` 与 FCTS `test_g16_flow_control.ff` 证明语句形式分支仍可控制最近外层
  循环，值表达式结果分支内新建的 `while`、三段式 `for` 和 `for/in` 仍可控制自身；defer 直接位置
  映射既有 `AE1504` / `AE1505` 用例。

#### 20.4.2 FLOW03、FLOW03-A 与 FLOW03-B：条件类型

- `test_g16_condition_type_diagnostics` 对普通 / 值形式 `if` 的首条件与 `else if`、`while` 和三段式
  `for` 分别锁定 `AE1102` / `AE1202`、实际条件 token、行列、来源文件和唯一诊断；
- FCTS 直接运行布尔字面量、返回 `bool` 的调用、infix `match` 条件以及省略条件的三段式 `for`。

#### 20.4.3 FLOW04 与 FLOW05：`for/in` 目标和 tuple 模式

- `test_g16_for_in_diagnostics` 锁定标量 / 普通对象不可迭代的 `AE0326`、非具名 tuple 元素的
  `AE0108` 及 tuple 位置数不一致的 `AE0109`；合法数组、迭代协议、self-cursor 与 tuple 空位映射
  G07、G08 既有 FCTS，并由 G16 Semantic 正向矩阵复验。

#### 20.4.4 FLOW05-A：三段式 `for` 更新子句

- `test_g16_for_update_binding_declarations_are_rejected` 精确锁定更新子句 `let` / `var` 的 `SE1203` 和
  空 AST；`test_g16_for_update_legal_ast_boundaries` 锁定初始化绑定、普通赋值、复合赋值、函数调用与
  空更新的 AST；FCTS 直接运行这些合法形态；
- 实施中发现并修复函数调用更新吞掉随后循环体 `{}` 的 Parser 缺陷，使用控制流头共享的对象字面量
  后缀抑制上下文，不影响普通表达式或真正的对象字面量初始化。

#### 20.4.5 FLOW06 与 FLOW06-A：合法嵌套和分支结果

- `test_g16_control_flow_acceptance` 证明普通嵌套 block、Lambda 自有循环和值表达式结果分支内层循环的
  合法目标；`test_g16_branch_local_expression_results_codegen` 覆盖 `if`、`match`、`try` 在前置局部
  声明 / 内层循环之后返回该局部值；
- FCTS 直接运行相同控制流，并复验既有 `test_value_method_capture.ff`。Codegen 只复用 Semantic 已选
  类型并在完全闭合的泛型 owner 上补全成员预收集，没有重复发码或新增运行时开销。

## 21 G17：异常语法、载荷、捕获与传播

### 21.1 测试重点与范围

验证 `throw`、`try/catch`、异常载荷、精确类型捕获、兜底捕获、原样重抛、表达式结果和普通 Feng
调用链传播的正向行为与反向诊断。语法依据统一引用
[异常规范](../specifications/feng-exception.md)，表达式结果与控制转移依据统一引用
[流程控制规范](../specifications/feng-flow.md)，本文不重复定义语言语义。

Feng 普通函数没有 `throws` 声明，也不要求调用方声明或显式标注传播；异常是否逃逸普通 Feng callable
不是类型签名的一部分。因此原 EXC04“函数异常声明与实现不一致”和 EXC05“调用方未满足异常传播
要求”不属于现有语言，不建立不存在的 checked-exception 反向用例。二者由本节的普通传播、捕获和
原样重抛正向行为替代。

本组明确不处理异常跨 `@abi` 边界的静态传播分析、跨 package-public `.ft` 的异常效果事实或 ABI
终止兜底；不修改 `AE1313`、`.ft` 格式、公开 ABI 或 runtime 私有 ABI。该范围须另行完成语义设计和
人工决策，不能计入 G17 已覆盖结论。

值形式的 `if`、块形式 `match` 和带 `catch` 的 `try` 共用路径级结果规则：只有能够正常执行到分支块
末尾结果表达式的路径才产生结果；返回所属 callable 的 `return` 路径与逃逸当前结果结构的 `throw`
路径不产生结果，也不参与既有目标类型选择与贴合。若没有末尾结果，只有每条可达路径均通过上述
`return` / `throw` 离开时才合法；存在正常无值落尾时必须编译期报错。嵌套 Lambda 的 `return` 只
属于该 Lambda，被内部 `try/catch` 捕获并正常继续的 `throw` 仍须最终产生块结果。实现不得改变
`if` / `match` / `try` 的目标类型选择、字面量贴合、分支顺序或推导算法，不得修改 Parser、runtime、
ABI 或 `.ft`，也不得新增生成程序的运行时判断、分支、调用、分配或 ARC。

### 21.2 用例 TODO

#### 21.2.1 `try/catch` 语法边界

- [x] EXC01：`try` 后必须是单个表达式并至少跟一个 `catch`；分别锁定裸 `try expr`、`try { ... }`、
  缺失 `catch` body、绑定形式缺失名称或类型、语句形式末尾多余分号的 Parser 诊断码、实际 token、
  行列、唯一诊断和空 AST；已有 G12 精确证据只建立映射，不重复新增同构用例；
- [x] EXC02：匿名 `catch { ... }`、`catch name: Type { ... }`、多个具体类型分支后接一个
  `catch name: unknown` 或匿名兜底分支均可通过 Parser；语句形式不要求结果表达式，值形式必须按
  EXC12～EXC15 提供结果或终止路径；
- [x] EXC03：`catch name { ... }`、`catch : Type { ... }` 及其他不完整绑定形态分别使用现有
  `SE1402` / `SE1403` 契约，匿名 `catch` 不得被误判为缺失名称。

#### 21.2.2 异常载荷与具体捕获类型

- [x] EXC04：`throw` 与具名 `catch` 对规范允许的完整具体类型矩阵使用同一个判定结果；覆盖
  `bool`、全部固定宽度整数与浮点、`string`、具名 enum、实体 `type`、具名 tuple、`@value type`、
  闭合泛型实例及规范允许的 `@abi` 类型形态，并为可执行类别建立或映射 FCTS 直接行为证据；
- [x] EXC05：`throw` 与具名 `catch` 对 array、全部 `spec` 形态、直接类型参数、仍含开放参数的泛型
  实例、pointer、`void`、函数 / 方法 / Lambda / callable-form 值分别产生精确 Semantic 反向证据；
  同一非法类型在 throw 与 catch 入口必须给出一致的拒绝原因，`void` throw 保留更精确的 `AE1402`；
- [x] EXC06：不同具名 enum、enum 与底层整数、不同具名 tuple、布局相同的不同 `@value type`、同一
  泛型类型的不同闭合实参分别不互相捕获；相同精确类型可以捕获并在 FCTS 中观察载荷值，证明匹配按
  完整名义类型身份而不是布局或底层值。

#### 21.2.3 捕获顺序、绑定和重抛

- [x] EXC07：具体 `catch` 按书写顺序匹配，第一个精确命中分支执行且后续分支不执行；多个未命中
  分支后由 `unknown` 或匿名兜底捕获，并在 FCTS 中通过互异结果观察实际选择；
- [x] EXC08：`catch name: unknown` 与匿名 `catch` 必须位于最后；后续仍有任意 `catch` 时使用
  `AE1406`，分别核对两种兜底形态的诊断位置、数量和阶段；
- [x] EXC09：具名 catch 绑定为 `let`，其头部作用域、body 子块遮蔽、不同子句复用同名绑定按 G13
  已确认规则工作；直接重新赋值被拒绝，合法读取具体类型载荷进入 FCTS；
- [x] EXC10：`catch error: unknown` 中仅允许 `throw error` 原样重抛；把 `error` 用作绑定初始化值、
  参数、返回值、成员 / 方法接收者、普通 throw 子表达式或其他运算时，均在实际使用 token 产生
  `AE1404`；匿名 catch 中引用不存在的异常名称仍按普通未定义名称诊断；
- [x] EXC11：具体类型 catch 可读取载荷、调用合法成员并以 `throw error` 重新抛出；外层相同具体
  类型或 `unknown` catch 能捕获原值，FCTS 直接验证一次及多层重抛后的类型和值不变。

#### 21.2.4 `try` 表达式结果与 callable 返回

- [x] EXC12：非 `void` 值形式 `try` 的主表达式与每个可正常完成的 catch 均产生结果；空 catch、
  仅含普通语句的 catch、条件退出后仍可能正常无值落尾的 catch 使用 `AE1401`，诊断定位该 catch；
  每条路径均通过所属 callable 的 `return` 或逃逸当前 `try/catch` 的 `throw` 离开时无需结果；
- [x] EXC13：主表达式、具体 catch 和兜底 catch 的结果按流程控制规范 §4.1 统一贴合；覆盖显式目标、
  无目标时首个确定非字面量目标、全数字字面量默认推导、越界字面量、union 完整目标及结果类型不
  兼容，所有合法可执行组合进入 FCTS；
- [x] EXC14：值形式 try 的 catch 在直接位置、普通嵌套 block、嵌套 `if` / `match`、循环和嵌套
  `try/catch` 中返回所属 callable 均合法；该路径不产生 catch 结果，其他正常路径仍须产生结果；
  `return` 后的不可达末尾表达式不得参与 try 结果贴合；
- [x] EXC15：值形式 catch 的 `return` 值继续按所属函数、方法或 Lambda 的显式 / 推导返回类型检查；
  返回路径执行普通 return 清理、活动异常 frame 清理和已登记 `defer`。catch 内嵌套 Lambda 的
  `return` 只属于该 Lambda，不得被计为外层 catch 路径退出；
- [x] EXC16：值形式 `if` / `match` 按与 catch 相同的路径规则支持外围 callable `return`；覆盖直接
  返回、部分路径返回后由正常路径产生结果、嵌套结构全部返回 / 抛出、return 后不可达结果，以及
  嵌套 Lambda 自有返回。三种值表达式必须复用同一控制流分类，不得为 `try` 添加特判。

#### 21.2.5 普通 Feng 调用链传播

- [x] EXC17：未捕获异常跨一层和多层普通函数 / 方法 / Lambda 调用传播，由最外层精确类型或兜底
  catch 捕获；传播不要求 callable 声明异常，也不要求调用点添加额外语法，FCTS 直接验证捕获结果；
- [x] EXC18：内层具体 catch 完成后不再传播，内层未匹配异常继续向外传播，内层 `throw` 新值按新值
  类型重新匹配，原样重抛保持原类型和值；每种路径都以互异可观察结果证明只执行预期分支一次；
- [x] EXC19：异常路径执行已经登记的 `defer`，并保持既有托管值清理与传播行为；只建立到 defer、
  生命周期和现有异常清理专项的直接证据映射，缺少可观察运行证据时才新增 FCTS，不在 G17 改 runtime。

#### 21.2.6 结果路径分类与既有类型推导回归

- [x] EXC20：`if` / `match` / catch 的直接末句 `throw`、嵌套全部 `throw`、`return` / `throw` 混合终止
  均识别为无结果路径；被内部 `try/catch` 捕获并正常继续的 `throw` 不得误判为逃逸；
- [x] EXC21：无末尾结果且至少存在一条正常落尾路径时，`if` / `match` 使用既有 `AE1101`、非 `void`
  catch 使用既有 `AE1401`；条件无 `else`、局部捕获后落尾、普通语句落尾分别建立精确反向证据；
- [x] EXC22：`return` / 逃逸 `throw` 后的不可达末尾表达式不作为结果候选，不得因其类型造成分支贴合
  错误；不可达代码是否另行诊断保持现状；
- [x] EXC23：复验显式上下文目标、无上下文首个确定非字面量目标、全数字字面量默认推导、union 完整
  目标和不兼容结果诊断，确认本次没有改变 `if` / `match` / `try` 的类型推导规则、分支顺序或逻辑；
- [x] EXC24：Codegen 只在正常结果路径写既有结果槽；纯 `return` / `throw` 结果块不写槽，返回与异常
  路径分别复用既有清理，不新增运行时判断、调用、分配、ARC 或接口。

### 21.3 已知问题、既有用例迁移与实现 TODO

- [x] 按 `ISSUE-G17-001` 修正主异常规范和中英文用户手册中“catch 可省略”的旧描述；实际 Parser
  继续要求至少一个 catch，不改语法实现；
- [x] 首次交付按 `ISSUE-G17-002` 删除值形式 try 将末尾 `return` 当作 catch 结果的 Semantic 特例，
  并建立三种值表达式共用的外围-callable return 禁令；该禁令已由后续人工决策废止，历史原因和
  首次交付证据保留在问题记录；
- [x] 按 `ISSUE-G17-007` 将 Semantic 与 Codegen 的末句直接 `throw` 特判替换为共用的路径级结果
  分类，支持值表达式结果块返回所属 callable；只改变结果候选收集与路径发码，不改变现有目标类型
  选择、字面量贴合、分支顺序和类型推导算法；
- [x] 实施 EXC14～EXC16 前须由人工明确批准以下既有测试迁移；批准后仅允许迁移该清单，且必须保留
  各项原测试重点，不能简单删除：
  - `test/semantic/test_semantic.c` 的 `test_g15_lambda_target_signature_diagnostics` 中嵌套 try case，改用
    非法 catch 结果类型继续验证 Lambda 目标签名；
  - `test/semantic/test_semantic.c` 的 `test_g15_structured_return_acceptance` 中值形式 catch-return case，
    改为合法 catch 结果表达式，并另由 EXC14 精确反向用例锁定 return 禁令；
  - `test/codegen/test_codegen.c` 的 `test_try_catch_return_codegen`，改为语句形式 catch 直接返回，并保留
    异常 frame、unwind 清理和生成 C 可编译断言；
  - `test/smoke/phase1a/exception_return.ff` 及其 `.expected`，改为语句形式 catch 直接返回，保持原运行
    输出；
  - `fcts/fcts_bin/src/test_exception.ff` 的 `ex_return_from_catch` 及对应断言，改为语句形式 catch 返回，
    保留“语句 catch 可退出 callable”的行为证据；
  - `fcts/fcts_bin/src/test_function_return_paths.ff` 的三个值形式 catch-return 路径及对应断言，改用 catch
    结果表达式或直接返回整个 try 表达式，保持原正常 / 异常路径结果；
- [x] 后续人工决策废止首次交付的表达式分支 `return` 禁令，因此将
  `test_g17_expression_return_boundary_diagnostics` 的 11 个旧反向输入迁移为路径级正向与反向矩阵；
  迁移只替换已失效的禁令断言，外围 callable 返回类型检查、嵌套 Lambda 边界和非法外层
  `break` / `continue` 诊断继续保留独立证据；
- [x] 除上述明确列出的既有用例外，本组只新增测试；发现其他既有测试必须修改时先记录原因并再次由
  人工决策；
- [x] 不迁移或扩展 ABI 异常效果测试，不修改 `AE1313`、`.ft`、公开 ABI 或 runtime 私有 ABI；发现
  相关失败时记录为延期问题，不在 G17 内顺带修复；
- [x] 明显重复或使用错误的异常诊断码只可在新增用例直接覆盖的产生点内修正；不做与新增覆盖无关的
  错误码整理或代码重构。

### 21.4 独立验收与交付 TODO

- [x] 建立 EXC01～EXC19 到现有 Parser、Semantic、Codegen、smoke 与 FCTS 的逐项映射，只为没有直接
  证据的边界新增用例；Semantic 接受不能代替可进入 FCTS 的运行行为证据；
- [x] 独立运行 G17 Parser / Semantic / Codegen 专项，核对诊断码、实际 token、行列、数量、阶段、
  类型身份、表达式结果和 callable 边界；
- [x] 独立运行 G17 FCTS 与 smoke 追加 / 迁移用例，核对精确捕获、兜底、重抛、普通调用链传播、
  语句 catch 返回及嵌套 Lambda 返回；
- [x] 复核生成代码差异，确认 return 边界修复和诊断补齐只影响编译期，合法程序没有新增运行时判断、
  分支、调用、分配、ARC 或 runtime 接口；
- [x] 在 Codex 沙箱外为 G17 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或决策 G17 问题；
- [x] 填写本组映射、实际新增 / 迁移用例、专项结果、全量结果、性能结论和交付结论。
- [x] 独立运行 G17 后续优化的 Semantic / Codegen / smoke / FCTS 专项，覆盖 EXC12、EXC14～EXC16、
  EXC20～EXC24；
- [x] 在 Codex 沙箱外为 G17 后续优化重新执行完整 `make test`，并核对 UBSan、normal、性能约束和
  全部既有回归；
- [x] 执行 `git diff --check`，确认 Parser、runtime、ABI、`.ft` 及类型推导逻辑没有越界变更，关闭
  `ISSUE-G17-007` 并填写后续交付记录。

### 21.5 独立交付记录

- 状态：首次交付已完成；路径级 `return` / `throw` 后续优化另见 §21.6；
- 稳定码映射：`try` / catch 绑定语法继续使用 `SE1401`～`SE1403`；非法 throw / 具体 catch 类型分别
  映射既有 `AE0076` / `AE0179`，void throw 使用 `AE1402`；值形式 catch 缺少结果使用 `AE1401`，
  `unknown` 非原样重抛使用 `AE1404`，兜底 catch 非末位使用 `AE1406`；`AE1110` / `AE1405` 在后续
  优化完成后只保留表达式结果分支向外层循环执行 `break` / `continue` 的诊断，匿名 catch 内引用
  不存在的名称继续使用 `AE0001`；
- 实际新增：Semantic 新增 5 个 G17 测试函数，覆盖两种兜底顺序、6 类非法 `unknown` 使用、匿名 catch
  无绑定、3 类缺失 catch 结果、直接重抛 / 终端 throw、11 个外围 return 反向程序及语句 / Lambda
  正向矩阵；FCTS 新增 `test_g17_exception_semantics.ff` 并登记 9 条运行用例；
- 既有迁移：只修改 §21.3 人工批准的 Semantic、Codegen、smoke 及两处 FCTS 用例；语句 catch 返回
  继续返回外围 callable，值形式 catch 改用结果表达式，原有 smoke 输出保持不变；没有修改清单外的
  既有测试用例；
- 本组专项结果：Parser、Semantic、Codegen 测试程序全部通过；smoke 为 `90/90`，FCTS 为
  `1123/1123`；
- 本组沙箱外 `make test`：2026-09-02 以退出码 `0` 通过；UBSan 与 normal 两阶段的 smoke 均为
  `90/90`、FCTS 均为 `1123/1123`，两阶段的 Parser、Semantic、Codegen、runtime、CLI、symbol、
  std、增量构建、发布脚本、性能约束及其余回归全部通过；
- 性能与 ABI：产品实现只修改 Semantic 编译期上下文与检查；Codegen、runtime、`.ft` 和公开 ABI
  没有产品代码差异，合法程序不新增运行时判断、分支、调用、分配或 ARC；跨 ABI 异常效果分析继续
  按 `ISSUE-G17-003` 延期；
- 问题：共记录 8 项问题，其中 7 项已关闭、1 项延期；后续优化的 `ISSUE-G17-007` 与覆盖复核的
  `ISSUE-G17-008` 均已关闭，见
  [G17 问题记录](./feng-language-conformance-coverage-hardening-issues/g17.md)；
- 建议 commit message：`fix(semantic): enforce exception result boundaries and complete G17 coverage`

#### 21.5.1 EXC01～EXC03：语法边界

- Parser 的 `test_try_block_form_is_rejected`、`test_try_expression_with_typed_catches`、
  `test_try_statement_ends_at_final_catch_brace`、`test_try_statement_rejects_trailing_semicolon`、
  `test_try_without_catch_is_rejected` 与 G12 `test_g12_control_flow_and_try_syntax` 共同覆盖合法匿名 / 具名 /
  多 catch AST，以及 `try` 块形态、缺 catch、缺名称、缺冒号 / 类型、缺 body 和多余分号的精确
  Parser 诊断、实际 token、行列、数量与空 AST；无需新增同构 Parser 用例。

#### 21.5.2 EXC04～EXC06：载荷集合与名义身份

- Semantic 的 `test_exception_payload_positive_type_matrix` 与
  `test_exception_payload_negative_type_matrix` 对 throw / typed catch 使用同一允许和拒绝矩阵；既有
  void、函数、方法、Lambda、pointer、spec 和 unknown 位置专项补齐稳定原因；
- FCTS `test_exception_payload.ff` 运行全部内建标量、string、实体、enum、tuple、`@value type`、闭合
  泛型和 `@abi` Feng 类型，并以不同 enum、底层整数、不同 tuple / value type、不同泛型实参及
  跨包 enum 验证精确名义匹配；聚合载荷的捕获、重抛和释放次数也在同一文件直接观察。

#### 21.5.3 EXC07～EXC11：捕获顺序、绑定与重抛

- 新增 `test_g17_catch_order_and_unknown_diagnostics` 精确锁定两种兜底非末位的 `AE1406`、匿名 catch
  的 `AE0001`，以及 `unknown` 绑定作为初始化值、参数、返回值、成员接收者、普通 throw 子表达式
  和运算操作数时实际 token 上的 `AE1404`；直接 `throw error` 继续通过；
- G13 的 catch `let` 赋值、作用域、body 遮蔽与兄弟子句隔离用例直接映射 EXC09；
- FCTS `test_exception.ff`、`test_exception_payload.ff` 与新增 G17 文件直接观察首个精确 catch、兜底、
  具体载荷读取、具体重抛及一层 / 两层 unknown 原样重抛的类型和值。

#### 21.5.4 EXC12～EXC16：首次交付的表达式结果与 callable return 边界

- 新增 `test_g17_try_result_presence_diagnostics` 锁定空 body、仅 binding 和仅 assignment 的
  `AE1401`；`test_g17_rethrow_and_terminal_throw_acceptance` 证明终端 throw 路径不要求结果；既有 try
  结果显式目标、首个非字面量目标、全字面量、越界、union 和不兼容类型 Semantic/FCTS 覆盖直接
  映射 EXC13；
- 新增 `test_g17_expression_return_boundary_diagnostics` 对值形式 catch 的直接位置、return 后结果、
  普通 block、嵌套 if / match、循环和嵌套 try 锁定 `AE1405`，并复验值形式 if / match 的 `AE1110`；
  每项均定位实际 return token 且只有一个诊断；
- `test_g17_expression_return_boundary_acceptance` 与新增 FCTS 证明语句形式 if / match / try 可经普通
  嵌套块返回当前 callable，值形式三者内部的 Lambda 可合法返回自身；获批 Codegen 与 smoke 迁移
  继续覆盖语句 catch 返回、异常 frame / unwind 清理及相同运行输出。

以上 return 禁令是首次交付记录，已由 `ISSUE-G17-007` 的后续人工决策废止；新语义和新验收结果在
§21.2.4、§21.2.6 与下节记录，不能继续把本段历史反向用例当作当前语言行为。

#### 21.5.5 EXC17～EXC19：普通传播与清理

- 新增 FCTS 直接观察异常跨两层函数、两层方法和 Lambda + 函数调用传播；分别证明内层捕获后停止、
  未匹配继续传播、替换值按新类型匹配、具体重抛及 unknown 原样重抛保持原类型和值；
- EXC19 映射 `test_defer.ff` 的异常路径 defer、`test_try_branch_exception_cleanup.ff` 的分支局部清理、
  `test_exception_payload.ff` 的聚合载荷释放，以及 `exception_cleanup` smoke；本组未修改 runtime。

### 21.6 路径级 `return` / `throw` 后续优化交付记录

- 状态：已完成；
- 规范边界：保持 §4.1 既有统一目标类型选择、字面量贴合、分支顺序和类型推导算法不变，只把结果
  候选识别从“块末直接 `throw`”扩展为可达路径分类；`return` 返回所属函数、方法或 Lambda，逃逸
  `throw` 沿用现有异常语义，两者均不产生当前分支结果；
- 实际实现：Semantic 新增纯编译期三态结果块分类并复用既有 callable 可达性分析；Semantic 与
  Codegen 删除各自的末句 `throw` 判定，统一消费同一分类。Codegen 的 `if`、`match`、catch 仅在
  正常结果路径写既有结果槽，退出路径复用既有 return lowering 或异常展开；同时修复所有分支均
  退出时 `if` 旧前置判断误报 `CE0199`，并避免非法外层 `break` / `continue` 级联结果诊断；
- Semantic 用例：将 11 个已失效的 return 禁令输入迁移并扩展为 4 组路径用例，覆盖 `if` / `match` /
  catch 的直接返回、普通与嵌套返回、部分返回后正常产生结果、全部 `return` / `throw`、不可达异型
  末尾表达式、外围 callable 显式与推导返回类型、嵌套 Lambda、已捕获异常、无限循环及正常无值
  落尾；值形式 catch 直接覆盖普通 block、嵌套 `if` / `match`、循环和嵌套 `try/catch`；既有显式
  目标、首个确定非字面量、全数字字面量、union 与不兼容类型用例继续原样通过；
- Codegen 与行为用例：新增 1 组 Codegen 生成 C 编译用例、1 个 smoke 程序和 7 条 FCTS 行为断言，
  覆盖函数、实例方法、Lambda 的返回归属，`if` / `match` / `try` 的值、返回、抛出与嵌套组合，
  managed string、catch 活动异常清理和 `defer`；
- 专项结果：Semantic、Codegen 均通过；smoke 为 `91/91`，FCTS 为 `1130/1130`；
- 完整回归：2026-09-02 在 Codex 沙箱外执行 `make test`，退出码为 `0`；UBSan 与 normal 两阶段的
  smoke 均为 `91/91`、FCTS 均为 `1130/1130`，Parser、Semantic、Codegen、runtime、CLI、symbol、
  std、增量构建、发布脚本、性能约束及其余回归全部通过；
- 性能与边界：路径分类仅发生在编译期；没有新增生成程序的运行时判断、分支、调用、分配或 ARC，
  性能约束通过；Parser、runtime、公开 ABI、runtime 私有 ABI、`.ft` 与跨 ABI 异常效果分析均未改；
- 问题：`ISSUE-G17-007` 与 `ISSUE-G17-008` 已关闭，具体分析与处理见
  [G17 问题记录](./feng-language-conformance-coverage-hardening-issues/g17.md)；
- 建议 commit message：`feat(semantic): support path-aware return and throw branches`

## 22 G18：普通类型与值诊断

### 22.1 测试重点与范围

验证非泛型普通值在确定目标类型位置中的匹配、显式转换、对象构造、对象字面量、成员访问方式和
默认零值资格，并锁定对应的 Semantic 稳定诊断及 Codegen 内部不变量。语言规则统一引用
[变量绑定与作用域规范](../specifications/feng-binding.md)、
[表达式与运算规范](../specifications/feng-expression.md)、
[内建类型规范](../specifications/feng-builtin-type.md)和
[类型规范](../specifications/feng-type.md)，本文不另行定义类型兼容或转换语义。

本组的普通类型范围包括固定宽度标量、`bool`、`string`、非泛型普通 `type`、非泛型 `@value type`
以及 `@abi type` 在 Feng 语言内部的普通对象类型关系。`@abi` 的 C ABI 资格、传递方式、指针与注解
约束不属于 G18；只验证 `@abi` 不改变 Feng 内部的名义类型匹配规则。

本组不重复以下专属领域：函数参数和返回目标归 G15；构造阶段顺序、`let` 三阶段绑定、隐式默认构造
和构造可见性归 G05；一元、二元、索引和复合赋值运算符本身归 G14；enum、tuple、数组、模块可见性、
spec / fit、union / intersection 和泛型分别归 G19～G25。`@value type` 的布局循环、callable 值、
借用指针和 ABI 边界只建立既有专项映射，不在 G18 重复排列。

TYPE08 只为验证默认零值使用点边界，取样一个最小泛型 union-form；该取样仅检查“首成员是否需要
展开默认零值”，不扩展到 union 进入、收窄、布局或一般泛型语义，这些仍分别归 G24、G25。

不同声明位置或类型类别只有在经过不同的 Semantic / Codegen 路径时才分别取样；不得机械生成
“全部位置 × 全部类型”的笛卡尔积。非法程序必须锁定唯一诊断的错误码、触发 token、行列、来源文件
和编译阶段；能够形成可观察结果的合法邻界必须进入 FCTS 直接运行。

### 22.2 用例 TODO

#### 22.2.1 绑定初始化与普通赋值的目标类型

- [x] TYPE01：对显式声明类型的局部绑定、模块级绑定、实例字段声明初始化器和 static 绑定初始化器
  分别提供一个最小不兼容值；取样必须覆盖已具备静态类型的不同固定宽度数值、`bool` / `string`、
  两个不同普通对象类型、两个不同 `@value type` 和两个不同 `@abi type`，并在实际初始化表达式
  起始 token 产生唯一 `AE1003`。`let` / `var` 只按经过的不同初始化路径取样，不重复可写性矩阵；
- [x] TYPE02：对局部 `var`、模块级 `var`、实例 `var` 字段和 static `var` 分别赋入不兼容右值，在
  右值起始 token 产生唯一 `AE1003`。至少覆盖已有静态类型的跨数值类型赋值和不同名义对象类型赋值；
  `let` 目标、非位置左值、数组元素及复合赋值分别映射 G13、G14 和 G21，不混入本项；
- [x] TYPE03：`void` 作为局部绑定、模块级绑定、普通实例字段或 static 绑定的声明类型时，均在
  `void` token 产生唯一 `AE0502`；函数 / Lambda 参数位置映射 G15，数组元素位置留给 G21，函数
  返回类型 `void` 继续合法。无初始化器但类型具有合法默认零值的对应普通绑定必须继续通过。

#### 22.2.2 显式类型转换

- [x] TYPE04：建立普通显式转换的反向矩阵，覆盖 `bool` 与数值、`string` 与数值 / `bool`、数值与
  普通对象、两个不同普通对象类型、两个不同 `@value type` 以及两个不同 `@abi type` 之间不受支持的
  转换；已有直接证据只建立映射，缺少的名义类型边界新增最小程序。所有非法转换统一在 cast 表达式
  起始 token 产生一个新的稳定表达式诊断 `AE1023`，不得继续产生未进入现行 AE 分段规范的
  `AE0051`，见 `ISSUE-G18-001`；
- [x] TYPE04-A：同一普通类型的显式转换和所有数值类型之间的合法显式转换继续通过；FCTS 至少直接
  观察一次整数缩窄截断、整数到浮点、浮点到整数和同一普通对象 / `@value type` 转换后的值。数组
  可写层转换、enum 底层值转换、tuple 形状转换、spec 视角转换、callable 转换和泛型目标分别只映射
  G19～G25 或既有专项，不在 G18 重复。

#### 22.2.3 对象构造参数与对象字面量字段

- [x] TYPE05：非泛型普通 `type` 已显式声明构造集合时，分别以 `Type(wrong)` 和
  `Type(wrong) {}` 传入参数数量正确但静态类型不匹配的值，必须在构造目标 token 产生唯一
  `AE0315`，不得延迟到 Codegen。无显式构造时传入非零参数的 `AE0313`、构造重载二义性的
  `AE0314`、无参 / 有参构造可用性和 `seal` 构造的 `AE0315` 均直接映射 G05；本组不修改这些
  已交付诊断契约；
- [x] TYPE05-A：构造参数为固定宽度数值类型时，数值字面量和纯字面量数值常量表达式必须按参数目标
  类型贴合；已具备另一数值静态类型的绑定必须先显式转换。合法直接构造和带对象字面量后缀的构造
  均进入 FCTS 并断言选中构造及最终字段值；
- [x] TYPE06：分别通过 `Type { field: wrong }`、`Type() { field: wrong }` 和
  `Type(args) { field: wrong }` 向普通实例字段写入不兼容值，在字段值表达式起始 token 产生唯一
  `AE1003`；至少覆盖跨固定宽度数值类型、不同普通对象类型和不同 `@value type`。字段不存在、字段
  重复直接映射既有 Semantic 专项，字段不可见映射 G22，`let` 三阶段重复绑定映射 G05；
- [x] TYPE06-A：对象字面量字段的数值字面量贴合、同名义对象引用、`@value type` 值和 `@abi type`
  Feng 内部值必须合法；FCTS 直接观察三种对象字面量入口写入后的最终值。

#### 22.2.4 实例与 static 成员访问面

- [x] TYPE07：普通对象实例读取 static 绑定、写入 static `var` 或调用 static 方法时，均在成员访问
  token 产生唯一 `AE0310`；不得因为外层是赋值或调用而追加派生诊断；
- [x] TYPE07-A：通过类型名读取实例字段或调用实例方法时，按类型的 static 成员面查找失败，在成员
  访问 token 产生唯一 `AE0309`。这一路径保持现有“类型没有该 static 成员”诊断，不为同一拒绝行为
  新增专用特判错误码；
- [x] TYPE07-B：同一 `type` 的实例面和 static 面允许声明同名成员；FCTS 分别通过对象实例与类型名
  访问并断言各自结果，证明两条访问路径不会串面。成员不存在映射 G14，`seal` / 跨模块可见性映射
  G22，spec、fit 和泛型约束上的 static 成员访问映射 G23 / G25。

#### 22.2.5 普通对象默认零值资格

- [x] TYPE08：只声明直接自引用或两个普通对象类型互相引用的类型仍可通过 Semantic；当局部、模块级
  或 static `let` / `var` 绑定无初始化器而实际要求这类对象的递归默认零值时，必须在该绑定 token
  产生唯一 Semantic 诊断 `AE0332`。同类绑定有初始化器时直接以初始化值完成初始化，不先产生默认
  零值；对 `spec Recursive<T>: T | Empty` 这类首成员可形成递归的 union-form 也遵循同一规则：
  无初始化器时沿首成员检查默认零值，显式初始化为非递归 member 时不得报错。Codegen 的残余防御
  只能产生 `IE0002`，不得把用户可修复错误继续报告为 `CE`，见 `ISSUE-G18-002`；
- [x] TYPE08-A：构造普通对象时，若某个实例字段没有声明初始化器，第一阶段需要递归生成不可能终止的
  普通对象默认零值，必须在构造目标 token 产生唯一 Semantic 诊断 `AE0332`。选中构造函数或对象
  字面量字段在后续阶段提供值都不能倒推取消第一阶段默认零值；只有该字段自身的声明初始化器直接
  提供初值时才不请求默认零值；这也包括字段类型为首成员可递归的 union-form、声明初始化器选取其
  非递归 member 的情况。本项只验证默认零值资格，不重复 G05 的三阶段 `let` 最终绑定规则，见
  `ISSUE-G18-003`。同包源码与跨包 `.ft` consumer 必须得到相同结论；`.ft` 必须同时保留实例 `let` /
  `var` 的声明初始化器事实，见 `ISSUE-G18-006`；
- [x] TYPE08-B：无环嵌套普通对象必须得到递归零值；`string` 和数组的默认值分别是空字符串和空数组，
  因而数组字段必须终止默认零值展开。仅通过数组形成运行时潜在引用环的普通类型仍允许默认初始化，
  并在 FCTS 中直接观察空数组及其他零值字段；默认零值资格不得直接复用包含数组边的 ARC 潜在循环
  标记，见 `ISSUE-G18-002`；
- [x] TYPE08-C：`@value type` / tuple 的内联布局循环继续使用既有声明期诊断，callable-form spec、
  enum 和数组自身的默认值分别映射既有专项、G19～G21；G18 不新增这些类别的同构反向用例。

#### 22.2.6 合法邻界与覆盖映射

- [x] TYPE09：新增或映射一个 FCTS 普通类型合法邻界文件，直接运行固定宽度数值字面量贴合、显式
  数值转换、同类型标量 / 字符串赋值、普通对象引用赋值、`@value type` 值赋值、`@abi type` 的
  Feng 内部同类型赋值、匹配构造参数、对象字面量字段初始化、实例 / static 双访问面和合法默认零值；
- [x] TYPE09-A：建立 TYPE01～TYPE09 到现有 Semantic、Codegen 和 FCTS 的逐项映射。已有测试只有
  “分析失败”或消息片段而没有稳定码、精确 token、行列、数量和阶段时，不视为完整反向证据；优先
  新增集中式 G18 精确矩阵，不为统一断言风格修改无关既有测试。

### 22.3 独立验收与交付 TODO

- [x] 先记录并关闭 [G18 问题记录](./feng-language-conformance-coverage-hardening-issues/g18.md) 中的
  `ISSUE-G18-001`～`ISSUE-G18-006`：将通用非法 cast 的唯一产生点从 `AE0051` 迁移为
  `AE1023`；将普通对象默认零值循环从 Codegen 的 `CE0226` 前移为 Semantic `AE0332`，并使默认
  零值依赖判断在数组、字符串、标量和指针处终止；Codegen 只保留 `IE0002` 内部不变量防御；
- [x] `AE0051` 迁移会影响四个既有 Semantic 测试函数中的 11 条错误码断言：
  `test_object_spec_method_values_reject_invalid_sources`、
  `test_explicit_generic_callable_values_reject_invalid_sources`、
  `test_callable_variadic_shape_mismatches_are_rejected` 和
  `test_object_spec_upcast_rejects_out_of_scope_conversions`。实施前必须取得修改这四个函数的明确人工
  批准；现已批准只把这些断言由 `AE0051` 更新为 `AE1023`，不得借机改写输入、其他断言或测试结构。
  除此名单外，既有测试如需修改必须再次取得人工决策；
- [x] 对 TYPE01～TYPE08-A 的反向程序独立运行 G18 Semantic 专项，逐项核对唯一诊断、稳定码、触发
  token、行列、来源文件和 Semantic 阶段；对默认零值 Codegen 防御独立运行结构专项，核对其仅为
  `IE0002`，并验证数组终止邻界和没有派生错误；
- [x] 运行完整 FCTS，确认 TYPE04-A、TYPE05-A、TYPE06-A、TYPE07-B、TYPE08-B 和 TYPE09 的合法
  邻界全部由主入口登记并真实执行，不能以 Semantic 接受或生成 C 文本代替运行断言；
- [x] 所有实现变更只允许增加或调整编译期诊断、类型检查和默认零值资格分析；不得给既有合法程序
  增加运行时判断、分支、调用、分配或 ARC，不得修改 runtime 私有 ABI、公开 ABI 或 `.ft` 格式；
- [x] 在 Codex 沙箱外为 G18 独立执行 `make test`，记录 UBSan 与 normal 两阶段的 smoke、std、FCTS、
  性能约束及其余回归结果；此前分组的全量结果不能代替本组回归；
- [x] 执行 `git diff --check`，关闭或取得人工决策保留 G18 问题；填写稳定码映射、既有证据、新增
  用例、专项结果、全量结果、性能与兼容性结论以及建议的英文 commit message 后，才可标记 G18
  已交付。

### 22.4 独立交付记录

- 状态：已交付
- 稳定码映射与新增用例：TYPE01～TYPE09-A 已逐项映射；新增集中式 Semantic 精确诊断矩阵，覆盖
  `AE1003`、`AE0502`、`AE1023`、`AE0315`、`AE0310`、`AE0309` 和 `AE0332` 的唯一诊断、token、
  行列、来源与阶段；仅按批准范围把四个既有 Semantic 测试函数中的 11 条 `AE0051` 断言迁移为
  `AE1023`。新增 Codegen 结构用例，覆盖数组终止默认零值展开、开放泛型默认工厂资格、`IE0002`
  防御和同类型 cast 无操作发码；新增 `.ft` 真实往返用例，覆盖实例 `var` 声明初始化器事实且保持
  后续可写；新增 `test_ordinary_type_semantics.ff`，运行普通类型匹配、转换、构造、字面量、双成员面、
  递归零值以及“首成员递归 union 显式选择非递归 member”的合法边界；
- 本组专项结果：`test_semantic`、`test_codegen`、`test_symbol` 均通过；Smoke 为 `91/91`，标准库为
  `604/604`，FCTS 为 `1139/1139`；
- 本组沙箱外 `make test`：2026-09-03 执行，退出码为 `0`；UBSan 与 normal 两阶段的 Smoke 均为
  `91/91`、标准库均为 `604/604`、FCTS 均为 `1139/1139`；Parser、Semantic、Codegen、runtime、
  CLI、symbol、增量构建、发布脚本、性能约束及其余回归全部通过；
- 性能与兼容性：默认零值有限性、类型检查、导出事实和工厂资格判断均只发生在编译期；同类型 cast
  不发射运行时操作；没有给合法程序增加运行时判断、分支、调用、分配或 ARC，性能约束通过。未修改
  Parser 行为、runtime、公开 ABI、runtime 私有 ABI 或 `.ft` 布局 / 版本；
- 问题：`ISSUE-G18-001`～`ISSUE-G18-006` 均已关闭，具体分析与处理见
  [G18 问题记录](./feng-language-conformance-coverage-hardening-issues/g18.md)；
- 建议 commit message：`test: harden ordinary type semantics and default-zero diagnostics`

## 23 G19：enum 诊断

### 23.1 测试重点

验证 enum 声明语法、成员值归一化、名义类型关系、成员访问和显式转换产生的稳定 Parser / Semantic
诊断，并以 Codegen、真实 package-public `.ft` 往返和 FCTS 运行结果确认 enum 的底层表示固定为
`i32`。enum 宽度不得随平台 `int` 别名变化；显式成员值必须位于 `i32` 可表示范围内。

G09 已交付的 enum 默认值语义，以及既有 `match`、`fit`、`spec` 和 ABI 专项行为只建立覆盖映射，
不在 G19 重复扩展；G19 只补 enum 自身的声明、值、类型和诊断边界。

### 23.2 用例 TODO

#### 23.2.1 Parser 语法与阶段边界

- [x] ENUM-D01：全隐式 enum 与全显式 enum 必须解析成功；全显式形式覆盖正数、负数、零，以及十进制、
  十六进制、八进制、二进制和带分隔符的合法整数字面量。Parser 只确认初始化器是单一整数字面量，
  不在本阶段执行 `i32` 语义值域判断；
- [x] ENUM-D02：完整映射 enum 专属 Parser 诊断：`extern enum` 为 `SE0401`；enum 声明注解和 enum item
  注解分别为 `SE0402`；泛型参数、父 `spec`、callable 签名、空声明体、非法成员、非整数字面量或
  字面量表达式、尾随逗号、缺失左 / 右花括号依次覆盖 `SE0403`～`SE0410`。缺少 enum 名或 item 名
  继续映射通用 `SE0002`；超过 Lexer `u64` 幅值上限的字面量继续映射 `LE0002`。G12 已有且满足
  稳定码、token、位置、数量和阶段要求的用例直接复用，只新增 `SE0401`、两种 `SE0402`、缺失左
  花括号及其他实际未覆盖出口，不复制等价用例；

#### 23.2.2 声明一致性与 `i32` 值归一化

- [x] ENUM-D03：显式成员后出现隐式成员、隐式成员后出现显式成员，均必须在首个破坏一致性的 item
  名称 token 产生唯一 `AE0401`；
- [x] ENUM-D04：同一 enum 中名称重复时，使用不同显式值隔离数值重复因素，并在后一个同名 item 的
  名称 token 产生唯一 `AE0402`；
- [x] ENUM-D05：名称不同但归一化数值相同的全显式成员必须在后一个 item 名称 token 产生唯一
  `AE0403`；至少覆盖不同字面量写法表示同一值，例如十进制 `1` 与十六进制 `0x1`，证明按数值而非
  源码文本判断重复；
- [x] ENUM-D06：`-2147483648` 与 `2147483647` 必须合法；`-2147483649`、`2147483648` 以及 Lexer
  可承载但超过 `INT64_MAX` 的正、负幅值必须在 Semantic 阶段产生唯一 `AE0405`。诊断位置指向初始化
  值的起始 token：正数指向整数 token，负数指向一元 `-`；错误消息保留原始字面量文本。Parser / AST
  必须保留足以表达原始符号、`uint64_t` 幅值和完整源码范围的事实，Semantic 不得只依据已经重解释为
  `int64_t` 的值判断范围，也不得发生编译器自身的有符号取负溢出。值域检查必须先于重复值检查、
  Codegen 截断和 `.ft` 导出；合法值通过后才归一化为底层 `i32` 值。全隐式成员序号使用同一归一化
  值域不变量，但不构造需要数十亿成员的不可执行测试；

#### 23.2.3 成员访问、名义类型与运算边界

- [x] ENUM-D07：本包和真实 package-public `.ft` consumer 均覆盖合法的 `Enum.Item` 访问；不存在的
  item 必须在 item token 产生唯一 `AE0404`。裸写 item 名不得向值命名空间泄漏，继续按通用未定义
  标识符产生 `AE0001`；
- [x] ENUM-D08：同一 enum 的绑定、参数、返回值和 `==` / `!=` 必须合法；不同 enum 之间赋值、enum
  到整数及整数到 enum 的隐式赋值必须在不匹配表达式产生唯一 `AE1003`，不得因底层值相同而放宽；
- [x] ENUM-D09：enum 显式转换到 `i8`、`i16`、`i32`、`i64`、`u8`、`u16`、`u32`、`u64`、`int`、
  `uint`、`byte` 均必须合法，并用负值、窄化值和宽化值核对目标整数类型的既有转换结果；`int` / `uint`
  必须分别使用 32 位和 64 位 `pointer_size` 分析选项验证。同类型 enum 显式转换保持合法且不发码；
  上述每一种整数类型到 enum、不同 enum 之间的显式转换，以及 enum 到 `f32`、`f64`、`float`、
  `double`、`bool`、`string` 的显式转换，必须在 cast 起始 token 产生唯一 `AE1023`；
- [x] ENUM-D10：同一 enum 的 `==` / `!=` 保持合法；不同 enum、enum 与整数的相等比较，以及 enum 上的
  顺序、算术、位运算、移位、逻辑、一元和复合赋值运算，分别映射既有 `AE1018`、`AE1019`、`AE1020`
  通用诊断。以各条通用验证路径的最小运算符矩阵覆盖全部 enum 非数值边界，不新增 enum 专用错误码；

#### 23.2.4 固定表示与跨包恢复

- [x] ENUM-D11：公开 enum 以 `INT32_MIN`、`0`、`INT32_MAX` 为成员值完成真实 package-public `.ft`
  写入、读取和 imported-module 恢复；consumer 观察到的声明顺序、成员名和有符号值必须完全一致。
  非法源 enum 必须先由 Semantic 产生 `AE0405`，不得以 `.ft` 导出错误作为用户诊断；既有 `.ft`
  `int32_t` 防御保留，但不改变格式或版本；
- [x] ENUM-D12：本包和导入 enum 的 Codegen 均断言类型固定生成为 `int32_t`，`INT32_MIN` / `INT32_MAX`
  常量不截断、不改号；分别用 32 位与 64 位 `pointer_size` 验证 enum 表示不跟随 `int` 改变。合法 enum
  路径继续复用既有标量 cast，不增加运行时 helper、分支、调用、分配或 ARC；

#### 23.2.5 FCTS 正向行为与覆盖映射

- [x] ENUM-D13：新增独立 G19 FCTS 文件，真实运行全隐式顺序值、全显式正 / 负 / 零值、`i32` 最小值与
  最大值、全部整数目标显式转换的结果，以及本包与跨包公开 enum 的边界值恢复。G09 默认值和既有 enum
  `match` 行为只映射已有 FCTS，不在本文件复制；
- [x] ENUM-D14：建立 ENUM-D01～ENUM-D13 到既有 Lexer、Parser、Semantic、Symbol、Codegen、CLI 和
  FCTS 证据的逐项映射。已有测试只有“失败”或消息片段、没有稳定码、精确 token、行列、数量和阶段时，
  不视为完整反向证据；优先新增集中式 G19 精确矩阵，不为统一断言风格修改无关既有测试。

### 23.3 独立验收与交付 TODO

- [x] 先更新并关闭 [G19 问题记录](./feng-language-conformance-coverage-hardening-issues/g19.md)：补齐
  `AE0405` 的原始幅值保存、Semantic 值域检查和诊断位置，完成 enum 到全部整数类型的转换覆盖；
- [x] 独立运行 G19 Lexer / Parser / Semantic 专项，核对每个反向程序的唯一诊断、稳定码、触发 token、
  行列、来源文件、阶段和 enum 上下文；
- [x] 独立运行 Symbol / Codegen / CLI 专项，验证 package-public `.ft` 边界值往返、固定 `int32_t`
  表示、32 / 64 位别名环境，以及非法公开 enum 在 Semantic 阶段终止；
- [x] 运行完整 FCTS，确认 ENUM-D13 已由主入口登记并真实执行，不能以 Semantic 接受、生成 C 文本或
  `.ft` 读取成功代替运行断言；
- [x] G19 的实现变更只允许调整编译期字面量事实保存、enum 值归一化和诊断；不得给合法程序增加运行时
  判断、分支、调用、分配或 ARC，不得修改 runtime、公开 ABI、runtime 私有 ABI 或 `.ft` 格式 / 版本；
- [x] 实施中发现 `extern enum` 的 `SE0401` 专用出口被通用 `SE0003` 提前拦截；已先记录为
  `ISSUE-G19-003` 并取得人工批准，只迁移既有通用负向矩阵中的这一条 enum 输入，其余既有测试不变；
- [x] 在 Codex 沙箱外为 G19 独立执行 `make test`；
- [x] 执行 `git diff --check`，关闭或取得人工决策保留 G19 问题；填写稳定码映射、既有证据、新增用例、
  专项结果、全量结果、性能与兼容性结论及建议的英文 commit message 后，才可标记 G19 已交付。

### 23.4 独立交付记录

- 状态：已交付
- 稳定码映射与新增用例：ENUM-D01～ENUM-D14 已逐项映射；Parser 精确矩阵覆盖 `LE0002`、`SE0002`
  及 `SE0401`～`SE0410`，并新增源字面量原始正负号、`uint64_t` 幅值与源码范围 AST 断言；仅按人工
  批准把既有通用负向矩阵中的 `extern enum` 输入迁移到精确 `SE0401` 断言。Semantic 精确矩阵覆盖
  `AE0001`、`AE0401`～`AE0405`、`AE1003`、`AE1018`～`AE1020` 和 `AE1023`，包括固定 `i32`
  正负邻界、Lexer 可承载的大幅值、声明一致性、成员访问、名义类型、所有整数转换方向及全部运算符
  类别；新增 Symbol 真实 package-public `.ft` 往返、Codegen 32 / 64 位固定 `int32_t` 和同 enum 无操作
  cast、CLI Semantic 阶段诊断用例。新增并登记 `test_enum_diagnostics_semantics.ff` 及跨包 provider，
  真实运行全隐式 / 全显式值、边界值、全部整数目标转换和跨包恢复；
- 本组专项结果：`test_lexer`、`test_parser`、`test_semantic`、`test_symbol`、`test_codegen`、`test_cli`
  均通过；Smoke 为 `91/91`，标准库为 `604/604`，FCTS 为 `1146/1146`；
- 本组沙箱外 `make test`：2026-09-06 执行，退出码为 `0`；UBSan 与 normal 两阶段的 Smoke 均为
  `91/91`、标准库均为 `604/604`、FCTS 均为 `1146/1146`；Parser、Semantic、Codegen、runtime、
  CLI、symbol、增量构建、发布脚本、性能约束及其余回归全部通过；
- 性能与兼容性：所有新增检查和原始字面量事实仅存在于编译期；合法 enum 继续使用既有固定
  `int32_t` 表示和标量 cast，同 enum cast 不发射运行时操作。没有新增运行时 helper、判断、分支、
  调用、分配或 ARC；性能约束通过。未修改 runtime、公开 ABI、runtime 私有 ABI 或 `.ft` 布局 / 版本；
- 问题：`ISSUE-G19-001`～`ISSUE-G19-003` 均已关闭，具体分析与处理见
  [G19 问题记录](./feng-language-conformance-coverage-hardening-issues/g19.md)；
- 建议 commit message：`fix(enum): enforce fixed i32 values and harden diagnostics`

## 24 G20：tuple 诊断

### 24.1 测试重点

以 [tuple 规范](../specifications/feng-tuple.md)、[表达式规范](../specifications/feng-expression.md)、
[类型规范](../specifications/feng-type.md)、[SE 错误码规范](../specifications/feng-error-codes-se.md)和
[AE 错误码规范](../specifications/feng-error-codes-ae.md)为唯一语义依据，验证具名 tuple 声明、tuple
字面量目标贴合、对象构造边界、默认零值、名义类型关系、元素访问及解构的稳定 Parser / Semantic
诊断，并通过 Codegen 结构断言和 FCTS 运行结果保护合法行为。

本组特别锁定两条已经人工确认的边界：

- 圆括号形式声明的具名 tuple 不是对象类型，不声明构造函数，也不获得隐式公开无参构造函数；
  `TupleType()`、`TupleType(args)` 及其对象字面量后缀，以及直接 `TupleType { ... }` 均必须在
  Semantic 阶段拒绝，不能进入 Codegen；无初始化器的具名 tuple 绑定仍按元素默认零值直接初始化；
- `let (x, y): Pair = value;` 不合法，由 Parser 在 `Pair` token 产生唯一 `SE0107`；解构各非空位置
  的类型分别从右侧对应位置推断，不为同一用户输入再定义可达的 `AE0110`。

G10 已交付的解构求值顺序、G12 已交付的通用语法矩阵、G13 的绑定名称与可写性、G14 的通用成员
访问、G16 的 `for/in` 解构，以及 tuple 的 `fit` / `spec` 装箱和泛型组合，只建立逐项覆盖映射；已有
证据满足诊断码、token、行列、数量、阶段或真实运行要求时不重复造例。值类型布局循环、ABI 与 `.ft`
格式不属于 G20 的新增语义范围。

### 24.2 用例 TODO

#### 24.2.1 声明、字面量与 Parser 阶段边界

- [x] TUP-D01：具名 tuple 声明的 0、2、8 个元素邻界必须解析成功，1 个与 9 个元素分别产生唯一
  `SE0308`；1 元素诊断定位 tuple 类型名称 token，超过上限的诊断定位第 9 个元素类型的起始 token。
  同时确认 `(expr)` 仍是分组表达式、`()` 与 `(left, right)` 才是 tuple 字面量；
- [x] TUP-D02：补齐 tuple 字面量的 Parser 反向邻界：第 9 个元素产生唯一 `SE0309`，逗号后直接
  `)` 产生唯一 `SE0310`，缺失右括号产生唯一 `SE0312`。既有用例若只断言消息片段，不视为完整
  诊断证据；
- [x] TUP-D03：建立解构模式语法的完整映射：单位置、嵌套位置、非标识符位置、模式后整体类型标注、
  缺少初始化器、非法声明上下文和括号 / 分隔符缺失依次锁定 `SE0104`～`SE0110`。其中
  `let (x, y): Pair = value;` 必须在 `Pair` token 产生唯一 `SE0107` 并停在 Parser；G12 的精确证据
  直接复用，缺失的错误出口才在 G20 新增，Semantic 不为该输入补造 `AE0110` 用例；

#### 24.2.2 tuple 字面量的目标类型与形状

- [x] TUP-D04：`let value = ();` 与 `let value = (1, 2);` 等没有具名 tuple 目标、又不处于直接解构
  上下文的字面量，均在字面量左括号产生唯一 `AE0301`；目标为标量、普通对象或其他非 tuple 类型时
  也产生 `AE0301`，不得推导匿名 tuple 类型或继续产生派生错误；
- [x] TUP-D05：分别以 0、2、8 元素具名 tuple 为目标覆盖完全匹配；以目标数量不足和过多的最小程序
  覆盖 `AE0302`，诊断必须位于字面量左括号，并同时报告实际元素数、目标类型名和期望元素数。泛型
  tuple 在完成类型实参替换后使用相同计数规则；
- [x] TUP-D06：对元素数量正确但某一位置类型不兼容的 tuple 字面量，绑定、函数返回、对象字段和
  显式 cast 目标必须在首个不兼容元素表达式起始 token 产生唯一 `AE1003`，且消息中的期望类型是
  泛型替换后的具体位置类型。普通函数调用先以完整实参形状进行重载决议；不匹配的 tuple 字面量使
  候选被排除，并在被调用函数 token 产生唯一 `AE0512`，不绕过 G15 已确定的调用诊断规则，见
  `ISSUE-G20-003`。五种规范目标上下文均须具有一条实际经过对应 Semantic 路径的反向证据；
- [x] TUP-D07：为上述五种目标上下文提供合法邻界，覆盖 0 元素、普通 2 元素、8 元素、泛型元素和
  托管元素；数值字面量按位置目标类型贴合，全部元素从左到右各求值一次。可观察结果进入 FCTS，
  已由 G10 证明的求值顺序只建立映射，不复制等价场景；

#### 24.2.3 非对象构造边界与默认零值

- [x] TUP-D08：同模块的 `Pair()`、`Pair(args)`、`Pair<T, U>()` 与 `Pair<T, U>(args)` 均必须在
  tuple 类型目标 token 产生唯一 `AE0312`；不允许以隐式默认构造、构造参数不匹配 `AE0313` 或
  构造可见性 `AE0315` 描述根因，见 `ISSUE-G20-001`；
- [x] TUP-D09：同模块的 `Pair {}`、`Pair { item1: value, ... }` 必须在对象字面量左花括号产生唯一
  `AE1004`；`Pair() { ... }` 与 `Pair(args) { ... }` 必须先在内部构造调用的 tuple 类型目标 token
  产生唯一 `AE0312`。空字段、完整 `itemN` 字段和缺失 / 重复字段均不得绕过“tuple 不是对象字面量
  目标”这一根因，也不得降级为字段级诊断；
- [x] TUP-D10：通过短名、`import ... as alias` 别名和完整模块路径引用公开 tuple 时，TUP-D08～
  TUP-D09 的拒绝阶段、稳定码和根因必须与同模块一致；同时映射既有 Symbol 证据，确认导入后仍保留
  tuple 类型分类。若实施需要修改 `.ft` 格式 / 版本或公开 ABI，必须暂停并再次取得人工决策；
- [x] TUP-D11：`let value: Pair;` 与 `let unit: Unit;` 必须合法，分别观察每个元素的默认零值和 0 元素
  tuple；再覆盖一个以 tuple 为字段的合法默认初始化。该入口属于绑定 / 字段默认初始化，不得调用
  构造函数、分配普通对象或先生成一个可观察的临时对象；
- [x] TUP-D12：增加 Codegen 结构断言，证明合法 tuple 字面量与无初始化器默认零值继续使用值布局，
  不出现普通对象类型描述符或 `feng_object_new`；同时通过 CLI 最小反向程序证明 TUP-D08～TUP-D10
  在 Semantic 失败后没有生成 C，更不能把错误延迟为宿主 C 编译错误；

#### 24.2.4 名义类型关系与显式转换

- [x] TUP-D13：同一具名 tuple 的绑定、赋值、参数和返回值流转必须合法；两个名称不同但元素形状
  完全相同的具名 tuple 仍不得隐式转换，在实际不匹配的源表达式 token 产生唯一 `AE1003`。0 元素
  tuple 和普通多元素 tuple 各覆盖一个名义边界；
- [x] TUP-D14：不同名称但元素数量及各位置类型完全一致的具名 tuple 允许显式转换，并在 FCTS 观察
  转换后的全部元素；元素数量不同或任一位置类型不同的显式转换在 cast 左括号产生唯一 `AE1023`。
  `(Pair)(left, right)` 属于字面量按显式目标贴合：其数量或元素错误仍分别归 `AE0302` / `AE1003`，
  不得误报为具名 tuple 间的 `AE1023`；
- [x] TUP-D15：泛型 tuple 的同一闭合实例正常流转；类型实参导致任一展开位置类型不同时，隐式流转
  与显式转换分别产生 `AE1003` 和 `AE1023`。裸泛型名称、类型实参数量和泛型推断只映射 G25，
  不在本项重复通用泛型矩阵；
- [x] TUP-D16：普通具名 tuple 和已闭合泛型 tuple 用作泛型上界时，均在类型参数名称 token 产生唯一
  `AE0304`；通过 `spec` + `fit` 表达共同能力的合法邻界只映射既有 tuple / spec 测试，不为 tuple
  增加结构化上界特例；

#### 24.2.5 元素访问与不可变性

- [x] TUP-D17：0 元素 tuple 不暴露任何 `itemN`；2 元素、8 元素和泛型 tuple 分别覆盖首个、末个及
  泛型替换后元素的合法读取，并在 FCTS 断言值和静态类型行为；
- [x] TUP-D18：实例访问 `item0`、超过实际数量的 `itemN` 或任意不存在名称时，在成员名称 token
  产生唯一 `AE0306`；通过类型名写 `Pair.item1` 时按 static 成员面查找，在 `item1` token 产生唯一
  `AE0309`。普通成员不存在矩阵只映射 G14；
- [x] TUP-D19：即使 tuple 存放在 `var` 绑定中，对 `value.itemN` 赋值也必须在成员目标 token 产生唯一
  `AE0104`；`var` 整体替换 tuple 继续合法并进入 FCTS，`let` 整体重绑定的 `AE0104` 只映射 G13。
  不得把绑定可变性错误传播为元素可变性；

#### 24.2.6 解构的语义诊断与合法邻界

- [x] TUP-D20：解构右侧为标量、普通对象、数组或其他非 tuple 静态类型时，在解构模式左括号产生
  唯一 `AE0108`；右侧为具名 tuple 与直接 tuple 字面量时，模式位置数不足或过多均在同一位置产生
  唯一 `AE0109`，并分别报告模式位置数与来源位置数；
- [x] TUP-D21：具名 tuple 表达式必须整体求值一次后再建立所有非空位置绑定；直接 tuple 字面量只按
  从左到右顺序求值非空位置，空位表达式不求值。覆盖无空位、首 / 中 / 尾空位、多空位、全部空位、
  0 元素，以及 `let` / `var` 独立绑定；直接映射 G10 已有 FCTS 和 Codegen 证据，只有缺口才新增；
- [x] TUP-D22：模式中的每个非空名称形成独立绑定；同一模式重名或与同作用域既有绑定重名均映射
  G13 的 `AE0105` 精确证据。向 `let` 解构名称赋值产生 `AE0104`，由 G20 增加精确直接证据；`var`
  解构名称可独立赋值。tuple 元素自身的不可写 `AE0104` 已由 TUP-D19 覆盖。空位不产生名称，Feng
  没有 `_` 丢弃符，不为 tuple 添加特殊绑定；
- [x] TUP-D23：`for let/var (left, right) in values` 的 Parser 形状、元素类型、位置数、空位、逐轮绑定
  身份与捕获语义分别映射 G07、G12、G13 和 G16；G20 只确认其复用普通 tuple 解构的 `SE01xx`、
  `AE0108` / `AE0109` 契约，不新增另一套循环专用 tuple 诊断；
- [x] TUP-D24：增加合法的“先约束、后解构”邻界 `let value: Pair = source; let (x, y) = value;`，并
  与 Parser 拒绝 `let (x, y): Pair = source;` 成对覆盖，明确合法写法不会产生匿名 tuple 类型；

#### 24.2.7 FCTS 行为与覆盖映射

- [x] TUP-D25：新增或映射一个独立 G20 FCTS 邻界文件，真实运行默认零值、五种字面量目标上下文、
  同名义流转、合法显式转换、首末元素读取、`var` 整体替换和合法解构。Semantic 接受、生成 C 文本
  或既有测试只检查“不崩溃”均不能代替可观察值断言；
- [x] TUP-D26：建立 TUP-D01～TUP-D25 到既有 Parser、Semantic、Symbol、Codegen、CLI 和 FCTS
  证据的逐项映射。已有反向测试只有“失败”或消息片段，没有稳定码、精确 token、行列、数量和阶段时，
  不视为完整证据；优先新增集中式 G20 精确矩阵，不为统一断言风格修改无关既有测试。

### 24.3 独立验收与交付 TODO

- [x] 先按 [G20 问题记录](./feng-language-conformance-coverage-hardening-issues/g20.md) 实施并关闭
  `ISSUE-G20-001`：在 Semantic 统一拒绝 tuple 的普通构造与对象字面量入口；确认
  `ISSUE-G20-002` 已把模式整体类型标注收敛为 Parser `SE0107`，不新增可达的 Semantic 重复诊断；
- [x] 独立运行 G20 Parser 专项，核对 TUP-D01～TUP-D03 的唯一诊断、稳定码、token、行列、来源文件
  和 Parser 阶段；G12 已有完整证据可直接映射，缺失项集中新增；
- [x] 独立运行 G20 Semantic 专项，至少覆盖 `AE0301`、`AE0302`、`AE0312`、`AE1004`、`AE1003`、
  `AE0512`、`AE1023`、`AE0304`、`AE0306`、`AE0309`、`AE0104`、`AE0108` 与 `AE0109` 的唯一诊断、
  精确位置、来源文件和 tuple 上下文；非法 tuple 对象构造不得到达 Codegen；
- [x] 独立运行 Symbol、Codegen 和 CLI 专项，验证导入类型仍保留 tuple 分类、合法 tuple 使用值布局和
  默认零值、没有普通对象分配，以及非法构造在 Semantic 阶段终止。不得以宿主 C 编译失败作为反向
  用例的通过条件；
- [x] 运行完整 FCTS，确认 TUP-D07、TUP-D11、TUP-D13～TUP-D15、TUP-D17、TUP-D19、TUP-D21、
  TUP-D24～TUP-D25 的合法行为均由主入口登记并真实执行；
- [x] G20 默认只新增集中式测试和修复 `ISSUE-G20-001` 所需的通用 Semantic 类型分类检查；不得为
  tuple 添加运行时特判、helper、判断、分支、调用、分配或 ARC。除测试主入口登记外，如需修改既有
  测试输入或断言，必须列出精确名单并再次取得人工批准；
- [x] 不修改 runtime、runtime 私有 ABI、公开 ABI 或 `.ft` 格式 / 版本；若导入 tuple 分类无法仅凭
  现有符号事实保持一致，必须记录问题并暂停，由人工决策；
- [x] 在 Codex 沙箱外为 G20 独立执行 `make test`，记录 UBSan 与 normal 两阶段的 Smoke、标准库、
  FCTS、性能约束及其余回归结果；此前分组的全量结果不能代替 G20 回归；
- [x] 执行 `git diff --check`，关闭或取得人工决策保留 G20 问题；填写稳定码映射、既有证据、新增
  用例、专项结果、全量结果、性能与兼容性结论及建议的英文 commit message 后，才可标记 G20 已交付。

### 24.4 独立交付记录

#### 24.4.1 状态与实现范围

- 状态：已于 2026-09-06 独立交付，TUP-D01～TUP-D26 及本组验收项全部完成；
- 产品修复仅位于 Semantic：新增可复用的花括号对象形式 type 分类，统一阻止圆括号 tuple type 进入
  普通构造、对象字面量后缀、构造器解析和默认零值构造检查；直接 tuple 对象字面量仍由对象字面量
  入口拒绝；
- 未修改既有测试输入或断言；只新增 G20 用例并在既有测试主入口登记；
- 未修改 Codegen、runtime、runtime 私有 ABI、公开 ABI 或 `.ft` 格式 / 版本；合法 tuple 的发码路径
  不变，没有新增运行时判断、分支、调用、分配、ARC 或其他开销。

#### 24.4.2 TUP-D01～TUP-D26 证据映射

##### Parser：TUP-D01～TUP-D03

- 新增 `test_g20_tuple_declaration_and_literal_parser_diagnostics`，精确覆盖 `SE0308` 的 1 / 9 元素声明、
  `SE0309` 的第 9 个字面量元素和 `SE0312` 的缺失右括号；0 / 2 / 8 元素合法声明与字面量由 G20
  Semantic 正向聚合程序实际经过 Parser；
- 新增 `test_g20_destructure_separator_parser_diagnostic` 覆盖 `SE0110`；复用 G12
  `test_g12_binding_type_and_callable_syntax` 与 `test_g12_expression_and_postfix_syntax` 对
  `SE0104`～`SE0109`、`SE0310` 及合法分组 / tuple 形状的精确证据。模式整体类型标注唯一归
  Parser `SE0107`，没有新增可达的 `AE0110` 契约。

##### 目标贴合、构造与导入：TUP-D04～TUP-D12

- 新增 `test_g20_tuple_literal_target_and_arity_diagnostics` 与
  `test_g20_tuple_literal_element_type_diagnostics`，覆盖无目标、非 tuple 目标、普通 / 泛型数量不匹配，
  以及绑定、调用、返回、对象字段和显式 cast 五种目标上下文；稳定码为 `AE0301`、`AE0302`、
  `AE1003` 与普通调用重载失败的 `AE0512`；
- 新增 `test_g20_tuple_object_construction_diagnostics`，覆盖 0 / 非零参数、0 / 2 元素、泛型 tuple、
  直接对象字面量、调用后缀以及空 / 完整 / 缺失 / 重复字段，分别锁定唯一 `AE0312` / `AE1004`；
- 新增 `test_g20_imported_tuple_construction_diagnostics`，通过真实公开模块模型覆盖短名、alias 和完整
  模块路径；复用 Symbol 的 `test_private_representation_dependency_closure_roundtrip`，确认公开 `.ft`
  往返后仍保留 tuple 分类；
- 新增 `test_g20_tuple_value_layout_and_default_zero_codegen`，验证普通和 0 元素 tuple 的绑定 / 对象字段
  默认零值、值布局和无 tuple 对象分配；新增 CLI
  `test_project_check_rejects_tuple_construction_before_codegen`，验证 `AE0312` 后不生成 C。

##### 名义关系、成员与解构：TUP-D13～TUP-D24

- 新增 `test_g20_tuple_nominal_and_conversion_diagnostics`，覆盖普通、0 元素与泛型 tuple 的同名义流转、
  不同名义隐式拒绝、等形显式转换邻界、数量 / 位置类型错误和 tuple 泛型上界；稳定码为 `AE1003`、
  `AE1023`、`AE0302`、`AE0304`；
- 新增 `test_g20_tuple_access_and_destructure_diagnostics`，覆盖 0 / 2 / 8 元素和泛型成员邻界、实例 / static
  错误访问、元素不可写、非 tuple 解构及具名 / 字面量位置数不匹配；稳定码为 `AE0306`、`AE0309`、
  `AE0104`、`AE0108`、`AE0109`；
- 复用 G10 的 `test_tuple_destructuring_evaluation_codegen` 和
  `test_tuple_destructuring_evaluation.ff` 覆盖具名表达式只求值一次、直接字面量仅求值非空位置及其顺序；
  复用 G13 的 tuple 模式重名、同作用域重名和 `let` / `var` 可写性证据；
- 复用 Parser / Semantic 的 `for/in` tuple 解构正反向用例及 G07 / G16 FCTS，覆盖逐轮绑定、空位、
  数量和来源类型；新增 G20 FCTS 的“先约束普通绑定、后解构”正向用例，并与 G12 的 `SE0107` 反向
  用例成对验收。

##### FCTS 与总映射：TUP-D07、TUP-D11、TUP-D13～TUP-D15、TUP-D17、TUP-D19、TUP-D21、
TUP-D24～TUP-D26

- 新增并登记 `test_tuple_diagnostics_semantics.ff`，包含 5 条实际运行用例：五种 tuple 字面量目标上下文；
  0 / 2 / 8 元素、泛型与托管元素；绑定及对象字段默认零值；同名义流转、整体替换和等形显式转换；
  带类型普通绑定后的合法解构及 `let` / `var` 独立绑定；
- TUP-D01～TUP-D25 已分别映射到上述 Parser、Semantic、Symbol、Codegen、CLI、既有 G07 / G10 /
  G12 / G13 / G16 证据和新增 G20 FCTS；本节完成 TUP-D26 的总映射。

#### 24.4.3 专项与全量回归结果

- Parser、Semantic、Symbol、Codegen、CLI 专项全部通过；生成的 C 编译验证通过；
- 完整 FCTS 通过：`1151/1151`，相对 G19 增加 5 条 G20 行为用例；
- 沙箱外 `make test` 全量通过：UBSan 与 normal 两阶段的 Smoke `91/91`、标准库 `604/604`、FCTS
  `1151/1151`、性能约束、增量构建、发布脚本、macOS 收尾、bundled packages、toolchain fetch、
  全部单元测试与 CLI 回归均通过；
- `git diff --check` 通过。

#### 24.4.4 问题关闭情况

- [ISSUE-G20-001](./feng-language-conformance-coverage-hardening-issues/g20.md#issue-g20-001具名-tuple-被误当成对象构造目标并进入-codegen)：
  产品缺陷已修复，非法 tuple 构造在 Semantic 唯一报错且不再进入 Codegen；
- [ISSUE-G20-002](./feng-language-conformance-coverage-hardening-issues/g20.md#issue-g20-002解构模式整体类型标注同时登记为-parser-与-semantic-诊断)：
  已保持用户可达诊断唯一归 Parser `SE0107`；
- [ISSUE-G20-003](./feng-language-conformance-coverage-hardening-issues/g20.md#issue-g20-003函数实参-tuple-元素不匹配被计划误写为-ae1003)：
  已按 G15 普通重载规则锁定调用 token 的 `AE0512`，未加入 tuple 特判；
- [ISSUE-G20-004](./feng-language-conformance-coverage-hardening-issues/g20.md#issue-g20-004正向聚合用例混入无关的整数运算类型错误)：
  新增测试夹具已修正，不涉及产品语义变更；
- [ISSUE-G20-005](./feng-language-conformance-coverage-hardening-issues/g20.md#issue-g20-005tuple-不可变绑定赋值沿用已迁移的旧错误码描述)：
  已把 `let` tuple 整体与解构绑定赋值统一映射到 G13 的 `AE0104`，并增加 tuple 解构直接证据；
- G20 没有待人工决策或遗留问题。

#### 24.4.5 建议 commit message

`fix(tuple): reject object construction and harden diagnostics`

## 25 G21：数组类型、创建长度与索引诊断

### 25.1 测试重点与范围

以[内建类型规范](../specifications/feng-builtin-type.md)第 5～8 节、
[表达式规范](../specifications/feng-expression.md)第 4、6.2、6.3、7 节、
[模块规范](../specifications/feng-module.md)第 4、7 节、
[SE 错误码规范](../specifications/feng-error-codes-se.md)和
[AE 错误码规范](../specifications/feng-error-codes-ae.md)为唯一语言依据，验证数组类型层级、逐层可写性、
字面量推导与目标贴合、指定长度创建、默认零值、索引读写和跨包类型身份，并锁定对应的 Parser / Semantic
稳定诊断及运行时失败边界。

本组特别锁定以下已经人工确认的语义：

- Feng 不设置固定的数组嵌套层数上限。源码内、Semantic 类型身份、package-public `.ft` 往返和跨包
  consumer 必须保留相同的完整层数及每一层 `!`；33 层与 65 层只用于命中现有 32 / 64 位固定容量
  缺口，不是新的语言边界；
- `Type[:n]` 的 `n` 必须是整数、只求值一次，合法范围为 `0` 到目标平台 `int` 类型的最大值。编译期
  可确定的负数或超上界值在长度表达式产生 `AE0204`；动态非法值在数组分配和任一元素默认初始化前
  触发 panic，不得截断、回绕或形成 UB；
- `Type[:n]` 是按元素默认零值创建数组的实例化入口，元素类型必须静态具备有限默认零值；该资格与
  `n` 的值无关，因此常量 `0`、常量正数和动态长度采用相同检查并在失败时产生 `AE0332`。数组自身
  的空数组默认零值与由显式已有元素组成的数组字面量不请求元素默认零值；
- shared generic 中 `T` 的默认零值只由具体 `T` 的 reified descriptor 决定，并在无初始值
  局部绑定、字段、捕获绑定、static 字段和 `T[:n]` 保持一致。`T[:n]` 只对每个元素重复该能力，
  不得自行识别 enum、string、数组、对象或其他具体类型；
- 合法 `n` 仍可能使元素总字节数或数组对象总大小无法表示，或者无法取得所需内存；这属于分配失败并
  在运行时 panic，不把合法长度重新定义成非法长度，也不以真实耗尽机器内存作为自动化测试手段；
- 数组索引只接受整数并只求值一次；每个 `value[index]` 只消费一层数组类型。负数、宽整数窄化前不可
  表示的值以及 `index >= length` 均须在读写元素前由现有边界检查路径触发 panic，不得截断或回绕。

G04 已交付的数值字面量贴合、G12 已交付的数组 / 索引语法错误、G13 已交付的绑定与写目标可写性、
G14 已交付的非数组目标和非整数索引诊断、G18 已交付的默认零值有限性，以及 G02 的短名、alias 和
完整模块路径规则，均优先建立逐项映射；只有缺少直接证据时才新增用例。本组为证明跨包数组类型身份
所需的最小 `.ft` 往返属于明确例外，不扩展到一般制品、发布或 C ABI 测试。

所有可观察的合法行为必须进入 FCTS 真实编译运行；预期 panic 的创建长度和索引用例必须由测试父进程
编译后在隔离子进程中运行，父进程只验收其未成功返回，并显式排除启动失败。Semantic 接受、生成 C
文本或宿主进程自身崩溃均不能代替上述证据。

### 25.2 用例 TODO

#### 25.2.1 数组类型语法、层级与类型身份

- [x] ARRAY-D01：覆盖 `T[]`、`T[!]` 以及 `T[][]`、`T[][!]`、`T[!][]`、`T[!][!]` 四种两层组合；
  Parser AST、Semantic 类型比较和合法读写必须一致确认 `!` 只属于其紧邻层，不能向内层或外层传播；
- [x] ARRAY-D02：映射 G12 已有的数组字面量、索引和 `Type[:n]` 缺项 / 缺右括号、非法 array-new
  target 等 `SE02xx` 精确证据；补齐 `T![]`、`T[]!`、`T[!!]`、`T[!` 等孤立 / 重复 `!` 和缺失
  `]` 的最小 Parser 反例，每项断言唯一错误码、token、行列、来源文件和 Parser 阶段；
- [x] ARRAY-D03：数组叶子元素类型必须是合法的非 `void` 类型。`void[]` / `void[!]` 在 `void` token
  产生唯一 `AE0502`；未知类型、裸泛型名称、非泛型类型误带实参和实参数量错误分别映射既有
  `AE1013`、`AE0006`、`AE1014`、`AE1015`，不得为数组重复定义同根因错误码；
- [x] ARRAY-D04：在同包源码中使用 33 层和 65 层、且 `!` 交替分布的数组类型，确认 Parser、Semantic、
  Codegen 和最外层空数组运行行为均成功；再用相邻层数及不同 `!` 分布证明类型身份不碰撞、不截断。
  这些层数是针对旧固定容量的回归哨兵，不能被实现为新的最大值；
- [x] ARRAY-D05：provider 的公开模块级绑定、字段、函数参数和返回类型至少各取一个 33 / 65 层混合
  可写数组，经真实 package-public `.ft` 写出和重新读取后，由 consumer 通过短名、`import ... as`
  和完整模块路径访问；逐层核对层数与 `!`，并验证不同长度实例仍属于同一数组类型；
- [x] ARRAY-D06：以超过 64 层且 `!` 分布不同的数组作为 `fit` / spec witness subject，只验证数组
  subject key 的结构身份和候选隔离；不得因固定 `uint64_t` mask 拒绝、合并或错配不同数组类型。
  一般 fit 规则仍归 G23，本项修复不得增加任何运行时代码。

#### 25.2.2 数组字面量推导、目标贴合与求值

- [x] ARRAY-D07：无目标的非空数组字面量覆盖全部纯数值字面量默认推导、首个已具备静态数值类型的
  非字面量位于首 / 中 / 尾时驱动字面量贴合，以及多个非字面量元素类型完全一致；结果叶子类型必须
  可由读取位置直接证明，并映射 G04 既有证据；
- [x] ARRAY-D08：显式数组目标分别来自局部绑定、普通赋值、函数参数、函数返回值和对象字段初始化；
  每个元素均按目标叶子类型贴合，合法固定宽度数值、托管引用和值类型各至少经过一条真实目标传播
  路径。普通重载调用仍按 G15 的候选规则决议，不为数组字面量增加调用特判；
- [x] ARRAY-D09：同一字面量内出现彼此不兼容的已知元素类型时，在首个不匹配元素产生唯一
  `AE0201`；元素彼此同型但与非调用目标数组叶子类型不兼容时，在首个不兼容元素产生唯一 `AE1003`；
  普通调用因此没有可匹配候选时沿用 `AE0512`。三条路径分别锁定码、精确位置、数量和 Semantic 阶段；
- [x] ARRAY-D10：无目标的直接 `[]` 与嵌套 `let value = [[]];` 都必须在实际缺少目标的空字面量左
  方括号产生唯一 `AE0203` 并停止在 Semantic；`let value: T[][] = [[], []];` 及相同目标来自参数、
  返回值或字段时必须合法。不得再把嵌套空字面量泄漏为 Codegen 的 `CE0182`；
- [x] ARRAY-D11：`[[1], [2, 3], []]` 在具有明确 `int[][]` 目标时合法，证明数组长度不是类型的一部分；
  嵌套层数或叶子元素类型与目标不一致时产生唯一 `AE1003`，不得把“各行长度不同”误报成维度错误；
- [x] ARRAY-D12：数组字面量的各元素从左到右各求值一次；覆盖普通值、函数调用和托管引用，并在
  FCTS 直接断言事件轨迹与最终元素值。编译期类型检查不得改变合法程序的求值次数或顺序；
- [x] ARRAY-D13：无目标字面量推导为当前层只读 `T[]`；同一个字面量贴合显式 `T[!]` 目标后当前层
  可写。`let` / `var` 只决定绑定能否重绑定，不能把推导出的 `T[]` 变成 `T[!]`，也不能取消显式
  `T[!]` 的元素写权限。

#### 25.2.3 类型兼容、显式转换与逐层可写性

- [x] ARRAY-D14：元素类型、层数和逐层 `!` 完全一致的数组可在绑定、赋值、参数和返回值位置流转，
  且实例长度不同不影响类型兼容；FCTS 至少观察一次整数组和托管元素数组的引用传递；
- [x] ARRAY-D15：叶子元素类型不同、数组层数不同或任一对应层 `!` 不同的已有数组值均不得隐式
  兼容，在实际源表达式产生唯一 `AE1003`。数组字面量按目标贴合属于 ARRAY-D08，不得与已有数组值
  的隐式转换混为一谈；
- [x] ARRAY-D16：显式转换可一次移除一个或多个既有 `!`，并保持同一数组实例与元素值；任何增加
  `!`、改变层数或改变叶子元素类型的 cast 均在 cast 起始 token 产生唯一 `AE1023`；
- [x] ARRAY-D17：即使 `Dog` 可按 `Animal` spec 视角使用，`Dog[]` 与 `Animal[]` 之间的隐式赋值和
  显式 cast 仍分别产生 `AE1003` 与 `AE1023`；合法邻界通过显式遍历重建目标数组，证明 Feng 不提供
  数组 variance；
- [x] ARRAY-D18：分别对 `T[][]`、`T[][!]`、`T[!][]`、`T[!][!]` 执行外层替换和内层元素写入；
  未标记层在准确写目标产生唯一 `AE0104`，已标记层正常写入，写入值类型错误产生唯一 `AE1003`；
- [x] ARRAY-D19：成对覆盖 `let value: T[!]` 可修改元素但不可重绑定，以及 `var value: T[]` 可重绑定
  但不可修改元素；多个绑定引用同一 `T[!]` 实例时，一个别名写入须由另一个别名观察到。可写性诊断
  映射 G13，引用身份和合法结果进入 FCTS。

#### 25.2.4 `Type[:n]` 创建、长度范围与元素零值

- [x] ARRAY-D20：`Type[:n]` 和 `Type<Args>[:n]` 覆盖内建类型、普通类型、类型参数和闭合泛型类型；
  公开具名元素类型必须分别支持导入短名、alias 限定路径和无需 import 的完整模块路径。三种路径的
  元素类型身份和运行结果一致；`Type[n]` 继续按普通索引解析，不恢复旧数组创建语法；
- [x] ARRAY-D20-A：`Type[:n]` 的元素 `Type` 必须接受任意合法、具备有限默认零值的类型引用，包括
  `int[][:n]`、`int[!][:n]` 与多层数组类型；这些写法创建“元素本身为数组”的数组，每个元素按其
  数组类型的默认空数组值初始化。Parser AST 必须保留完整内层层级及每层 `!`，不得把 `Type[]` 的
  方括号误解析成普通索引，也不得把本项实现为仅针对内建类型或固定层数的特判；
- [x] ARRAY-D21：长度为 `bool`、浮点、`string`、普通对象或数组时，在长度表达式起始 token 产生
  唯一 `AE0202`；所有固定宽度有符号 / 无符号整数及 `int` / `uint` / `byte` 均可作为长度表达式的
  静态类型，是否落入有效值域由 ARRAY-D22～ARRAY-D24 独立判断；
- [x] ARRAY-D22：在 32 位和 64 位 Semantic 目标上分别覆盖常量 `0`、`1`、目标 `int` 最大值、`-1`
  及刚好超过目标 `int` 最大值；前 3 个通过编译期范围检查但最大值不实际分配，后 2 个在长度表达式
  产生唯一 `AE0204`。超上界样本须以足够宽的显式整数类型承载，避免先触发字面量自身的目标类型
  越界诊断；直接字面量、显式整数 cast 和现有编译期可确定的纯整数表达式均不得绕过长度检查；
- [x] ARRAY-D23：动态合法长度覆盖 `0`、`1` 和普通正数；带副作用的长度表达式只执行一次，随后
  分配恰好 `n` 个元素。FCTS 必须直接断言长度、求值次数，以及标量、`bool`、enum、`string`、普通
  对象、tuple / `@value type`、嵌套数组和闭合泛型元素的规定零值；显式 enum 首成员取非零值时，
  直接 `Enum[:n]` 与 shared-generic `T[:n]` 都必须得到该首成员，不得退化为底层零比特；
- [x] ARRAY-D23-A：为 ARRAY-D23 建立非数组泛型基线：以显式非零首成员 enum 及零默认 trivial、
  managed pointer、aggregate 邻界，覆盖 shared-generic 无初始值局部绑定、实例 / static 字段和捕获绑定。
  各入口必须与同一具体类型的直接默认零值一致；该基线只用于证明 `T[:n]` 复用通用能力，不把非数组
  行为重新定义为数组特例；
- [x] ARRAY-D24：动态负数和动态大于目标 `int` 最大值的无符号 / 宽整数必须在隔离子进程中于分配前
  panic；测试同时证明长度表达式只执行一次、值没有先转换成 `size_t` 后截断或回绕，且没有进入元素
  默认初始化。常量非法值不得进入这条运行时路径；
- [x] ARRAY-D25：对语言值域内但 `n * element_size` 或对象总大小溢出的确定性输入，直接测试运行时
  分配边界并验收 panic；不得通过申请接近机器物理内存来测试 OOM。算术溢出、总大小溢出和分配器
  返回失败继续归同一“分配失败”边界，不新增长度语义或 UB；
- [x] ARRAY-D26：无显式目标时 `Type[:n]` 推导为 `T[]`，贴合显式 `T[]` / `T[!]` 时按目标决定当前
  层可写性；已有数组目标的叶子类型、层数或 `!` 不兼容时产生唯一 `AE1003`，不得靠结果值的实际
  长度改变类型结论；
- [x] ARRAY-D27：把 `Type[:n]` 作为独立的元素默认零值实例化入口接入 G18 的有限性检查。元素类型
  具备有限默认零值时，分别以常量 `0`、常量正数和动态合法长度通过 Semantic；正数路径进入 FCTS，
  直接断言标量、`string`、普通无环对象和闭合泛型对象的每个元素均为规定默认零值。该检查只增加
  编译期成本，不得新增运行时资格检查、分支、元数据或 ABI。
- [x] ARRAY-D27-A：在同包源码中，以直接自引用普通对象、两个普通对象互相引用，以及首成员沿元素
  类型形成递归的 union-form 作为 `Type[:n]` 元素类型；对常量 `0`、常量 `1` 和动态整数长度分别在
  元素类型引用处产生唯一 Semantic `AE0332`，锁定诊断码、token、行列、数量和阶段。默认零值资格
  必须与长度值无关，不依赖常量求值，也不得让任一路径进入 Codegen 或退化为 NULL 元素。
- [x] ARRAY-D27-B：经过真实 package-public `.ft` 往返，以导入短名、alias 限定路径和无需 import 的
  完整模块路径分别创建数组；外部有限默认零值元素类型必须合法，外部直接 / 间接递归元素类型必须与
  同包一致产生唯一 `AE0332`。该结论必须由通用元素类型解析及默认零值元数据得到，不得按路径或类型
  名增加特判；若完整修复要求改变 `.ft` 格式或版本，必须暂停并再次取得人工批准。
- [x] ARRAY-D27-C：补齐不会请求元素默认零值的邻界：`let empty: RecursiveType[];` 得到合法空数组，
  有明确 `RecursiveType[]` 目标的 `[]` 合法，函数参数提供的既有 `RecursiveType` 可用于
  `[existing]`；这些正例必须通过 Semantic，其中可运行的空数组行为进入 FCTS 并断言长度为 `0`。
- [x] ARRAY-D27-D：增加 Codegen 内部不变量防御专项；若测试夹具绕过 Semantic，向 Codegen 传入元素
  类型没有有限默认零值的 array-new，必须在元素类型引用处产生唯一 `IE` 并终止，不得生成数组对象
  或 NULL 元素、暴露 `CE` 或静默继续发码。

#### 25.2.5 索引类型、逐层访问与运行时边界

- [x] ARRAY-D28：所有固定宽度有符号 / 无符号整数及 `int` / `uint` / `byte` 均可作为索引；`bool`、
  浮点、`string`、普通对象和数组索引沿用 G14 的唯一 `AE1022` 契约。已有 G14 精确证据直接映射，
  缺少的整数正例进入集中式 Semantic / FCTS 用例；
- [x] ARRAY-D29：标量、对象、tuple、spec 值和其他非数组目标在读 / 写位置均沿用 G14 的唯一
  `AE1021`；写位置不得因后续可写性或右值类型检查追加派生诊断；
- [x] ARRAY-D30：`matrix[i][j]` 的每一对方括号只消费一层；读取一层后得到内层数组，耗尽全部层后
  再索引产生唯一 `AE1021`。`matrix[i, j]` 不属于 Feng 多维索引语法，必须在 Parser 阶段拒绝，不得
  把它描述成 Semantic 的“索引数量”错误；
- [x] ARRAY-D31：长度为 `n > 0` 时，`0` 与 `n - 1` 的读写成功；`-1`、`n`、`n + 1`、无法由目标
  平台索引表示的宽整数，以及对长度 `0` 数组的任意索引均在隔离子进程中于元素访问前 panic。常量
  越界索引仍走相同运行时边界，不新增数组长度到静态类型；
- [x] ARRAY-D32：索引读取按“先数组接收者、后索引”顺序各求值一次；索引写入按既有赋值顺序先
  定位数组与索引、再求值右值、最后写入。FCTS 用事件轨迹同时覆盖嵌套索引和带副作用的接收者，
  不得重复接收者、索引或右值求值；
- [x] ARRAY-D33：索引读取和写入覆盖标量、托管引用、tuple / `@value type` 及泛型元素；值类型读取
  保持值复制，引用类型读取保持引用语义，写入继续使用既有逐元素所有权 / ARC 路径。本组不得为了
  边界检查增加额外元素复制、堆分配或 ARC。

#### 25.2.6 FCTS 行为与覆盖总映射

- [x] ARRAY-D34：新增或扩展一个独立 G21 FCTS 文件，真实运行字面量推导与贴合、不等长嵌套数组、
  多层可写性、不同长度同类型、合法显式降权、引用别名、三种 `Type[:n]` 名称路径、动态合法长度、
  元素零值、索引邻界和求值顺序；每项必须断言可观察值，不能只检查编译成功；
- [x] ARRAY-D35：建立 ARRAY-D01～ARRAY-D34 到既有 Parser、Semantic、Symbol、Codegen、runtime、
  CLI、G02 / G04 / G12 / G13 / G14 / G18 和 FCTS 证据的逐项映射。已有反向测试若只断言失败或消息
  片段，没有稳定码、精确 token、行列、数量和阶段，则不视为完整证据；优先新增集中式 G21 用例，
  不得为统一断言风格修改无关既有测试。

### 25.3 独立验收与交付 TODO

- [x] 先实施并关闭 [G21 问题记录](./feng-language-conformance-coverage-hardening-issues/g21.md) 中的
  `ISSUE-G21-001`～`ISSUE-G21-015`；其中 `ISSUE-G21-005`、`ISSUE-G21-011`、`ISSUE-G21-015` 已关闭，
  `ISSUE-G21-006` 与 `ISSUE-G21-013` 已完成人工决策，分别按 ARRAY-D27～ARRAY-D27-D 和
  ARRAY-D23～ARRAY-D23-A 实施并验收；
- [x] 数组层数实现必须采用可随实际类型结构扩展的表示；不得只把 32 / 64 改成另一个常量。`.ft v1`
  应优先使用已有可组合记录实现向后兼容往返；若完整修复确实要求改变 `.ft` 格式 / 版本，必须先暂停
  并取得人工批准。Semantic subject key 的调整只发生在编译期，不得影响生成程序；
- [x] 在 AE 数组段登记并实现 `AE0204`；`AE0201`～`AE0204`、`AE1003`、`AE0512`、`AE1023`、
  `AE0104`、`AE1021`、`AE1022`、`AE0502` 与 `AE0332` 各自只负责其既有根因。用户源码非法数组必须
  在 Parser / Semantic 终止；若修复触及 Codegen 防御，其错误应为 `IE`，不得继续暴露 `CE0182` /
  `CE0185` 等用户可修复错误，也不得开展与新增用例无关的 CE 全面迁移；
- [x] 独立运行 Parser 与 Semantic 专项，逐项核对唯一诊断、稳定码、触发 token、行列、来源文件、
  编译阶段和 32 / 64 位目标差异；合法最大长度只检查 Semantic 接受，不实际申请对应内存；
- [x] 独立运行 Symbol、Codegen 与 runtime 专项，验证 33 / 65 层本包和真实 `.ft` 往返不截断、限定
  array-new 解析到同一声明、长度仅求值一次且在窄化前验证、确定性分配溢出 panic，以及合法元素
  继续使用既有默认零值与所有权路径；同时验证 array-new 的元素默认零值资格在同包和跨包均于
  Semantic 收口，Codegen 只保留 `IE` 内部防御且不会生成 NULL 元素；
- [x] 独立运行隔离进程 CLI 专项，覆盖动态非法长度和非法索引。父进程不得依赖某一具体信号或
  sanitizer 退出码，但必须确认子进程未成功返回且不是启动失败；故意 panic 的程序不接入常规 FCTS
  主入口；
- [x] 运行完整 FCTS，确认 ARRAY-D04、ARRAY-D07～ARRAY-D08、ARRAY-D11～ARRAY-D19、ARRAY-D20、
  ARRAY-D23、ARRAY-D26～ARRAY-D27、ARRAY-D27-C、ARRAY-D32～ARRAY-D34 的合法行为均由主入口
  登记并真实执行；
- [x] 性能约束：固定层数修复和静态长度诊断必须是纯编译期成本；常量合法长度不得新增运行时检查；
  动态长度只允许在创建点执行一次已批准的范围验证，并应与既有分配溢出检查收敛。普通数组读取、
  写入、`length()`、遍历和 ARC 不得增加检查、分支、调用、分配或元数据；索引仍只保留既有必需的
  一次边界判断，并须在该判断内完成窄化前合法性判定；
- [x] 不新增针对 std、具体容器、具体类型名或固定层数的特判，不修改公开 ABI。除
  `ISSUE-G21-013` 已批准的 runtime 私有 trivial descriptor 能力与 aggregate 默认初始化命名收敛外，
  默认不修改 runtime 私有 ABI；若其他问题的正确实现要求新增 / 变更 runtime 接口，必须先列出
  调用面、开销和替代方案，再取得人工批准；
- [x] 未经再次批准，不修改任何既有测试输入或断言；已有证据只建立映射，缺口通过新增集中式测试、
  新增 FCTS 用例和测试主入口登记补齐。若实施发现必须修改既有测试，先列出精确函数 / 文件和原因；
- [x] 在 Codex 沙箱外为 G21 独立执行 `make test`，记录 UBSan 与 normal 两阶段的 Smoke、标准库、
  FCTS、性能约束及其余回归结果；此前分组的全量结果不能代替 G21 回归；
- [x] 执行 `git diff --check`，关闭或取得人工决策保留 G21 问题；填写稳定码映射、既有证据、新增
  用例、专项结果、全量结果、性能与兼容性结论及建议的英文 commit message 后，才可标记 G21 已交付。

### 25.4 独立交付记录

#### 25.4.1 状态与诊断映射

- 状态：已交付；ARRAY-D01～ARRAY-D35 全部完成；
- 数组语法继续映射 G12 的 `SE02xx`，并新增逐层 `!`、缺失 `]`、限定 / 数组 type-ref 的 Parser
  精确证据；
- 数组叶子类型映射 `AE0502`、`AE1013`、`AE0006`、`AE1014`、`AE1015`；字面量推导、目标贴合和
  调用候选分别锁定 `AE0201`、`AE1003`、`AE0512`，无目标空字面量锁定 `AE0203`；
- array-new 长度类型、静态值域及元素默认零值资格分别锁定 `AE0202`、`AE0204`、`AE0332`；已有
  数组兼容、cast、写目标、非数组索引目标和索引类型继续映射 `AE1003`、`AE1023`、`AE0104`、
  `AE1021`、`AE1022`；
- 所有新增反向用例均断言唯一诊断、精确 token、行列、来源文件和阶段；Semantic 不变量被人工测试
  夹具绕过时，Codegen 只报告 `IE`，不再把对应用户错误泄漏为 `CE`。

#### 25.4.2 Compiler 与 runtime 证据

- Parser 新增 ARRAY-D01、ARRAY-D02、ARRAY-D20、ARRAY-D20-A 集中用例，覆盖一 / 两层全部可写
  组合、非法后缀、短名 / alias / 完整路径、闭合泛型、数组元素 type-ref 及 65 层动态类型树；
- Semantic 新增 ARRAY-D03、ARRAY-D06、ARRAY-D09～ARRAY-D11、ARRAY-D15～ARRAY-D18、
  ARRAY-D21～ARRAY-D22、ARRAY-D26～ARRAY-D30 集中矩阵，并覆盖 32 / 64 位目标常量边界、同包及
  导入类型的默认零值有限性、分支非结果表达式目标隔离；
- Symbol 通过真实 package-public `.ft v1` 写出 / 读取验证 33 / 65 层数组、逐层 `!`、公开绑定、字段、
  参数、返回类型及 ARRAY-D27-B 的有限性元数据；`.ft` 复用既有 ARRAY record 链，没有改变格式版本；
- Codegen 新增长度与索引结构、tuple 元素目标、完整元素默认零值和 `IE` 防御用例；所有生成 C 均成功
  编译。runtime 新增 trivial 默认零值策略测试，并复用数组分配边界、aggregate 初始化和所有权专项；
- CLI 在隔离子进程中运行动态负长度、超目标 `int` 长度、负 / 边界后 / 空数组 / `u64` 最大索引，
  确认程序已成功编译启动并在实际非法操作时失败。

#### 25.4.3 FCTS 行为证据

- 新增并登记 `test_g21_array_semantics.ff`，包含 6 条实际运行用例：33 / 65 层类型；字面量推导、五类
  目标传播及求值顺序；数组精确身份、逐层降权与可写性；三种名称路径、动态长度及完整元素默认零值；
  shared-generic 局部 / 实例字段 / static 字段 / 捕获；索引顺序及标量、托管、tuple、值类型和泛型
  元素行为；
- 新增真实依赖包 provider 与无 import 完整路径 consumer，覆盖公开闭合泛型、首成员非零 enum 及
  包含该 enum 的 trivial 值类型；短名、alias 和完整路径均得到相同默认零值；
- ARRAY-D01～ARRAY-D35 已分别映射到上述 Parser、Semantic、Symbol、Codegen、runtime、CLI、既有
  G02 / G04 / G12 / G13 / G14 / G18 证据与新增 FCTS，没有以仅编译成功替代可观察行为断言。

#### 25.4.4 专项、全量回归与性能

- Parser、Semantic、Symbol、Codegen、runtime、CLI 专项与生成 C 编译全部通过；
- 完整 FCTS 通过：`1157/1157`，相对 G20 增加 6 条 G21 行为用例；
- 沙箱外 `make test` 全量通过：UBSan 与 normal 两阶段的 Smoke `91/91`、标准库 `604/604`、FCTS
  `1157/1157`，以及性能约束、增量构建、发布脚本、macOS 收尾、bundled packages、toolchain fetch、
  全部单元测试和 CLI 回归；
- 深层类型、`.ft` 和静态长度检查只有编译期成本；静态合法长度不生成新检查，动态 `Type[:n]` 只在
  创建点进行一次窄化前检查；索引保持一次边界判断；
- 具体零默认类型保持直接 `calloc` / 直接初始化路径。shared generic 只在默认初始化入口按 descriptor
  选择一次策略，`T[:n]` 每次创建只选择一次；只有非零 trivial 或托管默认值执行必要回调。普通数组
  读写、`length()`、遍历和 ARC 没有新增开销；
- 没有新增 std、具体容器、具体类型名或固定层数特判。公开 ABI、Feng + C ABI 和 `.ft` 格式不变；
  runtime 私有 trivial descriptor 扩展及 aggregate `default_zero_init` 命名收敛均属于已批准范围；
  既有语言测试输入和断言未修改，既有 runtime 私有接口引用只做已批准的机械命名同步；
- `git diff --check` 通过。

#### 25.4.5 问题关闭情况

- [G21 问题记录](./feng-language-conformance-coverage-hardening-issues/g21.md) 中
  `ISSUE-G21-001`～`ISSUE-G21-015` 已全部实施、验证并关闭，没有待人工决策或遗留问题。

#### 25.4.6 建议 commit message

`test(language): complete G21 array conformance hardening`

## 26 G22：模块可见性诊断

### 26.1 测试重点与范围

验证模块名称访问、文件级 import / alias、名称冲突和有效可见范围的正反向行为，保障同模块跨文件、
同包跨模块及跨包时的名称解析结果与诊断正确。规范依据为
[模块规范 §2～§4、§7](../specifications/feng-module.md)、
[可见性规范](../specifications/feng-visibility.md)及
[AE 错误码规范](../specifications/feng-error-codes-ae.md)。

本轮语义调整仅为 MODULE08、MODULE09 对应的两类别名冲突惰性化；同模块顶层声明的非法重名、
alias 与本文件顶层声明或其他 alias 的冲突继续急切检查。普通 import 与本文件或同模块其他文件
顶层声明的冲突继续惰性检查。“局部值优先于模块路径”只按模块规范已有范围验收，不扩展为
“具体类型或所有具体符号优先于模块路径”。规则只在主规范定义，以下条目是其测试验收条件。

所有条目先映射已有直接证据，再补缺口。G02 的三种访问形式和合法隔离行为、G05 的 seal 构造、
G13 的同模块重名、G15 的重载、G18 的静态性等已有用例可以引用，但不能以它们代替本轮新增行为
的直接验证。spec / fit 满足、泛型推导、`@friend` 和 `@mixable` 完整专项不在本组重做；这里只核对
名称与可见性边界未被本轮修改破坏。

跨包用例须经过真实 package-public `.ft` 导出、读取与 consumer 分析；`.ft` 在本组是语言行为测试
载体，不扩展为二进制格式、打包发布或 C ABI 专项。合法行为放入 `fcts/`，非法语义与诊断放入
`test/`。本组修复发生在编译期，不引入运行时名称解析、权限检查或额外初始化。

### 26.2 用例 TODO

#### 26.2.1 原有用例细化

- [ ] MODULE01：在依赖集合固定、源码输入或已加载包内均不存在目标模块时，分别验证无别名 import、
  alias import 被拒绝，即使没有引用成员也应在 import 处报错；未知完整路径分别覆盖类型引用和
  值表达式，诊断落在 consumer 的相关引用。增加改为存在且可见模块后通过的邻界程序。
  包下载失败、网络错误和损坏制品不作为本条的语言诊断用例。
- [ ] MODULE02：目标模块存在且可见，但目标公开名称确实不存在时，分别验证短名、alias 限定名、
  完整路径下的类型引用、函数调用和绑定访问被拒绝；对短名使用不存在任何其他候选的唯一名称，
  区分“名称未定义”与限定访问的“模块未导出名称”。仅把目标名称替换为实际公开声明后应通过，
  不可见声明单列 MODULE03，不能用不存在声明代替私有性检查。
- [ ] MODULE03：以 `type`、`enum`、`spec`、顶层 `func`、模块级 `let`、模块级 `var` 六类声明分别
  验证顶层可见性：同模块其他文件可直接访问模块私有声明；同包其他模块和跨包 consumer 不得
  通过短名、alias 或完整路径访问该私有声明。对应公开声明按所在模块的有效可见范围通过；省略
  修饰与显式 `seal` 均须有直接证据。跨包被 `.ft` 过滤的声明允许按未导出或未知名称诊断，不要求
  consumer 恢复已过滤的私有信息。
- [ ] MODULE04：来自两个不同无别名 import 的同名公开符号，未引用时编译并运行通过，实际裸名
  引用时报二义性；分别覆盖六类顶层声明，并增加至少一组跨种类冲突和三个来源的多义冲突。
  不得只检查候选插入顺序选出的第一个声明；同名函数来自不同模块时，不因参数签名不同而合成
  新的重载集合。已有直接证据可映射，缺口补最小反例及未引用的正向配对。
- [ ] MODULE05：alias 与本文件顶层 `type`、`enum`、`spec`、`func`、`let`、`var` 或另一 alias 同名，
  分别验证未引用、已引用都在声明检查阶段报错；诊断指向本文件的冲突声明，不依赖函数体执行。
  同名 alias 指向不同目标和同一目标都不能规避重复声明检查。嵌套块局部绑定不混入该矩阵。
  原条目中的两类惰性冲突拆至 MODULE08、MODULE09。
- [ ] MODULE06：文件 A、B 属于同一模块，只有 A 无别名 import 外部模块；B 使用该外部模块的
  六类公开名称短名时分别被拒绝，诊断来源文件必须是 B。正向验证 B 自己 import 后通过、B 使用
  可见目标的完整路径后通过，以及本模块自身在 A 声明的名称仍可由 B 使用，避免把文件级导入
  隔离错误扩大为模块声明隔离。
- [ ] MODULE07：分别以短名、alias 和无需 import 的完整路径访问六类公开顶层声明，正向程序
  实际运行并断言构造后字段、enum 值、spec 视角操作、函数返回值和模块绑定读写结果；函数同时
  覆盖直接调用与函数值，`var` 经一种形式写入后由另一种形式读到同一状态。映射 G02 既有证据，
  补齐本轮 alias 解析变更影响的入口，不仅断言 Semantic 接受或 Codegen 文本。

#### 26.2.2 名称冲突与文件隔离补充

- [ ] MODULE08：alias 与同模块其他文件的六类顶层声明同名时，各自构造“存在冲突但未引用”
  的运行正例，以及只增加一次名称引用的反例；后者在使用点报二义性并指出 alias 与其他文件
  声明的来源。类型位和表达式位分别覆盖，不能在 import 声明处报错，也不能无声选择模块或
  本模块声明。关联 ISSUE-G22-001。
- [ ] MODULE09：alias 与无别名 import 引入的六类公开名称同名时，未引用允许、引用时二义性；
  以 alias import 在普通 import 前后两种顺序验证相同结论。诊断指出 alias 对应的目标模块和
  同名导入声明的来源，不因为 alias 是本文件显式声明而给予优先级。关联 ISSUE-G22-001。
- [ ] MODULE10：对 MODULE08、MODULE09 的冲突补齐使用入口：`alias.Type` 的绑定类型、参数、
  返回类型和构造；`alias.Enum.Item`；`alias.Spec` 的类型引用；`alias.func()` 和函数值；
  `alias.binding` 的读取及可变绑定写入。各入口各有最小反例，不得只在普通调用处检查而遗漏
  类型解析、构造或赋值目标。泛型只覆盖限定类型名出现在类型实参中的名称解析，推导规则归 G25。
- [ ] MODULE11：对有歧义的限定访问设置“后续成员只存在于 alias 目标”“只存在于另一候选”
  两种反例，两者均在首段名称使用处报二义性；另以两个外部同名函数中只有一个签名匹配实参的
  反例，验证不以成员存在性、返回类型或签名匹配来消除名称来源冲突。
- [ ] MODULE12：仅依赖包或当前编译输入含有同名声明、但该声明所在外部模块未被本文件 import，
  不构成本文件 alias 的候选冲突；普通 import 目标的非公开名称也不参与冲突。以未导入、已导入
  但不可见两种运行正例验证候选可见范围，再以对应公开且已导入声明的使用点反例形成配对。
- [ ] MODULE13：无别名 import 引入名称与本文件顶层声明同名，仍为未引用通过、裸名引用二义性；
  覆盖六类声明及函数／绑定的跨种类碰撞，防止本轮误改为本文件顶层声明优先或声明时急切报错。
  已有 `test_lazy_ambiguity_import_vs_local_type` 等及 G02 正例优先映射。
- [ ] MODULE14：无别名 import 引入名称与同模块其他文件顶层声明同名，导入文件未引用时通过、
  导入文件引用时二义性；没有该 import 的兄弟文件仍能使用本模块声明。覆盖类型、函数和绑定
  三类入口，其余声明种类结合 MODULE04 的统一规则补缺，避免兄弟文件被无关 import 污染。
- [ ] MODULE15：映射 G13 同模块顶层非法重名和 G15 非法重载的急切诊断，以及合法重载运行证据；
  同文件和跨文件各保留最小未引用反例，证明 MODULE08 的放宽只针对文件级 alias，不放宽真正的
  模块声明重复。不在本条改动模块级绑定规则或重载规则；若发现需要改实现，先记录原因并人工决策。
- [ ] MODULE16：对 MODULE04、MODULE08、MODULE09、MODULE13、MODULE14 的碰撞分别用不冲突
  alias 和可见目标的完整模块路径消歧，程序运行时断言实际访问的是指定来源；非冲突名称的使用
  不触发旁侧名称的潜在冲突。需要回避局部值遮蔽的模块使用不冲突 alias，不能假定完整路径总能
  绕过局部值。
- [ ] MODULE17：按既有“局部值优先于模块路径”规则，分别用函数参数、局部 `let` 和局部 `var`
  与完整路径首段同名；成员存在时读取或调用局部对象并断言结果，成员不存在时报告普通成员错误，
  不能回退到恰好有该成员的模块。正向验证不冲突 alias 仍可访问被遮蔽模块；不新增具体类型、
  顶层声明优先规则。映射 IMP27 后补其反向及绑定形式缺口。
- [ ] MODULE18：alias 只在声明文件生效：兄弟文件使用未声明的 alias 限定名应失败；两个文件
  各自使用同一 alias 拼写指向不同模块应通过，并分别运行验证目标。alias import 不额外注入其
  成员短名，用唯一名称排除其他候选；同一目标经不同且不冲突的 alias 访问应指向相同声明及绑定
  状态，不因访问入口不同制造两个存储槽。
- [ ] MODULE19：无名称碰撞的模块 alias 不能单独作为类型或运行时值；分别覆盖 `let x: alias`
  及把 alias 作为值读取、传参或直接调用，检查对应 Semantic 诊断。增加 `alias.Type` 与
  `alias.func()` 的合法邻界；涉及碰撞时由 MODULE08～MODULE11 验证二义性，不以别名误用诊断
  掩盖尚未解析清楚的名称来源。
- [ ] MODULE20：对同文件急切冲突、两类别名惰性冲突、普通 import 多义及文件隔离用例，分别
  交换 import 顺序和 provider／consumer／兄弟文件输入顺序，验收成功或失败结论、来源集合及
  使用点归属一致；不要求与语义无关的候选列举顺序固定。单个冲突使用点不因多轮分析重复报错，
  无引用时不提前报错；错误不泄漏到没有冲突引用的文件。

#### 26.2.3 可见性与跨包补充

- [ ] MODULE21：验证模块层可见范围：默认和显式 `seal module` 的公开声明可在同包其他模块
  使用，但不能由包外经 import、alias 或完整路径访问；`open module` 的公开声明可跨包使用。
  同一模块多文件声明的可见性必须一致，冲突时编译期拒绝；单一文件或一致的多文件声明形成正例。
  不用更改模块可见性同时又更改成员可见性的程序代替单层边界对照。
- [ ] MODULE22：在可见 owner type 上验证实例及静态字段／普通方法的默认 open、显式 open、
  显式 seal：所属 type 的实现内可用，普通顶层函数或其他 type 无论在同模块、同包其他模块还是
  包外，都不能越过 seal。字段覆盖读取、可变字段写入和对象字面量字段初始化；方法覆盖调用与
  方法值形成。方法值形成后的普通调用不额外增加运行时权限检查；可变性、静态性另用合法上下文
  隔离，不让其他错误掩盖可见性诊断。
- [ ] MODULE23：映射 G05 的公开／seal 构造及全 seal 构造跨包证据，补缺时分别覆盖所属 type
  自身允许构造、同模块外部代码拒绝、同包跨模块拒绝、跨包拒绝；区分 `Type()`、`Type() { ... }`
  和有公开无参构造时的 `Type { ... }`。公开静态工厂返回实例的运行正例可用，不能因 package-public
  `.ft` 未暴露 seal 构造就错误恢复隐式公开无参构造。
- [ ] MODULE24：在同名成员重载集合中先过滤不可访问候选、再匹配签名：可见 open 候选不被
  同名 seal 候选遮蔽，仅有不可见候选时拒绝；覆盖实例／静态普通方法直接调用和有目标 callable
  类型的方法值形成，合法路径运行验证选中目标。已有专项直接映射，不改变重载合法性规则。
- [ ] MODULE25：映射公开签名可见性一致性的 `AE0327` 证据，补齐顶层绑定、函数参数与显式／
  推导返回类型，以及公开 type 的字段、方法、构造签名；递归包含泛型实参、数组元素、指针目标
  和 callable 组成类型，检查私有类型不能因包裹而泄露。合法邻界覆盖私有成员使用私有类型、包内
  可见签名引用同等范围类型。spec / fit 签名及泛型约束的完整检查映射已有专项，不改变其语义。
  显式类型诊断定位相关类型引用，推导类型没有类型 token 时定位声明，provider 失败后不生成公开 `.ft`。
- [ ] MODULE26：经过真实 package-public `.ft` 往返，验证公开声明可访问、模块私有声明不可见，
  因布局或实现所需而保留的 seal 成员仍不可由无权限 consumer 访问。分别核对导出选择和 consumer
  语义，不能把“`.ft` 收录”当作“语言公开”；源码直接联合分析或手工构造导入元数据只能作为补充。
- [ ] MODULE27：在真实跨包 consumer 中复验 MODULE08、MODULE09 的未引用通过／使用时报冲突，
  以及 MODULE16、MODULE18 的合法消歧和文件隔离；provider 与 consumer 分开编译，consumer 只
  通过包公开接口取得 provider 声明。别名只在 consumer 文件产生，不能写成新的 provider 声明，
  也不能按 alias 拼写改变原声明身份；至少覆盖类型、函数、模块绑定入口，六类声明的直接证据在
  源码及跨包矩阵中逐项登记，缺项继续补齐。
- [ ] MODULE28：映射现有 `@friend`、`@mixable`、spec seal 与 fit 访问授权的正反向测试，核对
  本轮名称解析变更没有扩大成员权限、穿透 module／owner 可见性或把保留的 seal 元数据当成 open。
  普通 seal 测试使用不含这些授权的夹具；已存在授权测试继续按各自主规范验收，不在 G22 增加
  新授权形式或重做这些特性的完整组合矩阵。

### 26.3 独立验收与交付 TODO

#### 26.3.1 已知问题与实现边界

- [ ] 按 [ISSUE-G22-001](./feng-language-conformance-coverage-hardening-issues/g22.md#issue-g22-001别名与非本文件声明的名称冲突仍被急切拒绝)
  调整两类别名冲突：声明检查只保留本文件急切冲突，使用点统一执行外来名称来源的二义性检查；
  类型位、表达式位及赋值目标同时覆盖，不能仅删除前置报错而留下候选静默优先。
- [ ] 按 [ISSUE-G22-002](./feng-language-conformance-coverage-hardening-issues/g22.md#issue-g22-002ae0906-文档废弃标记与现有别名急切诊断不符)
  对齐错误码说明：新增惰性冲突沿用名称二义性 `AE0005`，本文件 alias／顶层声明冲突保留当前
  `AE0906`，重复 alias 保留 `AE0902`；纠正文档中的废弃表述，不借此大范围迁移错误码。
- [ ] 过程中新增问题先在 G22 问题文件记录，再分析、修复并补回归；语义不能由规范确定、需要
  特判、改模块级绑定实现、增加运行时开销或改 runtime 私有 ABI／公开 ABI／`.ft` 格式时，说明
  具体原因并交人工决策。与补测无关的纯重构不进入本组。

#### 26.3.2 既有用例迁移清单

以下是本次 Review 的完整迁移清单，只调整两类已确认语义对应的诊断断言；实施入口仍以人工
批准 G22 为准。两条均位于 `test/semantic/test_semantic.c`，原函数名与复现场景保留：

- [ ] `test_import_alias_conflicts_with_other_file_local_value`：原期望在第 2 行 import 处报告
  `AE0906`，改为第 4 行 `helper.assert()` 的 `helper` 使用处报告 `AE0005`，并检查 alias 与
  兄弟文件声明来源；补列、token 和数量断言。
- [ ] `test_import_alias_conflicts_with_imported_short_name`：原期望在第 3 行 import 处报告
  `AE0906`，改为第 5 行 `helper.store()` 的 `helper` 使用处报告 `AE0005`，并检查两方来源；
  补列、token 和数量断言。

未使用通过的正例及其他入口均新增用例；`test_import_alias_conflicts_with_same_file_local_value`、
`test_duplicate_use_alias_in_same_file` 等急切冲突既有用例保持原断言。清单外既有测试需要修改时，
必须先记录具体测试、原因与预期差异，再由人工决定，不得批量刷新断言以通过回归。

#### 26.3.3 本组独立验收

- [ ] 逐项登记 MODULE01～MODULE28 的规范条款、已有测试函数／新增用例、测试层级、源码或
  跨包路径、正反向结果。六类顶层声明、三种访问形式及可见范围的适用格均须有证据；不适用格
  写明理由，不用一个成功路径代表全部入口。已有用例满足时可以直接映射，不重复造同构测试。
- [ ] 建立稳定诊断映射，至少包含 `AE0005`、`AE0901`～`AE0904` 中现用出口、`AE0906`、
  普通成员可见性和 `AE0327`，未知短名／类型引用分别映射现有诊断。每个最小反例断言错误来源
  文件、码、token、行列、数量和 Semantic 阶段；惰性别名冲突定位首段名称 token，急切冲突定位
  相冲突声明。只核对必要的来源信息，不锁定完整诊断文本或无语义要求的候选顺序。
- [ ] 非法输入在 Semantic 被拒绝，不进入 Codegen 或生成无效后端代码；单个使用点的错误不因
  多轮分析重复产生，不退化为 IE／CE。对真实 `.ft` 中已过滤的名称按 consumer 可获得的信息诊断，
  provider 的公开签名错误定位 provider，不转嫁给 consumer。
- [ ] 合法行为在 FCTS 实际编译、运行并断言选中来源、返回值、字段结果及共享绑定状态；未使用
  碰撞的文件通过公开的无冲突测试入口实际纳入编译与运行。Compiler test 的“分析成功”不能代替
  这些运行证据；测试产物在工程 `build/` 或 `temp/` 下执行。
- [ ] 本组专项完成后，在 Codex 沙箱外独立执行完整 `make test`；记录命令、退出码和各套件结果。
  其他组的回归结果不能代替本组验收。若继续修复产品实现或测试，完成后重新执行本组全量回归。
- [ ] 执行 `git diff --check`，核对只发生已批准的既有测试迁移，关闭或明确决策 G22 问题；填写
  实际新增用例、实现变更、专项及全量结果，给出英文 commit message，不自动提交。

### 26.4 独立交付记录

- 状态：TODO 已细化，待人工 Review；尚未开始实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：[G22 问题记录](./feng-language-conformance-coverage-hardening-issues/g22.md)
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
