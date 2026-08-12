# Feng 泛型跨特性场景 FCTS 补强开发文档

> 状态：已完成
>
> 泛型语言规则以
> [feng-generics-draft.md](../specifications/feng-generics-draft.md) 为准；前两轮组合覆盖及修复记录见
> [feng-generic-composition-fcts-hardening-dev.md](./feng-generic-composition-fcts-hardening-dev.md) 和
> [feng-generic-advanced-composition-fcts-hardening-dev.md](./feng-generic-advanced-composition-fcts-hardening-dev.md)。
> 本文只补现有语言能力仍缺少的跨特性行为闭环，不新增泛型语义。
>
> 第三组曾暴露出泛型类型 owner 缺少 callable dependency slots，以及泛型终结器实现与权威规范不一致。
> [feng-generic-type-owner-reification-bugfix-dev.md](./feng-generic-type-owner-reification-bugfix-dev.md)
> 已完成并通过全量回归；第三组、`@mixable` 生成 wrapper 的泛型 reified dependency 补充组及
> 第四组现均已完成。

## 1 目标

前两轮已经覆盖泛型共享体具化、普通 `type`、`@value type`、tuple、多种 spec、三种 callable
来源、复杂控制流、数组/`List`/`Option`、union-form 以及递归描述符图。本轮继续补齐以下尚未形成
完整运行行为证据的组合：

1. 泛型共享体持有泛型值时经过异常传播和 `defer` 清理；
2. 捕获泛型值的闭包逃逸出创建它的共享体；
3. 泛型构造器主体创建派生泛型局部值并调用泛型 callable；
4. `@mixable` 泛型静态 wrapper 和实例 wrapper 跨包、多层转发 reified dependencies；
5. 从上下文目标类型推导函数或方法类型参数，并通过静态工厂契约产生结果；
6. 泛型约束中的静态字段访问；
7. 跨包泛型 `fit` 及不同 subject 表示；
8. enum、tuple、数组和泛型引用实例直接作为 `T`，以及 `Span`、变参、`Map` 等复合路径。

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

### 4.3.1 第三组补充：`@mixable` 泛型 reified dependencies

新增独立的 imported 来源类型和 consumer 展开目标，不修改既有 mixin 用例。覆盖：

- 来源 owner 类型参数 `T` 在 `@mixable` 方法体中闭合 `List<T>`、聚合类型和泛型 callable；
- 方法类型参数 `U` 在生成静态 wrapper 和实例 wrapper 中保持独立描述符映射；
- owner `T` 与方法 `U` 同时出现在参数、返回值及派生 callable dependencies 中；
- provider 导出的生成 wrapper 经 `.ft` 恢复后再被 consumer 多层展开；
- 来源实例入口、导入生成入口、consumer 静态入口和 consumer 实例入口；
- 标量、托管引用和 descriptor-sized 闭合实参。

这些路径必须复用普通手写方法的函数描述符、callable dependency slots 和泛型共享体。
禁止为 `@mixable` 生成 wrapper 增加专用运行时描述符查找或特判。

验收：专项 FCTS 通过后，在沙箱外执行一次 `make test`；通过后再进入第四组。

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
- [x] 第三组补充：`@mixable` 泛型 reified dependencies
- [x] `@mixable` 泛型补充组完成后的 `make test`
- [x] 第四组：目标类型推导与静态工厂/静态字段约束
- [x] 第四组完成后的 `make test`
- [x] 第五组：跨包泛型 `fit` 与多种 subject 表示
- [x] 第五组完成后的 `make test`
- [x] 第六组：剩余表示与标准泛型组合
- [x] 第六组完成后的 `make test`
- [x] 最终核对新增用例覆盖计划中的全部语言场景；允许与标准库 `std_test` 行为重合
- [x] 最终 `make test` 无 sanitizer 报告

上述完成标准均已满足。

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

### 7.8 泛型 owner 的泛型方法返回类型代入（Semantic 缺陷）

`@mixable` 泛型补充组首次专项运行确认：当泛型 owner 的泛型方法返回只依赖方法参数 `U`
的泛型聚合时，调用返回类型的待具化依赖收集只检查并代入了 owner 参数 `T`。返回类型不含
`T` 时收集器提前退出，因而遗漏 `Composite<U>` 的 aggregate descriptor slot，共享体发码报
`IE0002`（原 `CE0066`）。该缺口属于普通泛型方法调用，不属于 `@mixable` 展开规则。

通用修复规则是：依赖收集以 Semantic 已记录的 caller-view owner 实例和 callable 类型实参为权威
映射，按“owner 类型参数、callable 类型参数”两层依次代入返回类型，再以完整的 caller-view
类型收集待具化依赖。该规则同时覆盖只依赖 `T`、只依赖 `U` 及同时依赖 `T/U` 的返回类型，
并统一使用显式或推导得到的 callable 类型实参。修复只完善编译期函数描述符内容，不修改 runtime
API/ABI，不增加 Feng 程序运行时分支、分配或描述符读取。

### 7.9 多层 callable dependency graph 的泛型实例预注册（Codegen 缺陷）

`@mixable` 泛型跨包、多层展开路径继续确认：consumer 的实例 wrapper 依赖 imported 中间层静态
wrapper，中间层再依赖原始来源方法。函数描述符生成已会递归闭合整个 callable dependency graph，
但发码前的泛型 type/spec 实例预注册只扫描直接依赖。第二层描述符生成时因此无法解析
只在转发链上间接出现的 `Source<T>` 具化实例，报 `CE0031`。该缺口同样属于普通泛型
callable dependency graph，不属于 `@mixable` 专用规则。

通用修复规则是：泛型实例预注册与函数描述符生成共用同一个 callable dependency graph 语义；
每层先用 caller 的闭合实参生成 callee 参数映射，注册该层的 managed/aggregate 依赖，再递归处理
其 callable 依赖。遍历以“依赖集身份 + 闭合类型实参”为循环判定单元，使直接递归和互递归不会
无限扫描。该修复只改变编译期预注册顺序，不修改 runtime API/ABI，不增加生成程序的描述符、
分支、分配或间接读取。

### 7.10 callable 返回类型未参与目标类型推导（Semantic 缺陷）

第四组首次专项运行确认：显式类型实参调用已经能够通过静态工厂约束返回完整值，但省略类型实参后，
顶层函数、静态方法和实例方法都没有使用绑定位置的目标类型。Semantic 的 callable 泛参收集只合并
显式类型实参和调用实参，未把 callable 返回类型与当前上下文目标类型统一；因此仅出现在返回类型中的
`T` 无法确定，后续以 `AE1003` 报告整个调用表达式与目标类型不匹配。

通用修复规则是：所有普通泛型 callable 共用同一套三源推导，依次合并显式类型实参、调用实参和
上下文目标类型。目标类型推导先将 owner 类型参数及 `fit` 目标参数代入 callable 返回类型，再将该
caller-view 返回类型与目标类型按完整类型结构统一；它必须覆盖直接 `T` 和 `Composite<T>` 等嵌套位置。
显式类型实参及调用实参先确定的泛参保持优先，目标类型只补足尚未确定的泛参，返回结果与目标类型的
最终兼容性继续由既有 expected-type 检查负责；数组可写性等合法结果适配不得反向否定已经完成的参数
推导。候选返回结构无法提供所需推导时，该候选不适用；唯一选中后仍有泛参未确定时，Semantic 必须以
`AE0525` 在调用点报错，不得继续到 Codegen。修复只增加编译期类型统一与语义元数据记录，不修改
runtime API/ABI，也不增加生成程序的运行时操作。

调用解析完成后，返回类型推断和 expected-type 校验必须优先消费调用点已记录的
`resolved_callable.callable_type_args`，不得在目标上下文已经退出后脱离该元数据重新推导。重新推导只
用于尚未生成 resolved-call 元数据的候选探测阶段。Codegen 对顶层函数、外部函数、静态方法和实例
方法发码时也必须以这组已闭合实参为权威，不得只根据调用实参再次推导；该规则仍只影响编译期，
生成代码与显式写出相同类型实参时一致。

### 7.11 泛型值对象字面量丢失 reified storage 元数据（Codegen 缺陷）

第四组的 `Composite<T>` 返回用例进一步确认：泛型共享体直接返回依赖开放泛参布局的 `@value`
对象字面量时，底层构造入口已经按具体聚合描述符分配了准确大小的栈存储，也完整返回了存储地址、
描述符及缓存后的大小；对象字面量封装层却只转交表达式、类型和所有权，丢失了地址形式与 reified
storage 元数据，导致通用返回路径无法取得源地址并误报 `CE0299`。

通用修复规则是：值对象字面量必须完整转交底层值构造结果的表示与所有权元数据；字段覆盖也必须通过
该结果声明的地址形式访问，不能重新假设结果一定是固定布局 C 局部值。修复不增加新的存储、复制、
描述符读取或运行时分支，只保留底层构造入口已经确定的信息。

### 7.12 独立库中的开放泛型 `fit` 未注册（Codegen 缺陷）

第五组首次专项运行确认：泛型 `fit Target<T>: Spec<T>` 与其目标类型定义在 provider 中，而闭合实例
只出现在 consumer 时，provider 的独立库构建会在声明发码阶段报 `CE0351`。泛型实例收集已经建立
`Target<T>` 的开放实例，但 fit 注册只为同一 generic origin 的闭合实例建立条目；独立构建 provider
时不存在闭合实例，因此共享 fit 方法体也失去了可供发码的注册身份。同包用例中恰好存在闭合实例，
所以没有暴露该缺口。

通用修复规则是：开放 fit 实例是泛型 fit 声明的编译期模板身份，负责解析开放 spec/方法签名并发出
唯一共享方法体；每个闭合 fit 实例只负责其 wrapper、函数描述符和 witness。开放实例解析 spec 时
必须使用声明的类型参数作用域，不能把 `Spec<T>` 当作闭合类型；开放实例不得生成闭合 wrapper、
实例函数描述符或 witness。fit 共享体的 aggregate、managed type 和 callable 依赖必须来自 fit 声明
自身的待具化依赖集，不能误用 target 类型声明的依赖集；闭合实例继续复用既有薄 wrapper 与描述符
槽位。该修复只补齐编译期注册和发码分工，不修改 runtime API/ABI，也不增加闭合 Feng 程序的运行时
分支、分配或描述符读取。

### 7.13 泛型约束 witness 调用未完整处理 address ABI（Codegen 缺陷）

第五组继续确认：通过 `U: Spec<T>` 调用 spec 方法时，generic-constraint witness 分支把所有 address-ABI
返回值都按直接泛参 `T` 处理，只会申请 generic-param erased storage。方法返回 `Composite<T>` 等依赖
开放泛参布局的聚合时，该入口无法建立结果槽；其参数侧也没有按 spec 成员已经确定的 ABI kind 统一
取得地址。普通 spec 值调用已经区分直接泛参、reified aggregate 和固定布局三种 address-ABI 结果，
两条 witness 路径的行为不一致。

通用修复规则是：generic-constraint witness 与普通 spec witness 共用同一 ABI 分类。address-ABI 参数
必须从表达式结果取得稳定地址；返回值为直接泛参时使用参数描述符定长 storage，为开放聚合时使用
函数描述符中的 aggregate dependency，为固定布局值时使用普通 C 局部槽，并完整保留对应 storage、
描述符和所有权元数据。修复只让原本无法发码的调用遵守既有 witness ABI，不修改 runtime API/ABI，
也不为既有正确路径增加运行时操作。

### 7.14 泛型约束适用性遗漏闭合 `fit` 实例（Semantic 缺陷）

第五组继续确认：调用 `func use<T, U: Spec<T>>(value: U)` 并显式绑定
`U = Target<string>` 时，候选过滤只按 `Target` 声明检查闭合的 `Spec<string>`，没有复用完整的
“类型引用是否满足 spec 实例”判定。对于只通过开放声明 `fit Target<T>: Spec<T>` 获得契约的泛型
目标，`Target<string>` 与 `Spec<string>` 的闭合关系虽已由可见 fit 正确建立，候选仍会被错误排除；
不带类型实参的具体 enum fit 因不经过这条分支而未暴露问题。

通用修复规则是：泛型约束候选过滤必须以完整的实际类型引用和完成泛参代入的约束类型引用为输入，
统一复用类型引用级 spec 满足关系。该关系依次处理类型自身的精确实现、名义契约关系以及当前作用域
可见 fit 的闭合实例，不得把泛型目标退化为仅检查其开放类型声明。修复只改变编译期候选适用性判定，
不修改 runtime API/ABI，也不增加生成程序的运行时操作。

### 7.15 开放泛型约束 witness 仅匹配直接泛参成员（Codegen 缺陷）

第五组进一步确认：泛型共享体的约束表面保留为开放 `Spec<T>`，调用点为具体实参生成 witness；这是
二进制共享 ABI 的既有设计。旧的 witness 实现匹配只把直接出现的 `T` 视为开放位置，因此
`read(): T`、`write(T)` 可以匹配具体实现，但 `exchange(Packet<T>): Packet<T>` 不能匹配
`Packet<string>`，最终在已通过 Semantic 的调用点误报 `CE0315`。

通用修复规则是：开放约束成员与具体实现的匹配必须递归遍历完整类型结构，并在同一方法的返回值和
所有参数之间保持每个约束泛参的唯一绑定。数组、指针、泛型 type、泛型 spec/callable 以及任意层级的
组合均使用同一结构匹配规则；不同泛参分别绑定，但同一泛参重复出现时必须一致。调用点仍生成既有的
具体 witness，泛型共享体仍接收相同的隐藏 witness 指针。修复不修改 runtime API/ABI，不新增装箱、
分配、描述符读取或运行时分支。

### 7.16 泛型值类型 fit wrapper 缺少函数描述符（Codegen 缺陷）

第五组继续发码确认：泛型 fit 共享体依赖 `Composite<T>` 时，每个闭合目标实例都需要一个只读
`FengFunctionDescriptor`，wrapper 通过它读取已经闭合的 aggregate/managed dependency。旧实现把该
描述符的发出逻辑嵌在引用类型的 `FengTypeDescriptor` 生成函数中；`@value` 与 tuple 走独立的
`FengAggregateDescriptor` 路径，因此其 wrapper 引用了未声明的 `__fitN__fdesc` 符号。

通用修复规则是：闭合泛型 fit 函数描述符属于 fit 实例，而不属于目标类型的某一种物理表示。将其发出
抽为引用类型、`@value` 和 tuple 共用的 fit 实例步骤；描述符中的依赖仍按 fit 声明的待具化集合和目标
实例实参闭合，并只为确有依赖的 fit 发出。修复不修改 runtime API/ABI，不新增运行时字段、分配、
描述符读取或分支；引用类型生成结果保持不变。

### 7.17 共享泛型 address ABI 丢失参数值语义（Codegen 缺陷）

第五组运行行为确认：`@value type` 作为普通非泛型参数时按值传递；闭合为直接泛参 `U` 后也必须保持
完全相同的语义。旧的共享泛型调用路径却把 address ABI 当成了引用语义：实参已有稳定地址时直接把
调用方存储传给 callee。callee 再通过泛型约束 witness 修改值 subject，会直接改写调用方变量；共享体
继续把该参数传给另一泛型 callable 时，也没有在新的调用边界建立下一层参数副本。

通用修复规则是：address ABI 只负责传输未知布局的值，不得改变 Feng 参数语义。每个共享 callable
调用边界都必须按实际表示准备参数：闭合 `@value`、tuple 及其他值表示建立独立参数存储，含托管槽的
聚合使用既有 aggregate 复制与清理协议；普通引用表示只复制引用值，仍共享其所指对象；调用方仍为
开放泛参时，由已传入的泛参描述符选择同一套 trivial、managed-pointer、managed-aggregate 处理。嵌套
泛型转发必须在每一层调用边界重复该规则，不能复用上一层参数存储。

实现必须由共享 callable 实参准备抽象统一覆盖顶层函数、静态/实例方法及 callable/spec witness 的
address 参数，不得针对 `fit`、具体测试类型或某个参数位置特判。已知闭合类型继续静态发码；只有调用方
本身仍持有开放泛参时才执行描述符 kind 分派。修复不修改 runtime API/ABI、不增加堆分配，也不改变
非泛型路径；新增的值复制、托管槽 retain/release 和开放表示分派均是恢复既有按值参数语义所必需，
不能以旧的错误借用地址路径作为性能基线。

上述规则只作用于 callable 的显式参数，不改变值类型实例方法的隐式 `self`。`@value`/tuple 方法仍以
接收者存储地址作为 `self`，方法内成员修改必须回写该接收者；当接收者恰好是另一个函数的按值参数时，
该地址自然指向当前函数已建立的参数副本，因此只修改这一层副本。

共享实参准备必须优先使用表达式结果已经携带的描述符与缓存大小，不能重新假设当前 C 函数仍可直接
访问最初的隐藏泛参符号。`defer` 生成的独立辅助函数捕获 descriptor-sized 局部值时，其既有栈闭包需随
值存储地址一并保存该局部值的描述符与大小，并在辅助函数作用域恢复为同一份编译期表示元数据。该处理
不新增 runtime API/ABI 或堆分配；仅在确实捕获动态布局值的 `defer` 闭包中保存两个已有标量值，是延迟
执行仍能按正确表示复制参数所必需的工作。

### 7.18 imported 开放泛型实例遗漏共享方法原型（Codegen 缺陷）

第六组首次专项构建确认：provider 的共享体调用 imported
`Span<Composite<T>>.slice` 或 `Map<K, Composite<T>>.get/set` 时，调用发码已经选择标准库导出的泛型共享
方法符号、owner 描述符和函数描述符，但 imported 原型预处理会跳过所有 generic instance。只有同一
编译单元恰好还注册了对应开放 origin type 时，相关共享方法原型才会被顺带发出；`List` 既有用例因此
没有暴露该缺口，而仅以嵌套开放实例出现的 `Span`、`Map` 会在宿主 C 编译阶段成为未声明函数。

通用修复规则是：imported 泛型共享方法原型由共享方法身份决定，不依赖某个开放 origin shell 是否
恰好出现在类型注册表。每个带开放上下文的 imported generic instance 都可提供其 origin 方法签名；
原型发出按最终共享 C 符号全局去重，并统一覆盖构造函数、实例方法和静态方法。参数 ABI 必须从 origin
声明计算，不能由某个闭合实例的参数表示反向决定；否则同一共享符号会随先遇到的实例产生不同 C
原型。该修复只补齐并规范化编译期 C 声明，不改变共享入口既有 ABI、runtime API/ABI 或生成程序的
任何运行时操作。

### 7.19 固定布局开放泛型值的 C 名义类型不统一（Codegen 缺陷）

第六组 consumer 发码确认：`Span<T>` 的物理布局始终固定，但 `Span<Composite<T>>` 在 provider 共享体
中与 `Span<Concrete>` 在 consumer 调用点仍是两个不同的 C struct tag。旧的共享参数 ABI 只对动态布局
聚合使用地址传输，因而试图把 consumer 的闭合 struct 直接按值传给 provider 的开放 struct 参数，宿主
C 编译器会以名义类型不兼容拒绝；物理尺寸相同不能消除 C 的类型差异。

通用修复规则是：共享 callable 的参数只要是仍引用活动泛参的值语义类型，就使用既有 address ABI；
动态布局与固定布局开放值都遵守同一规则。调用方按 §7.17 建立本调用边界的参数副本，callee 通过 origin
声明视角读取该副本；闭合值类型与非泛型函数仍保持原来的直接 C ABI。固定布局值原本就必须完成一次
按值参数复制，修复只把这次复制显式放在调用方并以地址传输，不增加复制次数、堆分配、描述符读取或
运行时分支，也不修改 runtime API/ABI。

### 7.20 顶层共享泛型函数遗漏变参归一化（Codegen 缺陷）

第六组 consumer 发码继续确认：普通函数和 callable/spec/method 路径都会把 Feng 变参调用的后缀实参
归一化为一个数组参数；顶层共享泛型函数却仍按调用点实参数量逐个传递。其共享入口只声明一个数组槽，
因此 `func collect<T>(values: T...)` 的普通调用会在宿主 C 编译阶段成为参数过多；显式 `...array` 路径
未暴露该缺口。

通用修复规则是：顶层共享泛型函数复用统一的变参归一化流程。固定参数先按声明处理；普通变参后缀中
的每个表达式只求值一次，再填入一个按闭合元素描述构造的数组；显式预打包数组只求值一次并直接传递。
共享入口始终只接收声明中的单个数组参数，类型推导与 address ABI 仍使用 Semantic 已记录的闭合实参及
origin 参数声明。修复不修改 runtime API/ABI；数组分配、元素复制及生命周期操作与 Feng 变参原有语义
完全一致，不增加额外求值或重复打包。

### 7.21 地址表示的值接收者被重复取址（Codegen 缺陷）

第六组运行验证确认：共享 callable 的显式值参数按 §7.17 建立独立副本后，如果该参数自身是仍引用活动
泛参的 `@value`/tuple 实例，其 C 表达式已经表示该副本的存储地址。旧的实例方法调用路径只识别部分
descriptor-sized 存储，仍会对固定布局开放值再次取址。例如 `Span<Composite<T>>` 参数调用 `slice` 时
错误生成指针载体自身的地址，而不是 Span 副本的地址，callee 因而读取无效字段并抛出边界异常。

通用修复规则是：所有值语义实例方法的隐式 `self` 始终表示接收者存储地址。普通值表达式在调用处取址；
表达式结果已经标记为存储地址时直接复用，不得再次取址。该规则统一覆盖类型自身方法、用户类型 `fit`
方法、方法级泛型方法、开放泛型实例的共享方法及共享体内的 self-call。它不改变显式参数的按值复制
规则：若接收者来自函数值参数，`self` 指向的仍是当前调用边界建立的参数副本；若接收者是调用方变量，
成员修改仍回写该变量。修复不修改 runtime API/ABI，不增加复制、分配、描述符读取或运行时分支，只
纠正生成的 C 地址表达式。

### 7.22 开放泛型用户类型的 fit 调用选错共享符号（Codegen 缺陷）

第六组补充 `fit self` 覆盖时确认：Semantic 已把 `LibValue<T>.replace` 解析到用户 `fit` 成员，Codegen
也注册并生成了该 fit 的共享入口；但开放泛型实例调用分支丢失了已选 fit 身份，仍按类型自身方法构造
`FengGenericMethod...` 符号，最终调用不存在的入口。

通用修复规则是：实例调用选择结果必须把“类型自身成员”与“用户 fit 成员”一直保留到最终入口选择。
开放泛型用户类型的自身成员使用 generic-method 共享符号，fit 成员使用该 fit 声明及成员序号对应的
fit-method 共享符号；二者继续复用相同的 receiver、类型描述符、函数描述符、显式参数和返回槽 ABI。
修复只纠正编译期符号选择，不修改 runtime API/ABI，也不增加生成程序的运行时操作。

### 7.23 地址表示的固定布局开放值无法直接返回（Codegen 缺陷）

同一用例继续确认：固定布局开放泛型值参数通过共享 address ABI 后，其表达式表示存储地址。旧的泛型
返回路径只对 descriptor-sized 聚合识别地址结果；直接 `return subject` 会把地址表达式强制转换成 C
struct，导致宿主 C 编译失败。含托管槽的固定布局值直接返回也存在同类错误，只是此前 `Span` 用例返回
的是方法调用产生的新结果，没有覆盖直接返回参数。

通用修复规则是：泛型共享体返回具体值类型时，必须根据表达式结果是否已经表示存储地址选择源。普通
值表达式保持既有直接赋值；地址式 trivial struct 从该地址复制一次完整值；地址式含托管槽聚合从该
地址复制并执行既有 aggregate retain，使 caller 的返回槽获得独立所有权。该处理正好对应原有按值返回
所需的一次值复制，不增加堆分配、描述符读取或运行时分支，不修改 runtime API/ABI；闭合及非泛型返回
路径保持原发码。
