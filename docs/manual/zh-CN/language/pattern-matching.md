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
};
```

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

绑定只在能够静态保证匹配成功的条件范围内可见。`&&` 可以把左侧绑定传递到右侧及分支体；`||`、`!` 和 `else` 不会传播绑定。

## 表达式结果

`match` 作为表达式时，每个正常分支的最后一个表达式是结果；以 `throw` 结束的分支不产生结果，也
不参与结果类型检查。表达式形式必须包含 `else`。上下文提供目标类型时，每个正常分支的结果都必须
能够贴合该类型；没有目标类型时，Feng 从分支结果确定目标类型，并要求其余正常分支的结果能够贴合。
