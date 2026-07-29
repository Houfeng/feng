# Feng 语言可见性规范

本文档定义 Feng 语言的三级可见性模型。

## 1 可见性关键字

- `seal` — 私有，外部不可访问
- `open` — 公开，外部可访问

## 2 三级可见性模型

Feng 的可见性由三级层次共同决定，外部代码访问某个成员时，必须三级均为 `open` 才可达。

### 第一级：module 可见性

- 控制模块自身是否对外可见。
- 默认 `seal`，包外不可导入。
- 需显式声明 `open module` 才可被外部 `import`。

```feng
// 默认 seal，包外不可见
module app.internal.cache;

// 显式 open，包外可导入
open module app.api.user;
```

### 第二级：module 成员可见性

- 控制模块内顶层声明（`let` / `var` / `type` / `spec` / `enum`）是否对外可见。
- 默认 `seal`，仅模块内部可访问。
- 需显式声明 `open` 才可被外部引用。

```feng
open module app.api.user;

// 默认 seal，外部不可见
let internal_cache = [];

// 显式 open，外部可访问
open let MAX_RETRY = 3;

// 显式 open，外部可引用此类型
open type User {
  // ...
}
```

### 第三级：type 成员可见性

- 控制类型内部成员（构造函数 / 方法 / 字段）是否对外可见。
- 默认 `open`，外部可访问。
- 需显式声明 `seal` 才能隐藏。

```feng
open type User {
  // 默认 open，外部可访问
  var name: string;

  // 默认 open，外部可调用
  func User(name: string) {
    self.name = name;
  }

  // 显式 seal，外部不可调用
  seal func resetInternal() {
    // ...
  }
}
```

## 3 可见性组合效果

外部代码能否访问某个 type 成员，取决于三级可见性的组合：

| module | module 成员 (type) | type 成员 | 外部可访问 |
|--------|-------------------|-----------|-----------|
| open   | open              | open      | 是        |
| open   | open              | seal      | 否        |
| open   | seal              | open      | 否        |
| seal   | open              | open      | 否        |

在 `seal` 模块中声明 `open type`，等价于其他语言中的 `internal`——同包内可见，包外不可访问。

## 4 包内可见类型

当需要一个类型仅在包内共享、但不对外暴露时，将 module 保持默认 `seal`，同时将 type 声明为 `open`：

```feng
// module 默认 seal，包外无法 import 此模块
module app.internal.cache;

// type 声明为 open，同包其他模块可访问
open type CacheEntry {
  var key: string;
  var value: string;

  func CacheEntry(key: string, value: string) {
    self.key = key;
    self.value = value;
  }
}
```

同包内的其他模块可正常导入并使用：

```feng
module app.internal.store;

import app.internal.cache;

func save(k: string, v: string) {
  let entry = CacheEntry(k, v);
  // ...
}
```

包外代码无法导入 `app.internal.cache`，因此 `CacheEntry` 对外不可见。

## 5 完全公开类型

module、type、type 成员三级均为 `open`，外部可自由导入、实例化、访问所有成员：

```feng
open module std.io;

open type Writer {
  var path: string;

  func Writer(path: string) {
    self.path = path;
  }

  func write(data: string) {
    // ...
  }
}
```

## 6 公开类型但隐藏部分成员

type 对外公开，但通过 `seal` 隐藏内部实现细节：

```feng
open module app.api.session;

open type Session {
  // 外部可读
  var id: string;

  // 外部不可访问
  seal var token: string;

  func Session(id: string, token: string) {
    self.id = id;
    self.token = token;
  }

  // 外部可调用
  func isValid(): bool {
    return self.checkToken();
  }

  // 外部不可调用
  seal func checkToken(): bool {
    // ...
  }
}
```

外部代码可以创建 `Session`、调用 `isValid()`，但无法访问 `token` 字段或调用 `checkToken()`。

## 7 禁止外部实例化

type 对外公开但构造函数标记为 `seal`，外部只能通过工厂方法获取实例：

```feng
open module app.api.connection;

open type Connection {
  var host: string;

  // 外部不可直接调用构造函数
  seal func Connection(host: string) {
    self.host = host;
  }

  // 外部通过此方法获取实例
  static func create(host: string): Connection {
    // 校验、池化等逻辑
    return Connection(host);
  }
}
```

外部代码只能调用 `Connection.create()`，无法直接 `Connection(host)`。

## 8 模块私有类型

在 `open` 模块中，type 默认为 `seal`，仅当前模块内部可用，同包其他模块也无法访问：

```feng
open module app.api.user;

// 默认 seal，仅本模块内部使用
type PasswordHasher {
  func PasswordHasher() {}

  func hash(plain: string): string {
    // ...
  }
}

// 对外公开
open type User {
  var name: string;

  func User(name: string) {
    self.name = name;
  }
}
```

外部代码可以访问 `User`，但 `PasswordHasher` 仅 `app.api.user` 模块内部可见。

## 9 默认值汇总

| 层级           | 默认可见性 |
|---------------|-----------|
| module        | `seal`    |
| module 成员    | `seal`    |
| type 成员      | `open`    |

## 10 私有成员的表示类型

私有成员可以使用在成员声明位置可访问的私有类型。成员类型是否为泛型不改变该
规则；泛型实参中的类型按相同规则处理。

```feng
open module app.api.box;

type Entry<T> {
  let value: T;
}

open type Box<T> {
  seal let entry: Entry<T>;
}
```

`Box<T>.entry` 为私有成员，因此可以使用当前模块的私有类型 `Entry<T>`。
其他模块仍不能直接引用、导入或构造 `Entry<T>`。
