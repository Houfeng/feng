# Feng ABI 无字段 `@abi type` 指针规则（待评审草案）

> 状态：草案（供 review）
> 目标：为 C 不透明结构体指针提供正式的 Feng 类型表达。
> 说明：本文是对 [dev/feng-interop-pending.md](./feng-interop-pending.md) 的补充草案；ABI 公共规则同步写入 [docs/feng-interop.md](../docs/feng-interop.md)。

## 1. 规则概述

对象形式的 `@abi type` 用于表达这类 ABI 类型。

对对象形式的 `@abi type T`，ABI 规则按是否声明字段分成两类：

- 有字段：可以按值进入 C ABI surface，也可以按取址规则形成 `T*`。
- 无字段：在 ABI 规则中视为 opaque pointee type，只用于形成 `T*`。

```feng
@abi
type TheCStruct {}

@cdecl("c")
extern fn c_make(): TheCStruct*;

@cdecl("c")
extern fn c_drop(v: TheCStruct*): void;
```

本文定义 C incomplete struct handle 场景的类型表达与 ABI 规则。

## 2. 适用范围

- 没有 `@abi` 的 `type` 不能进入 C 边界。
- 本文只讨论对象形式的 `@abi type`。

## 3. 类型定位

### 3.1 有字段的 `@abi type`

有字段的 `@abi type` 表示可按值进入 C ABI surface 的对象类型，也可以按取址规则形成 `T*`。

### 3.2 无字段的 `@abi type`

无字段的 `@abi type T {}` 在 ABI 语义上只承担一个职责：

- 为 `T*` 提供名义 pointee 身份。

因此：

- `T` 是合法的 Feng 类型名。
- `T*` 是合法类型。
- `T*` 遵循指针规则：不可解引用、不可运算、不可显式转换、不可排序比较，仅允许同类型指针参与 `==` / `!=`。
- 本文不因为“无字段”而为 `T` 增加额外的普通 Feng 语义限制；本文只约束它在 C ABI 边界上的出现形状。

## 4. ABI 可观察规则

### 4.1 `extern fn` 中的出现形状

对无字段的 `@abi type T`：

- `extern fn f(x: T)` 非法。
- `extern fn f(): T` 非法。
- `@abi fn f(x: T): void` 非法。
- `@abi fn f(): T` 非法。
- `@abi spec F(x: T): void;` 非法。
- `@abi spec F(): T;` 非法。
- `extern fn f(x: T*): void` 合法。
- `extern fn f(): T*` 合法。
- `@abi fn f(x: T*): T*` 合法。
- `@abi spec F(x: T*): T*;` 合法。

也就是说，在任何 ABI callable surface 中，无字段 `@abi type` 只能以 `T*` 形式出现，不能按值出现。

### 4.2 一元 `&` 规则

一元 `&` 只适用于可形成 data-addressable ABI value 的值。

对对象形式的 `type`，规则如下：

- 只有带 `@abi` 且声明了字段的 `type`，才能通过 `&` 形成 `T*`。
- 无字段的 `@abi type` 不能通过 `&` 形成 `T*`。

因此，无字段 `@abi type` 的 `T*` 合法来源仅包括：

- `extern fn` 返回值。
- 作为参数从外部 ABI 边界传入。
- 已存在的 `T*` 绑定、字段或返回值的传递。

本文不为其增加新的构造来源。

## 5. 代码生成规则

对无字段的 `@abi type`，ABI lowering 只需要保证以下结果：

- 无字段的 `T*` 在 C surface 上按 opaque pointer 处理。
- 无字段的 `T` 不进入按值 ABI surface。
- 编译器不为无字段的 `T` 生成 `abi_layout`。

普通 Feng `type` 的发码路径不在本文定义范围内；本文只要求 C ABI lowering 满足上述结果。

## 6. 不新增的限制

本文不新增专门的声明限制或成员形状限制。

也就是说：

- 不新增新的注解、关键字或声明形式。
- 不额外增加“不得有方法”“不得有终结器”“不得参与普通构造/绑定/成员访问”之类限制。
- 除“无字段 `@abi type` 不能按值出现在 `extern fn` 中，且不能通过 `&` 形成 `T*`”之外，不新增新的专门校验。

若某种写法已被其他语言规则禁止，则按那些规则处理；本文不重定义这些规则。

## 7. 建议测试覆盖

### 7.1 正例

```feng
@abi
type TheCStruct {}

@cdecl("c")
extern fn c_make(): TheCStruct*;

@cdecl("c")
extern fn c_drop(v: TheCStruct*): void;

fn run() {
    let p: TheCStruct* = c_make();
    c_drop(p);
}
```

应覆盖：

- extern 返回 `T*`
- extern 参数 `T*`
- 本地绑定 `T*`
- 同类型指针 `==` / `!=`

### 7.2 反例

应覆盖以下错误：

- 没有 `@abi` 的 `type` 出现在 C 边界
- `extern fn f(x: T)` 试图按值传递无字段 `@abi type`
- `extern fn f(): T` 试图按值返回无字段 `@abi type`
- `@abi fn f(x: T)` 试图按值传递无字段 `@abi type`
- `@abi fn f(): T` 试图按值返回无字段 `@abi type`
- `@abi spec F(x: T)` 试图按值传递无字段 `@abi type`
- `@abi spec F(): T` 试图按值返回无字段 `@abi type`
- `let p: T* = &x` 试图通过 `&` 形成无字段 `@abi type` 的指针
- 把 `T*` 用于不允许的指针操作

## 8. 规则摘要

本文可摘要为一句话：

> 对象形式 `@abi type` 在 ABI 规则中分为两类：有字段者可以按值进入 C ABI surface，也可以按取址规则形成 `T*`；无字段者只作为 `T*` 的名义 pointee，不能按值出现在任何 ABI callable surface 中，也不能通过 `&` 在 Feng 侧形成指针，且不生成按值 ABI layout。

## 9. Todo

### 9.1 文档同步

- [x] 将本稿规则同步到 [dev/feng-interop-pending.md](./feng-interop-pending.md)。
- [x] 将 ABI 公共规则同步到 [docs/feng-interop.md](../docs/feng-interop.md)。

### 9.2 语义实现

涉及文件：

- [src/semantic/analyzer.c](../src/semantic/analyzer.c)：调整 `validate_extern_function_signature`、`validate_abi_callable_signature`、`validate_abi_type_declaration`、`inferred_expr_type_is_data_addressable_abi_value`、`type_ref_is_abi_stable` / `type_decl_is_abi_stable` 相关判定。

- [x] 在 ABI callable 签名校验中区分“有字段 `@abi type`”与“无字段 `@abi type`”。
- [x] 拒绝无字段 `@abi type` 以按值 `T` 形态出现在 `extern fn`、顶层 `@abi fn` 与 callable-form `@abi spec` 的参数位与返回位。
- [x] 调整一元 `&` 的判定，使对象形式 `type` 只有在“带 `@abi` 且声明了字段”时才能形成 `T*`。
- [x] 保持没有 `@abi` 的 `type` 不能进入 C 边界。

### 9.3 代码生成

涉及文件：

- [src/codegen/codegen.c](../src/codegen/codegen.c)：调整 `cg_pointer_inner_is_lowerable`、`cg_emit_c_type`、`cg_init_user_type_abi_symbols`、`cg_emit_user_type_abi_surface` 等 ABI lowering 路径。

- [x] 让无字段 `@abi type` 的 `T*` 在 C surface 上走 opaque pointer lowering。
- [x] 禁止无字段 `@abi type` 的 `T` 进入按值 ABI lowering。
- [x] 跳过无字段 `@abi type` 的 `abi_layout` 生成。
- [x] 保持有字段 `@abi type` 的按值 ABI lowering 与取址形成 `T*` 的能力。

### 9.4 测试补齐

- [x] 为无字段 `@abi type` 增加 `extern fn` 返回 `T*` 的正例。
- [x] 为无字段 `@abi type` 增加 `extern fn` 参数 `T*` 的正例。
- [x] 为无字段 `@abi type` 增加顶层 `@abi fn` 参数/返回 `T*` 的正例。
- [x] 为无字段 `@abi type` 增加 callable-form `@abi spec` 参数/返回 `T*` 的正例。
- [x] 为无字段 `@abi type` 增加按值参数 `T` 的反例。
- [x] 为无字段 `@abi type` 增加按值返回 `T` 的反例。
- [x] 为无字段 `@abi type` 增加顶层 `@abi fn` 按值参数/返回 `T` 的反例。
- [x] 为无字段 `@abi type` 增加 callable-form `@abi spec` 按值参数/返回 `T` 的反例。
- [x] 为无字段 `@abi type` 增加通过 `&` 形成 `T*` 的反例。
- [x] 回归验证有字段 `@abi type` 仍可按值进入 ABI，且仍可通过 `&` 形成 `T*`。

### 9.5 回归验证

- [x] 执行相关语义测试与代码生成测试。
- [x] 执行全量回归测试。
