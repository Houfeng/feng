# Feng 泛型 fit 成员 reifiable dependency 闭合修复开发文档

> 状态：待 Review，尚未实施
>
> 本文只记录 generic fit 普通成员 dependency 的既有正确性缺陷及通用修复边界，
> 不改变泛型、fit 或 spec 的语言语义。

## 1. 问题

以下合法代码只形成 `Host<int>`，没有直接调用 fit 方法：

```feng
type Box<T> {
  let value: T;
}

type Host<T> {}

fit Host<T> {
  func identity(value: T): T {
    let box = Box<T> { value: value };
    return box.value;
  }
}

let host = Host<int>();
```

Semantic 能确定 `Host<int>` 是闭合实例，但 Codegen 仍可能报告：

```text
CE0031: codegen: generic type/spec instance 'Box<int>' was not registered
```

实际代码验证表明，即使增加 `host.identity(...)` 直接调用，当前仍报告同一个 `CE0031`。
原因是 generic-instance 调用点的公共预登记函数当前只处理顶层函数和 type 成员，没有处理
fit 成员；因此该缺陷不是“缺少直接调用”的偶发现象。泛型 owner 的闭合本来也不能依赖
源码中是否恰好存在直接调用；witness、方法值或 wrapper 都可能消费同一个成员实现。

## 2. 已确认根因

### 2.1 Semantic 把不同方法域合并到 fit declaration dep set

`collect_for_type()` 已把普通方法依赖记录为 `(owner type decl, owner member)` 的 member
dep set；字段、构造器和终结器才使用 type declaration dep set。

`collect_for_fit()` 当前却为整个 fit 只创建一个 declaration dep set，再把所有方法体和
签名依赖合并进去。一个 fit 因此可能同时产生：

```text
identity(value: T)  -> Box<T>
wrap<U>(value: U)   -> Cell<U>
combine<U>(...)     -> Pair<T, U>
```

`T` 在 `Host<int>` 闭合时已经确定，`U` 只在具体方法调用点确定。合并集合没有单一合法
闭合时刻，也不能由一个只描述 owner 实参的静态 descriptor 正确承载。

### 2.2 Generic fit shell 没有登记普通成员的闭合实现依赖

`cg_register_generic_type_instance_shell()` 已在 fully closed type shell 上逐成员关闭没有
方法泛参的普通方法 dep set；`cg_register_user_fit_shell_for_target()` 当前只收集替换后的
成员签名，没有登记方法实现体依赖。因此只形成 closed generic fit target 时，`Box<int>`
没有进入现有 generic instance registry。

同时，`cg_collect_resolved_call_reifiable_dep_instances()` 目前没有 fit method/static method
分支，所以增加直接调用也不会补上这一步。这两个入口应复用同一个 member dep set 和同一个
closed-dependency helper，不能分别建立 fit 专用闭合规则。

### 2.3 现有 fit descriptor 归属也是 declaration 级

`cg_emit_closed_generic_fit_descriptors()` 当前为一个 closed fit 生成一个 fit-wide
`FengFunctionDescriptor`，全部 wrapper 共用。直接调用和共享体的 dependency mapping 也按
fit declaration 查找 dep set。这与 2.1 的合并表示一致，也正是 owner 域与方法域无法分别
闭合的原因。

该缺陷与 spec witness 无关；没有声明任何 spec 的普通 generic fit 已可复现。

## 3. 能否修复

可以修复。现有 Semantic member dep set、`.ft` 成员 dependency、closed callable
descriptor 和 generic type shell 已提供所需通用抽象，不需要新增 runtime 查找、缓存、
分配或动态构造 descriptor，也不需要改变 runtime 私有 ABI。

正确方向是让 dependency 的 owner 与实际 `FengFunctionDescriptor` 闭合域一致：

1. Semantic 为每个 fit 普通方法建立 `(fit decl, member)` dep set，不再把不同方法的
   owner / method 泛参域合并为一个集合；该规则同时适用于 named target 与 builtin target，
   不能按具体 fit 形态特判；
2. exporter 已通过 `build_member_decl()` 调用 `fill_reifiable_deps(..., owner, member, ...)`，
   应直接复用现有成员格式；importer 将 member dependency 恢复从仅遍历 type 成员推广到
   type / fit 成员，不增加 `.ft` 字段或版本；
3. closed generic fit shell 对“owner 已闭合且该成员没有方法泛参”的 member dep set，复用
   `cg_collect_closed_reifiable_dep_instances()` 预登记闭合依赖；fit 直接调用也进入已有的
   resolved-call 预登记链路；
4. fit 直接调用、共享体 dependency mapping 和 callable dependency 查询均改为按
   `(fit decl, member)` 读取同一集合；
5. 没有方法泛参的 closed wrapper 绑定该成员的静态 closed callable descriptor；有方法
   泛参的成员继续在最终合法调用点以 owner 实参和方法实参共同关闭 descriptor 树。

最终生成代码只读取并转发编译期静态生成的 descriptor。不得通过跳过未闭合项、按泛参名
区分 `T` / `U`、依赖直接调用副作用、运行时搜索或动态缓存补洞。

## 4. 需要人工确认的运行时边界

普通、无方法泛参的 fit 成员可以保持现有 wrapper 调用参数数量：wrapper 内绑定成员自己的
静态 descriptor，因此修复 `Host<int>` / `Box<int>` 不增加运行时操作。编译期只把依赖从
fit-wide 集合拆回对应成员，并提前登记原本就必须生成的闭合实例。

对于“generic fit target 上的方法级泛型成员”，现有 closed fit wrapper 绑定 fit-wide
descriptor；当方法体真正依赖 `U` 或 `Pair<T, U>` 时，该 descriptor 本来就无法正确表达。
通用修复需要让此类 wrapper 像 generic type 的方法级泛型 wrapper 一样接收调用点已经构造的
closed callable descriptor。descriptor 仍由编译期静态生成，不增加查找、分配、动态构造
或缓存；但 concrete generic-fit 的“调用点 -> thin wrapper”这一级会多转发一个原本缺失的
descriptor 指针，随后 wrapper 再把同一指针传给已有 `_desc` 形参。该行为与 generic type
方法级泛型的既有链路一致，但仍属于运行时参数变化，实施前按工程规则由人工确认。

## 5. 测试要求

### 5.1 Compiler tests

- 无 spec、只形成 closed generic fit target、普通实例方法未直接调用，owner-dependent
  managed dependency 不再触发 `CE0031`；
- 同条件的静态方法与 `@value` target / aggregate dependency；
- 同一 fit 的不同普通方法使用不同依赖树，descriptor slot 不串用；
- fit 同时含 owner-only 普通方法和方法级泛型方法时，两个参数域独立；
- `.ft` roundtrip 恢复每个 fit member 的 dependency；
- generated C 只包含静态 descriptor 与既有转发，不新增 runtime helper、查找、缓存或分配。

### 5.2 FCTS

- 同包 generic fit：仅构造 target，不直接调用含 owner dependency 的普通方法；
- 同包通过普通调用和 spec witness 分别执行两个依赖树不同的实现；
- 跨包 provider 导出 generic fit，consumer-only 关闭 owner；
- 实例/static、managed/aggregate 各至少一个非等价代表；
- 方法级泛参路径在人工确认第 4 节后补齐 owner + method 联合闭合行为。

## 6. TODO

- [x] **[分析]** 分别复现无直接调用和存在直接调用时的 `CE0031`，确认与 spec witness
  无关，且当前 resolved-call 预登记同样缺少 fit 分支；
- [x] **[分析]** 核对 type 与 fit 的 Semantic dependency owner 差异；
- [x] **[分析]** 核对 generic fit shell、fit-wide descriptor、wrapper、直接调用与共享体映射；
- [x] **[方案]** 确认复用 member dep set、现有 `.ft` member dependency 与 closed callable
  descriptor，不新增运行时机制；
- [ ] **[人工决策]** 确认第 4 节中方法级泛型 fit wrapper 的 descriptor 参数修正；
- [ ] **[实际变更]** Semantic 按 fit member 收集 dependency；
- [ ] **[实际变更]** importer 恢复 fit member dependency；exporter 仅验证复用现有格式；
- [ ] **[实际变更]** Codegen 按 fit member 预登记、闭合、绑定和读取 descriptor；
- [ ] **[测试变更]** 补齐 Compiler tests 与 FCTS；
- [ ] **[验证]** 确认无特判、无新 `.ft` 格式、无 runtime helper/查找/缓存/分配；
- [ ] **[全量回归]** 在非沙箱环境执行 `make test`。
