# Feng 语言对象模型规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的对象模型概要说明,聚焦 Feng 原生 `type` 的实例创建、引用语义、成员可变性与方法调用规则。

## 1 对象模型概览

- Feng 原生 `type` 表示 GC 托管的引用对象类型。
- `extern type` 表示 C 兼容数据布局,不属于 Feng 原生对象模型。
- `type` 实例在赋值、传参和返回时默认复制引用,而不是复制整个对象。
- `let` / `var` 作用于绑定和成员可变性,不等价于深度不可变对象。

## 2 `type` 实例的创建方式

Feng 原生对象支持两种创建方式。

### 2.1 对象字面量

对象字面量使用 `Type { field: value }` 形式。

规则说明:

- 对象字面量会分配一个新的 Feng 原生对象实例。
- 字面量中显式列出的成员按名称初始化。
- 未显式列出的成员按其类型默认零值初始化。
- 对象字面量不会自动调用构造函数。

```feng
let user = User { name: "guest", age: 18 };
```

### 2.2 构造函数调用

若 `type` 中定义了与类型同名的构造函数,则可使用 `Type(args)` 创建对象并执行初始化逻辑。

```feng
type User {
    var name: string;
    pr let id: int;
    var age: int;

    fn User(name: string, id: int, age: int) {
        self.name = name;
        self.id = id;
        self.age = age;
    }
}

let user = User("guest", 1, 18);
```

## 3 赋值、传参与返回语义

Feng 原生 `type` 实例采用引用语义。

规则说明:

- 变量赋值只复制对象引用,不复制对象本体。
- 函数参数传递和返回值传递同样只复制引用。
- 多个变量可同时引用同一个对象实例。
- 若需要独立对象,必须显式重新构造。

```feng
let a = User { name: "guest" };
let b = a;
b.name = "admin";
```

上例中,`a` 与 `b` 指向同一个对象,因此对 `b.name` 的修改也会通过 `a` 观察到。

## 4 绑定不可变与成员可变性

`let` / `var` 在对象场景中分为两层语义:

- 绑定层: 变量名是否允许重新绑定到另一个对象
- 成员层: 对象的具体成员是否允许被改写

规则说明:

- `let user = ...;` 表示变量 `user` 不可重新绑定,但不自动冻结对象。
- 若对象成员声明为 `var`,即使对象存放在 `let` 绑定中,仍可修改该成员。
- 若对象成员声明为 `let`,则该成员在初始化后不可再次赋值。

```feng
type User {
    var name: string;
    let created_at: int;
}

let user = User { name: "guest", created_at: 1 };
user.name = "admin";
```

## 5 方法与 `self`

- 成员方法通过 `obj.method(args)` 调用。
- 编译器在成员方法中隐式提供 `self`,指向当前对象实例。
- 构造函数中的 `self` 与成员方法中的 `self` 指向当前正在初始化或调用的对象。

```feng
type Counter {
    var value: int;

    fn inc() {
        self.value = self.value + 1;
    }
}
```

## 6 对象身份与相等性

- Feng 原生 `type` 对象上的 `==` / `!=` 默认比较引用身份。
- 两个不同实例即使成员值完全相同,仍视为不相等。
- 若需要值语义比较,应显式编写比较函数或成员方法。

## 7 与 `extern type` 的边界

- `type` 是 Feng 原生对象,受 GC 管理。
- `extern type` 是 C 兼容布局值,不受 GC 管理。
- `type` 不可直接出现在 `extern fn` 的参数或返回值中。
- `extern type` 不具备 Feng 原生对象的构造函数、成员方法与引用语义。

## 8 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、对象模型概要、类型、函数、GC、C 互操作与包分发。
- [feng-type.md](./feng-type.md): 数据类型、原生类型声明与成员规则。
- [feng-gc.md](./feng-gc.md): GC 托管范围、可达性与内存边界。
- 本文档: Feng 原生 `type` 的实例创建、引用语义、成员可变性与方法规则。
