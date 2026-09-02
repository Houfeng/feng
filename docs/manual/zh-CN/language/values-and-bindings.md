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
同一参数列表中的名称必须唯一，函数体最外层也不能再次声明与参数同名的绑定；函数体内更深一层
的子块可以合法屏蔽参数。

## 块作用域

绑定从声明位置开始，在所在块及其子块中可见。同一块内不能重复声明同名绑定；子块可以用同名
绑定屏蔽外层绑定：

```feng
let label = "outer";

if true {
  let label = "inner";
  println(label);
}

println(label);
```

子块结束后，外层的 `label` 会重新可见。

`_` 是普通标识符，不是丢弃符。`let _ = expression;` 会声明一个名称为 `_` 的普通绑定，因此同一
作用域再次声明 `_` 也会报重复绑定错误。仅需执行表达式并忽略结果时，直接写成表达式语句：

```feng
advance();
```

## 解构绑定

具名元组和元组字面量可以按位置解构：

```feng
type Pair(int, string);

let pair: Pair = (7, "seven");
let (number, text) = pair;
let (first, second) = (1, 2);
let (, only_text) = pair;
```

空位置表示丢弃对应值，不产生绑定。同一解构模式中的非空位置不能使用重复名称。Feng 当前只支持
单层解构。
