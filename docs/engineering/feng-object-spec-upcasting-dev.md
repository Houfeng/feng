# Object-form Spec 向上 Coercion 开发设计

> **状态**：方案已确定，待人工 Review，未实施。
> **日期**：2026-08-07。
> **范围**：object-form 子 `spec` 值到其父 `spec` 值的上下文向上 coercion 与显式 cast。
> **权威语义**：语言层资格与禁止项以 [Feng 语言 `spec` 规范](../specifications/feng-spec.md) 为准；本文只定义编译器与 witness ABI 的实现方案。

## 1. 目标

实现 object-form 子 `spec` 到父 `spec` 的视角投影：

```feng
spec A {
    func a(): string;
}

spec B: A {
    func b(): string;
}

type X: B {
    func a(): string { return "a"; }
    func b(): string { return "b"; }
}

let b: B = X {};
let a: A = b;
let explicitA = (A)b;
```

其中：

- `let a: A = b` 在 expected-type 位置沿已声明的 `B: A` 名义关系自动建立父视角。
- `(A)b` 显式建立同一个父视角，不扩大可达关系集合。
- Coercion/cast 资格和父级路径均在编译期确定。
- 运行时不搜索、不遍历、不比较候选、不试探且不回退。
- 不改变 object-form `spec` 值现有的 `{ subject, witness }` 表示。
- 不分配 wrapper 或 box，不根据 `subject` 反查动态类型。
- 投影后仍通过普通目标 witness 分派，成员调用不增加额外开销。
- 多父、传递祖先、菱形继承、泛型父 `spec`、默认 subject 及跨包使用遵守同一套机制，不增加专用特判。

## 2. 语义边界

### 2.1 上下文向上 Coercion

赋值、初始化、参数传递、返回值、字段写入、数组元素写入及已由合法无重叠重载集合唯一确定目标参数类型的调用位置，若源是 object-form 子 `spec`、目标是其直接或传递父 object-form `spec`，允许自动建立父视角：

```feng
let parent: Parent = child;
useParent(child);
return child; // 当前函数返回 Parent
```

该行为是沿 `spec Child: Parent` 已声明名义关系进行的契约视角投影，不是无名义关系类型之间的一般隐式转换。

### 2.2 显式 Cast

显式 cast 继续允许，并与上下文 coercion 使用完全相同的资格和 lowering：

```feng
let parent = (Parent)child;
```

显式形式不能用于父到子、无关 `spec`，也不能依据 subject 的运行时具体类型建立静态类型不可达的视角。

### 2.3 重载集合边界

Feng 的重载集合必须由调用方实参类型准确区分，不使用“精确优先”或“最具体优先”保留可能重叠的候选。Object-form `spec` 向上 coercion 必须纳入现有重载重叠检查。

以下重载均不合法：

```feng
func test(value: UserType) {}
func test(value: UserSpec) {} // UserType 满足 UserSpec

func show(value: ChildSpec) {}
func show(value: ParentSpec) {} // ChildSpec: ParentSpec
```

因为同一个实参类型可同时匹配两个候选，编译器必须在声明阶段诊断签名冲突。调用点显式 cast 不用于挽救已经重叠的重载集合。

对于通过重叠检查的合法重载集合，调用解析唯一确定目标函数后，该参数位置才形成明确 expected type，并按普通参数 coercion 规则记录父级路径。

### 2.4 当前兼容性核查与实施边界

截至本设计定稿时，仓库中没有发现依赖 `ChildSpec` / `ParentSpec` 可以组成重载的生产代码或测试：

- `std`、FCTS、examples 与 smoke 中存在 object-form 父子 `spec`，但没有同名函数或方法分别以该子、父 `spec` 作为同一参数位置的重载。
- 现有 semantic 测试要求具体 `type` 与其满足的 `spec` 参数重载在声明阶段被拒绝。
- 现有 semantic 测试要求两个具有共同具体满足类型的 `spec` 参数重载在声明阶段被拒绝。
- 现有 semantic 测试允许两个没有共同满足类型、彼此也无父子关系的 `spec` 参数构成重载。
- 当前没有直接覆盖 `ChildSpec` / `ParentSpec` 参数重载的测试。

当前重载重叠检查在两个参数均为 `spec` 时，主要通过搜索可见具体类型是否同时满足二者来判断重叠；因此当尚无具体实现类型时，父子 `spec` 重载可能被接受。这是重叠检查未直接纳入 `spec` 名义父级可达关系的实现缺口，不作为已交付语言能力保留。

本次实施只做保持现有重载原则所必需的一致性补齐：

1. 当两个 object-form `spec` 参数存在直接或传递父子关系时，直接判定参数类型重叠。
2. 继续在声明阶段使用现有重叠诊断，不把冲突延后到调用点。
3. 不删除或放宽现有 `type` / `spec`、共同满足类型等重叠检查。
4. 不增加精确优先、最具体优先、父级距离排序或其他重载候选计分。
5. 不改变两个无父子关系且没有共同满足类型的 `spec` 参数可以重载的现有行为。

上述补齐属于向上 coercion 扩大可匹配关系后对既有重载不重叠约束的同步，不属于重载功能优化。

### 2.5 阶段决策

本次开发按以下阶段边界执行：

1. 先保持重载规则一致：具体 `UserType: UserSpec` 使 `UserType` / `UserSpec` 参数重载冲突；同理，`ChildSpec: ParentSpec` 使 `ChildSpec` / `ParentSpec` 参数重载冲突。
2. 在该一致规则下完成 object-form `spec` 上下文向上 coercion、显式 cast、witness 父级入口及全部正确性场景。
3. 本次不调整重载声明合法性、候选优先级或调用点消歧规则，也不为可能的后续放宽预埋分支或特判。
4. Spec 向上 coercion 完成交付后，再由人工单独决定是否启动重载规则重构。

未来若讨论允许上述重叠重载，必须作为独立设计统一评估 `Type` / `Spec` 与 `ChildSpec` / `ParentSpec`，并同时覆盖顶层函数、成员方法、静态方法、构造函数、泛型候选、函数值和跨包调用。本文不选择该未来方案，也不承诺实施。

### 2.6 非目标

本次不实现或改变：

- 无已声明名义契约关系的类型之间的隐式转换。
- 父 `spec` 到子 `spec` 的向下转换。
- 无关 object-form `spec` 之间的转换。
- 依赖 `subject` 运行时具体类型才能成立的转换。
- union-form 到共同 object-form `spec` 的投影。
- intersection-form 到单个 object-form `spec` 的投影。
- callable-form `spec` 的转换。
- object-form `spec` 值布局、成员调用约定或引用身份比较语义。
- 允许当前被重叠检查拒绝的重载集合。
- 重载候选优先级、计分、父级距离比较或调用点消歧策略。

## 3. 已确定的 Witness 方案

### 3.1 Witness 只记录直接父级

每个 object-form `spec S` 的 witness 结构为 `S` 的每个直接父 object-form `spec` 增加一个具名、强类型的父 witness 字段。

例如：

```feng
spec A {}
spec B: A {}
spec C: A {}
spec D: B, C {}
```

概念 C 布局为：

```c
struct FengSpecWitness__D {
    /* D 的既有成员分派槽。 */

    const struct FengSpecWitness__B *parent_B;
    const struct FengSpecWitness__C *parent_C;
};
```

规则如下：

1. 只记录源码声明中列出的直接父级，不在当前 witness 中拍平传递祖先。
2. 父 witness 字段使用父 `spec` 的完整规范身份生成具名字段，不使用无类型的统一 `void *` 数组。
3. 泛型父 `spec` 的实例化类型实参属于父 `spec` 身份的一部分。
4. 父字段按源码父级声明顺序布局。
5. 父字段追加在该 witness 的既有成员槽之后，不改变成员槽之间的既有顺序。
6. 只要 witness 含有成员槽或父字段，就不生成空结构占位字段；仅两者均为空时保留现有空结构处理。

具名字段使 codegen 按父关系访问字段，而不是把父级解释为运行时数字索引。声明顺序仍决定 C 结构体物理布局，必须作为编译器内部跨包 ABI 的一部分稳定保存。

### 3.2 Witness 实例保持同一主体实现

本文用 `K` 表示 witness 的主体实现键。`K` 不只表示源码中的具体 `type`，还必须区分实际 subject storage 与实现来源，包括：

- 普通对象类型；
- builtin、enum、array 使用的具体 subject storage；
- scalar box、value box 或其他装箱形式；
- 某个 object-form `spec` 的隐藏默认 subject；
- 泛型实例化后形成的具体主体实现。

对于 witness 实例：

```text
W(K, S)
```

其每个直接父字段必须引用同一 `K` 对应的父 witness：

```text
W(K, S).parent_P = &W(K, P)
```

不得把父字段指向其他 subject storage、父 `spec` 的独立默认 subject，或需要运行时重新适配 subject 的表。

例如 `(X, D)` witness 概念上初始化为：

```c
static const struct FengSpecWitness__D FengWitness__X__as__D = {
    /* D 的成员槽初始化。 */
    .parent_B = &FengWitness__X__as__B,
    .parent_C = &FengWitness__X__as__C,
};
```

### 3.3 传递祖先视角使用固定读取链

Semantic 在编译期从源 `spec` 到目标祖先 `spec` 选择一条由直接父边组成的路径。Codegen 将该路径完整展开为具名字段读取链；上下文 coercion 与显式 cast 共用该路径和发码。

直接父投影：

```c
target.subject = source.subject;
target.witness = source.witness->parent_B;
```

传递祖先投影：

```c
target.subject = source.subject;
target.witness = source.witness->parent_B->parent_A;
```

生成代码不得包含循环、递归调用、条件分支、名称查询、候选比较或失败回退。

若目标距离源 `depth` 条直接父边，则视角投影执行 `depth` 次固定的 witness 指针读取，时间复杂度为 `O(depth)`。该成本只发生在建立父视角时；完成后的成员调用仍为一次普通 witness 槽读取和一次函数指针调用。

### 3.4 路径选择

当源到目标存在多条静态路径时，编译器按每一级直接父级的源码声明顺序做深度优先查找，选择第一条可达路径。

例如：

```text
    A
   / \
  B   C
   \ /
    D
```

对于 `spec D: B, C {}`，`D -> A` 选择：

```text
D -> B -> A
```

路径选择只决定生成哪条固定读取链，不改变语言可观察语义。所有路径到达的 `(K, A)` 必须引用同一个规范 witness 实例，不能因路径不同产生不同的父视角行为。

### 3.5 菱形唯一性

Witness 实例继续按主体实现键与目标 `spec` 身份唯一化。生成 `W(K, D)` 的直接父闭包时：

```text
W(K, D).parent_B -> W(K, B)
W(K, D).parent_C -> W(K, C)
W(K, B).parent_A -> W(K, A)
W(K, C).parent_A -> W(K, A)
```

两条路径最终必须指向同一个 `W(K, A)`，不得分别生成语义来源不同的重复实例。已有 witness 缓存、可见 `fit` 选择及跨包恢复若不足以保证该性质，必须先补齐统一的 witness 身份和生成规则，不能在父视角发码处增加路径特判。

## 4. Witness 布局规则

### 4.1 字段身份

父字段身份必须来自解析后的直接父 `spec` 引用，包括：

- 父 `spec` 声明的模块与声明身份；
- 完整泛型实参；
- 现有类型规范化规则要求纳入身份的其他信息。

字段名使用现有稳定 C 名称编码机制生成，不能只使用源码短名。不同模块的同名父 `spec`、同一泛型父 `spec` 的不同实例不得发生字段名或 witness 身份冲突。

### 4.2 字段顺序

父字段按解析后的源码直接父级声明顺序生成。编译器不得按短名、导入顺序、指针地址或哈希容器迭代顺序重新排序。

例如：

```feng
spec D: B, C {}
```

固定为 `parent_B` 后 `parent_C`；改写为 `spec D: C, B {}` 会改变内部 witness 布局，属于需要重新编译依赖方的 ABI 变化。

由于父视角表达式访问具名字段，语义正确性不依赖裸槽位编号；稳定顺序只用于保证结构体物理布局和跨编译单元一致。

### 4.3 声明依赖

发射子 witness 结构前，codegen 必须已发射所有直接父 witness 结构的前向声明。发射 `W(K, S)` 实例前，所有被引用的 `W(K, P)` 必须：

1. 已在当前编译单元生成；或
2. 具有可链接的稳定外部符号与声明。

父图已经禁止循环，因此递归确保直接父 witness 的生成过程必须终止。实现必须使用显式生成状态区分“未开始、生成中、已完成”，避免重复生成或因菱形关系重复进入。

## 5. 默认 Subject

`spec S` 的默认值使用隐藏主体实现 `DefaultSubject(S)`。其默认 witness 应视为：

```text
W(DefaultSubject(S), S)
```

当 `S` 有直接父 `P` 时，该默认 witness 的父字段必须指向：

```text
W(DefaultSubject(S), P)
```

不得直接指向父 `spec P` 自己的默认 witness：

```text
W(DefaultSubject(P), P)
```

因为建立父视角必须保留原来的 `DefaultSubject(S)`，而两个默认 subject 的存储与生命周期身份不同。

因此，codegen 必须能够为 `DefaultSubject(S)` 生成全部父链所需的普通父视角 witness。父视角 thunk 必须继续解释同一个 `DefaultSubject(S)`，并满足父 `spec` 的默认成员语义。

## 6. Semantic 设计

### 6.1 Coercion 与 Cast 资格

当 object-form 子 `spec` 值进入父 `spec` expected-type 位置，或显式 cast 的源和目标均为 object-form `spec` 时，semantic 必须：

1. 解析并实例化源、目标 `spec` 引用。
2. 仅沿 object-form `spec` 的名义直接父边检查目标是否可达。
3. 按 §3.4 选择确定路径。
4. 记录从源到目标的直接父边序列，供 codegen 使用。

上下文位置的目标不可达时，按现有类型不匹配规则诊断；显式 cast 的目标不可达时，保持现有非法 cast 诊断。不得因为源值运行时可能保存某个满足目标的具体类型而接受转换。

具体 `type` 到其已满足 object-form `spec` 的现有 coercion 继续使用已有满足关系选择与 witness 生成机制；本设计不把它改为先构造某个子 spec 再逐级投影。

### 6.2 Sidecar 信息

Semantic 应为合法上下文 coercion 或显式 cast 记录至少以下信息：

- 源 object-form `spec` 的实例化身份；
- 目标 object-form `spec` 的实例化身份；
- 按顺序排列的直接父边路径；
- 每条边实例化后的父 `spec` 引用。

Codegen 只能消费该结论，不得重新判断父关系或自行选择另一条路径。

重载重叠检查必须把子 `spec` 到父 `spec` 的可达性视为参数类型重叠关系，不得仅通过搜索当前可见具体实现类型间接推断。对于已经通过重叠检查的合法集合，仅在调用目标唯一确定后记录正式 coercion sidecar，避免为未选候选生成 witness 依赖。

## 7. Codegen 设计

### 7.1 Witness 结构发射

在现有 object-form witness 结构的成员槽之后，按直接父声明顺序追加父 witness 指针字段。字段类型是对应父 witness 的具名结构指针。

该变化不得：

- 把所有 witness 降级成无类型统一表；
- 改变 `{ subject, witness }` fat value 的字段与顺序；
- 改变已有成员 thunk 的函数签名；
- 改变成员槽之间的顺序。

### 7.2 Witness 闭包生成

确保 `W(K, S)` 时，codegen 必须递归确保每个直接父 `P` 的 `W(K, P)`，然后使用其稳定符号初始化 `parent_P`。

该闭包生成只发生在编译期。运行时 witness 仍为 `static const` 表，不执行注册、连接或延迟初始化。

若某个父 witness 因缺少公开声明事实、实现来源或可链接符号而无法生成，编译器必须明确诊断，不得生成空指针、默认 witness 或运行时回退路径。

### 7.3 父视角表达式发射

无论父视角来自上下文 coercion 还是显式 cast，codegen 都必须只求值源表达式一次，然后：

1. 原样保留 `subject`。
2. 沿 semantic 记录的具名父字段链取得目标 witness。
3. 构造目标 object-form `spec` fat value。

概念结果：

```c
((struct FengSpecValue__A) {
    .subject = source_once.subject,
    .witness = source_once.witness->parent_B->parent_A,
})
```

若源表达式不是可安全重复引用的局部值，codegen 必须使用现有表达式暂存机制保证一次求值，不能复制可能具有副作用的源表达式。

### 7.4 生命周期

父视角投影不创建新 subject，也不改变 subject 的存储解释。投影本身不增加额外 ARC 操作；其结果在绑定、参数、返回、字段或数组位置中的 retain、move、release 继续由现有 aggregate value ownership 规则决定。

投影期间只更换 witness 指针：

```text
source.subject == target.subject
```

因此 object-form `spec` 的引用身份比较语义保持不变。

## 8. 泛型与跨包

### 8.1 泛型父 Spec

例如：

```feng
spec A<T> {}
spec B<T>: A<T> {}
```

实例化 `B<U>` 的直接父字段必须具有 `A<U>` 的完整实例化身份：

```text
W(K, B<U>).parent_A_U -> W(K, A<U>)
```

Semantic 记录的路径和 codegen 生成的字段必须使用相同的类型实参替换结果。不得在运行时解析、比较或恢复泛型实参。

### 8.2 跨包恢复

公开 object-form `spec` 的编译产物必须保留：

- 直接父级的声明顺序；
- 每个父级的规范声明身份；
- 泛型父引用的完整类型实参；
- 生成父 witness 字段名和字段类型所需的信息。

Provider 与 consumer 必须由这些事实生成一致的 witness 结构和父字段访问。不得依赖源码短名、导入别名或本地遍历偶然顺序。

若 `.ft` 或 `.fb` 当前缺少上述事实，必须先扩展对应序列化与恢复路径，再开放跨包父视角投影；不能将跨包场景静默降级为只支持单模块。

## 9. 性能与空间边界

| 维度 | 结果 |
| --- | --- |
| Spec fat value 大小 | 不变，仍为 `{ subject, witness }` |
| 每个 witness 静态大小 | 每个直接父级增加一个指针 |
| 直接父视角投影 | 一次固定指针读取 |
| 传递祖先视角投影 | `depth` 次固定链式指针读取 |
| 运行时搜索或遍历 | 无 |
| 运行时条件分支 | 无 |
| 分配 | 无 |
| 投影后成员调用 | 与普通目标 spec 值完全相同 |
| Runtime 私有 ABI | 不新增 runtime API |

本方案接受视角投影成本随父链深度线性增加。该成本只发生在上下文 coercion 或显式 cast 建立父视角时；不把额外间接层带入投影后的每次成员调用。

## 10. 实施顺序

文档 Review 通过后，按以下顺序实施：

1. Semantic：在 expected-type 位置与显式 cast 中接受合法的 object-form 子 `spec` 到祖先 `spec` 视角投影，并记录确定的直接父路径。
2. 重载一致性：仅把子 `spec` 到父 `spec` 的名义可达性纳入现有声明阶段重叠检查，使其与既有 `type` / `spec` 冲突规则一致；不调整候选解析与优先级。
3. Codegen 声明：在 witness 结构中追加具名直接父字段及所需前向声明。
4. Witness 生成：按同一主体实现键递归生成并初始化直接父 witness 闭包。
5. Codegen 表达式：将上下文 coercion 与显式 cast 统一 lower 为保留 subject 的固定父字段读取链。
6. 默认 subject：补齐默认主体到全部父视角的 witness 生成。
7. 泛型与跨包：验证类型实参替换、声明恢复和链接符号一致性。
8. 测试：新增 semantic、codegen 与 FCTS 覆盖，最后执行全量回归 `make test`。

不得通过只放宽类型匹配或 cast 校验、只支持单父单层、只处理普通对象 subject，或在失败时改用默认 witness 的方式交付不完整实现。

本次完成后停止在 spec 向上 coercion 的交付边界；未经后续人工决策，不继续修改重载合法性或候选解析策略。

## 11. 测试范围

### 11.1 Semantic

- Expected-type 位置中的直接父和传递祖先 coercion 成功。
- 显式 cast 到直接父和传递祖先成功。
- 多父分别建立父视角成功。
- 菱形父视角投影成功并选择声明顺序下的确定路径。
- 泛型父 spec 的类型实参替换正确。
- 跨模块与跨包父关系恢复正确。
- 具体 `type` 与其满足的 spec 参数重载在声明阶段诊断冲突。
- 新增独立测试：子 spec 与父 spec 参数重载在声明阶段诊断冲突，即使当前没有可见具体实现类型。
- 多个父 spec 参数可接纳同一实参类型时在声明阶段诊断冲突。
- 两个无父子关系且没有共同满足类型的 spec 参数继续允许重载。
- 父到子、无关 spec、依赖运行时具体类型的上下文 coercion 与显式 cast 继续失败。

### 11.2 Codegen

- Witness 只包含直接父字段，字段顺序与声明顺序一致。
- 每个父字段引用同一主体实现键对应的父 witness。
- 传递投影生成固定具名字段读取链，无循环、搜索、分支或回退。
- 源表达式只求值一次。
- 菱形的不同父路径最终引用同一个祖先 witness。
- 父视角投影保留 subject，不产生 box、wrapper 或额外分配。
- 投影结果的 retain、move、release 与普通 spec 值一致。
- 默认 subject、普通对象、builtin、enum、array、scalar/value box 均保持正确 storage 解释。
- 方法、`let` getter、`var` getter/setter 均通过目标父 witness 正确分派。
- 泛型实例和跨包生成的字段身份、结构布局与符号一致。

### 11.3 FCTS

- 具体类型进入子 spec，再在赋值、传参、返回、字段及数组位置自动建立直接父和传递祖先视角。
- 子 spec 显式 cast 到直接父和传递祖先。
- 多父与菱形继承的父视角成员行为。
- 父视角字段读取与写入。
- 默认 spec 值建立父视角。
- boxed/value subject 建立父视角。
- 泛型父 spec 视角投影。
- 跨包 provider/consumer 视角投影。

完成非文档实现后，必须按项目规则在 Codex 沙箱外执行全量回归 `make test`。

## 12. 完成标准

仅当以下条件全部满足，object-form `spec` 向上 coercion/cast 才视为完成：

1. 规范允许的 expected-type 位置和显式 cast 均可建立父视角。
2. 直接父、传递祖先、多父、菱形和泛型场景均可用。
3. 所有 subject storage 形态均通过同一直接父 witness 机制工作。
4. 默认 subject 和跨包场景不存在语义降级。
5. 生成代码不存在运行时搜索、遍历、分支、包装或分配。
6. 投影后成员调用成本不增加。
7. 新增测试与全量回归全部通过。
