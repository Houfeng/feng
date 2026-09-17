# 自定义类型

`type` 把数据和行为组织为具名类型。普通对象类型是托管引用类型；具名元组和标注 `@value` 的对象类型是值类型。

## 字段与对象字面量

```feng
type User {
  let id: int;
  var name: string;
}

let user = User { id: 1, name: "Alice" };
user.name = "Bob";
```

`let` 字段在完成初始化后不可修改，`var` 字段可以修改。对象赋值复制引用：

```feng
let alias = user;
alias.name = "Carol";
println(user.name); // Carol
```

## 构造函数

构造函数与类型同名，通过 `self` 初始化当前对象：

```feng
type User {
  let id: int;
  var name: string;

  func User(id: int, name: string) {
    self.id = id;
    self.name = name;
  }
}

let user = User(1, "Alice");
let renamed = User(2, "Bob") { name: "Carol" };
```

花括号形式的对象类型没有显式构造函数时，可使用默认无参构造和对象字面量。对象字面量也可以在构造后
覆盖尚可绑定或可写的成员。圆括号形式的具名 tuple 不适用这条规则，它没有普通构造函数。

## 方法

```feng
type Counter {
  var value: int;

  func increment() {
    self.value += 1;
  }

  func current(): int {
    return self.value;
  }
}
```

实例方法通过 `self` 访问当前实例。方法可以重载，但不能只依靠返回类型区分。

## 静态成员

```feng
type Counter {
  open static var created: int = 0;

  open static func create(): Counter {
    Counter.created += 1;
    return Counter {};
  }

  var value: int;
}

let counter = Counter.create();
```

静态成员通过类型名访问，不能通过实例访问。

## 终结器

需要释放外部资源时，可以定义终结器：

```feng
type Resource {
  var handle: int;

  func Resource(handle: int) {
    self.handle = handle;
  }

  func ~Resource() {
    // 释放由 handle 表示的外部资源
  }
}
```

托管内存由 Feng 自动管理；终结器适合清理文件句柄等非托管资源。可预测的词法清理优先使用 `defer`，详见[异常处理](./error-handling.md)。

## 值类型与方法值捕获

用 `@value` 标注花括号类型后，赋值、传参和返回会复制值。下面的完整程序同时演示值字段独立、引用字段共享，以及方法值保存自己的接收者副本：

```feng
module manual_value;
import std.io;
import std.numeric;

/** Shared reference stored inside a value. */
type SharedCount { var count: int; }

/** Mutable value with a shared reference field. */
@value
type Position {
  var x: int;
  let shared: SharedCount;

  /** Changes the current receiver storage. */
  func add(delta: int): int {
    self.x += delta;
    return self.x;
  }
}

spec Step(delta: int): int;

/** Changes a parameter copy and returns its value. */
func shifted(value: Position): Position {
  value.add(5);
  return value;
}

/** Prints the observable copy boundaries. */
func main(args: string[]) {
  let original = Position { x: 1, shared: SharedCount {} };
  let copy = original;
  copy.add(2);
  copy.shared.count = 9;
  let moved = shifted(original);
  let step: Step = original.add;
  let first = step(2);
  let second = step(1);
  println<int>("{0} {1} {2}", original.x, copy.x, moved.x);
  println<int>("{0}", original.shared.count);
  println<int>("{0} {1} {2}", first, second, original.x);
}
```

输出依次为 `1 3 6`、`9`、`3 4 1`。直接调用实例方法时，`self` 引用当前接收者的存储，不会再复制一次；形成 `original.add` 方法值时才保存值副本，后续调用继续使用该副本。普通引用对象的方法值保存对象引用。更多可调用用法见[函数](./functions.md)。

| 形式 | Feng 内部的复制行为 | 使用重点 |
| --- | --- | --- |
| 普通 `type` | 复制引用 | 多个绑定访问同一对象 |
| `@value type` | 复制字段值 | 引用字段仍指向同一对象，不是深拷贝 |
| 具名 tuple | 复制元素值 | 元素不可原地修改，可整体替换 |
| `@abi type` | 仍复制对象引用 | 约束 C ABI 布局，不代表 Feng 值语义 |

具名 tuple 的创建方式见[类型](./types.md)，ABI 传参方式见[C 互操作](../interop/c-interop.md)。

## 成员展开与 @mixable

成员展开把来源的字段和允许复用的行为加入目标类型，不建立来源到目标的继承或转换关系。目标需要自己声明所满足的契约。三种形式的差别在于是否先构造来源值：

```feng
module manual_mixin;
import std.io;
import std.numeric;

spec Counted { var value: int; }

/** Supplies reusable fields and behavior. */
type CounterPart: Counted {
  var value: int = 4;
  @mixable
  seal var hidden: int = 5;

  /** Initializes the source instance. */
  func CounterPart(seed: int) { self.value = seed; }

  /** Becomes a callable instance operation on each participating type. */
  @mixable
  static func bump(target: Counted): int {
    target.value += 1;
    return target.value;
  }

  /** Supplies a restricted reusable operation. */
  @mixable
  seal static func secret(target: Counted): int { return target.value + 100; }
}

/** Expands fields with their type defaults. */
type ZeroCounter: Counted { ...: CounterPart; }

/** Constructs one source value before copying its selected fields. */
type SeededCounter: Counted {
  ...: CounterPart = CounterPart(10);

  /** Reads the generated seal field from its owning type. */
  func hidden_value(): int { return self.hidden; }

  /** Uses the generated seal method inside its owning type. */
  func secret_value(): int { return self.secret(); }
}

/** Infers the source type from its construction expression. */
type InferredCounter: Counted { ... = CounterPart(20); }

/** Compares all three initialization forms. */
func main(args: string[]) {
  let zero = ZeroCounter {};
  let seeded = SeededCounter {};
  let inferred = InferredCounter {};
  println<int>("{0} {1} {2}", zero.value, seeded.value, inferred.value);
  println<int>("{0} {1} {2}", zero.bump(), seeded.hidden_value(), seeded.secret_value());
}
```

输出为 `0 10 20` 和 `1 5 110`。`...: Source;` 按生成字段各自的类型取零值，不执行来源字段初值或构造函数；带 `= Source(...)` 的两种形式每次构造目标时只构造一次来源，再使用其最终字段值。右侧必须是对象构造表达式，不能替换成任意变量或工厂函数调用。引用字段复制引用，生成字段与来源字段不保持后续同步。

公开实例字段自动参与展开；普通实例方法、静态字段和未标注的方法不参与。复用行为时，`@mixable` 静态方法的首参数必须是来源和目标都已声明满足的对象契约；它同时提供省略首参数的实例调用形式。目标显式成员优先；不同展开来源之间没有优先级，无法按普通成员规则共存的冲突会报错，循环展开也会报错。

`@mixable seal` 字段和方法展开后仍是 `seal`。直接展开目标的自身实例／静态方法可使用对应来源的受限能力，但授权不会传给顶层函数、其他类型或间接展开目标。来源字段授权不适用于目标的 `fit` 方法、构造函数或普通字段初始化器；自动生成的展开初始化由编译器处理。方法授权仅覆盖标注过的 seal 静态方法，不开放其他 seal 成员。

## 默认值与初始化顺序

无初始值绑定使用类型默认值，不等于执行无参构造。对象构造按字段声明初值、构造函数、对象字面量的顺序执行：

```feng
module manual_initialization;
import std.io;
import std.numeric;

/** Exposes constructor and literal initialization separately. */
type Record {
  let id: int;
  var stage: int = 1;

  /** Binds id once and advances the writable field. */
  func Record(id: int) { self.id = id; self.stage = 2; }
}

/** Compares default values with ordinary construction. */
func main(args: string[]) {
  let zero: Record;
  let ready = Record(7) { stage: 3 };
  println<int>("{0} {1}", zero.id, zero.stage);
  println<int>("{0} {1}", ready.id, ready.stage);
}
```

输出为 `0 0` 和 `7 3`。`let` 字段在三个构造阶段合计只能最终显式绑定一次；上例不能再在对象字面量中写 `id: 8`。没有声明初值的字段先取得类型零值，这一步不占用 `let` 的首次显式绑定机会；构造结束后，未显式绑定的 `let` 也不能再改写。

对象零值递归初始化字段，必须能有限展开；自引用或互引用对象不能直接作为默认零值目标。构造函数或对象字面量的后续赋值不能免除第一阶段的非法默认值，只有字段声明初值可以在该阶段直接提供值。数组默认空数组，tuple 逐元素取默认值，enum 取第一项，联合契约取第一个直接成员的默认值；相关用法见[类型](./types.md)和[模式匹配](./pattern-matching.md)。

## 内存与资源生命周期

普通对象、字符串、数组和闭包由自动引用计数管理。绑定、字段和闭包持有的强引用会保持对象存活。返回闭包时，捕获的局部绑定可以比创建它的函数存活更久：

```feng
module manual_lifetime;
import std.io;
import std.numeric;

/** Reference object kept alive by a closure. */
type Label { var text: string; }

spec ReadLabel(): string;

/** Returns a closure that retains access to a local binding. */
func make_reader(): ReadLabel {
  let label = Label { text: "still alive" };
  return () -> label.text;
}

/** Uses the captured object after make_reader returns. */
func main(args: string[]) {
  let reader = make_reader();
  println(reader());
}
```

输出为 `still alive`。捕获 `var` 绑定的闭包与外层代码共享该绑定；方法值的接收者捕获方式见前文值类型示例。普通对象最后一个强引用消失时进入释放流程；不可达的循环引用由循环检测器回收，不能依赖环内对象的具体释放时刻或终结器顺序。

托管对象的内存与文件句柄、套接字、C 内存等外部资源是两回事。外部资源应提供显式 `close`／释放操作，并在取得资源后立即登记 `defer`；终结器作为遗漏清理的补充，清理应能处理已关闭状态。终结器不可直接调用，异常必须在其内部捕获。不要依赖固定执行线程、循环组内其他对象仍处于未清理状态，或通过保存 `self` 使终结器反复延后释放。

借给 C 的 `T*` 本身不持有托管对象；指针有效期间应持续保留对应 owner，不能只保存指针。具体代码见[C 互操作](../interop/c-interop.md)，作用域清理与抛错边界见[异常处理](./error-handling.md)。
