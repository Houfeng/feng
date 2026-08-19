# Feng 泛型 owner 普通方法依赖闭合修复开发文档

> 状态：已实施并通过全量回归（2026-08-19）
>
> 本文最初由 object-form `spec` witness 调用失败引出。实际代码与隔离用例已经
> 证明：根因不在 `spec` 或 witness，而在 closed generic `type` 实例没有主动预登记
> 普通成员方法的 reifiable dependencies。`spec` witness 只是没有像直接方法调用那样
> 偶然补齐该缺口。

## 1. 问题结论

本文只修复既有具化管线的登记遗漏，不重新定义 descriptor 或 witness 语义；相关既有
设计继续以
[Feng 泛型共享体具化修复开发文档](./feng-generic-shared-body-reification-bugfix-dev.md)
和
[Feng 泛型 `spec` 满足关系及跨包实现符号修复开发文档](./feng-generic-spec-implementation-package-bugfix.md)
为准。

当前编译器对泛型 `type` 的 reifiable dependency 已经按用途分为两类：

- 字段、字段初始化器、构造器、终结器等依赖记录在 type declaration dep set；
- 普通成员方法（包括实例方法和静态方法）的依赖记录在独立的 member dep set。

当 `A<T>` 被闭合为 `A<int>` 时，Codegen 当前会：

1. 建立并发布 `A<int>` 的 `UserType` shell；
2. 收集替换后的成员签名所引用的泛型实例；
3. 关闭并登记 type declaration dep set；
4. 之后为 `A<int>` 的普通方法生成 closed wrapper 和
   `FengFunctionDescriptor`。

缺失的是：在第 4 步以前，Codegen 没有关闭并登记普通方法各自的 member dep set。
因此，只要某个方法自身没有泛参的成员方法在实现体中依赖 `Box<T>`，生成
`A<int>.method` 的 closed function descriptor 时就可能找不到 `Box<int>`，报告
`CE0031`。

直接调用 `A<int>.method(...)` 当前能够通过，是因为调用表达式收集阶段会另外查找该
方法的 member dep set，并提前登记 `Box<int>`。这只是另一条链路偶然掩盖了缺口；
只构造 `A<int>` 而不直接调用该方法、通过 `spec` witness 调用，或者通过其他不含该
直接调用表达式的合法路径，都会暴露相同问题。

所以，本次通用修复应当是：

> closed generic `type` shell 在 owner 闭合时，主动关闭并预登记每个“方法自身没有
> 泛参”的普通成员方法 dep set，保证后续任何 closed wrapper 使用者都能取得同一棵
> 已闭合 descriptor tree。

本次不得在 `spec`、witness 或具体成员处增加特判。

## 2. 实际代码现状

### 2.1 Semantic 已经按正确归属记录依赖

`src/semantic/reifiable_deps.c` 的 `collect_for_type()` 当前行为是：

- 泛型 type 的字段、初始化器、构造器和终结器使用 declaration dep set；
- 只要 owner 有类型参数，或者方法自身有类型参数，普通
  `FENG_TYPE_MEMBER_METHOD` 就使用
  `feng_semantic_get_or_create_member_reifiable_dep_set()` 建立 member dep set；
- `collect_from_callable()` 已经把方法体中的 `Box<T>` 等直接依赖记录到该集合。

因此，本问题不需要改变 Parser、AST、Semantic 依赖发现规则或 dep set 数据结构。

### 2.2 generic type shell 漏掉了 member dep set

`src/codegen/codegen.c` 的 `cg_register_generic_type_instance_shell()` 当前已经：

- 先把 generic `UserType` instance shell 放入 `cg->user_types`，符合 shell-first；
- 通过 `cg_collect_instantiated_callable_member_instances()` 收集替换后成员签名中的
  泛型类型；
- 通过 `cg_collect_closed_reifiable_dep_instances()` 关闭 declaration dep set。

但函数最后只调用：

```text
feng_semantic_lookup_reifiable_dep_set(analysis, type_decl)
```

没有逐个调用：

```text
feng_semantic_lookup_member_reifiable_dep_set(
    analysis, type_decl, member)
```

所以成员签名中的泛型实例会被登记，成员实现体仅通过 member dep set 记录的闭合实例
不会在这里登记。

### 2.3 closed method wrapper 是依赖的正确使用者

`cg_emit_generic_type_method_wrapper_into()` 对以下组合已有明确处理：

- owner 已完全闭合，即 `generic_context_type_param_count == 0`；
- 普通成员方法；
- 方法自身 `type_param_count == 0`。

该分支查找方法的 member dep set，并调用 `cg_emit_closed_callable_fdesc()` 生成 closed
`FengFunctionDescriptor`。owner 的 `T = int` 仍由 `A<int>` 的 type descriptor
提供；方法描述符只承载 `Box<int>` 等 callable-local dependencies。

这条 wrapper/descriptor 生成逻辑方向正确。它的前置条件是 `Box<int>` 等实例已经由
实例收集阶段登记；当前缺失的正是该前置登记，而不是 descriptor 的表示或传参能力。

### 2.4 直接调用为什么会掩盖问题

`cg_collect_resolved_call_reifiable_dep_instances()` 当前会处理解析为
`FENG_RESOLVED_CALLABLE_TYPE_METHOD` 或
`FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD` 的直接调用：

1. 找到声明 owner 与具体 owner instance；
2. 合并 owner 类型实参与方法类型实参；
3. 查找该实现成员的 member dep set；
4. 调用 `cg_collect_closed_reifiable_dep_instances()` 递归登记依赖。

因此，源代码中出现 `A<int>.method(...)` 时，调用点先补齐了 `Box<int>`，随后 wrapper
发码不再失败。没有该直接调用表达式时，generic type instance 注册链路自身无法保证
wrapper 的依赖已登记。

### 2.5 `spec` witness 不是修复点

object-form `spec` 调用在源码收集阶段只知道 requirement 身份，不应该静态选择
`A<T>` 或 `B<T>` 的具体实现依赖树。运行时 witness 负责选择已经确定的实现 wrapper；
当前 witness thunk 也已经调用 closed implementation wrapper。

例如：

```feng
let surface: Surface<int> = A<int>();
surface.identity(42);
```

witness 只需路由到 `A<int>.identity` wrapper；wrapper 再静态传入 `A<int>` 的 type
descriptor 和 `A<int>.identity` 的 function descriptor。只要 generic type instance
注册阶段已预登记该 wrapper 的依赖，现有链路即可工作。

因此本次不需要：

- 给 witness 增加 dependency slot；
- 让 witness thunk 动态构造或查找 descriptor；
- 改变 witness、`FengTypeDescriptor` 或 `FengFunctionDescriptor` 的 ABI；
- 让 requirement dep set 代替 implementation member dep set；
- 为 object-form `spec` 调用增加运行时分支、缓存或 resolver。

### 2.6 隔离验证结果

基于当前代码进行的最小隔离验证结果如下：

| 场景 | 当前结果 | 说明 |
|---|---|---|
| 泛型 `type A<T>`，只构造 `A<int>`，普通实例方法体使用 `Box<T>` | `CE0031` | 不含 `spec`，证明根因与 witness 无关 |
| 上述场景增加直接调用 `a.identity(42)` | 通过 | 直接调用表达式提前登记 member dependencies |
| `type A<T>: Surface<T>`，经 `Surface<int>` witness 调用 | `CE0031` | witness 路径暴露同一缺口 |
| 泛型 managed type 的普通静态方法体使用 `Box<T>` | `CE0031` | 静态方法属于同一 member dep 归属 |
| 泛型 `@value type` 的普通方法体使用泛型依赖 | `CE0031` | value type 也复用 generic `UserType` shell 路径 |

这组结果说明，本次实际代码变更不应以“是否参与 spec 满足”或“是否由 witness 调用”
为条件；只要 closed generic type 会生成该普通方法的 closed wrapper，就必须先登记其
member dependencies。

## 3. 修复范围

### 3.1 本次实际修复

本次只修复 generic `type` instance 注册遗漏，覆盖：

- managed generic type；
- `@value` generic type；
- 普通实例方法；
- 普通静态方法；
- owner 已完全闭合；
- 方法自身没有方法级类型参数。

修复后，下列两类入口必须得到相同结果：

- 仅形成 `A<int>`，不依赖直接方法调用表达式触发补登记；
- 通过直接调用、`spec` witness 或其他既有合法链路使用
  `A<int>.method` closed wrapper。

### 3.2 保持现状的路径

- 字段、初始化器、构造器和终结器继续使用 declaration dep set；
- 方法自身声明 `<U>` 时，依赖继续在具体调用点以 owner 实参和方法实参共同关闭；
- owner 仍含开放上下文参数时，继续使用现有开放 wrapper/descriptor 传递路径；
- 直接调用的依赖收集继续保留，它仍负责方法级泛参以及独立调用需求；
- `.ft` 已经通过 `fill_reifiable_deps()` 与
  `restore_imported_reifiable_deps()` 序列化、恢复成员 dep set，本次不增加新的符号格式
  或跨包依赖链路；
- `spec` 满足、witness 选择和 thunk 发码保持现状。

### 3.3 不纳入本次的独立问题

以下问题不属于本次 generic type shell 修复：

- object-form `spec` requirement 声明方法级泛参；该限制与后续方向由
  [Feng object-form `spec` 方法级泛型限制备注](./feng-object-form-spec-method-generic-restriction-note.md)
  跟踪；
- object-form `spec` 方法值；
- descriptor 提升或其他性能优化；
- 泛型推导、满足关系、可见性或成员导出规则变更；
- runtime 私有 ABI 变更。

## 4. 泛型 `fit` 的实际边界

隔离验证还发现：

```feng
type Host<T> {}

fit Host<T> {
  func identity(value: T): T {
    let box = Box<T>(value);
    return box.value;
  }
}

let host = Host<int>();
```

当前同样报告 `CE0031`，即使没有调用 `identity`。但它不能直接并入本次代码改动：

- `collect_for_fit()` 当前把一个 fit 的全部成员依赖收集到 fit declaration dep set；
- 该集合可能同时包含 owner 参数 `T` 与某些 fit 方法自己的方法级参数 `U`；
- `cg_register_user_fit_shell_for_target()` 只收集替换后的成员签名；
- `cg_emit_closed_generic_fit_descriptors()` 之后按 owner 参数关闭 fit 级集合。

如果直接在 closed `Host<int>` 注册时关闭整个 fit dep set，含 `U` 的依赖并没有具体
方法调用实参，不能正确关闭。泛型 fit 因此涉及“依赖集合的参数域与 descriptor 归属”
问题，不是 generic type 已有 member dep set 的简单漏用。

本次不得通过跳过未闭合项、按参数名判断或只处理 `T` 等特判绕过。泛型 fit 应另立
专项，在确认其 member-level dependency 归属方案后修复；本文件只记录这个已确认边界，
不把它列为本次实施 TODO。

## 5. 实施方案

### 5.1 在现有 generic type shell 链路补齐预登记

在 `cg_register_generic_type_instance_shell()` 已发布 `UserType` shell 之后、任何 closed
method descriptor 发码之前，复用现有成员遍历和
`cg_collect_closed_reifiable_dep_instances()`：

1. 确认当前 owner instance 已完全闭合；
2. 遍历 `FENG_TYPE_MEMBER_METHOD`；
3. 对方法自身 `type_param_count == 0` 的成员，使用
   `feng_semantic_lookup_member_reifiable_dep_set()` 取得既有集合；
4. 使用原 owner 类型参数和当前 closed type args 关闭该集合；
5. 让现有递归收集器登记 `Box<int>` 等直接及传递 generic instances。

该改动与当前 declaration dep set 收集处于同一 generic instance shell 注册阶段，不
增加新的 collector、缓存或旁路入口。

### 5.2 条件必须与 closed wrapper 消费条件一致

预登记范围应与 `cg_emit_generic_type_method_wrapper_into()` 静态生成 closed callable
descriptor 的条件一致：

- owner 完全闭合；
- 成员是普通方法；
- 方法自身没有方法级泛参。

不得在 owner 仍开放或方法仍有 `<U>` 时尝试提前关闭；这些信息应继续由最终合法调用点
提供并通过现有调用收集链路关闭。

### 5.3 保持 shell-first 与依赖方向

必须先发布 `A<int>` shell，再递归登记 `A<int>.method` 的 `Box<int>` 等依赖。递归
登记过程中不得依赖可能因 `cg->user_types` 扩容而失效的 `UserType *` 地址；应继续使用
稳定的 origin declaration、type args 和既有查找/注册抽象。

修复只改变编译期预登记时机，不改变生成 C 的调用层次：

```text
spec witness thunk（如有）
  -> A<int>.method closed wrapper
  -> A<T>.method shared body(
       A<int> type descriptor,
       A<int>.method function descriptor,
       ...)
```

## 6. 测试要求

### 6.1 Compiler tests

在 `test/codegen/test_codegen.c` 增加回归，至少验证：

- 只形成 closed generic type、没有直接调用该成员时，不再报告 `CE0031`；
- 普通实例方法与普通静态方法；
- managed type 与 `@value type`；
- `Box<int>` 等依赖进入对应成员的 closed `FengFunctionDescriptor`；
- owner 参数仍从 type descriptor 获取；
- witness thunk 继续引用同一个 closed implementation wrapper；
- 没有新增 runtime helper、descriptor 参数或 witness slot；
- 方法级泛参 `<U>` 仍由调用点关闭，现有行为不受影响。

### 6.2 FCTS

在 `fcts/` 增加语言行为用例，至少覆盖：

- 无 `spec` 的 generic type，方法体包含 owner-dependent 泛型依赖；
- 同一 `Surface<int>` 分别承载 `A<int>` 与 `B<int>`，两个实现使用不同的依赖类型，
  witness 调用分别返回正确结果；
- 普通静态方法；
- generic `@value type`；
- 同包路径；
- provider 定义 generic type/spec、consumer 只经 package surface 使用的跨包路径。

泛型 fit 不作为本次通过标准，避免把独立架构问题混入；其失败用例应在后续专项中
落地。

### 6.3 回归与性能边界

- 运行相关 compiler tests；
- 运行相关 FCTS；
- 检查生成 C 与 descriptor/witness struct 布局没有 ABI 变化；
- 检查成功路径没有新增运行时分支、查找、缓存、分配或 helper 调用；
- 最终在非沙箱环境执行全量 `make test`。

## 7. TODO

TODO 按“先锁定通用闭合点，再实施最小改动，最后分层验证”的顺序执行：

- [x] **[分析]** 核对 Semantic dependency 归属，确认普通 generic owner 方法已有独立
  member dep set；
- [x] **[分析]** 核对 generic type shell、直接调用收集、closed wrapper 和 witness
  thunk 链路，确认缺口在 generic type instance 的 member dep 预登记；
- [x] **[分析]** 用无 `spec`、直接调用、witness、静态方法、`@value type` 探针确认
  根因与覆盖边界；
- [x] **[分析]** 核对泛型 fit 的依赖表示，确认它是不同的参数域/descriptor 归属问题，
  不纳入本次实施；
- [x] **[实际变更]** 在 `cg_register_generic_type_instance_shell()` 的既有 shell-first
  链路中，为 fully closed owner 中方法自身没有泛参的普通成员逐一关闭并预登记
  member dep set；
- [x] **[验证]** 确认实际变更只复用
  `feng_semantic_lookup_member_reifiable_dep_set()` 与
  `cg_collect_closed_reifiable_dep_instances()`，没有新增 spec/witness 特判、collector、
  runtime helper 或 ABI；
- [x] **[验证]** 确认 owner 开放实例和方法级泛参成员没有被提前关闭，继续走既有调用点
  闭合链路；
- [x] **[测试变更]** 在 `test/codegen/test_codegen.c` 增加无直接调用、实例/静态、
  managed/value、witness 与生成 C 结构回归；
- [x] **[测试变更]** 在 `fcts/` 增加无 `spec`、两个 witness 实现、同包与跨包的行为
  回归；
- [x] **[验证]** 复查 `.ft` 的 member reifiable dependency 写入/恢复继续复用现有链路，
  不新增符号格式；
- [x] **[验证]** 检查 diff，不包含泛型 fit、object-form spec 方法级泛参、方法值、性能
  优化或其他无关变更；
- [x] **[回归]** 运行相关 compiler tests 与 FCTS；
- [x] **[全量回归]** 在非沙箱环境执行 `make test`。

## 8. 完成标准

本专项仅在以下条件全部满足时完成：

1. closed generic type 不再依赖源码中存在直接成员调用，便可完整登记其中方法自身没有
   泛参的普通成员 callable dependencies；
2. 无 `spec` 路径与 object-form `spec` witness 路径都不再出现该 `CE0031`；
3. 实例/静态、managed/value、同包/跨包用例通过；
4. 方法级泛参、开放 owner、既有直接调用、满足关系和可见性行为不变；
5. 没有 spec/witness 特判，没有新依赖收集链路，没有运行时开销或 ABI 变化；
6. 泛型 fit 独立问题未被旁路或混入本次改动；
7. 全量 `make test` 通过。

## 9. 实施记录

- 2026-08-19：新增 Codegen 回归时，最初按固定生成文本断言方法体必须读取
  `_type_desc->reified_generic_params[...]`。隔离检查确认，用例中的方法只通过已经闭合的
  member dependency 操作 `T`，共享体虽然继续接收 owner type descriptor，但没有语义
  操作要求读取其中的 generic parameter slot，因此 Codegen 正确地没有生成该读取。
  本专项改为验证既有 `_type_desc + FengFunctionDescriptor` 共享方法 ABI；需要实际读取
  owner generic parameter slot 的行为继续由既有 generic type owner reification 用例
  覆盖。这是测试断言边界修正，不是编译器缺陷，也不需要产品代码变更。
- 2026-08-19：新增跨包 FCTS 时，provider 的 open `spec` 成员最初冗余声明了
  `open`，在当时的 Parser 规则下触发诊断 `SE0601`；测试随即按当时语义移除了
  冗余修饰符。后续 object-form `spec` 已允许显式 `open`，当前规则统一见
  [Feng 语言 `spec` 规范](../specifications/feng-spec.md)；本条仅保留当时的实施记录。
- 2026-08-19：定向 `test_codegen` 通过；FCTS 新增 4 组行为测试后共 805/805 通过；
  非沙箱全量 `make test` 的 sanitize 与 normal 两阶段全部通过，其中 smoke 91/91、
  std 579/579、FCTS 805/805，性能约束、增量构建、发布与工具链检查均通过。
