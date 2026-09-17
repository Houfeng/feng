# 契约与 fit

`spec` 声明能力边界，`type` 提供实现，`fit` 在不修改原类型定义的情况下建立满足关系或补充方法。

## 对象契约

```feng
spec Named {
  let name: string;

  func display(): string;
}

type User: Named {
  let name: string;

  func display(): string {
    return self.name;
  }
}

func print_name(value: Named) {
  println(value.display());
}
```

字段必须在名称、类型和 `let`/`var` 方式上匹配；方法必须在名称、参数与返回类型上匹配。声明头中的关系表示类型定义者主动承诺满足契约。

## 契约视角

已经通过 `type` 声明头或当前可见的 `fit` 建立满足关系后，具体值可以直接进入对应的对象契约位置。
对于对象形式（object-form）的 `spec`，父子关系已在定义时显式声明，因此子契约可以沿该关系自动
投影到直接或间接父契约视角。这些基于已声明关系的契约视角建立不视为隐式类型转换。绑定、赋值、
传参、返回、字段和数组元素写入都可以根据目标类型建立相应视角：

```feng
spec Profile: Named {}

/** 同时满足 Profile 及其父契约 Named。 */
type Member: Profile {
  let name: string;

  /** 返回成员名称。 */
  func display(): string {
    return self.name;
  }
}

/** 返回同一值的父契约视角。 */
func as_named(value: Profile): Named {
  return value;
}

let member = Member { name: "Alice" };
let profile: Profile = member;
let named: Named = profile;
let parent = as_named(profile);
print_name(member);
print_name(profile);
```

`spec Profile: Named {}` 已经显式声明父子关系，因此 `profile` 可以自动投影到 `Named` 视角，
无需手写 `(Named)profile`；显式写出该转换也可以建立同一个父视角。仅有相同字段或方法不会自动
建立满足关系，也不会因为运行时对象恰好满足某个契约而自动接受该目标类型。

## 可调用契约

```feng
spec Mapper(value: int): int;

let double: Mapper = (value: int) -> value * 2;
println("{0}", double(21));
```

可调用形式只描述函数签名，适合 Lambda、顶层函数和方法值。它不能出现在 `type Foo: Mapper` 或 `fit Foo: Mapper` 中。

## 联合契约

```feng
spec Identifier: int | string;

let id: Identifier = "user-42";
let label = match id {
  number: int { "numeric" }
  text: string { text }
  else { "unknown" }
};
```

联合形式表达“成员之一”，必须先通过 `match` 收窄才能按具体成员使用。

## 交叉契约

```feng
spec Readable {
  func read(): string;
}

spec Writable {
  func write(value: string): void;
}

spec ReadWrite: Readable & Writable;
```

交叉形式组合多个对象契约，可用于类型位置或泛型约束。具体类型应分别声明满足组成它的对象契约，而不是直接声明满足交叉契约。

交叉契约到组成契约的投影仍须显式书写。例如，将 `ReadWrite` 值 `value` 转换到 `Readable` 视角时，
应写 `(Readable)value`，不能直接把 `value` 赋给 `Readable` 绑定。

## 通过 fit 满足契约

```feng
spec DisplayName {
  func display_name(): string;
}

type Account {
  let name: string;
}

fit Account: DisplayName {
  func display_name(): string {
    return self.name;
  }
}
```

`fit` 适合不能修改原类型，或希望把适配关系放在独立模块中的场景。

## 使用 fit 扩展方法

不列出目标 `spec` 时，`fit` 可以只补充方法：

```feng
fit Account {
  func greeting(): string {
    return "Hello, " + self.name;
  }
}
```

扩展是否可跨模块使用由模块和 `fit` 的可见性共同决定。需要导出时，在公开模块中使用 `open fit`；导入该模块后，关系和扩展方法才在当前文件中生效。

## 使用建议

- 类型自身天然承担的契约写在 `type` 声明头。
- 第三方适配或按模块启用的能力使用 `fit`。
- 不要把 `spec` 当作实现继承；它只描述可见契约。
- 不要依赖隐式结构匹配；满足关系必须显式声明。

## 多父契约、静态能力与 seal requirement

对象契约用逗号列出多个父契约，交叉契约用 `&` 组合已有契约。静态能力通过类型名或受约束类型参数访问；`seal` requirement 限制的是契约视角上的访问：

```feng
module manual_contract_advanced;
import std.io;
import std.numeric;

spec NamedDevice { func name(): string; }

spec Capacity {
  static let maximum: int;

  static func hint(): int;
}

spec InternalDevice { seal func secret(): int; }

spec DeviceContract: NamedDevice, Capacity, InternalDevice {}

spec DeviceView: NamedDevice & Capacity;

spec Query(): int;

/** Implements all parents and exposes a controlled internal operation. */
type Device: DeviceContract {
  static let maximum: int = 32;

  /** Supplies a static contract requirement. */
  static func hint(): int { return 16; }

  /** Supplies the public name. */
  func name(): string { return "device"; }

  /** Implements a restricted requirement. */
  seal func secret(): int { return 9; }

  /** The implementing type can use its restricted contract view. */
  func inspect(): int {
    let view: InternalDevice = self;
    return view.secret();
  }
}

/** Uses the static capability of a constrained type argument. */
func capacity<T: Capacity>(): int { return T.maximum + T.hint(); }

/** Forms a static method value from the same constrained surface. */
func query<T: Capacity>(): Query { return T.hint; }

/** Projects an intersection member explicitly. */
func main(args: string[]) {
  let device = Device {};
  let view: DeviceView = device;
  let named = (NamedDevice)view;
  let get_hint = query<Device>();
  println(named.name());
  println<int>("{0} {1} {2}", capacity<Device>(), device.inspect(), get_hint());
}
```

输出为 `device` 和 `48 9 16`。多父契约会合并各父契约的要求；同名签名必须满足一致性要求，不能借父列表隐藏不兼容的成员。交叉投影保持原对象身份，嵌套交叉成员及成员的父契约也可沿声明关系显式投影；只是拥有相同成员集合的另一交叉契约不自动成为可转换目标。

公开 requirement 必须由公开成员满足；seal requirement 可以由符合规则的公开或 seal 成员满足。它不会自动改变具体实现成员的可见性。普通外部调用不能通过契约视角访问 seal requirement；实现类型以及符合条件的同包 fit 可按其权限访问。静态字段只能由类型自身提供，fit 可提供静态方法。更多泛型组合见[泛型](./generics.md)，定向开放 seal 能力见[模块与可见性](./modules-and-visibility.md)。

## 泛型与其他类型的 fit

fit 可以适配泛型对象，也可以扩展标量、字符串、数组、具名 tuple 和 enum。它只补方法和满足关系，不能添加实例字段或静态绑定：

```feng
module manual_fit;
import std.io;
import std.numeric;

spec Reader<T> { func read(): T; }

/** A generic payload container. */
type Box<T> { let value: T; }

fit Box<T>: Reader<T> {
  /** Reads the closed payload type. */
  func read(): T { return self.value; }

  /** Adds a static factory without adding storage. */
  static func wrap(value: T): Box<T> { return Box<T> { value: value }; }
}

fit i32 {
  /** Adds one to the scalar receiver. */
  func plus_one(): i32 { return self + 1; }
}

fit string {
  /** Surrounds text with brackets. */
  func tagged(): string { return "[" + self + "]"; }
}

fit int[] {
  /** Adds the array elements. */
  func total(): int {
    var result = 0;
    for let value in self { result += value; }
    return result;
  }
}

/** A named tuple with extension behavior. */
type Coordinates(int, int);

fit Coordinates {
  /** Adds the two tuple elements. */
  func sum(): int { return self.item1 + self.item2; }
}

enum Stage { Idle, Ready }

fit Stage {
  /** Tests one named enum case. */
  func is_ready(): bool { return self == Stage.Ready; }
}

spec HasName { let name: string; }

/** Already provides the required field. */
type NamedRecord { let name: string; }

fit NamedRecord: HasName;

/** Uses each extension through its normal receiver. */
func main(args: string[]) {
  let box = Box<int>.wrap(3);
  let number: i32 = 4;
  let values: int[] = [1, 2, 3];
  let point: Coordinates = (3, 4);
  let named: HasName = NamedRecord { name: "record" };
  println<int>("{0} {1} {2}", box.read(), values.total(), point.sum());
  println<i32>("{0}", number.plus_one());
  println("fit".tagged());
  println(named.name);
  if Stage.Ready.is_ready() { println("ready"); }
}
```

输出依次为 `3 6 7`、`5`、`[fit]`、`record`、`ready`。`fit Box<T>` 的 `T` 引用目标类型已有的泛参，不能改名、换序、增减或改成 `fit Box<int>` 来特化；每个闭合实例按自己的实参使用这些方法。无块体的 `fit NamedRecord: HasName;` 只声明已有成员满足契约。

可见性由 fit 所在模块和 `open fit` 决定，使用方需要导入相应扩展模块。若适配双方的实现都来自其他包，属于孤儿适配：关系只在当前包生效，即使写 `open fit` 也不会导出这条关系。无契约目标的纯方法扩展不受这条孤儿关系限制。fit 不能因位于同包就直接访问目标类型的 seal 成员；定向授权见[模块与可见性](./modules-and-visibility.md)。
