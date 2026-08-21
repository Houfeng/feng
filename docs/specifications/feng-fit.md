# Feng 语言 `fit` 规范

本文档说明 Feng 中 `fit` 的职责、语法、语义与实现约束。`fit` 用于声明具体类型如何进入一个或多个 object-form `spec` 契约,或如何给类型自身补充额外能力。

内建类型与数组目标（如 `fit int[]`、`fit T[]`、`fit T[!]`）的专门规则统一见 [feng-fit-builtin-type.md](./feng-fit-builtin-type.md)。

## 1 职责

- `fit` 用于显式建立“具体类型满足 object-form 契约”的关系。
- `fit` 支持非侵入适配,允许在不修改原始 `type` 定义的前提下建立契约关系。
- `fit` 还支持为类型自身补充扩展成员。
- `fit` 支持在不特化目标类型参数的前提下消费目标泛型 `type` 已声明的类型参数,并按与直接声明一致的规则建立泛型契约关系或补充扩展成员。
- `fit` 不用于任意类型之间的开放式自动桥接。

## 2 术语

- 契约适配: `fit A: B`、`fit A: B, C`、`fit A<T>: B<T>` 或 `fit A<T>: B<T>, C<T>` 形式,左侧为具体类型（含 `type`、`enum`）或对目标泛型 `type` 已声明类型参数的直接引用,右侧为一个或多个 object-form `spec`。
- 自扩展: `fit A { ... }` 或 `fit A<T> { ... }` 形式,为类型 `A` 增加成员,不建立新的 `spec` 契约关系。
- 静态扩展方法: `fit` 块中使用 `static func` 声明的类型级扩展方法; 引入定义该 `fit` 的模块后,其公开静态方法可通过目标类型名访问。`fit` 不得声明 `static let` 或 `static var`。
- 可见契约关系: 当前作用域可见的“`type` 与 `spec` 已建立适配”的事实。
- 适配检查: 编译器对 `fit` 声明进行的满足性、冲突和合法性校验。
- `open fit`: `fit` 声明的公开导出形式；导出后其他文件通过 `import` 引入当前模块时，该契约关系一同在导入文件内可见并生效。
- 孤儿适配: `fit A: B` 中 `A` 与 `B` 的实现源码均不在当前包中的满足声明；孤儿适配在当前包内正常建立契约关系并生效，但有效性不得超出当前包；以 `open fit` 形式声明的孤儿适配，编译器移除其导出并输出肯定式提示（非错误、非警告）。纯自扩展 `fit A { ... }` / `open fit A { ... }` 不属于孤儿适配判定范围。
- 目标类型参数引用: `fit Box<T, U>` 中的 `<T, U>` 不是新的类型参数定义,而是对目标 `type Box<T, U>` 已声明类型参数的逐位置引用。

## 3 语法

正确语法一,契约适配:

```feng
fit User: Named;
fit User: Named, Auditable;
```

正确语法二,公开导出契约适配:

```feng
open fit User: Named;
open fit User: Named, Auditable;
```

正确语法三,带块体的契约适配与自扩展:

```feng
fit User: Named {
  func display(): string {
    return self.first + " " + self.last;
  }
}

fit User {
  func say_hi() {
    print("Hello, " + self.name);
  }
}
```

正确语法四,公开导出带块体的契约适配:

```feng
open fit User: Named {
  func display(): string {
    return self.first + " " + self.last;
  }
}
```

正确语法五,泛型契约适配与泛型自扩展:

```feng
type Box<T> {
  let value: T;
}

spec Reader<T> {
  func get(): T;
}

fit Box<T>: Reader<T>;

fit Box<T>: Reader<T> {
  func get(): T {
    return self.value;
  }
}

fit Box<T> {
  func has_value(): bool {
    return true;
  }
}
```

正确语法六,静态扩展方法:

```feng
fit User {
  open static func from_name(name: string): User {
    return User { name: name };
  }
}

fit string {
  open static func fromUtf8Bytes(bytes: byte[]): string { ... }
}

fit Box<T> {
  open static func of(value: T): Box<T> {
    return Box<T> { value: value };
  }
}
```

`fit` 静态方法与 `type` 静态方法使用同一成员关键字顺序: `[open | seal] static func ...`。引入定义该 `fit` 的模块后,公开静态方法在调用侧可见面可用,如 `User.from_name("feng")` 或 `string.fromUtf8Bytes(bytes)`。

`fit Source` 中的 `@mixable` 静态方法参与成员展开时，open 与 seal 方法的候选、wrapper
及直接 mix 授权统一遵循 [Feng 语言函数规范](./feng-function.md#43-mixable-静态方法与-wrapper)；
fit 声明本身仍须满足本规范的现有可见性、导出和孤儿适配规则。

正确语法七,泛型参数的子到父传递与直接声明一致:

```feng
spec Animal {}
spec Dog: Animal {}

type Cage<T: Dog> {
  let value: T;
}

spec View<T: Animal> {
  func get(): T;
}

fit Cage<T>: View<T> {
  func get(): T {
    return self.value;
  }
}
```

正确语法八,同名但泛型参数数量不同的 `type` 在 `fit` 中按不同目标类型处理:

```feng
type UserType<T, U> { ... }

spec UserSpec<T, U> { ... }

fit UserType<T, U>: UserSpec<T, U>;
fit UserType<T, U>: UserSpec<T, U> { ... }
fit UserType<T, U> { ... }
```

错语法一,契约适配右侧不是 `spec`:

```feng
fit User: Order;
```

错语法二,`fit` 自扩展误写为契约适配:

```feng
fit User: User {
  func ping(self) {}
}
```

错语法三,`fit` 块中声明存储字段:

```feng
fit User {
  let nickname: string;  // 错误: fit 块中不得声明 let/var 字段
  var visits: int;       // 错误: fit 块中不得声明 let/var 字段
  static let count: int; // 错误: fit 块中也不得声明 static let/static var
}
```

错语法四,`fit` 契约适配右侧出现 callable-form `spec`:

```feng
spec Click(): void;

fit Button: Click;
```

错语法五,`fit` 契约适配右侧出现 union-form `spec`:

```feng
spec Choice: int | string;

fit Box: Choice;
```

错语法六,在 `fit` 中特化目标泛型 `type` 到具体类型:

```feng
type Box<T> {
  let value: T;
}

spec Reader<T> {
  func get(): T;
}

fit Box<int>: Reader<int>;
```

错语法七,在 `fit` 中增删、重排或改写目标类型参数:

```feng
type Pair<T, U> {
  let left: T;
  let right: U;
}

fit Pair<T> {}
fit Pair<T, U, V> {}
fit Pair<U, T> {}
```

错语法八,`fit` 左侧写成不存在的同名不同泛型参数数量 `type`:

```feng
type UserType<T, U> { ... }

fit UserType<T> { ... }
```

## 4 语义

- `fit` 有两类合法用法: 契约适配与自扩展。
- 契约适配形式中,右侧必须是一个或多个 object-form `spec`; 编译器会检查左侧具体类型是否满足全部目标契约。
- 泛型 `fit` 的本质仍然是“声明约束关系,并可选添加扩展方法”; 它不引入新的泛型实例化语义。
- 在 `fit Box<T>: Reader<T>`、`fit Box<T>: Reader<T> {}`、`fit Box<T> {}` 这类写法中,左侧 `<T>` 不是新的类型参数定义,而是对目标 `type Box<T>` 已声明类型参数的直接引用。
- 泛型 `fit` 对目标泛型 `type` 的每个具体实例生效。例如 `fit Box<T>: Reader<T>` 使 `Box<int>` 可按 `Reader<int>` 使用,其 `fit` 块方法中的 `T` 必须按该具体实例的类型实参解析。
- 因此,泛型 `fit` 左侧必须与目标泛型 `type` 的声明保持一致: 参数个数、参数顺序与参数身份都必须逐位置对应; `fit` 不得在左侧把目标泛型 `type` 特化到具体类型,也不得增删、重排或改写目标类型参数。
- 泛型参数数量也是目标 `type` 身份的一部分。`UserType<T>` 与 `UserType<T, U>` 是两个不同的具名 `type`；`fit` 左侧必须先按“名称 + 泛型参数数量”解析到一个已经存在的目标 `type` 声明,若不存在精确匹配,则必须按“找不到目标 type 声明”报错,而不是尝试匹配其他同名但参数数量不同的 `type`。
- callable-form `spec` 只描述可调用签名形状,不能作为 `fit A: B` 的目标。
- union-form `spec` 只描述值进入时的 member 选择与收窄边界,不能作为 `fit A: B` 的目标; union-form 的专门规则见 [feng-union-type.md](./feng-union-type.md)。
- 契约适配可带块体; 带块体时,编译器先把新增成员计入类型可见能力集合,再做契约满足检查。
- 自扩展形式 `fit A { ... }` 仅向 `A` 补充能力,不建立新的 `spec` 关系。
- `fit` 静态方法仅向目标类型补充类型级行为能力,不建立新的 `spec` 关系,也不补充任何存储字段或绑定。其可见性与 fit 实例方法一致: 定义所在模块内可见; 若 `fit` 声明和方法均对调用侧可见,其他模块通过 `import`/`use` 引入后可通过目标类型名访问公开静态方法。
- 当前位置可见的 `fit` 静态方法与目标 type 自有静态方法使用同一 `Type.method` 方法值
  来源面；在明确 callable-form `spec` 目标下按
  [Feng 语言函数规范](./feng-function.md) 形成值，并在形成点固定 fit 与方法声明。
- `fit` 静态方法可作为 object-form `spec` 静态方法的满足来源（`fit` 仍不得声明 `static let` / `static var`）; spec 静态字段只能由 `type` 自身提供。
- `enum` 可作为 `fit` 左侧目标；其适配与扩展在语义上与其他具名目标一致，但 `enum` 声明体本身仍不得直接声明方法。
- 在泛型契约适配右侧继续传递目标类型参数时,其子到父约束传递规则与直接写在 `type` / `spec` 声明头上的泛型父约束规则完全一致; 若当前目标类型参数已带有更强约束,则按同一套“在当前可见契约关系下是否满足父约束”规则判定合法性。
- 若没有在 `type` 声明头或可见 `fit` 中显式建立关系,即使结构看似满足 `spec`,编译器也不自动建立契约关系。
- `fit` 建立的契约关系以作用域可见性为生效条件；默认仅在声明所在 `module` 内有效，不自动对外暴露。
- 使用 `open fit` 可将契约关系公开导出；其他文件通过 `import` 引入当前模块后，该契约关系在导入文件作用域内生效。
- 孤儿适配（`A` 与 `B` 实现源码均不在当前包中）在当前包内正常建立契约关系并生效；其有效性不超出当前包，`open fit` 形式的孤儿适配由编译器移除导出并输出肯定式提示（非错误、非警告）。无右侧 `spec` 的纯自扩展 `fit` 不受孤儿规则影响。
- 声明层面的可见性修饰（`open` 导出或无修饰默认模块内可见）与是否带块体是两个独立维度，四种组合均合法，语义互不影响；块体内 `func` 成员的可见性（默认公开或 `seal` 收窄）是独立于声明层面的第三个维度。
- `fit` 块内的函数不能访问目标 `type` 的私有成员（`seal` 修饰的字段或方法），无论目标 `type` 定义在当前包内还是外部包中；`fit` 块内只能使用目标类型对外公开的能力。
- 同包 `fit T: S` 可以按 `spec` 规范的兼容矩阵使用 `T` 的 `seal` 成员满足
  `S` 的 `seal` requirement；跨包 `fit T: S` 不得使用 `T` 的任何 `seal`
  成员建立满足关系，即使 imported `.ft` 为布局恢复保留了对应私有字段事实。
  跨包 fit 自己显式提供的方法实现不受此限制。
- 当 fit 的目标 type 在访问点满足 object-form `spec S` 时，fit 实例或静态
  方法可以通过 spec 视角访问 `S` 的 `seal` 成员。该权限只属于 spec
  访问与 witness 分派，不使 fit 成为目标 type 自身，也不允许直接访问
  目标 type 的 `seal` 成员。
- `fit` 块中新增方法的重载与冲突判定规则,必须与直接定义在目标 `type` 中的成员方法保持一致; 若是泛型方法,则同样按既有“名称 + 泛型参数数量 + 参数个数 + 参数类型”规则判定,约束不参与重载区分。
- `fit` 静态方法与目标 `type` 的静态方法形成静态重载集合,不与实例方法互相冲突; 静态方法之间按既有方法重载规则判定。
- 同一 `(A, B)` 组合的多次 `fit A: B` 声明视为幂等，编译器自动合并；若合并后产生方法签名冲突，以与多 `spec` 冲突相同的规则判定并报错。
- `type A: B` 声明头建立的满足关系与 `fit A: B` 是两个独立的声明，各自通过自身的 `open` 决定可见范围，互不影响；其他 `module` 通过 `import` 引入相应模块后各自升效。

## 5 规则

分为「必须、禁止、建议」。

- [必须] 契约适配形式中,`fit A: B, C, ...` 的左侧必须是合法 fit 目标（具体 `type`、`enum` 或 [feng-fit-builtin-type.md](./feng-fit-builtin-type.md) 允许的内建目标）,右侧每一项都必须是 object-form `spec`。
- [必须] 若目标 `type` 为泛型 `type`,则允许使用 `fit A<T>: B<T>`、`fit A<T>: B<T> {}` 与 `fit A<T> {}` 这类泛型 `fit` 形式。
- [必须] 泛型 `fit` 左侧的类型参数列表必须逐位置引用目标泛型 `type` 已声明的类型参数。
- [必须] 当 `fit` 左侧是具名 `type` 目标时,必须按“名称 + 泛型参数数量”解析到一个已存在的 `type` 声明；同名但泛型参数数量不同的 `type` 不得互相代替。
- [必须] 契约适配形式中,不带块体与带块体都表示“声明满足并触发编译期检查”。
- [必须] `fit` 块中只能声明实例 `func` 方法或 `static func` 静态方法; `let`/`var` 绑定只能在 `type` 定义中声明,`fit` 块中不得声明任何实例字段或静态绑定。字段必须属于对象本体,`fit` 只负责方法实现、静态方法扩展与契约建立。
- [必须] `fit` 块中 `func` 成员的可见性规则与 `type` 中成员方法一致：默认公开，需要收窄时显式使用 `seal`。
- [必须] fit 实例普通方法和静态普通方法在显式 `seal` 时允许使用 `@friend`；
  `fit FriendType` 还可以在与被访问成员同包时使用目标 type 的 friend 身份。两种
  方向均只放行明确标注的成员，不扩大目标 type 普通 seal 可见性，完整规则统一见
  [Feng 语言可见性规范](./feng-visibility.md#103-friend-seal-成员)。
- [必须] `fit` 块中的 `func` 成员与 `type` 成员方法使用完全相同的隐式 `self` 规则；`self` 由编译器隐式提供。对值类型，它引用当前值的存储；对引用类型，它引用接收者引用所指向的实例。实例方法调用本身不复制接收者。值类型与引用类型在赋值、显式参数传递、返回及捕获边界上的复制规则统一见 [Feng 语言类型规范](./feng-type.md)。
- [必须] `fit` 静态方法必须写作 `[open | seal] static func`; 静态方法不接收隐式 `self`。
- [必须] `fit` 静态方法值必须复用类型名静态直接调用的 fit 可见面、访问过滤、owner
  代入和重载规则；不可见或不可访问的 fit 方法不得成为方法值候选。
- [必须] `fit` 块内的函数不得访问目标 `type` 的私有成员（`seal` 字段或 `seal` 方法），无论目标 `type` 定义在当前包内还是外部包中。
- [必须] 泛型 `fit` 右侧若继续传递目标类型参数,其合法性检查必须与直接写在 `type` / `spec` 声明头中的父泛型 `spec` 传递规则一致。
- [必须] `fit` 块中新增方法的重载与冲突判定规则必须与直接定义在目标 `type` 中的成员方法一致。
- [必须] 当同一个 `type` 通过 `fit` 同时满足多个 `spec` 时,若出现“方法名相同,且参数个数、参数类型和参数顺序一致,但返回值类型不一致”的方法签名,必须视为冲突。
- [必须] `fit` 建立的契约关系必须遵守 `spec` 规范中的字段/方法精确匹配规则。
- [必须] `fit` 方法作为 object-form `spec` requirement 的实现来源时，必须
  遵守 `spec` 规范定义的 requirement/实现成员可见性兼容规则。
- [必须] `fit` 与目标 type 同包时，可以使用目标 type 的 `seal` 成员满足
  `spec seal` requirement；二者跨包时必须从满足候选和 witness 选择中排除
  目标 type 的全部 `seal` 成员，但不得排除当前 fit 自己声明的方法实现。
- [必须] 目标 type 满足成员原声明 spec 时，fit 实例方法和静态方法可以
  通过 spec 视角访问该 spec 的 `seal` 成员；该授权不得用于具体 type
  成员访问。
- [必须] `fit` 静态方法可作为 object-form `spec` 静态方法的满足来源; spec 静态字段只能由 `type` 自身提供,fit 不得声明 `static let` 或 `static var`。
- [必须] `fit` 建立的契约关系默认仅在声明所在 `module` 内可见；如需跨模块共享，须使用 `open fit` 导出，其他 `module` 通过 `import` 引入后方可激活该契约关系。
- [必须] 孤儿适配导出限制仅适用于带 `spec` 右侧的契约适配；无右侧 `spec` 的纯自扩展 `fit` 不受孤儿规则影响。
- [必须] 同一 `(A, B)` 组合的多次 `fit A: B` 声明幂等，编译器自动合并；合并后若产生方法签名冲突（同名同参数但返回值不一致），以与多 `spec` 冲突相同的规则判定并报错。
- [禁止] 契约适配右侧出现普通 `type`。
- [禁止] 契约适配右侧出现 callable-form `spec` 或 union-form `spec`。
- [禁止] 在 `fit` 左侧把目标泛型 `type` 特化为具体类型,或通过增删、重排、改写目标类型参数来改变目标 `type` 的声明形状。
- [禁止] 在仅存在 `type UserType<T, U>` 这类声明时，把 `fit UserType<T>` 当作同一个目标 `type` 使用。
- [禁止] 在同一适配声明中重复列出同一个 `spec`。
- [禁止] `fit` 块中声明 `let` 或 `var`,无论是否带 `static`。
- [说明] 声明层面的可见性修饰（`open` 或无修饰）与是否带块体是两个独立维度，任意组合均合法，语义互不影响。
- [说明] 孤儿适配规则：`fit A: B` 或 `open fit A: B` 中 `A` 与 `B` 实现源码均不在当前包中时，该声明在当前包内正常有效；`open fit` 形式的孤儿适配不得导出，编译器移除其导出并输出肯定式提示（非错误、非警告），明确告知有风险的公开契约导出已被移除。无右侧 `spec` 的纯自扩展 `fit` 不受该规则影响。
- [说明] `type A: B` 声明头建立的满足关系与 `fit A: B` 是两个独立声明，各自通过自身的 `open` 决定是否公开导出，互不影响；其他 `module` 通过 `import` 引入相应模块后各自升效。
- [建议] 优先在 `type` 声明头表达定义者主动承诺; `fit` 优先用于第三方类型或无法修改源类型定义的场景。

## 6 编译期

- 编译器必须检查 `fit A: ...` 中左侧是否是合法 fit 目标（具体 `type`、`enum` 或 [feng-fit-builtin-type.md](./feng-fit-builtin-type.md) 允许的内建目标）。
- 编译器必须在左侧为泛型 `type` 目标时,检查其是否逐位置引用了目标泛型 `type` 已声明的全部类型参数,并拒绝任何特化、增删、重排或改写。
- 编译器必须在左侧为具名 `type` 目标时,先按“名称 + 泛型参数数量”解析目标 `type`；若不存在精确匹配的 `type` 声明,必须按“找不到目标 type 声明”报错。
- 编译器必须检查契约适配右侧是否全部为 object-form `spec`。
- 编译器必须在泛型 `fit` 右侧继续传递目标类型参数时,按与直接 `type` / `spec` 声明一致的规则检查父约束传递是否合法。
- 编译器必须在泛型 `fit` 的成员签名、成员体和 witness materialization 中保留目标泛型实例的类型实参；当 object-form `spec` coercion 的源表达式是具体泛型实例时,必须按该具体实例生成 witness,不得退化到只按原始 `type` 声明生成的 open witness。
- 编译器必须在带块体的契约适配中先纳入新增成员再判定满足性。
- 编译器必须按与直接定义在目标 `type` 中相同的规则检查 `fit` 块方法的重载与冲突。
- 编译器必须检查并拒绝多 `spec` 方法签名冲突（同名同参数顺序但返回值不一致）。
- 编译器必须检查并拒绝适配右侧重复列出同一 `spec`。
- 编译器必须拒绝 `fit` 块中出现 `let` 或 `var` 声明,无论是否带 `static`。
- 编译器必须在类型名静态方法解析时纳入当前文件可见的 `fit` 静态方法,包括通过 `import`/`use` 引入模块后可见的公开 fit 静态方法。
- 编译器必须让类型名静态方法值解析消费同一可见 fit 静态方法集合；选定后把稳定的
  owner、fit 与方法身份交给代码生成，不得按名称重新查找。
- 编译器为用户类型 `fit` 方法生成的跨包实现符号必须由声明来源模块、具体目标类型、
  `fit` 可见性以及同一来源模块内同目标同可见性的声明顺序稳定决定，不得依赖当前
  producer 或 consumer 分析过程中的全局 fit 注册序号。相同 `.ft` 声明在来源包与
  consumer 中必须恢复出同一个实现符号；具体泛型目标实例必须得到彼此不同且稳定的
  目标类型分量。
- 编译器必须检查并拒绝 `fit` 块内对目标 `type` 私有成员（`seal` 字段或 `seal` 方法）的访问，无论目标 `type` 定义在当前包内还是外部包中。
- 编译器必须在跨包 `fit` 的契约验证中排除目标 type 的 `seal` 实现成员，
  不得因 package-public `.ft` 中存在私有布局 skeleton 而建立 witness；同包
  fit 和当前 fit 自己提供的方法实现保持既有规则。
- 编译器必须以 fit 目标 type 作为 `spec seal` 访问域的实现主体；仅当该
  目标在访问点满足成员原声明 spec 且接收者使用 spec 视角时允许访问。
- 编译器必须在语义分析阶段对上述违规报错并阻止通过。
- 编译器必须识别孤儿适配（`A` 与 `B` 实现源码均不在当前包中）；普通 `fit` 形式的孤儿适配在当前包内正常有效，编译器无需额外处理；`open fit` 形式的孤儿适配，编译器必须移除其导出并输出肯定式提示消息（非错误、非警告），明确告知用户有风险的公开契约导出已被移除。无右侧 `spec` 的纯自扩展 `fit` 不受该规则影响。
- 编译器必须对同一 `(A, B)` 组合的多次 `fit` 声明执行幂等合并；合并后若产生方法签名冲突（同名同参数但返回值不一致），以与多 `spec` 冲突相同的规则报错。

## 7 运行时

- `fit` 主要是编译期语义机制,不要求引入额外用户可见语法对象。
- 通过 `fit` 建立的契约关系在运行时表现为具体类型可按目标 `spec` 视角访问与调用。
- 运行时分发与成员映射策略遵循 `spec` 规范中的实现侧约束,`fit` 本身不强制具体分发表实现。

## 8 关联

- [Feng 语言 `spec` 规范](./feng-spec.md): 契约边界、匹配规则、冲突判定与运行时约束。
- [Feng 语言类型规范](./feng-type.md): 具体类型定义与成员规则。
- [Feng 语言泛型规范](./feng-generics-draft.md): 泛型参数约束、子到父传递与泛型方法重载规则。
- [Feng 语言核心规范](./feng-language.md): 语言总规范与跨章节总约束。
