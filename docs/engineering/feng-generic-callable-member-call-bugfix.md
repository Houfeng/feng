# Feng 泛型类型 callable 成员调用修复

> 状态：成员调用分派修复已完成；全量回归被独立的泛型 callable ABI 问题阻塞
>
> 后续 ABI 修复见：
> [feng-reified-value-generic-callable-bugfix.md](./feng-reified-value-generic-callable-bugfix.md)

## 1 问题

泛型类型的方法体直接调用 callable-form spec 字段时：

```feng
spec Handler<T>(value: T): void;

type Dispatcher<T> {
  var handler: Handler<T>;

  func dispatch(value: T): void {
    self.handler(value);
  }
}
```

语义分析已将 `self.handler` 判定为 callable 值，但代码生成在看到 `self.member(...)`
后，把泛型 self 上的所有成员调用提前送入泛型实例方法路径。该路径只查找方法声明，
因此错误报告 `generic type 'Dispatcher' has no method 'handler'`。

## 2 修复规则

成员调用必须先服从语义分析得到的 callable 分类：

- 已解析为 type/fit 方法的成员调用继续进入对应方法路径；
- 未解析为方法、但成员表达式本身具有 callable-form spec 类型时，先按普通成员访问
  生成 callable 值，再复用统一 callable value 调用路径；
- 非 callable 成员仍保持现有编译错误，不增加按名称猜测或类型特判。

非泛型 receiver 已经复用普通 callable value 调用路径；本次修复使泛型 receiver
遵循同一规则。不改变 callable ABI、泛型共享主体 ABI 或运行时数据结构，且只更正
编译期分派顺序，不增加生成后 Feng 程序的运行时开销。

本修复使此前不可达的泛型 callable value 调用路径能够继续发码，同时暴露出
open/closed callable `invoke` C ABI 不一致的问题。后者是独立问题，不属于本文件
通过编译期分派顺序解决的范围。

## 3 验证

- 增加跨包 fcts 用例，覆盖泛型类型的嵌套泛型 `Action<Event<T>>` 字段直接调用；
- 确认闭合后的参数类型和 callable 调用结果正确；
- 执行 `make test` 全量回归。

## 4 实施清单

- [x] 修正 member-call 的 callable value 分派顺序
- [x] 增加 fcts 语言行为用例并注册执行
- [ ] 执行 `make test`
