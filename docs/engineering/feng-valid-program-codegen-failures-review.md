# Feng 合法程序发码失败问题汇总

> **状态**：已 Review，修复、用例和全量回归完成（2026-09-11）；第 4 节保留修复前的复现证据。
>
> **性质**：独立编译器问题分析记录。语言行为以第 2 节引用的主规范为准，本文不新增或修改语言规则。
>
> **复现基线**：`86f69489a3623dd5921d799d11078cd5c30169b6`
> （`fix: harden generic conformance and complete G25 coverage`）。
> 复现环境：macOS arm64，`feng 0.1.0`，Homebrew Clang 21.1.8。

## 1. 结论与范围

复现基线中已确认 **6 组合法程序无法完成编译的场景**：4 组在 Codegen 阶段报告 CE，
另 2 组 Codegen 返回成功，但生成的 C 无法通过宿主编译器检查。
所有场景均已通过 Parser 和 Semantic。

| 编号 | 场景 | Semantic | Codegen | 生成 C 检查 |
|---|---|---|---|---|
| 01 | 内建类型 fit 的泛型静态方法返回 `T`，直接调用 | 通过 | `CE0237` | 未进入 |
| 02 | 同类方法返回 `T[]`，局部绑定省略类型标注 | 通过 | `CE0001` | 未进入 |
| 03 | 同类方法体构造并返回普通 `Box<T>` | 通过 | `CE0007` | 未进入 |
| 04 | 内建类型 fit 的泛型静态方法形成方法值并调用 | 通过 | 通过 | 返回值 C 类型不匹配 |
| 05 | 内建类型 fit 的泛型静态方法返回 `@value Box<T>` | 通过 | 通过 | 描述符 C 类型不匹配等错误 |
| 06 | 65 个需要前置临时值的 `else if` 分支 | 通过 | `CE0267` | 未进入 |

这里的 6 组是**复现场景数**，不是已经去重确认的独立根因数。01—05 集中在内建类型
fit 的泛型静态方法相关路径；01、02 有共同的返回类型替换遗漏证据，其他场景仍需沿各自
失败路径确认完整根因。本文也不声称穷举了全部合法程序的发码缺陷。

本文聚焦这批已复现问题。非法源码的前端漏检、尚未确认合法可达入口的 CE0144，以及
全量 CE → IE 迁移，继续作为独立工作处理。

## 2. 规范依据

下列主规范提供这些程序的合法性和期望行为依据，本文仅引用其相关部分：

| 适用场景 | 主规范及相关内容 |
|---|---|
| 01—05 | [内建类型 fit 规范](../specifications/feng-fit-builtin-type.md) §2、§3：内建目标范围及通用 fit 语义 |
| 01—05 | [fit 规范](../specifications/feng-fit.md) §4、§5：泛型成员、静态方法及与类型成员一致的调用规则 |
| 01—05 | [泛型规范](../specifications/feng-generics-draft.md)：类型实参替换及泛型调用 |
| 02 | [绑定规范](../specifications/feng-binding.md)：从初始化器推导绑定类型 |
| 03、05 | [类型规范](../specifications/feng-type.md)：普通类型、值类型及其泛型实例的值语义 |
| 04 | [函数规范](../specifications/feng-function.md) §4：显式闭合泛型方法值，静态方法值与直接调用的签名一致性 |
| 06 | [流程控制规范](../specifications/feng-flow.md) §2：普通条件判断 |

因此，这批问题的修复目标是让合法程序正确编译、运行。只将 CE 改成 IE，或增加前端
错误来拒绝这些程序，均不能视为完成修复。

## 3. 复现方法

下列示例均是独立文件，不需要第三方包。先在工程根目录创建复现目录，将各节源码保存为
对应文件名；场景 06 使用其生成命令创建文件。

```sh
mkdir -p build/legal-codegen-review
```

以场景 01 为例，分别执行 Semantic 和 Codegen：

```sh
build/bin/feng tool semantic --target=lib build/legal-codegen-review/static_scalar_generic_method.ff
build/bin/feng tool compile --target=lib --emit-c=build/legal-codegen-review/static_scalar_generic_method.c build/legal-codegen-review/static_scalar_generic_method.ff
```

Codegen 成功后，继续检查生成的 C：

```sh
clang -Isrc -Isrc/runtime -Ithird_party/miniz -std=gnu11 -fexceptions -Werror -fsyntax-only build/legal-codegen-review/static_scalar_generic_method.c
```

其他场景替换文件名即可。不要检查 Codegen 失败后遗留的旧 C 文件。
`tool compile` 本身不会完成这一步宿主 C 检查。

基线失败样例未进入运行阶段；第 4 节的运行结果为**修复后的期望行为**，实施验证结果见第 8 节。

## 4. 已复现场景

### 01. 泛型静态方法返回 T，直接调用报 CE0237

文件：`static_scalar_generic_method.ff`。

```feng
module audit;

fit i32 {
    static func keep<T>(value:T):T {
        return value;
    }
}

func use():i32 {
    return i32.keep<i32>(1);
}
```

Semantic 返回 0，Codegen 返回 1，报错位于 `keep` 调用：

```text
CE0237: codegen: missing generic descriptor for erased storage
```

省略调用点的显式类型实参也已复现。文件：`static_generic_inferred_call.ff`。

```feng
module audit;

fit i32 {
    static func keep<T>(value:T):T {
        return value;
    }
}

func use(value:i32):i32 {
    return i32.keep(value);
}
```

该变体同样在 Semantic 通过后报告 CE0237。

**已确认原因**：`cg_emit_generic_static_method_call` 的 builtin fit 分支通过
`cg_instantiate_builtin_fit_return_type` 计算返回类型。该 helper 在
`bf->target_type_param_count == 0` 或 `receiver_element == NULL` 时直接 clone 返回类型。
`fit i32` 没有目标类型参数，但方法自身有 `T`；当前路径没有使用方法级实参完成
`T → i32` 替换，随后把返回值按开放泛型存储处理，在调用方查找不到对应 descriptor。

定位：[codegen.c](../../src/codegen/codegen.c) 中
`cg_emit_generic_static_method_call`（基线 28998—29035 行）、
`cg_instantiate_builtin_fit_return_type`（12180 行）、
`cg_emit_erased_generic_storage_declaration`（20580 行）。
同一静态调用函数的其他 owner 分支使用 `cg_resolve_selected_callable_return_type`，可作为
后续统一返回类型解析的检查入口。

**期望**：显式调用编译、运行后返回 `1`；推导调用保持输入的 `i32` 值。

### 02. 泛型静态方法返回 T[]，推导绑定报 CE0001

文件：`static_generic_array_result.ff`。

```feng
module audit;

fit i32 {
    static func wrap<T>(value:T):T[] {
        return [value];
    }
}

func use():i32 {
    let xs = i32.wrap<i32>(1);
    return xs[0];
}
```

Semantic 返回 0，Codegen 返回 1，报错位于 `xs` 绑定：

```text
CE0001: codegen: missing debug display type
```

**已确认原因**：该调用经过场景 01 的同一返回类型解析分支，将开放的 `T[]` 保留到
Codegen 局部绑定类型。`cg_debug_add_variable_record_slice_cgtype` 无法生成它在当前调用方
上下文中的显示类型，最终在基线 3190 行报错。虽然报错来自调试元数据，源头仍是调用结果
没有正确闭合；不能通过关闭或跳过调试信息生成来完成修复。

**期望**：`xs` 在调用点对应 `i32[]`，元素读取正确，`use()` 返回 `1`。

### 03. 泛型静态方法构造普通 Box<T>，报 CE0007

文件：`static_generic_object_result.ff`。

```feng
module audit;

type Box<T> {
    let value:T;
}

fit i32 {
    static func wrap<T>(value:T):Box<T> {
        return Box<T> { value:value };
    }
}

func use():i32 {
    let x = i32.wrap<i32>(1);
    return x.value;
}
```

Semantic 返回 0，Codegen 返回 1，报错位于方法体的 `Box<T>` 对象字面量：

```text
CE0007: codegen: no reified_type_dep found for generic managed dependency 'FengTypeDesc__audit__Box__G__T__CTX__T'
```

**已确认事实**：`cg_rtd_expr_for_managed_descriptor` 查找当前泛型上下文的 managed
descriptor 依赖失败，出口位于 [codegen.c](../../src/codegen/codegen.c) 基线 3628 行。
失败发生在方法体发码，早于调用方读取返回对象字段。

**待继续定位**：完整原因位于依赖收集、上下文传递还是依赖身份匹配，当前证据尚不能
区分。不能将它直接合并为场景 01 的返回类型替换问题，也不能仅在 descriptor 查找处
添加按名称兜底。

**期望**：方法体能构造相应泛型对象，调用结果闭合为 `Box<i32>`，`use()` 返回 `1`。

### 04. 泛型静态方法值发码成功，但生成 C 的返回类型不匹配

文件：`static_scalar_generic_method_value.ff`。

```feng
module audit;

spec Mapper(value:i32):i32;

fit i32 {
    static func keep<T>(value:T):T {
        return value;
    }
}

func use():i32 {
    let f:Mapper = i32.keep<i32>;
    return f(1);
}
```

Semantic 和 Codegen 均返回 0，Clang 返回 1：

```text
error: incompatible pointer to integer conversion returning 'void *' from a function with result type 'int32_t' (aka 'int')
```

**已确认事实**：生成的 `keep_G` 实现声明返回 `void *`，而对应方法值的调用适配函数
声明返回 `int32_t`。适配函数直接 `return keep_G(...)`，未完成两者返回表示的适配。

基线生成 C 中的定位为：被调用函数声明第 120 行，适配函数第 166—168 行。
这些行号对应未格式化的原始复现源码；按本文源码重新生成时，行号可能变化。

**待继续定位**：需同时检查共享方法的返回 ABI 与闭合方法值适配函数，确认责任边界，
不能只对返回表达式增加 C 强制转换。

**期望**：方法值与直接调用结果一致，`use()` 返回 `1`。仅 Codegen 返回成功不能作为
该场景的验收依据。

### 05. 泛型静态方法返回 @value Box<T>，描述符 C 类型不匹配

文件：`static_generic_value_result.ff`。

```feng
module audit;

@value type Box<T> {
    let value:T;
}

fit i32 {
    static func wrap<T>(value:T):Box<T> {
        return Box<T> { value:value };
    }
}

func use():i32 {
    let x = i32.wrap<i32>(1);
    return x.value;
}
```

Semantic 和 Codegen 均返回 0，Clang 返回 1。其中一类错误为：

```text
error: incompatible pointer types passing 'const FengTrivialDescriptor *' to parameter of type 'const FengAggregateDescriptor *'
```

**已确认事实**：生成代码把
`Feng__audit__Box__G__T__CTX__T__aggregate_desc` 声明为 `FengTrivialDescriptor`，
又将其地址传给 `feng_aggregate_take`、`feng_aggregate_release` 等要求
`FengAggregateDescriptor` 的入口。调用方还保留了开放的 `Box<T>` 表示。

在原始复现生成 C 中，相关位置为描述符声明第 44 行、方法返回处理第 212 行、调用方
第 232—240 行。此处列出的是能够直接确认的错误，不是该文件全部 C 诊断的穷举。

**待继续定位**：需检查泛型值类型的具体布局、描述符类别、返回槽和调用方类型是否在
同一闭合上下文下产生。目前没有证据支持通过修改 runtime ABI 或放宽 C 类型检查解决。

**期望**：生成 C 通过检查，返回值采用正确的值类型表示和生命周期操作，`use()` 返回 `1`。

### 06. else-if 发码存在固定 64 个 wrapper scope 的容量限制

在工程根目录运行以下生成命令，得到 64 个和 65 个 `else if` 的对照样例：

```python
from pathlib import Path

root = Path("build/legal-codegen-review")
root.mkdir(parents=True, exist_ok=True)
for count in (64, 65):
    source = (
        "module audit;\n"
        "func probe(values:int[]):bool { return true; }\n"
        "func use() {\n"
        "if probe([1]) {}\n"
        + "\n".join("else if probe([1]) {}" for _ in range(count))
        + "\n}\n"
    )
    (root / f"if_wrappers_{count}.ff").write_text(source)
```

两个样例均通过 Semantic。64 个 `else if` 的样例通过 Codegen 和 Clang；65 个的样例
在 Codegen 报错：

```text
CE0267: codegen: too many nested else-if wrappers
```

**已确认原因**：`cg_emit_if` 使用固定数组 `wrapper_scopes[64]` 保存生成 C 时需要的
嵌套作用域。条件 `probe([1])` 会产生前置临时值，每个相应 `else if` 增加一个 wrapper
scope，第 65 个超过容量。限制针对的是需要这些 wrapper 的分支，不是任意 65 个
`else if` 都会触发。

定位：[codegen.c](../../src/codegen/codegen.c) 中 `cg_emit_if`，基线 40600—40601 行
定义固定容量，40660—40664 行检查并报错。

**期望**：上述合法分支链均可编译。修复应让编译期作用域记录随实际需要增长，保持主规范
要求的条件求值顺序和临时值生命周期；仅调大固定常量仍会保留同类问题。

## 5. 修复方向与验收建议

建议先处理 01—05 的泛型方法相关路径，按实际根因收敛修复，再处理 06 的编译期容量限制。
已确认的共同问题是 01、02 的方法级返回类型替换遗漏；03—05 的完整根因应在实施前继续
定位并记录，不预先认定为同一个补丁即可解决。

实现应复用通用的类型替换、依赖登记、值表示及调用适配机制，不针对 `i32`、`Box`、
单一 CE 编号或固定样例添加特判。若涉及运行时开销增加、runtime 私有 ABI 变更，或必须
修改已有测试，应按 [AGENTS.md](../../AGENTS.md) 交由开发者决策。

| 验收层次 | 需要验证的结果 |
|---|---|
| 编译器测试 `test/` | 本文全部场景通过 Semantic、Codegen；生成 C 通过宿主编译器检查；按修复点检查闭合类型、依赖和存储/调用表示 |
| 泛型相关回归 | 覆盖显式和可推导调用、直接调用和方法值、`T` / `T[]` / 普通及值类型 `Box<T>` 返回；保留修复涉及的既有合法路径对照 |
| 分支容量回归 | 覆盖容量边界前后及更长的分支链，区分有无前置临时值的条件 |
| 语言行为 `fcts/` | 验证上述返回值、字段和数组元素读取；按流程控制和生命周期规范验证分支条件的按需求值、临时值释放 |
| 全量回归 | 非文档修复完成后，在 Codex 沙箱外执行 `make test` |

修复后的检查链应覆盖 **Semantic → Codegen → 生成 C 编译 → Feng 行为**。
场景 04、05 已证明，只检查 CE 是否消失会漏掉仍然存在的发码错误。

## 6. 分析文档交付状态

- 已记录上述 6 组失败场景、场景 01 的推导调用变体和场景 06 的容量边界对照。
- 已在各场景中区分规范期望、实测结果、已确认原因与待进一步定位事项。
- 已从本文提取全部复现源码重新验证，结果与表格一致；已检查文档引用和 Markdown 结构。
- 分析阶段仅新增本文档，未实施修复，未修改已有测试。
- 文档变更按项目约定不要求全量回归；后续代码修复仍需完成第 5 节的验证。

## 7. 修复 Todo

- [x] 完成问题 Review，确认按现有语言规范修复合法程序。
- [x] 修复 01、02：统一内建类型 fit 泛型静态调用的返回类型替换。
- [x] 定位并修复 03：普通泛型对象的 reified descriptor 依赖。
- [x] 定位并修复 04：泛型静态方法值的返回表示与调用适配。
- [x] 定位并修复 05：泛型值类型返回的布局、描述符和返回槽。
- [x] 修复 06：移除 else-if wrapper scope 的固定容量限制。
- [x] 对过程中发现的新问题，先记录事实，再分析修复并补充用例；不确定事项交由开发者决策。
- [x] 新增编译器用例，覆盖上述场景、类型闭合及生成 C 的合法性。
- [x] 新增 FCTS 用例，覆盖泛型返回值、方法值、分支求值及生命周期行为。
- [x] 重新验证本文复现与边界对照。
- [x] 在沙箱外执行全量 `make test`。
- [x] 记录修复结果、测试结果及尚存限制，更新 Todo 完成状态。

## 8. 实施记录

### 8.1 已定位的修复点

- 01、02：静态调用改用语义选中 callable 的通用返回类型替换结果，保留方法级类型实参。
- 03：内建 fit 的 managed 依赖映射仅在 fit 目标有泛参时建立，遗漏只有方法级泛参的
  情况；静态直接调用构造函数描述符时也遗漏了 builtin fit 的依赖 owner。方法体统一复用
  `cg_activate_reified_dependency_mapping`，调用方使用实际 fit 声明的成员依赖集。
- 04、05：`cg_builtin_fit_return_uses_out` 只识别 fit 目标泛参，导致方法级开放返回值
  未使用已有返回槽协议；同一判定须覆盖全部活动泛参，直接调用和方法值适配继续共用它。
  05 的 aggregate 映射还仅按目标泛参排序，并按 origin 选取开放实例；改用通用映射后，
  使用目标及方法泛参的完整顺序和依赖类型身份。
- 06：wrapper 作用域已经通过 `Scope.parent` 形成栈，直接复用此栈回退到进入 `if` 时
  的作用域即可，不再额外保存固定数组；失败路径同样回退并释放编译期作用域。

以上修改复用现有共享调用、返回槽和描述符协议；不变更 runtime 私有 ABI，不增加
运行时查找、装箱或分派层。验证结果见第 8.4 节。

### 8.2 修复中新增的复现

以下场景均通过 Semantic；失败发生于生成 C 的检查阶段。

| 编号 | 最小触发形态 | 实测结果及分析入口 | 状态 |
|---|---|---|---|
| N01 | 场景 03 修复依赖映射后，直接返回 `Box<T>`；或 `relay<U>` 转调同一方法 | 共享入口返回 `Box<T>*`，调用方接收 `Box<i32>*` / `Box<U>*`，C 指针类型不一致；需统一共享入口的引用返回表示 | 已修复，全量回归通过 |
| N02 | `@value type Box<T>{let value:T;}`；`fit i32{static func pass<T>(value:Box<T>):Box<T>{return value;}}`，传入 `Box<i32>` | 调用方按地址传参，入口却声明按值接收开放结构体；入口参数判定只处理裸 `T`，遗漏开放值类型 | 已修复，全量回归通过 |
| N03 | `fit A[]{func pick<B>(value:B):Box<B>{return Box<B>{value:value};}}`，对 `i32[]` 调用 `pick<i32>(7)`，其中 `Box` 为值类型 | 入口要求目标及方法两个 descriptor，调用点只传目标 descriptor；函数描述符和返回类型也未完成方法级替换 | 已修复，全量回归通过 |

新增用例须覆盖这些输入以及开放调用方的转发，不能只验证原有最小场景。

处理方案：N01 的开放引用返回在共享 C 入口使用中性 `void *`，保持原有直接返回和
引用计数协议；调用方仍持有已替换的具体 Feng 类型。N02 的入口声明与定义复用
`cg_shared_generic_param_uses_address`。N03 的方法级泛型调用接入与静态方法相同的
发码路径，增加接收者及目标泛参的输入，统一处理目标、方法两层替换及描述符绑定；
已有无方法泛参的调用路径保持原有协议。

用例补充时确认 N02 也影响方法值：闭合 `CopyValue<T>` 适配函数仍把开放 aggregate
参数按值解引用后传入共享入口。其参数判定同样改为通用地址协议，新增直接调用和方法值
两条验收路径。

### 8.3 运行阶段新增问题 N04

编译器回归（包含生成 C 检查）已通过，但新增 FCTS 中，标量测试通过后，大型平凡元组
在数组、普通对象、值类型返回及其参数传递的组合测试中触发 signal 10，尚未执行完断言。
这是实际运行失败，不能以“C 编译通过”关闭任务。继续缩小到具体调用并核对数据布局、
传参和生命周期，再补充对应回归证据。

调用栈已定位到 `vcgPass` 的 aggregate retain。生成 C 实际执行
`memcpy(_out, (void *)&(value), descriptor->size)`，复制的是参数指针所在位置，
而不是它指向的完整 aggregate。原因是 builtin fit 参数已改为共享地址协议，但局部
参数元数据未标记为 storage address。补齐通用 `cg_mark_last_shared_address_parameter`
调用，使返回、字段访问和后续传参统一识别该存储；保留大型平凡及托管实际类型的运行用例。

N04 修复后，同一 FCTS 测试通过，普通返回和异常传播均验证实际托管对象恰好释放一次。

### 8.4 用例和验证结果

- [编译器用例](../../test/codegen/test_valid_program_codegen.c)：两组泛型方法组合，
  覆盖直接调用、推导、方法值、开放转发和目标／方法两层泛参；分支链覆盖
  63、64、65、128 个需 wrapper 的 `else if`，以及 160 个交替条件的混合链。
  所有源码均通过语义分析、Codegen 和宿主 C 编译，并检查地址参数不会被二次取址后返回。
- [泛型行为用例](../../fcts/fcts_bin/src/test_valid_codegen.ff) 与
  [跨包 provider](../../fcts/fcts_lib/src/test/lib_valid_codegen.ff)：新增 7 项，
  覆盖标量、字符串、大型平凡及托管实际类型、数组、普通和值类型返回、方法值及生命周期。
- [长分支链行为用例](../../fcts/fcts_bin/src/test_long_if_codegen.ff)：新增 3 项，
  验证 96 个 `else if` 的求值顺序、短路、最终 `else`，以及正常和异常退出时的逆序清理。
- 已有用例内容和断言未修改，仅在编译器测试入口和 FCTS 入口注册新增用例。
- 12 个独立复现及对照程序重新通过 Semantic → Codegen → Clang 检查。
- `make fcts-tests`：1384 项通过，0 失败，0 跳过；包含上述新增 10 项。
- `make test`：2026-09-11 在沙箱外完成，退出码 0。macOS UBSan 和普通构建两轮
  全量回归均通过，FCTS 每轮均为 1384/1384；编译器、运行时、CLI、符号、标准库、
  性能约束、增量构建、发布脚本、内置包及工具链检查全部通过，未出现 UBSan 运行时诊断。

本次已完成 01—06 和实施中记录的 N01—N04，暂无待处理的已复现问题或需人工决策项。
该结论限于本文范围；第 1 节已排除的前端诊断和全量 CE → IE 迁移仍属独立工作。
