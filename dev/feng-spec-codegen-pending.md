# spec 运行时与 codegen 可执行方案草案（不含 @fixed / C ABI）

> 本文档是实现草案，不是语言权威规范。
> 语言语义以 [docs/feng-spec.md](../docs/feng-spec.md)、[docs/feng-fit.md](../docs/feng-fit.md)、[docs/feng-function.md](../docs/feng-function.md) 为准。
> 本草案只讨论非 `@fixed` 场景，不涉及 C ABI 与函数指针桥接。

## 1 目标与范围

本草案把当前 `spec` 语义承诺收敛成一版可以直接拆任务实施的方案，覆盖以下能力：

- object-form `spec` 的运行时值表示
- callable-form `spec`（非 `@fixed`）的运行时值表示
- `spec` 默认零值 / 默认 witness 的生成规则
- 具体 `type` 进入 `spec` 位置时的 codegen 降级规则
- `spec` 成员访问、方法调用、字段赋值、参数传递、返回值的发码路径
- 对现有 ARC / finalizer / cycle collector 元数据的接入方式
- 最小测试面与分阶段落地顺序

本草案明确不处理：

- `@fixed spec` 与 C ABI
- object-form `spec` 的去虚化、内联缓存、JIT 风格优化
- 多编译单元链接层面的符号导出稳定化
- 运行时自动发现“谁满足谁”的全局 registry

## 2 设计约束

### 2.1 必须保持的语言语义

以下语义来自权威规范，方案必须满足：

- `spec` 可作为参数类型、返回类型、成员类型和其他类型位置中的引用目标
- object-form `spec` 不约束物理布局，只约束可见形状
- object-form `spec` 的满足关系必须来自声明头或可见 `fit`，禁止鸭子类型
- 无初始值的 `spec` 绑定必须得到默认 witness
- 默认 witness 对开发者不可见，不可显式引用
- 每次 `spec` 默认初始化都创建新实例，不复用共享单例
- `spec` 值的 `==` / `!=` 默认比较引用身份
- callable-form `spec` 是可调用形状，不是对象布局契约

### 2.2 必须贴合的现有运行时基座

当前运行时已经稳定存在以下基础设施：

- 统一托管头 `FengManagedHeader`
- 统一描述符 `FengTypeDescriptor`
- ARC retain/release 与 `release_children`
- cycle collector 对 `managed_fields` 的静态追踪
- `FENG_TYPE_TAG_OBJECT` / `FENG_TYPE_TAG_CLOSURE`

见 [src/runtime/feng_runtime.h](../src/runtime/feng_runtime.h)。

因此，`spec` 的运行时方案必须优先复用现有托管模型，而不是引入独立 GC 体系或与现有头布局冲突的新对象模型。

## 3 总体模型

`spec` 分两类分别落地：

- object-form `spec`
  运行时值表示为“spec box”，内部持有一个具体对象 `subject` 与一张 witness 表 `witness`
- callable-form `spec`
  运行时值直接复用闭包 / 函数值 / 方法值基座，不再额外包一层 object witness box

编译期与运行时职责边界如下：

- 编译期负责决定：
  - 某个 `type` 是否满足某个 `spec`
  - 当前作用域哪些 `fit` 可见
  - 某个赋值 / 传参 / 返回 / 字段初始化应选择哪张 witness 表
- 运行时只负责执行已经选定的 witness thunk 或 callable thunk

运行时不得做以下工作：

- 动态搜索“哪些类型满足某个 `spec`”
- 在运行期重新解释 `fit` 可见性
- 基于对象形状做结构化自动适配

## 4 object-form spec 的运行时结构

### 4.1 结构选择

object-form `spec` 不直接等于具体对象指针，而是一个托管 box：

```c
typedef struct FengSpecRef__Named {
    FengManagedHeader header;
    void *subject;
    const struct FengSpecWitness__Named *witness;
} FengSpecRef__Named;

typedef struct FengSpecWitness__Named {
    const char *debug_name;

    FengString *(*get_name)(void *subject);
    void (*set_name)(void *subject, FengString *value);   /* let 字段时为 NULL */

    FengString *(*display)(void *subject);
} FengSpecWitness__Named;
```

其中：

- `subject` 指向真实的具体对象实例
- `witness` 描述“这个具体对象如何以 `Named` 视角暴露字段与方法”
- `witness` 是静态只读表，不参与 retain/release
- `FengSpecRef__Named` 本身是普通托管对象，参与 ARC 与 cycle collector

### 4.2 为什么用 box，而不是直接用具体对象指针

不建议直接把 object-form `spec` 运行时值表示成具体对象指针，原因有三点：

- 同一个 `spec` 位置上可能接收多个不同具体 `type`，而成员访问不能直接静态绑定到某个对象布局
- `fit` 允许同一对象通过不同路径满足同一个 `spec`；运行时需要一个显式 witness 载体来固定“当前这次转换使用哪张表”
- 现有 ARC / cleanup / array 元素 / 绑定槽位都围绕“一根 managed pointer”工作；box 最贴合现有基座，改动面最小

### 4.3 为什么字段访问用 witness thunk，不用 offset 直读

object-form `spec` 的权威语义是不约束具体物理布局，因此 v1 必须把字段访问与方法调用都建模成 thunk：

- `n.name` 发码为 `n->witness->get_name(n->subject)`
- `n.name = value` 发码为 `n->witness->set_name(n->subject, value)`
- `n.display()` 发码为 `n->witness->display(n->subject)`

不采用 offset 直读的原因：

- 规范已经声明 object-form `spec` 不承诺布局
- `fit` 块未来可能给 `spec` 行为补 thunk，而不是与类型方法一一同名同位
- thunk 是稳定契约；offset 只适合作为后续优化信息，不应成为第一版语义主轴

### 4.4 与现有 `FengManagedHeader` 的关系

本方案不修改 `FengManagedHeader`，也不新增新的 header 形状。

`FengSpecRef__Named` 直接复用当前头：

```c
typedef struct FengSpecRef__Named {
    FengManagedHeader header;
    void *subject;
    const struct FengSpecWitness__Named *witness;
} FengSpecRef__Named;
```

也不要求新增新的 `FengTypeTag`。v1 里 spec box 继续使用：

- `FENG_TYPE_TAG_OBJECT` 作为 object-form spec box 的 tag

理由：

- 当前 tag 只区分是否需要 tag-specific 清理路径
- spec box 没有自己的 tag-specific 清理逻辑，`release_children` 足够表达其托管子引用
- 避免把 tag 变成语义类别枚举；真正的语义差异应由 `FengTypeDescriptor` 驱动

### 4.5 描述符与追踪元数据

每个 object-form `spec` box 有一份独立 descriptor：

```c
const FengTypeDescriptor FengSpecDesc__demo__Named = {
    .name = "demo.Named",
    .size = sizeof(FengSpecRef__Named),
    .finalizer = NULL,
    .release_children = FengSpecRef__Named__release_children,
    .is_potentially_cyclic = true,
    .managed_field_count = 1,
    .managed_fields = FengSpecRef__Named__managed_fields,
};
```

注意点：

- `subject` 是唯一托管槽位，`witness` 不是托管值
- `managed_fields[0].static_desc = NULL`
  原因是 `subject` 是多态的，静态上不固定为某一个具体对象 descriptor
- `is_potentially_cyclic = true`
  原因是 spec box 很容易参与“对象 -> spec box -> 对象”型环路，保守纳入 cycle collector 最稳妥

## 5 object-form spec 的 witness 生成规则

### 5.1 每个 `(具体 type, spec)` 组合生成一张 witness 表

例如：

```feng
spec Named {
    let name: string;
    fn display(): string;
}

type User: Named {
    let name: string;
    fn display(): string { ... }
}
```

生成：

```c
static const FengSpecWitness__Named FengWitness__demo__User__as__Named = {
    .debug_name = "demo.User as demo.Named",
    .get_name = FengWitnessThunk__demo__User__as__Named__get_name,
    .set_name = NULL,
    .display = FengWitnessThunk__demo__User__as__Named__display,
};
```

如果满足关系来自 `fit`，则 witness thunk 指向 fit 提供的实现；如果来自声明头，则指向 type 自身的方法 / 字段访问 thunk。

### 5.2 witness thunk 的责任

witness thunk 是编译器生成的静态适配层，负责：

- 把 `void *subject` 转回具体对象类型指针
- 完成字段读 / 写
- 调用 type 方法或 fit 方法
- 在返回值是 object-form `spec` 时继续做装箱
- 在返回值是 callable-form `spec` 时继续走 callable 形状转换

字段读 thunk 例子：

```c
static FengString *FengWitnessThunk__demo__User__as__Named__get_name(void *subject) {
    struct Feng__demo__User *_self = (struct Feng__demo__User *)subject;
    return _self->name;
}
```

字段写 thunk 例子：

```c
static void FengWitnessThunk__demo__User__as__Editable__set_name(void *subject, FengString *value) {
    struct Feng__demo__User *_self = (struct Feng__demo__User *)subject;
    feng_assign((void **)&_self->name, value);
}
```

## 6 object-form spec 的默认 zero / 默认 witness

### 6.1 语义目标

object-form `spec` 的默认零值必须满足：

- 对开发者不可见
- 每次默认初始化都产生新实例
- 字段按字段类型默认零值初始化
- 方法调用按规范返回默认零值或空实现

### 6.2 生成物

对每个 object-form `spec S`，编译器生成以下隐藏工件：

- 一个隐藏 subject type：`FengSpecDefault__S__Subject`
- 一张默认 witness：`FengSpecDefault__S__Witness`
- 一个默认工厂：`FengSpecDefault__S__new_subject()`
- 一个默认 box 工厂：`FengSpec__S__default_zero()`

结构示意：

```c
typedef struct FengSpecDefault__Named__Subject {
    FengManagedHeader header;
    FengString *name;
} FengSpecDefault__Named__Subject;
```

默认 zero 工厂示意：

```c
static FengSpecRef__Named *FengSpec__demo__Named__default_zero(void) {
    FengSpecDefault__Named__Subject *subject = FengSpecDefault__Named__new_subject();
    FengSpecRef__Named *box = (FengSpecRef__Named *)feng_object_new(&FengSpecDesc__demo__Named);
    box->subject = subject;
    box->witness = &FengSpecDefault__Named__Witness;
    return box;
}
```

### 6.3 默认方法实现

默认 witness 的方法实现按以下规则生成：

- 返回 `void`
  生成空实现
- 返回普通 built-in / string / array / object type
  调用现有默认零值路径
- 返回 object-form `spec`
  继续调用对应 `spec` 的 `default_zero`
- 返回 callable-form `spec`
  生成该 callable-form `spec` 的默认 callable 值

## 7 object-form spec 的相等性规则

这是必须显式定下来的规则。

如果 object-form `spec` 值每次 coercion 都新建 box，那么直接比较 box 地址会导致：

- 同一个底层对象
- 两次转换到同一个 `spec`
- 得到两个不相等的 spec 值

这会让“是否相等”被中间 coercion 次数污染。

因此 v1 规定：

- object-form `spec` 的 `==` / `!=` 比较 `subject` 身份，不比较 box 地址

也就是：

```c
left->subject == right->subject
```

该规则只对 object-form `spec` 生效；普通对象仍按对象身份比较。

## 8 callable-form spec（非 @fixed）

### 8.1 总原则

callable-form `spec` 不是对象布局契约，而是函数形状。

因此它不应再走 object-form `spec` 的“subject + witness box”模型，而应直接复用闭包 / 函数值 / 方法值基座。

### 8.2 运行时承载

本草案假设 callable 值统一落到 closure 基座：

- 顶层函数值：零捕获 closure
- lambda：捕获环境 closure
- 方法值：绑定 `self` 的 closure

每个 closure 的第一成员仍然是 `FengManagedHeader`，tag 为：

- `FENG_TYPE_TAG_CLOSURE`

具体 closure struct 由 codegen 按捕获布局逐个生成，而不是由 runtime 提供一个统一胖结构。

示意：

```c
typedef struct FengClosure__demo__User__say__bound {
    FengManagedHeader header;
    struct Feng__demo__User *self;
} FengClosure__demo__User__say__bound;
```

其对应 invoke thunk 由 codegen 静态生成：

```c
static void FengInvoke__demo__M0__User__say(void *env) {
    FengClosure__demo__User__say__bound *_env = (FengClosure__demo__User__say__bound *)env;
    Feng__demo__User__say(_env->self);
}
```

### 8.3 callable-form spec 与 closure 的衔接方式

对每个 callable-form `spec`，编译器生成一套“期望调用签名”的 thunk typedef / invoke 适配层。

例如：

```feng
spec M0(): void;
```

生成概念上等价于：

```c
typedef void (*FengCallableSig__demo__M0)(void *env);
```

所有能放进 `M0` 位置的值，都必须被 codegen 降成：

- 一块 closure env 对象
- 一个符合 `M0` 调用签名的 invoke thunk

### 8.4 默认 zero / 默认 witness

callable-form `spec` 也必须满足“每次默认初始化创建新实例”。

因此不使用共享单例函数值，而是：

- 每次生成一个零捕获 closure 对象
- 其 invoke thunk 指向编译器生成的默认 stub

例如：

```c
static int FengDefaultStub__demo__Mapper(void *env, int x) {
    (void)env;
    (void)x;
    return 0;
}
```

default zero 工厂则分配一个零捕获 closure，把 invoke 指针绑定到上面的 stub。

## 9 codegen 降级规则

### 9.1 从具体对象到 object-form spec

以下场景都走同一条 coercion 路径：

- `let x: Named = user;`
- `use_named(user);`
- `return user;`，当返回类型是 `Named`
- `obj.field = user;`，当 `field` 的类型是 `Named`

统一发码为：

1. 分配 spec box
2. `box->subject = retain(user)`
3. `box->witness = &<T as S witness>`
4. 返回 box

建议抽一个 codegen helper：

```c
static bool cg_emit_spec_coercion(CG *cg,
                                  ExprResult subject,
                                  const FengDecl *spec_decl,
                                  const VisibleSpecRelation *relation,
                                  ExprResult *out);
```

### 9.2 object-form spec 成员访问

统一改写为 witness thunk 调用：

- 读字段：`spec->witness->get_x(spec->subject)`
- 写字段：`spec->witness->set_x(spec->subject, value)`
- 方法调用：`spec->witness->method(spec->subject, ...)`

### 9.3 从函数值 / 方法值 / lambda 到 callable-form spec

以下场景统一生成 closure：

- `let cb: Mapper = pick;`
- `let cb: Mapper = user.pick;`
- `let cb: Mapper = (x: int) -> x + 1;`
- 参数 / 返回 / 字段初始化中的相同场景

统一发码为：

1. 为当前可调用值选择 invoke thunk
2. 分配对应 closure env 对象
3. 复制并 retain 所有托管捕获
4. 返回 closure 指针

### 9.4 spec 默认零值

当绑定 / 字段 / 返回路径需要 `spec` 默认零值时：

- object-form `spec`
  调用 `FengSpec__S__default_zero()`
- callable-form `spec`
  调用 `FengCallable__S__default_zero()`

### 9.5 数组与嵌套成员

数组元素如果是 `spec`：

- object-form `spec[]` 的元素就是 spec box 指针
- callable-form `spec[]` 的元素就是 closure 指针

对现有 `feng_array_new` 没有 ABI 级改动要求；只需要 element size 和 managed flag 正确即可。

## 10 semantic 需要提供给 codegen 的数据

### 10.1 为什么要优化 semantic

这里的 semantic 优化，核心目的不是为 object-form `spec` 选择 box 还是未来可能的 fat value 布局服务；核心目的是把“具体 `type` 如何进入 `spec` 位置”这件事在语义阶段定型，避免 codegen 承担规则判断。

原因如下：

- `spec` 发码真正缺的，不是单纯的运行时布局信息，而是稳定的“适配结论”
- object-form `spec` 与 callable-form `spec` 都存在“一个具体值进入某个 `spec` 位置时，到底采用哪条满足关系、哪张 witness、哪种 coercion”的问题
- 这些判断依赖声明头、可见 `fit`、当前作用域可见性与成员形状，属于语义规则，不属于 codegen/runtime 的职责
- box、single-managed-first-fat、closure 这些只是值表示选择；无论选哪一种表示，`type -> spec` 的关系选择都仍然必须先被确定

如果不在 semantic 侧补这层结果，而让 codegen 自行重复推导，会出现以下问题：

- 同一套满足关系规则在 semantic 与 codegen 各实现一遍，后续容易漂移
- `fit` 可见性、歧义与报错时机不稳定，可能出现 semantic 放行、codegen 才失败的分层错位
- codegen 需要知道过多语言规则细节，导致 object-form `spec`、callable-form `spec`、默认 witness、返回值/参数 coercion 各自重复判断
- 后续即使把 object-form `spec` 从 box 改成其他布局，semantic 与 codegen 的职责边界仍然不清晰

因此，本草案要求把 semantic 的优化目标表述为：

- semantic 负责产出“这个值如何适配成某个 `spec`”的结论
- codegen 负责按该结论选择具体 lowering
- runtime 只承接 lowering 后的布局与生命周期，不再参与契约规则判断

### 10.2 semantic 需要稳定产出的事实

为避免 codegen 重新做契约推导，semantic 应提供下列可消费事实：

- 某个命名类型引用是否为 object-form `spec` / callable-form `spec`
- 某个表达式从具体 `type` 进入 `spec` 位置时，应使用哪条可见满足关系
- 该次适配是对象契约适配还是可调用形状适配
- 该次适配需要落到哪张 witness 表或哪组 callable invoke 适配信息
- 某个 `fit` 是否在当前模块 / 文件作用域内可见
- 对于 object-form `spec` 的每个成员：
  - 是字段还是方法
  - 字段是 `let` 还是 `var`
  - 成员类型 / 参数类型 / 返回类型

语义阶段最终最好显式产出一个“spec coercion / witness resolution”结果。是否做成独立 AST 节点、绑定注记或 codegen 可直接消费的分析表，可以后定；但最终必须满足以下边界：

- “可见关系选择”只在 semantic 做一次
- codegen 不自行推断 fit 可见性
- codegen 不自行重新决定 object-form / callable-form 的适配路径
- codegen 不负责把“是否可适配”升级成“如何适配”的规则推导

### 10.3 与值表示选择的关系

需要明确：这里的 semantic 优化，与 object-form `spec` 最终选择 box 还是 fat value 没有直接绑定关系。

- 如果继续使用 box，semantic 仍然要告诉 codegen：当前 coercion 采用哪条关系、绑定哪张 witness、默认 witness 走哪条链路
- 如果未来改为 single-managed-first-fat，semantic 侧要提供的仍然是同一类结论；变化主要发生在 codegen/runtime 的值布局、cleanup、数组与 retain/release 路径

也就是说：

- semantic 优化解决的是“规则归谁决定”
- box / fat 选择解决的是“值在内存里怎么表示”

两者相关，但不是同一个层面的决策。

第一阶段可以不强制把这些做成新的公共 semantic API；允许 codegen 暂时沿用现有分析结果做受控二次查询，但该做法只能作为过渡，不应成为长期边界。

## 11 runtime 层建议变更

### 11.1 `feng_runtime.h`

v1 不要求修改 `FengManagedHeader`。

也不强制新增公用 `feng_spec_*` helper。理由：

- 现有 `feng_object_new` + `feng_assign` 足以支撑 spec box 与 closure env 的分配
- 过早把 `spec` 细节暴露到公共 runtime ABI，会把仍在演进的设计冻结得太早

建议仅在需要时新增少量注释，说明：

- codegen 生成的 spec box / closure env 也是普通 managed object
- polymorphic managed slot 的 `static_desc` 可以为 `NULL`

### 11.2 runtime 源文件

第一阶段 runtime 不需要新增专门的 `feng_spec.c`。

原因：

- object-form `spec` box 的分配可直接走 `feng_object_new`
- object-form `spec` 的释放可直接由 codegen 生成 `release_children`
- callable-form `spec` 复用 closure 基座

如果后续需要：

- 统一的 spec equality helper
- 统一的 debug 打印
- 通用 witness 调用 trampoline

再单独评审是否引入 `feng_spec.c`。

## 12 测试面

### 12.1 semantic

新增 / 扩充以下语义用例：

- object-form `spec` 参数接受声明头满足的 `type`
- object-form `spec` 参数接受可见 `fit` 满足的 `type`
- object-form `spec` 参数拒绝仅形状相同但未显式满足的 `type`
- object-form `spec` 字段初始化接受满足关系
- object-form `spec` 字段初始化拒绝未满足关系
- callable-form `spec` 绑定接受函数值 / 方法值 / lambda
- callable-form `spec` 默认 witness 的返回类型链路通过

### 12.2 codegen / smoke

至少新增以下 smoke：

- `spec_object_param.ff`
  具体对象传入 `spec` 参数并通过 spec 视角访问字段 / 方法
- `spec_object_field.ff`
  对象字段类型为 `spec`，赋值后通过字段访问其成员
- `spec_object_default.ff`
  `let s: Named;` 使用默认 witness，字段为默认零值，方法返回默认零值
- `spec_callable_bind.ff`
  顶层函数 / lambda / 方法值绑定到 callable-form `spec`
- `spec_callable_default.ff`
  `let f: Mapper;` 调用返回默认零值
- `spec_equality.ff`
  同一 subject 经两次 coercion 到同一 object-form `spec` 后仍相等

### 12.3 runtime

如果新增运行时 helper，再补 `test/runtime/test_runtime.c`；否则第一阶段无需 runtime 单测新增文件。

## 13 分阶段实施顺序

### Phase A

- 不改公共 runtime ABI
- 补齐最小可消费的 spec coercion / witness resolution 语义结果，避免 codegen 自行重复推导可见关系
- 只实现 object-form `spec` box + witness table
- 只支持“具体对象 -> object-form spec” coercion
- 支持字段访问 / 方法调用 / 参数传递 / 返回值 / 默认 witness

### Phase B

- 实现 callable-form `spec` 的 closure 基座接入
- 打通函数值 / 方法值 / lambda 到 callable-form `spec`
- 实现 callable-form `spec` 默认 zero

### Phase C

- 处理 object-form `spec` 与 callable-form `spec` 互相嵌套的完整路径
- 增加 debug / equality / devirtualization 的可选优化点

## 14 非目标与禁止项

以下做法在本方案中明确禁止：

- 运行时通过全局 registry 动态判断“某对象是否满足某个 spec”
- object-form `spec` 直接复用具体对象指针而完全不携带 witness
- object-form `spec` 通过字段 offset 直读作为唯一契约实现
- 以共享单例实现 `spec` 默认 witness
- 因 `fit` 可见性复杂而把契约选择推迟到运行时

## 15 验收标准

实现完成时，应满足以下验收标准：

- `spec` 类型可以出现在参数、返回值、成员字段、局部绑定中
- object-form `spec` 的 coercion、字段访问、方法调用、默认 witness 均可正确发码
- callable-form `spec` 的函数值 / 方法值 / lambda 绑定与调用可正确发码
- `spec` 默认零值不使用 `NULL`
- `spec` 值比较不因 coercion 次数改变语义
- 不修改 `FengManagedHeader` 布局
- 不引入与 `@fixed` / C ABI 绑定的额外前提

## 16 建议的首个实现切片

如果按“最小可落地”开始，建议第一刀只做以下子集：

- object-form `spec` box + witness table
- 具体对象传入 `spec` 参数
- `spec` 视角字段读取 / 方法调用
- object-form `spec` 默认 witness
- 一组 smoke 覆盖 `Named` / `Displayable` 这类简单对象契约

理由：

- 这条路径只依赖现有 object runtime，不依赖 closure runtime
- 可以最先验证“subject + witness + default box”是否正确
- 一旦这条链路稳定，callable-form `spec` 的接入就只是把 callable 值并入同一套“默认 zero + 类型位置 coercion”框架