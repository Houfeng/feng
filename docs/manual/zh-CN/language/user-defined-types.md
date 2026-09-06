# 自定义类型

`type` 把数据和行为组织为具名类型。对象类型是托管引用类型；具名元组是值类型。

## 字段与对象字面量

```feng
type User {
  let id: int;
  var name: string;
}

let user = User { id: 1, name: "Alice" };
user.name = "Bob";
```

`let` 字段在完成初始化后不可修改，`var` 字段可以修改。对象赋值复制引用：

```feng
let alias = user;
alias.name = "Carol";
println(user.name); // Carol
```

## 构造函数

构造函数与类型同名，通过 `self` 初始化当前对象：

```feng
type User {
  let id: int;
  var name: string;

  func User(id: int, name: string) {
    self.id = id;
    self.name = name;
  }
}

let user = User(1, "Alice");
let renamed = User(2, "Bob") { name: "Carol" };
```

花括号形式的对象类型没有显式构造函数时，可使用默认无参构造和对象字面量。对象字面量也可以在构造后
覆盖尚可绑定或可写的成员。圆括号形式的具名 tuple 不适用这条规则，它没有普通构造函数。

## 方法

```feng
type Counter {
  var value: int;

  func increment() {
    self.value += 1;
  }

  func current(): int {
    return self.value;
  }
}
```

实例方法通过 `self` 访问当前实例。方法可以重载，但不能只依靠返回类型区分。

## 静态成员

```feng
type Counter {
  open static var created: int = 0;

  open static func create(): Counter {
    Counter.created += 1;
    return Counter {};
  }

  var value: int;
}

let counter = Counter.create();
```

静态成员通过类型名访问，不能通过实例访问。

## 终结器

需要释放外部资源时，可以定义终结器：

```feng
type Resource {
  var handle: int;

  func Resource(handle: int) {
    self.handle = handle;
  }

  func ~Resource() {
    // 释放由 handle 表示的外部资源
  }
}
```

托管内存由 Feng 自动管理；终结器适合清理文件句柄等非托管资源。可预测的词法清理优先使用 `defer`，详见[异常处理](./error-handling.md)。

## 具名元组与值类型

圆括号形式声明具名元组；`@value` 可用于声明值语义对象类型。二者的复制与生命周期规则不同于普通对象类型。
