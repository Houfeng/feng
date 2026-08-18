# Feng object-form `spec` 方法级泛型修复开发文档

> 状态：草案，待 Review（2026-08-18）
>
> 本文档只跟踪 object-form `spec` 方法级泛型从声明解析、满足检查到
> witness 调用的完整正确性修复。`spec seal` 的可见性与授权语义继续以
> [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 为准；本文不重新
> 定义该语义。

## 1 依据与目标

语言语义以以下权威规范为准：

- [Feng 语言 `spec` 规范](../specifications/feng-spec.md)；
- [Feng 泛型规范草案](../specifications/feng-generics-draft.md)；
- [Feng 语言函数规范](../specifications/feng-function.md)；
- [Feng 符号表规范](../specifications/feng-symbol-table.md)。

现有规范已经规定 object-form `spec` 的实例方法和静态方法支持泛型，
但当前实现不能完整处理以下合法声明：

```feng
open spec Mapper {
  func map<T>(value: T): T;
  seal static func hiddenMap<T>(value: T): T;
}

open type MapperImpl: Mapper {
  func map<U>(value: U): U {
    return value;
  }

  seal static func hiddenMap<U>(value: U): U {
    return value;
  }
}
```

本专项的目标是让 object-form `spec` 方法级泛型完整进入现有泛型 callable
模型，并覆盖：

- 实例方法与静态方法；
- 无修饰（公开）与 `seal` requirement；
- `type` 自有实现与 `fit` 方法实现；
- 非泛型 owner、泛型 `spec` owner、泛型 `type` owner 和泛型 `fit`；
- 显式类型实参、现有类型实参推导、泛型约束和返回值；
- 同包调用以及只依赖 package-public `.ft` / `.fb` 的跨包调用。

## 2 已确认的问题

### 2.1 声明解析没有进入方法泛参作用域

Parser 已经把方法级类型参数写入 `FengCallableSignature.type_params`，无需
增加语法或 AST。Semantic 解析 object-form `spec` 成员时却直接解析方法
参数和返回类型，没有复用 type/fit 方法所走的统一 `resolve_callable()`。
因此签名中的方法类型参数会被当作普通具名类型查询并报告 `AE1013`：

```feng
open spec Identity {
  func identity<T>(value: T): T;
}
```

`resolve_callable()` 已经统一完成：

- 方法泛参压栈与退栈；
- owner 泛参与方法泛参的遮蔽检查；
- 方法泛参约束解析；
- 参数和返回类型解析；
- callable 上下文恢复。

object-form `spec` 方法签名没有方法体，但仍应复用该入口；不得复制一套
只处理 `spec` 的泛参作用域逻辑。

### 2.2 满足检查没有表达方法泛参身份

当前 spec requirement 与实现方法的签名匹配只比较显式参数和返回类型，
没有先检查方法泛参 arity，也没有把双方方法泛参按声明位置建立对应关系。
这会产生两类错误：

1. 泛参名称不同但结构等价的方法可能被误判为不匹配；
2. owner 泛参与方法泛参恰好同名或类型文本相同的不同签名可能被误判为
   匹配。

方法泛参名称不是运行时或契约身份。满足检查必须先按 arity 建立位置映射，
再在该映射下比较参数、返回类型、变长参数形态和方法泛参约束。

### 2.3 object-form witness 没有方法级泛型调用面

当前 `UserSpecMember` 只记录普通参数、返回类型和 static 等事实，没有记录
方法泛参数量及其约束。object-form witness 的方法槽位也只接收：

```text
[subject] -> 显式参数 -> [out]
```

现有 type/fit 泛型方法共享体则已经使用：

```text
[subject] -> [owner descriptor] -> function descriptor
          -> 方法泛参 descriptors -> 显式参数 -> [out]
```

如果只修复声明解析，代码会继续在 spec member 类型解析、witness 槽位生成、
调用点或 thunk 转发阶段失败。因此本专项必须完整修复 witness ABI，不能只
消除 `AE1013`。

## 3 修复规则

### 3.1 声明解析

object-form `spec` 的字段继续按字段类型规则解析；每个普通方法成员统一调用
现有 `resolve_callable()`：

- 实例方法以 `allow_self = false` 解析，因为 spec 签名没有方法体；
- 静态方法使用同一路径；
- 构造器、终结器等非法 spec member 继续由既有成员种类检查拒绝；
- 不增加 spec 专用类型参数表、类型引用种类或诊断码。

无修饰和 `seal` 方法必须走完全相同的泛型声明解析；visibility 不参与类型
参数作用域建立。

### 3.2 requirement 与实现方法的泛型签名匹配

泛型方法满足检查按以下顺序执行：

1. 方法名称、实例/静态形态和现有可见性兼容规则匹配；
2. 方法泛参 arity 相同；
3. 双方方法泛参按声明位置建立一一对应，不比较泛参名称；
4. 在 spec owner 实参替换和方法泛参位置映射同时生效的环境下，比较显式
   参数数量、顺序、变长标记和类型；
5. 在同一环境下比较返回类型；
6. 检查实现方法的泛参约束能够接受 requirement 允许的全部调用。

该比较必须实现为可复用的 callable 泛型签名比较抽象，供直接 `type`、
`fit`、父 spec requirement 和 witness 选择统一调用；不得在各入口分别按
类型参数名称或字符串实现匹配。

#### 3.2.1 待 Review：方法泛参约束兼容方向

建议采用“实现不得比 requirement 更严格”的安全规则：

```text
allowed(requirement constraint) ⊆ allowed(implementation constraint)
```

具体含义：

- requirement 无约束时，实现也必须无约束；
- requirement 有约束而实现无约束时允许；
- 双方都有约束时，使用 owner 实参替换后的现有名义 spec 关系证明
  requirement 约束能够满足实现约束；
- 实现约束更窄、或无法证明上述包含关系时拒绝满足。

例如：

```feng
open spec Parent {}
open spec Child: Parent {}

open spec Contract {
  func use<T: Child>(value: T): T;
}

open type Impl: Contract {
  // 安全：所有满足 Child 的 T 都满足 Parent。
  func use<U: Parent>(value: U): U {
    return value;
  }
}
```

备选的“约束必须完全相同”规则实现更简单，但会拒绝安全的更宽实现。该决策
必须在编码前由 Review 确认，并只在权威规范中定义一次；本开发文档随后只
引用最终规则。

### 3.3 调用解析

通过 object-form spec 视角调用泛型方法时，继续复用普通泛型 callable 的
调用解析结果：

- 显式类型实参数量必须与 spec requirement 的方法泛参 arity 一致；
- 无显式类型实参时，按现有方法泛型推导规则从实参和既有目标上下文推导；
- 类型实参约束按 spec requirement 检查；满足检查已保证实际实现能够接受
  该调用，不在运行时重复检查实现约束；
- 参数 coercion、返回类型替换、变长参数和 reified dependency 收集复用
  现有 callable sidecar；
- `spec seal` 的编译期访问检查先于发码，合法调用与公开方法使用同一泛型
  witness 分派。

不允许根据 witness 中实际函数地址重新解析重载或重新推导类型实参。

### 3.4 witness 方法槽位 ABI

object-form spec 的泛型方法槽位使用现有泛型方法隐藏参数类型，固定顺序为：

```text
实例方法：subject
       -> FengFunctionDescriptor
       -> 方法泛参 FengGenericParamDescriptor（按声明顺序）
       -> 显式参数（按声明顺序）
       -> 可选 out

静态方法：FengFunctionDescriptor
       -> 方法泛参 FengGenericParamDescriptor（按声明顺序）
       -> 显式参数（按声明顺序）
       -> 可选 out
```

其中：

- owner 泛参继续由具体 subject/type/fit 的既有 owner descriptor 提供，不在
  witness slot 中重复展开；
- 方法泛参 descriptor 由调用点确定，不能收归到 owner descriptor；
- `FengFunctionDescriptor` 和方法泛参 descriptor 的构造、依赖 slot、参数
  地址 ABI 与返回 `_out` 规则直接复用现有泛型 type/fit 方法；
- witness thunk 只适配 receiver/owner 并按原顺序转发描述符和显式参数；
- 不新增运行时名称查询、动态泛型实例化、装箱、缓存或额外 witness 层。

普通非泛型 spec 方法保持现有槽位布局。runtime 库公开 ABI 不变；新增的
compiler-generated witness 槽位只服务此前不能正确发码的方法级泛型能力。

### 3.5 Codegen 成员表示

`UserSpecMember` 的方法表示需要保留至少以下声明级事实：

- 方法泛参 arity 和声明顺序；
- 泛参约束及其 owner/method 双层作用域；
- 未闭合参数与返回类型模板；
- 每个显式参数和返回值的稳定 generic callable ABI 分类；
- 对应的原始 `FengTypeMember` 身份。

开放泛型 spec 与闭合 spec 实例必须从同一原始声明槽位选择 ABI，不能按某个
具体类型实参重新决定 direct/address 表示。父 spec、intersection 展平和 slot
adapter 克隆成员时必须完整复制上述事实。

### 3.6 `.ft` 与跨包

符号表规范已经要求公开泛型成员保留方法类型参数、约束、未实例化签名骨架
和 reified dependencies。本专项首先验证当前 writer/reader 是否已经完整往返
object-form spec 泛型方法：

- 若事实完整，只修复 Semantic/Codegen 消费路径，不改 `.ft` 格式；
- 若发现字段缺失，必须先按符号表通用 callable 表示补齐 writer/reader，不能
  增加 spec 专用 attr、section 或格式旁路；
- consumer 只能依赖 package-public `.ft` 和 provider 二进制生成 witness，不能
  读取 provider 源码或 workspace-cache `.ft`。

跨包实现方法的链接可用性由
[泛型 spec 满足关系跨包修复开发文档](./feng-generic-spec-implementation-package-bugfix.md)
独立处理。本专项负责方法级泛型 witness ABI；两项修复不得互相隐藏失败。

## 4 范围边界

### 4.1 本次包含

- object-form spec 自有及父 spec 继承的方法级泛型 requirement；
- 实例、静态、公开和 seal 方法；
- type/fit 实现；
- owner 泛参和方法泛参同时出现；
- 方法泛参约束引用 owner 泛参或其他已在作用域中的方法泛参；
- 标量、managed、aggregate/object-form spec 类型实参及返回值；
- 同包、package `.fb` 和泛型约束调用。

### 4.2 本次不包含

- object-form spec 方法值；该能力由
  [object-form spec 方法值开发文档](./feng-object-form-spec-method-value-dev.md)
  独立跟踪；
- callable-form spec 对开放泛型函数/方法的反向推导；
- 新的 variance、结构满足或运行时泛型机制；
- 更改 `spec seal`、`type seal`、`@friend`、`@mixable` 的访问规则；
- 泛型 owner/fit 的 package 名义关系恢复及 seal 实现符号导出；
- 静态字段 storage/ensure 跨包链接；
- 与正确性无关的 witness 或泛型调用性能优化。

## 5 性能与兼容性

- 非泛型 spec 方法的 witness 布局和每次调用开销不变；
- 泛型 spec 方法与现有泛型 type/fit 方法一样传递函数描述符和方法泛参
  descriptors，不增加额外动态查找；
- 仍只有一次既有 witness 函数指针间接调用，不新增第二层分派；
- 不修改 runtime 库数据结构或函数 API；
- 不改变任何成员的 Feng 可见性，也不让 `.ft` 中的 seal 实现成为普通可见
  成员。

凡实施中发现需要增加运行时查找、缓存、额外间接调用或改变非泛型 ABI，必须
暂停并由人工决策。

## 6 测试要求

### 6.1 编译器测试

- Parser：验证 object-form spec 实例/静态泛型方法 AST 已完整保留，无需新语法；
- Semantic：公开/seal、实例/static、显式/推导类型实参、约束、泛参改名、arity
  不匹配、约束方向不兼容；
- 满足选择：type、fit、父 spec、泛型 owner + 方法泛型双层替换；
- Symbol：package-public `.ft` 对方法泛参、约束、签名和 reified dependencies
  完整 round-trip；
- Codegen：标量、managed、aggregate 参数与返回值，实例/static witness thunk，
  type/fit 实现，以及生成 C 可编译；
- CLI：provider 先打包 `.fb`，consumer 只通过包导入并执行泛型 spec 方法。

### 6.2 FCTS

必须在 `fcts_lib -> fcts_bin` 增加语言行为覆盖：

- 公开实例方法与 seal 实例方法；
- 公开静态方法与 seal 静态方法；
- type 与 fit 实现；
- 泛型 spec owner 与非泛型 spec owner；
- 方法泛参名称不同但位置等价；
- 约束、显式类型实参、推导及 aggregate/object-form spec 实参；
- 返回值和副作用均可观察，避免只验证“能够编译”。

object-form spec 方法值不纳入本专项 FCTS。

## 7 TODO 与实施顺序

- [ ] **实际变更（规范）**：Review 并把方法泛参约束兼容方向写入
  `feng-spec.md` / `feng-generics-draft.md` 的唯一权威位置；关联文档只引用。
- [ ] **实际变更（Semantic 声明）**：让 object-form spec 普通方法统一复用
  `resolve_callable()`，删除手写参数/返回类型解析分支。
- [ ] **实际变更（Semantic 满足）**：实现按位置 alpha-equivalent 的泛型 callable
  签名比较，统一覆盖 type、fit、父 spec 和 witness 选择。
- [ ] **实际变更（Semantic 调用）**：确保 spec 视角泛型方法调用记录完整 callable
  类型实参、约束 witness 与参数/返回 coercion 事实。
- [ ] **实际变更（Codegen 表示）**：扩展 `UserSpecMember` 及其克隆/实例化路径，保存
  方法泛参和双层泛型签名模板。
- [ ] **实际变更（Codegen witness）**：为实例/static 泛型方法槽位、调用点和
  type/fit thunk 复用现有函数描述符、方法泛参 descriptors、参数地址 ABI 与 `_out`
  规则。
- [ ] **验证（Symbol）**：确认 `.ft` 已完整 round-trip；只有验证发现通用 callable
  事实确实缺失时，才把 writer/reader 补齐列为实际变更，不增加 spec 特判格式。
- [ ] **验证（编译器用例）**：补齐 Parser、Semantic、Symbol、Codegen 和隔离 `.fb`
  CLI 正负用例。
- [ ] **验证（FCTS）**：补齐跨包可观察行为用例并注册执行。
- [ ] **验证（回归）**：执行定向测试后，在沙箱外执行 `make test` 全量回归。

实施中若无法在既有泛型 callable ABI 上表达 spec witness 方法槽位，或约束兼容
需要引入运行时判断，必须暂停并由人工决策，不得增加专用特判。

