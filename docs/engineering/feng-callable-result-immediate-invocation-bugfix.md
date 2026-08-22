# Feng callable 表达式结果立即调用修复方案

> **状态**：待 Review，尚未开始实施。
>
> **性质**：独立编译器 bugfix 工程方案，不是语言权威规范。调用表达式与求值顺序的
> 正式语义由 [`feng-expression.md`](../specifications/feng-expression.md) 定义；
> callable-form `spec` 的类型与调用约束由
> [`feng-function.md`](../specifications/feng-function.md) 和
> [`feng-spec.md`](../specifications/feng-spec.md) 定义。
>
> **来源**：本问题最初记录于
> [`feng-object-form-spec-method-value-dev.md` 的 ISSUE-003](./feng-object-form-spec-method-value-dev.md#issue-003fcts-使用了当前尚不支持的-callable-返回值立即调用形式)。
> ISSUE-003 当时解决的是 MV01 新增 FCTS 夹具超出专项范围的问题，并未修复编译器对
> callable 返回值立即调用的缺口。后续核对确认 `arr[0]()` 与其根因相同，因此合并到
> 本专项统一修复。

## 1. 问题与结论

以下调用表达式已经具备完整的静态 callable-form `spec` 返回类型，但当前无法直接调用
该返回值：

```feng
spec IntReader(value: int): int;

func makeReader(base: int): IntReader {
  return (value: int) -> base + value;
}

func run(): int {
  return makeReader(30)(3);
}
```

当前结果：

```text
CE0166: codegen: only direct or method calls supported in this iteration
```

将同一个值先显式绑定再调用则已经合法：

```feng
func run(): int {
  let reader: IntReader = makeReader(30);
  return reader(3); // 33
}
```

数组索引表达式已经能够产生静态类型明确的 callable 元素，但同样不能把该元素直接作为
callee：

```feng
func runArray(): int {
  let reader: IntReader = (value: int) -> value + 1;
  let readers: IntReader[] = [reader];
  return readers[0](41);
}
```

当前同样在 Codegen 报告 `CE0166`。先绑定索引结果再调用已经合法：

```feng
func runArray(): int {
  let reader: IntReader = (value: int) -> value + 1;
  let readers: IntReader[] = [reader];
  let selected: IntReader = readers[0];
  return selected(41); // 42
}
```

`makeReader(30)(3)` 与 `readers[0](41)` 分别形成：

```text
CALL(callee = CALL(...))
CALL(callee = INDEX(...))
```

两者具有同一个实现缺口，不应按 callee 的 AST kind 分开修复。立即调用应当：

1. 求值完整 callee 表达式一次，得到静态类型为 `IntReader` 的 callable value；
2. 保证该临时 callable 在外层调用完成前有效；
3. 再从左到右求值外层实参；
4. 通过 `IntReader` 的既有 invoke ABI 调用，并得到对应结果；
5. 按普通 callable 临时值规则完成清理。

这不是新增匿名 callable 类型、返回类型推导或新的调用机制，而是让已经完成类型确定的
callable 表达式进入既有 callable value 调用路径。

## 2. 与普通链式方法调用的边界

本问题只涉及“computed expression 的结果本身是 callable，并立即成为下一次调用的
callee”：

```text
foo()()       CALL(callee = CALL(...))
arr[0]()      CALL(callee = INDEX(...))
```

普通返回对象后的成员调用已经支持，不属于本专项：

```text
foo().bar()   CALL(callee = MEMBER(object = CALL(...), name = bar))
foo()[0]      INDEX(object = CALL(...))
```

例如以下代码当前已经能够完成 Semantic 与 Codegen：

```feng
func makeBox(value: int): Box {
  return Box(value);
}

func run(): int {
  return makeBox(30).read();
}
```

`foo()[0]` 也已经支持：数组索引发码会递归求值数组来源，现有 FCTS 已覆盖
`obj.openOpen.entries()[0]`。它不属于修复项，只作为本专项不得破坏的非回归基线。

本次不得改写普通方法调用、成员访问、重载选择或 receiver 物化路径。组合形式
`factory().makeReader(30)(3)` 中，前半段继续使用既有链式方法调用；组合形式
`makeReaders()[0](41)` 中，前半段继续使用既有调用结果索引。两者都只有最后一个
callable 结果调用进入本专项新增的通用 computed-callee 路径。

## 3. 现有规范依据

[`feng-expression.md` §4](../specifications/feng-expression.md#4-求值顺序) 已规定：

- 函数调用先求值被调用目标，再从左到右求值每个参数；
- 成员访问、下标访问和调用属于后缀运算，按从左到右连续展开。

[`feng-expression.md` §5](../specifications/feng-expression.md#5-运算符优先级) 同时把
`expr(...)` 定义为最高优先级、从左到右结合的后缀形式。由此，`foo()()` 在语法和求值
顺序上已经属于合法表达式组合；其是否可调用只取决于第一次调用结果的静态类型。

正式规范在实施前仅需在 `feng-expression.md` 的权威位置补充一条明确约束和代表示例：

- 任意表达式在完成泛型代入后的静态类型为 callable-form `spec`，或为受 callable-form
  `spec` 约束的类型参数时，均可作为 `expr(...)` 的 callee；
- 调用、索引或其他表达式产生上述类型时，可以立即继续调用；callee 必须只求值一次，
  并在全部外层实参之前求值；
- callee 非 callable 时继续由 Semantic 报 `AE0507`，callable 参数不匹配时继续报
  `AE0506` 或既有更精确的变参诊断，不得延迟到 Codegen；
- 该规则不产生匿名 callable 类型，也不允许对尚未闭合的泛型函数或方法值进行延迟泛型
  调用。

`feng-function.md` 与 `feng-spec.md` 继续只负责 callable value 的形成、签名、转换和运行时
表示，不重复定义调用表达式的后缀组合规则。

## 4. 当前实现事实与根因

### 4.1 Parser 已支持

Parser 以循环方式解析调用、成员和下标后缀，能够把 `foo()()` 构造成 callee 为内层
`FENG_EXPR_CALL` 的外层调用，也能够把 `arr[0]()` 构造成 callee 为
`FENG_EXPR_INDEX` 的外层调用。本问题不需要新增语法或 AST kind。

### 4.2 Semantic 的最小路径已支持

Semantic 会先递归解析完整 callee，再由通用 callable-typed expression 检查读取调用
返回类型或数组元素类型并校验外层实参。第 1 节两个最小复现均能够通过 Semantic，最终
进入 Codegen。

实施时仍需用新增测试确认普通、泛型、变参和非法调用场景均在 Semantic 得到正确且稳定
的结果；若任一场景缺少必要的静态类型事实，必须先记录问题，不得在 Codegen 重新推导
类型、按名称查找 callable 或增加来源特判。

### 4.3 Codegen 仅按 callee AST 形态分流

当前 `cg_emit_call` 先处理 `FENG_EXPR_MEMBER`，随后要求其余 callee 必须是
`FENG_EXPR_IDENTIFIER`。callee 为 `FENG_EXPR_CALL` 或 `FENG_EXPR_INDEX` 时，都会在
递归发射该表达式之前直接报告 `CE0166`。

Codegen 已有两条可复用的普通调用能力：

- 已知具体 callable-form `spec` 的 callable value 调用；
- 静态类型为受 callable-form `spec` 约束泛型参数的 callable value 调用。

两者都接收已经发射完成的 callee 表达式结果，并已经负责参数 lowering、变参打包、返回
值 ABI、所有权和 invoke。缺口只是 `cg_emit_call` 没有把 Semantic 已确认的 computed
callable callee 发射为表达式结果并交给上述通用能力。

## 5. 修复范围与强制边界

### 5.1 必须覆盖

修复以“Semantic 已确认静态 callable 类型的 computed callee expression”为抽象，不得只
对 `FENG_EXPR_CALL` 增加语法树特判。最低交付范围包括：

1. 顶层函数返回 callable 后立即调用：`makeReader(30)(3)`；
2. callable 数组元素直接调用：`readers[0](41)`；
3. 调用返回数组、索引取得 callable 后直接调用：`makeReaders()[0](41)`；
4. 实例方法和静态方法返回 callable 后立即调用；
5. callable value 返回另一个 callable value 后立即调用；
6. 泛型函数或方法返回已实例化 callable-form `spec` 后立即调用；
7. computed callee 的静态类型是受 callable-form `spec` 约束的泛型参数；
8. 返回或索引得到的 callable 为变参 callable-form `spec`；
9. 多级后缀组合，例如 `factory().makeReader(30)(3)`；
10. imported 函数或方法返回 callable，或 imported callable 数组元素在 consumer 中
    直接调用。

这里的“通用 computed-callee 路径”只消费 Semantic 已经确定的 callable 类型，不为当前
尚未具备静态类型的表达式新增推导，不扩大 callable 来源、隐式转换、成员访问或泛型
闭合规则。

### 5.2 必须保持的非法行为

```feng
func number(): int {
  return 1;
}

func invalid(): int {
  return number()(); // Semantic：AE0507，非 callable
}
```

```feng
spec IntReader(value: int): int;

func makeReader(): IntReader {
  return (value: int) -> value;
}

func invalid(): int {
  return makeReader()("bad"); // Semantic：既有参数类型不匹配诊断
}
```

修复不得把非法 callee 或签名不匹配延迟到 Codegen，也不得用运行时检查兜底。

### 5.3 强制停止条件

本专项必须同时满足：

- 不修改或删除任何既有测试用例，只允许新增用例及必要的测试入口注册；
- 不修改 Runtime、Runtime 私有 ABI、公开 ABI、生成程序 ABI；
- 不修改 `.ft` schema、字段、枚举值、版本或既有记录含义；
- 不给任何既有合法调用路径增加运行时分支、分配、retain/release、查找、wrapper 或
  invoke 转发层；
- 新支持的立即调用路径相对于语义等价的“先绑定 callee 表达式结果，再调用”写法，不得
  增加额外 closure、wrapper、调用层级、retain/release 或动态查找；
- 不增加只识别 `FENG_EXPR_CALL`、函数名、方法名、spec 名称、包名或测试模型的特判；
- 不在 Codegen 重做 Semantic 已完成的重载决议、泛型代入、签名检查或 callable 来源
  选择。

如果正确修复必须突破任何一项，或实施中发现规范与现有 Semantic 行为不一致、需要新增
持久 sidecar/ABI 事实、无法复用既有所有权路径，必须先写入第 8 节并停止，由人工决策。

## 6. 通用实现方案

### 6.1 正式规范与诊断

1. 在 `feng-expression.md` 的调用后缀与求值顺序权威位置补充第 3 节所述规则和最小示例；
2. 核对 `feng-function.md`、`feng-spec.md`，只保留交叉引用，不复制表达式规则；
3. 修复完成后消解 Codegen 的 `CE0166` 触发点，并同步当前错误码总表；非法程序继续使用
   既有 Semantic 诊断，不新增替代 CE；
4. `feng-error-codes-ce.md` 已把对应调用形态限制标记为“消解”，不得重新定义为新的用户
   约束。

### 6.2 Semantic

Semantic 保持现有通用 `validate_callable_typed_expr_call` 方向：

- 先完整解析 callee 表达式并获得实例化后的静态类型；
- 具体 callable-form `spec` 使用该实例的参数、变参形态和返回类型检查外层调用；
- 受 callable-form `spec` 约束的类型参数使用其约束形状检查；
- 非 callable 与参数不匹配继续在 Semantic 拒绝；
- 不因 callee 是调用结果而形成新的 callable coercion、匿名类型或重载入口。

若现有分析事实已经完整，Semantic 只新增覆盖，不增加实现。若发现事实缺口，必须先在
第 8 节记录最小复现、缺失事实及通用修复方案，再决定是否修改。

### 6.3 Codegen

在保持直接函数、构造函数和直接成员方法现有专用路径不变的前提下，为剩余
Semantic 已确认的 computed callee 增加一个通用入口：

1. 递归发射 callee 表达式一次，得到既有 `ExprResult`；
2. 在发射任何外层实参之前，按 callee 的所有权与别名语义固定本次调用目标，避免后续
   实参副作用重新读取或替换原表达式位置；
3. callee 类型为具体 callable-form `spec` 时，复用普通 callable value 调用 helper；
4. callee 类型为受 callable-form `spec` 约束的泛型参数时，复用 generic callable value
   调用 helper；
5. 复用 helper 已有的参数 ABI、变参打包、返回槽、reified storage、异常事实与所有权
   清理；
6. Semantic 已接受但 Codegen 得到非 callable 类型时，按编译器内部事实不一致处理，
   不保留 `CE0166` 作为用户级限制。

该入口必须面向“已发射的 callable 表达式结果”，不得实现为
`if (callee->kind == FENG_EXPR_CALL)` 的独立发码分支。递归发射与 callee 结果物化共同
保证调用目标只求值并固定一次；外层参数必须在 callee 完成物化后继续按既有从左到右
顺序发射。

### 6.4 所有权与运行时成本

若内层调用返回拥有引用的 callable closure，必须复用现有 callable value 调用 helper
的 owned-result 物化与作用域清理机制，使其至少存活到外层参数求值和 invoke 完成。不得
为了立即调用再复制 closure、生成 wrapper 或重新绑定 receiver/witness。

数组索引当前产生借用的元素表达式。`arr[0]()` 必须在外层实参求值前固定当时选中的
callable；即使某个外层实参通过其他可写别名替换同一数组元素，本次调用也不得改为读取
替换后的 callable。实现必须复用普通 managed value 物化与清理机制，在必要时对选中
callable 执行与显式 `let selected: IntReader = arr[0]` 等价的 retain/release。

同时必须保持索引 receiver 的既有生命周期：普通数组绑定在调用期间保持有效；
`makeReaders()[0](41)` 这类临时数组必须按既有索引发码物化，并至少保持到 callee 已固定。
不得在等价显式绑定及既有索引临时值规则之外额外复制 callable、延长生命周期或增加
retain/release。若现有
`ExprResult`/物化能力无法同时表达“固定 callable 值”和“临时数组 receiver 生命周期”，
必须先记录问题并停止，由人工决定是否调整通用所有权抽象。

以下两种写法的可观察调用次数、分派目标和必要所有权操作必须一致：

```feng
makeReader(30)(3);
```

```feng
let temporary: IntReader = makeReader(30);
temporary(3);
```

数组元素直接调用与先绑定元素再调用也必须满足同一等价性：

```feng
readers[0](41);
```

```feng
let selected: IntReader = readers[0];
selected(41);
```

允许编译器消除不影响语义的临时 C 局部，但不得以优化为由改变 callee 先于外层参数求值
或缩短 callable 到 invoke 前失效。

## 7. 实施与验证清单

### 7.1 规范

- [ ] Review 本方案并确认范围、语义和强制边界。
- [ ] 在 `feng-expression.md` 的唯一权威位置明确 callable 结果立即调用及求值顺序。
- [ ] 核对 `feng-function.md`、`feng-spec.md`，避免重复定义。
- [ ] 同步当前错误码总表中 `CE0166` 的消解结果。

### 7.2 实现

- [ ] 保持 Parser AST，不新增语法或 AST kind。
- [ ] 用 Semantic 定向探针确认各合法与非法场景的类型事实和诊断阶段。
- [ ] 若 Semantic 存在事实缺口，先记录问题，再采用通用方案补齐。
- [ ] 为 computed callable callee 增加统一 Codegen 发射入口。
- [ ] 复用具体 callable 与受约束泛型 callable 的既有调用 helper。
- [ ] 删除 `CE0166` 的现有限制触发点，不增加替代用户级 CE。
- [ ] 核对 callee 单次求值、参数顺序、返回槽和所有权清理。
- [ ] 核对普通直接函数、构造函数、直接成员方法与已绑定 callable 调用生成结果不变。

### 7.3 编译器测试 `test/`

- [ ] Parser 新增 `foo()()`、`arr[0]()` 与多级后缀组合的嵌套 AST 结构覆盖。
- [ ] Semantic 新增调用结果与数组索引结果立即调用的正向覆盖。
- [ ] Semantic 新增非 callable 结果、实参数量/类型不匹配及变参非法转发的负向覆盖。
- [ ] Semantic 覆盖具体 callable-form `spec`、泛型实例和受 callable 约束类型参数。
- [ ] Codegen 覆盖顶层函数、实例/静态方法、callable value、数组索引及 imported 来源。
- [ ] Codegen 验证内层 callee 只发射一次，并复用既有 callable invoke/cleanup 路径。
- [ ] Codegen 正确消费索引产生的借用 callable，必要时按显式绑定等价物化，并复用临时
      数组 receiver 的既有生命周期与清理规则。
- [ ] Codegen 验证外层实参替换原数组元素时，仍调用实参求值前固定的 callable。
- [ ] Codegen 保持 `foo()[0]` 及普通数组索引生成结果不变。
- [ ] 不修改或删除任何既有测试场景；只新增用例与入口注册。

### 7.4 Feng 兼容性测试 `fcts/`

- [ ] 新增 `makeReader(30)(3) == 33` 的真实执行用例。
- [ ] 新增 `readers[0](41) == 42` 与 `makeReaders()[0](41) == 42` 的真实执行用例。
- [ ] 新增实例/静态方法返回 callable 后立即调用的代表用例。
- [ ] 新增泛型 callable 返回、受约束泛型 callable 结果和变参结果的代表用例。
- [ ] 新增多级后缀组合与 imported consumer 的代表用例。
- [ ] 保留 `foo()[0]` 已支持行为，并验证本次没有引入索引路径回归。
- [ ] 验证 callee 单次求值、callee-before-arguments 顺序及闭包捕获生命周期。
- [ ] 不做来源、泛型和包形态的笛卡尔积；每个非等价语义或 ABI 路径至少一个代表用例。

### 7.5 回归与交付

- [ ] 运行 Parser、Semantic、Codegen、Symbol 和 FCTS 定向测试。
- [ ] 在沙箱外运行完整 `make test`。
- [ ] 审计 runtime、ABI、`.ft`、现有合法路径运行时成本和既有测试边界。
- [ ] 补齐第 8 节实施问题及解决结果。
- [ ] 补齐第 9 节交付记录并更新本文状态。

## 8. 实施过程问题记录

> 当前为空，实施尚未开始。

实施过程中发现任何偏离既有规范、本文方案或预期测试结果的问题，必须先在本节记录，
再分析和解决。不确定或触及第 5.3 节强制停止条件时，不得继续修改实现，交由人工决策。

记录模板：

```markdown
### ISSUE-XXX：问题标题

- **关联任务**：
- **状态**：待分析 / 待人工决策 / 已解决
- **最小复现**：
- **实际结果**：
- **期望结果**：
- **根因**：
- **通用修复方案**：
- **运行时性能影响**：
- **runtime ABI / 公开 ABI / `.ft` 影响**：
- **既有测试影响**：
- **是否需要人工决策**：
- **专项验证结果**：
- **全量回归结果**：
```

## 9. 交付记录

> 实施完成后填写。

- **规范变更**：
- **实现结果**：
- **新增测试**：
- **运行时成本**：
- **ABI 与 `.ft`**：
- **定向验证**：
- **全量回归**：
- **未解决问题**：
- **建议 commit message**：
