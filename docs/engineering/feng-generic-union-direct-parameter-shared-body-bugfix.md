# Feng generic union 直接泛参成员共享体修复开发文档

> 状态：已实施并通过全量回归（2026-08-19）
>
> 本文记录 union-form generic spec 把 owner 泛参直接作为 union member，且该 spec 出现在共享泛型体中时的既有 Codegen 正确性缺陷。本文不改变 union-form 或泛型语义。

## 1. 问题

```feng
spec Choice<T>: int | T;

func choose<T>(value: T): Choice<T> {
  return value;
}
```

`Choice<int>`、`Choice<string>` 等闭合使用已有直接用例；但 `choose<T>` 会先形成开放实例 `Choice<T>`，当前 Codegen 在注册其成员时直接报告：

```text
CE0045: codegen: union-form spec member layout requires a concrete type argument
```

跨包 provider 导出 `choose<T>`、consumer 才提供闭合实参时会稳定触发，因此既有闭合实例测试不能覆盖该路径。

## 2. 实际架构现状

共享泛型 union 的主体机制已经存在：

- `cg_user_spec_generic_layout_fact()` 会把含直接泛参或其他动态布局 member 的 union 标记为 reified layout；
- 共享体通过 `reified_agg_deps` 取得最终闭合 union 的 aggregate descriptor，并按其 `size` 分配地址形式存储；
- `cg_emit_reified_union_header()` 已能从泛参 descriptor 设置 forwarding slot；
- `cg_emit_reified_union_path_store()` 已能把直接泛参值写入 union payload；
- 开放 union C struct 只作为 tag、forwarding metadata 与 payload 固定起始 offset 的 overlay，最终存储大小来自闭合 descriptor，而不是该 overlay 的 `sizeof`。

当前阻塞来自两处尚未对齐的旧限制：

1. generic union 成员注册仍显式拒绝 `CG_TYPE_GENERIC_PARAM`；
2. 开放 generic-context union 仍尝试生成只适用于闭合布局的静态 aggregate descriptor/default initializer。该 descriptor 不应成为共享体的运行时 authority；共享体必须使用已经静态装入 callable/type descriptor slot 的闭合 dependency。

## 3. 修复方案

1. generic union 成员注册保留直接泛参 member，使已有 reified-layout 判定和 path store 正常工作；
2. 对 `generic_context_type_param_count > 0` 的开放 union 实例，只发出共享体所需的 C overlay，不生成或使用伪闭合 aggregate descriptor/default initializer；
3. 闭合 union 实例继续生成现有静态 aggregate descriptor，调用点把它写入既有 `reified_agg_deps` slot；
4. 共享体只读取固定 slot 并转发，不增加 runtime 查找、缓存、分配机制或动态 descriptor 构造；
5. 同包与跨包、受约束与无约束 owner 泛参复用同一闭合链路，不按 package 或具体 spec 名称特判。

## 4. 测试要求

- 同包共享泛型函数返回/接收 `Choice<T>`，直接 member 为 `T`；
- 跨包 provider 只保留开放 `Choice<T>`，consumer 分别以 managed、scalar 与 aggregate 实参闭合；
- union 首 member 为固定类型和直接泛参各有用例，验证默认值由闭合 descriptor 决定；
- 生成 C 的共享体从既有 `reified_agg_deps` 取 descriptor，不引用开放 union 的伪静态 descriptor；
- 不影响既有闭合 generic union、嵌套 union、match/收窄与 generic union constraint 无 witness 路径。

## 5. TODO

- [x] **[分析]** 以跨包 `Choice<T>` 共享体复现 `CE0045`；
- [x] **[分析]** 确认 reified union layout/header/path-store 链路已经存在；
- [x] **[实际变更]** 允许开放 union member 保留 `CG_TYPE_GENERIC_PARAM`；
- [x] **[实际变更]** 开放 generic-context union 不生成伪闭合静态 aggregate descriptor；
- [x] **[测试变更]** 补同包/跨包及 managed/scalar/aggregate 闭合行为；
- [x] **[验证]** 确认 descriptor 仅经静态 slot 传播，无新增 runtime 机制；
- [x] **[全量回归]** 在非沙箱环境运行 `make test`。

## 6. 实施结果

- 开放 union overlay 保留直接泛参 member，闭合 descriptor 继续由既有 reified dependency
  slot 静态提供；没有新增 runtime 查找、缓存、分配或动态 descriptor 构造；
- generic owner constraint FCTS 的跨包 `Choice<T>` provider 共享体，以及既有 generic
  composition 用例共同覆盖 scalar、managed 与 aggregate 闭合表示；
- Codegen 专项测试通过，FCTS `814/814` 通过；非沙箱全量 `make test` 通过。
