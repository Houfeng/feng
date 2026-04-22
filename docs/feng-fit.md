`fit` 用于声明具体类型如何进入一个或多个 `spec` 契约,或如何给类型自身补充额外能力。`spec` 的职责、语法和默认语义见 [feng-spec.md](./feng-spec.md)。

# 语言设计规约：`fit`

## 1 设计目标

- 默认不开放。仅因两个类型结构相似，不自动建立可赋值、可传参或可返回的关系。
- 显式声明。类型能力的建立必须通过 `fit` 明确写出。
- 非侵入适配。允许在不修改原始 `type` 定义的前提下，使其满足一个或多个 `spec`。

## 2 `fit` 的职责

- `fit` 有两类合法用法: 契约适配与自扩展。
- 契约适配使用 `fit A: B` 或 `fit A: B, C, ...`; 其中左侧 `A` 必须是具体类型，右侧必须是一个或多个 `spec`。
- 自扩展使用 `fit A`，用于为类型 `A` 自身补充额外能力。
- `fit` 不用于“任意类型对任意类型”的开放式桥接。

示例:

```feng
fit User: Named;
fit User: Named, Auditable;

fit User {
  pu let nickname: string;
  var visits: int;

  fn say_hi(self) {
    print("Hello, " + self.name);
  }

  pr fn ping() {
    print("pong");
  }
}
```

`fit FooType: BarSpec;` 表示声明 `FooType` 满足 `BarSpec`; 编译器会检查 `FooType` 是否确实满足该契约,若不满足则报错。

`fit FooType: BarSpec, XyzSpec;` 表示声明 `FooType` 同时满足 `BarSpec` 与 `XyzSpec`; 编译器会检查是否全部满足,若有任一不满足则报错。

当具体类型尚未直接满足目标 `spec` 时,可在 `fit` 声明中显式补充成员,编译器会在这些成员生效后再检查是否满足目标契约:

```feng
fit FooType: BarSpec {
  fn display(self): string {
    return self.first + " " + self.last;
  }
}

fit FooType: BarSpec, XyzSpec {
}
```

上例中,编译器会先把 `fit` 块中新增的成员计入 `FooType` 的可见能力集合,再检查其是否满足 `BarSpec` 或 `BarSpec, XyzSpec` 中声明的全部契约要求; 若仍不满足,则报错。

补充规则:

- `fit` 只建立“类型 A 满足契约 B”的关系，不改变 `A` 原本的定义类别。
- `fit` 块中的成员定义方式与直接写在 `type` 中的成员一致,可声明 `let`、`var`、`fn`,也可显式声明可见性。
- 在 `fit A: B` 或 `fit A: B, C, ...` 的契约适配形式中,编译器会先把 `fit` 块中的成员附着到 `A`,再检查 `A` 是否满足右侧列出的全部 `spec`。
- 契约适配形式中，右侧列出的每一项都必须是 `spec`，并使用逗号分隔。
- 契约适配形式中，不带块体与带块体两种写法都表示“声明满足并触发编译期检查”; 带块体时，编译器在把新增成员纳入检查后再判定是否满足。
- `fit` 块中的 `fn` 成员其第一个参数必须是 `self`; 若该 `fn` 没有其他参数且函数体中也不使用 `self`,则可省略 `self` 形参声明。
- 在 `fit A` 的自扩展形式中，`fit` 块用于向 `A` 补充额外能力，不要求先存在目标 `spec`。

## 3 `fit` 与 `spec` 的配合关系

- `spec` 先定义契约边界，`fit` 再声明哪些具体类型进入该契约。
- 若没有在 `type` 声明头上直接写出满足关系，也没有可见的 `fit` 声明，即使某个 `type` 在结构上已经“看起来满足”某个 `spec`，编译器也不自动建立该关系。
- 只要当前作用域同时可见具体类型 `A`、目标 `spec` 以及对应的 `fit A: ...` 声明，`A` 就可以在这些 `spec` 要求的位置上使用。
- `fit A` 不建立新的契约关系，而是直接把新增能力附着在 `A` 上。

更多 `spec` 细节见 [feng-spec.md](./feng-spec.md)。

## 4 作用域与导出规则

- `fit` 的可见性规则与普通声明一致。
- 跨模块使用时，必须显式 `use` 包含该 `fit` 声明的模块。
- 若一个 `fit` 声明需要导出到包外，当前包必须拥有左侧具体类型 `A` 或右侧目标 `spec B` 之一。
- 不满足导出条件的 `fit` 声明只在当前包内生效，不应把关系扩散到包外。

## 5 结论与语义边界

- `fit` 用于声明具体类型与 `spec` 之间的适配关系。
- `fit` 还具有为类型自身补充扩展成员的能力。
- `spec` 的定义、默认初始化与相等性规则由 [feng-spec.md](./feng-spec.md) 单独说明。
- `fit A: B` 或 `fit A: B, C, ...` 的右侧只能是一个或多个 `spec`。
- 普通 `type` 不能出现在 `fit A: B` 或 `fit A: B, C, ...` 的右侧。
