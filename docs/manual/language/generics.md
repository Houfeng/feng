# 泛型

泛型让类型和函数复用于多种静态类型。当前权威文档为[泛型规范](../../specifications/feng-generics-draft.md)；文件名保留 `draft`，使用时应关注规范后续变化。

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
