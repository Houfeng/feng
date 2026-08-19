# Feng 泛型 spec 约束成员 surface 闭合修复开发文档

> 状态：已实施并通过全量回归（2026-08-19）
>
> 本文记录泛型参数通过 object-form spec 约束访问成员时，成员声明者与类型实参没有完整闭合的既有 Semantic 正确性缺陷。本文不改变 spec、泛型、继承或成员访问语义。

## 1. 问题

以下程序是合法的：

```feng
spec Base<T> {
  var value: T;
}

spec Child<T>: Base<T> {}

func accumulate<U: Child<int>>(subject: U): int {
  subject.value += 1;
  return subject.value;
}
```

`U` 的约束已把 `Child<T>` 闭合为 `Child<int>`，父 spec 映射又把它闭合为 `Base<int>`，因此 `subject.value` 的类型必须是 `int`。当前 Semantic 却可能把字段类型保留为父 spec 声明中的开放 `T`，随后对复合赋值报告：

```text
AE0023: compound assignment operator '+=' requires operands of same numeric type,
        got '<type>' and 'i64'
AE0030: binary '+' got '<type>' and 'i64'
```

普通读取、普通写入、参数匹配或返回值检查也可能因上下文推断而暂时没有报错；这不代表成员类型已经正确闭合。

## 2. 已确认根因

Semantic 已有两段正确但尚未统一复用的能力：

1. `instantiate_parent_spec_ref_for_instance()` 能沿泛型父 spec 链把 `Child<int>` 投影为成员实际声明者 `Base<int>`；
2. `substitute_spec_member_type_ref_for_instance()` 能用声明者实例把成员签名中的 owner 泛参替换为具体实参。

实例字段类型推导的普通路径先对接收者类型调用 `resolve_type_ref_decl()`。当接收者是函数泛参 `U` 时，`U` 不是一个具体声明，所以该解析返回空；后续没有像泛型 spec 方法调用路径一样取得 `U` 的完整约束引用，也没有执行上述父 surface 投影和成员类型替换。

成员访问合法性检查目前会对已知泛型参数直接放行，交由后续 spec witness 处理，因此它不会补足缺失的字段类型事实。

## 3. 修复原则

修复应建立并复用一个统一的泛型参数约束成员解析结果，至少包含：

- 泛型参数声明的完整约束 type ref，例如 `Child<int>`；
- 实际找到成员的声明 spec，例如 `Base<T>`；
- 沿父 spec 链投影后的声明者实例，例如 `Base<int>`；
- 以该实例闭合后的字段或方法签名。

该解析必须：

1. 以声明身份和父 spec 参数映射为依据，不能按泛参名称相同进行替换；
2. 同时支持直接成员、继承成员、重排/固定父参数以及多层父链；
3. 对同包与从 `.ft` 恢复的 spec 使用同一链路；
4. 供字段类型推导和现有方法解析复用，不能只在 `+=`、某一种字段或某一个测试处加特判；
5. 只补 Semantic 编译期类型事实，不改变 witness、descriptor、`.ft` 格式、runtime ABI 或运行时操作。

## 4. 测试要求

### 4.1 Compiler tests

- `U: Child<int>` 访问从 `Base<T>` 继承的实例 `let` / `var`，覆盖读取、普通写入和复合写入；
- 静态字段通过同一父 surface 得到闭合类型；
- 父 spec 对 owner 参数进行重排或固定后，字段和方法参数/返回类型均按声明映射闭合；
- 直接成员与多层继承成员使用同一解析规则；
- 同包和 `.ft` 恢复路径结果一致。

### 4.2 FCTS

- generic type 与 generic fit 分别满足含继承字段的 generic spec；
- constrained generic 函数通过实例/静态 witness 读取、普通写入和复合写入；
- 跨包 provider 共享泛型函数操作 consumer 实现，验证恢复后的 parent surface；
- managed 与数值类型各覆盖一个非等价 ABI/行为代表。

## 5. TODO

- [x] **[分析]** 用合法 `Child<int> -> Base<int>` 继承字段访问复现开放 `<type>`；
- [x] **[分析]** 确认父 surface 实例化和成员 owner 参数替换能力已经存在；
- [x] **[实际变更]** 提取并复用泛型参数的完整约束实例解析；
- [x] **[实际变更]** 让实例成员类型推导按成员实际声明者 surface 闭合；
- [x] **[验证]** 核对现有泛型 spec 方法解析并改为复用同一结果，避免字段与方法规则分叉；
- [x] **[测试变更]** 按第 4 节补齐 Compiler tests 与 FCTS；
- [x] **[专项回归]** 运行 Semantic、Codegen 与 FCTS；
- [x] **[全量回归]** 在非沙箱环境运行 `make test`。

## 6. 实施结果

- Semantic 以统一 `SpecMemberSurface` 保存成员、声明 spec 与投影后的声明者实例，字段与
  方法共同复用完整约束及父 surface 映射；
- Codegen/FCTS 直接覆盖 `U: Child<int>` 的继承字段、普通/复合写入、实例/静态 witness、
  同包与跨包恢复；
- Semantic、Codegen 专项测试通过，FCTS `814/814` 通过；非沙箱全量 `make test` 通过。
