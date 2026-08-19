# Feng 泛型 owner 的 `spec` witness 方法依赖闭合修复开发文档

> 状态：待 Review，尚未实施（2026-08-19）
>
> 本文只跟踪：已经闭合的泛型 `type` owner 通过 object-form `spec` witness
> 调用一个**没有方法级泛参**的实现方法时，witness 物化链路没有完整复用该
> closed method wrapper 的 callable dependency 注册结果，因而在编译期报告
> `CE0031` 的问题。

## 1. 依据与范围

本修复复用以下既有设计，不重新定义泛型或 `spec` 语义：

- [Feng 泛型共享体具化修复开发文档](./feng-generic-shared-body-reification-bugfix-dev.md)：
  定义 `_type_desc`、`FengFunctionDescriptor`、closed wrapper 与静态
  descriptor graph；
- [Feng 泛型 `spec` 满足关系及跨包实现符号修复开发文档](./feng-generic-spec-implementation-package-bugfix.md)：
  定义泛型 type/fit 关系关闭、witness 物化与跨包实现恢复；
- [Feng object-form `spec` 方法级泛型修复开发文档](./feng-object-form-spec-generic-method-bugfix.md)：
  单独跟踪 requirement 自身声明方法级泛参时的动态实现 descriptor 路由问题。

本专项只处理如下组合：

```text
泛型实现 owner A<T>
  + 已闭合 owner 实例 A<int>
  + 已闭合 spec 实例 Surface<int>
  + 非方法泛型 requirement / implementation
  + 实现方法体含依赖 T 的 callable-local reified dependency
  + 通过 spec witness 调用
```

本专项不处理：

- `spec` requirement 自身声明 `<U>` 等方法级泛参；
- object-form `spec` 方法值；
- 新的泛型推导、满足、可见性或跨包导出规则；
- descriptor 提升或其他性能优化；
- runtime descriptor 构造、缓存、名称查找或反射；
- runtime 私有 ABI 变更。

## 2. 已确认事实

### 2.1 基础 `Surface<T>` 路径已经工作

以下基础路径目前可以正确编译和运行：

```feng
spec Surface<T> {
  func read(): T;
}

type Value<T>: Surface<T> {
  let value: T;

  func read(): T {
    return self.value;
  }
}

let view: Surface<int> = Value<int>();
view.read();
```

现有 FCTS 已覆盖：

- 同包与跨包的泛型 type/fit 满足关系；
- `Surface<int>` / `Surface<string>` object-form 值及实例 witness 调用；
- 泛型 type、泛型 fit、泛型 value type；
- 实例成员、静态成员和映射后的泛型父 spec。

代表用例：

- `fcts/fcts_lib/src/test/lib_generic_spec_implementation.ff`；
- `fcts/fcts_bin/src/test_generic_spec_implementation.ff`；
- `fcts/fcts_bin/src/test_generic.ff`。

因此，本问题不能描述为“Feng 不支持泛型 object-form spec”或“`Surface<T>`
无法闭合”。

### 2.2 最小失败场景

隔离验证确认以下合法代码在当前编译器中失败：

```feng
type Box<T> {
  let value: T;

  func Box(value: T) {
    self.value = value;
  }
}

spec Surface<T> {
  func identity(value: T): T;
}

type A<T>: Surface<T> {
  func identity(value: T): T {
    let box = Box<T>(value);
    return box.value;
  }
}

func invoke(subject: Surface<int>, value: int): int {
  return subject.identity(value);
}

let subject: Surface<int> = A<int>();
invoke(subject, 42);
```

Semantic 已经接受该程序；Codegen 在处理 `A<T>.identity` 时报告：

```text
CE0031: codegen: generic type/spec instance 'Box<...>' was not registered
```

失败发生在编译期，尚未生成可执行文件，不是运行时 witness 调错方法或读取空
descriptor slot。

### 2.3 对照验证限定了问题边界

同一轮隔离验证结果如下：

| 场景 | 当前结果 |
|---|---|
| `A<int>().identity(42)`，方法体使用 `Box<T>` | 通过并正确运行 |
| `A<T>: Surface<T>`，但只直接调用 `A<int>.identity` | 通过 |
| 非泛型 `A: Surface<int>`，实现方法使用 `Box<int>`，再经 `Surface<int>` 调用 | 通过并正确运行 |
| `A<T>: Surface<T>`，实现方法不含 callable-local 泛型依赖，再经 `Surface<int>` 调用 | 通过 |
| `A<T>: Surface<T>`，实现方法使用 `Box<T>`，再经 `Surface<int>` 调用 | `CE0031` |

因此，当前证据把问题限定为：

> spec witness 对 closed generic owner 方法产生需求时，没有建立与普通直接调用
> 相同的 closed callable dependency ensure/register 顺序。

它不是普通泛型共享体不能闭合 `Box<T>`，也不是 `A<T>: Surface<T>` 的名义满足
关系错误。

### 2.4 当前方法 ABI 已按 owner 与 callable domain 分离

当前直接调用探针生成的共享方法 ABI 等价于：

```c
static void A_identity_shared(
    void *_self,
    const FengTypeDescriptor *_type_desc,
    const FengFunctionDescriptor *_func_desc,
    const void *_value,
    void *_out
) {
    const FengGenericParamDescriptor *_T =
        _type_desc->reified_generic_params[0];

    const FengTypeDescriptor *box_desc =
        _func_desc->reified_type_deps[0];
    /* ... */
}
```

其中：

- owner 类型参数 `T` 不作为独立隐藏参数重复传入；
- `T` 从 `A<int>` 的 `_type_desc->reified_generic_params[0]` 读取；
- `_func_desc` 只承载 `A<int>.identity` 自己的直接 callable-local 依赖，例如
  `Box<int>`；
- closed wrapper 静态传入 `A<int>` 的 type descriptor 和
  `A<int>.identity` 的 function descriptor。

直接调用已经生成如下等价结构：

```c
A_int_identity(self, value) {
    return A_identity_shared(
        self,
        &A_int_type_desc,
        &A_int_identity_func_desc,
        value
    );
}
```

所以本修复不得新增独立 `_T` 参数，也不得把 callable-local dependency 移入
`FengTypeDescriptor`。

### 2.5 正常 witness 已经调用 closed wrapper

对“不含 `Box<T>` 依赖”的等价成功场景检查生成 C，当前 witness thunk 已经采用：

```c
static void A_int_as_Surface_int_identity(
    void *_subject,
    const void *value,
    void *out
) {
    /* 调用 closed A<int> wrapper，而不是直接调用开放共享体。 */
    int result = A_int_identity((A_int *)_subject, *(const int *)value);
    memcpy(out, &result, sizeof result);
}
```

而 `A_int_identity` 再负责把静态 `_type_desc` 和 `_func_desc` 传给共享体。由此可见：

- witness 函数槽与 closed wrapper 的现有 ABI 方向正确；
- 不需要给 witness 增加 descriptor slot；
- 不需要在 witness 调用点运行时拼装描述符；
- 当前缺口发生在带 callable dependency 的 closed wrapper/descriptor 尚未完整注册
  之前，不能仅靠修改 thunk 参数规避。

## 3. 正确性不变量

### 3.1 类型级泛参必须在 witness 物化前闭合

当程序形成：

```feng
let subject: Surface<int> = A<int>();
```

编译器已经同时知道：

- subject 类型为 `A<int>`；
- target spec 为 `Surface<int>`；
- requirement 对应的实现为 `A<int>.identity`；
- owner 泛参 `T = int`；
- `A<int>.identity` 的直接依赖为 `Box<int>`。

这些信息必须在编译期闭合。该路径不存在方法级泛参调用中“调用点知道 `U`、运行时
witness 才知道 A/B”的信息分离。

### 3.2 owner 参数与方法依赖保持现有分工

- `_type_desc` 保存 owner 的具体泛参、字段布局和类型结构依赖；
- `_func_desc` 保存当前 callable 的 aggregate/type/callable 直接依赖；
- 方法入口可为方便读取而建立局部 `_T`，但 `_T` 只是
  `_type_desc->reified_generic_params[i]` 的别名，不是额外 ABI 参数；
- 每层 descriptor 继续只保存直接依赖，不拍平依赖树。

### 3.3 witness 必须复用 closed implementation wrapper

对于已闭合关系 `(A<int>, Surface<int>)`，witness 方法槽必须调用现有
`A<int>.identity` closed wrapper，或调用一个 ABI 完全等价且复用同一 closed
callable descriptor 的 wrapper。

不得：

- 从 requirement 的开放签名重新构造另一套 implementation descriptor；
- 把 `Surface<int>` 的 descriptor 当作 `A<int>.identity` 的 function descriptor；
- 让 witness thunk 直接调用共享体却遗漏 `_func_desc`；
- 按类型名、成员名、`Box` 或是否跨包增加特判。

### 3.4 不增加运行时开销

修复只调整编译期 closed generic demand 的收集、注册和发码顺序。修复后：

- 不新增运行时分支、查找、缓存或堆分配；
- 不新增 witness slot；
- 不改变 `FengTypeDescriptor`、`FengFunctionDescriptor` 或 witness struct 布局；
- 不改变共享方法或 closed wrapper 的隐藏参数顺序；
- 成功路径仍为一次既有 witness 间接调用，再进入 closed wrapper/shared body。

## 4. 修复方向

### 4.1 复用现有 closed callable dependency 管线

spec witness 对一个 closed generic subject 产生需求时，必须让所选实现方法进入普通
closed generic method 使用的现有流程：

```text
closed subject/spec witness demand
  -> 取得声明期选中的 implementation callable
  -> 用 closed owner 实参关闭 implementation owner
  -> 复用现有 callable dependency collect/ensure/register
  -> 递归登记 Box<int> 等直接依赖
  -> 取得/生成现有 closed implementation wrapper
  -> witness thunk 引用该 wrapper
```

如果当前 witness demand 从另一 generic instance 注册入口进入，应把它接到现有统一
closed instance / callable surface 收集入口，不得复制一套仅供 spec witness 使用的
dependency collector。

### 4.2 保持 shell-first 注册顺序

closed owner、closed callable descriptor 及其递归依赖必须继续遵守现有
shell-first 规则：先登记稳定身份 shell，再解析和连接依赖，避免 `A<int>.identity ->
Box<int>` 在递归收集期间因尚未登记而产生假缺失。

具体缺失入口需在实施前由调用链确认；如果实际问题不能通过复用现有
ensure/register 抽象解决，必须暂停并由人工决定，不能增加回退查找或旁路注册。

### 4.3 type、fit、实例与静态路径的边界

当前已确认失败的是“泛型 type 声明头实现 + 实例方法”。泛型 fit、静态 requirement
和跨包恢复是否触发同一缺口，实施时必须按现有合法语言形态进行验证：

- 如果它们已复用正确的 closed callable 管线，只保留回归用例，不作代码修改；
- 如果它们命中同一个通用缺口，由同一抽象修复；
- 如果发现独立根因或需要改变 ABI/运行时开销，记录为独立问题并暂停，交由人工决策。

## 5. 测试要求

### 5.1 Compiler tests

在 `test/` 中验证：

- Semantic 接受最小合法场景；
- Codegen 不再报告 `CE0031`；
- `Box<int>` 等闭合依赖进入实现方法的 `FengFunctionDescriptor`；
- owner `T` 仍从 `_type_desc->reified_generic_params[i]` 读取，没有新增 `_T`
  隐藏参数；
- witness thunk 调用 closed implementation wrapper；
- wrapper 静态传入正确的 `_type_desc` 与 `_func_desc`；
- 两个实现 `A<T>` / `B<T>` 使用不同依赖 `Box<T>` / `Cell<T>` 时，两个 witness
  分别引用各自 closed wrapper 和 descriptor tree；
- 未使用 callable-local dependency 的既有 witness 生成保持不变。

### 5.2 FCTS

在 `fcts/` 中增加实际运行用例，至少覆盖：

- 同一个 `Surface<int>` 视角分别承载 `A<int>` 与 `B<int>`；
- `A<int>.identity` 使用 `Box<int>`，`B<int>.identity` 使用另一闭合泛型类型；
- 两条 witness 调用均返回正确值；
- 同包路径；
- provider 定义泛型 type/spec、consumer 仅使用 package surface 的跨包路径。

泛型 fit、静态 requirement、value type 和嵌套依赖属于验证项：若当前合法基线支持
相应形态，应增加回归覆盖；不得为了制造覆盖而引入新的语言能力。

### 5.3 回归与性能边界

- 运行相关 compiler tests；
- 运行相关 FCTS；
- 检查生成 C 没有新增 runtime resolver/cache/helper 调用；
- 最终在非沙箱环境执行全量 `make test`。

## 6. TODO

- [ ] **[验证]** 以最小探针再次锁定失败和 2.3 节对照矩阵，记录准确失败调用链；
- [ ] **[实际变更]** 找到 spec witness demand 进入 generic instance 注册的入口，让其
  复用现有 closed callable surface/dependency collect/ensure/register 管线；
- [ ] **[实际变更]** 保证 closed implementation wrapper 的
  `FengFunctionDescriptor` shell 及 `Box<int>` 等递归依赖在 witness thunk 发码前完成
  注册；
- [ ] **[验证]** 确认 witness thunk 继续调用现有 closed wrapper，不增加 descriptor
  参数、witness slot、runtime helper 或缓存；
- [ ] **[验证]** 覆盖 generic type 的实例方法及两个不同实现/依赖树；
- [ ] **[验证]** 检查 generic fit、静态 requirement、value type、嵌套依赖的等价合法
  路径；只有命中同一通用根因时才由本修复处理；
- [ ] **[验证]** 覆盖同包与跨包 package consumer，确认两者复用同一闭合链路；
- [ ] **[验证]** 在 `test/` 增加 Semantic、Codegen 与生成 C 结构回归；
- [ ] **[验证]** 在 `fcts/` 增加实际语言行为回归；
- [ ] **[验证]** 检查无类型名、成员名、依赖类型、包或测试场景特判；
- [ ] **[验证]** 在非沙箱环境执行全量 `make test`。

