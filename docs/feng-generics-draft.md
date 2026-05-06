# Feng 语言泛型规范

## 语法示例

```feng
type List<T> {...}          // type 关键字 → 类型泛型
fn   map<T>(...) { ... }    // fn 关键字 → 函数泛型
spec Comparable<T> {...}    // spec 关键字 → 对象契约类型泛型
spec Foo<T>(...): T;        // spec 关键字 → 函数契约类型泛型

type UserType<T> {          // 泛型类型
  fn foo(): T {...}         // 泛型方法，使用类型泛型参数 T
  fn bar<U>(...): T {...}   // 泛型方法，使用方法泛型参数 U
}

type Box<T> {
  fn map<U>(value: U): T {...}  // 合法：方法泛型参数 U 与类型泛型参数 T 不重名
  fn bad<T>(value: T): T {...}  // 非法：方法泛型参数 T 与类型泛型参数 T 重名
}
```

## 语法解析

在 feng 中严禁 Parser 反向依赖 Semantic，在泛时定义时 Parser 可通过 type/fn/spec 关键字来安全判断是否为泛型，但在调用时 Parser 无法直接判断是否为泛型，除非通过 Semantic 分析，但 Parser 禁止反向依赖 Semantic。所以需要从语法上消除歧义。

歧义仅存在于泛型方法或函数的调用时，通过 `foo:<T>(...)` 形式来消除歧义。

凡是在调用点显式写出泛型参数，无论目标是顶层函数、模块函数还是成员方法，都统一使用 `:<...>` 语法；`foo<T>(...)`、`pkg.foo<T>(...)`、`obj.foo<T>(...)` 都不作为显式泛型调用语法。

```feng
foo<bar>(qux);      // 泛型调用？还是 (foo < bar) > (qux)？
foo:<int>(x);       // 顶层函数显式泛型调用
pkg.foo:<int>(x);   // 模块函数显式泛型调用
obj.foo:<int>(x);   // 成员方法显式泛型调用
```

无歧义示例

```feng
// 变量声明：: 后面一定是类型
let x: List<int> = ...;

// 函数参数/返回值：类型位置固定
fn foo(a: Map<string, int>): List<int> {...}

// 字段声明
type Foo {
  let items: List<int>;
}

// 类型构造
let x = List<int>();
```

## 编译期约束

- 在泛型 `type` 内声明泛型方法时，方法自己的泛型参数名不得与所在类型的泛型参数名重名。
- 若方法泛型参数名与所在类型泛型参数名重名，编译期必须报错。

最终效果

```feng
// 定义：正常用 <>
fn map<T, U>(p1: T, p2: U): T { ... }
spec Comparable<T> { ... }
type List<T> { ... }

// 类型位置：正常用 <>（无歧义）
let x: List<int> = ...;
fn foo(a: Map<string, int>): List<int> {...}

// 调用（推导成功）：不写泛型参数 —— 最常见
let r = map(data, option);

// 调用（需显式指定）：单冒号，避免泛型参数歧义
let r = map:<int, string>(data, option);

type UserType<T> {
  fn convert<U>(value: U): T {...}
  // fn convert<T>(value: T): T {...}  // 错误：方法泛型参数 T 与类型泛型参数 T 重名
}
```
