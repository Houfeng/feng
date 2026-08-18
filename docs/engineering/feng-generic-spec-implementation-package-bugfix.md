# Feng 泛型 `spec` 满足关系跨包修复开发文档

> 状态：草案，待 Review（2026-08-18）
>
> 本文档只处理泛型 `type` / 泛型 `fit` 已声明的 object-form `spec` 名义
> 满足关系在 package-public `.ft` / `.fb` 边界上的恢复和实现符号可链接性。
> 方法级泛型 requirement 与静态字段链接分别由其他专项处理。

## 1 依据与目标

本专项以以下权威规范为准：

- [Feng 语言 `spec` 规范](../specifications/feng-spec.md)；
- [Feng 泛型规范草案](../specifications/feng-generics-draft.md)；
- [Feng 包规范](../specifications/feng-package.md)；
- [Feng 符号表规范](../specifications/feng-symbol-table.md)；
- [Feng 语言 `fit` 规范](../specifications/feng-fit.md)。

需要保证下面两类 provider 声明在独立打包后仍可由 consumer 正确使用：

```feng
open spec Reader<T> {
  seal func read(value: T): T;
}

// 关系来源一：泛型 type 声明头。
open type DirectReader<T>: Reader<T> {
  seal func read(value: T): T {
    return value;
  }
}

// 关系来源二：泛型 fit。
open type FitReader<T> {}

open fit FitReader<T>: Reader<T> {
  seal func read(value: T): T {
    return value;
  }
}
```

consumer 只导入 `.fb` 时必须能够：

```feng
let direct = DirectReader<int>();
let directView: Reader<int> = direct;

let fitted = FitReader<int>();
let fittedView: Reader<int> = fitted;
```

并通过现有 witness 调用 provider 中声明期选中的实现方法。consumer 不得
读取 provider 源码、workspace-cache `.ft`，也不得重新进行无名义结构满足。

## 2 已确认的现状

### 2.1 现有覆盖并非空白

FCTS 已经覆盖：

- 泛型 object-form spec；
- `open fit Type<T>: Spec<T>`；
- 泛型 type 声明头上的父 spec；
- spec 参数、返回值、父视角和 witness 调用。

代表用例包括：

- `fcts/fcts_lib/src/test/lib_generic_fit_subject.ff`；
- `fcts/fcts_bin/src/test_generic_fit_subject.ff`；
- `fcts/fcts_lib/src/test/lib_generic_composition_coverage.ff`；
- `fcts/fcts_bin/src/test_generic_composition_coverage.ff`。

这些用例证明普通泛型 spec/fit 组合的主体能力存在，但不能替代一个严格的
“provider 先独立打包，consumer 只读取 package-public `.ft` 和归档库”的
CLI 回归。当前 `test/cli/test_cli.c` 中对应严格探针仍被 `#if 0` 禁用。

### 2.2 泛型 type 声明头关系在严格 `.fb` 边界丢失

已确认的最小场景为：

```feng
open spec Surface<T> {
  func value(input: T): T;
}

open type Value<T>: Surface<T> {
  func value(input: T): T {
    return input;
  }
}
```

provider 可以构建和打包；consumer 从 `.fb` 导入 `Value<int>` 后，将其赋给
`Surface<int>` 报 `AE1003`。该失败发生在 witness 或 seal 实现选择之前，说明
consumer 没有恢复出可用于闭合实例的完整名义关系证明。

目前只确认“关系在 package-public 消费路径上不可用”，尚未把根因武断归为
writer、reader、导入登记、泛型实例关闭或关系查询中的某一个函数。实施第一步
必须比较：

1. provider Semantic 中的开放关系模板；
2. package-public `.ft` 写出的 relation 与类型参数引用；
3. consumer reader 恢复的 AST/semantic relation；
4. `Value<int>` 关闭后查询 `Surface<int>` 的替换结果。

不得通过 consumer 使用点对成员重新做结构匹配来绕过名义关系丢失。

### 2.3 普通泛型 fit 关系可恢复

严格 `.fb` 探针确认，以下公开路径能够通过 Semantic、Codegen 和链接：

```feng
open fit FitValue<T>: Surface<T> {
  open func value(input: T): T {
    return input;
  }
}
```

因此“泛型 type owner”和“泛型 fit”不能被描述为同一个名义关系恢复故障。
泛型 fit 的普通公开实现不是本专项要重写的路径，只作为基线回归保留。

### 2.4 泛型 fit 的 seal 实现缺少跨包符号

把同一泛型 fit 的 requirement 和实现方法改为合法的 seal 组合后：

- consumer 能恢复 fit 名义关系；
- spec coercion 和 witness 选择能够通过；
- 生成 C 能引用相应 fit 实现 wrapper/shared body；
- 最终链接缺少稳定的 `FengFitMethod...` provider 符号。

这说明该问题不是 fit 关系不可见，也不是 witness 不存在，而是泛型 callable
package surface 仍只按普通公开或 `@mixable seal static` 分类，没有消费声明期
已经记录的 spec implementation dependency 事实。

泛型 type 自有 seal 方法在修复 2.2 的名义关系后也必须走同一通用分类规则；
不能只针对 `fit` 符号名称补一个导出分支。

## 3 正确性模型

### 3.1 声明时做结构证明

provider 分析以下声明时：

```feng
open type Value<T>: Surface<T> { ... }
open fit FitValue<T>: Surface<T> { ... }
```

必须在开放泛型作用域中完成现有结构满足检查，并记录：

- 关系来源及其 owner；
- target 类型模板；
- spec 类型模板及类型参数位置；
- 每个 requirement 选中的 type/fit 实现成员；
- 父 spec 闭包中的同类选择。

这一步继续应用公开 requirement 与 seal requirement 的既有实现可见性兼容
规则。非法关系必须在 provider 声明时拒绝，不能推迟到 consumer。

### 3.2 使用时做名义关系关闭

consumer 使用 `Value<int>` 或 `FitValue<int>` 时，只执行：

1. 查询当前位置可见的已声明名义关系模板；
2. 按 owner 类型参数位置把模板关闭为目标实例；
3. 精确比较关闭后的 spec 实例；
4. 取得构造既有 witness 所需的实现声明和链接信息。

consumer 不重新搜索同名成员、不重新做一般结构满足，也不因为 imported `.ft`
中恰好存在 seal 方法而建立新的关系。泛型实例继续保持不变性；这里的模板
替换不是泛型 variance。

### 3.3 泛型 fit 可见性

`fit Type<T>: Spec<T>` 只有在该 fit 按现有 module/import 规则对使用点可见时才
参与名义关系查询。关闭泛型 fit 模板不扩大 fit 的可见范围，也不允许外包自行
定义 spec/fit 后选择 imported type 的 seal 成员。

本专项不得把 fit 关系挂到 type 上成为无条件全局关系，也不得因为二进制中存在
实现符号就绕过 fit 可见面。

### 3.4 选中 seal 泛型方法的 package callable surface

对每个进入 package-public `.ft` 的名义关系，声明期选中的泛型实现方法必须
进入现有 package callable surface：

```text
package_callable(member) =
    existing_public_callable(member)
    or existing_mixable_seal_static_callable(member)
    or selected_exported_spec_implementation_dependency(member)
```

该谓词必须统一覆盖：

- 泛型 type owner 的实例方法和静态方法；
- 泛型 fit 的实例方法和静态方法；
- 使用 owner 泛参但没有方法级泛参的方法；
- 同时具有方法级泛参的方法（其 witness ABI 由独立专项提供）；
- 编译器生成且实际被满足选择选中的 wrapper；
- 父 spec requirement 的实现选择。

不得使用“seal 方法已经出现在 `.ft`”作为选中证明，因为 `.ft` 还可能因
`@mixable` 或 reified callable dependency 收录其他 seal helper。唯一授权来源仍是
provider 声明期的统一满足选择 sidecar。

### 3.5 稳定链接身份

被选中的 seal 泛型方法复用对应公开泛型方法的现有 shared body、薄 wrapper、
函数描述符和 reified dependency 路径。provider 与 consumer 必须只根据
package-public `.ft` 可恢复的声明事实得到一致符号身份：

- owner 类型参数和方法类型参数顺序；
- type/fit 来源身份；
- 实例/static 形态；
- 重载/成员稳定序号；
- 完整未实例化签名；
- reified aggregate/type/callable dependencies。

普通私有声明顺序、workspace-cache 中额外成员和 consumer 本地物化顺序不得影响
符号名称或序号。不得为 spec seal 泛型实现新建专用符号前缀或专用 thunk ABI。

### 3.6 `.ft` 边界

符号表规范已经要求：

- 公开泛型 type/spec/fit 的类型参数和未实例化关系进入 package-public `.ft`；
- 已导出关系选中的 seal 实现方法作为编译器依赖进入 `.ft` 并保持 seal；
- 泛型 callable 保留完整签名和 reified dependencies。

本专项优先修复这些既有通用事实的写入、读取或消费，不增加逐槽 witness plan、
spec seal 专用 flag、运行时 relation 表或新的结构满足机制。只有确认现有 wire
表示无法表达某项必需事实时，才允许先更新权威符号表规范并由人工 Review。

## 4 范围边界

### 4.1 本次包含

- `open type Owner<T>: Spec<T>` 及多类型参数、重排/嵌套类型实参；
- `open fit Owner<T>: Spec<T>` 的现有可见关系；
- 泛型 type/fit 中被公开关系选中的 seal 实例方法和静态方法；
- 父 object-form spec 闭包；
- 引用 type 与 `@value type` 的现有泛型 owner 路径；
- provider 独立 pack、consumer 只读 `.fb` 的编译、链接和运行。

### 4.2 本次不包含

- object-form spec requirement 自身声明方法级泛参时的解析、匹配和 witness ABI；
- spec static 字段实现的 storage/ensure 链接；
- 新增结构满足、variance、运行时关系查询或 witness 缓存；
- 修改 fit 导入/可见性规则；
- 修改 type/spec seal 访问、`@friend` 或 `@mixable` 授权；
- object-form spec 方法值；
- 与正确性无关的泛型单态化或 witness 性能优化。

## 5 性能与兼容性

- 关系关闭和实现选择全部在编译期完成；
- 生成程序不增加运行时名称查找、关系搜索、成员扫描、缓存、锁或分配；
- witness 布局和每次调用间接层级不变；
- 选中 seal 方法只改变 provider 符号链接能力，不改变其 Feng visibility；
- 普通公开泛型 type/fit 路径保持现有发码和开销；
- 未被导出关系选中的 seal helper 不得新增二进制公开符号。

如果正确修复需要修改 runtime ABI、增加运行时分支或把 fit 关系变为无条件全局
关系，必须暂停并由人工决策。

## 6 测试要求

### 6.1 编译器测试

- Semantic：provider 开放泛型 type/fit 声明期满足检查与选择 sidecar；
- Symbol：package-public `.ft` 对泛型 owner/fit relation、类型参数引用、选中 seal
  方法签名和 reified dependencies 的 round-trip；
- Imported Semantic：consumer 关闭 `Owner<int> -> Spec<int>`，验证直接 type 与
  可见 fit 两种关系；
- Codegen：泛型 type/fit 被选中 seal 实例/static 方法使用 package callable
  shared body 和稳定符号；
- CLI：provider 独立 `pack` 后，consumer 只依赖 `.fb` 完成 coercion、witness
  调用和链接。

负向覆盖必须包括：

- 未导入 fit 所在 module 时关系不可用；
- 类型实参不一致时不得通过；
- 无关普通 seal helper 不进入 package-public callable surface；
- consumer 外包自定义 spec/fit 不能选择 imported type seal 成员；
- `.ft` 中因 `@mixable` 或 reified dependency 存在的 seal 方法不被误当作满足选择。

### 6.2 FCTS

在 `fcts_lib -> fcts_bin` 增加独立行为组，至少覆盖：

- 泛型 type 声明头关系；
- 泛型 fit 关系；
- type/fit seal 实例方法和静态方法；
- 父 spec；
- reference/value owner；
- 多个闭合实例，确认 witness 和返回值不串实例。

FCTS 之外必须保留隔离 `.fb` CLI 用例，防止本地 workspace-cache 或依赖源码
偶然掩盖 package-public 事实缺失。

## 7 TODO 与实施顺序

- [ ] **验证（根因定位）**：逐层比较 provider Semantic relation、package-public
  writer、reader 恢复、generic instance 关闭和 consumer 名义查询，确定泛型 type
  声明头关系在哪一层丢失；不得先写使用点结构匹配补丁。
- [ ] **实际变更（关系恢复）**：在确认的通用 relation 模型层修复开放泛型
  `type -> spec` 模板的导出、恢复或关闭，使 consumer 使用现有名义查询得到精确
  闭合关系。
- [ ] **验证（普通泛型 fit 基线）**：固定现有公开泛型 fit 的隔离 `.fb` 正向用例；
  该项只验证，不重写已工作的 fit 关系路径。
- [ ] **实际变更（泛型 callable 分类）**：让泛型 type/fit shared body 与 wrapper
  的 package surface 统一消费声明期 spec implementation dependency sidecar，替代
  仅按 public/`@mixable` 判断的过窄条件。
- [ ] **实际变更（稳定符号）**：确保选中 seal 泛型方法使用与公开方法一致且只由
  package-public 事实确定的 owner、fit、成员序号和 shared symbol；不得依赖私有成员
  顺序。
- [ ] **验证（reified dependencies）**：验证泛型 type/fit 的 aggregate、managed
  type、callable dependencies 和 wrapper 调用链已经完整；只有确认存在通用事实
  丢失时才列为实际修复，不为当前用例增加依赖特判。
- [ ] **验证（编译器用例）**：补齐 Semantic、Symbol、Codegen、Imported Semantic
  和隔离 `.fb` CLI 正负用例，并恢复现有 `#if 0` 探针中属于本专项的部分。
- [ ] **验证（FCTS）**：补齐泛型 type/fit seal 实现的跨包可观察行为用例。
- [ ] **验证（回归）**：执行定向测试后，在沙箱外执行 `make test` 全量回归。

若根因定位表明泛型 type 关系和泛型 fit seal 符号确实需要不同实际修改，应在
同一文档中保留两个明确提交阶段；不得为了“统一改动”强行改变已经正确工作的
普通泛型 fit 名义关系。
