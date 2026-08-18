# Feng `spec seal` 静态字段复用 `open` 跨包路径修复开发文档

> 状态：已实施（2026-08-18）
>
> 本文档只处理 object-form `spec seal` 静态字段 requirement 已在声明期选中
> type 的 seal 静态字段实现后，该字段没有复用现有 `open static` 字段跨包发码
> 路径的问题。字段满足、`spec seal` 访问域和 type 成员可见性继续以权威规范
> 为准；本专项不新增任何字段 ABI。

## 1 依据与目标

本专项以以下权威规范和已确认实现草案为准：

- [Feng 语言 `spec` 规范](../specifications/feng-spec.md)；
- [Feng 可见性规范](../specifications/feng-visibility.md)；
- [Feng 符号表规范](../specifications/feng-symbol-table.md)；
- [`spec` 支持定义 `seal` 成员实现草案](./feng-spec-seal-member-draft.md)。

需要让下面的公开名义关系在 provider 独立打包后正确工作：

```feng
open spec State {
  seal static let initial: int;
  seal static var current: int;
}

open type StateImpl: State {
  seal static let initial: int = 1;
  seal static var current: int = 2;

  open static func observe<T: State>(): int {
    T.current = T.current + 1;
    return T.initial + T.current;
  }
}
```

consumer 只导入 `.fb` 后调用：

```feng
StateImpl.observe<StateImpl>();
```

必须能够通过既有 static field witness getter/setter 访问 provider 中唯一的
静态存储，并继续遵守惰性初始化、`let`/`var`、所有权和可见性规则。

## 2 已确认的现状

### 2.1 spec 静态字段主体能力已经存在

现有实现已经支持：

- object-form spec 声明 `static let` / `static var`；
- type 自有静态字段完成 requirement 满足；
- 泛型约束通过 witness getter/setter 读取和写入静态字段；
- spec owner 泛参替换后的字段类型匹配；
- 字段 skeleton、static、mutability、类型和 visibility 写入 `.ft`；
- provider 内或同一编译单元中的 static field witness 发码。

`fit` 继续不能声明静态字段，因此本专项的字段实现来源只有 type 自身。

### 2.2 显式 `open` 实现路径可跨包工作

严格 `.fb` 探针确认：公开 spec static requirement 由显式 `open static` type
字段实现时，consumer 可以生成 getter/setter thunk，并正确使用 provider 的
storage 与 `ensure_init`。

因此 `spec seal` 字段必须完整复用这一现有路径，不设计新的 static field
witness、storage 名称、初始化协议、运行时 API 或独立 ABI。两者唯一的语言层
差异是：seal 字段在符号表中仍然保持 seal，普通成员访问仍受 seal 可见性检查。

### 2.3 选中的 seal 实现字段没有进入现有 `open` 发码路径

`spec seal` static requirement 可以按现有满足规则由 type seal static 字段
实现。provider 的声明期选择、字段 `.ft` skeleton、公开名义关系以及 consumer
按需计算出的 witness 选择均已存在，但当前 Codegen 的
`TypeStaticBinding.exports_public_surface` 只接受显式
`FENG_VISIBILITY_PUBLIC`。

结果是：

- provider 没有按现有 `open static` 路径生成可供 package consumer 使用的
  storage / `ensure_init`；
- consumer 已经能够恢复字段并生成 witness thunk；
- consumer 生成的 witness thunk 因而无法使用 provider 中对应的唯一静态状态。

最终可能表现为生成 C 缺少声明或链接器找不到符号，但这是“没有复用 open 发码
路径”的结果，不代表需要一套新的链接模型或 ABI。静态字段不是实例布局字段，
consumer 必须继续使用 provider 的唯一 storage 和初始化状态。

### 2.4 type 与 spec 的 default 均按现有 `open` 语义处理

[Feng 可见性规范](../specifications/feng-visibility.md) 已规定 type 与 spec 成员
无修饰时默认 `open`。Semantic 也以“不是显式 seal”处理公开 requirement 与实现
成员，但当前 type 静态字段 package surface 使用：

```text
member.visibility == FENG_VISIBILITY_PUBLIC
```

统一判定必须按 Feng 当前语义把 DEFAULT 与显式 open 完全等价处理，即以“不是
seal”判断语言公开性，不能继续只识别 `FENG_VISIBILITY_PUBLIC`。DEFAULT 不建立
独立分支，也不具有不同于 open 的跨包行为。这不是新增能力，而是保持既有语言
语义。

## 3 修复规则

### 3.1 统一复用 `open static` 发码路径

为 type 静态字段建立一个统一的编译期路径选择判定：

```text
language_public(member) = member.visibility != PRIVATE

provider_selected_dependency(member) =
    member 被进入 package-public `.ft` 的名义关系在声明期选为 requirement 实现

consumer_selected_dependency(member) =
    imported member 被当前分析中已经按需建立的 spec witness 精确选为实现

selected_spec_dependency(owner, member) =
    provider 使用 provider_selected_dependency(member)
    imported consumer 使用 consumer_selected_dependency(member)

uses_open_static_binding_codegen(owner, member) =
    owner 具有现有 package-public 声明面
    and (language_public(member) or selected_spec_dependency(member))
```

这里的 `selected_spec_dependency` 不是新的符号可见性或成员访问授权，而是复用
两处已经存在的统一满足选择事实：provider 使用声明期
`SpecImplementationSelection` sidecar；consumer 因 imported 声明不会重复执行
provider 的声明验证，使用当前分析中已经按需建立的 `SpecWitness` 精确映射。两者
都指向同一个实际实现成员，不按字段名或 spec 名称重新匹配。

该谓词只决定字段是否复用既有 `open static` 跨包发码路径，不能用于名称解析、
普通成员查询或可见性检查。该路径必须同时服务：

- provider storage 定义；
- provider `ensure_init` 定义和前向声明；
- consumer 对 imported storage / `ensure_init` 的 extern 声明；
- 普通非泛型 type 与现有泛型静态绑定 shared/closed 路径；
- `static let` 与 `static var`。

不得创建 seal 专用 storage、seal 专用 `ensure_init`、seal 专用 witness 槽位或
seal 专用命名规则。`selected_spec_dependency` 必须直接消费既有满足选择结果；
不得按字段名或 spec 名称增加特判，也不得成为绕过 seal 可见性的通用能力。

### 3.2 provider 完整复用现有路径

当 `uses_open_static_binding_codegen` 成立时：

- storage 完整使用现有显式 open static 字段的名称和生成逻辑；
- `ensure_init` 完整使用现有显式 open static 字段的名称和生成逻辑；
- 初始化状态、storage 和 `ensure_init` 仍只有 provider 定义的一份；
- 字段初始值、默认值、惰性初始化次序及失败行为不变；
- aggregate、managed pointer、普通 by-value struct 和 scalar 继续复用现有
  static binding 实现。

不得为 spec seal 字段生成第二份影子 storage、consumer 本地 storage 或专用
getter/setter 状态。

### 3.3 consumer 声明面

consumer 从 package-public `.ft` 恢复公开名义关系和字段 skeleton，并在现有语义
分析中按需建立 witness 后，使用 witness 中已经选中的精确 `impl_member` 判断该
imported 字段是否进入同一路径，完全复用 `open static` 字段已有的 storage /
`ensure_init` 声明。witness thunk 继续调用现有 `ensure_init` 后读写 storage：

- `static let` 只生成 getter；
- `static var` 生成 getter 和 setter；
- getter/setter 的参数、返回方式和 aggregate 所有权规则不变；
- 不在 consumer 再次执行字段初始化器。

consumer 不能仅因 imported `.ft` 中存在一个 seal static 字段就生成可用实现；
只有当前合法使用所需的现有 witness 已经精确选择该字段时，
`consumer_selected_dependency` 才成立。该查询只发生在编译期，不增加生成代码的
运行时判断。

### 3.4 `.ft` 中仍然是 seal

现有符号表规则已经让 type 字段 skeleton 进入必要的编译器视图，并记录公开名义
关系；consumer 使用这些既有信息按需重建 `SpecWitness` 选择。本专项不序列化
`SpecImplementationSelection` 或 `SpecWitness`，也不增加新的 `.ft` section、
flag、attr、relation 或版本。

字段在 `.ft` 中继续保留等价的语言可见性：

- 无修饰/default 与显式 open 均按现有 wire 规则记录为公开；
- seal 仍为 seal。

复用 `open static` 发码路径不会把 seal 字段改为 open，也不会让它进入普通 Feng
成员访问、补全或文档公开面。所有名称解析和成员访问仍只依据 `.ft` 中保存的 seal
可见性及既有授权规则；Codegen 的路径选择结果不得反向参与或绕过可见性检查。

### 3.5 满足与安全边界

以下既有规则保持不变：

- spec 无修饰/static requirement 只能由 type 的 open/default static 字段满足；
- spec seal/static requirement 可以由 type 的 open/default 或 seal static 字段满足；
- 字段名称、`let`/`var` 和替换后的类型必须完全匹配；
- fit 不得提供静态字段；
- 外包自定义 spec/fit 不能选择 imported type 的 seal 字段；
- spec seal static 字段只能在现有实现域内通过 spec 静态视角访问；
- type 自身的普通 seal 静态字段访问规则不变。

未被任何 package-public 名义关系选中的普通 seal static 字段不得因为同一 owner
的其他字段复用了跨包路径而被一并放行。

### 3.6 泛型 owner 的边界

路径选择规则必须对普通及泛型 type 使用同一抽象。泛型 type 的 closed
static storage、shared `ensure_init` 和 descriptor sidecar 继续使用现有泛型静态
绑定模型，不新增运行时查找。

但是，泛型 `type<T>: Spec<T>` 名义关系在严格 `.fb` 边界的恢复由
[泛型 `spec` 满足关系跨包修复开发文档](./feng-generic-spec-implementation-package-bugfix.md)
独立处理。在该关系修复前，本专项的非泛型用例应能独立完成；泛型正向用例作为两项
修复集成验证，不得用静态字段补丁掩盖 relation 的 `AE1003`。

## 4 范围边界

### 4.1 本次包含

- spec 无修饰和 seal static `let` / `var` requirement；
- type 显式 open、default 和 seal static 字段实现；
- 非泛型及现有泛型 type static binding 路径选择；
- scalar、managed、aggregate 与普通 by-value struct 字段；
- provider、consumer、witness getter/setter 和惰性初始化；
- 同一字段多次读取/写入及多个 consumer 调用共享唯一状态。

### 4.2 本次不包含

- 改变 spec/type 静态字段满足规则或访问域；
- 允许 fit 声明静态字段；
- 新增字段 offset witness 或把静态字段改成实例布局字段；
- 泛型 type/fit 名义关系恢复；
- spec 方法级泛型；
- object-form spec 方法值；
- 修改 runtime static binding 数据结构、惰性初始化协议或生命周期语义；
- 与正确性无关的静态绑定性能优化。

## 5 性能与兼容性

- seal 实现只进入现有 `open static` 字段发码分支，不增加运行时操作；
- getter/setter 仍执行现有一次直接 `ensure_init` 调用和一次 storage 读写；
- 不增加 witness 槽位、间接调用、名称查找、成员遍历、堆分配、锁或缓存；
- 不复制静态 storage，不改变初始化次数或时序；
- open/default static 字段继续使用既有路径；
- seal 字段的 Feng 源码访问范围保持不变。

如果实施需要新增 runtime API、每次访问分支或 consumer 本地镜像 storage，必须暂停
并由人工决策。

## 6 测试要求

### 6.1 编译器测试

- Semantic：无修饰/seal spec requirement 与 open/default/seal type 字段的满足矩阵；
- Semantic sidecar：只标记进入 package-public 名义关系且实际被选中的字段；
- Symbol：static、mutability、类型和 visibility round-trip，确认不新增 wire 特判；
- Codegen provider：selected-seal 与 open/default 使用完全相同的 storage 和
  `ensure_init` 生成路径；
- Codegen consumer：selected-seal 与 open/default 使用完全相同的已有声明和
  getter/setter thunk 路径，并可编译执行；
- CLI：provider 独立 pack，consumer 只依赖 `.fb` 观察初值、读取、写入和共享状态。

负向覆盖必须包括：

- spec 公开 requirement + type seal 字段仍拒绝满足；
- 未选中的普通 seal static 字段不进入 open 跨包发码路径；
- consumer 普通代码不能通过具体 type 或 spec 视角访问 seal 字段；
- 外包自定义 spec/fit 不能选择 imported seal 字段；
- static `let` 没有 setter；
- 同一 type 中相邻公开/selected-seal/无关-seal 字段不会互相扩大导出范围。

### 6.2 FCTS

在 `fcts_lib -> fcts_bin` 增加跨包可观察行为：

- spec seal static `let` 和 `var`；
- type seal 实现；
- type 成员方法、静态方法及合法 `fit Target` 通过 spec 视角访问；
- default/open 实现基线；
- scalar、managed 和 aggregate 字段；
- 多次 getter/setter 共享 provider 唯一状态；
- 普通 seal 字段没有被意外授权。

同时保留隔离 `.fb` CLI 用例，避免本地 workspace 构建掩盖 seal 字段未复用 open
路径的问题。

## 7 TODO 与实施顺序

- [x] **实际变更（统一路径选择）**：在 type static binding 现有发码入口建立通用
  helper：open/default 字段保持原路径；被 package-public spec relation 选中的 seal
  实现也进入同一路径。不得按字段名或具体 spec 增加特判。
- [x] **实际变更（消费端选择查询）**：增加只读的通用 Semantic 查询，判断一个
  member 是否被当前已有的 `SpecWitness` 精确选为 `impl_member`；只供 imported
  consumer 的发码路径选择使用，不改变 witness 计算、满足规则或成员可见性。
- [x] **实际变更（provider Codegen）**：让 selected-seal static storage 与
  `ensure_init` 完整复用显式 open 路径，不增加名称、状态或生成分支。
- [x] **实际变更（consumer Codegen）**：让 imported selected-seal 字段完整复用
  open 路径已有的声明、getter/setter thunk 和初始化调用。
- [x] **验证（Symbol）**：确认现有字段 skeleton 和公开名义 relation 已足以让
  consumer 的既有 witness 恢复精确选择，并确认 selected-seal 字段在 `.ft` 中仍为
  seal；不得增加 spec-static 专用 wire 格式。
- [x] **验证（普通公开基线）**：覆盖显式 open 以及规范等价的 DEFAULT static 字段
  跨包直接访问和 spec witness；该项用于防止路径选择再次只识别显式 open。
- [x] **验证（编译器用例）**：补齐 Semantic、Symbol、Codegen 和隔离 `.fb` CLI
  正负用例，并恢复现有暂停静态字段探针中属于本专项的部分。
- [x] **验证（FCTS）**：补齐 `fcts_lib -> fcts_bin` 的可观察状态与访问边界用例。
- [ ] **后续集成验证（不属于本专项完成条件）**：在泛型关系专项完成后验证泛型
  owner static 字段；本专项不重复实现泛型 relation 修复。
- [x] **验证（回归）**：执行定向测试后，在沙箱外执行 `make test` 全量回归。

本专项的最小正确修复只有一件事：让声明期已选中的 seal static 字段进入现有
`open static` 字段跨包发码路径；其 `.ft` visibility 仍为 seal。不得创建新的 spec
seal 字段 ABI。若现有实现无法原样复用该路径，必须暂停并由人工决策。
