# Feng object-form spec 子类型泛型父约束 Bugfix 开发设计

> 状态：已完成
>
> 本文只修复 object-form `spec` 子类型作为父 `spec` 泛型约束实参时被错误拒绝的问题，
> 不包含 object-form `spec` 成员访问发码优化，也不改变 runtime ABI。

## 1. 问题

Feng 已支持沿显式声明的 object-form `spec` 父关系建立父视角：

```feng
spec Parent {
  func value(): int;
}

spec Child: Parent {}
```

`Child` 值可以进入 `Parent` 的普通参数、赋值、返回和显式转换位置。该规则由
[Feng `spec` 主规范](../specifications/feng-spec.md) 定义，object-form `spec` 向上转换的
既有实现设计见
[feng-object-spec-upcasting-dev.md](./feng-object-spec-upcasting-dev.md)。

但是，同一名义父关系目前没有进入泛型类型实参的约束满足判定：

```feng
func accept<T: Parent>(value: T): void {}

func inferred(child: Child): void {
  accept(child);          // 当前错误：AE0512
}

func explicit(child: Child): void {
  accept<Child>(child);   // 当前错误：AE0512
}
```

两次调用都应合法：隐式调用应推导 `T = Child`，显式调用已经指定 `T = Child`；
`Child` 通过声明的名义父关系满足 `Parent` 约束。

当前编译器会在候选适用性检查中错误排除 `accept`，最终报告顶层函数没有可接受该
实参的重载。2026-08-17 使用最小复现分别验证了隐式与显式类型实参路径，二者均报告
`AE0512`。

## 2. 正确语义

本修复不新增语言规则，只让泛型约束检查遵守已经存在的 object-form `spec` 名义父关系。

对于：

```feng
func identity<T: Parent>(value: T): T {
  return value;
}

let child: Child = ...;
let result: Child = identity(child);
```

必须同时满足：

1. 泛型类型参数保持为 `T = Child`；
2. `Child` 只在验证和使用 `Parent` 约束成员时投影到 `Parent` 契约视角；
3. 参数和返回值仍使用 `Child` 的值表示，不能把本次实例化改写为 `identity<Parent>`；
4. 父视角必须来自 `Child: Parent` 的编译期名义父路径，不能根据运行时 subject 类型搜索；
5. 无关 spec、父到子方向以及未声明的结构相似关系继续拒绝。

因此，本修复处理的是“`Child` 是否满足 `T: Parent`”的约束关系，不是在调用点先把
实参值整体转换成 `Parent`，也不改变泛型函数中 `T` 的类型身份。

### 2.1 泛型父实例

父关系中的类型实参替换继续复用既有 object-form `spec` 向上转换规则：

```feng
spec Parent<T> {}
spec Child<T>: Parent<T> {}

func accept<T: Parent<int>>(value: T): void {}

func ok(value: Child<int>): void {
  accept(value);                 // 合法，T = Child<int>
}
```

显式映射的父实例也按声明替换后的完整身份判断：

```feng
type Box<T> {}

spec Parent<T> {}
spec MappedChild<T>: Parent<Box<T>> {}

func accept<T: Parent<Box<int>>>(value: T): void {}

func ok(value: MappedChild<int>): void {
  accept(value);                 // 合法，T = MappedChild<int>
}
```

不得由此引入 variance。例如 `Parent<Child>` 不能仅因 `Child: Base` 自动满足
`Parent<Base>`。

## 3. 当前根因

语义分析使用 `generic_type_arg_satisfies_constraint` 统一过滤泛型候选。当前流程是：

1. 对约束完成类型参数替换；
2. 类型实参与约束类型引用完全相同时直接通过；
3. 外层泛型参数转发保持既有延迟判定；
4. 其余情况调用普通的 `type_ref_satisfies_spec_type_ref`。

第 4 步主要处理具体 `type`、枚举、builtin/fit 等主体对 object-form `spec` 的满足关系。
当实际类型引用本身是另一个 object-form `spec`（如 `Child`）时，它不会查询已经存在的
`Child -> Parent` 名义向上转换路径，因此返回不满足。

仓库已经有统一的 `find_object_spec_upcast_path`，负责：

- 直接父、传递祖先、多父与确定路径选择；
- 泛型父 spec 的类型实参替换；
- 显式映射父实例的精确身份判断；
- 排除无关 spec、父到子和 variance。

普通 expected-type coercion 和显式 cast 已复用该查询；泛型约束检查遗漏了这条路径，
是本 Bug 的直接原因。

### 3.1 既有测试缺口

当前相邻用例已经覆盖：

- `Child` spec 值进入普通 `Parent` expected-type 位置；
- 外层 `U: Child` 泛型参数转发给内层 `T: Parent`；
- 实现 `Child` 的具体 type 作为泛型实参。

但没有用 object-form `Child` spec 值自身触发 `T = Child`，再验证它满足 `T: Parent`。
因此既有用例没有经过本次失败的约束判定分支。

## 4. 修复方案

### 4.1 Semantic

在泛型类型实参与 object-form `spec` 约束的统一判定中，按以下顺序处理：

1. 保留现有的类型引用完全相等判定；
2. 当实际类型引用和约束均为 object-form `spec` 时，调用既有
   `find_object_spec_upcast_path`；
3. 找到名义父路径则认为约束满足，并立即释放查询产生的临时路径；
4. 未找到父路径时，继续进入现有具体类型/fit/builtin 满足判定；
5. 其余既有约束形态和诊断流程保持不变。

该逻辑应收敛在泛型约束的公共 predicate 中，使以下入口得到同一结果：

- 隐式类型实参推导后的候选过滤；
- 显式泛型类型实参检查；
- 泛型函数、泛型方法和 callable 相关的约束检查；
- 跨泛型调用的约束转发。

不得在各调用入口分别增加 `Child`/`Parent` 名称或 AST 形态特判。

泛型类型实参检查只记录“关系成立”，不为实参表达式记录普通 object-spec coercion
sidecar。泛型实参仍按 `T = Child` 传递；constraint witness 由泛型发码路径负责。

### 4.2 Codegen 与 witness

当前 `cg_generic_descriptor_expr` 在实际泛型类型实参是 object-form `spec` 时，会调用
`cg_ensure_spec_slot_witness(source_spec, constraint_spec)`，为 spec fat value 和约束 witness
ABI 建立 adapter。对于 `T = Child`、约束为 `Parent`，source 为 `Child`，target 为
`Parent`。

本 Bugfix 应继续使用该既有机制：

- 泛型参数和返回值保持 `Child` fat value；
- adapter 从当前 `Child` 值取得其动态 subject/witness；
- `Parent` 约束成员通过目标约束 surface 分派；
- 不在运行时搜索父关系；
- 不新增 runtime API、runtime 字段或通用运行时类型判断。

实现时必须通过 codegen 和执行用例验证该既有 adapter 对 direct/transitive parent、实例
字段及实例方法的实际行为。若验证发现 adapter 自身存在阻断本语义的缺陷，应在本文
范围内按既有 witness 抽象修复；不得用默认 witness、运行时候选搜索或把 `T` 改写成
`Parent` 绕过。

本文件不改变 getter/setter thunk、字段 offset、subject/witness 提升或 slot witness 的
性能方案；这些属于独立发码优化。

## 5. 兼容性与范围

### 5.1 本次包含

- object-form 子 spec 作为直接或传递父 spec 约束的泛型类型实参；
- 隐式推导和显式泛型类型实参；
- 泛型父实例和声明中显式映射的父实例；
- 泛型函数、方法及既有公共约束检查入口；
- 对应 semantic、codegen 和 FCTS 回归。

### 5.2 本次不包含

- 无名义父关系 object-form spec 之间的结构满足；
- 父 spec 到子 spec 的向下约束满足；
- 泛型实例 variance；
- callable-form、union-form 或 intersection-form 约束规则扩展；
- 重载优先级、重叠声明或诊断码重设计；
- object-form spec 值布局或 runtime ABI 变更；
- object-form spec 成员访问发码优化。
- object-form spec 作为泛型实参时静态约束成员的既有行为调整。

### 5.3 诊断兼容性

合法的 `Child -> Parent` 候选不再被错误过滤。无关 spec、错误泛型实参映射和反向关系
仍走现有约束失败/无适用重载诊断。

当修复使一个此前被错误排除的候选重新进入重载集合时，继续使用现有重载选择与歧义
规则；本次不新增优先级或消歧规则。

## 6. 测试设计

### 6.1 Semantic

新增独立测试，至少覆盖：

1. `test(child)` 隐式推导 `T = Child`，满足直接父约束；
2. `test<Child>(child)` 显式指定 `T = Child`，满足直接父约束；
3. 泛型函数返回 `T`，返回值仍可绑定为 `Child`，防止错误改写为 `Parent`；
4. `Child -> Middle -> Parent` 传递祖先约束；
5. `Child<int> -> Parent<int>`；
6. `MappedChild<int> -> Parent<Box<int>>`；
7. 无关 spec、`Parent -> Child` 和错误泛型父实例继续拒绝；
8. 已有具体 type、fit/builtin 和外层泛型约束转发结果不退化。

不得修改已有测试表达的既有预期；新增测试单独覆盖本 Bug。

### 6.2 Codegen

新增 focused codegen 用例，验证：

1. `Child` spec fat value 作为 `T: Parent` 的隐式和显式泛型实参均可发码；
2. 生成 constraint descriptor/slot witness 使用 `Child -> Parent` 适配，而不是默认 witness；
3. 泛型共享体可读取/写入父 spec 字段并调用父 spec 方法；
4. 泛型函数返回的 `T` 仍按 `Child` fat value ABI 返回；
5. 泛型父实例的替换身份出现在正确 witness surface 中。

### 6.3 FCTS

新增语言行为用例，使用一个实现 `Child` 的真实 type 构造 `Child` spec 值，验证：

- 隐式与显式泛型调用均执行成功；
- 父字段读写和父方法调用作用于原 subject；
- 返回值仍可作为 `Child` 使用；
- direct parent 与至少一个 transitive/generic parent 场景正确。

完成 focused 测试后，按仓库规则在沙箱外执行全量回归：

```sh
make test
```

## 7. 实施 TODO

- [x] 1. 在 generic constraint 公共判定中接入既有 object-spec 名义父路径查询。
- [x] 2. 确认隐式推导、显式类型实参及 callable/方法入口统一复用修复后的 predicate。
- [x] 3. 验证 constraint witness 继续保持 `T = Child`，并使用 `Child -> Parent` slot witness adapter。
- [x] 4. 验证既有 adapter 可完成实例字段及实例方法的父约束分派，无需修改 codegen 或 runtime。
- [x] 5. 新增 semantic 测试，覆盖成功、类型身份保持及反向/无关/错误实例拒绝。
- [x] 6. 新增 codegen 测试，覆盖实例字段、实例方法、返回 `T` 和泛型父实例。
- [x] 7. 新增 FCTS 行为测试，覆盖真实动态 subject 的父约束调用。
- [x] 8. 运行 focused 测试。
- [x] 9. 在沙箱外运行 `make test` 全量回归。

## 8. 完成标准

满足以下条件后，本 Bugfix 才算完成：

1. `test(child)` 与 `test<Child>(child)` 均通过，且 `T` 保持为 `Child`；
2. direct、transitive 和泛型父实例约束行为与普通 object-spec 向上转换一致；
3. 无关、反向和 variance 场景继续拒绝；
4. constraint witness 正确访问原 subject 的父契约成员，不使用默认 witness 或运行时搜索；
5. 未改变 runtime ABI、object-form spec 值布局和独立发码优化方案；
6. focused 测试与 `make test` 全量回归全部通过。

## 9. 实施结果

本次只修改 generic constraint 的公共语义判定：当实际类型实参是 object-form 子
`spec`、约束是其名义父 `spec` 时，复用 `find_object_spec_upcast_path` 判断父关系。
泛型类型实参始终保持为子 `spec`，未在调用点把参数改写或向上转换为父 `spec`。

既有 slot witness adapter 已能为该类型实参建立父约束 surface，实例字段读写、实例方法
调用及 `T` 返回链均通过发码和执行验证，因此没有修改 codegen、runtime ABI 或
object-form `spec` 值布局。object-form `spec` 成员访问发码优化继续作为独立工作暂停。

验证结果：focused semantic、codegen、FCTS 均通过；沙箱外 `make test` 的 sanitizer 与
normal 全量回归均通过，FCTS 为 `787/787`。
