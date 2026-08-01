# 契约与 fit

`spec` 声明能力边界，`type` 提供实现，`fit` 在不修改原类型定义的情况下建立满足关系或补充方法。详细规则见[`spec` 规范](../../specifications/feng-spec.md)和[`fit` 规范](../../specifications/feng-fit.md)。

## 对象契约

```feng
spec Named {
  let name: string;
  func display(): string;
}

type User: Named {
  let name: string;

  func display(): string {
    return self.name;
  }
}

func print_name(value: Named) {
  println(value.display());
}
```

字段必须在名称、类型和 `let`/`var` 方式上匹配；方法必须在名称、参数与返回类型上匹配。声明头中的关系表示类型定义者主动承诺满足契约。

## 可调用契约

```feng
spec Mapper(value: int): int;

let double: Mapper = (value: int) -> value * 2;
println("{0}", double(21));
```

可调用形式只描述函数签名，适合 Lambda、顶层函数和方法值。它不能出现在 `type Foo: Mapper` 或 `fit Foo: Mapper` 中。

## 联合契约

```feng
spec Identifier: int | string;

let id: Identifier = "user-42";
let label = match id {
  number: int { "numeric" }
  text: string { text }
};
```

联合形式表达“成员之一”，必须先通过 `match` 收窄才能按具体成员使用。

## 交叉契约

```feng
spec Readable {
  func read(): string;
}

spec Writable {
  func write(value: string): void;
}

spec ReadWrite: Readable & Writable;
```

交叉形式组合多个对象契约，可用于类型位置或泛型约束。具体类型应分别声明满足组成它的对象契约，而不是直接声明满足交叉契约。

## 通过 fit 满足契约

```feng
spec DisplayName {
  func display_name(): string;
}

type Account {
  let name: string;
}

fit Account: DisplayName {
  func display_name(): string {
    return self.name;
  }
}
```

`fit` 适合不能修改原类型，或希望把适配关系放在独立模块中的场景。

## 使用 fit 扩展方法

不列出目标 `spec` 时，`fit` 可以只补充方法：

```feng
fit Account {
  func greeting(): string {
    return "Hello, " + self.name;
  }
}
```

扩展是否可跨模块使用由模块和 `fit` 的可见性共同决定。需要导出时，在公开模块中使用 `open fit`；导入该模块后，关系和扩展方法才在当前文件中生效。

## 使用建议

- 类型自身天然承担的契约写在 `type` 声明头。
- 第三方适配或按模块启用的能力使用 `fit`。
- 不要把 `spec` 当作实现继承；它只描述可见契约。
- 不要依赖隐式结构匹配；满足关系必须显式声明。
