# 表达式

表达式产生值，语句负责绑定、赋值或控制执行。

## 字面量与构造

```feng
let integer = 42;
let decimal = 3.14;
let enabled = true;
let text = "Feng";
let items = [1, 2, 3];
let user = User { name: "Alice" };
```

数组创建使用 `Type[:length]`，索引访问使用 `value[index]`：

```feng
let bytes: byte[!] = byte[:256];
bytes[0] = (byte)65;
```

`length` 是只求值一次的整数，合法范围为 `0` 到目标平台 `int` 类型的最大值。编译期可确定的非法值
会导致编译错误；动态非法值在分配前触发 panic。数组索引同样只求值一次，负数或越界值在元素读写前
触发 panic。没有显式数组目标类型时，`Type[:length]` 的结果为当前层只读的 `T[]`。

## 算术与比较

Feng 提供常用算术、关系、相等、逻辑、位与移位运算符：

```feng
let total = price * count + shipping;
let in_range = score >= 60 && score <= 100;
let same = left == right;
let masked = flags & 0xff;
```

两个操作数通常必须具有相同静态类型。数值字面量可以贴合已确定的目标数值类型，但已经绑定的不同数值类型之间必须显式转换。

逻辑 `&&` 和 `||` 短路求值。函数实参和二元运算数按从左到右顺序求值。

## 显式转换

转换写作 `(TargetType)expression`：

```feng
let small: i32 = 42;
let wide = (i64)small;
let ratio = (f64)small / 100.0;
```

显式写法不表示任意类型都可互转；具体合法范围由源类型与目标类型规则决定。

## 赋值与复合赋值

```feng
var count = 0;
count = count + 1;
count += 2;
count *= 3;

var mask: i32 = 1;
mask <<= 3;
```

赋值目标必须可写：`var` 绑定、`var` 成员或可写数组层的元素。复合赋值只对左侧目标求值一次。

## 分支表达式

`if`、`match` 和 `try/catch` 都可以产生值：

```feng
let label = if score >= 60 {
  "pass";
} else {
  "fail";
};

let category = match code {
  200 { "ok" }
  400...499 { "client error" }
  else { "other" }
};
```

表达式形式的每条正常完成路径都必须产生结果，块中最后一个可达表达式就是该块的值；通过当前函数、
方法或 Lambda 的 `return` 返回，或者通过 `throw` 逃逸的路径不产生结果。上下文提供目标类型时，
每个正常结果都必须能够贴合该类型；没有目标类型时，Feng 从正常结果确定目标类型，并要求其余正常
结果能够贴合。`return` / `throw` 后的不可达表达式不会成为块结果。
