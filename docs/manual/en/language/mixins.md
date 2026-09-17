# Member Expansion (mixin)

Member expansion adds fields and eligible reusable methods from a source type to a target type. You can reuse fields alone or provide reusable behavior with `@mixable`. Feng declares expansion with `...`; `mixin` is not a keyword.

Expansion establishes no inheritance, subtype relationship or conversion between source and target. It also does not copy the source's spec relationships. The target must explicitly declare any contracts it needs to satisfy; see [Contracts and fit](./contracts-and-fit.md).

## Field-Only Expansion and Initialization

Public instance fields participate automatically. They neither need nor permit a redundant `@mixable` annotation. Reusing fields alone does not require a spec:

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

The output is `0 100 120` and `0 24 24`.

| Form | Field initialization |
| --- | --- |
| `...: Source;` | Each field type's zero value; does not construct Source or run its field initializers or constructor |
| `...: Source = Source(...);` | Constructs Source once per target construction and uses its completed field values |
| `... = Source(...);` | As above, with the source type inferred from the construction expression |

The source must resolve to a concrete object type declaration. A generic instance such as `Source<T>` may refer to the target type's generic parameters, but a bare type parameter `T`, spec, scalar, array, tuple or enum cannot be the source. When an initializer is present, its right-hand side must be an ordinary object construction expression, optionally including constructor arguments and an object literal. An existing variable, factory call or conditional expression is not accepted.

Expanded fields belong to the target itself and retain their names, types, `let` / `var` mutability and visibility. Value fields copy values, and reference fields copy references; field slots do not stay synchronized with the source. Initialized expansion does not retain a hidden source field. The temporary source is released under ordinary lifetime rules.

The zero-value form leaves generated `let` fields available for their first explicit binding. With an initializer, a source `let` field that was explicitly bound is also fully bound in the target and cannot be bound again. See [User-Defined Types](./user-defined-types.md#defaults-and-initialization-order) for construction order and binding rules.

## Reusing Methods with @mixable

Ordinary instance methods do not participate in expansion. To reuse behavior, write a `@mixable static` method whose first parameter is an object contract explicitly satisfied by both source and target:

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

The output is `12`, `15` and `19`. All three calls modify the same `panel`. The compiler provides both `WidthPart` and `Panel` with a same-named instance entry that omits the first parameter. It also provides `Panel` with a static entry retaining the full parameter list and forwarding to the source implementation.

The first parameter must exist, must not be variadic and must have an object-form spec type. Both source and target must satisfy that contract; matching fields or methods alone are insufficient. The remaining parameters, return type, generic parameters, variadic parameter and visibility are preserved. A `fit Source` visible at the expansion site can also supply `@mixable` static methods that meet these conditions.

Static fields, unannotated methods, ordinary instance methods and instance methods from `fit Source` do not participate. A source-qualified call such as `WidthPart.grow(...)` always selects the source implementation. Receiver-specific contract dispatch occurs only when the implementation calls a contract member through its first parameter.

## Seal Access Granted by Direct Expansion

`@mixable` can also mark a concrete type's `seal` instance field for expansion; unmarked seal fields do not participate. For methods, use `@mixable seal static`. The expanded fields, static entries and instance entries remain `seal`; expansion does not make them public to ordinary callers.

Direct expansion also permits the target's own instance and static methods to access the corresponding marked source members:

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

The output is `7 110 103`. `self.hidden` is the target's own field. In contrast, `source.hidden` and `SecretPart.secret(self)` access the original source under the permission granted by `Composed` directly expanding `SecretPart`. The accessed source instance need not be the temporary instance used during initialization.

This permission covers only the corresponding source's marked seal instance fields and seal static methods. It does not expose other seal members or grant access to top-level functions, the target's fit methods, constructors, ordinary field initializers or other types. Compiler-generated expansion initialization and method forwarding follow their respective expansion rules.

Sharing a spec, module or package does not replace a direct mix relationship. Access to seal members through a spec view is a separate permission; see [Seal Members in Object Contracts](./modules-and-visibility.md#seal-members-in-object-contracts).

## Explicit Members, Conflicts and Multiple Expansion Layers

An explicit target field takes precedence over a conflicting source field. To customize behavior, declare your own `@mixable static` method. Matching by the static signature or the instance signature with the first parameter omitted can replace a corresponding source candidate. To retain source behavior, explicitly call `Source.method(...)` before adding your own processing.

An ordinary instance method does not provide this replacement behavior. A retained `@mixable` static method still generates an instance entry, after which ordinary overload and conflict checks apply. Multiple expansion sources have no declaration-order priority. Members that cannot legally coexist cause a conflict, and direct or indirect expansion dependency cycles are errors.

Expansion can occur in multiple layers. If `Middle` directly expands `Source`, and `Target` directly expands `Middle`, eligible fields and methods can participate again. However, `Target` receives direct permission for the corresponding members of `Middle`, not permission to access the original seal members of `Source`.

## Usage in the TUI Library

The standard library TUI uses contracts to describe widget capabilities and member expansion to reuse state and implementations:

| Type | Explicitly declared contract | Direct expansion source | Use |
| --- | --- | --- | --- |
| `Button` | `Widget` | `View = View()` | Reuses basic widget fields and behavior |
| `Container` | `ContainerWidget` | `View = View()` | Adds a child collection and extends styling, layout and drawing behavior |
| `VStack` | `VStackWidget` | `Container = Container()` | Customizes child style preparation and vertical layout |

`ContainerWidget` declares `Widget` as a parent contract, and `VStackWidget` declares `ContainerWidget` as a parent. These relationships are explicitly declared by the contracts; they are not inheritance supplied by expansion.

Public `View` fields such as `frame` and `tabIndex` expand automatically. Internal state such as `state`, `rtStyle` and `overrideStyle` uses `@mixable seal` fields. Reusable operations use `@mixable static`, with `seal` added for internal operations. `Button` uses the completed `View()` construction, preserving declared initializers such as `tabIndex = -1`.

The following program uses actual TUI types and follows the `VStack` pattern to customize child style preparation. It demonstrates this extension point only; the full `VStack` also implements vertical layout:

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

The output is `-1` and `start`. Three different access paths appear here:

- `stack.addChild(button)` uses the public instance method expanded from `Container`.
- `Container.prepareChildOverrideStyle(stack, child)` calls the source's seal static method under the direct permission from `StartStack` expanding `Container`. This permission remains even though the target supplies its own same-named `@mixable` method.
- `child.overrideStyle` accesses a seal field through the `Widget` view because the current implementing type satisfies `Widget`. It is not ordinary access to a seal field through the concrete `Button` or `View` type.

`StartStack` does not directly expand `View`, so this expansion chain alone does not permit direct calls to the original `@mixable seal` methods on `View`. In the actual TUI implementation, `Container` calls `View.doStyling(...)` where it directly expands `View`, while `VStack` calls `Container.prepareChildOverrideStyle(...)` where it directly expands `Container`.
