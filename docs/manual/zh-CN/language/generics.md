# 泛型

泛型让类型和函数复用于多种静态类型。

## 泛型类型

```feng
type Box<T> {
  var value: T;

  func get(): T {
    return self.value;
  }
}

let number = Box<int> { value: 42 };
let text = Box<string> { value: "Feng" };
```

类型构造必须显式给出类型实参；Feng 不从构造参数或上下文推导所属 `type` 的类型参数。

## 泛型函数

```feng
func identity<T>(value: T): T {
  return value;
}

let first = identity(42);
let second = identity<string>("Feng");
```

调用时可以显式写出类型实参，也可以在实参、接收者或目标类型足以唯一确定时让编译器推导。

## 多个类型参数

```feng
type Pair<T, U> {
  let first: T;
  let second: U;
}

func make_pair<T, U>(first: T, second: U): Pair<T, U> {
  return Pair<T, U> { first: first, second: second };
}
```

## 泛型约束

约束必须引用 `spec`：

```feng
spec Named {
  let name: string;
}

func name_of<T: Named>(value: T): string {
  return value.name;
}
```

对象契约约束允许在泛型实现中直接使用契约成员。可调用契约约束允许直接调用参数。联合契约约束仍需要先通过 `match` 收窄。

## 泛型方法

```feng
type Box<T> {
  let value: T;

  func pair_with<U>(other: U): Pair<T, U> {
    return Pair<T, U> { first: self.value, second: other };
  }
}
```

方法自己的类型参数不能与外层类型参数重名。

## 不变性

泛型实例按不变方式处理。即使 `Dog` 满足 `Animal`，`Box<Dog>` 也不会自动转换为 `Box<Animal>`。需要这种转换时，应显式遍历并创建新的目标容器或适配对象。

无约束类型参数不提供成员、比较或逻辑运算能力。只有规范明确允许的基础操作，或约束声明提供的能力，才能在泛型实现中使用。

## 泛型契约与约束组合

四种 spec 都可以声明类型参数，也都可以作为约束。下面同时使用对象、可调用、联合和交叉契约，并在 `SizedReader<T>` 中声明泛型父契约：

```feng
module manual_generic_contracts;
import std.io;
import std.numeric;

/** Reads a value of the chosen type. */
spec Reader<T> { func read(): T; }

/** Extends the explicitly instantiated parent contract. */
spec SizedReader<T>: Reader<T> { func size(): int; }

/** Supplies a label. */
spec Labelled { func label(): string; }

/** Combines two object contracts. */
spec LabelledReader<T>: Reader<T> & Labelled;

/** Produces a value. */
spec Producer<T>(): T;

/** Holds either the chosen type or text. */
spec ValueOrText<T>: T | string;

/** Implements the generic object contracts. */
type Box<T>: SizedReader<T>, Labelled {
  let value: T;

  /** Reads the stored value. */
  func read(): T { return self.value; }

  /** Reports this example's fixed size. */
  func size(): int { return 1; }

  /** Labels the box. */
  func label(): string { return "box"; }
}

/** Calls a value through a callable constraint. */
func produce<F: Producer<int>>(factory: F): int { return factory(); }

/** Uses both parts of an intersection constraint. */
func read_labelled<R: LabelledReader<int>>(reader: R): int {
  println(reader.label());
  return reader.read();
}

/** Narrows a value admitted by a union constraint. */
func number_or_zero<V: ValueOrText<int>>(value: V): int {
  return match value {
    let number: int { number }
    else { 0 }
  };
}

/** Exercises the four forms together. */
func main(args: string[]) {
  let box = Box<int> { value: 7 };
  let child: SizedReader<int> = box;
  let parent: Reader<int> = child;
  let factory: Producer<int> = () -> 5;
  let choice: ValueOrText<int> = "text";
  let read = read_labelled(box);
  println<int>("{0} {1} {2} {3}", read, parent.read(), produce(factory),
    number_or_zero<ValueOrText<int>>(choice));
  println<int>("{0}", number_or_zero<int>(9));
}
```

输出为 `box`、`7 7 5 0`、`9`。每个泛参至多有一个约束；需要多种对象能力时，先声明交叉契约。`SizedReader<T>: Reader<T>` 使用的是父契约实例，不能在父列表重新声明泛参约束；已有约束必须足以证明父契约所需的能力。

联合约束允许能进入该联合的类型实参，也允许完整联合自身。上例的 `V` 分别是完整的 `ValueOrText<int>` 和 `int`，不会因为约束而被改成同一种类型；泛型体使用匹配绑定后再操作成员。交叉契约作为类型实参时同样保留完整契约视角。静态 requirement 和受约束方法值见[契约与 fit](./contracts-and-fit.md)，联合收窄见[模式匹配](./pattern-matching.md)。

## 自约束、目标推导与声明重载

自约束可表达“与自身同类型的值协作”。无约束泛参则适合保存、复制、传递和返回值；这些操作保留具体类型原有的值或引用语义，不要求它提供额外成员。

```feng
module manual_generic_inference;
import std.io;
import std.numeric;

/** Compares values of a chosen type through a named operation. */
spec Equal<T> { func same(other: T): bool; }

/** Satisfies a contract instantiated with its own type. */
type Key: Equal<Key> {
  let id: int;

  /** Compares identifiers. */
  func same(other: Key): bool { return self.id == other.id; }
}

/** Requires comparison with the same actual type. */
func same_key<T: Equal<T>>(left: T, right: T): bool {
  return left.same(right);
}

/** Stores one unconstrained type. */
type Cell<T> { let value: T; }

/** A distinct declaration selected by its two type parameters. */
type Cell<T, U> { let first: T; let second: U; }

/** Produces one type. */
spec Mapper<T>(): T;

/** A separate callable declaration with two type parameters. */
spec Mapper<T, U>(value: T): U;

/** Copies and returns a value using its existing semantics. */
func identity<T>(value: T): T { let copy = value; return copy; }

/** Gets its type parameter from the caller's result type. */
func empty<T>(): T[] { return []; }

/** Demonstrates inference and exact generic arity selection. */
func main(args: string[]) {
  let values: int[] = empty();
  let cell = Cell<int> { value: identity(3) };
  let pair = Cell<int, string> { first: 4, second: "four" };
  let source: Mapper<int> = () -> cell.value;
  let convert: Mapper<int, string> = (value: int) -> pair.second;
  println<int>("{0}", source());
  println(convert(pair.first));
  if same_key(Key { id: 7 }, Key { id: 7 }) { println("same"); }
}
```

输出为 `3`、`four`、`same`。`empty()` 的目标 `int[]` 确定 `T = int`；类型构造仍必须写成 `Cell<int>`，不能靠目标类型省略所属类型的泛参。

同一作用域内，同类别的 type 或同形式的 spec 可以按泛参数量区分同名声明；只改参数名或约束、但数量相同，不构成新声明。推导到同一泛参的多个类型必须一致，不能靠寻找共同父契约或插入转换消除冲突。无约束的 `T` 不能直接使用成员、相等比较或逻辑运算；像 `same` 这样的能力应通过约束表达，约束本身也不会新增运算符。

函数或方法调用的推导与形成函数值不同：后者须显式闭合来源泛参，见[函数值](./functions.md)。泛型 fit 的参数对应关系与静态扩展示例见[契约与 fit](./contracts-and-fit.md)。
