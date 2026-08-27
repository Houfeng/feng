# Feng 泛型类型推导字段 Codegen 修复方案

> **状态**：已完成并通过全量回归。
>
> **性质**：独立编译器 bugfix 工程记录，不是语言权威规范。字段绑定的正式行为由
> [`feng-binding.md`](../specifications/feng-binding.md) 与
> [`feng-type.md`](../specifications/feng-type.md) 定义。

## 1. 问题与结论

泛型 `type` 的实例字段带初始化器并省略显式类型时，Semantic 已按主规范从初始化器
推导字段静态类型，但 Codegen 生成开放泛型 owner 的共享实例方法时，owner view 初始化、
字段读取和字段写入三条路径都只读取 `FENG_SEMANTIC_TYPE_FACT_TYPE_REF` 形式的字段类型
事实。初始化器推导为
`FENG_SEMANTIC_TYPE_FACT_BUILTIN` 或 `FENG_SEMANTIC_TYPE_FACT_DECL` 时，字段被错误降级为
`CG_TYPE_VOID`，随后在默认值生成阶段报告 `CE0227`。

最小复现包括：

```feng
enum State { Pending = 0 }

type Box<T> {
  seal var state = State.Pending;
  func read(): State { return self.state; }
}
```

问题不是枚举专属，同一路径也影响整数字面量、布尔字面量、字符串字面量和非泛型对象
构造等推导字段。非泛型 owner 以及推导为 `TYPE_REF` 的 `Inner<T>()` 字段走完整字段类型
解析路径，不受该遗漏影响。

## 2. 修复范围与边界

本次修复必须让开放泛型 owner view 与普通 user type 成员注册使用同一字段类型解析规则，
完整处理：

1. 显式字段类型；
2. `BUILTIN` 推导事实；
3. 枚举和非泛型对象的 `DECL` 推导事实；
4. 泛型对象等 `TYPE_REF` 推导事实；
5. 缺少可用类型事实时的既有诊断。

本次不修改 Parser、Semantic、语言规范、runtime、公开 ABI、生成程序 ABI、`.ft` schema
或版本，也不增加合法程序的运行时分配、查找、分支、wrapper 或调用层级。若实现需要突破
上述边界，必须停止并交由人工决策。

## 3. 通用实现方案

删除 `cg_init_generic_owner_view`、泛型类型共享方法体内的实例字段读取和实例字段写入中
重复且不完整的字段类型事实分派，统一改为复用现有 `cg_resolve_user_field_type`。该入口
已经负责：

- 显式类型在 owner 泛型上下文中的代入与解析；
- `BUILTIN`、`DECL`、`TYPE_REF` 三类 Semantic 类型事实的解析；
- 推导事实缺失时的字面量兼容回退与既有错误报告。

统一入口必须区分持久化具体/普通 owner 与临时开放泛型 owner：前者按 owner 实例执行类型
参数代入；后者的显式类型和 `TYPE_REF` 事实必须沿用当前共享体已激活的泛型上下文解析，
避免二次 owner 代入产生不同的具化依赖键。`BUILTIN` 和 `DECL` 事实继续通过统一事实解析
分派处理。该差异由 owner view 的既有 `is_ephemeral_generic_owner_view` 语义表达，不按字段
类型增加特判。

此修复只收敛编译期 Codegen 元数据解析，不改变任何运行时代码路径或值表示。

## 4. 验证矩阵

Codegen 单元测试必须在同一个开放泛型 owner 中覆盖并核对 Semantic 类型事实类别：

| 字段初始化器 | 预期类型事实 | 预期 Codegen 类型 |
|---|---|---|
| `1` | `BUILTIN` | `int` |
| `true` | `BUILTIN` | `bool` |
| `"ready"` | `BUILTIN` | `string` |
| `State.Pending` | `DECL(enum)` | `State` |
| `Payload()` | `DECL(type)` | `Payload` |
| `GenericPayload<T>()` | `TYPE_REF` | `GenericPayload<T>` |
| 显式标注字段 | 不依赖推导事实 | 声明类型 |

生成的 C 必须通过宿主 C 编译器检查。FCTS 必须构造具体泛型实例并通过公开方法验证上述
推导字段的初始化值、读取和更新行为，确保修复不仅消除 `CE0227`，也保持字段运行时语义。

## 5. 实施与验证清单

- [x] 复用统一字段类型解析器修复开放泛型 owner view。
- [x] 增加 Codegen 类型事实与生成 C 编译回归覆盖。
- [x] 增加 FCTS 具体泛型实例运行行为覆盖。
- [x] 运行 Codegen 与 FCTS 定向验证。
- [x] 沙箱外运行完整 `make test`。
- [x] 更新交付记录与最终状态。

## 6. 实施过程问题记录

实施中发现任何偏离既有规范、本文方案或预期测试结果的问题，必须先在本节记录最小
复现、实际结果、期望结果和影响，再分析与修复。不确定或触及第 2 节边界时停止，由
人工决策。

定向 Codegen 首次验证在 owner view 初始化修复后不再报告 `CE0227`，但生成 C 的共享
泛型字段读取仍出现 `*(void *)`，宿主 C 编译器拒绝。检查确认字段读取与字段写入各自还有
一份仅处理 `TYPE_REF` 的重复分派；它们与原根因相同，属于第 2 节既定修复范围，不涉及
新的语言、runtime 或 ABI 决策。修复方案因此按第 3 节统一收敛这三条路径。

三条路径首次统一调用普通 owner 解析模式后，既有跨包 `List<T>` FCTS 报告 `CE0007`。
原因是临时开放泛型 owner 已处于共享体的有效具化依赖上下文，再按持久化 owner 实例进行
一次代入会形成不同的依赖键。该问题不需要恢复重复分派；统一入口按上述 owner view 语义
选择当前共享体上下文即可同时保持既有 `TYPE_REF` 行为并补齐 `BUILTIN`/`DECL`。

本次 `CE0227` 修复后，原始 `examples/hello_world` 已继续通过字段类型解析，但在
`throw self._error` 处生成的 C 报告把 `Option<E>` 的具化字段存储地址初始化为结构体值。
该问题涉及具化聚合表达式在 `throw` 路径上的地址/值物化，不属于字段静态类型事实解析；
本次不扩展修复，需另行分析和决策。

## 7. 交付记录

Codegen 已通过统一字段类型解析入口处理开放泛型 owner view 的初始化、实例字段读取和
实例字段写入，完整支持显式类型以及 `BUILTIN`、`DECL`、`TYPE_REF` 三类推导事实。临时
owner view 继续沿用共享方法体的有效泛型上下文，不改变既有具化依赖身份。

验证结果：

- Codegen 定向测试通过，并确认生成的 C 可由宿主 C 编译器编译；
- FCTS 定向测试通过（`1018/1018`）；
- 沙箱外全量 `make test` 以退出码 0 完成；UBSan 与常规优化构建阶段均通过，包括
  Smoke（`91/91`）、标准库（`601/601`）、FCTS（`1018/1018`）以及性能约束、增量构建、
  发布、安装、工具链和各编译器组件测试。

本次变更只影响编译期类型解析，不修改 runtime、ABI 或生成程序的运行时路径，因此不
引入运行时开销。第 6 节记录的 `throw Option<E>` 聚合值物化问题是本次修复后显露的独立
缺陷，不计入本次字段类型推导修复范围。
