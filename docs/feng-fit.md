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

- 契约适配: `fit A: B`、`fit A: B, C`、`fit A<T>: B<T>` 或 `fit A<T>: B<T>, C<T>` 形式,左侧为具体类型或对目标泛型 `type` 已声明类型参数的直接引用,右侧为一个或多个 object-form `spec`。
- 自扩展: `fit A { ... }` 或 `fit A<T> { ... }` 形式,为类型 `A` 增加成员,不建立新的 `spec` 契约关系。
- 可见契约关系: 当前作用域可见的“`type` 与 `spec` 已建立适配”的事实。
- 适配检查: 编译器对 `fit` 声明进行的满足性、冲突和合法性校验。
- `pu fit`: `fit` 声明的公开导出形式；导出后其他 `mod` 通过 `use` 引入当前模块时，该契约关系一同可见并生效。
- 孤儿适配: `fit A: B` 中 `A` 与 `B` 的实现源码均不在当前包中的满足声明；孤儿适配在当前包内正常建立契约关系并生效，但有效性不得超出当前包；以 `pu fit` 形式声明的孤儿适配，编译器移除其导出并输出肯定式提示（非错误、非警告）。纯自扩展 `fit A { ... }` / `pu fit A { ... }` 不属于孤儿适配判定范围。
- 目标类型参数引用: `fit Box<T, U>` 中的 `<T, U>` 不是新的类型参数定义,而是对目标 `type Box<T, U>` 已声明类型参数的逐位置引用。

## 3 语法

正确语法一,契约适配:

```feng
fit User: Named;
fit User: Named, Auditable;
```

正确语法二,公开导出契约适配:

```feng
pu fit User: Named;
pu fit User: Named, Auditable;
```

正确语法三,带块体的契约适配与自扩展:

```feng
fit User: Named {
  fn display(): string {
    return self.first + " " + self.last;
  }
}

fit User {
  fn say_hi() {
    print("Hello, " + self.name);
  }
}
```

正确语法四,公开导出带块体的契约适配:

```feng
pu fit User: Named {
  fn display(): string {
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
  fn get(): T;
}

fit Box<T>: Reader<T>;

fit Box<T>: Reader<T> {
  fn get(): T {
    return self.value;
  }
}

fit Box<T> {
  fn has_value(): bool {
    return true;
  }
}
```

正确语法六,泛型参数的子到父传递与直接声明一致:

```feng
spec Animal {}
spec Dog: Animal {}

type Cage<T: Dog> {
  let value: T;
}

spec View<T: Animal> {
  fn get(): T;
}

fit Cage<T>: View<T> {
  fn get(): T {
    return self.value;
  }
}
```

正确语法七,同名但泛型参数数量不同的 `type` 在 `fit` 中按不同目标类型处理:

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
  fn ping(self) {}
}
```

错语法三,`fit` 块中声明存储字段:

```feng
fit User {
  let nickname: string;  // 错误: fit 块中不得声明 let/var 字段
  var visits: int;       // 错误: fit 块中不得声明 let/var 字段
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
  fn get(): T;
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
- 在泛型契约适配右侧继续传递目标类型参数时,其子到父约束传递规则与直接写在 `type` / `spec` 声明头上的泛型父约束规则完全一致; 若当前目标类型参数已带有更强约束,则按同一套“在当前可见契约关系下是否满足父约束”规则判定合法性。
- 若没有在 `type` 声明头或可见 `fit` 中显式建立关系,即使结构看似满足 `spec`,编译器也不自动建立契约关系。
- `fit` 建立的契约关系以作用域可见性为生效条件；默认仅在声明所在 `mod` 内有效，不自动对外暴露。
- 使用 `pu fit` 可将契约关系公开导出；其他 `mod` 通过 `use` 引入当前模块后，该契约关系在其作用域内生效。
- 孤儿适配（`A` 与 `B` 实现源码均不在当前包中）在当前包内正常建立契约关系并生效；其有效性不超出当前包，`pu fit` 形式的孤儿适配由编译器移除导出并输出肯定式提示（非错误、非警告）。无右侧 `spec` 的纯自扩展 `fit` 不受孤儿规则影响。
- 声明层面的可见性修饰（`pu` 导出或无修饰默认模块内可见）与是否带块体是两个独立维度，四种组合均合法，语义互不影响；块体内 `fn` 成员的可见性（默认公开或 `pr` 收窄）是独立于声明层面的第三个维度。
- `fit` 块内的函数不能访问目标 `type` 的私有成员（`pr` 修饰的字段或方法），无论目标 `type` 定义在当前包内还是外部包中；`fit` 块内只能使用目标类型对外公开的能力。
- `fit` 块中新增方法的重载与冲突判定规则,必须与直接定义在目标 `type` 中的成员方法保持一致; 若是泛型方法,则同样按既有“名称 + 泛型参数数量 + 参数个数 + 参数类型”规则判定,约束不参与重载区分。
- 同一 `(A, B)` 组合的多次 `fit A: B` 声明视为幂等，编译器自动合并；若合并后产生方法签名冲突，以与多 `spec` 冲突相同的规则判定并报错。
- `type A: B` 声明头建立的满足关系与 `fit A: B` 是两个独立的声明，各自通过自身的 `pu` 决定可见范围，互不影响；其他 `mod` 通过 `use` 引入相应模块后各自升效。

## 5 规则

分为「必须、禁止、建议」。

- [必须] 契约适配形式中,`fit A: B, C, ...` 的左侧必须是合法 fit 目标（具体 `type` 或 [feng-fit-builtin-type-draft.md](./feng-fit-builtin-type-draft.md) 允许的内建目标）,右侧每一项都必须是 object-form `spec`。
- [必须] 若目标 `type` 为泛型 `type`,则允许使用 `fit A<T>: B<T>`、`fit A<T>: B<T> {}` 与 `fit A<T> {}` 这类泛型 `fit` 形式。
- [必须] 泛型 `fit` 左侧的类型参数列表必须逐位置引用目标泛型 `type` 已声明的类型参数。
- [必须] 当 `fit` 左侧是具名 `type` 目标时,必须按“名称 + 泛型参数数量”解析到一个已存在的 `type` 声明；同名但泛型参数数量不同的 `type` 不得互相代替。
- [必须] 契约适配形式中,不带块体与带块体都表示“声明满足并触发编译期检查”。
- [必须] `fit` 块中只能声明 `fn` 成员；`let`/`var` 字段只能在 `type` 定义中声明，`fit` 块中不得声明任何存储字段。字段必须属于对象本体，`fit` 只负责方法实现与契约建立。
- [必须] `fit` 块中 `fn` 成员的可见性规则与 `type` 中成员方法一致：默认公开，需要收窄时显式使用 `pr`。
- [必须] `fit` 块中的 `fn` 成员与 `type` 成员方法相同,`self` 由编译器隐式提供,无需在参数列表中显式声明; `fn` 体内可直接使用 `self` 引用当前实例。
- [必须] `fit` 块内的函数不得访问目标 `type` 的私有成员（`pr` 字段或 `pr` 方法），无论目标 `type` 定义在当前包内还是外部包中。
- [必须] 泛型 `fit` 右侧若继续传递目标类型参数,其合法性检查必须与直接写在 `type` / `spec` 声明头中的父泛型 `spec` 传递规则一致。
- [必须] `fit` 块中新增方法的重载与冲突判定规则必须与直接定义在目标 `type` 中的成员方法一致。
- [必须] 当同一个 `type` 通过 `fit` 同时满足多个 `spec` 时,若出现“方法名相同,且参数个数、参数类型和参数顺序一致,但返回值类型不一致”的方法签名,必须视为冲突。
- [必须] `fit` 建立的契约关系必须遵守 `spec` 规范中的字段/方法精确匹配规则。
- [必须] `fit` 建立的契约关系默认仅在声明所在 `mod` 内可见；如需跨模块共享，须使用 `pu fit` 导出，其他 `mod` 通过 `use` 引入后方可激活该契约关系。
- [必须] 孤儿适配导出限制仅适用于带 `spec` 右侧的契约适配；无右侧 `spec` 的纯自扩展 `fit` 不受孤儿规则影响。
- [必须] 同一 `(A, B)` 组合的多次 `fit A: B` 声明幂等，编译器自动合并；合并后若产生方法签名冲突（同名同参数但返回值不一致），以与多 `spec` 冲突相同的规则判定并报错。
- [禁止] 契约适配右侧出现普通 `type`。
- [禁止] 契约适配右侧出现 callable-form `spec` 或 union-form `spec`。
- [禁止] 在 `fit` 左侧把目标泛型 `type` 特化为具体类型,或通过增删、重排、改写目标类型参数来改变目标 `type` 的声明形状。
- [禁止] 在仅存在 `type UserType<T, U>` 这类声明时，把 `fit UserType<T>` 当作同一个目标 `type` 使用。
- [禁止] 在同一适配声明中重复列出同一个 `spec`。
- [禁止] `fit` 块中声明 `let` 或 `var` 字段。
- [说明] 声明层面的可见性修饰（`pu` 或无修饰）与是否带块体是两个独立维度，任意组合均合法，语义互不影响。
- [说明] 孤儿适配规则：`fit A: B` 或 `pu fit A: B` 中 `A` 与 `B` 实现源码均不在当前包中时，该声明在当前包内正常有效；`pu fit` 形式的孤儿适配不得导出，编译器移除其导出并输出肯定式提示（非错误、非警告），明确告知有风险的公开契约导出已被移除。无右侧 `spec` 的纯自扩展 `fit` 不受该规则影响。
- [说明] `type A: B` 声明头建立的满足关系与 `fit A: B` 是两个独立声明，各自通过自身的 `pu` 决定是否公开导出，互不影响；其他 `mod` 通过 `use` 引入相应模块后各自升效。
- [建议] 优先在 `type` 声明头表达定义者主动承诺; `fit` 优先用于第三方类型或无法修改源类型定义的场景。

## 6 编译期

- 编译器必须检查 `fit A: ...` 中左侧是否是合法 fit 目标（具体 `type` 或 [feng-fit-builtin-type-draft.md](./feng-fit-builtin-type-draft.md) 允许的内建目标）。
- 编译器必须在左侧为泛型 `type` 目标时,检查其是否逐位置引用了目标泛型 `type` 已声明的全部类型参数,并拒绝任何特化、增删、重排或改写。
- 编译器必须在左侧为具名 `type` 目标时,先按“名称 + 泛型参数数量”解析目标 `type`；若不存在精确匹配的 `type` 声明,必须按“找不到目标 type 声明”报错。
- 编译器必须检查契约适配右侧是否全部为 object-form `spec`。
- 编译器必须在泛型 `fit` 右侧继续传递目标类型参数时,按与直接 `type` / `spec` 声明一致的规则检查父约束传递是否合法。
- 编译器必须在泛型 `fit` 的成员签名、成员体和 witness materialization 中保留目标泛型实例的类型实参；当 object-form `spec` coercion 的源表达式是具体泛型实例时,必须按该具体实例生成 witness,不得退化到只按原始 `type` 声明生成的 open witness。
- 编译器必须在带块体的契约适配中先纳入新增成员再判定满足性。
- 编译器必须按与直接定义在目标 `type` 中相同的规则检查 `fit` 块方法的重载与冲突。
- 编译器必须检查并拒绝多 `spec` 方法签名冲突（同名同参数顺序但返回值不一致）。
- 编译器必须检查并拒绝适配右侧重复列出同一 `spec`。
- 编译器必须拒绝 `fit` 块中出现 `let` 或 `var` 字段声明。
- 编译器必须检查并拒绝 `fit` 块内对目标 `type` 私有成员（`pr` 字段或 `pr` 方法）的访问，无论目标 `type` 定义在当前包内还是外部包中。
- 编译器必须在语义分析阶段对上述违规报错并阻止通过。
- 编译器必须识别孤儿适配（`A` 与 `B` 实现源码均不在当前包中）；普通 `fit` 形式的孤儿适配在当前包内正常有效，编译器无需额外处理；`pu fit` 形式的孤儿适配，编译器必须移除其导出并输出肯定式提示消息（非错误、非警告），明确告知用户有风险的公开契约导出已被移除。无右侧 `spec` 的纯自扩展 `fit` 不受该规则影响。
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
