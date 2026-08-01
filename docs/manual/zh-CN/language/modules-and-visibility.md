# 模块与可见性

模块组织源代码，`open` 与 `seal` 控制声明的可见范围。详细规则见[模块规范](../../../specifications/feng-module.md)和[可见性规范](../../../specifications/feng-visibility.md)。

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

无别名导入会把目标模块的公开名称以短名引入当前文件。别名导入通过限定名访问：

```feng
let current = service.load();
```

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

来自多个导入的同名符号在真正使用裸名时产生二义性。优先使用导入别名或完整模块路径消除冲突：

```feng
import app.first as first;
import app.second as second;

let a = first.User {};
let b = second.User {};
```
