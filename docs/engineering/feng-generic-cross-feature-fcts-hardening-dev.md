# Feng 泛型跨特性场景 FCTS 补强开发文档

> 状态：进行中（第一至第三组已完成，待第四组）
>
> 泛型语言规则以
> [feng-generics-draft.md](../specifications/feng-generics-draft.md) 为准；前两轮组合覆盖及修复记录见
> [feng-generic-composition-fcts-hardening-dev.md](./feng-generic-composition-fcts-hardening-dev.md) 和
> [feng-generic-advanced-composition-fcts-hardening-dev.md](./feng-generic-advanced-composition-fcts-hardening-dev.md)。
> 本文只补现有语言能力仍缺少的跨特性行为闭环，不新增泛型语义。
>
> 第三组曾暴露出泛型类型 owner 缺少 callable dependency slots，以及泛型终结器实现与权威规范不一致。
> [feng-generic-type-owner-reification-bugfix-dev.md](./feng-generic-type-owner-reification-bugfix-dev.md)
> 已完成并通过全量回归；第三组现已完成，本计划从第四组继续。

## 1 目标

前两轮已经覆盖泛型共享体具化、普通 `type`、`@value type`、tuple、多种 spec、三种 callable
来源、复杂控制流、数组/`List`/`Option`、union-form 以及递归描述符图。本轮继续补齐以下尚未形成
完整运行行为证据的组合：

1. 泛型共享体持有泛型值时经过异常传播和 `defer` 清理；
2. 捕获泛型值的闭包逃逸出创建它的共享体；
3. 泛型构造器主体创建派生泛型局部值并调用泛型 callable；
4. 从上下文目标类型推导函数或方法类型参数，并通过静态工厂契约产生结果；
5. 泛型约束中的静态字段访问；
6. 跨包泛型 `fit` 及不同 subject 表示；
7. enum、tuple、数组和泛型引用实例直接作为 `T`，以及 `Span`、变参、`Map` 等复合路径。

新增成功行为用例优先写入 FCTS，并验证完整值或确定的生命周期结果，不能只验证源码可编译。

## 2 边界

### 2.1 纳入范围

- provider 定义于 `fcts_lib`、consumer 与断言定义于 `fcts_bin` 的跨包路径；
- 顶层泛型函数、泛型静态方法、泛型实例方法及泛型 owner 构造器；
- 标量、托管引用、object-form spec、enum、tuple、数组和 descriptor-sized `@value type`；
- `.ft` 恢复、函数/类型描述符、witness、callable capture、aggregate 复制和清理；
- 当前规范已确认的上下文目标类型推导与 object-form spec 静态成员约束；
- 使用现有标准库泛型类型形成语言组合，但不扩展标准库公开 API。

### 2.2 测试归属

- 可以在 Feng 用户代码中观察的成功行为统一优先放入 FCTS；
- 编译期必然失败的约束、推导、variance 和语法场景放入 `test/semantic` 或对应 compiler test；
- 只用于锁定 C ABI 形状、描述符静态结构或诊断归属的用例放入 `test/codegen`；
- 只有无法在普通 Feng `main` 返回前稳定观察的进程级行为才使用 `test/cli`；
- 标准库容器自身的 API 边界继续由 `std/std_test` 验证，本轮 FCTS 只验证其参与泛型共享 ABI 的组合。

### 2.3 不纳入范围

- 泛型重载优化、variance、泛型与 C ABI 的新交叉语义；
- 新增 runtime API、修改 runtime 私有 ABI 或增加 Feng 程序运行时开销；
- `Promise<T>` / `Future<T>` 等尚未纳入本轮语言 ABI 缺口的标准库专项行为；
- 对所有类型和语法位置做笛卡尔积枚举；
- 闭合泛型函数或方法 target 直接作为一等 callable 值。

最后一项当前没有权威成功语义。[feng-generics-draft.md](../specifications/feng-generics-draft.md)
只定义显式泛型 target 被调用、构造、数组创建或其他已定义形式消费，未被合法消费时必须报错；
[feng-function.md](../specifications/feng-function.md) 也没有定义如何把闭合泛型 callable 及其函数描述符绑定为函数值。
因此本轮不得自行把 `let f: Func<i64, i64> = identity<i64>;` 当作合法行为。若后续决定支持，必须先更新
权威规范，再建立独立实现与 FCTS 计划。

若任一纳入组只能通过新增语言规则、runtime API、runtime ABI、非通用特判或增加 Feng 程序运行时
开销实现，必须停止该组并提交人工决策。

## 3 测试组织

- 新建独立的 provider 文件，不修改既有泛型模型的定义或语义；
- 新建独立的 consumer 测试文件，使用 `std.test` 的 `describe`、`test` 和 `assert`；
- `fcts/fcts_bin/src/main.ff` 只增加一次新测试入口调用；
- 测试库不使用 `puts` 或其他额外输出；
- 每组可以继续向本轮新文件追加模型和断言，但不得修改此前已有测试的语义或断言；
- 缺陷修复需要新增 compiler regression 时，新增独立测试函数并保持已有断言不变。

## 4 分组实施

### 4.1 第一组：异常与 `defer` 生命周期

在 imported 泛型共享体中分别让直接 `T` 和 `Composite<T>` 保持存活，并经历：

- `try` 正常返回；
- 抛出具体异常后由 caller 捕获；
- 自动传播，并由 consumer 捕获后重新抛出；
- imported 共享体捕获具体异常并返回泛型 fallback；
- `defer` 捕获、读取和替换泛型值；
- 异常路径和正常路径离开嵌套作用域。

至少使用托管引用与 descriptor-sized `@value type` 两类闭合实参。通过带终结器的普通引用叶子验证
异常展开和 `defer` 清理没有提前释放、遗漏释放或重复释放；标量路径只验证完整值。

验收：运行 `./build/bin/feng run fcts/fcts_bin`，通过后在沙箱外执行一次 `make test`。

### 4.2 第二组：逃逸泛型闭包

分别从 imported 顶层泛型函数和泛型 owner 的实例方法返回 callable-form spec 值，使闭包在创建共享体
返回后继续存活并调用。覆盖：

- 捕获直接 `T`；
- 捕获 descriptor-sized `Composite<T>`；
- 同时捕获 owner `T` 与方法 `U`；
- 闭包值复制、覆盖和最终释放；
- 标量、托管引用和 descriptor-sized 闭合实例。

断言既验证逃逸后的完整返回值，也通过终结器计数验证捕获单元的生命周期。这里只测试 Lambda 产生并
返回 callable 值，不涉及未定义的“闭合泛型函数 target 直接作为函数值”。

验收：专项 FCTS 通过后，在沙箱外执行一次 `make test`。

### 4.3 第三组：泛型构造器主体具化

> 已完成。描述符职责、构造/终结器共享体 ABI 和专项回归统一由
> [feng-generic-type-owner-reification-bugfix-dev.md](./feng-generic-type-owner-reification-bugfix-dev.md)
> 定义，本文只保留跨特性 FCTS 的覆盖结果，不重复维护这些规则。

定义 imported 泛型普通 `type` 和泛型 `@value type`，其构造器主体同时：

- 接收并保存 owner 类型参数 `T`；
- 创建 `List<T>` 与 `Composite<T>` 局部值；
- 调用另一个 imported 泛型 callable；
- 将完整结果写入字段并由 consumer 读取。

使用标量、托管引用和 descriptor-sized 类型实参，验证构造器能从 owner 描述符获得 aggregate、managed
type 与 callable 三类依赖，并保持既有构造器 ABI；不得为构造器增加独立函数描述符。

验收：专项 FCTS 通过后，在沙箱外执行一次 `make test`。

### 4.4 第四组：目标类型推导与静态工厂/静态字段约束

定义跨包 object-form spec：

```feng
spec Factory<T> {
  static let label: string;
  static var count: i64;
  static func make(seed: i64): T;
}
```

并定义 `create<T: Factory<T>>(seed: i64): T`。分别验证：

- `create<Concrete>(seed)` 显式类型实参路径；
- `let value: Concrete = create(seed);` 仅从上下文目标类型推导 `T`；
- `T.make()` 的标量、托管引用及 descriptor-sized 返回；
- `T.label` 读取和 `T.count` 读写；
- 本地声明与 imported `.ft` 声明使用一致的静态 witness 槽位。

目标类型推导只负责确定 `T`；构造能力必须来自 `Factory<T>` 静态契约，不能假设 Feng 存在 C#
`new()` 约束或允许对无约束 `T` 直接构造。`Json.parse<T: JsonDeserializable<T>>` / `T.fromJson()`
属于同一种静态工厂模式，但本组使用独立最小模型，避免把 JSON API 行为混入编译器泛型验收。

无法从目标类型唯一推导、推导结果不满足约束等负向场景放入 `test/semantic`；成功行为放入 FCTS。

验收：专项 FCTS 通过后，在沙箱外执行一次 `make test`。

### 4.5 第五组：跨包泛型 `fit` 与 subject 表示

provider 导出泛型 object-form spec、目标类型及可见泛型 `fit`，consumer 通过 imported 泛型共享体调用
fit 提供的方法。至少覆盖：

- 普通引用泛型 type；
- 泛型 `@value type`；
- enum 或内建标量的具体 fit subject；
- 方法参数和返回值直接或传递引用 spec 类型参数；
- fit witness 作为泛型约束继续转发到另一 imported callable。

用例验证真实方法结果和 subject 的值/引用语义，不只检查 `.ft` 含有 fit 记录。静态字段不由 fit 提供，
继续遵守 object-form spec 静态成员的既有满足性规则。

验收：专项 FCTS 通过后，在沙箱外执行一次 `make test`。

### 4.6 第六组：剩余表示与标准泛型组合

在 imported 共享体中补齐以下非等价路径：

1. enum、具名 tuple、数组和闭合泛型引用实例直接作为 `T`；
2. descriptor-sized 泛型值的多维数组构造、读取、覆盖和返回；
3. `Span<Composite<T>>` 的参数、切片和返回；
4. descriptor-sized `T...` 的普通传参及预打包数组转发；
5. `Map<K, Composite<T>>` 的插入、覆盖、读取和返回，其中 `K` 使用现有 `Hashable<K>` 约束；
6. 上述托管叶子的确定性释放。

该组按运行时表示和 lowering 路径选取代表性实例，不为 `List`、`Span`、`Map` 或当前测试类型添加编译器
特判。标准库 API 若缺少某项操作，只使用其已有公开能力形成等价数据流。

验收：专项 FCTS 通过后，在沙箱外执行一次 `make test`。

## 5 缺陷处理流程

每组严格执行：

1. 只增加当前组所需模型和断言；
2. 先运行 FCTS 专项命令，区分测试写法、Semantic、Codegen、runtime 或标准库问题；
3. 发现缺陷后停止下一组，在本文记录最小复现、根因、通用修复规则及 ABI/性能影响；
4. 修复必须复用既有类型/函数描述符、aggregate descriptor、witness、callable ABI 和 cleanup 抽象；
5. 禁止针对测试类型、某个包、某个参数位置、某个容器或某种错误文本增加特判；
6. 修复若增加 Feng 程序运行时操作、分配，或改变 runtime 私有 ABI，必须先取得人工批准；
7. 当前组 FCTS、compiler regression 和 `make test` 全部通过后，才进入下一组。

## 6 完成标准

- [x] 第一组：异常与 `defer` 生命周期
- [x] 第一组完成后的 `make test`
- [x] 第二组：逃逸泛型闭包
- [x] 第二组完成后的 `make test`
- [x] 第三组：泛型构造器主体具化
- [x] 第三组完成后的 `make test`
- [ ] 第四组：目标类型推导与静态工厂/静态字段约束
- [ ] 第四组完成后的 `make test`
- [ ] 第五组：跨包泛型 `fit` 与多种 subject 表示
- [ ] 第五组完成后的 `make test`
- [ ] 第六组：剩余表示与标准泛型组合
- [ ] 第六组完成后的 `make test`
- [ ] 最终核对新增用例与既有 FCTS / `std_test` 无等价重复
- [ ] 最终 `make test` 无 sanitizer 报告

完成前不得把本文状态改为“已完成”。

## 7 实施中确认的边界与缺陷

### 7.1 泛型类型参数不能直接作为 catch 类型（规则边界，非缺陷）

第一组首次专项运行确认，现行 Semantic 对 `catch ex: T` 报 `AE0179`：泛型类型参数不能作为 catch
子句的运行时匹配类型。本轮不扩展异常语言规则；imported 共享体只捕获规范允许的具体异常类型。

### 7.2 开放泛型参数不是具体异常载荷类型（规则边界，非缺陷）

异常规范只允许抛出列明的具体值类型，开放泛型参数 `T` 本身不属于具体异常载荷类型。因此本轮不要求
`throw T` 或 `throw Composite<T>`；正确覆盖形态是在共享体持有这些泛型值时抛出具体 `string` 异常，
验证展开期间的清理、传播、重新抛出，以及共享体 catch 后返回泛型 fallback。该形态不需要新增异常
装箱元数据或修改 runtime ABI。

### 7.3 `try` 受保护区间内的描述符定长存储（Codegen 缺陷）

第一组用例暴露出：泛型共享体在 `try` 受保护区间内接收直接 `T` 或 descriptor-sized 聚合返回值时，
Codegen 会在函数级 C 复合语句中声明按描述符大小确定的 VLA；同一复合语句中用于保留 landing pad 的
条件 `goto` 会跨过该声明，主机 C 编译器因此拒绝发码结果。

通用修复规则是让 try 的受保护区间拥有独立 C 词法块，区间起止标签与其中产生的描述符定长临时量
都位于该块内，landing pad 和 catch 分派位于块外。异常帧、LSDA、cleanup 协议及运行控制流均保持
不变；该修复不修改 runtime ABI，也不增加 Feng 程序运行时分支、分配或描述符读取。

### 7.4 表达式 join 的直接泛型参数存储（Codegen 缺陷）

第一组继续确认：`if`、`match`、`try` 共用的表达式 join 只处理了固定布局值和 descriptor-sized
聚合，没有处理直接 `T`。因此 `try` 的结果槽被错误声明为单个 `void *`；闭合为宽 `@value type`
后，返回逻辑会按真实 aggregate descriptor 读取该槽，造成越界访问。

通用修复规则是扩展统一的 expression join slot，使直接 `T` 与普通直接泛型局部量复用同一套
descriptor-sized erased storage、generic value store 和 cleanup 协议。描述符与大小在结果槽声明时各读取
一次，分支只执行既有三类 value-kind 复制/保留协议。该修复同时覆盖 `if`、`match`、`try`，不新增
runtime API/ABI、堆分配或表示特判；新增操作均是正确保存开放 `T` 所必需，不能以原错误的指针槽作为
性能基线。

### 7.5 单表达式 Lambda 返回值的目标类型传播（Codegen 缺陷）

第二组首次专项运行确认：Semantic 已按显式 callable-form spec 目标接受 Lambda 返回的具名 tuple
字面量，但 Lambda invoke 发码仍使用无目标的普通表达式入口，最终报 `CE0121`。这与函数规范要求的
“先用目标 callable 返回类型检查 Lambda 结果”以及 tuple 返回目标绑定规则不一致。

通用修复规则是：非 `void` 单表达式 Lambda 使用 callable 已解析的返回类型作为 expected type 发码；
`void` Lambda 继续按普通表达式发码并丢弃结果。这样 tuple、数组、数值字面量和 spec coercion 统一
复用既有 expected-type lowering，不新增 runtime 操作或 ABI 变化。

### 7.6 Lambda 目标 callable 签名的待具化依赖（Semantic 缺陷）

第二组继续确认：泛型共享体返回 Lambda 时，待具化依赖收集只遍历 Lambda 的显式参数和表达式体，
没有把已解析 callable-form spec 的 caller-view 签名纳入函数依赖。例如目标为
`Reader<T>(): Pair<T, string>` 时，Lambda 体中的 tuple 字面量没有显式写出 `Pair<T, string>`，导致函数
描述符缺少该 aggregate 依赖，invoke 发码报 `CE0066`。

通用修复规则是：Lambda 已被解析为 callable-form spec 后，依赖收集以该具体目标的参数和返回类型为
权威签名，将 spec 泛参替换为调用方视角实参，再按既有规则收集 managed、aggregate 依赖。该规则覆盖
所有由目标类型决定表示的 Lambda 参数与返回值，不针对 tuple 或某个 callable 特判；只完善编译期函数
描述符内容，不修改 runtime ABI，也不增加既有正确路径之外的运行时操作。

### 7.7 共享 callable 的 owned aggregate 实参清理（Codegen 缺陷）

第二组生命周期断言确认：将新构造的、带托管字段的值类型直接传给泛型函数时，实参已有稳定 C 地址，
旧的顶层泛型调用路径因此不再物化它；但该路径也没有为这个 owned aggregate 临时值注册 cleanup。callee
正确复制参数后，调用方原始临时值永久保留一个托管引用，表现为逃逸闭包销毁后叶子对象仍未终结。

通用修复规则是让顶层泛型函数与共享方法统一使用同一个 callable 实参准备入口：所有 owned managed 或
aggregate 实参必须先取得 cleanup 所有权，再根据声明选择传值或地址 ABI。对于已经由表达式生产者放入
唯一稳定 C 局部槽的 fixed-layout owned aggregate，直接把该槽登记到当前作用域，不为登记 cleanup 再做
一次结构体复制。修复不修改 runtime API/ABI，不增加堆分配或描述符读取；新增的 release 只回收原本
泄漏的所有权。
