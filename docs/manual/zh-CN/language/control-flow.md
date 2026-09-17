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

表达式形式必须包含 `else`，每条正常完成路径都必须到达块末结果表达式。通过当前函数、方法或
Lambda 的 `return` 返回，或者通过 `throw` 逃逸的路径不产生结果。上下文提供目标类型时，每个正常
结果都必须能够贴合该类型；没有目标类型时，Feng 从正常结果确定目标类型，并要求其余正常结果能够
贴合。

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

绑定只能声明在初始化子句中。更新子句可以修改此前已经可见的绑定或调用函数，但不能使用 `let` /
`var` 声明新绑定。

非空初始化子句在进入当前循环时执行且只执行一次。初始化子句声明的绑定在整个循环中保持同一个
绑定实例；循环外已有的绑定也始终是原来的绑定。条件、更新和每轮循环体都引用这些相同的绑定。
初始化绑定属于循环头作用域，循环体花括号是其子块；循环体内可以声明同名局部绑定并从声明处开始
屏蔽初始化绑定。

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
的局部声明。循环变量属于本轮循环头作用域，循环体花括号是其子块，因此循环体内可以声明同名局部
绑定并从声明处开始屏蔽循环变量。

序列元素为具名 tuple 时，可以在循环头直接解构：

```feng
type Entry(string, int);

let apples: Entry = ("苹果", 2);
let pears: Entry = ("梨", 3);
let entries: Entry[] = [apples, pears];

for let (name, count) in entries {
  println("{0}: {1}", name, count);
}

for var (name, count) in entries {
  count += 1;
}
```

tuple 模式必须显式写 `let` 或 `var`，并同时作用于每个非空位置。模式位置数必须与 tuple 元素数一致；
空位表示跳过该元素，例如 `for let (name, ) in entries`。模式只支持一层，不支持嵌套 tuple 模式、
单位置模式或模式内类型标注。每个非空位置都是独立的逐轮绑定，同一模式中的名称不得重复。

`let` 循环变量不可重新赋值；`var` 循环变量可在本轮修改，但不会改变被遍历的序列，也不会影响下一
轮的初始值。闭包只捕获当前轮的绑定：同一轮的多个闭包共享该轮绑定，不同轮的闭包互不共享；如果
先创建闭包、再修改同轮的 `var` 循环变量，闭包会读取修改后的值。tuple 模式中的每个非空位置分别
遵循相同规则。

## 控制转移

- `break` 立即退出最近一层循环。三段式 `for` 不再执行当前轮的更新子句，`for/in` 不再获取下一
  元素。
- `continue` 跳过最近一层循环的当前轮剩余语句：在 `while` 中重新判断条件；在三段式 `for` 中先
  执行一次更新子句，再判断条件；在 `for/in` 中获取下一元素。即使循环体中有多个 `continue`
  分支，每条实际执行路径也只会到达一次继续步骤。
- `return` 结束当前函数，可按函数返回类型携带值。

Feng 的 `break` 和 `continue` 不携带标签或层数参数。循环可以嵌套，但每次只能作用于最近一层循环，
不能跨越多层循环。

`if`、`match` 或 `try/catch` 作为表达式使用时，每个结果分支都是控制转移边界：分支内的 `break` /
`continue` 不能作用于表达式外部的循环。分支内部新建的 `while`、三段式 `for` 或 `for/in` 循环仍可
正常使用 `break` / `continue`，并且只作用于该内层循环。相同结构作为语句使用时没有这一额外限制。

结果分支可以使用 `return` 退出当前函数、方法或 Lambda。只有能够继续正常执行的路径才必须到达
块末结果表达式；`return` 路径不产生分支值，也不参与分支结果类型推导。嵌套 Lambda 中的 `return`
只返回该 Lambda，不退出外围 callable。

## 编写自定义迭代器

容器负责创建游标，游标负责保存当前位置。下面通过 `fit` 为容器提供 `@iterable` 入口，并用具名 `(bool, int)` tuple 表达一次推进结果：

```feng
module manual_iterator;
import std.io;
import std.numeric;

/** Half-open range with reusable traversal state. */
type NumberRange { let start: int; let end: int; }

/** Indicates whether an element was produced. */
type RangeStep(bool, int);

/** Mutable state for one traversal. */
type RangeCursor {
  var current: int;
  let end: int;

  /** Produces an element or the end marker. */
  @iterator
  func next(): RangeStep {
    if self.current >= self.end { return (false, 0); }
    let value = self.current;
    self.current += 1;
    return (true, value);
  }
}

fit NumberRange {
  /** Creates a new cursor for each traversal. */
  @iterable
  func cursor(): RangeCursor {
    return RangeCursor { current: self.start, end: self.end };
  }
}

/** Compares container traversal and direct cursor traversal. */
func main(args: string[]) {
  let range = NumberRange { start: 0, end: 3 };
  var total = 0;
  for let value in range { total += value; }
  let cursor = range.cursor();
  var direct = 0;
  for let value in cursor { direct += value; }
  var remaining = 0;
  for let value in cursor { remaining += 1; }
  println<int>("{0} {1} {2}", total, direct, remaining);
}
```

输出为 `3 3 0`。每次遍历容器都会调用一次 `@iterable`，这里返回新游标，因此重复或嵌套遍历互不影响；直接遍历游标则继续使用其当前状态，耗尽的游标不会自动重置。

两种注解都标在无显式参数的实例方法上，可声明在类型体或可见的 `fit` 中。同一类型的可见成员面不能同时有两种入口，也不能有多个同种入口。`@iterable` 必须返回具有合法 `@iterator` 的类型；`@iterator` 必须返回具名二元组，首元素为 `bool`。返回 `false` 时第二元素应取默认值，循环不会消费它。方法名可以自行选择。
