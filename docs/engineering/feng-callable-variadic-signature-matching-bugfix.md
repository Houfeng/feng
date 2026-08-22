# Feng callable 变长参数签名匹配修复方案

> **状态**：已完成并通过全量回归。
>
> **性质**：独立编译器 bugfix 工程记录，不是语言权威规范。正式行为由
> [`feng-function-variadic.md`](../specifications/feng-function-variadic.md) 与
> [`feng-spec.md`](../specifications/feng-spec.md) 定义。

## 1. 问题与结论

Feng 将参数声明 `T...` 的参数类型规范化为 `T[]`，并以 `is_variadic` 保留其调用形态。
因此两者底层参数类型相同，但 callable 完整签名不同：

```feng
spec Variadic(args: int...): int;

type Source {
  func fixed(args: int[]): int { args; return 0; }
}

func invalid(value: Source): Variadic {
  return value.fixed; // 必须在 Semantic 以 AE0522 拒绝
}
```

已绑定 callable-form `spec` 之间也不得跨越该结构差异显式转换：

```feng
spec Fixed(args: int[]): int;
spec Variadic(args: int...): int;

func invalid(value: Fixed): Variadic {
  return (Variadic)value; // 必须在 Semantic 以 AE0051 拒绝
}
```

当前 Semantic 的多条 callable 签名比较路径只比较参数数量、规范化后的参数类型和返回
类型，遗漏 `is_variadic`；第一例被错误接受，第二例通过 Semantic 后在 Codegen 报
`CE0191`。Codegen 已将变长形态视为签名的一部分，问题位于 Semantic 的通用比较面。

## 2. 修复范围与强制边界

修复必须统一覆盖：

1. 未绑定顶层函数、Lambda、具体 type/fit 实例与静态方法；
2. object-form/intersection-form `spec` 值的实例方法；
3. 受 object-form/intersection-form `spec` 约束的泛型值实例方法和类型参数静态方法；
4. 已显式闭合的泛型函数和方法；
5. 已绑定 callable-form `spec` 值之间的显式转换。

本次不得修改 runtime、生成程序 ABI、公开 ABI、`.ft` schema 或版本，不得修改或删除
任何既有测试用例，不得增加合法程序的运行时分配、查找、分支、wrapper 或调用层级。
若正确修复必须突破任一边界，立即记录问题并停下交由人工决策。

## 3. 通用实现方案

Semantic 在现有各签名比较循环中同时比较参数的 callable 形态与代入后的类型：

- 参数数量必须相同；
- 每个位置的 `is_variadic` 必须相同；
- 每个位置完成 owner、fit、spec 和 callable-local 泛参代入后的类型必须相同；
- 返回类型必须相同。

参数形态比较通过一个无分配的共享 helper 表达，并嵌入现有参数循环，不新增重复遍历。
Codegen 保持现有检查作为防御性边界，不新增变长参数特判或运行时适配。

## 4. 实施与验证清单

- [x] 收敛 `feng-function-variadic.md` 与 `feng-spec.md` 的权威表述。
- [x] 增加共享参数形态比较并接入普通、owner/fit、显式泛型、Lambda 和 callable-form
      spec 显式转换路径。
- [x] `test/` 新增负向用例，覆盖 `T...`/`T[]` 双向不匹配、未绑定方法值和已绑定
      callable 显式转换，并验证 Semantic 诊断。
- [x] `test/` 新增跨 object/intersection、泛型约束、实例/静态与显式泛型来源的负向覆盖。
- [x] `fcts/` 新增同变长形态的正向形成、绑定、显式转换和真实调用覆盖。
- [x] 运行 Semantic、Codegen、FCTS 定向验证。
- [x] 沙箱外运行完整 `make test`。
- [x] 更新实施问题记录、验证结果与最终状态。

## 5. 实施过程问题记录

实施中发现任何偏离既有规范、本文方案或预期测试结果的问题，必须先在本节记录最小
复现、实际结果、期望结果和影响，再分析与修复。不确定或触及第 2 节强制边界时停止，
由人工决策。

本次实施未发现新增问题。

## 6. 交付记录

### 6.1 实现

- Semantic 新增无分配的共享参数形态比较，在既有签名参数循环内同步检查
  `is_variadic`，没有新增遍历。
- 检查统一接入 callable-form `spec` 显式转换、普通与待决 callable 来源、owner/fit
  方法、显式泛型方法以及 Lambda 相关签名路径。
- Codegen 已有的变长形态一致性检查继续作为防御性边界；本次没有增加 Codegen 特判。
- 两个最小复现均在 Semantic 阶段拒绝：未绑定来源不匹配报告 `AE0522`，显式转换
  不匹配报告 `AE0051`，不再延迟到 Codegen。

### 6.2 用例

- `test/semantic/test_semantic.c` 新增 16 个负向场景，覆盖 `T...`/`T[]` 双向不匹配、
  顶层函数、Lambda、具体 type/fit、object/intersection、泛型约束、实例/静态、显式泛型
  方法、未绑定显式转换及已绑定 callable 双向显式转换。
- `fcts/fcts_bin/src/test_variadic_method_value_shape.ff` 新增 6 个正向行为用例，覆盖同形态
  方法值的目标贴合、绑定后同形态显式转换与真实调用，并覆盖上述各类方法来源。
- 未修改或删除任何既有测试场景；仅新增测试及其入口注册。

### 6.3 验证

- Semantic、Codegen 与 Symbol 定向测试通过。
- FCTS 定向验证通过：`863/863`。
- 沙箱外完整 `make test` 退出码为 `0`；UBSan 与普通 O2/Werror 两个阶段均通过，
  每个阶段均包含 smoke `91/91`、std `601/601`、FCTS `863/863` 和性能约束检查，
  其余编译器、runtime、CLI、增量构建及发布脚本回归也全部通过。

### 6.4 边界审计

- 没有 runtime、公开 ABI、生成程序 ABI 或 `.ft` schema/version 变更。
- 合法程序没有新增运行时分配、查找、分支、wrapper、调用层级或其他增量运行开销；
  新检查仅发生在编译期既有参数循环中。
- 没有遗留未修复问题。
