# Feng 语言迭代器规范

本文档说明 Feng 中迭代器协议的设计、注解语义、`for/in` 展开规则与标准库支持。迭代器协议与 `for/in` 循环语法的关系见 [Feng 语言流程控制规范](./feng-flow.md)。

## 1 设计原则

- 迭代协议由两个内建注解 `@iterable` 与 `@iterator` 表达，编译器感知注解名称与返回形状约定，不感知任何标准库类型名称或模块路径。
- 标准库与用户代码在协议上完全平等，均通过注解参与迭代协议，无任何特权差异。
- 现阶段不支持生成器（`yield`）语义。

## 2 内建注解

### 2.1 `@iterable`

`@iterable` 标注在 `type` 的某个成员方法上，表示该方法是该类型的"产生游标"入口。`@iterable` 方法可声明在 `type` 体内，也可通过 `fit` 块为类型扩展。

约束：

- 编译器在类型的可见面（`type` 体及全部 `fit` 块）内查找所有带 `@iterable` 的合法方法，找到多于一个时编译期报错。
- 被标注的方法必须无参数（除隐式 `self`）。
- 被标注的方法的返回值类型上必须存在恰好一个 `@iterator` 方法；若不满足，编译期报错。
- `@iterable` 与 `@iterator` 不得同时出现在同一个 `type` 的可见面内；违反时编译期报错。

### 2.2 `@iterator`

`@iterator` 标注在 `type` 的某个成员方法上，表示该方法是该游标类型的"推进迭代"入口。`@iterator` 方法可声明在 `type` 体内，也可通过 `fit` 块为类型扩展。

约束：

- 编译器在类型的可见面内查找所有带 `@iterator` 的合法方法，找到多于一个时编译期报错。
- 被标注的方法必须无参数（除隐式 `self`）。
- 被标注的方法的返回值类型必须是具名元组类型，且该类型的定义须形如 `(bool, E)`：第一元素为 `bool`，第二元素为任意具体类型（`E` 为文档占位符，用户实现时直接写具体类型，无需泛型）。编译器按结构检查此约定，不关心类型名称；否则编译期报错。
- 返回值的第一元素（`bool`）表示本轮是否产出了有效元素：`true` 表示产出，`false` 表示迭代结束。第一元素为 `false` 时，第二元素为其类型的默认零值，调用方不得将其视为有效元素使用。

## 3 `for/in` 展开规则

```feng
for let it in expr { body }
```

编译器按以下步骤展开：

1. 求 `expr` 的静态类型，设为 `S`。
2. 若 `S` 是 `T[]` 或 `T[!]`，走现有数组内建迭代路径（不变）。
3. 否则，在 `S` 上查找 `@iterable` 方法：
   - 找到 → 调用该方法得到游标对象 `cursor`，再在 `cursor` 的类型上查找 `@iterator` 法。
   - 未找到 → 在 `S` 上直接查找 `@iterator` 方法（`S` 自身即游标）。
   - 两者均未找到 → 编译期报错，提示类型不可迭代。
4. 每轮循环调用 `@iterator` 方法，得到 `(ok, val)`：
   - `ok == false` → 退出循环。
   - `ok == true` → 将 `val` 绑定到循环变量 `it`，执行 `body`。

等价展开伪代码：

```
// 有 @iterable 的情况
let __cursor = expr.<@iterable方法>();
loop {
  let (ok, val) = __cursor.<@iterator方法>();
  if !ok { break; }
  let it = val;
  body;
}
```

每个 `for/in` 节点独立调用 `@iterable` 方法，产生各自独立的游标对象，嵌套迭代不会共享游标状态。

## 4 发码与运行时约束

编译器针对 `for/in` + 迭代器的发码，强目标是：与用户手写调用 `@iterable` 方法和 `@iterator` 方法等价，或开销更小。

具体要求：

- **`@iterable` 恰好调用一次**：每个 `for/in` 节点对 `@iterable` 方法只生成一次调用，不得重复求值。
- **`@iterator` 调用内联**：游标类型在编译期静态已知时，编译器应将 `@iterator` 方法调用内联到循环体，不产生间接调用（函数指针/虚表）开销。
- **不引入额外堆分配**：`for/in` 展开本身不得额外分配堆内存；用户 `@iterable` 实现自身分配的内存不在此限。
- **循环变量无额外拷贝**：循环变量绑定应直接来自 `@iterator` 返回值的第二元素，不得引入中间临时拷贝。

以上为编译器的强制目标，不达到此目标视为编译器缺陷。

## 5 示例

### 5.1 自定义容器

```feng
type Range {
  let start: int;
  let end: int;    // 不含端点
}

type StepResult(bool, int)

type RangeCursor {
  var cur: int;
  let end: int;

  @iterator
  func step(): StepResult {
    if self.cur >= self.end {
      return (false, 0);
    }
    let val = self.cur;
    self.cur = self.cur + 1;
    return (true, val);
  }
}

fit Range {
  @iterable
  func iter(): RangeCursor {
    return RangeCursor { cur: self.start, end: self.end };
  }
}
```

使用：

```feng
let r = Range { start: 0, end: 5 };
for let i in r {
  print(i);   // 0 1 2 3 4
}
```

### 5.2 类型自身即游标

若某类型本身维护迭代状态，可直接标注 `@iterator`，无需 `@iterable`：

```feng
type NextResult(bool, int)

type Counter {
  var n: int;
  let max: int;

  @iterator
  func next(): NextResult {
    if self.n >= self.max {
      return (false, 0);
    }
    let v = self.n;
    self.n = self.n + 1;
    return (true, v);
  }
}
```

```feng
var c = Counter { n: 0, max: 3 };
for let v in c {
  print(v);   // 0 1 2
}
```

## 6 标准库：`Iterator<T>`

标准库在此注解协议之上提供泛型游标类型 `Iterator<T>`，供用户复用，不享有任何编译器特权。

`Iterator<T>` 是一个普通的 `type`，其 `@iterator` 方法标注方式与用户自定义游标完全相同。标准库通过 `fit Iterator<T>` 在其上统一挂载惰性组合子，如 `filter`、`map`、`take` 等。

用户若希望使用这些组合子，只需在 `@iterable` 方法中返回 `Iterator<T>`；若使用自定义游标类型，则无法直接访问这些组合子，但功能上不受任何限制。

示例（容器返回 `Iterator<T>` 以使用组合子）：

```feng
type MyList<T> { ... }

fit MyList<T> {
  @iterable
  func iter(): Iterator<T> {
    // 构造并返回 Iterator<T>
  }
}
```

```feng
let list = MyList { ... };
for let x in list.iter().filter(func(v) { v > 0 }) {
  print(x);
}
```

`Iterator<T>` 的完整 API 见标准库文档。

## 7 编译器约束汇总

| 约束 | 说明 |
| --- | --- |
| `@iterable` 至多一个 | 在类型可见面内查找所有带 `@iterable` 的合法方法，找到多于一个时编译期报错 |
| `@iterator` 至多一个 | 在类型可见面内查找所有带 `@iterator` 的合法方法，找到多于一个时编译期报错 |
| 不可同时拥有 | 同一类型可见面内不得同时存在 `@iterable` 和 `@iterator`，违反时编译期报错 |
| `@iterable` 返回类型须有 `@iterator` | 否则编译期报错 |
| `@iterator` 返回类型须为结构形如 `(bool, E)` 的具名元组 | 必须是具名元组类型，第一元素为 `bool`，第二元素为任意具体类型，编译器按结构检查，不关心类型名称 |
| 两注解方法均须无参 | 被标注方法不得声明参数（除隐式 `self`） |

## 8 与主规范的关系

- `for/in` 语法形式与循环变量绑定规则见 [Feng 语言流程控制规范](./feng-flow.md)。
- 注解的通用规则与现有内建注解列表见 [Feng 语言规范总览](./feng-language.md)。
- 标准库 `Iterator<T>` 的完整 API 定义属于标准库文档范畴，不在本规范中定义。
