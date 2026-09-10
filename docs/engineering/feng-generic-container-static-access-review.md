# 泛型容器静态成员访问对照验证

本文记录 `List<T>` 与 `T[]` 在静态成员访问上的对照验证，关联
[合法程序发码失败 Review](./feng-valid-program-codegen-failures-review.md)。

## 1. 判定依据

开发者确认：`T[]` 是泛型容器类型；若对应的 `List<T>` 静态成员访问合法，数组目标也应合法。
本轮验证直接调用、方法级泛参、具体方法值、泛型函数中的方法值，以及 `spec` 静态成员适配。
通用规则引用 [fit 规范](../specifications/feng-fit.md)、
[内建类型 fit 规范](../specifications/feng-fit-builtin-type.md) 和
[函数规范](../specifications/feng-function.md)，本文不另行定义语言规则。

## 2. 实测结果

基线为 `1905b521`，验证日期为 2026-09-11。使用实际标准库
`std.collections.List<T>`（从 `std/std/build/pkg/std-0.1.0.fb` 导入），并以本地声明的
`type List<T>` 做对照；两者均通过 `fit List<T>` 提供 `echo(value:T):T` 和
`keep<U>(value:U):U`。每个场景独立编译，成功后运行并检查结果和退出码。

| 场景 | 示例形态 | 标准库 List<T> | 本地泛型类型 |
|---|---|---|---|
| 01 静态直接调用 | `List<i32>.echo(7)` | 编译、运行通过 | 编译、运行通过 |
| 02 目标及方法分别带泛参 | `List<i32>.keep<string>("method")` | 编译、运行通过 | 编译、运行通过 |
| 03 具体静态方法值 | `let f:Mapper<i32> = List<i32>.echo;`；另测 `List<i32>.keep<string>` | 编译、运行通过 | 编译、运行通过 |
| 04 泛型函数中的静态方法值 | `bindGeneric<E>()` 返回 `List<E>.echo` | 前端通过，发码报 CE0032 | 前端通过，发码报 CE0032 |
| 05 spec 静态调用 | `callSpec<E,A:EchoFactory<E>>` 调用 `A.echo(value)`，A 为 `List<E>` 的具体实例 | 编译、运行通过 | 编译、运行通过 |
| 06 spec 静态方法值 | `bindSpec<E,A:EchoFactory<E>>` 返回 `A.echo`，随后调用 | 编译、运行通过 | 编译、运行通过 |

场景 05、06 是此前第五处静态 witness 检查所涉及能力的两种使用方式，不代表新增了
一个 CE0144 报错位置。各成功场景覆盖 `i32` 和 `string`；场景 02 还覆盖独立的
`i64` 方法实参与超出 32 位的值。

另外 5 份定位对照及最小复现见 §3。主矩阵 12 份与定位对照 5 份合计：13 份编译、运行
通过，4 份在发码阶段复现同一个 L01；另有 1 份不依赖标准库的最小 lib 复现。
这是本轮定向验证结果，未运行新的全量回归，也未修改编译器或现有测试。

## 3. 新发现 L01：fit 静态方法值依赖调用方泛参名称

**状态：已独立复现，尚未修复。** 该问题同时影响本地泛型类型和导入的标准库 `List<T>`。
以下完整源码不依赖标准库即可复现：

```feng
module review.minimal_fit_shared_value;

type List<T> {}
spec Mapper<T>(value: T): T;

fit List<T> {
    static func echo(value: T): T { return value; }
}

func bindGeneric<E>(): Mapper<E> {
    return List<E>.echo;
}

func run(): i32 {
    let mapper = bindGeneric<i32>();
    return mapper(7);
}
```

语义检查通过；发码在 `List<E>.echo` 处报：

```text
CE0032: codegen: unknown type 'E'
```

已完成以下对照：

| 对照 | 结果 | 支持的结论 |
|---|---|---|
| 标准库 List 与本地类型均将函数泛参 E 改名为 T | 两份均编译、运行通过 | 当前发码结果错误地依赖不同作用域泛参是否同名 |
| 本地类型保留 E，去掉 fit 的 spec 关系 | 仍报 CE0032 | 复现不要求 spec 关系 |
| 本地类型保留 E，去掉容器字段 | 仍报 CE0032 | 复现不要求容器存储实现 |
| 本地类型保留 E，将方法直接声明在 type 中 | 编译、运行通过 | 已复现差异集中在 fit 静态方法值路径 |

泛参改名不应改变这段代码的合法性。结合前端通过、同名对照通过和 type 自有方法对照
通过，可以确认这是合法程序的发码缺陷，应修复泛参上下文处理并补用例；不能通过拒绝
该源码或仅更换诊断码处理。具体遗漏位置仍需在修复时沿方法值依赖与 fit 实例化路径定位。

## 4. 现有用例覆盖

在 `test/`、`fcts/`、`std/std_test/` 中未找到直接针对标准库 `List<T>` 执行上述静态
扩展场景的常驻用例；已有等价泛型类型的用例如下。表中“已有”仅表示对应形态存在，
不表示覆盖了所有泛参命名与作用域组合。

| 场景 | 现有编译器用例 | 现有 FCTS 用例 |
|---|---|---|
| 01 泛型目标的 fit 静态直接调用 | `test_generic_static_methods_codegen`，包含 `Box<i32>.of(2)` | `test_generic_fit_member_dependency.ff` 调用泛型 fit 的 `transform` |
| 02 目标及方法分别带泛参 | `test_generic_owner_method_constraint_codegen`，包含 `FitHost<int>.echo<FitValue<int>>(2)` | `test_generic_owner_method_constraint.ff` 覆盖本地及跨包泛型 fit 静态泛型方法 |
| 03 具体静态方法值 | `test_concrete_static_method_value_codegen_uses_singletons` | `test_static_method_value.ff` 覆盖本地、跨包、目标泛参与方法泛参 |
| 04 泛型函数中的方法值 | 上述方法值用例中的 `makeFit<T>` | `makeStaticFitMapper<T>` 和跨包 `makeLibStaticFitMapper<T>`；调用方与目标泛参均名为 T |
| 05、06 spec 静态成员适配及方法值 | `test_constrained_generic_spec_static_method_value_codegen`、泛型 fit 成员依赖用例 | `test_generic_spec_implementation.ff` 覆盖泛型 fit 静态 witness；`test_spec_static_method_value.ff` 覆盖约束静态方法值 |

主要入口：

- [Codegen 用例](../../test/codegen/test_codegen.c)。
- [静态方法值 FCTS](../../fcts/fcts_bin/src/test_static_method_value.ff) 和
  [跨包 provider](../../fcts/fcts_lib/src/test/lib_static_method_value.ff)。
- [泛型目标及方法约束 FCTS](../../fcts/fcts_bin/src/test_generic_owner_method_constraint.ff)。
- [泛型 fit 成员依赖 FCTS](../../fcts/fcts_bin/src/test_generic_fit_member_dependency.ff)。
- [泛型 spec 实现 FCTS](../../fcts/fcts_bin/src/test_generic_spec_implementation.ff)。
- [spec 静态方法值 FCTS](../../fcts/fcts_bin/src/test_spec_static_method_value.ff)。

L01 暴露的覆盖缺口是：目标声明使用 T，调用方使用 E 等不同名称，并在泛型函数中
形成该目标的 fit 静态方法值。现有正例使用同名 T，未覆盖本轮失败形态。

## 5. 对数组待判定项的结论

依据开发者确认的类比规则，上述静态访问能力对 `T[]` 也应合法。当前
`i32[].echo(...)`、`i32[].keep<U>(...)`、`i32[].echo` 和 `E[].echo` 在语法阶段报
SE0006，是待补齐的数组类型静态访问能力，不能归为语言禁止。

通过 spec 约束访问泛型数组静态方法的源码已通过语义、Codegen 和生成 C 的编译检查。
代码会先进入 `cg_emit_generic_builtin_spec_method_thunk`；该路径不会触发普通静态
thunk 中保留的 CE0144 检查。仍需在补齐数组直接访问后验证其实际发码路径，不能仅凭
现有 5 处 guard 就断言有 5 个独立缺陷，或将它们全部判为可直接更换错误码的 IE。

## 6. 本轮验证产物与复现

临时源码、输出及结果记录位于 `build/ce0144-review-20260911/list_cases/`；其中
`results.json` 记录主矩阵和定位对照的编译、运行结果。该目录不纳入常驻回归，可能被
后续 `make test` 清理。最小失败源码已完整保存在 §3，便于重新建立复现。

标准库场景 01 示例命令（在工程根目录运行）：

```sh
build/bin/feng build/ce0144-review-20260911/list_cases/std_01_direct.ff --pkg=std/std/build/pkg/std-0.1.0.fb --out=build/ce0144-review-20260911/list_cases/std_01_direct --name=std_01_direct --keep-ir
build/ce0144-review-20260911/list_cases/std_01_direct/bin/std_01_direct
```

将 §3 源码保存为 `build/ce0144-review-20260911/list_cases/minimal_fit_shared_value.ff` 后：

```sh
build/bin/feng tool semantic --target=lib build/ce0144-review-20260911/list_cases/minimal_fit_shared_value.ff
build/bin/feng tool compile --target=lib --emit-c=build/ce0144-review-20260911/list_cases/minimal_fit_shared_value.c build/ce0144-review-20260911/list_cases/minimal_fit_shared_value.ff
```
