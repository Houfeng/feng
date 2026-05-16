# Feng Runtime 互操作（待评审草案）

> 状态：讨论整理稿（供 review）
> 目标：收敛 Feng 源码与编译器随附 runtime / internal API 的通信方案，再进入实现与测试拆解。
> 说明：当前 runtime 互操作方向以本文为准；后续公共规范回写时，由本文收敛后再同步到权威文档。

## 1. 讨论收敛结论（当前方向）

本轮结论如下：

1. `extern fn` 不是 C ABI 独占语法；它的职责是声明“实现不在当前 Feng 源文件中的外部函数”。
2. 具体走哪一类外部调用契约，由注解决定，而不是由 `extern fn` 关键字本身决定。
3. C ABI 仍然使用既有 `@cdecl` / `@stdcall` / `@fastcall` 路径；runtime object contract 新增 `@runtime` 路径。
4. runtime 导入统一采用 `@runtime` 作为目标注解入口，不新增关键字，也不通过库名特判扩展语义。
5. `@runtime` 的目标是“编译器自有且可版本化的 runtime contract”，不是对整个 runtime 内部实现的开放访问。
6. `@runtime` 首版不新增来源身份限制或公开导出过滤逻辑；只保留“非公开 API、无稳定性承诺、仅建议编译器和标准库使用”的边界。
7. `@runtime` 不维护独立类型白名单 / 黑名单，也不引入额外签名语义检查；参数和返回值合法性直接复用普通 Feng 函数签名规则。
8. `@runtime extern fn` 在 Feng 侧的使用语义应与普通 Feng 顶层函数一致；区别仅在于实现体来自 C native runtime contract，而不是由 Feng 源码提供函数体。
9. `@runtime extern fn` 的最小实现路径是：生成对应 C 符号声明，并把调用点直接发为对该 C 函数的普通调用；不额外引入 ABI bridge、trampoline 或特殊调用机制。
10. 顶层 `intrinsic` 不再作为独立层保留；现存 compiler-shipped helper 应迁入 `src/runtime/contract/`（名称待最终定稿，但本文先用 `contract`），作为允许 `@runtime extern fn` 声明使用的受控白名单。
11. 不移动 `src/runtime/` 目录本身；只取消顶层 `src/intrinsic/`，并把其剩余实现与声明并入 runtime 子层。
12. 迁移完成后的结束形态只保留一个 `libfeng_runtime.a`；`libfeng_intrinsic.a`、`FENG_INTRINSIC_LIB` 以及 `@cdecl("feng_intrinsic")` 都应消失。
13. 当前实现里，`extern fn` 仍被语义层直接视为 C ABI 导入：先强制要求恰好一个 `@cdecl` / `@stdcall` / `@fastcall`，再对参数和返回值统一做 C ABI 兼容检查；引入 `@runtime` 前必须先解耦这条现状。

当前推荐方向：

- 保持 `extern fn` 作为统一外部函数声明语法。
- 保持 C ABI 与 runtime contract 两条路径并列存在。
- 通过新增 `@runtime` 注解，为 `extern fn` 提供独立于 C ABI 的目标分流与发码分支；除注解互斥和 `extern fn` 壳约束外，不新增额外语义检查。
- 不再把顶层 intrinsic 视为长期层；将其剩余 helper 并入 `src/runtime/contract/` 这类 runtime 子层，不移动 runtime 根目录。

---

## 2. `extern fn` 的总模型

### 2.1 重新收敛后的定义

`extern fn` 表示：函数实现不在当前 Feng 源中，由外部契约提供，且声明本身不得带函数体。

这里的“外部契约”不预设必须是 C ABI。当前先收敛两类目标：

1. C ABI 导入
2. Runtime contract 导入

未来如有新的外部目标（如其他原生平台契约、特定 VM 契约等），继续通过新增目标注解扩展，不重新设计 `extern fn` 语法。

### 2.2 目标注解分流

当前阶段，`extern fn` 必须且只能选择一种目标注解分支：

| 路径 | 注解 | 语义 | 当前签名约束 |
| --- | --- | --- | --- |
| C ABI 导入 | `@cdecl(...)` / `@stdcall(...)` / `@fastcall(...)` | 调用外部 C 函数或编译器随附 intrinsic ABI surface | 必须满足 C ABI 兼容资格 |
| Runtime 导入 | `@runtime` | 调用编译器随附 runtime contract | 与普通 Feng 顶层函数签名规则一致；不新增专属语义检查 |

补充规则：

1. `extern fn` 不允许无目标注解悬空存在。
2. 不同目标注解族两两互斥。
3. 目标注解的含义必须可由声明规则静态判定，不依赖运行时猜测或具体库名特判。

注：上表是目标模型；当前实现仍把所有 `extern fn` 直接当作 C ABI 导入处理，因此落地前需先完成 [§11.2](dev/feng-runtime-interop-pending.md#112-phase-05前置解耦当前-extern-fn-的-c-only-检查) 的前置解耦。

---

## 3. `@runtime` 的定位

### 3.1 定位

`@runtime` 表示该 `extern fn` 走 Feng runtime object contract，而不是 C ABI lowering。

从 Feng 代码视角看，`@runtime extern fn` 应尽量等价于“由原生实现提供函数体的普通顶层函数声明”：

1. 调用方式与普通 Feng 函数一致。
2. 参数和返回值语义与普通 Feng 函数一致。
3. 差异只在实现来源与 codegen lowering，而不在调用方书写模型。

它解决的问题是：

- Feng 源码与编译器随附 runtime / internal API 之间，需要一条比 C ABI 成本更低、语义更自然的通信路径。
- 该路径应允许直接表达 Feng 自然值语义；当 contract 语义本来就是字符串、数组或对象值时，不应再强迫开发者把它们改写成 `string*`、`T*`、`Foo*` 一类 ABI 形状。若某个 helper 本身就是面向指针值工作，则指针类型仍可按普通 Feng 类型规则出现。

它不解决的问题是：

- 用户自定义 C 库互操作。
- 包公开 surface 的跨语言 ABI 承诺。
- runtime 内部实现细节对任意用户代码的无约束开放。

### 3.2 与 C ABI 的边界

`@runtime` 与 C ABI 路径明确分离：

1. `@runtime` 不参与 `@abi("c")` 语义。
2. `@runtime` 不使用库名或路径参数。
3. `@runtime` 不按 `cdecl` / `stdcall` / `fastcall` 进行兼容性检查，也不额外引入一套平行于普通 Feng 函数的签名语义检查。
4. `@runtime` 不要求也不默认复用 `T*` / `Foo*` 这一套 ABI 指针模型来表达普通托管值；但若签名本身确实需要指针类型，则按普通 Feng 指针类型处理。
5. `@runtime` 的语义核心是“自然运行时表示 + 编译器自有 contract”，而不是“显式 ABI payload 形状”。

---

## 4. `@runtime` 的语法与声明规则

### 4.1 基本语法

```feng
@runtime
extern fn feng_string_length(value: string): long;
```

### 4.2 适用位置

`@runtime` 当前仅适用于无函数体的顶层 `extern fn` 声明。除这一声明形态约束及目标注解互斥外，`@runtime` 不再引入额外语义检查。

### 4.3 互斥规则

以下组合一律非法：

1. `@runtime` 与 `@cdecl(...)` 同时出现。
2. `@runtime` 与 `@stdcall(...)` 同时出现。
3. `@runtime` 与 `@fastcall(...)` 同时出现。
4. `@runtime` 与 `@abi` 同时出现。

原因：

- `@abi` / `@cdecl` / `@stdcall` / `@fastcall` 定义的是 ABI surface。
- `@runtime` 定义的是 runtime contract surface。
- 两条路径的签名检查与发码策略不同，不能混用。

### 4.4 非公开定位与使用边界

`@runtime extern fn` 的定位是编译器自有的非公开 API。为减少实现复杂性，首版不新增“标准库 / 普通库”来源身份限制，也不新增公开导出过滤逻辑；当前只保留以下边界：

1. `@runtime` 不属于面向用户承诺的公共规范能力，不作为公开互操作层文档化。
2. `@runtime` 不提供 API 稳定性承诺；其签名、carrier、符号与 lowering 细节都可随编译器和 runtime 演进调整。
3. `@runtime` 的预期使用方仅为编译器与标准库；除这两类代码外，其他地方不应依赖它形成长期接口。
4. 其他代码若自行使用 `@runtime`，兼容性风险由使用方自行承担；编译器不保证跨版本兼容。

其本质是“编译器自有 contract 的声明入口”，而不是用户可依赖的稳定开放 API。

---

## 5. `@runtime` 的签名语义（与普通 Feng 函数一致）

### 5.1 基本结论

`@runtime` 没有独立的签名资格检查。参数和返回值是否合法，直接按普通 Feng 顶层函数既有规则判定；`@runtime` 只改变目标注解分流与 lowering，不再引入第二套并行语义。

当前原则：

1. 优先复用普通 Feng 函数签名的既有合法性规则，而不是为 `@runtime` 额外列白名单。
2. `@runtime` 本身不新增“只允许某些类型”或“额外拒绝某些类型”的专属检查。
3. 某个类型若当前仍不能用于 `@runtime`，原因只能是它本来就不是普通 Feng 函数签名中的合法类型，或当前普通函数 lowering 尚不支持它。

### 5.2 当前结论

当前结论：`@runtime` 原则上允许所有“当前阶段可作为普通 Feng 顶层函数参数 / 返回值出现并被编译器稳定 lowering”的合法 Feng 类型。换言之，不再单列一份 `@runtime` 专属允许类型表。

这意味着：

1. 不再为 `@runtime` 单独维护“只允许标量 / string / 数组”的专属白名单。
2. 普通 `type`、数组、字符串等值类别，若本来就是普通 Feng 函数签名里的合法类型，则在 `@runtime` 中也应直接允许。
3. 类型检查实现应尽量复用普通函数签名合法性与现有 lowering 路径。

### 5.3 不额外引入 `@runtime` 专属类型限制

因此，`@runtime` 路径上出现类型报错时，原因应尽量只来自以下两类来源：

1. 该类型本身当前就不是普通 Feng 函数签名里的合法类型。
2. 该类型虽然语言层面存在，但当前编译器尚未具备稳定的普通函数 lowering。

补充说明：

- `T*`、`Foo*` 在 Feng 中就是合法的不透明指针类型；`@runtime extern fn` 与普通 Feng 函数一样可直接使用它们，不需要额外说明或专属检查。
- `@runtime` 允许指针类型，不等于应该把原本可自然写成 `string`、数组或对象值的 contract 一律改写成指针版本；是否使用指针，应由 contract 的真实语义决定。
- 这一路径的目标是减少规则分叉：`@runtime` 主要新增的是目标注解分流与 lowering 分支，而不是第二套类型系统门槛。

---

## 6. `@runtime` 的语义与 lowering 方向

### 6.1 总原则

`@runtime` 签名在 Feng 源层表达的是普通 Feng 函数的自然值语义，而不是 C ABI 形状。

也就是说：

- 当 contract 语义是 Feng 字符串值时，在 `@runtime` 路径中写 `string`，而不是仅因 ABI 习惯写成 `string*`。
- 当 contract 语义是 Feng 数组值时，在 `@runtime` 路径中写数组类型本身，而不是仅因 ABI 习惯写成元素区地址。
- 若 contract 本身就是面对指针值做操作，指针类型仍可直接出现在 `@runtime` 签名中。
- 不要求开发者通过 `&` 把值先改写为 ABI 指针才能调用 runtime helper。
- 在调用点、重载选择和返回值使用上，应尽量与普通 Feng 顶层函数保持同一套规则。

### 6.2 发码分支

对 `@runtime extern fn`，codegen 走独立 lowering：

1. 前端语义尽量复用普通 Feng 函数调用规则，而不是额外发明新的调用模型。
2. 不做 C ABI 兼容资格检查，也不新增一套专属于 `@runtime` 的签名语义检查。
3. 不生成 C ABI payload bridge。
4. 对签名里本来就是普通值的 `string`、数组、对象，不应用 `@abi type` / `T*` / `Foo*` 这一套 C ABI payload 改写规则；若签名本身声明的是指针类型，则按该指针类型的既有语义直接 lowering。
5. 直接映射到 runtime contract 对应的 C 符号声明；与普通 Feng 函数相比，区别只是 callee 实现由外部原生符号提供，而不是由当前 Feng 源码发出函数定义。

首版最小实现建议进一步收敛为：

1. 对每个 `@runtime extern fn`，生成与其 runtime contract carrier 对应的 C 原型声明。
2. 在调用点，直接把实参 lowering 后发为对该 C 符号的普通调用表达式。
3. 不额外生成 trampoline、shim、桥接结构或第二套调用协议。
4. 只有在未来出现明确需求时，才为个别能力引入额外 bridge；首版默认路径就是“直接发为相应 C 函数调用”。

### 6.3 头文件与 contract 边界

当前建议：

1. 收敛 runtime contract 头的唯一目的，是为 `@runtime` 提供一份可声明目标白名单；它不是整个 runtime public header 的别名。
2. 若过渡阶段复用现有 `runtime/feng_runtime.h`，也只应把其中被 `@runtime` 明确纳入白名单的声明视为可达；这不等于把整份 header 都开放给 `@runtime`。
3. `runtime_internal.h` 一类内部头永远不进入该白名单；`src/runtime/contract/` 的作用也是收敛这份受控入口集合，而不是重新定义 runtime 的其他层次。

当前最小落地方式：

1. 保持 `runtime/feng_runtime.h` 作为 codegen 继续复用的唯一入口头。
2. 新增 `src/runtime/feng_runtime_contract.inc` 作为 contract 条目清单；该片段既用于在 `feng_runtime.h` 中展开声明，也用于 codegen 侧做 `@runtime` 名字检查。
3. 新增 `src/runtime/feng_runtime_contract.c` 承载 contract helper 实现；helper 仍编入同一个 `libfeng_runtime.a`，不新增独立库。

目标是把 `@runtime` 可声明的受控入口集合明确下来；目录上体现为 `src/runtime/contract/` 与 `src/runtime/` 其余实现文件的区别，而不是继续保留顶层 `src/intrinsic/`。

### 6.4 符号命名

首版建议继续沿用 `extern fn` 当前做法：Feng 声明名与目标 C 符号名保持一致，不额外引入第二套命名系统。

若未来 runtime contract 需要名字重映射，再单独引入精确规则；当前不提前扩展。

---

## 7. 所有权、生命周期与异常边界

### 7.1 参数语义

对 `@runtime` 路径，入参在 Feng 语义层与普通 Feng 函数调用一致：

1. 标量按值传递。
2. `string`、数组等托管值按其自然运行时 carrier 传递。
3. callee 若需要在调用结束后继续持有调用方传入的托管值，必须通过 runtime contract 规定的方式取得独立持有权；不得把“调用期间可见”默认等同于“可长期缓存”。
4. 若参数类型本身就是 `T*` / `Foo*` 一类指针，则按该指针值本身传递；若该指针来自借用形成的临时地址，其可见期与可缓存性仍需由对应 contract 明确约束，不能默认长期持有。

### 7.2 返回值语义

`@runtime` 的返回值语义与普通 Feng 函数保持一致：

1. 返回的标量值按普通返回值处理。
2. 返回的托管值按普通 Feng 返回语义交给调用方。
3. `@runtime` 不引入额外“隐式借用返回值”规则。

### 7.3 异常边界

`@runtime` 不应额外引入一条不同于普通 Feng 函数的异常模型。

也就是说，从 Feng 调用方视角看：

1. 若原生实现通过 runtime contract 抛出 Feng 异常，调用方应按普通 Feng 函数调用一样观察和处理该异常。
2. `@runtime` 与 C ABI 路径不同；它不是“异常必须截断”的外部 ABI 边界。
3. 真正需要禁止的，是把不属于 Feng 异常模型的外部原生异常机制直接穿过这条 contract。

### 7.4 与内存管理实现细节的关系

本文只定义语义边界：

- 哪些值类别允许进入 `@runtime` 签名。
- 调用期间与调用后持有责任如何划分。

具体 runtime 如何实现这些语义，仍应收敛在对应权威实现文档中，不在本文展开。

---

## 8. 与其他路径的关系

### 8.1 与 intrinsic 的关系

顶层 `intrinsic` 在本文中不再视为长期保留层，但其“受控可声明入口集合”这一职责仍然保留，并迁入 runtime 子层。

当前收敛方向是：现存通过 `@cdecl("feng_intrinsic")` 暴露的 compiler-shipped helper，从顶层 `src/intrinsic/` 迁入 `src/runtime/contract/`，并改为 `@runtime extern fn` 路径可声明的受控白名单。

这意味着：

1. `intrinsic` 这个名称不再保留为顶层层级名；本文暂用 `contract` 表示该 runtime 子层，因为它表达的是“允许 `@runtime` 声明使用的 contract surface”，而不是泛泛的内部 helper。
2. 该子层是 runtime 的受控白名单，不是整个 runtime 目录的别名，也不是所有 runtime 实现文件都自动可被 `@runtime` 声明。
3. 迁移完成后，应移除 `src/intrinsic/`、`libfeng_intrinsic.a`、`FENG_INTRINSIC_LIB`、`@cdecl("feng_intrinsic")` 保留解析以及相关文档 / 测试 / build 逻辑。
4. runtime 根目录保持不动；变化的是把原先顶层 intrinsic 的职责并回 runtime 子层，以更准确地表达这是 runtime contract 的一部分。

### 8.2 与 `@abi` 的关系

`@abi` 用于 ABI 兼容性声明；`@runtime` 用于 runtime contract 声明。两者分层如下：

1. `@abi` 关心外部 ABI 形状。
2. `@runtime` 关心内部 runtime 值表示。
3. 两者都属于注解驱动的编译期规则，但作用域不同，不能互相替代。

### 8.3 与未来扩展的关系

采用 `extern fn` + 目标注解分流后，未来新增其他外部调用契约时，原则上继续复用：

1. `extern fn` 作为统一语法壳。
2. 通过新的目标注解引入新的目标分流与发码分支；除目标注解自身约束外，不轻易新增一套平行的语义检查。
3. 不引入额外关键字，除非出现注解无法表达的根本性语义差异。

---

## 9. 语法示例

### 9.1 合法示例：runtime string helper

```feng
@runtime
extern fn feng_string_length(value: string): long;

fn size_of(s: string): long {
    return feng_string_length(s);
}
```

### 9.2 合法示例：runtime array helper

```feng
@runtime
extern fn feng_array_length_int(values: int[]): long;
```

### 9.3 错误示例：混用 C ABI 与 runtime 注解

```feng
@runtime
@cdecl("feng_runtime")
extern fn bad(value: string): long;
// 编译期报错：`@runtime` 与 C ABI 目标注解互斥
```

### 9.4 合法示例：runtime 指针 helper

```feng
@runtime
extern fn feng_debug_bytes(data: byte*, size: long): void;
// 合法：若 runtime contract 的语义本来就是操作指针值，`@runtime` 可以直接使用指针类型
// 但这不意味着应把本来可自然写成 `string` / 数组 / 对象值的 contract 一律改写成指针形状
```

### 9.5 合法示例：普通对象值沿用自然表示

```feng
type User {
    var name: string;
}

@runtime
extern fn feng_user_debug(value: User): void;
```

---

## 10. 入口收敛

为减少噪声与分叉，本文只保留当前有效入口：runtime 导入统一使用 `@runtime extern fn` 表达。

其他命名或入口形式（如新增关键字、使用更宽泛的内部语义命名、或通过库名特判模拟 runtime 路径）当前都不进入本稿正文规则；除非后续确有必要，否则不再展开历史讨论。

---

## 11. 分阶段任务建议

### 11.1 Phase 0：规范与术语收敛

- [ ] 在开发文档中确认 `extern fn` 的总定义不再限定为 C-only。
- [ ] 同步公共权威文档：把 `extern fn` 的总定义回写到权威规范，并把 `docs/feng-interop.md` 收敛为 C ABI 路径权威，而不是继续覆盖全部 `extern fn` 语义。
- [ ] 为 `@runtime` 明确首版适用位置、互斥规则、非公开定位，以及“签名语义与普通 Feng 函数一致”的原则。
- [ ] 明确顶层 intrinsic 迁入 `src/runtime/contract/` 子层；runtime 语义继续停留在 `src/runtime/` contract surface。

### 11.2 Phase 0.5：前置解耦当前 `extern fn` 的 C-only 检查

- [ ] 把当前“所有 `extern fn` 都必须恰好使用一个 `@cdecl` / `@stdcall` / `@fastcall`”的检查，收敛到 C ABI 路径本身，而不是继续绑定在裸 `extern fn` 上。
- [ ] 把当前 `extern fn` 参数 / 返回值统一走 C ABI 兼容检查的逻辑，收敛到 C ABI 路径本身，避免未来 `@runtime extern fn` 误触发 “not C ABI-stable” 诊断。
- [ ] 补充前置回归：C ABI 导入保持现状；非 C ABI 的 `extern fn` 形态在进入 `@runtime` 阶段前不再先被 C ABI 规则拦截。

### 11.3 Phase 1：词法 / 语法入口

- [ ] 新增内建注解 `@runtime`。
- [ ] parser 允许其标注在无函数体的顶层 `extern fn` 上。
- [ ] parser 拒绝明显非法组合（如标注在非 `extern fn` 场景）。

### 11.4 Phase 2：目标分流与普通函数语义复用

- [ ] 实现 `@runtime` 与 C ABI 目标注解互斥检查。
- [ ] 确保 `@runtime` 在参数 / 返回值、重载决议与调用语义上直接复用普通 Feng 函数规则，不新增独立签名检查。
- [ ] 不为 `@runtime` 维护独立类型白名单 / 黑名单，也不额外引入标准库 / 普通库身份特判或公开导出过滤逻辑，保持实现简洁。
- [ ] 诊断策略上尽量复用普通 Feng 函数已有报错；只对目标注解冲突等 `@runtime` 自身声明错误新增诊断。

### 11.5 Phase 3：发码与链接

- [ ] 为 `@runtime extern fn` 新增独立 lowering 分支；首版直接 emit 对应 C 原型与普通 C 调用，不引入额外 bridge / trampoline。
- [ ] 收敛 runtime contract 头文件白名单边界；必要时从现有 runtime 头中拆出更窄的 contract 头，仅用于枚举 `@runtime` 可声明入口。
- [ ] 明确 runtime contract 符号的链接来源与 build 集成方式。

### 11.6 Phase 4：intrinsic 并入 runtime 子层

- [ ] 把现存 `feng_intrinsic` helper 移入 `src/runtime/contract/`，并迁移到 `@runtime extern fn` 路径。
- [ ] 删除顶层 `src/intrinsic/`、独立 `libfeng_intrinsic.a`、`FENG_INTRINSIC_LIB` 与 `@cdecl("feng_intrinsic")` 相关保留逻辑。
- [ ] 同步清理 build、文档与测试中对顶层 intrinsic 层的长期假设。

### 11.7 Phase 5：标准库迁移试点

- [ ] 选择 1 到 2 个确实依赖 runtime object contract 的标准库能力做迁移验证。
- [ ] 验证迁移后是否比 ABI surface 方案更自然、成本更低且不损害边界清晰度。

### 11.8 Phase 6：测试与回归

- [ ] 新增 lexer / parser / semantic / codegen 正反例测试。
- [ ] 新增标准库或 smoke 场景，覆盖至少一个 `string` 和一个数组场景。
- [ ] 全量回归 `make test`。

---

## 12. 本稿定位

本文不是公共规范终稿，而是 runtime 互操作开发决策稿。

后续流程建议：

1. 先按第 11 节收敛术语、语义边界与首版资格。
2. 再进入实现，先完成当前 `extern fn` 的 C-only 解耦，再做 `@runtime` 注解入口、目标分流与发码接入。
3. 最后根据实现结果，把稳定结论同步回公共规范文档。
