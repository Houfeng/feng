# Feng LSP 类型名称族 References / Rename 优化方案

> 状态：已实施并通过自动化、性能与全量回归，待人工代码 Review。
>
> 本文档是 Feng 类型、构造函数和终结器名称关联语义在 LSP References / Rename 中的主规范。
>
> 关联文档：
>
> - [Feng LSP 已交付方案](feng-lsp-delivered.md)：定义既有 LSP 能力和通用边界；
> - [Feng LSP 工作区多成功分析缓存方案](feng-lsp-local-project-dependency-dev.md)：定义多项目缓存、
>   跨 session 稳定身份和全局查询；
> - [Feng LSP 性能优化方案](feng-lsp-performance-optimize.md)：定义请求路径和性能验收标准。
>
> 本文档只定义类型名称族语义，不重复定义上述缓存、稳定身份和性能规范。

---

## 1. 最终结论

Feng LSP 必须区分两个概念：

1. **精确符号引用**：保留构造函数重载级精度；
2. **类型名称族重命名**：保证所有受类型名称约束的源码 Token 原子改名。

一个显式构造调用同时具有两层语义：

```text
Thickness(margin)
│
├── 类型引用：Thickness
└── 精确构造引用：Thickness(int)
```

不得把类型和全部构造函数合并为一个符号，否则会失去重载级 References、Definition 和 Hover
精度；也不得只保留精确构造关系，否则从类型名查询和重命名会漏掉构造声明、构造调用及终结器。

统一行为如下：

| 发起位置 | References | Rename |
| --- | --- | --- |
| 类型声明或普通类型引用 `Thickness` | 类型引用及整个类型名称族 | 整个类型名称族 |
| 显式构造声明 `Thickness(int)` | 仅该构造重载及其调用 | 先提升到所属类型，再重命名整个类型名称族 |
| 显式构造调用 `Thickness(value)` | 仅语义分析命中的构造重载 | 先提升到所属类型，再重命名整个类型名称族 |
| 显式终结器 `~Thickness` | 仅该终结器；不伪造运行时调用位置 | 先提升到所属类型，再重命名整个类型名称族 |

其中“整个类型名称族”包括：

- 类型声明；
- 普通类型引用；
- 全部显式构造函数声明；
- 全部显式构造重载的调用；
- 隐式默认构造对应的源码调用；
- 显式终结器声明。

一个类型名称族只属于一个**精确类型声明**。同一 module 中即使名称相同，`UserType`、
`UserType<T>` 和 `UserType<T1, T2>` 也分别属于三个互不影响的名称族。

类型名称族只关联真正的构造函数和终结器。普通实例方法、普通静态方法及其所有重载均不参与；spec
中与 spec 同名的方法仍是普通方法，也不参与 spec/type 名称族。

隐式默认构造函数和终结器的运行时隐式调用没有源码 Token，不生成虚构 Location 或 TextEdit。

---

## 2. 变更边界

### 2.1 允许范围

- 生产代码只能修改 `src/cli/lsp/`；
- 采用最小变更时，生产实现应集中在 `src/cli/lsp/service.c`；
- 测试可在 `test/` 增加新用例、fixture 和仅服务于新用例的 helper；
- 禁止改变已有测试用例的行为和断言。

### 2.2 禁止范围

本优化禁止修改：

- parser、semantic analyzer 和编译器核心符号查询接口；
- FT / FB 的结构、生成、读取和 symbol id 规则；
- IR、`#line`、生成 C、Codegen、Runtime 和 DAP；
- `feng.fm`、依赖解析和项目定位规则；
- 多项目成功缓存的生命周期、调度和发布规则。

本问题是 LSP 对现有 AST / Semantic Analysis 结果的消费不完整，不需要改变语言语义或编译产物。

### 2.3 不在本次范围内

- 不新增构造函数或终结器语法；
- 不允许构造函数或终结器拥有独立于所属类型的名称；
- 不改变构造重载选择；
- 不改变普通方法、静态方法或其重载的 References / Rename；
- 不把 spec 中与 spec 同名的方法解释为构造函数；
- 不改变 Definition、Hover、Completion、Signature Help 或 Implementation 的目标语义；
- 不扫描未被既有工作区缓存方案纳入的任意仓库目录。

---

## 3. 当前代码事实

### 3.1 构造函数名称由所属类型决定

`src/parser/parser.c` 在类型成员名称等于所属类型名称，并且返回类型为空或 `void` 时，将成员分类为
`FENG_TYPE_MEMBER_CONSTRUCTOR`。

因此只修改某一个构造函数名称会改变该声明的语言语义，使其不再是原构造函数，不能作为合法的独立
Rename 操作。

### 3.2 终结器名称强制等于所属类型

显式终结器使用：

```feng
func ~Thickness() {}
```

parser 已强制终结器名称匹配所属类型；不匹配时报：

```text
finalizer name must match the enclosing type name
```

Feng 同时禁止通过 `.~` 直接调用终结器。因此终结器只有显式声明位置，没有应由 LSP 伪造的调用位置。

### 3.3 spec 同名成员是普通方法

`src/parser/parser.c` 的 spec member 解析默认并保持 `FENG_TYPE_MEMBER_METHOD`，不会因为 member 名称与
spec 名称相同而分类为 constructor。现有 parser 用例已经覆盖同一 spec 中同名的 instance/static
method。

因此以下声明中的两个 `SameName` 都是普通方法：

```feng
spec SameName {
  static func SameName(): int;
  func SameName(): int;
}
```

它们的 References / Rename 必须继续使用各自精确方法身份，不能跟随 spec 声明重命名，也不能把
method Rename 提升为 spec Rename。

### 3.4 LSP 已经区分类型和构造重载

当前 `FengLspResolvedTarget` 使用：

- `FENG_LSP_RESOLVED_DECL` 表示类型声明；
- `FENG_LSP_RESOLVED_MEMBER` 表示具体构造函数成员。

调用表达式通过 `FengResolvedCallable` 精确定位一个构造重载。该区分是正确能力，必须保留。

### 3.5 当前缺陷位于调用引用收集

当前 `collect_references_in_expr()` 遇到 `FENG_EXPR_CALL` 时：

1. 通过 `resolved_callable` 得到具体 callable；
2. 把 callee Token 只与该具体 callable target 比较；
3. callable 成功解析后，不再把 identifier callee 作为所属类型引用遍历。

因此 `Thickness(margin)` 只被记录为某个构造重载的引用，没有同时成为 `Thickness` 类型引用。

### 3.6 实际复现结果

在 `std/std/src/tui/common/Thickness.ff` 和 `std/std_test/src/test_tui.ff` 上，当前协议结果为：

| 查询位置 | 总结果 | `test_tui.ff` 结果 |
| --- | ---: | ---: |
| 第 9 行类型 `Thickness` | 9 | 0 |
| 第 52 行 `Thickness(int)` 构造 | 15 | 13 |
| 第 60 行 `Thickness(double)` 构造 | 1 | 0 |

第 52 行能够返回本地依赖消费项目中的 13 个调用，证明：

- `std_test` 成功 session 已经存在；
- 跨项目稳定身份和物理源码映射已经工作；
- 构造重载选择已经工作；
- 本缺陷不是路径、FT/FB 或多项目缓存问题。

### 3.7 当前 Rename 明确排除构造函数和终结器

`resolved_target_can_rename()` 目前只允许字段和普通方法成员 Rename，构造函数与终结器均返回不可
Rename。类型声明本身可以 Rename，但其引用集合会漏掉构造声明、构造调用和终结器。

因此当前状态同时存在：

- 从类型名 Rename 不完整；
- 从构造函数、构造调用或终结器不能发起 Rename。

---

## 4. 目标语义

### 4.1 精确符号身份保持不变

以下目标继续保持相互独立：

- 类型声明；
- 每一个显式构造重载；
- 显式终结器。

进入类型名称族的 member 必须同时满足：

```text
owner declaration kind == FENG_DECL_TYPE
member kind == FENG_TYPE_MEMBER_CONSTRUCTOR
             或 FENG_TYPE_MEMBER_FINALIZER
```

`FENG_TYPE_MEMBER_METHOD` 永不进入类型名称族，不论名称、参数、返回类型、static、可见性或重载数量
是否与 owner 相似。`FENG_DECL_SPEC` 下的 member 也永不进入类型名称族。

现有 AST 指针身份、跨 session 稳定身份和 imported `(module_name, symbol_id)` 身份均不改变。

泛型元数是类型声明身份的一部分：

- 同一 session 内以具体 owner declaration 指针区分，不以名称区分；
- 跨 session 的 nominal fallback 已包含 module、声明 kind、名称和 generic arity；
- imported symbol 主路径继续优先使用精确 `(module_name, symbol_id)`；
- 完整 declaration shape fallback 继续比较泛型形参列表及约束。

因此 `UserType` 的 References / Rename 不得进入 `UserType<T>` 或 `UserType<T1, T2>` 的声明、构造、
调用和终结器，反向亦然。

不同具体类型实参不是不同声明：`UserType<int>` 与 `UserType<string>` 都引用同一个 arity 为 1 的
`UserType<T>` 声明，因此属于同一名称族，并应随该声明一起 Rename。

### 4.2 类型名称族

仅在 LSP 请求期间建立不拥有 AST 的“类型名称族”关系：

```text
TypeNameFamily(Thickness)
├── owner type: Thickness
├── constructors[]
│   ├── Thickness(int)
│   ├── Thickness(double)
│   └── ...
└── finalizer?: ~Thickness
```

该关系由现有 `FengDecl` 所有权和 `FengTypeMemberKind` 推导，不持久化、不写缓存、不按文本名称猜测。
名称族的逻辑 key 是精确 owner declaration 身份，不是 `Thickness` 文本，也不是单独的
`(module, name)`。同名不同泛参数量的类型必须建立不同名称族。

### 4.3 从类型目标发起 References

当原始查询目标是类型声明时，References 使用非对称匹配：

```text
类型目标  匹配  同一类型的普通类型引用
类型目标  匹配  同一类型的全部构造声明
类型目标  匹配  同一类型的全部构造调用
类型目标  匹配  同一类型的显式终结器声明
```

“同一类型”必须使用当前 session 内已经本地化的 owner declaration 身份证明，禁止只比较名称、module
字符串或缺少 generic arity 的签名。

当 `includeDeclaration == true` 时，结果包括类型声明、构造声明和终结器声明；为 `false` 时，只返回
普通类型引用和构造调用，不返回上述声明位置。

### 4.4 从具体构造发起 References

当原始查询目标是某个构造函数时，继续使用精确成员身份：

- 包含该构造声明（仅当 `includeDeclaration == true`）；
- 包含语义分析证明调用该重载的位置；
- 不包含其他构造重载；
- 不包含类型声明、普通类型引用或终结器。

从一个显式构造调用发起 References，行为与从其解析命中的构造声明发起完全一致。

### 4.5 从终结器发起 References

终结器保持精确目标：

- `includeDeclaration == true` 时返回显式终结器声明；
- `includeDeclaration == false` 时通常为空；
- 不把变量离开作用域、ARC release、循环回收等运行时事件伪造成源码引用。

### 4.6 Rename 统一提升到所属类型

Rename 与 References 不使用完全相同的入口目标：

```text
类型目标                  → owner type
构造声明或构造调用目标    → owner type
终结器目标                → owner type
其他可 Rename 目标        → 保持原目标
```

构造/终结器到 owner type 的提升必须先验证 owner 为 `FENG_DECL_TYPE` 且 member kind 精确为 constructor
或 finalizer。普通方法和 spec member 必须走现有 Rename 路径，禁止按同名关系提升。

类型名称族 Rename 必须一次生成覆盖全部相关成功 session 的单个 `WorkspaceEdit`，包含：

- owner type 声明；
- 全部普通类型引用；
- 全部显式构造声明；
- 全部构造调用，包括不同文件和本地依赖项目中的调用；
- 显式终结器名称。

从任意构造重载、任意构造调用、终结器或类型名发起，得到的 TextEdit 集合必须相同；只允许输出顺序
因现有 JSON 构建顺序不同。

Rename 仍遵守既有安全规则：

- 定义类型必须具有可写工作区物理源码；
- 所有参与 session 必须完整且与打开文档一致；
- 任一必要 session 无法证明或已 stale 时不返回部分 edit；
- 外部只读 package 不生成 Rename；
- 新名称继续使用既有标识符合法性校验；
- 同一路径和 range 的 edit 必须去重。

### 4.7 Prepare Rename

从类型、构造声明、构造调用或终结器发起 Prepare Rename 时：

- 返回用户当前指向的名称 Token range；
- placeholder 为当前类型名称；
- `~Thickness` 只选择 `Thickness`，不得把 `~` 放入 edit range；
- 仅当完整 Rename 能力可用时才成功。

### 4.8 其他 LSP 能力保持不变

- 构造调用 Definition 继续跳到精确构造重载；
- 类型名 Definition 继续跳到类型声明；
- 构造 Hover 继续显示精确构造签名；
- 类型 Hover 继续显示类型信息；
- Completion、Signature Help、Implementation 和 Document Symbol 不改变。

---

## 5. 最小实现方案

### 5.1 增加非对称引用匹配 helper

在 `src/cli/lsp/service.c` 内增加私有 helper，统一替代引用收集路径中分散的纯
`resolved_targets_equal()` 判断：

```text
reference_target_matches(expected, candidate)
├── 两者精确相等：匹配
├── expected 是 type declaration，candidate 是同 type owner 的 constructor：匹配
├── expected 是 type declaration，candidate 是同 type owner 的 finalizer：匹配
└── 其他情况：不匹配
```

该匹配必须是非对称的：

- 类型查询可以聚合构造与终结器；
- 构造查询不能反向聚合类型或其他构造；
- 终结器查询不能反向聚合类型。

其中“同 owner”必须是当前 session 内同一个精确 declaration；禁止通过 owner 名称相等接受候选。
跨 session 进入该 helper 前，必须已经通过既有稳定身份完成本地化，从而保持 generic arity 隔离。
candidate 为普通 method 或 owner 为 spec 时，即使名称与 expected 完全相同，也只能在“精确相等”分支
匹配自己的 method target，不能进入 type 聚合分支。

所有新增 helper 和结构体必须按仓库规则编写职责注释。

### 5.2 复用现有 callable 解析

构造调用继续使用现有 `resolved_callable` 结果，不重新执行重载推断，也不新增基于参数文本的匹配。

调用收集只把已经得到的：

```text
candidate.kind   = FENG_LSP_RESOLVED_MEMBER
candidate.decl   = owner type
candidate.member = exact constructor
```

交给非对称引用匹配 helper。这样同一个 callee Token 可进入类型查询结果，同时仍属于一个精确构造
查询结果。

### 5.3 将构造和终结器声明接入同一匹配

`collect_references_in_member()` 在处理声明位置时构造临时 member target，并使用同一非对称匹配
helper：

- 类型查询且 `includeDeclaration == true`：收集全部构造及终结器名称；
- 具体构造查询：只收集该构造声明；
- 具体终结器查询：只收集该终结器声明。

禁止另外扫描源码字符串查找 `func TypeName` 或 `func ~TypeName`。

### 5.4 增加 Rename 目标规范化 helper

在 Prepare Rename 和 Rename 的 workspace 路径进入稳定身份构建前，将构造函数或终结器目标规范化为
其 `target->decl` owner type：

```text
normalize_rename_target(target) -> owner type target
```

随后完全复用：

- 既有可写源码检查；
- 既有跨 session 稳定目标；
- 既有 workspace References 聚合；
- 既有 stale / complete 检查；
- 既有单个 `WorkspaceEdit` 构建和去重。

未命中精确成功 session 时，单文件 current-parse fallback 无法证明其他文件和项目中不存在类型名称族
引用。因此该 fallback 必须识别并拒绝类型、构造函数和终结器的 Prepare Rename / Rename，只调度既有
后台分析；参数、局部变量等可由单文件证明完整的既有 Rename 行为保持不变。禁止返回当前文件的部分
类型 Rename。

普通字段、方法、函数、参数、局部变量、泛型形参等 Rename 不进入该规范化分支，行为保持不变。
尤其是普通重载方法仍按当前精确 member/callable 身份查询和 Rename；spec 同名方法不规范化为 spec。

### 5.5 跨项目处理

类型名称族关系只在每个已本地化 session 内使用：

1. 原始类型或构造目标先通过既有稳定身份定位 defining workspace；
2. 以 owner type 稳定身份本地化到每个相关 session；
3. 在该 session 中按 owner declaration 身份聚合类型、构造和终结器 Token；
4. 按既有 physical path/range 规则合并结果。

不新增项目缓存、不改变 session 数组、不反向扫描任意 repo 项目，也不读取或生成额外 FT/FB。

### 5.6 禁止方案

以下方案禁止采用：

- 把所有构造重载与类型永久合并成一个 `FengLspResolvedTarget`；
- 让具体构造 References 返回所有重载；
- 仅按 `Thickness` 文本匹配引用；
- 因普通方法与 type/spec 同名而把它加入名称族；
- 把 spec 同名方法视为 constructor，或把其 Rename 提升为 spec Rename；
- 为每种类型或 `Thickness` 增加特判；
- 修改 parser / semantic 以注入 LSP 专用关系；
- 修改 FT/FB 载荷以保存 LSP Rename 数据；
- Rename 开始时修改磁盘源码或编译缓存；
- 某个 session 失败时返回部分跨项目 Rename。

---

## 6. 性能与并发约束

[Feng LSP 性能优化方案](feng-lsp-performance-optimize.md) 的全部既有指标保持不变，不因结果集合扩大而
放宽。

本优化在请求路径中只能增加：

- O(1) 的 target kind / owner pointer 判断；
- O(1) 的 Rename 目标规范化；
- 对既有 References AST 遍历中已经访问到的声明和调用进行一次额外语义匹配。

必须继续保留：

- source name 必要条件预过滤；
- 缓存优先；
- 请求线程零同步磁盘 I/O；
- 请求线程零整项目分析；
- 后台 candidate 成功后无空窗替换；
- References 可跳过不精确 session、Rename 必须要求完整 session 的既有区别。

不得为了类型名称族结果同步等待未完成分析，缓存 miss / stale 继续使用既有后台调度策略。

---

## 7. 新增测试方案

测试只增加新用例，不修改任何已有用例或 expected。

### 7.1 单项目语义

新增一个包含两个可区分构造重载及终结器的类型：

```feng
type Widget {
  func Widget(value: int) {}
  func Widget(value: double) {}
  func ~Widget() {}
}
```

至少验证：

- [ ] 类型 References 包含类型声明、两个构造声明、终结器声明、普通类型引用和两个构造调用；
- [ ] `includeDeclaration == false` 时不包含类型、构造和终结器声明；
- [ ] `Widget(int)` References 只包含该构造及 `int` 调用；
- [ ] `Widget(double)` References 只包含该构造及 `double` 调用；
- [ ] 从两个构造调用发起 References，分别得到对应重载集合；
- [ ] 终结器 References 不包含伪造的运行时调用位置；
- [ ] 同名但不同 module / owner 的类型、构造和终结器不串线；
- [ ] 同一 module 同时声明 `UserType`、`UserType<T>`、`UserType<T1, T2>` 时，三组类型、构造调用和
      终结器的 References 完全隔离；
- [ ] 从上述任一 arity 发起 Rename，只编辑该精确类型名称族，其他 arity 零 edit；
- [ ] 普通实例/静态重载方法保持精确 References，类型名称族查询不包含任何普通方法声明或调用；
- [ ] 从一个普通重载方法发起 Rename，只保持既有方法重载行为，不产生类型、构造或终结器 edit；
- [ ] spec 中与 spec 同名的 instance/static method 均按普通方法处理；
- [ ] 从 spec 名称发起 References / Rename 不包含同名方法，从同名方法发起也不包含或重命名 spec；
- [ ] 隐式默认构造调用进入类型 References，但不产生虚构构造声明。

### 7.2 Rename 等价入口

使用同一 fixture 验证从以下入口 Rename：

- [ ] 类型声明；
- [ ] 普通类型引用；
- [ ] `int` 构造声明；
- [ ] `double` 构造声明；
- [ ] `int` 构造调用；
- [ ] `double` 构造调用；
- [ ] 终结器名称。

上述入口必须得到相同的 TextEdit 集合，覆盖类型、全部构造、全部调用及终结器；终结器 edit range
不得包含 `~`。

同时验证：

- [ ] Prepare Rename 在全部入口返回正确 range 和 placeholder；
- [ ] 新名称非法时继续拒绝；
- [ ] 外部只读 package 的构造或类型不允许 Rename；
- [ ] 任一必要 session stale 时不返回部分 edit；
- [ ] 精确成功 session 尚未发布时，不通过单文件 fallback 返回部分类型名称族 edit；
- [ ] edit 应用后的 fixture 可通过现有 parser / semantic 检查。

### 7.3 本地项目依赖回归

新增 hermetic 本地依赖项目 A 和消费项目 B：

- [ ] A 定义含多个构造重载和终结器的类型；
- [ ] B 的至少两个文件分别包含普通类型引用和不同构造调用；
- [ ] 从 A 的类型名查询，返回 A 与 B 中的完整类型名称族；
- [ ] 从 A 的具体构造查询，只返回该重载在 A 与 B 中的引用；
- [ ] 从 B 的具体构造调用查询，与 A 中对应构造查询结果一致；
- [ ] 从 A 类型、A 构造、B 调用或 A 终结器发起 Rename，得到相同的跨项目 WorkspaceEdit；
- [ ] 不相关本地项目中同 module/name 或同名类型不进入结果；
- [ ] 跨项目导入同名不同 generic arity 类型时，References / Rename 不跨 arity 串线；
- [ ] 跨项目普通重载方法及 spec 同名方法的 References / Rename 行为保持不变；
- [ ] 非本地 package 的 Hover、Completion、Definition 和 References 保持不变。

### 7.4 真实问题回归

- [ ] 从 `std/std/src/tui/common/Thickness.ff` 第 9 行类型名查询，结果包含
      `std/std_test/src/test_tui.ff` 中全部合法 `Thickness(...)` 调用；
- [ ] 从第 52 行单参数 `int` 构造查询，继续只包含解析到该重载的调用；
- [ ] 第 60 行 `double` 构造不误收 `int` 调用；
- [ ] 从类型名或任一构造入口 Rename 时，全部显式构造名称同步更新。

真实工程路径用于人工/协议验收；自动测试优先使用 hermetic fixture，避免依赖标准库源码行号变化。

### 7.5 回归与性能

- [ ] 运行新增定向用例；
- [ ] 运行既有全部 LSP 性能脚本，确认主性能规范全部通过；
- [ ] 在非 Codex 沙箱环境执行全量 `make test`；
- [ ] 确认生产代码 diff 仅位于 `src/cli/lsp/`；
- [ ] 确认未修改任何已有测试用例的断言或预期。

---

## 8. 分步实施 TODO

### Phase 1：规范确认

- [x] Review 并确认 §1 的 References / Rename 行为矩阵；
- [x] 确认类型查询在 `includeDeclaration == true` 时聚合构造和终结器声明；
- [x] 确认具体构造 References 保持重载级精度；
- [x] 确认同名不同 generic arity 的类型名称族严格隔离；
- [x] 确认普通方法、普通重载方法及 spec 同名方法严格排除在类型名称族之外；
- [x] 确认从构造、构造调用和终结器发起 Rename 时统一提升到 owner type；
- [x] 确认生产代码仅允许修改 `src/cli/lsp/`。

### Phase 2：References 最小实现

- [x] 增加带注释的非对称 target 匹配 helper；
- [x] 将调用 callee 引用匹配接入 helper；
- [x] 将构造及终结器声明引用匹配接入 helper；
- [x] 保持具体构造与终结器查询为精确成员身份；
- [x] 保持 textual prefilter、range 去重和取消检查不变。

### Phase 3：Rename 最小实现

- [x] 增加带注释的 Rename owner-type 规范化 helper；
- [x] Prepare Rename 在稳定目标构建前规范化类型名称族目标；
- [x] Rename 在稳定目标构建前规范化类型名称族目标；
- [x] current-parse fallback 识别并拒绝无法证明完整的类型名称族 Rename，禁止返回单文件部分 edit；
- [x] 复用既有全局 References、完整 session 检查和 WorkspaceEdit 构建；
- [x] 确认普通符号 Rename 行为零变化。

### Phase 4：新增测试

- [x] 增加 §7.1 References 语义覆盖；
- [x] 增加 §7.2 Rename 等价入口覆盖；
- [x] 增加 §7.3 本地项目依赖用例；
- [x] 完成 §7.4 `Thickness` 真实协议回归；
- [x] 不修改已有测试用例的断言或预期。

### Phase 5：验收

- [x] 执行新增定向测试；
- [x] 执行既有 LSP 性能回归；
- [x] 在非 Codex 沙箱执行全量 `make test`；
- [x] 检查生产变更范围和测试变更范围；
- [x] 更新本文档 TODO 状态和实测结果；
- [ ] 完成人工代码 Review。

### 8.1 实施与验收结果

生产实现只修改 `src/cli/lsp/service.c`。新增协议用例位于 `test/cli/test_cli.c`，使用两个 hermetic
本地项目验证类型宽 References、构造重载精确 References、类型名称族 Rename、终结器、
`includeDeclaration`、跨 session 稳定身份、generic arity 隔离，以及普通重载方法和 spec 同名方法
不串线。

真实工程协议复验结果：

| 查询位置 | 总结果 | `test_tui.ff` 结果 |
| --- | ---: | ---: |
| `Thickness.ff` 第 9 行类型名 | 29 | 13 |
| `Thickness.ff` 第 52 行 `int` 构造 | 15 | 13 |

类型查询已包含 `test_tui.ff` 的全部 13 个合法构造调用；具体构造查询仍保持原重载级结果。

性能实测：

- 常规 LSP 交互 P99 为 0.072ms；
- 1 万、10 万、100 万行矩阵交互 P99 / Max 为 0.387 / 0.439ms；
- 调度回归 Definition P95 为 0.240ms，queued cancellation 为 1.207ms；
- inferred callable 和 Completion 恢复专项回归通过；
- 缓存保留专项的 LSP 场景通过。现有脚本仍读取旧的 `build/<name>.fb` 路径，而当前 `pack` 输出到
  `build/pkg/<name>.fb`；直接运行会在启动 LSP 前终止。本次只在 `build/` 使用一次性路径适配器完成
  场景验证，未修改该脚本或任何范围外代码。

非 Codex 沙箱全量 `make test` 通过：UBSan 与普通优化两轮 CLI/LSP 用例均通过，smoke 91/91、
`std_test` 601/601、FCTS 923/923，perf constraints、增量构建、发布脚本和工具链测试全部通过。

---

## 9. 完成标准

- 从类型名发起 References 能获得普通类型引用、全部构造声明/调用及显式终结器声明；
- 从具体构造发起 References 仍只获得该重载及其调用；
- 从类型、构造声明、构造调用或终结器发起 Rename 得到相同的完整类型名称族 TextEdit；
- Rename 跨本地项目依赖完整生效，不返回部分 edit；
- 隐式构造和终结器运行时事件不产生虚构源码位置；
- 同名不同 owner、module 或 package 的符号不串线；
- 同 module 同名但 generic arity 不同的类型名称族互不产生 References 或 Rename edit；
- 普通方法、普通重载方法和 spec 同名方法的 References / Rename 行为零变化；
- Definition、Hover、Completion、Signature Help、Implementation 和非本地 package 能力零回归；
- 不修改 LSP 目录外的生产代码，不修改编译器接口、FT/FB、IR、Codegen、Runtime 或 DAP；
- 全部既有性能标准保持不变；
- 新增定向测试、LSP 性能回归及全量 `make test` 全部通过。
