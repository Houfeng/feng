# 元组

## 1 核心概念

Feng 中与元组相关的概念只有两个：

- **具名元组类型**：用 `type` 关键字以圆括号形式声明的、具有类型名的元组类型。它是类型，可出现在类型位置（参数类型、返回类型、绑定类型注解等）。与对象类型（花括号形式）在 parse 层以名字后的首个 token 区分：`(` 为元组，`{` 为对象。
- **元组字面量**：以 `(expr, expr, ...)` 形式写出的值表达式。字面量是**值**，不是类型，不能出现在类型位置；字面量本身没有类型，只有绑定到具名元组类型时才获得类型身份。例外：在解构上下文中，字面量元组可直接按位置展开，各位置类型由元素自身推断，无需具名类型。

Feng 中**没有匿名元组类型**：不存在 `(int, string)` 这样可单独作为类型使用的匿名元组形式，元组字面量也不能因为"结构已知"就被视作类型。

```feng
type MyTuple(int, string);       // 具名元组，圆括号形式
type User { var name: string; }  // 对象类型，花括号形式（不冲突）
```

## 2 值类型语义

元组是**值类型**，在栈上布局，不受运行时托管内存管理，赋值与传参均为拷贝语义。

**元组元素总是不可变的**，无论绑定使用 `let` 还是 `var`，均不能对单个元素原地赋值。`var` 绑定只允许将整个元组替换为新值，不能修改某一元素。需要部分可变字段时，应改用带 `var` 字段的 `type` 对象类型。

```feng
type MyTuple(int, string);

var a: MyTuple = (1, "hello");
a = (2, "world");   // ✅ var 绑定允许整体替换
a.item1 = 2;        // ❌ 不可变：元素不允许原地修改
```

## 3 元素数量限制

- 允许 **0 ~ 8** 个元素（含两端）。
- **0 元素**：合法，不占任何存储空间，等价于 unit 类型；可用于泛型占位或与未来联合类型配合表达可选语义（如 std 中的 `None`）。字面量写作 `()`。
- **1 元素**：合法，但语义上等价于直接使用该元素类型，建议用注释说明设计意图。
- **超过 8 个元素**：编译错误，建议改用带字段的 `type` 对象类型。

## 4 泛型

具名元组支持泛型类型参数，语法与对象类型一致。

```feng
type Pair<T, U>(T, U);
type Triple<T>(T, T, T);

let p: Pair<int, string> = (1, "hello");
let t: Triple<float> = (1.0, 2.0, 3.0);

// 泛型元组作为参数类型（参数化多态）
func first<T, U>(p: Pair<T, U>): T { return p.item1; }
first(p);  // ✅ T 推断为 int，U 推断为 string
```

## 5 字面量元组绑定规则

字面量元组在绑定时，由目标具名元组的元素类型决定各位置类型；元素数量与各位置类型必须完全一致，否则为编译错误。

```feng
type MyTuple(int, char*);
type OrderPair(int, char*);

// ✅ 字面量元组未绑定时无类型，可贴合结构匹配的具名元组
let a: MyTuple = (1, "str");
let b: OrderPair = (1, "str");

// ❌ 结构不匹配，禁止绑定
type TripleInt(int, int, int);
let c: TripleInt = (1, "str");  // 错误：元素类型不匹配
```

## 6 具名元组间的转换规则

具名元组是名义类型，不同名称的具名元组之间**不可隐式转换**；元素数量与各位置类型完全相同时允许**显式转换**，不同时**禁止转换（含显式转换）**。

```feng
type MyTuple(int, char*);
type OrderPair(int, char*);
type TripleInt(int, int, int);

let a: MyTuple = (1, "str");
let b: OrderPair = (1, "str");

// ✅ 结构相同，显式转换允许
let c: MyTuple = (MyTuple)b;

// ❌ 结构不同，禁止转换（包括显式转换）
let d: MyTuple = (MyTuple)(1, 2, 3);  // 错误：结构不兼容
```

## 7 成员访问

具名元组的成员按位置命名为 `item1`、`item2`、...，从 1 开始。

```feng
type MyTuple(int, char*);
let a: MyTuple = (42, "hello");

let x = a.item1;  // int，值为 42
let y = a.item2;  // char*，值为 "hello"
```

## 8 解构

解构是语法糖，按位置顺序展开为独立绑定，`let`/`var` 语义与普通绑定一致。解构右侧可以是具名元组变量，也可以是字面量元组：

- **具名元组变量**：类型已知，各位置类型由具名类型的元素类型决定。
- **字面量元组**：无具名类型，各位置类型由元素自身推断，纯位置展开。

```feng
type MyTuple(int, char*);
let a: MyTuple = (42, "hello");

// 具名元组解构
let (x, y) = a;      // 等价于 let x = a.item1; let y = a.item2;
var (x, y) = a;      // 等价于 var x = a.item1; var y = a.item2;

// 字面量元组解构
let (p, q) = (1, 2); // 等价于 let p = 1; let q = 2;
var (p, q) = (1, 2); // 等价于 var p = 1; var q = 2;
```

字面量元组解构不引入匿名元组类型，仅是编译期位置展开。`let a = (1, 2)` 仍然非法（无具名类型，无法推断绑定类型）。

### 位置丢弃

解构时可用**空位**（相邻逗号之间不写名字）跳过不需要的位置，该位置不产生绑定：

```feng
type Triple(int, string, float);
let t: Triple = (1, "hello", 3.14);

let (x, , z) = t;   // x: int = 1，跳过 item2，z: float = 3.14
let ( , , z) = t;   // 仅绑定 z
let (x, , ) = t;    // 仅绑定 x
```

空位是纯语法层扩展，不引入任何特殊标识符；被跳过的位置不产生绑定，不可引用。

**不支持嵌套解构**：解构模式只允许一层，各位置只能是标识符或空位，不能是嵌套的 `(...)`。需要访问嵌套元组的内层元素时，先完成外层解构，再对内层变量单独解构：

```feng
type Inner(float, float);
type Outer(int, Inner);
let o: Outer = (1, (2.0, 3.0));

// ❌ 不支持嵌套解构模式
let (x, (y, z)) = o;   // 编译错误

// ✅ 平铺两步
let (x, inner) = o;
let (y, z) = inner;
```

## 9 fit 扩展

具名元组类型与其他 `type` 一样，支持通过 `fit` 显式建立与 `spec` 的满足关系，也支持通过 `fit` 为元组补充方法。`fit` 块中的方法通过 `self` 访问元素，元素仍然只读。

```feng
spec Describable {
  func describe(): string;
}

type Point(float, float);

fit Point: Describable {
  func describe(): string {
    return "(" + self.item1 + ", " + self.item2 + ")";
  }
}

// 也可以用 fit 单独补充方法，不声明满足某个 spec
fit Point {
  func length(): float {
    return sqrt(self.item1 * self.item1 + self.item2 * self.item2);
  }
}

let p: Point = (3.0, 4.0);
p.describe();  // ✅
p.length();    // ✅
```

泛型元组的 `fit` 可以针对具体实例化类型，也可以对全量类型参数扩展：

```feng
type Pair<T, U>(T, U);

// 针对具体类型实例化
fit Pair<int, int> {
  func sum(): int { return self.item1 + self.item2; }
}

// 对全量类型参数扩展（需要 spec 约束）
spec Printable { func print(): void; }
fit<T: Printable, U: Printable> Pair<T, U>: Printable {
  func print(): void { self.item1.print(); self.item2.print(); }
}
```

## 10 泛型约束说明

**具名元组类型不能作为泛型上界约束**，这与主流语言保持一致。

| 语言 | 元组/数组能否作上界 | 说明 |
|------|-------------------|------|
| Swift | ❌ | 元组无法遵循协议，是已知长期限制 |
| Rust | ❌ | 数组/元组不能作 trait 上界，需通过 trait 间接约束 |
| C# | ❌ | `ValueTuple` 只能约束到 `struct`，无法表达"必须是 N 元组" |
| Scala 3 | ✅（有限） | `Tuple2[A,B]` 可作上界，但属于例外 |
| TypeScript | ✅（结构类型） | 结构类型天然支持，非名义类型系统 |

在 Feng 中，若需要"多个具名元组类型共享同一行为契约"，应通过 `spec` 定义对象形状契约，再用 `fit` 显式建立满足关系，与普通类型的约束机制保持一致，不为元组引入额外特例。

```feng
spec Reducible<T> {
  func reduce(): T;
}

type Triple<T>(T, T, T);

// 若需要 Triple<int> 满足 Reducible<int>，通过 fit 显式建立
fit Triple<int>: Reducible<int> {
  func reduce(): int { return self.item1 + self.item2 + self.item3; }
}
```
