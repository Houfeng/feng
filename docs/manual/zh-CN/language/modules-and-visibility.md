# 模块与可见性

模块组织源代码，`open` 与 `seal` 控制声明的可见范围。

## 声明模块

`module` 必须是文件中第一个非空、非注释声明：

```feng
module app.internal;
```

模块默认对包外不可见。公开模块使用：

```feng
open module app.api;
```

同一模块可以分布在多个文件中，文件路径不决定模块名。

## 导入模块

`import` 位于模块声明之后、其他声明之前：

```feng
module app;

import std.io;
import app.user;
import app.service as service;
```

公开顶层声明有三种名称访问形式：

```feng
import app.user;
import app.service as service;

let first = load();                    // import 后使用短名
let second = service.load();           // import + as 后使用别名
let third = app.archive.load();        // 无需 import，直接使用完整模块路径
```

三种形式访问同一公开声明。完整模块路径适用于公开的 `type`、`enum`、`spec`、顶层函数和模块级
`let` / `var`；模块及目标声明都必须公开。

表达式中的局部值优先于同名的完整模块路径首段。需要访问被局部值遮蔽的模块时，使用不冲突的
import alias。

每个文件的导入彼此独立；同一模块中另一个文件的 `import` 不会自动作用于当前文件。

## 三级可见性

外部访问类型成员时，要依次通过三个层级：

1. 模块：默认 `seal`，公开时写 `open module`。
2. 模块成员：顶层 `type`、`spec`、`enum`、函数和绑定默认 `seal`，公开时写 `open`。
3. 类型成员：字段、方法和构造函数默认 `open`，隐藏时写 `seal`。

```feng
open module app.api.user;

open type User {
  var name: string;
  seal var token: string;

  func display(): string {
    return self.name;
  }
}
```

公开 API 的签名不能泄漏可见范围更窄的类型。

## 公开 fit

`fit` 不是可命名声明。只有位于公开模块中的 `open fit` 才能作为包外公开扩展；其他 `fit` 只在声明模块内生效。

```feng
open module app.extensions;

open fit User {
  func greeting(): string {
    return "Hello, " + self.name;
  }
}
```

使用方需要导入 `app.extensions` 才能使用该扩展。

## 避免名称冲突

同模块顶层声明之间的非法重名会在声明检查阶段报错，不以是否使用为条件；顶层函数允许合法重载。

导入别名是当前文件产生的名称，不是模块的顶层声明。它与本文件的顶层声明或其他别名同名时，即使未使用也立即报错；这里不包括嵌套块中的局部绑定。

以下两类别名冲突在使用点才报二义性，未使用时允许：

- 别名与同模块其他文件的声明同名；
- 别名与无别名 import 引入的公开名称同名。

诊断会指出冲突来源。`别名.成员` 也会触发这一检查，无论用于类型引用还是普通表达式；不会根据后续成员是否存在来选择来源，也没有别名优先规则。

无别名 import 引入的名称与本文件或同模块其他文件顶层声明同名时，也在使用点报二义性，顶层声明不会自动遮蔽导入名称。前文的“局部值优先于模块路径”规则保持适用。

来自多个导入的同名符号在真正使用裸名时产生二义性。可使用导入别名或完整模块路径消除冲突：

```feng
import app.first as first;
import app.second as second;

let a = first.User {};
let b = app.second.User {};
```
