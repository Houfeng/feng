# 时间与数学

## 数学函数

导入 `std.numeric` 使用 `Math`：

```feng
import std.numeric;

let distance = Math.sqrt(9.0);
let angle = Math.sin(0.5);
let bounded = Math.max(0.0, Math.min(1.0, value));
```

`Math` 使用 `f64`，覆盖绝对值、最小值/最大值、三角与双曲函数、指数、对数、幂、平方根、取整和余数。

## DateTime

导入 `std.time`：

```feng
import std.time;

let now = DateTime.now();
let release = DateTime.of((i32)2026, (i32)8, (i32)1);
let tomorrow = now.addDays((i64)1);

println(now.toString());
println(release.toDateString());
```

`DateTime` 是不带时区信息的不可变值对象。修改类方法会返回新值：

- `addYears`、`addMonths`、`addDays`、`addHours` 等用于时间运算。
- `withYear`、`withMonth`、`withDay` 等替换单个分量。
- `isBefore`、`isAfter`、`equals` 和 `compareTo` 用于比较。
- `toMilliseconds`、`toSeconds` 与对应的 `from...` 用于时间戳转换。

`DateTime.now()` 获取当前 UTC 时间。需要向用户展示本地时间时，应显式处理时区需求，不要把无时区值默认为本地时间。
