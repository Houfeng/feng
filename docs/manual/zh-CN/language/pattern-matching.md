# 模式匹配

`match` 可用于常量匹配和联合类型收窄，也可以作为返回 `bool` 的中缀运算。

## 常量匹配

整型、`string`、`bool` 和 `enum` 可以按值匹配：

```feng
let label = match status_code {
  200 { "ok" }
  201, 204 { "success" }
  400...499 { "client error" }
  else { "other" }
};
```

同一匹配体中的标签不能重叠。整数区间 `a...b` 是包含两端的闭区间。

枚举标签必须使用完整枚举项：

```feng
enum State {
  Idle,
  Running,
  Done
}

match state {
  State.Idle { println("idle"); }
  State.Running { println("running"); }
  State.Done { println("done"); }
}
```

## 联合类型

union-form `spec` 声明一组可能的成员类型：

```feng
spec Result: int | string;

let result: Result = "ready";
```

联合值在收窄前不能直接访问成员或进行相等比较。通过带绑定的分支取得具体成员：

```feng
let message = match result {
  value: int { "code" }
  text: string { text }
  else { "unknown" }
};
```

分支头声明的绑定属于该分支自己的头部作用域，后面的花括号是子块。分支体可以声明同名局部绑定并
从声明处开始屏蔽头部绑定；不同分支也可以独立复用同一绑定名称。

无绑定分支只判断成员类型，不会改变原变量的静态类型：

```feng
match result {
  int { println("integer"); }
  string { println("text"); }
}
```

## 中缀 match

`value match pattern` 返回 `bool`。多个标签用 `|` 分隔：

```feng
if code match 200 | 201 | 204 {
  println("success");
}

if score match 0...59 {
  println("retry");
}
```

联合成员模式可以同时绑定收窄值：

```feng
if result match text: string && !text.isEmpty() {
  println(text);
}
```

绑定只在能够静态保证匹配成功的条件范围内可见。`&&` 可以把左侧绑定传递到右侧及分支体；`||`、
`!` 和 `else` 不会传播绑定。传播到 `if` / `while` 的绑定属于条件头作用域，花括号 body 是子块，
可以声明同名局部绑定；同一个 `&&` 条件同时传播的多个绑定名称不得重复。

## 表达式结果

`match` 作为表达式时，每条正常完成路径都必须到达所在分支块的最后一个结果表达式；通过当前函数、
方法或 Lambda 的 `return` 返回，或者通过 `throw` 逃逸的路径不产生结果，也不参与结果类型检查。
表达式形式必须包含 `else`，即使已经列出联合类型的全部成员。独立语句形式的 `match` 可以省略
`else`。上下文提供目标类型时，每个正常结果都必须能够贴合该类型；没有目标
类型时，Feng 从正常结果确定目标类型，并要求其余正常结果能够贴合。

## 嵌套联合、多级模式与子集绑定

联合保持声明时的嵌套层级。`A -> B` 表示先匹配直接成员 `A`，再匹配其中的直接成员 `B`；带绑定的链式模式取得末端成员值：

```feng
module manual_nested_union;
import std.io;
import std.numeric;

spec TextOrCode: int | string;

spec Reply: TextOrCode | bool;

spec Flat: int | string | bool;

/** Reads a nested member using its declared path. */
func describe(value: Reply): string {
  return match value {
    var code: TextOrCode -> int { code += 1; code.toString(); }
    let text: TextOrCode -> string { text; }
    bool { "flag"; }
    else { "other"; }
  };
}

/** Narrows a subset once more before using its concrete member. */
func describe_subset(value: Flat): string {
  return match value {
    let selected: int, string {
      let text = match selected {
        let code: int { code.toString(); }
        let word: string { word; }
        else { "other"; }
      };
      text;
    }
    bool { "flag"; }
    else { "other"; }
  };
}

/** Uses both an entered value and the first-member default. */
func main(args: string[]) {
  let reply: Reply = 7;
  let empty: Reply;
  println("{0} {1} {2}", describe(reply), describe(empty), describe_subset("hello"));
}
```

输出为 `8 1 hello`。`var code` 可重新赋值，改动不会把原联合的 active member 换成别的类型；省略修饰或写 `let` 得到不可重新赋值的绑定。逗号合并分支得到的仍是联合子集，必须继续收窄后才能使用具体成员能力。链上每一级都必须是上一层的直接成员，不能跳过 `TextOrCode` 直接写 `int`。

无初始值的联合取第一个直接成员的默认值，若它仍是联合则继续按其第一成员取值，所以 `empty` 最终持有 `int` 的零值。表达式 match 仍须包含 `else`。

进入联合时先在整个成员图中寻找精确类型匹配，再寻找可满足或可贴合的成员；每一轮都按广度优先、同层声明顺序选择第一个命中。因此深层精确匹配优先于浅层契约满足，多条合法路径本身不报歧义。需要指定某个嵌套成员时，可先显式形成该成员值，再放入外层联合；同一完整联合类型的复制保留原 active member。

## 以对象契约和可调用契约为联合成员

先明确形成 callable 值，再放入联合，可以把数据和行为放在同一结果类型中；收窄后只使用被选成员的能力：

```feng
module manual_union_contracts;
import std.io;
import std.numeric;

spec Named { func display(): string; }

spec Action(): string;

spec Result: Named | Action | int;

/** Supplies a nominal object-contract member. */
type Message: Named {
  /** Returns the object member's text. */
  func display(): string { return "object"; }
}

/** Uses the stored member rather than testing the runtime object type. */
func render(result: Result): string {
  return match result {
    let named: Named { named.display(); }
    let action: Action { action(); }
    let code: int { code.toString(); }
    else { "other"; }
  };
}

/** Creates both contract member forms. */
func main(args: string[]) {
  let object: Result = Message {};
  let callback: Action = () -> "callable";
  let function: Result = callback;
  println("{0} {1}", render(object), render(function));
}
```

输出为 `object callable`。`Named` 分支只匹配 active member 就是 `Named` 的情况；若联合另外列出具体 `Message` 成员，而值按该具体成员进入，不会因为它也满足 `Named` 就命中 `Named` 分支。收窄到对象契约后不能继续按运行时具体实现向下收窄。
