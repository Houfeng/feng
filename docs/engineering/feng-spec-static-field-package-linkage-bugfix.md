# Feng `spec` 静态字段实现依赖跨包链接修复开发文档

> 状态：草案，待 Review（2026-08-18）
>
> 本文档只处理 object-form `spec` 静态字段 requirement 已在声明期选中
> type 静态字段实现后，provider storage / `ensure_init` 在 package `.fb`
> 边界不可链接的问题。字段满足、`spec seal` 访问域和 type 成员可见性继续
> 以权威规范为准。

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

### 2.2 显式 open 实现路径可跨包工作

严格 `.fb` 探针确认：无修饰（公开）spec static requirement 由显式
`open static` type 字段实现时，consumer 可以生成 getter/setter thunk，找到
provider storage 与 `ensure_init`，并完成链接。

因此无需设计新的 static field witness、storage 命名、初始化协议或运行时 API。
`spec seal` 字段应复用这一现有路径；差异只在“哪些实现字段被允许进入
package compiler ABI surface”。

### 2.3 选中的 seal 实现字段只有声明骨架，没有链接能力

`spec seal` static requirement 可以按现有满足规则由 type seal static 字段
实现。声明期选择和字段 `.ft` skeleton 均已存在，但当前 Codegen 的
`TypeStaticBinding.exports_public_surface` 只接受显式
`FENG_VISIBILITY_PUBLIC`。

结果是：

- provider 把 storage / `ensure_init` 保持为内部符号，或没有生成可供包外引用的
  声明形态；
- consumer 已经能够恢复字段并生成 witness thunk；
- consumer 生成 C 时缺少 storage / `ensure_init` 声明，或最终链接找不到符号。

这是真正链接信息缺失，不是满足检查失败，也不能由 Codegen 绕过可见性直接按
未知偏移访问。静态字段不是实例布局字段，consumer 必须引用 provider 的唯一
storage 和初始化状态。

### 2.4 type 成员 DEFAULT 与规范 open 语义不一致

[Feng 可见性规范](../specifications/feng-visibility.md) 已规定 type 成员无修饰时
默认 `open`。Semantic 也以“不是显式 seal”允许普通访问和公开 requirement
满足，但当前静态字段 package surface 使用：

```text
member.visibility == FENG_VISIBILITY_PUBLIC
```

因此无修饰 type static 字段即使位于 open module/open type 中，也可能在
consumer 侧产生未定义 storage/`ensure_init`。这与语言默认可见性不一致。

本专项建议在同一个通用 eligibility 谓词中修复 DEFAULT/open 等价性；这不是
新增可见性能力，而是让静态字段链接面符合既有权威规范。不得只针对某个 spec、
字段名或 seal 场景增加分支。

## 3 修复规则

### 3.1 统一 package static binding eligibility

为 type 静态字段建立统一的编译期判定：

```text
language_public(member) = member.visibility != PRIVATE

selected_spec_dependency(member) =
    member 被进入 package-public `.ft` 的名义关系在声明期选为 requirement 实现

exports_package_static_binding(owner, member) =
    owner 具有现有 package-public 声明面
    and (language_public(member) or selected_spec_dependency(member))
```

该谓词必须同时服务：

- provider storage 定义的 C linkage；
- provider `ensure_init` 定义和前向声明；
- consumer 对 imported storage / `ensure_init` 的 extern 声明；
- 普通非泛型 type 与现有泛型静态绑定 shared/closed 路径；
- `static let` 与 `static var`。

不得让 Symbol writer、provider Codegen 和 consumer Codegen 各自重新按名称或
visibility 猜测。`selected_spec_dependency` 必须直接消费声明期统一满足选择
sidecar。

### 3.2 provider 链接面

当 `exports_package_static_binding` 成立时：

- storage 使用与现有显式 open static 字段相同的稳定符号名称和外部 linkage；
- `ensure_init` 使用同一现有命名和外部 linkage；
- 初始化状态、storage 和 `ensure_init` 仍只有 provider 定义的一份；
- 字段初始值、默认值、惰性初始化次序及失败行为不变；
- aggregate、managed pointer、普通 by-value struct 和 scalar 继续复用现有
  static binding 实现。

不得为 spec seal 字段生成第二份影子 storage、consumer 本地 storage 或专用
getter/setter 状态。

### 3.3 consumer 声明面

consumer 从 package-public `.ft` 恢复公开名义关系和字段 skeleton 后，使用同一
eligibility 规则生成 storage / `ensure_init` extern 声明。witness thunk 继续调用
现有 `ensure_init` 后读写 storage：

- `static let` 只生成 getter；
- `static var` 生成 getter 和 setter；
- getter/setter 的参数、返回 ABI 和 aggregate 所有权规则不变；
- 不在 consumer 再次执行字段初始化器。

consumer 不能仅因 imported `.ft` 中存在一个 seal static 字段就生成可用实现；
只有已恢复的公开名义关系选择该字段时，`selected_spec_dependency` 才成立。

### 3.4 `.ft` 与可见性

现有符号表规则已经要求 type 字段 skeleton 进入必要的布局/编译器视图，并明确
要求被选中的 seal static 字段 storage/ensure 具有编译器 ABI 链接身份。本专项
原则上不增加新的 `.ft` section、flag、attr、relation 或版本。

字段在 `.ft` 中继续保留等价的语言可见性：

- 无修饰/default 与显式 open 均按现有 wire 规则记录为公开；
- seal 仍为 seal。

获得外部 C linkage 不会让 seal 字段进入普通 Feng 成员访问、补全或文档公开面。
名称解析和访问检查必须继续拒绝未授权的具体 type seal 访问。

### 3.5 满足与安全边界

以下既有规则保持不变：

- spec 无修饰/static requirement 只能由 type 的 open/default static 字段满足；
- spec seal/static requirement 可以由 type 的 open/default 或 seal static 字段满足；
- 字段名称、`let`/`var` 和替换后的类型必须完全匹配；
- fit 不得提供静态字段；
- 外包自定义 spec/fit 不能选择 imported type 的 seal 字段；
- spec seal static 字段只能在现有实现域内通过 spec 静态视角访问；
- type 自身的普通 seal 静态字段访问规则不变。

未被任何 package-public 名义关系选中的 seal static 字段不得因为同一 owner 的其他
字段被导出而获得外部 linkage。

### 3.6 泛型 owner 的边界

eligibility 和链接规则必须对普通及泛型 type 使用同一抽象。泛型 type 的 closed
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
- 非泛型及现有泛型 type static binding 链接资格；
- scalar、managed、aggregate 与普通 by-value struct 字段；
- provider package、consumer extern、witness getter/setter 和惰性初始化；
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

- 只改变编译器生成符号的 linkage 和 extern 声明，不增加运行时操作；
- getter/setter 仍执行现有一次直接 `ensure_init` 调用和一次 storage 读写；
- 不增加 witness 槽位、间接调用、名称查找、成员遍历、堆分配、锁或缓存；
- 不复制静态 storage，不改变初始化次数或时序；
- 非 spec 的公开/default static 字段只修正既有公开语义对应的链接面；
- seal 字段的 Feng 源码访问范围保持不变。

如果实施需要新增 runtime API、每次访问分支或 consumer 本地镜像 storage，必须暂停
并由人工决策。

## 6 测试要求

### 6.1 编译器测试

- Semantic：无修饰/seal spec requirement 与 open/default/seal type 字段的满足矩阵；
- Semantic sidecar：只标记进入 package-public 名义关系且实际被选中的字段；
- Symbol：static、mutability、类型和 visibility round-trip，确认不新增 wire 特判；
- Codegen provider：open/default/selected-seal storage 与 `ensure_init` 使用外部 linkage；
- Codegen consumer：生成匹配的 extern 声明、getter/setter thunk，并可编译链接；
- CLI：provider 独立 pack，consumer 只依赖 `.fb` 观察初值、读取、写入和共享状态。

负向覆盖必须包括：

- spec 公开 requirement + type seal 字段仍拒绝满足；
- 未选中的普通 seal static 字段不获得 package linkage；
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

同时保留隔离 `.fb` CLI 用例，避免本地 workspace 构建掩盖 storage/ensure 链接缺失。

## 7 TODO 与实施顺序

- [ ] **实际变更（统一判定）**：建立 type static binding 的 package eligibility
  helper，统一表达 DEFAULT/open 语言可见性和声明期选中的 spec implementation
  dependency；不得按字段名或 spec seal 增加特判。
- [ ] **实际变更（provider Codegen）**：让符合 eligibility 的普通/泛型 static
  storage 与 `ensure_init` 复用显式 open 路径的稳定名称和外部 linkage。
- [ ] **实际变更（consumer Codegen）**：让 imported 字段使用同一 eligibility 生成
  storage / `ensure_init` extern 声明，保持 getter/setter 与初始化逻辑不变。
- [ ] **验证（Symbol）**：确认现有字段 skeleton 和公开名义 relation 已足以恢复
  eligibility；只有验证发现通用事实缺失时，才补齐 writer/reader，且不得增加
  spec-static 专用 wire 格式。
- [ ] **验证（普通公开基线）**：覆盖显式 open 以及规范等价的 DEFAULT static 字段
  跨包直接访问和 spec witness；该项用于防止 eligibility 再次只识别显式 open。
- [ ] **验证（编译器用例）**：补齐 Semantic、Symbol、Codegen 和隔离 `.fb` CLI
  正负用例，并恢复现有暂停静态字段探针中属于本专项的部分。
- [ ] **验证（FCTS）**：补齐 `fcts_lib -> fcts_bin` 的可观察状态与访问边界用例。
- [ ] **验证（集成）**：在泛型关系专项完成后验证泛型 owner static 字段；该项不在
  本专项重复实现泛型 relation 修复。
- [ ] **验证（回归）**：执行定向测试后，在沙箱外执行 `make test` 全量回归。

本专项的最小正确修复是“复用 open static binding 路径并修正统一 eligibility”，
不是创建新的 spec seal 字段 ABI。若现有实现无法在不改变 runtime 的前提下复用该
路径，必须暂停并由人工决策。
