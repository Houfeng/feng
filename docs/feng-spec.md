# Feng 语言 `spec` 规范

本文档说明 Feng 中 `spec` 的职责、语法与默认语义。`spec` 用于声明契约形状; 具体类型如何显式满足这些契约,见 [feng-fit.md](./feng-fit.md)。

## 1 `spec` 的职责

- `spec` 用于声明契约形状,不提供实现体。
- `spec` 可以描述对象形状,也可以描述可调用形状。
- `spec` 可作为参数类型、返回类型、成员类型和其他类型位置中的引用目标。
- `type` 负责具体定义,`spec` 负责契约目标。

## 2 语法形式

对象形状示例:

```feng
spec Named {
  let name: string;
  fn display(): string; // spec 中的 fn 可无函数体
}

spec FooSpec: Named {
  fn title(): string;
}
```

可调用形状示例:

```feng
spec Click(): void;
spec Mapper(x: int): int;
```

规则说明:

- `spec` 只声明成员与签名,不声明构造函数实现、方法实现或对象布局细节。
- `spec` 中声明的成员与行为签名默认公开。
- `spec` 不允许为成员或行为签名显式添加 `pu` 或 `pr` 可见性修饰。
- 对象形状中的行为签名使用 `fn` 关键字; 在 `spec` 中,这类 `fn` 可以不写函数体。
- 对象形状中的字段使用普通成员声明语法。
- 可调用形状使用 `spec Name(args): ReturnType;` 形式定义。

## 3 契约引用与直接声明满足

`spec` 支持在声明头上直接引用一个或多个 `spec`,表示当前 `spec` 需要满足这些契约的要求。

示例:

```feng
spec BarSpec {
  fn id(): int;
}

spec XyzSpec {
  fn code(): string;
}

spec FooSpec: BarSpec, XyzSpec {
  fn name(): string;
}
```

上例中,`FooSpec` 同时满足 `BarSpec` 与 `XyzSpec` 的全部契约要求,因此任何满足 `FooSpec` 的类型也必须同时满足这两个 `spec`。

具体 `type` 也可以在声明头上直接写出其满足的一个或多个 `spec`:

```feng
spec AbcSpec {
  fn enabled(): bool;
}

type FooType: FooSpec, AbcSpec {
  fn name(): string {
    return "foo";
  }

  fn id(): int {
    return 1;
  }

  fn code(): string {
    return "x";
  }

  fn enabled(): bool {
    return true;
  }
}
```

规则说明:

- `spec FooSpec: BarSpec, XyzSpec {}` 中,冒号右侧必须是一个或多个 `spec`,并使用逗号分隔。
- `type FooType: FooSpec, AbcSpec {}` 表示 `FooType` 在定义处直接声明同时满足 `FooSpec` 与 `AbcSpec`。
- 若某个列出的 `spec` 本身还要求满足其他 `spec`,则该 `type` 也必须同时满足这些额外 `spec` 的全部要求。
- 同一声明头中的 `spec` 列表不应重复列出同一个 `spec`。
- 对同一个 `type`,声明头上的直接满足关系与后续可见的 `fit A: SpecB` 或 `fit A: SpecB, SpecC` 一样,都属于显式建立的契约关系。
- 直接写在 `type` 声明头上的满足关系优先表达“定义者主动承诺”; `fit` 仍保留给无法修改原始 `type` 定义时的非侵入适配。

## 4 默认初始化与默认 witness

- `spec` 支持默认零值初始化。
- 无初始值的 `spec` 绑定会自动得到该 `spec` 的默认 witness。
- 默认 witness 属于默认值语义的一部分,不属于用户编写的实体函数体。
- 每个 `spec` 都有对应的默认 witness,且默认 witness 必须满足该 `spec` 声明的全部成员与签名要求。
- 字段成员按各自类型的默认零值初始化。
- 返回 `void` 的行为签名提供空实现; 该实现可以直接结束执行,但不产生任何返回值。
- 返回非 `void` 的行为签名返回其返回类型的默认零值。
- 若某个行为签名返回另一个 `spec` 类型,则返回该 `spec` 类型对应的默认 witness。

补充规则:

- “非 `void` 的实体函数必须显式写出 `return`”只约束带函数体的普通 `fn`、成员方法和 Lambda,不约束 `spec` 的默认 witness。
- `spec` 的默认 witness 由语言规则直接定义,而不是由用户源码提供函数体。

## 5 实例与相等性

- 每次对 `spec` 类型执行默认初始化时,都会创建该 `spec` 默认 witness 的一个新实例,而不是复用共享单例。
- `spec` 值上的 `==` / `!=` 默认比较引用身份,不执行深度比较。
- 因此两个分别默认初始化得到的 `spec` 值,即使形状完全一致,默认也视为不同实例。

示例:

```feng
spec S {}

let s1: S;
let s2: S;
```

上例中,`s1` 与 `s2` 默认不是同一个实例,因此 `s1 == s2` 为 `false`。

## 6 与 `fit` 的关系

- `spec` 只定义契约边界,不自动把任意具体类型纳入该契约。
- 某个 `type` 若要在要求某个 `spec` 的位置上使用,必须显式建立契约关系; 该关系既可以写在 `type` 声明头上,也可以通过可见的 `fit A: SpecB` 或 `fit A: SpecB, SpecC` 声明提供。
- `fit A` 只用于给 `A` 自身补充额外能力,不建立新的 `spec` 契约关系。

更多适配与导出规则见 [feng-fit.md](./feng-fit.md)。