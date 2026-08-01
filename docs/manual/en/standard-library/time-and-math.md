# Time and Math

## Math Functions

Import `std.numeric` to use `Math`:

```feng
import std.numeric;

let distance = Math.sqrt(9.0);
let angle = Math.sin(0.5);
let bounded = Math.max(0.0, Math.min(1.0, value));
```

`Math` operates on `f64` and covers absolute values, minimum and maximum, trigonometric and hyperbolic functions, exponentials, logarithms, powers, square roots, rounding, and remainders.

## DateTime

Import `std.time`:

```feng
import std.time;

let now = DateTime.now();
let release = DateTime.of((i32)2026, (i32)8, (i32)1);
let tomorrow = now.addDays((i64)1);

println(now.toString());
println(release.toDateString());
```

`DateTime` is an immutable value object without time-zone information. Methods that modify a component return a new value:

- `addYears`, `addMonths`, `addDays`, `addHours`, and related methods perform date and time arithmetic.
- `withYear`, `withMonth`, `withDay`, and related methods replace one component.
- `isBefore`, `isAfter`, `equals`, and `compareTo` compare values.
- `toMilliseconds`, `toSeconds`, and their corresponding `from...` methods convert timestamps.

`DateTime.now()` returns the current UTC time. When displaying local time to a user, handle time-zone requirements explicitly rather than treating a value without a time zone as local time.
