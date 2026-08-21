# Feng 语言可见性规范

本文档定义 Feng 语言的三级可见性模型。

## 1 可见性关键字

- `seal` — 私有，外部不可访问
- `open` — 公开，外部可访问

## 2 三级可见性模型

Feng 的可见性由三级层次共同决定，外部代码访问某个成员时，必须三级均为 `open` 才可达。

### 第一级：module 可见性

- 控制模块自身是否对外可见。
- 默认 `seal`，包外不可导入。
- 需显式声明 `open module` 才可被外部 `import`。

```feng
// 默认 seal，包外不可见
module app.internal.cache;

// 显式 open，包外可导入
open module app.api.user;
```

### 第二级：module 成员可见性

- 控制模块内顶层声明（`let` / `var` / `type` / `spec` / `enum`）是否对外可见。
- 默认 `seal`，仅模块内部可访问。
- 需显式声明 `open` 才可被外部引用。

```feng
open module app.api.user;

// 默认 seal，外部不可见
let internal_cache = [];

// 显式 open，外部可访问
open let MAX_RETRY = 3;

// 显式 open，外部可引用此类型
open type User {
  // ...
}
```

### 第三级：type 成员可见性

- 控制类型内部成员（构造函数 / 方法 / 字段）是否对外可见。
- 默认 `open`，外部可访问。
- 需显式声明 `seal` 才能隐藏。
- `seal` 成员仅允许所属 `type` 自身的成员实现访问；同 module、同包中的其他
  `type` 或顶层函数仍属于外部代码，不得访问。
- `@mixable seal` 成员保持 seal 可见性，但可由直接 mix 目标在受限上下文中访问：
  static 方法规则由
  [Feng 语言函数规范](./feng-function.md#435-mixable-seal-的直接-mix-授权) 定义，
  实例字段规则由
  [Feng 语言类型规范](./feng-type.md#4221-mixable-seal-实例字段) 定义；这些例外不
  适用于其他 seal 成员。
- `@friend(Type, ...)` 可以把一个显式 `seal` 字段或普通方法定向放行给列出的
  具体 type 及本包 `fit Type`；该例外只替代成员自身的 seal 检查，不穿透 module、
  owner type/spec 或 fit 的可见性。

```feng
open type User {
  // 默认 open，外部可访问
  var name: string;

  // 默认 open，外部可调用
  func User(name: string) {
    self.name = name;
  }

  // 显式 seal，外部不可调用
  seal func resetInternal() {
    // ...
  }
}
```

## 3 可见性组合效果

外部代码能否访问某个 type 成员，取决于三级可见性的组合：

| module | module 成员 (type) | type 成员 | 外部可访问 |
|--------|-------------------|-----------|-----------|
| open   | open              | open      | 是        |
| open   | open              | seal      | 否        |
| open   | seal              | open      | 否        |
| seal   | open              | open      | 否        |

在 `seal` 模块中声明 `open type`，等价于其他语言中的 `internal`——同包内可见，包外不可访问。

## 4 包内可见类型

当需要一个类型仅在包内共享、但不对外暴露时，将 module 保持默认 `seal`，同时将 type 声明为 `open`：

```feng
// module 默认 seal，包外无法 import 此模块
module app.internal.cache;

// type 声明为 open，同包其他模块可访问
open type CacheEntry {
  var key: string;
  var value: string;

  func CacheEntry(key: string, value: string) {
    self.key = key;
    self.value = value;
  }
}
```

同包内的其他模块可正常导入并使用：

```feng
module app.internal.store;

import app.internal.cache;

func save(k: string, v: string) {
  let entry = CacheEntry(k, v);
  // ...
}
```

包外代码无法导入 `app.internal.cache`，因此 `CacheEntry` 对外不可见。

## 5 完全公开类型

module、type、type 成员三级均为 `open`，外部可自由导入、实例化、访问所有成员：

```feng
open module std.io;

open type Writer {
  var path: string;

  func Writer(path: string) {
    self.path = path;
  }

  func write(data: string) {
    // ...
  }
}
```

## 6 公开类型但隐藏部分成员

type 对外公开，但通过 `seal` 隐藏内部实现细节：

```feng
open module app.api.session;

open type Session {
  // 外部可读
  var id: string;

  // 外部不可访问
  seal var token: string;

  func Session(id: string, token: string) {
    self.id = id;
    self.token = token;
  }

  // 外部可调用
  func isValid(): bool {
    return self.checkToken();
  }

  // 外部不可调用
  seal func checkToken(): bool {
    // ...
  }
}
```

外部代码可以创建 `Session`、调用 `isValid()`，但无法访问 `token` 字段或调用 `checkToken()`。

## 7 禁止外部实例化

type 对外公开但构造函数标记为 `seal`，外部只能通过工厂方法获取实例：

```feng
open module app.api.connection;

open type Connection {
  var host: string;

  // 外部不可直接调用构造函数
  seal func Connection(host: string) {
    self.host = host;
  }

  // 外部通过此方法获取实例
  static func create(host: string): Connection {
    // 校验、池化等逻辑
    return Connection(host);
  }
}
```

外部代码只能调用 `Connection.create()`，无法直接 `Connection(host)`。

## 8 模块私有类型

在 `open` 模块中，type 默认为 `seal`，仅当前模块内部可用，同包其他模块也无法访问：

```feng
open module app.api.user;

// 默认 seal，仅本模块内部使用
type PasswordHasher {
  func PasswordHasher() {}

  func hash(plain: string): string {
    // ...
  }
}

// 对外公开
open type User {
  var name: string;

  func User(name: string) {
    self.name = name;
  }
}
```

外部代码可以访问 `User`，但 `PasswordHasher` 仅 `app.api.user` 模块内部可见。

## 9 默认值汇总

| 层级           | 默认可见性 |
|---------------|-----------|
| module        | `seal`    |
| module 成员    | `seal`    |
| type 成员      | `open`    |
| object-form spec 成员 | `open` |

## 10 有效可见范围

可见范围从窄到宽分为：

1. **类型私有**：仅所属 `type` 或 `fit` 内可用。
2. **模块私有**：仅声明所在 module 内可用。
3. **包内可见**：当前包内可用，包外不可用。
4. **包外公开**：包内、包外均可用。

有效可见范围按以下规则计算：

- module 为 `open` 时不收窄范围；module 为 `seal` 时收窄到包内可见。
- 顶层声明为 `open` 时不收窄范围；为 `seal` 或省略修饰时收窄到模块私有。
- `type`、`fit` 成员为 `open` 或省略修饰时不收窄范围；为 `seal` 时收窄到类型私有。
- 声明的有效可见范围是 module、所属顶层声明和成员各层范围中的最窄者。
- object-form `spec` 成员省略修饰时为公开 requirement；显式 `open` 具有
  相同语义，显式 `seal` 收窄 spec 访问面。显式修饰事实由 Parser / AST
  保留，有效可见性由 Semantic 解释。
- `fit` 不是可命名声明：仅 `open module` 中的 `open fit` 形成包外公开签名；
  其他 `fit` 仅在声明 module 内生效，按模块私有检查。

因此，`seal module` 中的 `open` 顶层声明为包内可见；`open module` 中未标记
`open` 的顶层声明为模块私有。

同名成员方法形成重载集合时，访问方必须先排除在当前 type 作用域中不可见的候选，
再按参数签名进行重载解析。不可见的 `seal` 候选不得遮蔽同名的可见候选；仅当同名
候选全部不可见时，成员访问才按不可见处理。

### 10.1 object-form `spec seal` 成员

`spec seal` 是契约视角的成员访问控制，不是具体 type 成员的可见性：

- 公开和 `seal` spec 成员都进入完整契约与 witness；普通 spec 使用者只能
  访问公开成员。
- 满足成员原声明 spec 的 type，可以在自身成员方法、静态方法及其 fit
  扩展方法中通过 spec 视角访问该 `seal` 成员。
- 权限按访问点的实现上下文和成员原声明 spec 在编译期判断，不按运行时
  接收者具体类型、module 或包判断。
- 该权限不得用于具体 type 视角的成员查找，也不得改变实现成员本身的
  `open` 或 `seal` 可见性。

requirement 与实现成员的可见性兼容矩阵由
[Feng 语言 `spec` 规范](./feng-spec.md) 唯一定义。

### 10.2 `@mixable seal` 成员

`@mixable seal static` 仍是具体 type 或 fit 的 seal 成员，普通成员查找必须继续按
seal 拒绝。只有 [Feng 语言函数规范](./feng-function.md#435-mixable-seal-的直接-mix-授权)
定义的直接 mix 目标实现上下文可以选择和调用对应方法；该授权不改变所属 type 的其他
seal 成员，也不来自 module、包、继承或 spec/witness 关系。

`@mixable seal` 实例字段仍是具体 type 的 seal 成员。只有
[Feng 语言类型规范](./feng-type.md#4221-mixable-seal-实例字段) 定义的直接 mix 目标
实例方法或静态方法可以访问对应 Source 字段；`let` / `var`、静态性、owner、module、
spec/witness 和 fit 规则均继续执行。字段的 `is_mixable` 事实单独存在不构成访问权。

### 10.3 `@friend` seal 成员

`@friend(F1, F2, ...)` 是成员级定向 seal 授权，遵循以下规则：

- 只能标注 `type`、object-form `spec` 或 `fit` 中显式声明为 `seal` 的实例字段、
  静态字段、实例普通方法或静态普通方法；fit 仍不能声明字段。
- 构造函数和终结器禁止标注 `@friend`。需要受限构造时，应使用 seal 构造函数配合
  `@friend` seal static 工厂方法；该限制只约束注解目标，不限制构造函数和终结器
  消费所属 type 已取得的 friend 权限。
- 每个参数都在注解声明位置按类型位解析，根类型必须是具体 `type`；spec、内建
  标量、数组、指针和单独类型参数不能作为 friend 主体。短名、别名和完整路径只
  影响名称解析，不影响最终授权身份。
- 同一成员上的一个或多个 `@friend` 注解最终归一化为一个 friend 集合；按完成
  泛型代入后的语义类型身份静默去重。
- friend type 的词法类型实现上下文可以使用授权。类型实现上下文包括实例字段和
  静态字段的初始化表达式、实例普通方法和静态普通方法、构造函数及终结器；嵌套
  lambda 继承其外层类型实现上下文的词法 owner type。friend 不改变字段初始化顺序、
  `self` 捕获或其他既有初始化规则。
- 与该成员同包声明且目标类型语义等于 friend type 的 `fit FriendType` 实例方法和
  静态方法也可以使用授权。
- `fit FriendType` 只取得目标 type 对该具体成员的 friend 身份，不因此成为目标
  type 自身，也不能访问未标注相应 `@friend` 的其他 seal 成员。
- 顶层函数、其他 type、其他包中的 fit、传递 friend、共同 spec 实现、共同 fit 或
  mix 关系均不产生授权。
- 授权只能在 module、owner type/spec、fit 可见性、静态性、可变性、泛型和重载等
  既有规则全部通过后，替代最后一层成员 seal 检查。
- 重载选择必须先移除对当前上下文不可访问的候选，再执行签名匹配；不可访问的
  friend/seal 候选不能遮蔽同名 open 候选。
- object-form spec 成员的 friend 访问只发生在 spec 视角并继续通过既有 witness；
  不要求 friend type 实现该 spec，也不扩大具体实现成员的可见性或修改满足规则。
- 已经支持的具体 type/fit 方法值在形成点执行同一 friend 检查；object-form spec
  方法值是否可形成由其独立规范和开发项决定，`@friend` 不新增该能力。
- `@friend` 不写入 package-public `.ft`，不改变成员本身的既有 Symbol 选择规则，
  也不增加运行时访问检查或 ABI。

## 11 公开签名的可见性一致性

声明签名中每个组成类型的有效可见范围不得小于该声明的有效可见范围。

检查范围：

- 顶层 `let`、`var`、函数的类型、参数、返回类型和泛型约束。
- `type` 的泛型约束、父 `spec`，以及实例和静态字段、方法、构造函数的公开签名。
- `spec` 的泛型约束、父 `spec`，以及 object、callable、union、intersection
  四种 form 的组成类型。
- `fit` 的目标类型、`spec` 列表，以及实例和静态成员的公开签名。
- `@mixable seal static` 的完整签名按所属 type 或 fit 中同位置 open 方法的有效可见
  范围检查；未标注 `@mixable` 的普通 seal 方法仍按类型私有范围处理。
- `@mixable seal` 实例字段的完整类型按所属 type 中同位置 open 字段的有效可见范围
  检查；未标注 `@mixable` 的普通 seal 字段仍按类型私有范围处理。
- 显式类型和推导类型；推导类型在类型推导完成后检查。

组成类型按以下规则递归检查：

- 泛型实参、数组元素和指针目标继续参与检查。
- callable 的参数和返回类型继续参与检查。
- 泛型参数本身不参与比较，其约束类型参与检查。
- 具名类型按语义分析已解析的目标声明计算有效可见范围，不按名称或 `.ft`
  是否收录判断。

可见性不一致使用 `AE0327`，在提供方语义分析阶段报错。显式类型以产生不一致的
类型引用为诊断位置；推导类型没有对应类型引用时，以声明位置为诊断位置。存在该
错误时不得生成公开 `.ft`。

```feng
open module app.api;

type Hidden {}

open let value: Hidden;                 // AE0327
open func create() -> Hidden;           // AE0327
open func consume(value: Hidden);       // AE0327
open func process<T: Hidden>(value: T); // AE0327
open let values: List<Hidden>;          // AE0327

open type Public {
  seal let hidden: Hidden;              // 合法：类型私有
  let exposed: Hidden;                  // AE0327：成员默认 open
  static let shared: Hidden;            // AE0327
}
```

在 `seal module` 中，`open` 声明可以使用同一包内其他 `seal module` 的 `open`
类型；两者均为包内可见。包外公开声明不能使用该类型。

### 11.1 `@friend` 成员签名可见性

设被标注成员的 owner 声明 module 为 `OM`，一个 friend type 的声明 module 为
`FM`，成员完整签名中递归出现的具名类型 `R` 的声明 module 为 `RM`：

1. `FM == OM` 时，该 friend type 的签名检查直接通过。
2. `FM != OM` 时，若存在 `RM == OM` 且 `R` 自身不是 `open`，则该
   `@friend` 声明非法；其他已经在 owner 声明位置成功解析的类型通过。

多个 friend type 分别检查，任意一个失败都使整个成员声明非法。完整签名递归范围
包括字段显式或推导类型、参数、显式或推导返回类型、方法泛型约束、泛型实参、数组
元素和指针目标；类型参数本身不参与比较，其约束继续递归检查。

同包 `fit FriendType` 使用授权时，还要把 fit 声明 module 作为 `FitM` 在访问点执行
同构检查：`FitM == OM` 时通过；否则，若签名含有 `RM == OM` 且非 `open` 的
类型，则该次 fit 访问被拒绝。该访问点检查不能挽救原本未通过 friend type 声明
检查的 `@friend` 声明。

## 12 私有成员的表示类型

私有成员可以使用在成员声明位置可访问的私有类型。成员类型是否为泛型不改变该
规则；泛型实参中的类型按相同规则处理。

```feng
open module app.api.box;

type Entry<T> {
  let value: T;
}

open type Box<T> {
  seal let entry: Entry<T>;
}
```

`Box<T>.entry` 为私有成员，因此可以使用当前模块的私有类型 `Entry<T>`。
其他模块仍不能直接引用、导入或构造 `Entry<T>`。
