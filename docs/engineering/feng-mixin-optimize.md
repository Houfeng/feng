# Feng mixin `@mixable` 目标显式优先优化开发设计

> **状态**：已实施并完成全量回归。
>
> **日期**：2026-08-09。
>
> **范围**：成员展开时，mix 来源 `@mixable` 静态方法候选与 mix 目标显式
> `@mixable` 静态方法之间的跳过判断。
>
> **关联规范**：[Feng 语言函数规范](../specifications/feng-function.md) 负责定义
> `@mixable`、静态 wrapper 与实例 wrapper 的权威语言语义；本文在 Review 通过前只记录
> 待确认的优化方案和实施步骤。
>
> **关联开发**：[泛型重载决议优化开发设计](./feng-generic-overload-resolution-optimize-dev.md)
> 计划放开调用侧可消歧的 `ChildSpec` / `ParentSpec` 等重载。本文应先于该重载优化实施，
> 使 `@mixable` 的目标显式优先不依赖当前较严格的普通静态重载冲突规则。

## 1. 背景与当前行为

一个 `@mixable` 静态方法在具体类型中保留后，会派生一个删除首参数的普通实例
wrapper：

```feng
@mixable
static func draw(target: Widget, manager: ViewManager): void {
}
```

概念上派生为：

```feng
func draw(manager: ViewManager): void {
  CurrentType.draw(self, manager);
}
```

成员展开把 mix 来源的 `@mixable` 静态方法复制为目标静态 wrapper。目标显式静态成员
当前按完整静态签名参与候选冲突检查；发生冲突时，目标显式成员优先，来源静态 wrapper
候选不生成。不同 mix 来源之间没有来源顺序或隐式优先级。

当前普通重载实现尚未统一允许 `ChildSpec` / `ParentSpec` 参数重载：

- 没有可见共同具体满足类型时，父子 `spec` 参数重载可能通过；
- 存在同时满足两者的可见具体类型时，当前定义检查报告 `AE0706`。

在 mix 场景中，目标类型必须按名义关系满足来源 `@mixable` 的首参数 `spec`；如果目标
又显式声明以自身子 `spec` 为首参数的同名 `@mixable`，该目标类型本身就是两个首参数
`spec` 的共同具体满足类型。因此，当前完整静态签名检查会把两者判为潜在重叠，并跳过
来源候选：

```feng
spec ButtonWidget: Widget {
  let text: string;
}

type Button: ButtonWidget {
  ...: View;

  let text: string;

  @mixable
  static func draw(target: ButtonWidget,
                   manager: ViewManager): void {
    View.draw(target, manager);
    // Button 自身绘制。
  }
}
```

当前结果是只保留 `Button.draw(ButtonWidget, ViewManager)`，并只生成一个实例
`Button.draw(ViewManager)`。该结果正确，但依赖当前普通静态重载仍拒绝存在共同具体满足
类型的父子 `spec` 参数重载。

## 2. 问题

后续重载优化计划允许调用侧能够消歧的父子 `spec` 静态重载：

```text
View.draw(Widget, ViewManager)
Button.draw(ButtonWidget, ViewManager)
```

这两个完整静态签名可以通过首参数区分。然而 `@mixable` 派生实例 wrapper 时固定删除
首参数，二者的实例投影完全相同：

```text
draw(ViewManager)
draw(ViewManager)
```

如果来源候选是否跳过仍然只复用普通静态重载声明规则，重载优化实施后两个静态方法会
同时保留，随后生成两个同签名实例 wrapper。调用方无法在实例调用
`value.draw(manager)` 中提供已经删除的首参数进行消歧，因此这不是可留到调用点解决的
普通重载重叠。

本问题只影响 mix 来源候选与 mix 目标显式 `@mixable` 的优先判断。实例 wrapper 已经是
普通实例成员；未被目标显式方法解决的多来源冲突继续由现有实例成员冲突检查报告即可。

## 3. 已确认方案

### 3.1 实例投影

对一个合法 `@mixable` 静态方法 `M`，定义其实例投影 `P(M)`：

1. 保留方法名；
2. 保留方法泛型参数及约束；
3. 删除第一个 object-form `spec` 参数；
4. 保留其余参数的顺序、类型和变长标记；
5. 保留返回类型和可见性等实例 wrapper 已经保留的声明事实。

两个实例投影是否冲突，不另行发明签名规则，而是假设二者已经生成普通实例方法，再按
普通实例成员的声明合法性规则判断。返回类型不能单独区分重载；泛型、变长参数及其他
签名事实继续遵循普通实例方法规则。

### 3.2 来源候选跳过规则

把一个 mix 来源 `@mixable` 静态方法作为静态 wrapper 候选加入目标时，继续执行现有的
完整静态成员冲突检查，并额外执行以下检查：

1. 只检查目标类型中显式声明的静态成员，不把其他 mix 来源生成的成员视为目标显式
   成员；
2. 只有目标显式静态方法也标注 `@mixable` 时，才执行实例投影检查；
3. 如果目标显式 `@mixable` 与来源候选的实例投影按普通实例方法规则构成非法冲突，
   则目标显式成员优先，来源静态 wrapper 候选不生成；
4. 如果删除首参数后的实例投影可以构成合法实例重载，则不因本规则跳过来源候选；
5. 目标显式非 `@mixable` 静态方法不会生成实例 wrapper，不参与新增的实例投影检查，
   但仍参与现有完整静态成员冲突检查。

概念判断为：

```text
现有完整静态成员检查判定冲突
或
目标显式成员也是 @mixable，且双方实例投影构成非法实例成员冲突
    => 跳过来源静态 wrapper 候选
```

新增检查只稳定 `@mixable` 从静态方法到实例方法的固有签名投影，不改变普通静态方法
能否重载，也不参与调用点候选排序。

### 3.3 多来源保持现状

不同 mix 来源之间继续没有优先级，也不增加来源之间的投影预筛选：

```feng
type Combined: CombinedWidget {
  ...: LeftSource;
  ...: RightSource;
}
```

如果两个来源最终生成同签名实例 wrapper，继续在定义处按普通实例方法规则报冲突。
编译器不得按来源声明顺序选择一个，也不得静默去重。

目标可以通过显式 `@mixable` 同时解决多个来源冲突：

```feng
type Combined: CombinedWidget {
  ...: LeftSource;
  ...: RightSource;

  @mixable
  static func draw(target: CombinedWidget,
                   manager: ViewManager): void {
    LeftSource.draw(target, manager);
    RightSource.draw(target, manager);
  }
}
```

每个来源候选分别与目标显式 `Combined.draw` 检查；实例投影冲突的来源候选均被跳过。
目标可以完整替换来源行为、只调用一个来源，或按明确顺序组合多个来源。

如果多个来源删除首参数后的实例签名不同，则继续形成普通实例重载：

```text
LeftSource.draw(LeftWidget, int)       -> draw(int)
RightSource.draw(RightWidget, string) -> draw(string)
```

这种情况不构成实例投影冲突，应合法保留。

### 3.4 显式实例方法保持现状

目标显式声明普通实例方法，不参与来源静态 wrapper 的目标显式跳过规则：

```feng
type Button: ButtonWidget {
  ...: View;

  func draw(manager: ViewManager): void {
  }
}
```

来源 `@mixable` 静态方法仍会生成实例 wrapper；如果它与手写实例方法冲突，继续由普通
实例成员冲突检查报错。需要显式替换或组合来源 `@mixable` 行为时，目标应显式声明
`@mixable` 静态方法，并通过 `Source.method(target, ...)` 调用来源逻辑。

## 4. 与重载优化的边界

普通函数、普通静态方法和普通实例方法是否允许 `ChildSpec` / `ParentSpec` 参数重载，
继续由重载规范统一决定。本文不改变以下行为：

- 重载候选适用性；
- 调用点匹配优先级；
- 多参数比较顺序；
- 父级 `spec` 距离是否参与比较；
- 调用点二义性诊断；
- 必然死角和动态死角的一般定义。

`@mixable` 的实例投影检查发生在来源静态 wrapper 候选进入目标普通成员表之前，只决定
目标显式 `@mixable` 是否使该来源候选跳过。普通父子 `spec` 静态重载即使以后合法，
也不能据此保留两个投影后必然冲突的实例 wrapper。

[泛型重载决议优化开发设计](./feng-generic-overload-resolution-optimize-dev.md) 当前关于
`@mixable` 的以下结论需要在本文 Review 通过后调整：

- 不能再把“只有普通完整静态声明规则禁止的冲突才跳过来源 wrapper”作为完整规则；
- 应在普通完整静态成员检查之外，引用本文定义的实例投影冲突检查；
- `@mixable` 生成后的静态与实例成员仍然复用普通重载规则，只有生成前的目标显式优先
  需要这项投影检查。

## 5. 实现范围

### 5.1 Semantic

修改现有来源静态 wrapper 候选筛选路径：

```c
target_explicit_member_conflicts_with_mixin_static_wrapper(...)
```

在现有完整静态成员冲突判断之外，增加一个实例投影冲突判断。实现应复用普通实例方法
声明冲突所使用的类型相等、泛型、变长参数和重叠判断基础能力；可以通过参数起始偏移
比较，或构造只读的投影视图完成，不需要创建临时 AST wrapper。

新增判断必须满足：

- 来源参数从索引 `1` 开始参与实例投影；
- 目标显式 `@mixable` 参数同样从索引 `1` 开始；
- 方法名、泛型形状及剩余参数共同决定实例投影；
- 只扫描目标显式静态成员；
- 命中后只丢弃当前来源候选，不修改目标显式成员；
- 每个来源候选独立检查，因此一个目标显式 `@mixable` 可以跳过多个冲突来源。

### 5.2 不修改的实现

本次不修改：

- `create_mixin_static_wrapper(...)`；
- `create_mixin_instance_wrapper(...)`；
- 为每个最终保留 `@mixable` 派生实例 wrapper 的流程；
- 不同来源 wrapper 的收集和顺序；
- 普通静态方法与实例方法重载决议；
- object-form `spec` coercion 和 witness；
- Codegen、`.ft` 格式、runtime ABI 或对象布局。

来源方法继续通过 `Source.method(target, ...)` 完整限定调用，不增加运行时查找、分支、
wrapper 层数或间接分派。

## 6. 正确性场景

实施测试至少覆盖：

1. `Widget` 来源方法与 `ButtonWidget: Widget` 目标显式方法具有相同实例投影，只保留
   目标显式静态方法并只生成一个实例 wrapper；
2. `View -> Button -> IconButton` 多层展开，每层使用自身子 `spec` 作为首参数并通过
   `Source.draw(target, ...)` 叠加行为；
3. 两个来源具有相同实例投影且目标没有显式决策，定义处继续报实例成员冲突；
4. 两个来源具有相同实例投影，目标显式 `@mixable` 同时跳过两个来源并按明确顺序调用
   来源行为；
5. 两个来源删除首参数后的剩余参数不同，继续形成合法实例重载；
6. 目标显式非 `@mixable` 静态重载不因实例投影规则跳过来源候选；
7. 目标显式普通实例方法与来源生成实例 wrapper 冲突时，继续按现有规则报错；
8. 泛型方法和变长参数的实例投影使用普通实例成员声明规则；
9. 同包、跨源码模块与跨二进制包来源行为一致；
10. 普通 `ChildSpec` / `ParentSpec` 静态重载规则不被本次改动改变。

Compiler tests 负责检查展开后的成员数量、来源关系、冲突诊断和 Semantic 目标；FCTS
负责检查实例动态调用、完整限定来源调用及多层/多来源最终行为。修改任何既有测试前需
再次取得人工批准；新增 FCTS 行为用例应写入 `fcts/fcts_bin`，跨包定义按需写入
`fcts/fcts_lib`，并使用 `std.test`。

## 7. 性能与兼容性

新增工作只发生在 Semantic 的 mix 来源候选筛选阶段。目标显式成员扫描本来已经存在；
实例投影比较只增加编译期签名检查，不增加运行时数据、调用、分支、分配或间接访问。

按当前重载实现，父子首参数且实例投影冲突的来源候选已经通常会被完整静态重叠检查
跳过，因此本优化预期不改变现有合法程序的运行行为。它把当前依赖普通静态重载政策的
结果收敛为独立、稳定的 `@mixable` 生成规则，为后续放开普通父子 `spec` 重载建立前置
保障。

## 8. 实施步骤与 TODO

- [x] 人工 Review 并确认本文方案。
- [x] 更新 [Feng 语言函数规范](../specifications/feng-function.md)，收敛实例投影和目标
  显式优先的权威语义。
- [x] 更新
  [泛型重载决议优化开发设计](./feng-generic-overload-resolution-optimize-dev.md)，引用新的
  `@mixable` 投影规则，删除与本文不一致的候选跳过描述。
- [x] 在 Semantic 来源静态 wrapper 候选筛选中增加实例投影冲突检查。
- [x] 经人工批准后增加 compiler tests 与 FCTS，不修改未获批准的既有测试。
- [x] 运行专项测试。
- [x] 在沙箱外运行全量回归 `make test`。

## 9. 验收标准

- 目标显式 `@mixable` 能按实例投影跳过一个或多个冲突来源候选；
- 普通静态重载是否合法不再决定同签名实例 wrapper 是否被错误地同时生成；
- 多来源无显式目标决策时继续报冲突，不增加来源优先级或静默去重；
- 实例投影不同的来源继续形成合法实例重载；
- 实例 wrapper、普通重载、Codegen、witness、runtime 和 ABI 均无专项改动；
- 同包、跨模块与跨包结果一致；
- 专项测试与全量回归全部通过。
