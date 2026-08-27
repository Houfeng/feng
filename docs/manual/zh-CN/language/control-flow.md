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

表达式形式必须包含 `else`，所有正常完成的分支必须产生结果；目标类型选择与贴合见 [Feng 语言流程控制规范](../../../specifications/feng-flow.md#41-if--match--try-%E8%A1%A8%E8%BE%BE%E5%BC%8F%E7%BB%93%E6%9E%9C%E7%B1%BB%E5%9E%8B)。

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

初始化执行次数、外部绑定与初始化绑定的共享范围、循环体局部绑定的逐轮身份，以及相应的闭包捕获
结果，以 [Feng 语言流程控制规范](../../../specifications/feng-flow.md#61-%E4%B8%89%E6%AE%B5%E5%BC%8F-for)
为准。

## for/in

```feng
let values = [10, 20, 30];
var total = 0;

for let value in values {
  total += value;
}
```

`for/in` 可遍历数组，以及通过 `@iterable` / `@iterator` 协议接入的标准库或自定义迭代器。循环变量
的逐轮身份、`let` / `var` 可变性和闭包捕获结果，以
[Feng 语言流程控制规范](../../../specifications/feng-flow.md#62-forin-%E5%BE%AA%E7%8E%AF)为准。

## 控制转移

- `break` 退出最近一层循环。
- `continue` 跳过最近一层循环的当前轮。
- `return` 结束当前函数，可按函数返回类型携带值。

Feng 不支持循环标签，因此不能用一次 `break` 或 `continue` 跨越多层循环。
