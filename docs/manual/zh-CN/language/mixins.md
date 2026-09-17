# 成员展开（mixin）

成员展开把来源类型的字段和允许复用的方法加入目标类型。可以只复用字段，也可以通过 `@mixable` 提供可复用的行为。Feng 使用 `...` 声明展开，`mixin` 不是关键字。

展开不建立继承、子类型或来源与目标之间的转换关系，也不自动复制来源的 spec 满足关系。需要使用契约能力时，目标类型仍须显式声明满足相应契约，见[契约与 fit](./contracts-and-fit.md)。

## 纯字段混入与初始化

公开实例字段自动参与展开，不需要也不能重复标注 `@mixable`。只复用字段时，不需要定义 spec：

```feng
module manual_mixin_fields;
import std.io;
import std.numeric;

/** Supplies dimensions and their construction defaults. */
type LayoutFields {
  var width: int = 80;
  var height: int = 24;

  /** Sets the width of a constructed source. */
  func LayoutFields(width: int) { self.width = width; }
}

/** Uses each field type's zero value. */
type ZeroLayout { ...: LayoutFields; }

/** Initializes fields from one explicitly typed source construction. */
type WideLayout { ...: LayoutFields = LayoutFields(100); }

/** Infers the source type from the construction expression. */
type WiderLayout { ... = LayoutFields(120); }

/** Compares all three expansion forms. */
func main(args: string[]) {
  let zero = ZeroLayout {};
  let wide = WideLayout {};
  let wider = WiderLayout {};
  println<int>("{0} {1} {2}", zero.width, wide.width, wider.width);
  println<int>("{0} {1} {2}", zero.height, wide.height, wider.height);
}
```

输出为 `0 100 120` 和 `0 24 24`。

| 写法 | 字段初值 |
| --- | --- |
| `...: Source;` | 各字段的类型零值；不构造 Source，不执行来源字段初值或构造函数 |
| `...: Source = Source(...);` | 每次构造目标时构造一次 Source，使用其构造完成后的字段值 |
| `... = Source(...);` | 与上一种相同，来源类型从构造表达式推导 |

来源必须能确定到具体对象类型的声明；可以是 `Source<T>` 这样的泛型实例，并引用目标类型的泛型参数，但不能直接是裸类型参数 `T`、spec、标量、数组、tuple 或 enum。带初值时，右侧必须是普通对象构造表达式，可以带构造参数和对象字面量；不能换成已有变量、工厂函数调用或条件表达式。

展开后的字段属于目标自身，保留名称、类型、`let` / `var` 和可见性。值字段复制值，引用字段复制引用；字段槽位不会与来源保持后续同步。带初值的展开不会把来源对象保存为隐藏字段，临时来源按普通生命周期规则释放。

零值形式保留生成 `let` 字段的首次显式绑定机会；带初值时，来源已经显式绑定的 `let` 字段在目标中也已完成绑定，不能再绑定一次。对象初始化顺序与绑定规则见[自定义类型](./user-defined-types.md#默认值与初始化顺序)。

## 用 @mixable 复用方法

普通实例方法不参与展开。要复用行为，把实现写成 `@mixable static` 方法，首参数使用来源和目标都已声明满足的对象契约：

```feng
module manual_mixin_methods;
import std.io;
import std.numeric;

spec HasWidth { var width: int; }

/** Supplies a field and an operation expressed through its contract. */
type WidthPart: HasWidth {
  var width: int;

  /** Grows any receiver that satisfies HasWidth. */
  @mixable
  static func grow(target: HasWidth, delta: int): int {
    target.width += delta;
    return target.width;
  }
}

/** Declares its own contract relationship and expands WidthPart. */
type Panel: HasWidth { ...: WidthPart; }

/** Calls the instance entry, target static entry and source static entry. */
func main(args: string[]) {
  let panel = Panel { width: 10 };
  println<int>("{0}", panel.grow(2));
  println<int>("{0}", Panel.grow(panel, 3));
  println<int>("{0}", WidthPart.grow(panel, 4));
}
```

依次输出 `12`、`15`、`19`，三次调用修改的是同一个 `panel`。编译器为 `WidthPart` 和 `Panel` 提供省略首参数的同名实例入口，并为 `Panel` 提供保留完整参数列表的静态入口；目标静态入口转发到来源实现。

首参数必须存在、不能是变长参数，类型必须是 object-form spec；来源和目标都要满足它，仅有相同字段或方法还不够。后续参数、返回类型、泛型参数、变长参数与可见性都随方法保留。当前展开位置可见的 `fit Source` 也可以提供符合这些条件的 `@mixable` 静态方法。

静态字段、未标注的方法、普通实例方法和 `fit Source` 的实例方法不参与展开。来源限定调用 `WidthPart.grow(...)` 始终指向来源实现；只有实现内部通过首参数的契约成员调用时，才会使用该接收者对应的契约实现。

## mix 带来的 seal 授权

`@mixable` 还可以标在具体类型的 `seal` 实例字段上，让字段参与展开；未标注的 seal 字段不参与。方法使用 `@mixable seal static`。展开后的字段、静态入口和实例入口仍为 `seal`，不会因此向普通调用方公开。

直接展开同时授予目标自身的实例方法和静态方法访问相应来源成员的权限：

```feng
module manual_mixin_access;
import std.io;
import std.numeric;

spec HasValue { var value: int; }

/** Supplies explicitly reusable private state and behavior. */
type SecretPart: HasValue {
  var value: int;
  @mixable
  seal var hidden: int = 7;

  /** Provides a restricted operation through the receiver contract. */
  @mixable
  seal static func secret(target: HasValue): int {
    return target.value + 100;
  }
}

/** Receives private members and direct access to the marked source members. */
type Composed: HasValue {
  ...: SecretPart = SecretPart { value: 3 };

  /** Reads the private field generated on this type. */
  func own_hidden(): int { return self.hidden; }

  /** Uses direct mix permission on the original source field and method. */
  func inspect(source: SecretPart): int {
    return source.hidden + SecretPart.secret(self);
  }

  /** Calls this type's generated private instance entry. */
  func call_mixed(): int { return self.secret(); }
}

/** Uses public methods that expose controlled results. */
func main(args: string[]) {
  let source = SecretPart {};
  let composed = Composed {};
  println<int>("{0} {1} {2}", composed.own_hidden(), composed.inspect(source),
    composed.call_mixed());
}
```

输出为 `7 110 103`。`self.hidden` 是目标自己的字段；`source.hidden` 和 `SecretPart.secret(self)` 则访问原始来源，依靠 `Composed` 直接展开 `SecretPart` 获得的授权。授权不要求所访问的来源实例就是初始化时使用的临时实例。

这项权限只覆盖对应来源中标注过的 seal 实例字段和 seal 静态方法。它不开放来源的其他 seal 成员，也不授予顶层函数、目标的 fit 方法、构造函数、普通字段初始化器或其他类型访问权；编译器生成的展开初始化和方法转发按相应展开规则处理。

共同实现同一个 spec、位于同一模块或包内，都不能替代直接 mix 关系。spec 视角自身的 seal 访问权限与此不同，见[spec 成员的 seal 可见性](./modules-and-visibility.md#spec-成员的-seal-可见性)。

## 显式成员、冲突与多层展开

目标显式字段可以优先于与之冲突的来源字段。定制行为时，可以声明自己的 `@mixable static` 方法，按静态签名或省略首参数后的实例签名匹配，取代对应来源候选；需要保留来源行为时，在方法体中显式调用 `Source.method(...)`，再增加自己的处理。

普通实例方法不具备这种替换效果：保留的 `@mixable` 静态方法仍会生成实例入口，再按普通规则检查重载或冲突。不同展开来源之间也没有“先写的优先”规则；无法合法共存的成员会报冲突，直接或间接的展开依赖循环也会报错。

展开可以分多层进行。若 `Middle` 直接展开 `Source`，`Target` 再直接展开 `Middle`，符合条件的字段和方法可以继续参与展开，但 `Target` 得到的是对 `Middle` 相应成员的直接授权，不会因此获得访问 `Source` 原始 seal 成员的权限。

## TUI 中的实际用法

标准库 TUI 用契约表达组件能力，用成员展开复用状态与实现：

| 类型 | 自己声明满足的契约 | 直接展开来源 | 用法 |
| --- | --- | --- | --- |
| `Button` | `Widget` | `View = View()` | 复用基础组件字段与行为 |
| `Container` | `ContainerWidget` | `View = View()` | 增加子组件集合，复用并扩展样式、布局和绘制行为 |
| `VStack` | `VStackWidget` | `Container = Container()` | 定制子组件样式准备和垂直布局 |

其中 `ContainerWidget` 的父契约是 `Widget`，`VStackWidget` 的父契约是 `ContainerWidget`。这些契约关系由各自定义显式声明，不是展开来源附带的继承关系。

`View` 的公开字段自动展开，例如 `frame`、`tabIndex`；内部状态使用 `@mixable seal` 字段，例如 `state`、`rtStyle`、`overrideStyle`。可复用操作使用 `@mixable static`，内部操作再加 `seal`。`Button` 使用 `View()` 的完整构造结果，因此保留了 `tabIndex = -1` 等声明初值。

下面的程序使用实际 TUI 类型，按 `VStack` 的方式定制子组件样式准备。它只演示这一扩展点；完整的 `VStack` 还实现了垂直布局：

```feng
module manual_mixin_tui;
import std;
import std.io;
import std.numeric;
import std.tui.view;
import std.tui.widgets;

spec StartStackWidget: ContainerWidget {}

/** Reuses Container and customizes one of its internal operations. */
type StartStack: StartStackWidget {
  ...: Container = Container();

  /** Preserves source preparation before setting vertical alignment. */
  @mixable
  seal static func prepareChildOverrideStyle(stack: StartStackWidget,
    child: Widget): void {
    Container.prepareChildOverrideStyle(stack, child);
    child.overrideStyle.verticalAlign = Align.Start;
  }

  /** Exercises the customized operation and reads its contract-visible state. */
  func prepare_and_check(child: Widget): bool {
    self.prepareChildOverrideStyle(child);
    return match child.overrideStyle.verticalAlign {
      let alignment: Align { alignment == Align.Start; }
      else { false; }
    };
  }
}

/** Uses the reused public API without starting a terminal event loop. */
func main(args: string[]) {
  let button = Button {};
  let stack = StartStack {};
  stack.addChild(button);
  println<int>("{0}", button.tabIndex);
  if stack.prepare_and_check(button) { println("start"); }
}
```

输出为 `-1` 和 `start`。这里有三个不同的访问入口：

- `stack.addChild(button)` 使用从 `Container` 展开得到的公开实例方法。
- `Container.prepareChildOverrideStyle(stack, child)` 调用来源的 seal 静态方法，依靠 `StartStack` 直接展开 `Container` 获得的授权；即使目标已经定义自己的同名 `@mixable` 方法，该直接授权仍然有效。
- `child.overrideStyle` 通过 `Widget` 视角访问 seal 字段，依靠当前实现类型满足 `Widget`。这不是对 `Button` 或 `View` 具体类型的普通 seal 字段访问。

`StartStack` 并未直接展开 `View`，因此不能仅凭上述展开链直接调用 `View` 原始的 `@mixable seal` 方法。实际 TUI 中，`Container` 在自己直接展开 `View` 的位置调用 `View.doStyling(...)`；`VStack` 则在自己直接展开 `Container` 的位置调用 `Container.prepareChildOverrideStyle(...)`。
