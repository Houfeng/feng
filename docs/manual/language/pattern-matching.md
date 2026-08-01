# 模式匹配

`match` 可用于常量匹配和联合类型收窄，也可以作为返回 `bool` 的中缀运算。详细规则见[流程控制规范](../../specifications/feng-flow.md)和[联合类型规范](../../specifications/feng-union-type.md)。

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

`match` 作为表达式时，每个正常分支的最后一个表达式是结果，类型必须一致。若匹配没有覆盖全部可能值，应提供 `else`。
