# Feng spec 与泛型组合覆盖补强开发文档

> 状态：本轮覆盖已补强；完整合法矩阵仍被 generic fit `CE0031` 阻塞
>
> 本文只补测试证据，不定义或改变语言语义。`spec` 与泛型规则分别以
> [feng-spec.md](../specifications/feng-spec.md) 和
> [feng-generics-draft.md](../specifications/feng-generics-draft.md) 为准。

## 1. 目标与测试边界

本轮基于当前规范和实际测试代码，核对所有已经允许的 `spec` form、泛型声明入口、
约束、满足关系、值表示及包边界。覆盖以“非等价语义或 lowering 分支”为单位，不对
等价类型排列、泛参数量和嵌套深度做笛卡尔积枚举。

- 合法程序的语法、跨包恢复、witness 与可观察行为由 FCTS 验证；
- 必须被拒绝的声明、约束和满足关系由 Parser / Semantic compiler tests 验证；
- descriptor、slot、wrapper 和零额外运行时操作等结构事实由 Codegen tests 验证；
- 已有直接证据的等价路径只记录映射，不重复增加用例。

## 2. 本轮确认的三个 FCTS 直接空白

`spec` 显式 `open` 已与省略修饰符统一为公开 requirement，但实施前的 FCTS 只直接覆盖
非泛型 type。以下三个合法组合当时缺少直接行为证据：

1. 同包泛型 object-form `spec` 的显式 `open` requirement，由泛型 `type` 声明头满足；
2. 同包泛型 object-form `spec` 的显式 `open` requirement，由泛型 `fit` 满足；
3. 跨包导入泛型 object-form `spec` 后，由 consumer 中的泛型 `type` / `fit` 满足并经
   provider 共享泛型函数使用。

新增模型必须直接覆盖 object-form 成员的全部合法种类：实例 `let` / `var`、实例方法、
静态 `let` / `var`、静态方法；同时以泛型 owner 参数出现在字段、参数和返回类型中，
验证显式 `open` 与泛型替换共同生效。跨包模型还应通过泛型父 spec 继承 requirement，
验证 `.ft` 恢复后的 parent surface。

普通 FCTS 不承载预期编译失败的程序。因此，“显式 `open` requirement 不能由 `seal`
实现成员满足”必须在 Semantic test 中直接覆盖 type 与 fit；这不是遗漏 FCTS。

### 2.1 全部合法分支复核后补出的非 `open` 空白

进一步按泛型规范逐条反查 FCTS 后，确认 generic spec 声明自身的类型参数约束当时没有直接
行为用例。现有测试覆盖“泛型函数/type/method 参数受 spec 约束”和“无约束 generic spec”，
但不能替代以下合法声明事实：

- object-form generic spec 的 owner 参数带约束，并把更强约束参数传给较弱约束的 generic
  parent；同时覆盖两个 owner 参数，防止位置映射错误；
- callable/union/intersection-form generic spec 的 owner 参数带约束；
- 上述约束从 `.ft` 恢复后，consumer 的合法闭合、object witness、callable 调用、union
  收窄和 intersection witness 仍成立。

这些 form 已分别有无约束行为覆盖；本轮只增加“owner 参数约束”这一非等价语义分支，
不枚举等价的参数数量、类型排列和 subject 表示笛卡尔积。非法实参、约束过宽、arity 和
variance 继续由 Parser / Semantic tests 负责。

## 3. 当前合法能力覆盖核对

| 规范分支 | 当前直接证据 | 本轮动作 |
| --- | --- | --- |
| 泛型 object-form spec：字段、方法、type/fit 满足、父 spec 映射 | `test_generic_spec_implementation.ff`、`test_spec_borrowed_getter_assign.ff`、`test_spec_upcast.ff` | 保留；补显式 `open` 三条路径 |
| 泛型 object-form spec 静态 `let` / `var` / 方法及 generic constraint 访问 | 静态方法和固定类型字段已有覆盖；owner 参数类型的静态字段缺少直接证据 | 新 FCTS 暴露 address-return ABI 缺陷，按独立 bugfix 修复并补 Codegen 证据 |
| object-form spec 的 default / `seal` requirement | `test_generic_spec_implementation.ff` 及 spec seal compiler/FCTS 用例 | 保留；与新显式 `open` 共同覆盖三种声明事实 |
| 泛型 callable-form spec：赋值、显式闭合、参数/返回/字段、type/fit 方法值、同/跨包 | `test_generic_callable_value_reification.ff`、`test_generic_multi_parameter_callable_coverage.ff`、`test_value_method_capture.ff` | 已覆盖，无新增排列 |
| 泛型 union-form spec：约束、进入、嵌套、match 与不匹配拒绝 | `test_union.ff`、`test_nested_union.ff`、`test_generic_composition_coverage.ff` 及 Semantic tests | 已覆盖 |
| 泛型 intersection-form spec：约束、展平、合并 witness、冲突与缺项拒绝 | `test_intersection_generic.ff` 及 Semantic tests | 已覆盖 |
| 泛型 spec 作为字段、参数、返回、数组/tuple/callable/另一泛型实参 | `test_spec_borrowed_getter_assign.ff` 与四组 generic coverage hardening FCTS | 已覆盖 |
| 泛型父 spec：直接转发、具体化映射、强约束向弱约束传递、child 到 parent 视角 | `test_generic_spec_implementation.ff`、`test_generic_constraint_forwarding.ff`、`test_spec_upcast.ff` | 已覆盖；跨包显式 `open` 用例再验证 parent surface |
| generic type / fit，同包/跨包，managed / `@value` subject | `test_generic_spec_implementation.ff`、`test_generic_fit_subject.ff` | 已覆盖；显式 `open` 不重复值表示排列 |
| spec 泛型 identity、arity、不变性和非法约束 | Parser / Semantic tests | 已覆盖，继续由 compiler tests 负责 |
| generic spec owner 参数约束（object/callable/union/intersection） | `test_generic_spec_owner_constraint.ff` 直接覆盖；既有 func/type/method 泛参约束不等价 | 已补同包 object 多参数/父约束与跨包四种 form 行为 |
| object-form spec 方法级泛参 | 当前语义禁止；`test_object_spec_method_type_params_rejected` 覆盖 default/open/seal 与实例/static | 非合法正向场景，不增加 FCTS |
| generic fit 普通方法体依赖 owner 泛参且没有直接调用 | 已确认可触发 `CE0031`；增加直接调用同样失败 | 属于产品缺陷，由 [generic fit 成员 dependency 闭合修复](./feng-generic-fit-member-reifiable-dependency-bugfix.md) 跟踪；修复后补 Codegen 与 FCTS |
| generic spec 字段类型为 owner 泛参或其他 address-ABI 类型 | 新 FCTS 暴露字段开放类型 fallback、静态 getter/setter 及调用端稳定 ABI 缺口 | 由 [generic spec 字段 witness 与稳定 ABI 修复](./feng-generic-spec-field-witness-abi-bugfix.md) 跟踪 |
| 泛型参数经 child spec 约束访问继承成员 | 新复合写入用例确认成员类型仍可能停留在父 spec 的开放 owner 泛参 | 由 [泛型 spec 约束成员 surface 闭合修复](./feng-generic-spec-constrained-member-surface-bugfix.md) 跟踪 |
| generic union 直接以 owner 泛参作为 member 并进入共享泛型体 | 闭合实例已有覆盖；跨包 provider 的开放 `Choice<T>` 新用例触发 `CE0045` | 由 [generic union 直接泛参成员共享体修复](./feng-generic-union-direct-parameter-shared-body-bugfix.md) 跟踪 |
| 跨包 generic child spec 的默认 parent projection 多次具化 | 新 FCTS 使同一个 canonical GenericABI projection 被不同 `UserSpec *` 身份重复发码 | 由 [generic spec 默认 parent projection 去重修复](./feng-generic-spec-default-parent-projection-dedup-bugfix.md) 跟踪 |

已有四组系统性泛型覆盖及最终实现分支映射见：

- [Feng 泛型组合 FCTS 补强开发文档](./feng-generic-composition-fcts-hardening-dev.md)；
- [Feng 泛型高级组合 FCTS 补强开发文档](./feng-generic-advanced-composition-fcts-hardening-dev.md)；
- [Feng 泛型跨特性 FCTS 补强开发文档](./feng-generic-cross-feature-fcts-hardening-dev.md)；
- [Feng 泛型剩余结构性死角测试补强开发文档](./feng-generic-remaining-structural-coverage-hardening-dev.md)。

初始静态核对只发现“显式 `open` 三条正向路径”和“generic fit 成员 dependency 闭合
缺陷”。实施三条 FCTS 时又实际暴露了 generic spec 字段 witness/address ABI 缺陷，以及
canonical generic parent 默认 projection 的去重键缺陷；两项均已记录并纳入本轮正确性
修复，不通过删除字段、父 spec 或重复具化用例绕过。

## 4. 实施顺序

- [x] **[分析]** 按主规范核对 object/callable/union/intersection form、泛型声明入口、
  constraint、满足关系、承载位置、值表示和包边界；
- [x] **[文档变更]** 修正 `feng-spec.md` 静态成员小节中仍禁止显式 `open` 的陈旧描述；
- [x] **[FCTS 变更]** 增加同包 generic type + explicit-open generic spec 用例；
- [x] **[FCTS 变更]** 增加同包 generic fit + explicit-open generic spec 用例；
- [x] **[FCTS 变更]** 增加跨包恢复、consumer type/fit 满足和 provider 共享泛型调用用例；
- [x] **[Compiler test 变更]** 直接验证显式 `open` 泛型 requirement 不能由 type/fit 的
  `seal` 实现满足；
- [x] **[FCTS 变更]** 增加 generic spec owner 参数约束：同包 object 多参数与强到弱父约束，
  以及跨包 object/callable/union/intersection 四种 form；
- [x] **[实际变更]** 按独立 bugfix 文档修复 generic spec 字段 witness 与稳定 address ABI，
  并增加 getter/setter、open/closed owner 参数和继承 surface 的 Codegen 回归；
- [x] **[实际变更]** 按独立 bugfix 文档修复 constrained generic 参数访问继承 spec 成员时的
  声明者 surface 闭合；
- [x] **[实际变更]** 按独立 bugfix 文档修复 generic union 直接泛参 member 的共享体闭合；
- [x] **[实际变更]** 按独立 bugfix 文档修复 canonical generic parent 默认 projection 去重；
- [x] **[验证]** 复查新用例覆盖实例/静态字段与方法、`let`/`var`、owner 参数替换、
  parent surface、同包/跨包和 type/fit，且不重复值表示的等价排列；
- [x] **[专项回归]** 运行 Semantic、Codegen tests 与 FCTS；
- [x] **[全量回归]** 在非沙箱环境执行 `make test`；
- [x] **[文档变更]** 记录本轮结果，并明确 generic fit `CE0031` 仍阻塞完整合法矩阵收口。

## 5. 完成标准

1. 三条缺少的合法显式 `open` × 泛型组合均有 FCTS 直接证据；
2. 公开 requirement 与 `seal` 实现不兼容有显式 `open` 泛型 Semantic 负向证据；
3. 除已独立跟踪的 generic fit `CE0031` 外，其余当前合法 `spec` × 泛型非等价分支均能
   映射到现有直接测试；
4. 不改变语言规则、witness、descriptor、`.ft` 格式、runtime ABI 或运行时开销；
5. 专项与全量回归通过。

## 6. 实施与验证结果

- 三个显式 `open` 直接组合已加入 FCTS，并覆盖 object-form spec 的实例/静态字段与方法、
  `let`/`var`、type/fit、泛型 owner 替换、父 spec 与跨包 provider 共享体；
- 全矩阵复核另外补入 generic spec owner 参数约束，直接覆盖同包 object 多参数/父约束，
  以及跨包 object/callable/union/intersection 四种 form；
- 显式 `open` requirement 由 `seal` type/fit 成员满足的非法组合已由 Semantic test 拒绝；
- Semantic、Codegen 专项测试通过，FCTS `814/814` 通过；非沙箱全量 `make test` 通过；
- 当前唯一尚不能加入正向 FCTS 的合法分支，是 generic fit 普通成员实现体依赖 owner 泛参
  时的 `CE0031`。其通用修复需要先完成人工确认，不通过删减或注释合法用例绕过。
