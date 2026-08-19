# Feng object-form `spec` 方法级泛参暂不支持备注

> 状态：已完成主规范、Semantic、未完成路径清理与测试更新，并已通过
> `make test` 全量回归（2026-08-19）。
>
> 本文只记录暂不支持的边界、原因、现状和实施任务。规则落地后，当前语言语义
> 仍以 [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 为唯一权威来源；
> 本文不长期复制主规范。未来恢复该能力的技术分析继续由
> [object-form `spec` 方法级泛型修复文档](./feng-object-form-spec-generic-method-bugfix.md)
> 跟踪。

## 1 决定与边界

当前阶段，Parser 继续接受并完整保留下列语法结构，但 Semantic 必须在编译
object-form `spec`、检查成员方法签名时以前置诊断拒绝成员自己声明方法级泛参：

```feng
spec Surface {
  func identity<T>(value: T): T;              // 暂不允许
  seal static func create<T>(value: T): T;   // 暂不允许
}
```

该限制统一适用于：

- 实例方法和静态方法；
- 无修饰（语义为 `open`）和 `seal` 方法；显式 `open` 仍由既有 spec 成员规则拒绝；
- `spec` 自有成员以及从父 `spec` 继承后形成的 requirement surface。

语法结构本身没有错误，因此不得从 Parser 语法或 AST 中删除方法泛参，也不得把
`<T>` 改报为语法错误。Semantic 应在 object-form `spec` 成员方法签名检查处报告
明确的“当前不支持 spec 方法级泛参”诊断，然后停止处理该声明，使其不再进入
后续声明解析、满足检查或 Codegen。

既有 `AE1013`“找不到类型”检查及其通用解析逻辑继续原样保留，本次不得修改。
新增前置检查只是让 object-form spec 方法级泛参先按更准确的当前语义被拒绝；
它不改变其他位置何时报告 `AE1013`。

以下既有能力不在限制范围内：

```feng
// spec 类型级泛参继续允许；成员本身不是泛型方法。
spec Surface<T> {
  func identity(value: T): T;
}

// type / fit 的成员方法级泛参继续允许。
type Value {
  func identity<T>(value: T): T {
    return value;
  }
}

// 顶层函数或成员方法以 spec 作为泛型约束继续允许。
func invoke<T: Surface<int>>(value: T): int {
  return value.identity(1);
}

// callable-form spec 的类型级泛参继续允许；泛型来源在形成 callable 值前闭合。
spec Identity<T>(value: T): T;
```

本决定只限制 **object-form `spec` requirement 自己声明的方法级泛参**，不得扩大
为禁止泛型 `spec` owner、type/fit 泛型方法、泛型函数、泛型约束或已显式闭合的
callable-form `spec` 来源。

## 2 当前仓库事实

截至 2026-08-19，对仓库中 322 个 `.ff` 文件的检查结果如下：

- `std/`、`fcts/`、smoke 和其他实际 Feng 源码中使用数为 **0**；
- 编译器测试中只有一段由 `#if 0` 禁用的正向片段，用于保留当前失败场景
  （`test/cli/test_cli.c`）；
- 工程文档中共有 6 处相关示例声明，分布在
  `feng-object-form-spec-static-dev.md`、
  `feng-object-form-spec-generic-method-bugfix.md` 和
  `feng-object-form-spec-method-value-dev.md`，不是可运行代码；
- 标准库规划中有一个会受影响的未来接口：
  `feng-std-future.md` 中的
  `Scheduler.awaitFuture<T>(future: Future<T>): T`。

当前编译器已经能够接受部分简单形式，但这不代表能力正确。隔离验证表明：

- `Surface.identity<T>` 的实现体没有 reified dependency 时，声明、满足、调用和
  Codegen 可以通过；
- 当 A 的实现依赖 `Box<T>`、B 的实现依赖 `Cell<T>`，同一个 `Surface` 调用点分别
  接收 A/B 时，编译和宿主链接仍会成功；
- 生成调用只传入 requirement 的空 `FengFunctionDescriptor`，实际实现随后读取
  `_desc->reified_type_deps[0]`，验证程序以退出码 139 失败。

所以当前状态是“部分形式被接受，但无法保证生成程序正确”，不能继续作为隐式的
实验能力开放。

因此，实施专用 Semantic 拒绝不会破坏仓库内当前可编译的 Feng 源码或既有包
ABI。受影响的是尚未完成的语言承诺和未来 API 设计，相关文档必须在实施时同步
收敛，不能继续把该能力描述为已经可用。

## 3 为什么当前不允许

### 3.1 类型级泛参可以在 witness 建立前闭合

对于：

```feng
spec Surface<T> {
  func identity(value: T): T;
}
```

`Surface<int>`、`A<int>: Surface<int>` 和 `B<int>: Surface<int>` 的 owner 类型实参
在具体 spec 值和 witness 形成时已经确定。方法调用只需要沿 witness 选择一个
签名已经闭合的实现，现有类型描述符树能够表达该关系。

### 3.2 方法级泛参需要同时路由实现与实现描述符

对于：

```feng
spec Surface {
  func identity<T>(value: T): T;
}

func invoke(subject: Surface): int {
  return subject.identity<int>(1);
}
```

调用点已经知道 requirement 是 `Surface.identity`，也知道方法实参是 `int`；但
实际实现仍由运行时 witness 决定。若 `A.identity<int>` 的共享体依赖 `Box<int>`，
而 `B.identity<int>` 的共享体依赖 `List<int>`，正确调用必须成对选择：

```text
A witness -> A.identity -> A.identity<int> descriptor tree（Box<int>）
B witness -> B.identity -> B.identity<int> descriptor tree（List<int>）
```

当前 witness 只完成实现函数选择，尚不能同时路由与该实现匹配的闭合
`FengFunctionDescriptor` 子树。调用点生成一个固定 descriptor 再原样转发，不能
同时适配 A 和 B；requirement 自身的空依赖树也不能替代实现方法体的依赖树。

这不是类型实参没有闭合，也不是现有树形 descriptor 不能表达多层泛型。缺失的
是动态 witness 所选实现与同一实现 descriptor 子树的一致路由。若为此引入运行时
descriptor 构造、缓存、查找、JIT 或新的增量分派成本，将违反 Feng 当前静态
descriptor tree 和可预测运行时开销的约束。

在完整、通用且不增加未经批准运行时开销的方案确定前，Semantic 在 spec 成员
方法签名检查阶段直接拒绝该 requirement，比让程序进入不完整 Codegen 或生成
错误 ABI 更安全、清晰。检查处必须通过代码注释说明：该语法已被 Parser 正确
识别；当前拒绝的原因是动态 witness 尚不能将实现函数与同一实现的 descriptor
子树一致路由，而不是方法泛参语法非法。

## 4 其他语言的处理

### 4.1 C++：最接近当前 Feng 决定

C++ 允许普通成员函数模板，也允许类模板拥有虚函数；但 C++ 工作草案明确规定：
成员函数模板不得声明为 `virtual`，成员函数模板的特化也不会覆盖基类虚函数：
[C++ working draft `[temp.mem]`](https://eel.is/c++draft/temp.mem)。

因此 C++ 可以表达：

```cpp
template<class T>
struct Surface {
  virtual T identity(T value) = 0; // owner T 在 Surface<int> 处闭合
};
```

但不能表达一个通过虚表动态分派的 `virtual identity<U>(U)`。Feng 当前允许
`spec Surface<T>` 的非泛型成员，同时暂不允许 object-form `spec` 方法自己声明
`<U>`，边界与 C++ 的“类模板可以虚分派，虚函数本身不能是模板”相近。

### 4.2 Go：限制更严格，原因同样来自接口动态关系

Go 允许泛型类型拥有使用 receiver 类型参数的方法，但不允许方法声明自己的类型
参数。Go 官方 FAQ 将主要困难归结为泛型方法与接口动态检查/分派的组合，并列出
链接期反复编译、JIT、慢速 fallback 等均不理想的实现选择；官方建议改用泛型
顶层函数，或把类型参数提升到 receiver 类型：
[Go FAQ：Why does Go not support methods with type parameters?](https://go.dev/doc/faq#generic_methods)。

Feng 的限制比 Go 小：type/fit 的方法级泛参继续允许，只禁止需要形成 object-form
spec witness requirement 的方法级泛参。

### 4.3 Rust：允许声明，但泛型方法不能进入 `dyn` 分派面

Rust trait 可以声明泛型方法；但 Rust Reference 规定，可从 trait object 动态分派
的方法不能带类型参数。带泛参的方法只有在显式成为不可动态分派成员时，才不妨碍
其他成员形成 `dyn Trait`；该方法本身不能通过 trait object 调用：
[Rust Reference：Dyn compatibility](https://doc.rust-lang.org/reference/items/traits.html#dyn-compatibility)。

Feng 当前没有把同一个 object-form `spec` 再划分为“仅泛型约束可用”和“可以形成
动态 spec 值”的两种形态，也没有 Rust `where Self: Sized` 对应的非 witness 成员
类别。当前直接在 spec 声明处拒绝方法级泛参，可保证所有合法 object-form `spec`
成员都能进入统一 witness 分派面。

### 4.4 C#：支持，但依赖 CLR 的 Generic Virtual Method 机制

C# 接口允许声明和实现泛型方法；C# 语言规范还单独规定了泛型接口方法的实现匹配：
[C# language specification §19.6.4](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/interfaces#1964-implementation-of-generic-methods)。

.NET NativeAOT/CoreCLR 的设计资料说明，Generic Virtual Method 调用需要在运行时
同时解析实现方法指针和 generic dictionary，并使用 GVM 表按对象运行时类型及方法
实例化进行查找：
[.NET runtime：Generic virtual methods](https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/botr/ilc-architecture.md#generic-virtual-methods)。

这正是 Feng 当前缺少的“双重路由”。C# 证明该能力可以实现，但其运行时与元数据
模型并不是 Feng 当前静态 witness + descriptor tree 的零增量方案，不能直接作为
Feng 本轮修复的依据。

## 5 既有基线、独立修复与本轮清理边界

现有
[方法级泛型修复文档](./feng-object-form-spec-generic-method-bugfix.md)
记录了此前尝试支持该能力时完成的部分修改。当前决定不是把未完成实现冻结在
编译器内，而是区分既有基线、独立修复和只服务该能力的新增路径：前两类保留，
第三类随 Semantic 限制一并移除。

| 部分 | 当前事实 | 处理结论 |
| --- | --- | --- |
| Parser / AST 对方法泛参的表示 | 尝试修复前已经存在，`FengCallableSignature.type_params` 已完整记录语法 | **保持既有逻辑不变**；这不是本轮新增或修复，语法合法，由 Semantic 决定当前不可用 |
| spec 方法统一进入 `resolve_callable()` | `c66d2e93` 已复用 type/fit 方法的 owner + callable 双层作用域 | **保留**；这是通用声明解析收敛，非泛型 spec 方法也继续走统一入口 |
| requirement/实现 callable 的普通签名比较、owner 闭合与 witness 选择 | 同一比较抽象也服务非泛型成员及泛型 spec owner | **保留独立正确性逻辑**；只为被禁止方法泛参增加的 arity、alpha 映射和约束分支不作为当前能力 |
| 父 spec 声明 owner 投影、generic owner 参数/返回类型替换、static/实例筛选与参数适配 | 对没有方法级泛参的合法 spec 调用也成立 | **保留独立正确性修复** |
| `SPEC_METHOD` / `SPEC_STATIC_METHOD` 调用种类 | 准确区分普通 spec witness 调用与 type/fit 直接方法调用 | **保留通用 Semantic 身份**；不得因此保留方法级泛型专用 payload 或 Codegen |
| 方法级泛型专用 resolved-callable witness payload、reifiable callable dependency、`UserSpecMember` 双层 Codegen 解析、witness 泛型 ABI、调用 emitter 与 type/fit thunk | 均由 `bc4e2e67` 为本次未完成能力新增 | **移除**；前置限制后没有合法消费者，不能以未来可能恢复为由保留不可达骨架 |

本轮清理不得整体回滚 `bc4e2e67`，以免同时删除其中已经证明对普通 spec、泛型
spec owner 或父 spec 有独立价值的修复。代码应按调用边界拆除方法级泛型专用
部分，普通 spec witness 的 ABI、行为和开销保持原状。

`owner_instance_type_ref` 对普通 spec 调用只承担声明 owner 投影及参数/返回类型
替换，不代表共享体还需要一个独立的 owner-spec aggregate descriptor。spec 调用
已经通过 receiver 或泛参描述符携带 subject witness；reifiable dependency 收集不得
再次把该 owner surface 本身加入 descriptor slots。返回类型中的真实依赖继续由
owner 替换后的返回类型收集链路处理。这样既保留泛型 spec owner 的正确类型闭合，
也不会把枚举等具体类型的发码表示误组成另一个名义 spec 实例。

实施过程中曾出现过一次“声明域泛参 index 被 caller 域 descriptor 查询误用”的
中间缺陷。尝试修复前，spec 方法泛参尚未进入 `UserSpecMember` Codegen 作用域，
不存在该 index 错位；缺陷是在新增双层 Codegen 作用域、但尚未替换旧 spec 调用
路径时产生，随后又由 generic spec 调用 emitter 消除。它不是原始编译器的独立
Bug，也不是保留未完成 emitter 的理由。未来若恢复能力，仍须遵守“不同泛参作用域
的 index 不得跨域解释”的通用不变量。

原 bugfix 文档应继续保留。它已经准确记录了：

- 尝试实现中出现过的跨泛参作用域 index 误用及其形成条件；
- 静态实现身份路径如何复用现有 callable dependency tree；
- 动态 witness 只路由函数、没有同步路由实现 descriptor 子树的根因；
- callable-form spec 当前为何不存在同一开放调用问题；
- 若未来恢复能力，必须满足的性能和跨包开放世界约束。

这些分析仍是未来恢复能力的依据，不应因当前 Semantic 限制而删除。

## 6 文档收敛范围

实施当前限制时，需要同步处理以下既有文档：

- `feng-spec.md`：把 object-form spec 方法级泛参从“必须支持”改为当前阶段
  “Semantic 必须拒绝”，作为唯一权威规则；
- `feng-generics-draft.md`：只引用 `feng-spec.md` 的规则，不重复定义；
- `feng-object-form-spec-static-dev.md`：删除或标注暂不合法的
  `static func create<U>()` 示例；
- `feng-object-form-spec-method-value-dev.md`：把泛型 spec 方法值明确标为依赖未来
  恢复的能力；
- `feng-object-form-spec-generic-method-bugfix.md`：保留为未来能力分析，停止作为当前
  实施 TODO；
- `feng-std-future.md`：`Scheduler.awaitFuture<T>` 在实现前另行调整或等待能力恢复，
  不得把无法声明的签名直接落入 std。

## 7 实施 TODO

- [x] **实际变更（主规范）**：在 `feng-spec.md` 唯一规定当前 Semantic 限制，
  `feng-generics-draft.md` 只引用；同步收敛第 6 节列出的工程与 std 规划文档。
- [x] **实际变更（Semantic）**：只在编译 object-form spec 时的成员方法签名
  统一验证入口前置检查 `type_param_count > 0`，以专用 `AE0331` 诊断拒绝；
  实例/static、无修饰（语义为 `open`）/seal 使用同一规则。不得修改 Parser、
  `resolve_callable()` 或 `AE1013` 检查。
- [x] **实际变更（代码注释）**：在上述前置检查处说明语法与 AST 合法；当前拒绝
  是因为动态 witness 尚不能同步路由实现函数与对应 descriptor 子树，防止未来
  将该检查误改为 Parser 限制或无理由删除。
- [x] **实际变更（清理未完成路径）**：移除只服务 object-form spec 方法级泛参的
  resolved-callable witness payload、reifiable callable dependency 字段与收集分支、
  `UserSpecMember` 方法泛参 Codegen 解析、witness 泛型方法 ABI、调用 emitter 和
  type/fit thunk；保留 `SPEC_METHOD` / `SPEC_STATIC_METHOD` 通用调用种类以及普通
  spec 的 owner 投影、返回类型替换和参数适配。
- [x] **实际变更（依赖收敛）**：保留普通 spec 调用的 owner instance 作为编译期
  签名替换事实，但验证其本身不进入 reifiable descriptor slots；只收集 owner
  替换后的真实返回类型依赖。
- [x] **验证（Parser / AST）**：确认实例和 static spec 泛型方法仍可解析，方法泛参、
  约束、参数和返回类型完整保留；不得新增 Parser 错误。
- [x] **验证（限制边界）**：增加编译器诊断用例，覆盖实例/static、无修饰
  （语义为 `open`）/seal，并确认 `AE0331` 在声明期产生；显式 `open` 的既有
  禁止规则保持不变。验证泛型 spec owner、type/fit 泛型方法、泛型约束及
  callable-form spec 不受影响。
- [x] **验证（既有部分）**：确认普通非泛型 spec 满足、spec seal、父 spec、fit、
  同包和跨包行为保持不变；不得删除既有测试。
- [x] **验证（回归）**：完成非文档变更后执行定向测试，并在沙箱外执行
  `make test` 全量回归。

当前限制不得通过删除 Parser 语法、删除 AST 字段、特判某个方法名、改变 type/fit
泛型方法 ABI，或改变普通 object-form spec witness 行为实现。
