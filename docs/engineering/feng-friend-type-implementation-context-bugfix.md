# Feng `@friend` 类型实现上下文授权修复方案

> **状态**：已完成。
>
> **文档定位**：本文只记录本次问题、修复语义、实现范围与验证计划，不是正式
> 语言规范。Review 通过后，先修改
> [`feng-visibility.md`](../specifications/feng-visibility.md) 中唯一的权威语义，再同步
> [`feng-friend-member-access-dev.md`](./feng-friend-member-access-dev.md) 的历史设计说明，
> 最后实施编译器与测试。
>
> **核心结论**：`@friend(F)` 的授权主体是语义类型 `F`，不是 `F` 的某一类普通
> 方法。凡词法归属于 `F` 的类型实现上下文，都可以消费该成员的 friend 权限。

## 1. 问题背景

TUI 将 `ViewManager.dispatchMouse` 收紧为以下成员时：

```feng
open type ViewManager {
  @friend(std.tui.TuiApp)
  seal func dispatchMouse(event: MouseEvent<Widget>): void {
    // ...
  }
}
```

`TuiApp` 的普通实例方法能够访问它，但在 `TuiApp` 构造函数中形成方法值会被拒绝：

```feng
open type TuiApp {
  func TuiApp() {
    self.input.onMouse = self.view.dispatchMouse;
    //                       ^ AE0308，随后产生级联 AE0522
  }
}
```

完整路径 `std.tui.TuiApp` 已经正确解析为 friend type，失败与跨 module 名称解析无关。
根因是编译器只把普通 `METHOD` 方法体识别为 friend 权限的消费位置，构造函数、
终结器和字段初始化表达式都无法建立当前 friend 主体。

当前 TUI 中其他 `@friend` 使用没有暴露该问题，是因为它们都由 `ViewManager` 或
`Inspector` 的普通方法访问；已有编译器测试也只覆盖了普通实例方法、静态方法、
普通方法中的方法值以及同包 fit 方法，没有覆盖 friend type 的构造函数、终结器和
字段初始化上下文。

## 2. 问题定性

### 2.1 注解位置与授权消费位置是两个独立问题

以下现有规则保持不变：

- 构造函数和终结器不能标注 `@friend`；
- `@friend` 仍只能标注符合既有规则的显式 `seal` 字段或普通方法；
- 如果需要限制 seal 构造函数的调用方，继续使用 `@friend` seal static 工厂方法。

本次修复处理的是另一方向：一个成员已经通过 `@friend(F)` 授权给类型 `F` 后，
`F` 的哪些实现代码可以消费该权限。不能因为构造函数和终结器不能成为注解目标，
就禁止它们使用所属 type 已经取得的授权。

### 2.2 当前限制不能形成有效安全边界

当前被拒绝的访问都可以通过普通方法间接完成：

```feng
type Reader {
  seal func read(vault: Vault): int {
    return vault.secret();
  }

  func Reader(vault: Vault) {
    let value = self.read(vault);
  }
}
```

因此，仅禁止构造函数、终结器或字段初始化表达式直接消费 friend 权限，不会限制
类型实际能够执行的操作，只会迫使代码增加包装方法和额外调用。

### 2.3 修复不改变运行时安全模型

`@friend` 仍是纯编译期访问控制。扩大合法的词法消费位置不会：

- 改变对象布局、ABI、方法表或运行时分派；
- 导出 friend 元数据到 package-public `.ft`；
- 绕过字段初始化顺序、`self` 捕获、静态性、可变性、泛型、重载或异常规则；
- 绕过 member owner type/spec、module 或 fit 的既有可见性；
- 让没有被列入 friend 集合的类型获得权限。

构造函数和字段初始化仍必须通过各自已有的初始化合法性检查；friend 只在其他规则
均通过后，替代被访问成员最后一层 `seal` 检查。

## 3. 修复后的统一语义

### 3.1 类型实现上下文

对带有 `@friend(F)` 的 seal 成员 `M`，以下词法位置属于类型 `F` 的实现上下文，
可以消费 `M` 的 friend 权限：

1. 实例字段初始化表达式；
2. 静态字段初始化表达式；
3. 普通实例方法体；
4. 普通静态方法体；
5. 构造函数体；
6. 终结器体。

嵌套在上述实现表达式或方法体中的 lambda 继承其词法 owner type，因此使用同一
friend 主体。lambda 是否在稍后执行、是否逃逸，不改变编译期词法授权；普通方法
返回的 lambda 已具有相同性质。

该规则应按“当前词法类型实现 owner”实现，不应继续把每一种成员类别分别加入
friend 特判。

### 3.2 不属于类型实现上下文的位置

以下位置不因 `@friend(F)` 获得权限：

- 顶层函数和顶层绑定初始化表达式；
- 其他 type/spec 的字段初始化或方法体；
- 仅实现共同 spec、存在 mix 关系或持有 `F` 实例的代码；
- friend type 的调用方；
- 其他包中的 `fit F`；
- package-public `.ft` 的消费者。

授权仍不传递：若 `F` 是 friend，`F` 自己的 friend、字段类型、父子类型或共同 spec
实现者都不会自动获得权限。

### 3.3 同包 fit 规则保持不变

与成员 `M` 位于同一包、且目标类型语义等于 `F` 的 `fit F` 实例方法和静态方法，
继续按现有规则消费 friend 权限：

- fit 目标必须与 friend type 完成泛型代入后的语义身份完全相等；
- fit 必须与 `M` 位于同一包，不能从外部包恢复未导出的 friend 元数据；
- `M` 的完整签名必须对 fit 声明 module 可用；
- fit 只取得 `F` 对具体成员 `M` 的授权，不因此成为 `F` 自身，也不能访问 `F`
  的普通 seal 成员或其他未授权成员；
- fit 仍只能通过其实例方法或静态方法消费权限，fit 不新增字段、构造函数或终结器
  上下文。

### 3.4 形式化访问条件

对 owner `O` 的 seal 成员 `M`、friend 集合中的类型 `F` 和访问点 `A`，friend 分支
仅在以下条件全部满足时放行：

```text
1. O、M、module、spec/fit 视角和普通成员查找规则均已通过；
2. M 是合法声明 @friend 的显式 seal 成员；
3. A 的词法类型实现 owner 经泛型代入后语义等于 F；
   或 A 位于满足现有同包规则、目标类型语义等于 F 的 fit 方法中；
4. 静态性、可变性、泛型、重载及完整签名可见性规则均已通过。
```

这里的“词法类型实现 owner”由源码声明位置决定，不由 receiver 的运行时类型、调用者、
动态分派结果或 lambda 实际执行时机决定。

## 4. 当前实现根因

语义分析器的 `build_current_friend_subject` 当前要求：

```c
context->current_callable_member->kind == FENG_TYPE_MEMBER_METHOD
```

由此产生三个直接后果：

- 构造函数和终结器虽然具有正确的 `current_type_decl`，仍无法建立 friend 主体；
- 字段初始化阶段没有 `current_callable_member`，也无法建立 friend 主体；
- 方法值形成与普通调用共用成员访问检查，所以二者在这些上下文中同时失败。

供 LSP completion 复用的 `feng_semantic_member_has_friend_access` 也要求 enclosing member
必须是 `METHOD`。如果只修复编译器成员访问，LSP 仍会在已经合法的构造函数、终结器
和字段初始化位置隐藏 friend 成员。

当前 friend 类型身份、owner 泛型实参代入、同包 fit 检查、签名可见性检查和成员
方法值过滤均可复用，不是本次问题根因。

## 5. 编译器修复方案

### 5.1 引入明确的词法实现 owner

Resolver 应明确维护当前正在解析的类型实现 owner，而不是从 `METHOD` 枚举值反推：

```text
current_type_implementation_owner: FengDecl?
```

进入下列可执行实现上下文前设置为所属 type，退出后恢复前值：

- 实例或静态字段初始化表达式；
- 普通实例或静态方法；
- 构造函数；
- 终结器。

顶层初始化、顶层函数、声明签名扫描和不属于 type 实现的其他阶段保持为空。这样既能
覆盖当前全部合法位置，也不会因为未来增加一种 type 方法类别而再次遗漏或继续堆叠
成员种类特判。

该标记只存在于编译器 Resolver 上下文，不进入 Feng 运行时，也不产生目标程序开销。

### 5.2 统一构建 friend 主体

friend 访问主体按以下顺序构建：

1. 如果当前位于 fit 方法，继续使用现有 fit 目标类型及同包规则；
2. 否则，如果存在 `current_type_implementation_owner`，使用该 type 及其当前有效泛型
   参数构建语义身份；
3. 否则不存在 friend 主体，访问按普通 seal 规则拒绝。

不得仅把当前判断机械扩展为 `METHOD | CONSTRUCTOR | FINALIZER`，因为这种修改仍会
遗漏字段初始化，并保留错误的“按成员种类枚举授权上下文”抽象。

### 5.3 保持成员访问流水线不变

修复只替换 friend subject 的来源，以下顺序保持不变：

```text
owner/module/spec/fit 可见性
    ↓
普通成员候选收集与静态性过滤
    ↓
friend 或既有 seal 访问判断
    ↓
泛型与重载选择
    ↓
方法调用或方法值形成
```

不可访问 friend 候选仍不能遮蔽同名 open 候选；普通调用和方法值继续使用同一个成员
访问谓词。

### 5.4 LSP 同步

LSP completion 必须使用与语义分析相同的“词法类型实现 owner”规则：

- 构造函数、终结器和字段初始化位置应展示当前 type 获得授权的 friend 成员；
- 非 friend type、顶层上下文和外包 fit 继续隐藏；
- parsed-only recovery 没有归一化 friend 元数据时继续 fail closed；
- 不在 LSP 中复制一份独立的类型身份或包判断算法。

现有公开语义查询的参数和注释应从“enclosing type/fit method”调整为能够表达 enclosing
type implementation context；具体接口形态在实施时以复用语义分析事实、避免重复判断为准。

## 6. 规范修改范围

Review 通过后，正式规范只在
[`feng-visibility.md`](../specifications/feng-visibility.md) 的 `@friend` 小节定义最终
语义：

- 将“friend type 自身声明的实例方法和静态方法可以使用授权”修改为“friend type
  的类型实现上下文可以使用授权”；
- 在同一处定义类型实现上下文包含实例/静态字段初始化、实例/静态普通方法、构造函数
  和终结器；
- 明确“构造函数和终结器不能标注 `@friend`”只限制注解目标，不限制它们消费所属
  type 的 friend 权限；
- 保持同包 `fit FriendType`、签名可见性、非传递性和 `.ft` 不导出规则不变。

已有工程文档只同步设计目标、授权语义、测试计划和完成状态，不再建立第二份独立规范。
本文在修复完成后保留为问题与实施记录，并继续引用正式规范。

## 7. 测试计划

未经人工批准不修改已有测试用例；实施时新增独立用例。

### 7.1 Semantic 正向测试

至少覆盖 friend type 在以下上下文中访问授权实例字段、静态字段、实例方法和静态方法：

- 实例字段初始化表达式；
- 静态字段初始化表达式；
- 普通实例方法；
- 普通静态方法；
- 构造函数；
- 终结器；
- 上述上下文中的普通调用；
- 上述上下文中的方法值形成；
- 嵌套 lambda；
- friend type 与 owner 位于同 module；
- friend type 与 owner 位于同包不同 module；
- 泛型 owner 与泛型 friend 完成代入后的精确身份匹配。

### 7.2 Semantic 反向测试

至少确认以下访问仍被拒绝：

- 非 friend type 的字段初始化、方法、构造函数和终结器；
- 顶层绑定初始化和顶层函数；
- 不同泛型实参的 friend type；
- 其他包中的 `fit FriendType`；
- friend type 的 friend 或共同 spec 实现者；
- owner 的其他未标注 seal 成员；
- 被授权成员签名包含访问点 module 不可见类型的非法跨 module 场景；
- 构造函数和终结器自身标注 `@friend`，继续产生既有非法注解诊断。

### 7.3 初始化规则回归

friend 放行后仍需确认：

- 字段初始化表达式不会额外获得直接捕获 `self` 的能力；
- 构造函数的 `let` 字段绑定次数和完成性检查不变；
- 静态字段与实例字段的访问形式不混淆；
- 终结器的参数、返回和异常逃逸规则不变。

### 7.4 LSP 测试

completion 至少覆盖：

- friend type 的字段初始化、构造函数和终结器中出现授权成员；
- 非 friend type 和顶层上下文中不出现；
- 同包 fit 保持现有结果；
- imported/parsed-only recovery 继续 fail closed。

### 7.5 Feng 兼容性与全量回归

FCTS 增加可执行语言行为测试，验证构造函数、终结器和字段初始化能够实际调用授权成员，
并确认普通方法及同包 fit 的既有行为不退化。全部非文档变更完成后，在非 Codex 沙箱
环境运行：

```sh
make test
```

## 8. 兼容性与风险

### 8.1 源码兼容性

本修复只把此前错误拒绝的访问改为接受，不改变已有合法程序语义，也不新增运行时行为。
唯一可观察变化是对应位置不再产生 seal 成员不可访问诊断，LSP 同步展示这些成员。

### 8.2 包边界

friend 元数据继续不导出到 package-public `.ft`。同包全部源码在一次语义分析中持有
归一化 friend 事实；外部包只能看到普通 seal 表面，不能恢复授权。

### 8.3 实现风险

主要风险不是运行时，而是编译器上下文设置不完整或恢复不对称：

- 某一类型实现入口没有设置词法 owner，导致继续错误拒绝；
- 退出嵌套解析后没有恢复 owner，导致后续声明错误继承权限；
- 编译器和 LSP 对实现上下文的判断不一致；
- lambda 嵌套时错误丢失或扩大词法 owner。

实现时应使用成对保存/恢复的上下文切换，并通过正反测试覆盖连续声明、嵌套 lambda、
同文件多 type 和跨文件顺序，不能依赖声明顺序或残留状态。

## 9. 实施任务清单

Review 通过后严格按以下顺序实施。任务完成并验证后将对应项从 `- [ ]` 更新为
`- [x]`；部分完成的任务保持未勾选，并在任务下记录当前进度。

### 9.1 正式规范与工程文档

- [x] 修改 `docs/specifications/feng-visibility.md`，将 friend 消费位置统一定义为
  “词法类型实现上下文”。
- [x] 在正式规范中明确类型实现上下文包含实例/静态字段初始化、实例/静态普通方法、
  构造函数和终结器。
- [x] 在正式规范中明确构造函数和终结器不能标注 `@friend`，但可以消费所属 type
  已取得的 friend 权限。
- [x] 确认正式规范中的同包 `fit FriendType`、签名可见性、非传递性和 `.ft` 不导出
  规则保持不变。
- [x] 同步 `docs/engineering/feng-friend-member-access-dev.md` 的设计目标、授权语义、
  测试计划和完成状态，并引用正式规范。

### 9.2 Semantic 实现

- [x] 在 Resolver 上下文中增加明确的词法类型实现 owner 状态。
- [x] 在实例字段和静态字段初始化表达式解析前设置实现 owner，并在退出时恢复前值。
- [x] 在普通实例方法和静态方法解析前设置实现 owner，并在退出时恢复前值。
- [x] 在构造函数和终结器解析前设置实现 owner，并在退出时恢复前值。
- [x] 调整 friend subject 构建，优先保持现有 fit 分支，否则使用词法类型实现 owner。
- [x] 删除 friend 消费位置对 `FENG_TYPE_MEMBER_METHOD` 的错误单一限制。
- [x] 确认普通调用与方法值形成继续复用同一个 friend 成员访问谓词。
- [x] 确认泛型身份代入、签名可见性、重载过滤和包边界逻辑保持不变。

### 9.3 LSP 实现

- [x] 调整 `feng_semantic_member_has_friend_access` 的接口和注释，使其能够表达词法
  类型实现上下文。
- [x] 让 completion 在 friend type 的字段初始化、构造函数和终结器中展示授权成员。
- [x] 确认非 friend type、顶层上下文、外包 fit 和 parsed-only recovery 继续 fail closed。
- [x] 确认 LSP 复用 Semantic 的 friend 类型身份、泛型代入和包判断，不复制独立算法。

### 9.4 Semantic 测试

- [x] 新增实例字段初始化表达式消费 friend 权限的正向测试。
- [x] 新增静态字段初始化表达式消费 friend 权限的正向测试。
- [x] 新增构造函数消费 friend 权限的普通调用与方法值测试。
- [x] 新增终结器消费 friend 权限的普通调用与方法值测试。
- [x] 新增嵌套 lambda 继承词法 owner type 权限的测试。
- [x] 新增同包不同 module 的正向测试。
- [x] 新增泛型 friend 精确身份匹配的正向与反向测试。
- [x] 新增非 friend type 在字段初始化、构造函数和终结器中的反向测试。
- [x] 新增顶层初始化、顶层函数、外包 fit 和未授权 seal 成员的反向测试。
- [x] 确认构造函数和终结器标注 `@friend` 继续产生既有非法注解诊断。
- [x] 确认字段初始化、构造函数和终结器的既有专用规则不退化。

### 9.5 LSP 与 FCTS 测试

- [x] 新增字段初始化、构造函数和终结器中的 friend completion 正向测试。
- [x] 新增非 friend type、顶层上下文和 imported/parsed-only recovery 的 completion
  反向测试。
- [x] 新增 FCTS 可执行行为测试，覆盖字段初始化、构造函数和终结器直接调用授权成员。
- [x] 确认普通方法、静态方法和同包 fit 的既有 friend 行为不退化。

### 9.6 验证与交付

- [x] 运行 friend Semantic 定向测试。
- [x] 运行 LSP 定向测试。
- [x] 运行 FCTS 定向测试。
- [x] 在非 Codex 沙箱环境运行全量 `make test`。
- [x] 将定向测试和全量回归结果记录到本文档。
- [x] 更新本文档状态和全部已完成任务的勾选状态。
- [x] 检查“实施过程问题记录”中的问题均已解决，或已经明确交由人工决策。
- [x] 提交全部变更供人工 Review，不自动提交 Git commit。

### 9.7 验证记录

2026-08-21 完成以下验证：

- `make build/bin/test_semantic && build/bin/test_semantic` 通过；
- 沙箱外 `build/bin/test_cli` 通过，覆盖类型实现上下文 completion、非 friend/顶层
  隐藏、parsed-only fail-closed 和 package-public friend 成员不导出；
- 沙箱外 `make fcts-tests` 通过：`Total: 817, Passed: 817, Failed: 0, Skipped: 0`；
- 沙箱外 `make test` 退出码为 0；UBSan 与普通构建两轮均通过，包含全部单元测试、
  两轮 91/91 smoke、CLI 项目测试、std/FCTS、性能约束、增量构建、发布脚本和工具链
  预构建包获取验证。

## 10. 实施过程问题记录

实施过程中发现任何偏离本文方案、现有规范或预期测试结果的问题时，必须先在本节新增
记录，再进行原因分析和解决；不得先加入特判或扩大变更范围。尚未解决的问题保留
`状态：处理中` 或 `状态：等待人工决策`，人工决定不纳入当前任务的问题标记为
`状态：已延期（人工决定）`，解决并验证后更新为 `状态：已解决`。

### P01：新增用例误用了尚不支持的静态方法值

- 状态：已解决
- 发现阶段：Semantic 正向测试
- 现象：字段初始化、构造函数和终结器中的 `Vault.readShared` 静态方法值形成产生
  `AE0522`；同一静态方法的直接调用和实例方法值均未产生 friend 访问诊断。
- 影响：仅影响新增测试组合，不影响本次 friend 类型实现上下文语义。
- 事实与复现：最小诊断源的三个 `Vault.readShared` 方法值位置均报告“不匹配预期函数
  类型 `Producer`”，没有报告 `AE0308` 等成员不可访问诊断。
- 原因分析：当前 Feng 已支持的具体成员方法值范围包含实例方法，不包含该静态方法值
  形式；本次修复不能借 friend 测试扩大独立的方法值能力。
- 决策：正向用例保留实例方法值，并分别在字段初始化、构造函数和终结器中通过直接调用
  覆盖静态 friend 方法；不修改静态方法值语言能力。
- 解决：已移除新增测试中的静态方法值断言，保留实例方法值和静态方法直接调用覆盖。
- 验证：`make build/bin/test_semantic && build/bin/test_semantic` 通过，新增与既有
  Semantic 用例全部通过。

### P02：终结器中的 lambda 捕获 `self` 未完成 codegen lowering

- 状态：已延期（人工决定）
- 发现阶段：Semantic/FCTS 正向测试设计验证
- 现象：终结器中声明 `() -> self.retained.read()` 的 lambda 已通过 Semantic，但直接
  编译在 codegen 阶段产生 `CE0103: lambda self capture was not lowered to a capture cell`。
- 影响：阻止“终结器中的嵌套 lambda 继承词法 owner”进入可执行 FCTS 覆盖；普通终结器
  friend 访问和不捕获 `self` 的 lambda 是否受影响仍待隔离验证。
- 事实与复现：当前最小诊断源稳定在终结器 lambda 的 `()` 位置报告 CE0103，错误发生
  在 friend Semantic 检查之后，不是 seal/friend 访问诊断。
- 原因分析：已使用不含 `@friend` 的独立最小复现确认：普通 type 的终结器只要通过
  lambda 捕获 `self`，就会产生同一 CE0103。该问题是既有 finalizer lambda capture
  codegen lowering 缺陷，与本次 friend Semantic 修改无关。
- 决策：人工确认该独立终结器问题不影响 friend 优化，本次不扩大范围修复。
- 解决：friend 用例避免终结器 lambda 捕获 `self`；终结器通过直接访问覆盖实例 friend
  成员，并通过不捕获 `self` 的 lambda 覆盖嵌套词法 owner 授权。CE0103 留待后续专项。
- 验证：friend Semantic 定向测试通过；沙箱外 `make fcts-tests` 为
  `Total: 817, Passed: 817, Failed: 0, Skipped: 0`，不捕获 `self` 的终结器 lambda、
  终结器直接访问和实例方法值均成功执行。

### P03：CLI 定向测试在既有进程管理用例提前失败

- 状态：已解决
- 发现阶段：LSP 定向测试
- 现象：运行完整 `build/bin/test_cli` 时，在到达新增 friend LSP 用例前输出
  `error: process exited with status -1 (no such process)`，随后于
  `test/cli/test_cli.c:634` 的 `WEXITSTATUS(status) == 0` 断言失败。
- 影响：当前无法用该次完整 CLI 测试结果验证新增 LSP 用例；是否为既有易波动问题或
  本次变更影响尚待隔离。
- 事实与复现：`build/bin/test_cli` 已成功以 `-Werror` 编译；首次完整运行约 18 秒后在
  既有进程等待辅助逻辑失败，输出发生在新增 friend LSP 测试之前。
- 原因分析：两次沙箱内运行均在既有 DAP 子进程用例稳定失败；同一测试二进制移到
  沙箱外后通过，确认是测试执行环境不符合仓库进程测试要求，不是 friend 代码变更。
- 决策：不修改或规避既有测试；所有 CLI/LSP 与全量测试均按仓库要求在沙箱外执行。
- 解决：未改动既有进程测试；改用沙箱外验证。
- 验证：新增 imported fail-closed 用例落盘并重新构建后，沙箱外
  `build/bin/test_cli` 通过；最终沙箱外 `make test` 也以退出码 0 完成。

新增问题使用以下模板：

```markdown
### PXX：问题标题

- 状态：处理中 / 等待人工决策 / 已延期（人工决定） / 已解决
- 发现阶段：对应的实施任务
- 现象：可复现的实际表现和诊断信息
- 影响：受影响的规范、Semantic、LSP、测试或兼容性范围
- 事实与复现：相关源码位置、最小复现和已执行的检查
- 原因分析：基于事实确认的根因；未确认内容明确标记为待验证
- 决策：采用的通用方案；需要扩大语义或加入特殊处理时记录人工决定
- 解决：实际修改内容
- 验证：定向测试与回归结果
```

## 11. Review 检查点

本方案请求人工确认以下结论：

1. friend 权限属于语义 type，而不是普通方法类别；
2. 类型实现上下文包含实例/静态字段初始化、实例/静态普通方法、构造函数和终结器；
3. 上述上下文中的嵌套 lambda 按词法 owner type 继承授权；
4. 构造函数和终结器仍不能成为 `@friend` 注解目标；
5. 同包 `fit FriendType` 规则保持不变；
6. 顶层代码、其他 type 和其他包 fit 继续不能消费权限；
7. 实现使用明确的词法实现 owner，不采用成员类别逐项特判。
