# Feng 语言模块规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的模块系统概要说明,聚焦 feng 语言的模块声明、模块导入与模块使用规则。

## 1 模块系统概览

- Feng 使用 `module` 进行文件级模块声明,使用 `import` 导入外部模块或二进制包。
- 模块系统支持多级命名空间,通过 `.` 进行层级分隔。
- 模块声明与模块导入在文件内有固定位置约束。
- 模块具备可见性控制,默认包外不可见,需显式声明公开访问权限后才可被外部导入。
- 模块既可用于源码组织,也可用于 feng 自有二进制包和 C ABI 兼容包的导入。

## 2 模块声明规则

- `module` 必须是文件中的第一个非空且非注释行,无花括号、无代码块。
- 一个文件只能属于一个 `module`; 同一个 `module` 可以分布在多个文件中,编译后自动合并。
- 模块名支持 `.` 分隔多级命名空间。
- 同一个 `module` 在单文件或多文件合并后,模块级 `type` 与 `enum` 名称必须唯一,且二者之间不得重名。
- 同一个 `module` 在单文件或多文件合并后,模块级顶层 `func` 可形成重载集合,但重载只在该模块自身声明的同名函数之间成立。
- 若同一模块内两个同名顶层 `func` 的参数类型在当前可见的显式契约关系下存在可重叠匹配,则该重载集合非法,编译期报错。

重名约束说明:

- 若同一模块在多个文件中分别声明同名 `type`、同名 `enum`,或 `type` 与 `enum` 之间重名,编译期报错。
- 若同一模块在多个文件中分别声明同名顶层 `func`,只有在参数类型集合互不重叠时才可构成合法重载; 若仅返回值不同、参数列表完全相同,或参数类型在当前可见的显式契约关系下存在可重叠匹配,则编译期报错。
- 上述约束在单文件内同样成立,不能通过拆分文件规避。

## 3 模块可见性

- 模块可见性遵循 `seal` / `open` 规则。
- `module` 默认等价于 `seal module`,包外不可见。
- 需要对外暴露时,必须显式使用 `open module` 声明公开模块,外部代码才可导入。
- 模块级 `let` / `var` 同样可使用 `seal` / `open` 控制可见性; 只有公开模块上的公开模块级绑定才可被外部 `import` 引入。

可见性示例:

默认私有模块:

```feng
module app.internal.cache;
```

显式私有模块:

```feng
seal module app.internal.cache;
```

公开模块:

```feng
open module app.api.user;
```

导入公开模块:

```feng
import app.api.user;
```

## 4 模块导入规则

- `import` 必须位于模块声明之后、类型与函数定义之前。
- `import` 会把目标模块或包中的公开 `type`、公开 `enum`、公开模块级顶层 `func`、公开模块级 `let` / `var` 引入当前文件的可见作用域，同时引入目标模块中以 `open fit` 导出的契约关系；引入后，该契约关系在当前文件作用域内生效。
- 支持导入源码模块、feng 自有二进制包、C ABI 兼容包。
- 支持使用 `import 模块名 as 别名` 为导入目标声明文件内别名。
- 通过 `import` 引入到当前文件作用域的 `type`、`enum`、顶层 `func` 或模块级 `let` / `var`,不会与其他来源的声明共同组成新的重载集合。重名冲突按以下规则处理,均为编译期错误。

导入符号间的重名冲突（惰性检查）:

- 来自不同 `import` 模块的同名符号（`type`、`enum`、`func`、`let` / `var`）,在代码未引用该名称时不报错; 仅在代码使用裸名引用时,报二义性冲突错误。
- `import` 引入的符号与当前 `module` 其他文件中定义的同名符号,同样在代码未引用该名称时不报错; 仅在代码使用裸名引用时,报二义性冲突错误。

本地声明与导入符号的重名冲突（急切检查）:

- 当前文件内新声明的 `type`、`enum`、顶层 `func` 或模块级 `let` / `var`,若与已通过 `import` 引入的可见名称重名,在声明时即报重复定义错误。

别名导入规则:

- `as` 别名仅在当前文件内生效,不改变被导入模块的真实模块名。
- 使用别名后,该导入目标的公开 `type`、公开 `enum`、顶层 `func` 与模块级 `let` / `var` 都通过 `别名.成员名` 访问,不再以短名直接注入当前作用域。
- 在类型引用位置允许使用完整模块路径访问公开 `type` / `enum`,例如 `my.app.user.User`; 完整模块路径本身即确定目标模块,不要求额外编写 `import my.app.user;`。该规则适用于类型标注、函数参数与返回类型、数组元素类型、对象构造目标类型等所有要求类型名的语境。
- 同一文件内的 `import` 别名必须唯一,不能与其他 `import` 别名重名。
- `import` 别名是当前文件作用域中的名称,不能与当前 `module` 的本地 `type`、`enum`、`spec`、顶层 `func`、模块级 `let` / `var` 重名,也不能与当前文件通过无别名 `import` 引入的公开名称重名。
- 若未使用 `as`,则目标模块的公开 `type`、公开 `enum`、顶层 `func` 与模块级 `let` / `var` 以短名直接进入当前文件作用域。

```feng
import my.app.user as user;
import my.utils.math as math;

let item = user.get_current();
let total = math.add(1, 2);
let limit = math.default_limit;

let current: my.app.user.User;
```

## 5 模块级绑定初始化

模块级 `let` / `var` 绑定采用**绑定级惰性初始化**: 每个绑定在首次被访问时自动执行初始化,而非在模块启动时统一初始化。语言不存在模块级初始化阶段,初始化粒度为单个绑定。

规则说明:

- 每个模块级 `let` / `var` 绑定在首次被读取或写入时自动执行其初始化表达式,初始化完成后不再重复执行。
- 绑定的初始化时机由其首次访问点决定,不依赖声明的书写顺序,也不依赖文件间的排列顺序。
- 同一 `module` 分布在多个文件中时,各文件中的模块级绑定各自独立惰性初始化,无跨文件顺序约束。
- 若绑定的初始化表达式引用了其他模块级绑定（包括本模块或其他模块中的绑定）,被引用的绑定会在本次访问中被自动触发初始化。
- 不存在模块级初始化阶段,因此不存在传统意义上的"初始化顺序"问题; 绑定之间的依赖关系通过惰性触发自然解决。
- 若初始化过程中出现循环访问（A 的初始化读取 B,B 的初始化又读取 A）,则被循环访问的绑定在其初始化表达式执行完成前,存储中保存的是该类型的默认值。

```feng
open module app.main;

import app.config;
import app.runtime;

let name = app.config.load_name();
let boot = app.runtime.start(name);
```

在上例中:

- `name` 在首次被访问时触发初始化,此时 `app.config.load_name()` 被调用。
- `boot` 在首次被访问时触发初始化,此时 `app.runtime.start(name)` 被调用,其中对 `name` 的访问会自动触发 `name` 的初始化（若尚未完成）。
- 两个绑定的初始化不由 `import` 声明顺序或书写顺序驱动,而由各自的运行时首次访问点驱动。

## 6 书写顺序与格式建议

- `module` 声明后进入 `import` 导入段时,建议在两者之间保留一个空行。
- 多个 `import` 声明之间不空行,保持连续排列。
- `import` 导入段与下方类型、函数或其他实现代码之间建议保留一个空行。

书写示例:

```feng
open module my.app.user;

import my.utils;
import my.core;
import my.app.service as service;

type User {
    open var name: string;
}

func load() {
    service.run();
}
```

## 7 符号决议原则

Feng 的符号决议机制遵循以下两项核心原则,指导编译器在名称查找与冲突检测时的行为。

### 7.1 惰性碰撞（Lazy Collision）

当当前文件作用域中存在来自多个来源的同名符号时,编译器不应在 `import` 发生时立即报错,而应在代码**真正引用该符号（标识符决议）时**才检查是否存在二义性。

规则说明:

- 若代码从未引用某个存在潜在同名冲突的名称,即使当前作用域中同时存在该名称的多个候选,也不产生编译错误。
- 仅当代码中使用了一个在当前作用域中存在多个同优先级来源的裸名时,才报二义性冲突错误。
- **例外（急切检查）**: 当前文件内的显式声明（`type`、`enum`、顶层 `func`、模块级 `let` / `var` 或 `import` 别名）与已通过 `import` 引入的可见名称重名时,在声明时即报重复定义错误。这是因为本地声明与导入名称的冲突是确定的,不属于潜在的二义性。

### 7.2 零副作用隔离（Zero Side-Effect Isolation）

每个文件的符号决议空间彼此完全独立。一个文件中的 `import` 声明只在本文件内生效,不会影响同一模块中其他文件的符号可见性。

规则说明:

- 即使文件 A 和文件 B 属于同一 `module`,B 中的 `import` 也不会将任何名称引入 A 的符号空间。
- A 文件中对某名称的决议结果不受 B 文件导入行为的影响,反之亦然。
- 每个文件的符号表独立构建、独立检查、独立报错。

### 7.3 场景验证

以下场景覆盖多文件、同模块、外部导入联动时的关键边界,分别验证惰性碰撞、零副作用隔离和本地声明的急切检查。

假设 `app.core`（文件 A 和 B）为模块,`app.ext1`（文件 C）和 `app.ext2`（文件 D）为两个不同的外部模块。`type Foo` 分别定义在文件 B、文件 C 和文件 D 中。

**场景 1（惰性碰撞 — import vs import）**: 文件 A 同时 `import app.ext1` 和 `import app.ext2`,两个外部模块各自导出了 `Foo`。

- 若 A 中未使用裸名 `Foo`,编译通过,无错误。
- 若 A 中使用裸名 `Foo`,报二义性冲突。

**场景 2（零副作用隔离 + 惰性碰撞 — import vs 同模块其他文件）**: 文件 A 不导入任何外部模块,文件 B 中 `import app.ext1` 但未使用 `Foo`。

- A 文件编译正常,不受 B 的 `import` 影响。
- B 文件中 `import app.ext1` 引入的 `Foo` 与 B 自身定义的 `Foo` 形成潜在冲突,但代码未引用裸名 `Foo`,编译通过。

**场景 3（惰性碰撞触发 — import vs 同模块其他文件,使用时报错）**: 文件 A 不导入任何外部模块,文件 B 中 `import app.ext1` 且使用了裸名 `Foo`。

- A 文件编译正常。
- B 文件在使用裸名 `Foo` 处报二义性冲突: `Foo` 同时来自当前文件自身定义（`app.core`）和 `import app.ext1` 的导入。

**场景 4（急切检查 — 本地声明 vs import）**: 文件 A 中 `import app.ext1`,同时在文件 A 本地定义了 `type Foo`。

- 编译期在文件 A 的 `type Foo` 声明处直接报重复定义错误,无需等到代码引用 `Foo`。

```feng
// 文件 A (app.core) — 不定义 Foo
module app.core;

func do_work() {
    // ...
}
```

```feng
// 文件 B (app.core) — 定义了 Foo
module app.core;

import app.ext1;

type Foo {
    open var id: i32;
}

// 未使用裸名 Foo —— 编译通过
func other_work() {
    // ...
}
```

```feng
// 文件 C (app.ext1) — 定义了 Foo
open module app.ext1;

type Foo {
    open var name: string;
}
```

```feng
// 文件 D (app.ext2) — 定义了 Foo
open module app.ext2;

type Foo {
    open var score: i32;
}
```

## 8 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、模块系统概要、C 互操作、函数、流程控制、异常、自动内存管理、包分发与完整示例。
- 本文档: 模块声明与导入规则的独立补充文档。
