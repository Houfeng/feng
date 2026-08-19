# Feng 泛型 spec 字段 witness 与稳定 ABI 修复开发文档

> 状态：已实施并通过全量回归（2026-08-19）
>
> 本文只修复 generic object-form spec 字段 witness 的开放类型匹配，以及字段槽消费
> 声明期稳定 ABI 时的 Codegen 正确性；不改变 spec、泛型、字段可见性或 witness 语义。

## 1. 实际失败

补充显式 `open` × generic spec FCTS 时，以下合法路径在生成 C 编译阶段失败：

```feng
spec Storage<T> {
  open static let empty: T;
  open static var current: T;
}

func read<T, U: Storage<T>>(): T {
  return U.current;
}
```

generic spec 的声明槽必须在开放定义上选择稳定 ABI。`T` 的运行时表示未知，因此 getter
正确声明为 address-return：

```c
void (*get_current)(void *_out);
```

但当前字段读取 lowering 仍生成 `get_current()` 并把其结果当作返回地址，导致 C 编译器
报告“expected 1 argument, have 0”。同一缺口也影响 `static let: T`。

## 2. 根因与相邻同域缺口

根因不是 parent spec、显式 `open` 或 `.ft` 恢复：生成的 child witness 已正确保留
`is_static` 和 `CG_CALLABLE_ABI_ADDRESS`，错误发生在消费这个既有 ABI 时。

实际代码核对还确认同一字段 witness 链路存在三个缺口：

1. generic type 闭合后需要为开放 generic spec 生成 witness 时，方法成员已有开放 spec
   参数的模式匹配，字段 fallback 却仍直接比较闭合实现类型与开放 `T`，合法字段会错误触发
   `CE0316`；
2. `cg_emit_static_user_spec_member_thunk()` 为静态字段始终生成具体 C 返回/参数类型，没有按
   `value_abi_kind` 生成 address-return getter 和 address-form setter；
3. spec 字段赋值在 closed generic spec 将 `T` 替换为 `int`、`string` 等具体类型后，仍需
   遵守开放声明选定的 address setter ABI；当前普通赋值和复合赋值会直接传值。该问题同时
   适用于 generic constraint receiver 与 object-form spec value receiver。

实施时还确认，generic type 静态字段初始化已有统一的 descriptor/state ensure-init
链路；witness thunk 直接调用每个闭合实例名下并不存在的零参 ensure 函数，会导致生成 C
出现未声明函数。修复必须把现有 ensure-init 发码抽象复用于 thunk，不得新建初始化链路。

这些路径共享一个规则：closed member type 不能覆盖 generic spec 原声明槽已经选定的稳定
ABI。不得按 `int`、`string`、字段名、是否继承或显式 `open` 特判。

## 3. 修复方案

1. 字段 fallback 与方法 fallback 复用同一开放 generic spec 参数匹配规则；closed spec
   仍保持精确相等，不引入泛型协变；
2. 静态字段 witness thunk 统一按 `value_abi_kind` 发码：
   - `ADDRESS` getter 接收 `_out`，从已经初始化的静态绑定复制到该地址；
   - `ADDRESS` setter 接收 `const void *`，复用现有 callable ABI 参数桥接后执行现有
     `feng_assign` / `feng_aggregate_assign` / 直接赋值；
   - `ERASED_POINTER` 与 `DIRECT` 同样由现有 ABI emit/bridge helper 决定 C 签名，不另造规则。
   - 初始化调用复用既有 generic type descriptor/state ensure-init 发码。
3. address-return 静态字段读取复用现有 erased generic、reified aggregate 或固定 C local
   存储声明，并把该地址传给 getter；结果保持字段读取的借用语义，不增加 retain/release。
4. spec 字段 setter 根据 `value_abi_kind` 复用
   `cg_shared_callable_argument_expr_dup()`：address ABI 传稳定地址，其他 ABI 保持当前直接值。
5. 数值复合赋值在 address ABI 下先通过 getter/out 或 instance borrow 读取具体旧值，在 C
   local 中计算，再把新值地址传给 setter；普通 direct ABI 路径保持不变。

修复不改变 witness 布局、`.ft` 格式或 runtime ABI；只让调用端和 thunk 端遵守已经生成的
函数指针签名。不会增加运行时查找、缓存、堆分配或间接层。

## 4. 测试

- Codegen：generic type/fit 的实例与静态字段可匹配开放 generic spec 字段；
- Codegen：generic spec 的 `static let T` / `static var T` 通过 constraint 读取和写入；
- Codegen：`Storage<int>` 这类 closed member type 仍使用声明期 address getter/setter；
- Codegen：generic parent spec 的静态字段被 child constraint 继承后保持同一 ABI；
- Codegen：generic constraint receiver 与 spec value receiver 的普通/复合实例字段赋值；
- FCTS：同包 generic type、同包 generic fit、跨包 consumer type/fit 均执行实例与静态字段
  读写；
- 生成 C 必须能以 `-Werror` 编译，且没有新 runtime helper、查找或分配。

## 5. TODO

- [x] **[分析]** 由新增 FCTS 复现 getter 少传 `_out`；
- [x] **[分析]** 确认 witness 声明正确，根因在字段调用与静态 thunk ABI 消费；
- [x] **[分析]** 核对 setter 和复合赋值的同域稳定 ABI 缺口；
- [x] **[分析]** 确认字段 fallback 缺少方法已有的开放 generic spec 参数匹配；
- [x] **[分析]** 确认 generic type 静态字段 witness thunk 未复用现有 ensure-init 链路；
- [x] **[实际变更]** 统一字段 fallback 的开放 generic spec 参数匹配；
- [x] **[实际变更]** 修复静态字段 getter/setter thunk 并复用现有 ensure-init 链路；
- [x] **[实际变更]** 修复 address-return 字段读取与 address setter 参数 lowering；
- [x] **[实际变更]** 修复 address ABI 数值复合赋值；
- [x] **[测试变更]** 补 Codegen 与 FCTS 直接回归；
- [x] **[验证]** 确认 direct ABI、非泛型 spec、可见性和满足关系不变；
- [x] **[全量回归]** 在非沙箱环境执行 `make test`。

## 6. 实施结果

- `test_generic_spec_field_stable_address_abi_codegen` 直接验证声明期 address ABI、实例与
  静态 getter/setter、普通/复合写入、继承 surface、静态初始化及生成 C 编译；
- FCTS 的同包 generic type、同包 generic fit 与跨包 consumer type/fit 均执行了实例和
  静态字段路径；
- Semantic、Codegen 专项测试通过，FCTS `814/814` 通过；非沙箱全量 `make test` 通过。
