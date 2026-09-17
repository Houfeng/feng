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

## 用 @friend 定向开放 seal 成员

`@friend(Type, ...)` 把指定的 `seal` 成员开放给列出的具体类型，适合受限工厂和协作类型。下面同时使用字段、实例方法、静态工厂和同包 `fit` 的定向授权：

```feng
module manual_friend;
import std.io;
import std.numeric;

/** Restricts creation and sensitive state to an auditor. */
type Vault {
  @friend(Auditor)
  seal var code: int;

  /** Initializes the private state. */
  seal func Vault(code: int) { self.code = code; }

  /** Exposes one private operation to the friend. */
  @friend(Auditor)
  seal func read(): int { return self.code; }

  /** Supplies the authorized construction entry. */
  @friend(Auditor)
  seal static func create(code: int): Vault { return Vault(code); }
}

/** Receives member-specific permission. */
type Auditor {
  /** Uses the restricted factory. */
  static func create_vault(): Vault { return Vault.create(41); }

  /** Uses the authorized field and method. */
  func inspect(vault: Vault): int {
    vault.code += 1;
    return vault.read();
  }
}

fit Auditor {
  /** Uses the same friend identity within this package. */
  func snapshot(vault: Vault): int { return vault.code; }
}

/** Uses only the public auditor interface. */
func main(args: string[]) {
  let auditor = Auditor {};
  let vault = Auditor.create_vault();
  println<int>("{0} {1}", auditor.inspect(vault), auditor.snapshot(vault));
}
```

输出为 `42 42`。顶层 `main` 不能直接读取 `vault.code` 或调用 `Vault.create`。注解也可用于 seal 静态字段和 `spec`／`fit` 中允许声明的 seal 成员；不能用于构造函数或终结器，所以受限创建使用上例的静态工厂。

授权只针对被标注的成员，不自动传递给 friend 的 friend；同包且目标正是 friend 类型的 `fit` 可使用该权限，其他包的 `fit` 不可。模块和所属类型必须仍然可见，`let` 可变性、泛型实参身份和其他访问规则仍要满足，不能用 friend 穿透这些边界。

## 模块绑定与静态初始化

初始化按绑定延迟执行。导入模块只影响名称可见性，不会执行该模块所有初始化器；某个模块绑定首次被读取或写入时才初始化一次：

```feng
module manual_lazy_init;
import std.io;
import std.numeric;

var calls: int;

/** Counts evaluation of a module initializer. */
func load(): int { calls += 1; return 10; }

let cached: int = load();

/** Owns separate static storage for each closed type. */
type Cache<T> { static var count: int = 0; }

/** Observes lazy initialization and closed generic static state. */
func main(args: string[]) {
  println<int>("{0}", calls);
  let first = cached;
  let second = cached;
  Cache<int>.count = 2;
  println<int>("{0} {1} {2}", first, second, calls);
  println<int>("{0} {1}", Cache<int>.count, Cache<string>.count);
}
```

输出为 `0`、`10 10 1`、`2 0`。初始化器访问其他绑定时，会按访问路径触发那些绑定的初始化；文件排列、声明顺序和 import 顺序不规定初始化顺序。

必须避免实际执行的循环依赖：若 A 的初始化读取 B，B 又读取尚在初始化的 A，当前行为等同无限递归，最终耗尽调用栈，不会自动检测、打断或恢复。类型静态绑定遵循相同的延迟规则；同一闭合泛型类型共享同一静态存储，不同完整类型实参拥有独立存储和初始化状态。
