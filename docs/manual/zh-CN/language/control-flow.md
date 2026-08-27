# 流程控制

Feng 提供条件分支、三种循环以及 `break`、`continue` 和 `return`。模式匹配单独见[模式匹配](./pattern-matching.md)。

## 条件分支

```feng
if score >= 90 {
  println("excellent");
} else if score >= 60 {
  println("pass");
} else {
  println("retry");
}
```

条件必须是 `bool`，Feng 不会把数值、字符串或对象隐式当作布尔值。

`if` 也可以作为表达式：

```feng
let status = if ready {
  "ready";
} else {
  "waiting";
};
```

表达式形式必须包含 `else`，所有正常完成的分支必须产生结果。上下文提供目标类型时，每个分支结果
都必须能够贴合该类型；没有目标类型时，Feng 从分支结果确定目标类型，并要求其余分支结果能够贴合。

## while

```feng
var index = 0;
while index < 3 {
  println("{0}", index);
  index += 1;
}
```

条件在每轮开始前重新求值。

## 三段式 for

```feng
for var index = 0; index < 10; index += 1 {
  if index == 3 {
    continue;
  }
  if index == 8 {
    break;
  }
  println("{0}", index);
}
```

初始化、条件和更新子句都可以省略。`for ;; { ... }` 表示无限循环。

非空初始化子句在进入当前循环时执行且只执行一次。初始化子句声明的绑定在整个循环中保持同一个
绑定实例；循环外已有的绑定也始终是原来的绑定。条件、更新和每轮循环体都引用这些相同的绑定。

每次进入循环体都是该块的一次新执行；每次执行到循环体内的局部声明时，都会创建新的绑定。因此，
闭包捕获循环外绑定或初始化绑定时，各轮闭包共享同一绑定；捕获循环体局部绑定时，各轮闭包分别
捕获本轮的绑定。

## for/in

```feng
let values = [10, 20, 30];
var total = 0;

for let value in values {
  total += value;
}
```

`for/in` 可遍历数组，以及通过 `@iterable` / `@iterator` 协议接入的标准库或自定义迭代器。每轮进入
循环体前，都会以本轮元素值创建一个新的循环变量绑定；这等同于在循环体开始前，每轮执行一次相应
的局部声明。

`let` 循环变量不可重新赋值；`var` 循环变量可在本轮修改，但不会改变被遍历的序列，也不会影响下一
轮的初始值。闭包只捕获当前轮的绑定：同一轮的多个闭包共享该轮绑定，不同轮的闭包互不共享；如果
先创建闭包、再修改同轮的 `var` 循环变量，闭包会读取修改后的值。

## 控制转移

- `break` 退出最近一层循环。
- `continue` 跳过最近一层循环的当前轮。
- `return` 结束当前函数，可按函数返回类型携带值。

Feng 不支持循环标签，因此不能用一次 `break` 或 `continue` 跨越多层循环。
