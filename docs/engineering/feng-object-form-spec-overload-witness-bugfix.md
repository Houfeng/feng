# Feng object-form `spec` 方法重载修复

状态：已完成。

本文档只整理 object-form `spec` 的实例方法、静态方法及父 `spec` 成员叠加后的重载问题。语言规则以 [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 和 [Feng 语言函数规范](../specifications/feng-function.md) 为准；本文档不重复定义新的重载规则。

## 1. 目标

object-form `spec` 用于约束 `type` 的实例能力与静态能力。它的方法必须与 `type` 方法使用相同的重载及冲突规则。

`spec ChildSpec: ParentSpec { ... }` 表示把 `ParentSpec` 的契约成员叠加到 `ChildSpec`，不是 OOP 实现继承。叠加后的方法按一个完整成员面检查：

- 参数列表不同且不重叠的方法保留为重载；
- 同名、同参数但返回类型不同的方法冲突；
- 变长参数冲突与可见具体 `type` 导致的参数重叠，按普通方法规则拒绝；子、父 `spec` 参数关系本身不构成冲突；
- 实例方法与静态方法属于不同的重载集合；
- 子 `spec` 与父 `spec` 中完整签名相同的方法合并为一个 requirement，并以子 `spec` 声明为代表；这只是叠加去重，不是 OOP 覆盖；
- 同一个 `spec` 体内重复声明完整签名相同的方法仍是重复声明错误。

## 2. 强制约束

- [x] 不增加任何运行时分支、成员查找、签名比较、候选回退、分配或间接调用。
- [x] 不变更 runtime ABI、Feng/C ABI 或现有已正确支持场景的 witness/descriptor 布局。
- [x] 保持已有主槽的字段名、顺序和偏移；新增的合法重载只使用现有确定性重载槽命名并追加，不重排已有槽。
- [x] 当前被错误丢弃的每个合法 requirement 仍只补一个普通 witness 槽，不增加辅助槽或额外分派层。object-form witness 不是公开 ABI；如果实现中发现必须修改已定义 ABI，立即停止，由人工决策。
- [x] 不修改已有测试用例，只新增测试。
- [x] 重载检查只发生在编译期。
- [x] 如果完整修复必须违反以上任一约束，立即停止，由人工决策。
- [x] 如果发现与本专项无关的编译器问题，先记录到“实施过程问题记录”，分析清楚后再决定是否继续；不确定时停止，由人工决策。

## 3. 修复前实现事实

以下结论基于修复前提交 `2af76f26` 的源码重新构建 `build/bin/feng` 后确认，不使用撤销前遗留的旧二进制。

### 3.1 `ChildSpec: ParentSpec` 修复前如何处理

使用以下合法程序可以看清当前各阶段的差异：

```feng
spec ParentSpec {
  func select(value: int): int;
}

spec ChildSpec: ParentSpec {
  func select(value: string): string;
}

type Choice: ChildSpec {
  func select(value: int): int { return value + 1; }
  func select(value: string): string { return value; }
}

func selectNumber(value: ChildSpec): int {
  return value.select(1);
}

func selectText(value: ChildSpec): string {
  return value.select("ok");
}
```

当前实现情况如下：

| 阶段 | 当前行为 |
| --- | --- |
| Parser / AST / `.ft` 导出 | 保留父 `spec` 引用和全部方法声明，没有按方法名丢弃声明。 |
| 满足检查 | 先收集父 `spec`，再处理子 `spec`；分别检查 `Choice` 是否实现父、子声明的两个 `select`。两个 requirement 都会检查。 |
| 实例调用语义解析 | 扫描父子闭包中的全部同名实例方法，能够按实参选择两个 `select`。 |
| 静态调用与方法值语义解析 | 使用另一条子优先的 requirement 遍历，也能够看到同名的不同签名。 |
| Semantic witness | 父优先遍历，但使用单纯的方法名去重；记录父级 `select(int)` 后会丢弃子级 `select(string)`。 |
| object-form Codegen 成员注册 | 先处理父成员；只要子 `spec` 存在同名成员，就按名字用子成员替代父槽，不检查签名；同名的后续成员继续被丢弃。 |

因此，问题不是 `spec` 定义或父关系丢失，而是 Semantic witness 与 object-form Codegen 把“同名”错误地当成“同一个 requirement”。上述程序当前在 Codegen 阶段报错：

```text
CE0048: codegen: default subject parent witness member mismatch
```

### 3.2 修复前各层规则不一致

修复前至少存在三套不同规则：

1. 实例调用大体按父到子的完整方法集合选择重载；
2. 静态调用和方法值按子到父的完整方法集合选择重载；
3. witness 与 object-form Codegen 按方法名去重。

合法程序可能在 Semantic 阶段通过，却在 Codegen 或生成 C 编译阶段失败；静态方法与实例方法同名时，还可能编译成功但进入错误的 witness 槽。

### 3.3 可以复用的现有能力

- 普通 `type` 已有重复签名、仅返回类型不同、变长参数冲突和契约关系重叠检查。
- Semantic 已能为直接调用和方法值保留选中的精确 requirement 声明。
- intersection-form Codegen 已有按完整签名判断等价 requirement、记录声明 alias、保留合法重载槽的能力。
- object-form `spec` 的 AST 和 `.ft` 导出已经保留全部声明，不需要修改语法或符号文件格式。

本次应抽取并复用这些通用能力，不再增加一套只服务 object-form `spec` 的特殊重载规则。

## 4. 修复问题与独立交付项

### OSM01：`spec` 自有方法没有执行普通重复与冲突检查

当前以下三个声明都被 Semantic 接受，并可完成编译；对应的 `type` 声明会分别被拒绝。

重复签名：

```feng
spec Invalid {
  func select(value: int): int;
  func select(value: int): int; // 应报 AE0508
}
```

仅返回类型不同：

```feng
spec Invalid {
  func select(value: int): int;
  func select(value: int): string; // 应报 AE0509
}
```

变长参数冲突：

```feng
spec Invalid {
  func select(value: int): int;
  func select(values: int...): int; // 应报 AE0510
}
```

静态方法存在同样问题：

```feng
spec InvalidFactory {
  static func create(seed: int): int;
  static func create(seed: int): int; // 应报 AE0508
}
```

交付任务：

- [x] OSM01-修复：让 object-form `spec` 自有实例方法与静态方法复用普通 `type` 的同一套检查核心。
- [x] OSM01-验证：在 `test/semantic/` 新增实例、静态、重复签名、返回类型冲突和变长参数冲突用例。
- [x] OSM01-交付：诊断发生在 Semantic；Codegen 不再接收这些非法声明。

### OSM02：完整契约成员面没有统一执行重载冲突检查

普通 `type` 的现行规则是：仅有子、父 `spec` 参数关系时允许重载；如果当前分析中存在可同时满足二者的具体 `type`，则两个候选构成重叠并报 `AE0706`。object-form `spec` 必须完全一致。

以下程序合法，不能仅因 `Child: Parent` 而拒绝：

```feng
spec Parent {}
spec Child: Parent {}

spec Valid {
  func select(value: Parent): int;
  func select(value: Child): int;
}
```

加入可同时满足两个参数的具体 `type` 后，以下程序应报 `AE0706`；当前 object-form `spec` 没有执行该检查而被错误接受：

```feng
spec Parent {}
spec Child: Parent {}

type Value: Child {}

spec Invalid {
  func select(value: Parent): int;
  func select(value: Child): int; // 应报 AE0706
}
```

父成员叠加到子 `spec` 后，也必须统一检查。当前程序通过 Semantic，直到 Codegen 才报 `CE0048`：

```feng
spec ParentSpec {
  func select(value: int): int;
}

spec Invalid: ParentSpec {
  func select(value: int): string; // 应在 Semantic 报 AE0509
}
```

泛型父 `spec` 必须先替换 owner 类型参数，再比较闭合签名：

```feng
spec ParentSpec<T> {
  func select(value: T): int;
}

spec Invalid: ParentSpec<int> {
  func select(value: int): string; // ParentSpec<int> 闭合后冲突
}
```

静态方法使用同一规则：

```feng
spec ParentFactory {
  static func create(seed: int): int;
}

spec InvalidFactory: ParentFactory {
  static func create(seed: int): string; // 应在 Semantic 报 AE0509
}
```

交付任务：

- [x] OSM02-修复：构建已完成泛型替换的完整实例方法面和静态方法面，并调用与 `type` 相同的重载检查核心。
- [x] OSM02-验证：新增无共同具体 type 时允许子/父 spec 参数重载、有共同具体 type 时报 AE0706，以及父子返回冲突、泛型父闭合冲突和静态对应场景。
- [x] OSM02-交付：所有非法契约在 Semantic 阶段给出 AE 诊断，不再退化成 CE 或生成 C 错误。

### OSM03：同一个 `spec` 内的合法重载在 witness / Codegen 中丢失

实例方法示例：

```feng
spec Selector {
  func select(value: int): int;
  func select(value: string): string;
}

type Choice: Selector {
  func select(value: int): int { return value + 1; }
  func select(value: string): string { return value; }
}

func selectText(value: Selector): string {
  return value.select("ok");
}
```

当前 Semantic 正确选择 `select(string)`，但 witness 只生成第一个 `select(int)` 槽；生成 C 因把 `string` 传给 `int` 槽而编译失败。

静态方法示例：

```feng
spec Factory {
  static func create(seed: int): int;
  static func create(seed: string): string;
}

type Choice: Factory {
  static func create(seed: int): int { return seed + 1; }
  static func create(seed: string): string { return seed; }
}

func createText<T: Factory>(): string {
  return T.create("ok");
}
```

当前也只生成第一个 `create(int)` 槽，生成 C 编译失败。

交付任务：

- [x] OSM03-修复：witness 和 Codegen 用完整方法身份区分合法重载，不再按名称去重。
- [x] OSM03-验证：新增实例/静态直接调用、spec 方法值、受 spec 约束泛型值方法值与静态方法值用例。
- [x] OSM03-交付：Semantic 选中的 requirement 与 Codegen 使用的槽完全一致；运行时不做重载选择。

### OSM04：父子及多个父 `spec` 叠加时丢失合法重载

子 `spec` 新增父方法的同名重载：

```feng
spec NumberSelector {
  func select(value: int): int;
}

spec Selector: NumberSelector {
  func select(value: string): string;
}
```

多个父 `spec` 提供同名重载：

```feng
spec NumberSelector {
  func select(value: int): int;
}

spec TextSelector {
  func select(value: string): string;
}

spec Selector: NumberSelector, TextSelector {}
```

两种程序当前都通过 Semantic 的声明及调用解析，但 Codegen 报 `CE0048`。静态方法对应程序也有相同结果：

```feng
spec NumberFactory {
  static func create(seed: int): int;
}

spec Factory: NumberFactory {
  static func create(seed: string): string;
}
```

交付任务：

- [x] OSM04-修复：父成员按完整签名叠加；同名不同签名全部保留，同名等价 requirement 才合并。
- [x] OSM04-验证：新增单父、多父、传递父、泛型父，以及实例/静态对应场景。
- [x] OSM04-交付：父视角、子视角、默认 witness、具体 type witness 与 fit witness 使用一致的槽映射。

### OSM05：实例方法与静态方法被错误放入同一个名称槽

普通 `type` 允许实例方法与静态方法同名，因为二者的访问面不同。object-form `spec` 应保持相同规则：

```feng
spec Surface {
  func value(): int;
  static func value(): string;
}

type Choice: Surface {
  func value(): int { return 1; }
  static func value(): string { return "ok"; }
}

func instanceValue(value: Surface): int {
  return value.value();
}

func staticValue<T: Surface>(): string {
  return T.value();
}
```

当前程序可以编译成功，但生成的 witness 只有实例 `value` 槽；`T.value()` 被错误生成为对实例槽的调用，并把类型 descriptor 当作 `_subject` 传入。这是静默错误分派，不是正常重载行为。

交付任务：

- [x] OSM05-修复：把 `is_static` 纳入 requirement/槽身份；实例面与静态面分别收集、检查和映射。
- [x] OSM05-验证：新增同名同签名、同名不同签名的实例/静态组合，并执行正向 FCTS 行为验证。
- [x] OSM05-交付：静态调用不携带 subject，实例调用继续只使用现有 subject + witness 路径。

### OSM06：等价父子 requirement 的代表身份不统一

以下声明合法，父子完整签名相同，应合并为一个 requirement，并以子声明为代表：

```feng
spec ParentSpec {
  func select(value: int): int;
}

spec ChildSpec: ParentSpec {
  func select(value: int): int;
}
```

该最小程序当前可以完成编译，但不同阶段保留的声明身份不同：实例调用和 Semantic witness 偏向父声明，静态调用/方法值偏向子声明，Codegen 又按名称换成子声明。当前成功依赖签名恰好相同，不能作为稳定实现。

交付任务：

- [x] OSM06-修复：Semantic 统一产生一个子优先的逻辑 requirement；Codegen 槽为父、子等价声明记录 alias，不按名称重新选择。
- [x] OSM06-验证：新增直接父子、传递父、菱形父图、泛型闭合后等价，以及实例/静态对应场景。
- [x] OSM06-交付：调用、方法值、满足选择、witness 与 Codegen 都能由精确声明身份定位同一个逻辑槽。

## 5. 实施方案

### 5.1 规范先行

- [x] 在主 `spec` 规范中把本专项涉及的“继承/覆盖”措辞收敛为“父契约成员叠加”。
- [x] 明确“子 `spec` 优先”只适用于父子叠加产生的等价 requirement 去重。
- [x] 明确叠加后的实例方法面与静态方法面分别使用普通 `type` 的完整重载规则。
- [x] 更新 AE 错误码文档，使 AE0508、AE0509、AE0510、AE0706 的 object-form `spec` 诊断模板有正式记录。

### 5.2 Semantic

- [x] 抽取可同时服务 `type` 与 object-form `spec` 的方法重载检查核心；规则只维护一份。
- [x] 为每个 spec 方法保留：原声明 member、原声明 spec、闭合后的 owner spec 引用、实例/静态标志和闭合签名。
- [x] 子成员先进入逻辑成员面，再叠加父成员；只合并语义等价 requirement。
- [x] 对完整逻辑成员面执行返回类型、变长参数及参数重叠检查。
- [x] witness 按逻辑 requirement 收集实现，不再使用 `seen_names`。

### 5.3 Codegen

- [x] 将现有 signature-level 等价判断、声明 alias 和确定性重载槽能力复用于 object-form `spec`。
- [x] 保持现有主槽的名字与顺序；额外合法重载追加到末尾。
- [x] 父子等价声明映射到同一槽；同名不同签名映射到不同槽。
- [x] 实例与静态同名成员映射到不同槽。
- [x] 所有调用和方法值只消费 Semantic 已选定的 requirement 身份，不在 Codegen 按名称重新选择。
- [x] 不增加 descriptor 字段、运行时查找或额外适配调用层。

## 6. 测试与验收

### 6.1 Semantic 负向测试

- [x] object-form `spec` 自有实例/静态重复签名：AE0508。
- [x] object-form `spec` 自有及父子叠加的仅返回类型冲突：AE0509。
- [x] object-form `spec` 自有及父子叠加的变长参数冲突：AE0510。
- [x] 契约关系造成的参数重叠：AE0706。
- [x] 泛型父 `spec` 在 owner 实参替换后形成的上述冲突。

### 6.2 Semantic / Codegen 正向测试

- [x] 同一 spec 内实例方法与静态方法的合法重载全部保留。
- [x] 子 spec 新增父方法的同名重载全部保留。
- [x] 多父、传递父及泛型父叠加后的合法重载全部保留。
- [x] 父子等价 requirement 子优先去重，所有声明身份可定位同一槽。
- [x] 实例方法与静态方法同名时分别定位正确槽。
- [x] `type` 自有实现和 `fit` 实现都能形成正确 witness。
- [x] 既有 object-form 与 intersection-form 方法值、静态方法值和直接调用不回归。
- [x] 跨包 `.ft` 往返后保留相同重载集合与精确 requirement 映射。

### 6.3 FCTS 行为验证

- [x] spec 参数/局部值的实例重载直接调用。
- [x] 受 spec 约束泛型值的实例重载直接调用。
- [x] spec 实例方法值及受约束泛型值实例方法值。
- [x] 受 spec 约束类型参数的静态重载直接调用与静态方法值。
- [x] 父子、多父、泛型父、`type` 实现与 `fit` 实现。
- [x] 同名实例/静态方法均执行到正确实现。

### 6.4 回归与性能验收

- [x] 新增专项测试全部通过。
- [x] `./build/bin/feng run fcts/fcts_bin` 通过。
- [x] 在 Codex 沙箱外运行全量 `make test` 并通过。
- [x] 对生成 C 做结构检查：没有新增运行时分支、查找、分配、descriptor 字段或调用转发层。
- [x] 对已有合法非重载 spec 的生成 C 做对比：主槽名称、顺序和布局不变。

## 7. 实施顺序

- [x] 第一步：完成 §5.1 主规范修订并 Review。
- [x] 第二步：完成 OSM01，单独修复、验证、交付。
- [x] 第三步：完成 OSM02，单独修复、验证、交付。
- [x] 第四步：完成 OSM03，单独修复、验证、交付。
- [x] 第五步：完成 OSM04，单独修复、验证、交付。
- [x] 第六步：完成 OSM05，单独修复、验证、交付。
- [x] 第七步：完成 OSM06，单独修复、验证、交付。
- [x] 第八步：完成全部 FCTS、跨包和全量回归验证。

## 8. 实施过程问题记录

实施过程中如发现问题，必须先记录复现代码、当前结果、预期结果和影响范围，再分析和处理；不确定时停止，由人工决策。

### P01：原修复假设与现行 type/顶层函数重载规则不一致

现有用例
`test_object_spec_upcast_preserves_current_overload_priority` 接受以下程序：

```feng
spec Parent {}
spec Child: Parent {}

func choose(value: Child): int { return 1; }
func choose(value: Parent): int { return 2; }

func exact(value: Child): int {
  return choose(value); // 现有用例要求选择 Child 重载
}
```

事实核对确认，当前顶层函数和 `type` 成员方法使用同一个可见契约重叠核心：

- 只有子、父 `spec` 参数关系而没有共同的可见具体 `type` 时，声明合法，调用按精确匹配优先；
- 当前分析中存在同时满足两个参数 spec 的可见具体 `type` 时，声明报 `AE0706`。

原实施曾把子到父的 object-form coercion 本身加入公共重叠判断，导致上述既有合法用例回归失败，也会同时改变顶层函数和 `type` 成员方法行为。该方向错误。

人工结论：object-form `spec` 成员方法与静态方法必须和当前 `type` 成员方法一致。保留既有用例，不增加直接父子关系特判；更新函数主规范，使其准确描述现行规则，并为 object-form `spec` 新增“无共同具体 type 时允许”和“存在共同具体 type 时拒绝”两类用例。

状态：已分析并解决。

### P02：等价泛型父/子 requirement 的既有槽 ABI 被错误保留为父 ABI

现有 MV07 Codegen 用例包含：

```feng
spec RootFactory<T> {
  static func duplicate(value: T): T;
}

spec NumberFactory: RootFactory<i32> {
  static func duplicate(value: i32): i32;
}
```

父 requirement 闭合后与子 requirement 语义等价。修复前的 object-form 注册会保留原有 `duplicate` 槽位置和字段名，但用子声明替换槽内容，因此 `NumberFactory` 视角使用具体 `i32 -> i32` ABI。

初版 OSM06 实现只把槽的主声明身份改成子声明，仍保留父泛型 requirement 的 erased ABI，导致生成调用由：

```c
_witness->duplicate(_arg0)
```

变成：

```c
_witness->duplicate((const void *)&_arg0, &_result)
```

程序仍能生成并编译，但改变了既有正确场景的 witness 槽 ABI，违反本专项“不变更 ABI”的强制约束。

分析结论：等价子 requirement 成为代表时，必须用子 requirement 的完整编译期槽描述替换父槽，同时保留原槽字段名、位置以及父/子全部声明 alias。多个父 requirement 之间仍保持首个等价槽，不改变 intersection 既有的 first-seen ABI 规则。该修复只调整编译期槽注册，不增加运行时字段、分支、分配或调用层。

状态：已分析并解决。

### P03：受 object-form spec 约束的泛型值实例调用仍按名称选择首个重载

FCTS 新增以下泛型 receiver 场景后，Semantic 仍把 `bool` 与 `string` 调用解析成第一个 `int` requirement：

```feng
spec Surface {
  func select(value: int): int;
  func select(value: string): string;
  func select(value: bool): bool;
}

func call<T: Surface>(value: T): string {
  let flag = value.select(true);      // 当前错误推断为 int
  return value.select("generic");    // 当前错误选择 int requirement
}
```

当前诊断包括：

```text
AE0030: binary operator '&&' requires bool operands, got 'bool' and 'i64'
AE1003: expression 'value.select' does not match expected type 'string'
```

预期：泛型值实例调用与普通 spec 值实例调用使用同一个完整 requirement 面，根据实参选择 `bool` / `string` 重载，并保留 `T` receiver 语义与精确 requirement 身份。

影响范围：OSM03/OSM04 的受约束泛型值直接调用；普通 spec 值、泛型静态调用和方法值已有独立覆盖，不能用修改 FCTS 绕过。

分析结论：受约束泛型值的实例直接调用仍使用旧的“按名称取第一个成员”路径，没有进入已用于普通 spec 值的完整实参重载决议。修复为共用同一套 requirement 遍历和实参匹配；只改变编译期决议，不改变生成 ABI 或运行时路径。

状态：已分析并解决。

### P04：fit 的实例/静态同名方法生成相同 C 符号

OSM05 的 fit 行为用例包含：

```feng
spec Surface {
  func marker(): int;
  static func marker(): string;
}

type Value {}

fit Value: Surface {
  func marker(): int { return 1; }
  static func marker(): string { return "static"; }
}
```

Semantic 接受该程序，但 Codegen 为两个 fit 方法都生成：

```text
FengFitUser__...__marker__from__void
```

生成 C 因函数声明/定义类型冲突而失败；静态调用还会错误命中需要 `self` 的实例函数。

预期：fit 与 `type`、object-form `spec` 使用相同的实例/静态重载集合边界；两个方法必须拥有不同且确定的编译期实现符号，并分别进入对应 witness 槽。

影响范围：fit 自身的实例/静态同名成员符号生成，以及 OSM05 的 fit witness。修复前必须确认既有成功程序的 fit 符号和跨包 ABI 不发生变化；如果只能通过修改既有符号规则完成，必须停止并由人工决策。

事实核对：普通 `type` 的实例和静态方法分别使用不同符号域；非泛型 `fit` 却把两个域的方法放入同一个 `UserMethod` 数组，并且旧符号只包含 fit 前缀、方法名和参数后缀，因而丢失了实例/静态身份。

修复方案：先完整生成旧 fit 方法符号。只有当该完整符号与同一 fit 中先前方法实际重复时，才为当前方法追加确定的实例/静态域后缀，并再次查重。因此：

- 所有既有可成功编译程序的 fit 符号逐字不变；
- 新后缀只出现在修复前必然生成冲突 C、因而不存在可交付 ABI 的程序中；
- 该去重只在编译期注册方法时执行，witness 布局和运行时调用层数不变。

状态：已分析并解决。

### P05：父 spec 环走入新增 requirement 收集器后无限递归

定位后的准确最小复现是既有 `AE0614` 负向用例：

```feng
spec A: B {}
spec B: A {}
```

当前结果：既有父列表检查已记录 `AE0614`，但诊断记录成功不代表中止整个声明的后续分析；新增的父契约累积收集器随后在 `A -> B -> A` 上无限递归，最终以 `SIGSEGV` （退出码 139）终止。

预期结果：保留既有 `AE0614`，后续编译期验证不再次追踪活动路径中的同一 spec，分析正常返回诊断而不崩溃。

根因：新收集器只对“已收集的等价成员”去重，没有对“当前正在访问的 spec 声明”做活动路径环保护；空 spec 环中甚至没有成员可用于间接去重。

修复方案：为 requirement 收集器增加仅编译期使用的递归活动 spec 路径；进入已在路径中的声明时直接结束该环边。不使用全局“已访问”去重，以免丢失合法菱形图中通过不同泛型投影得到的 requirement。该修复不改变诊断、ABI 或任何运行时路径。

状态：已分析并解决。

### P06：同一泛型父 spec 的不同闭合实例被当成同一声明局部集合

以下两个中间父 spec 分别闭合同一泛型契约：

```feng
spec ReturnSurface<T> {
  func select(value: int): T;
}

spec NumberSurface: ReturnSurface<int> {}
spec TextSurface: ReturnSurface<string> {}
spec Invalid: NumberSurface, TextSurface {}
```

`Invalid` 的完整契约面同时含有 `select(int): int` 和
`select(int): string`，应报 `AE0509`，当前却被错误接受。

参数重叠也有同样问题：

```feng
spec Root {}
spec Leaf: Root {}
type Concrete: Leaf {}

spec SelectSurface<T> {
  func select(value: T): int;
}

spec RootSurface: SelectSurface<Root> {}
spec LeafSurface: SelectSurface<Leaf> {}
spec Invalid: RootSurface, LeafSurface {}
```

`Concrete` 同时满足 `Root` 和 `Leaf`，所以闭合后的两个
`select` 应按普通方法规则报 `AE0706`，当前也被错误接受。

根因：完整契约面已正确保留两个不同的闭合 requirement，但累积冲突检查只要看到两者的 `declaring_spec` 指针相同就跳过比较，没有比较 `ReturnSurface<int>` / `ReturnSurface<string>` 这一闭合 owner 实例。

修复方案：只在“声明相同且闭合 owner 实例语义相同”时跳过该配对，因为这类配对才确实已经由声明局部检查覆盖。同一泛型声明的不同闭合实例必须继续按完整契约面规则比较。这只是编译期类型比较，不改变 ABI 或运行时路径。

状态：已分析并解决。

### P07：同一泛型父 spec 的不同闭合实例进入 Codegen 后仍注册失败

P06 修复后，以下正向程序已通过 Semantic：

```feng
spec Base<T> {
  func choose(value: T): T;
}

spec Number: Base<int> {}
spec Flag: Base<bool> {}
spec Combined: Number, Flag {}

type Choice: Combined {
  func choose(value: int): int { return value + 1; }
  func choose(value: bool): bool { return value; }
}
```

当前结果：Semantic 正确保留 `Base<int>.choose(int)` 和
`Base<bool>.choose(bool)`，但 Codegen 在为 `Combined` 的默认 subject
构建 `Flag` 父 witness 投影时报告：

```text
CE0048: codegen: default subject parent witness member mismatch
```

预期结果：两个闭合 requirement 形成两个确定的 witness 槽，`int` 和 `bool` 调用分别进入正确实现。

影响范围：OSM04/OSM06 中同一泛型父契约经不同父路径闭合的正向场景。

根因：`Base<int>.choose` 与 `Base<bool>.choose` 来自同一个泛型 AST
成员声明，因此成员指针相同；但两者是参数、返回类型均不同的两个闭合
requirement 槽。Codegen 已正确注册两个槽，父 witness 投影却仍只按 AST
成员指针查找 root 槽，并总是取得第一个 `Base<int>` 槽，随后与
`Base<bool>` 目标槽做兼容性检查而失败。

修复方案：父 witness 投影和 spec 槽适配继续以 AST 声明身份限定候选，
同时用已经存在的闭合槽签名完成最终匹配。只调整编译期槽查找，不增加
witness 字段、运行时查找、运行时分支或调用层级，也不改变既有槽顺序和
字段名。

第一处父 witness 投影修复后，补充不在子 spec 重声明方法的精确行为用例：

```feng
let view: Combined = Choice {};
let result = view.choose(true);
```

Semantic 正确推断 `result` 为 `bool`，但直接调用的 Codegen 路径仍只用
Semantic 记录的 AST 成员指针查槽，再次取得第一个 `Base<int>` 槽；当
`result` 进入 `!result` 时报告：

```text
CE0094: codegen: '!' requires bool operand
```

这与父 witness 投影是同一根因在调用发出路径中的残留：精确 requirement
身份包含闭合 owner/闭合签名，不能退化为裸 AST 成员指针。后续修复必须
让直接调用和方法值也消费 Semantic 已记录的闭合选择，仍不得增加运行时
选择。

直接调用和方法值改为使用闭合槽后，程序已能完成 Semantic、Codegen 和
C 编译，但 FCTS 执行进入上述精确用例时以 signal 11 终止。检查生成 C 后
确认：崩溃不是方法值 adapter 或运行时分派本身引起的，而是具体 `type`
witness 的闭合槽仍绑定错误。例如生成的 `bool` 槽：

```c
static void ...__choose__feng_overload_2(
    void *_subject, const void *p0, void *_out) {
  int64_t _ret = Choice__choose__from__i64(
      (Choice *)_subject, *((int64_t const *)p0));
  *((int64_t *)_out) = _ret;
}
```

该槽应读取、返回 `bool` 并调用 `Choice.choose(bool)`，实际却使用了第一个
`int` 实现，造成参数和返回存储越界，最终触发 signal 11。

根因是具体 witness 绑定仍通过“槽包含相同 AST 声明”取得第一条 Semantic
witness entry。`Base<int>` 与 `Base<bool>` 的 entry 共享 `spec_member` 指针，
但分别记录了不同的 `impl_member`；旧查找没有用实现的闭合签名区分二者。

修复方案：在编译期同时使用 requirement 的 AST 声明身份和已选
implementation 的完整闭合签名，把 Semantic witness entry 映射到唯一
Codegen 槽；实例方法、静态方法及 fit 实现共用该匹配，不按数组下标或方法
名猜测。生成 witness 的字段、顺序、偏移和 thunk 调用层级保持不变，不增加
运行时分支、查找、分配或签名比较，也不变更 runtime ABI 或 Feng/C ABI。

完成上述重复 entry 区分后，精确 FCTS 已通过 `874/874`；但生成 C 的结构
检查又发现，单独物化的 `Base<bool>` 父 witness 仍错误调用 `int` 实现：

```c
static void ...Base_bool___choose(
    void *_subject, const void *p0, void *_out) {
  int64_t _ret = Choice__choose__from__i64(
      (Choice *)_subject, *((int64_t const *)p0));
  memcpy(_out, &_ret, sizeof _ret);
}
```

此时 `Base<bool>` 自身只有一个 Codegen 槽，不存在同一表面内的声明歧义。
进一步确认：Semantic witness 缓存以 `(subject, spec_decl)` 为键，未包含泛型
`spec` 的闭合参数，因此查询 `Base<bool>` 时可能取得先建立的 `Base<int>`
entry。Codegen 不能仅因 AST 声明唯一就直接采用该实现；还必须校验 entry
实现的完整签名是否匹配当前闭合槽。若不匹配，则沿现有编译期 type/fit
元数据按完整签名恢复唯一实现。该校验和恢复只发生在编译器中，不增加生成
代码的运行时开销，也不修改 Semantic sidecar、witness 或 descriptor ABI。

状态：已分析并解决。

## 9. 最终验证结果

- `build/bin/test_semantic`：通过。
- `build/bin/test_codegen`：通过。
- `./build/bin/feng run fcts/fcts_bin`：`Total: 874, Passed: 874, Failed: 0, Skipped: 0`。
- 沙箱外全量 `make test`：通过，退出码为 0；UBSan、正常优化构建、性能约束、CLI、发布脚本及全部回归项均通过。
- 生成 C 结构检查：闭合泛型父 `spec` 的实例/静态槽均调用匹配的精确实现；未增加运行时选择、查找、分配、descriptor 字段或适配层。
- ABI 检查：已有主槽的字段名、顺序和布局保持不变；新槽只对应此前被错误丢弃的合法重载。
