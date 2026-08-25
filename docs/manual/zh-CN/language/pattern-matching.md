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

`match` 作为表达式时，每个正常分支的最后一个表达式是结果；目标类型选择与贴合规则统一见 [Feng 语言流程控制规范](../../../specifications/feng-flow.md#41-if--match--try-%E8%A1%A8%E8%BE%BE%E5%BC%8F%E7%BB%93%E6%9E%9C%E7%B1%BB%E5%9E%8B)，`throw` 分支与 `else` 完备性见同规范的[块形式 `match` 表达式结果](../../../specifications/feng-flow.md#34-%E5%9D%97%E5%BD%A2%E5%BC%8F-match-%E7%9A%84%E8%A1%A8%E8%BE%BE%E5%BC%8F%E7%BB%93%E6%9E%9C)。
