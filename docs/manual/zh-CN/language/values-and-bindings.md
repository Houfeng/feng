# 值与绑定

Feng 使用 `let` 声明不可重新赋值的绑定，使用 `var` 声明可以重新赋值的绑定。

## 声明绑定

有初始值时可以省略类型：

```feng
let language = "Feng";
var count = 0;
count += 1;
```

没有初始值时必须写出类型。Feng 会使用该类型的默认零值：

```feng
let retries: int;    // 0
let enabled: bool;   // false
let message: string; // ""
let values: int[];   // []
```

优先使用 `let`；只有确实需要重新赋值时才使用 `var`。

## 绑定与对象可变性

绑定是否可重新赋值，与对象成员是否可修改是两层规则：

```feng
type User {
  var name: string;
}

let user = User { name: "Alice" };
user.name = "Bob";        // 合法：name 是 var 成员
// user = User {};         // 非法：user 是 let 绑定
```

数组也使用独立的元素可写标记。`T[]` 的当前层只读，`T[!]` 的当前层可写：

```feng
let readonly = [1, 2, 3];
let writable: int[!] = [1, 2, 3];
writable[0] = 10;
// readonly[0] = 10;       // 非法
```

## 参数绑定

参数必须声明类型。省略 `let` 或 `var` 时默认不可重新赋值：

```feng
func add(a: int, b: int): int {
  return a + b;
}

func advance(step: int, var position: int): int {
  position += step;
  return position;
}
```

参数按值进入函数；`var` 只允许在函数体中修改参数绑定，不会把重新赋值写回调用方的绑定。

## 块作用域

绑定从声明位置开始，在所在块及其子块中可见。子块可以用同名绑定屏蔽外层绑定：

```feng
let label = "outer";

if true {
  let label = "inner";
  println(label);
}

println(label);
```

子块结束后，外层的 `label` 会重新可见。

## 解构绑定

具名元组和元组字面量可以按位置解构：

```feng
type Pair(int, string);

let pair: Pair = (7, "seven");
let (number, text) = pair;
let (first, second) = (1, 2);
let (, only_text) = pair;
```

空位置表示丢弃对应值。Feng 当前只支持单层解构。
