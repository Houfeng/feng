# 泛型容器静态成员访问对照验证

本文记录 `List<T>` 与 `T[]` 在静态成员访问上的对照验证，关联
[合法程序发码失败 Review](./feng-valid-program-codegen-failures-review.md)。

## 1. 判定依据

开发者确认：`T[]` 是泛型容器类型；若对应的 `List<T>` 静态成员访问合法，数组目标也应合法。
本轮验证直接调用、方法级泛参、具体方法值、泛型函数中的方法值，以及 `spec` 静态成员适配。
通用规则引用 [fit 规范](../specifications/feng-fit.md)、
[内建类型 fit 规范](../specifications/feng-fit-builtin-type.md) 和
[函数规范](../specifications/feng-function.md)，本文不另行定义语言规则。

## 2. 修复前实测结果

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
以上为修复前的定向验证结果；当时未运行新的全量回归，也未修改编译器或现有测试。
后续实施、常驻用例和回归结果见 §7—§9。

## 3. 新发现 L01：fit 静态方法值依赖调用方泛参名称

**状态：已修复，全量回归通过；实现和用例见 §9。以下保留修复前复现。**
该问题同时影响本地泛型类型和导入的标准库 `List<T>`。
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

## 4. 修复前已有用例覆盖

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

## 5. 修复前数组待判定项的结论

依据开发者确认的类比规则，上述静态访问能力对 `T[]` 也应合法。当前
`i32[].echo(...)`、`i32[].keep<U>(...)`、`i32[].echo` 和 `E[].echo` 在语法阶段报
SE0006，是待补齐的数组类型静态访问能力，不能归为语言禁止。

通过 spec 约束访问泛型数组静态方法的源码已通过语义、Codegen 和生成 C 的编译检查。
代码会先进入 `cg_emit_generic_builtin_spec_method_thunk`；该路径不会触发普通静态
thunk 中保留的 CE0144 检查。仍需在补齐数组直接访问后验证其实际发码路径，不能仅凭
现有 5 处 guard 就断言有 5 个独立缺陷，或将它们全部判为可直接更换错误码的 IE。

## 6. 修复前验证产物与复现

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

## 7. 实施 Todo

开发者已要求修复 L01、补齐实际 List<T> 的常驻用例，并完整支持对应数组静态访问。
本节记录后续实施进度；§2—§6 保留修复前的验证事实。

- [x] 将数组静态访问及目标／方法泛参独立替换要求收敛到主规范。
- [x] 修复 L01：开放 fit 保持声明的共享 ABI 泛参域，在调用点按语义记录绑定实际类型，保留调用方泛参身份。
- [x] 用结构化类型目标贯通数组静态访问的解析、语义、发码及跨包泛型依赖。
- [x] 补齐普通与泛型静态直接调用、具体与开放方法值，覆盖 `[]`、`[!]`、嵌套元素及目标／方法泛参组合。
- [x] 新增实际标准库 List<T> 的六类常驻行为用例，覆盖同名与异名泛参。
- [x] 新增编译器用例验证 AST、语义选取、生成 C；新增 FCTS 验证数组行为、跨包及生命周期。
- [x] 新发现的问题先记录，再定位、修复及补用例；本轮未引入需新增语言或运行时决策的方案。
- [x] 在沙箱外运行全量 `make test`，记录最终结果。

实现采用通用类型目标和泛参上下文处理，不为 List 或某个数组元素类型增加特判；复用
现有描述符、返回槽、静态方法值和 witness 协议，不修改 runtime 私有 ABI，也不增加
运行时查表、装箱或分派层。已有用例保持不变，以新增文件及入口注册补充覆盖。

## 8. 扩展验证记录

- L02（2026-09-11，新发现）：调用方泛参恰好与方法泛参同名时，合法调用被语义阶段
  误拒绝。`fit T[] { static func own<U>(value:T, unused:U):T { return value; } }`
  配合 `func relay<U>(value:U):U { return U[].own<string>(value,"unused"); }`
  报 AE0512；替换为 `type List<T>` 与 `List<U>.own<string>(...)` 同样复现。
  已定位：先替换目标 T 为调用方 U，再替换方法 U 为 string，错误捕获了调用方名称。
  Semantic 的替换与推导、Codegen 的实际签名解析均改为同一次遍历绑定目标和方法
  两组声明参数。只修 Semantic 仍会生成错误的参数／返回表示，须由行为用例一起验证。
  两种容器的显式、推导调用及大于机器字的实际类型均已通过。
- L03（集成中记录的跨文件类型上下文问题）：新增跨包回归时，构建整个 fcts_lib
  报 CE0031，提示已有 `LibAdvancedComposite<...>` 未注册，诊断路径落在新增
  provider 文件。泛型实例的实际参数副本未完整保留来源文件，代入另一个文件的 fit
  成员后，收集和解析实例可能使用错误的命名空间。实例参数现在保留来源，收集阶段也
  优先使用该来源。开放 fit 成员同时保持声明的有限泛参域，避免将每个调用方上下文
  当成新的共享 ABI。独立三模块用例覆盖“类型声明、fit 声明、含私有泛型实参的调用方”
  分属不同文件的形态，完整 fcts_lib 构建通过。本项是修复过程的集成记录，未追加
  计入 §2 的修复前失败数量。
- L04（新增数组静态发码用例发现）：`fit T[]` 中声明
  `static func first(values:T...):T { return values[0]; }` 后，
  `i64[].first(11,13)` 前端通过，但报 CE0184。打包变长参数时仍使用方法声明中的
  开放元素类型，没有使用调用点已确定的目标／方法实参。已在共用泛型方法调用路径
  按实际类型打包，并复用已发码的实参，避免再次求值；新增用例检查目标泛参、方法泛参、
  开放转发、大型实际类型和副作用计数。
- L05（新增跨包行为用例发现）：数组静态方法的泛型约束依赖目标元素类型时，
  如 `fit T[] { static func read<U:Reader<T>>(value:U):T { return value.read(); } }`，
  生成 C 可通过编译，但实际调用出现 SIGSEGV。已定位：具体对象实参绕过了约束
  实例化，生成的 witness 仍对应开放 `Reader<T>`，成员未填充。现在所有具体实参
  都先闭合约束并注册其成员，开放泛参转发继续使用已有描述符与约束投影协议。
  显式及推导直接调用、方法值和跨包泛型转发均已通过运行验证。

## 9. 修复与常驻验证结果

### 9.1 实现结果

- L01：开放泛型 fit 的成员类型保留原始声明参数，供共享入口使用；具体调用和方法值
  依据 Semantic 记录的目标及方法实参完成绑定。fit 的 spec 关系代入实际类型后使用
  实例的调用方上下文，保留泛参名称和顺序。`E`、`T` 同名对照和 `A,E` 多参数上下文
  均通过，修复不依赖改名。
- 数组类型目标：AST 保存完整类型引用，贯通 Semantic、泛型依赖、Codegen 和 LSP
  的类型引用遍历；`T[]`、`T[!]` 和嵌套结构不会被拆成索引或数组构造表达式。
- 数组静态发码：直接调用从类型目标取得元素描述符，方法值绑定目标和方法的全部
  描述符；沿用已有静态 witness 路径及返回、清理协议。
- 原有五处 CE0144 检查中，四处所涉调用／方法值路径已补齐；普通静态 witness thunk
  的兜底改为 IE0002，泛型内建目标由已有专用 thunk 处理。`src/codegen/` 已无 CE0144。
  这不表示完成其他 CE 错误码的整体分类或迁移。

### 9.2 常驻用例

| 层级 | 新增文件 | 覆盖内容 |
|---|---|---|
| Parser | [结构化类型目标](../../test/parser/test_structural_type_target.c) | 完整泛型及数组层级、可写标记、限定名；与索引及构造区分；非法表达式目标的 SE0201 |
| Semantic | [泛型容器静态成员](../../test/semantic/test_generic_container_static.c) | 精确 fit／成员／数组目标选取；8 类非法输入在前端终止并检查诊断码和位置 |
| Codegen | [泛型容器静态成员](../../test/codegen/test_generic_container_static.c) | 4 组源码矩阵通过 Semantic、生成 C 和宿主 C 编译；L01、L02、L03、L04、L05 及数组静态访问 |
| FCTS | [行为用例](../../fcts/fcts_bin/src/test_generic_container_static.ff) 与 [跨包 provider](../../fcts/fcts_lib/src/test/lib_generic_container_static.ff) | 18 项运行用例：真实 List 的六类场景、数组对应场景、泛参独立性、约束、可写与嵌套数组、完整返回布局、变长参数求值、正常及异常清理 |

List 的六类场景均覆盖本地 fit 与包内 fit；L01 同时覆盖同名和异名调用方泛参。
数组对应验证覆盖普通和泛型方法、直接调用和方法值，以及 spec 静态调用和静态方法值。
已有用例内容与断言未改动，只在原有测试入口注册新文件中的用例。

### 9.3 回归结果

- Parser、Semantic、Codegen 定向测试均通过；FCTS 为 1402/1402，0 失败、0 跳过。
- `make test` 于 2026-09-11 在沙箱外完成，退出码 0。macOS UBSan 与普通构建两轮
  全量回归均通过：标准库每轮 604/604，FCTS 每轮 1402/1402，均为 0 失败、0 跳过。
  编译器、运行时、CLI、符号、性能约束、增量构建、发布脚本、内置包和工具链检查通过，
  未出现 UBSan 运行时诊断。
- 本文范围内的 L01、数组静态访问缺口及实施中记录的 L02—L05 均已处理，暂无待修复的
  已复现问题或需人工决策项。该结论限于上述范围，不代表所有潜在发码缺陷已被排除。

## 10. Review 后补充：数组覆盖对称性与源码组织

### 10.1 核对结果

基线 `ef96ab9b` 中，`T[]` 与 `T[!]` 均已有静态直接调用、具体方法值、开放目标方法值、
spec 静态调用与方法值的运行用例，但覆盖并不对称。此前“补齐”的表述不足以表示两种
数组形态已分别覆盖全部组合，现按实际覆盖修正如下。

| 核对维度 | T[] 已有覆盖 | T[!] 已有覆盖／缺口 |
|---|---|---|
| 本地与跨包 fit | 两者都有 | 主要覆盖跨包 fit，缺本地 fit 对照 |
| 泛型函数中的方法值 | 目标泛参、目标与方法两组泛参 | 只有目标泛参，缺目标与方法两组泛参 |
| 泛参名称与顺序 | 已有 L02 及 List 的 L01 顺序对照 | 缺独立的同名／异名／多参数顺序矩阵 |
| 依赖元素类型的泛型约束 | 直接、推导、方法值及跨包转发 | 缺对应矩阵 |
| 变长参数 | 目标／方法泛参、转发、大型值、求值计数 | 缺对应矩阵；两种形态还应补显式展开路径 |
| 嵌套目标与完整返回布局 | 有外层 [] 的嵌套目标、对象和值返回 | 有可写数组返回，缺外层 [!] 的同等组合 |
| 生命周期 | 正常与异常清理 | 缺独立的对应验证 |
| 前端与生成 C | 两种目标均有基本选取；多数负例、发码组合使用 [] | 缺两种后缀分别运行的完整对照 |

`callable_instantiation.inc` 与 `callable_instance_inference.inc` 仅通过文本包含拆出
`analyzer.c` 的私有实现，没有形成独立编译模块。开发者确认本轮先将其结构体与
静态函数放回 `analyzer.c` 对应的类型替换、泛参推导区域，后续再拆分；删除这两个文件。
本次调整沿用现有私有接口及绑定算法。

### 10.2 补充 Todo

- [x] 记录现有覆盖事实与缺口，明确本次代码组织调整。
- [x] 移除两个 Semantic `.inc`，核对展开前后实现一致。
- [x] 新增独立 Parser、Semantic 和 Codegen 成对矩阵，分别验证 `[]` 与 `[!]`。
- [x] 新增本地及跨包行为用例，逐项覆盖上述缺口；保留已有用例与断言。
- [x] 新失败先记录、定位和修复，再增加回归用例；本次定向验证未发现新的功能失败。
- [x] 在沙箱外执行全量 `make test`，记录覆盖矩阵及结果。

### 10.3 新增覆盖及验证结果

[行为用例](../../fcts/fcts_bin/src/test_array_static_parity.ff) 与
[跨包 provider](../../fcts/fcts_lib/src/test/lib_array_static_parity.ff)
增加 11 组 × 2 种数组目标，共 22 项。每组两种目标均独立运行并检查结果：

| 场景 | T[] | T[!] |
|---|---|---|
| 本地／跨包普通及泛型直接调用、推导、无参和 void 调用 | 通过 | 通过 |
| 本地／跨包普通及泛型具体方法值 | 通过 | 通过 |
| 开放目标及方法泛参的方法值、同名／异名／多参数顺序 | 通过 | 通过 |
| 目标与方法泛参重名的显式／推导调用、大型及托管实际类型 | 通过 | 通过 |
| 本地／跨包 spec 静态调用与方法值 | 通过 | 通过 |
| 依赖元素类型的约束：直接、推导、方法值和跨包转发 | 通过 | 通过 |
| 嵌套只读／可写元素、具名泛型元素、内层可写数组身份 | 通过 | 通过 |
| 数组／普通对象／值类型返回、方法值及跨包布局 | 通过 | 通过 |
| 目标／方法变参、显式展开、方法值、跨包转发、空包、单次求值 | 通过 | 通过 |
| 托管结果正常返回后恰好释放一次 | 通过 | 通过 |
| 托管结果异常退出后恰好释放一次 | 通过 | 通过 |

补充的编译器用例分别位于：

- [Parser](../../test/parser/test_array_static_parity.c)：内外两层可写性的 4 种组合，
  每种同时检查调用和方法值 AST；两种后缀的非法值表达式目标均报 SE0201。
- [Semantic](../../test/semantic/test_array_static_parity.c)：8 类负例分别对两种后缀
  验证，共 16 次诊断检查，包含两种 fit 目标不能互相替代的对照。
- [Codegen](../../test/codegen/test_array_static_parity.c)：同一源码模板分别以 `[]` 和
  `[!]` 编译，覆盖上述泛型、约束、变参、嵌套及返回入口，并检查生成 C 的合法性。

两个 `.inc` 已删除；将基线中的原 include 逐处替换为文件内容后，与当前 `analyzer.c`
逐字一致。已有测试仅增加新用例的入口注册，原有源码与断言保持不变。

Parser、Semantic、Codegen 定向测试通过，FCTS 为 1424/1424，0 失败、0 跳过。
2026-09-11 在沙箱外完成全量 `make test`，退出码 0。macOS UBSan 与普通构建两轮
均通过：标准库每轮 604/604，FCTS 每轮 1424/1424，均为 0 失败、0 跳过。
编译器、运行时、CLI、符号、性能约束、增量构建、发布脚本、内置包与工具链检查通过，
未出现 UBSan 运行时诊断。本次补充 Todo 已完成。
