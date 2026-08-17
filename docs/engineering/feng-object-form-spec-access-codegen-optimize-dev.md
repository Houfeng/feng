# Feng object-form `spec` 参数提升与字段 offset 发码优化开发草案

> **状态**：待 Review，尚未实施。
>
> **性质**：Codegen 性能优化开发草案，不新增或修改 Feng 语言语义。
>
> 当前 object-form `spec` 的运行时基线见
> [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md)。本文 Review 通过并完成实施后，
> 必须同步更新该已交付文档中的 witness 字段 ABI；本文只保留开发范围、实施顺序和验收记录，
> 不与已交付文档长期并列定义两套现行 ABI。

## 1 背景

object-form `spec` 当前使用胖值表示：

```c
struct FengSpecValue__S {
    void *subject;
    const struct FengSpecWitness__S *witness;
};
```

实例方法、实例字段当前分别按下列形式访问：

```c
value.witness->method(value.subject, args...);
value.witness->get_field(value.subject);
value.witness->borrow_field(value.subject);
value.witness->set_field(value.subject, next);
```

其中：

- 具体 `type` 的普通实例方法调用是已知 C 函数符号调用；
- `spec` 方法必须保留运行时间接分派，因为实际 subject 类型可以不同；
- `spec` 字段已经由编译期 witness 唯一映射到实际字段存储，不需要通过函数分派实现读写；
- 当前字段 getter / borrow / setter thunk 会为每次字段访问增加一次间接调用；
- 同一 `spec` 参数的每次成员访问还会重复从胖值中读取 `subject` 和 `witness`。

本开发项只消除后两类可避免开销，不处理方法去虚。

### 1.1 现有“字段必须经 thunk、禁止 offset 直读”的来源

该限制最初写入 2026-04-29 的第一版 spec codegen 草案（commit `0a2e299d`），随后在
pending / delivered 文档中沿用。初始草案给出的理由是：

1. object-form `spec` 不承诺具体 type 的物理布局；
2. 同一 spec 可以由多个布局不同的 type 满足；
3. `fit` 可能提供与 type 自身方法不同的行为实现；
4. thunk 是容易先闭环的稳定契约，offset 应留作后续优化信息，而不是第一版语义主轴。

这个选择对第一版实现是合理的：当时需要先统一字段与方法的 erased `void *subject` 适配、
字段写入的 ARC、默认 witness 和后续 `fit` 方法分派，getter / setter thunk 能把具体布局与
值模型细节集中封装在每张 witness 的适配函数中，实施风险最低。

但其中必须区分两种完全不同的“offset 直读”：

- **错误方案**：把 spec 成员绑定为相对于某个统一 spec 布局的固定 offset。该方案确实违反
  “spec 不约束实现布局”，多个实现 type 会立即错位；
- **本草案方案**：每张 `(具体实现, spec, subject storage kind)` witness 记录自己相对于实际
  subject 基址的 offset。不同 type、managed / value box / default subject 可以记录不同
  offset，因此仍不要求实现共享布局。

当前语言与实现还提供了更明确的事实边界：

- `fit` 不得声明或补充存储字段；spec 实例字段只能映射到 type 自身字段。因此 `fit` 会影响
  方法 thunk，但不会使实际字段存储脱离 type；
- Semantic witness 已经把每个 spec 字段映射为 `TYPE_OWN_FIELD`，Codegen 能确定实际字段；
- 当前 aggregate `borrow_field` thunk 已经返回实际字段地址，说明实现本身已经依赖 subject
  内部字段存储地址稳定；
- getter / setter thunk 中的 retain / release / aggregate assign 属于字段类型的值模型，
  可以在调用点取得字段地址后继续使用同一 helper，不要求由函数 thunk 承载。

因此，现有禁令中仍然成立的是“不能使用脱离具体 witness 和 subject 表示的统一固定
offset”；不再足以否定“由每张 witness 携带实际 offset”的方案。

### 1.2 为什么仍不能做成无条件的单一 offset

第一版禁令之后，编译器已经增加 default subject、`@value` box、reified layout、父 spec、
intersection spec，以及 object-form spec 值作为受约束泛型实参的 slot witness adapter。
这些能力进一步证明：不能只按 `(spec, field)` 生成一个全局 offset。

其中大多数形态都可以由各自 witness 记录相对于其真实 subject 表示的 offset；唯一不能直接
化为外层固定 offset 的既有路径，是“spec 胖值本身作为 constrained generic 实参”：外层
subject 指向 `{subject, witness}` 胖值，实际字段位于胖值内部动态 subject 所指对象中。
因此本草案在 §6 为这条既有 adapter 路径保留字段地址 resolver，同时让普通 object-form
spec 值与具体 type 泛型实参走 offset 快路径。

## 2 Review 结论项

第一版只实施以下两个优化：

1. **提升 `subject`、`witness`**：在函数或方法入口，把稳定 object-form `spec` 参数的
   `subject` 与 `witness` 分别保存为 C 局部绑定，后续成员访问复用这两个绑定。
2. **实例字段改用 offset**：object-form `spec` witness 的实例字段槽记录相对于当前
   subject 存储基址的字段 offset；字段读写在调用点通过 `subject + offset` 直接访问。

第一版同时确认以下边界：

- 不提升或缓存单个字段的投影地址；
- 不缓存字段值；
- 不优化或去虚 `spec` 方法调用；
- 不改变静态字段、静态方法的现有 witness ABI；
- 不扩展到 constrained generic 参数的 `subject` / `witness` 入口提升；
- 不改变 object-form `spec` 胖值布局；
- 不改变 `type` 自身成员访问与可见性；
- 不改变语言语义、Semantic 满足规则、`.ft` 语义信息和 Feng runtime 私有 ABI；
- 不以字段类型、具体 TUI 类型或成员名称增加特判。

## 3 目标与非目标

### 3.1 目标

- 同一稳定 `spec` 参数的成员访问不重复读取胖值中的 `subject` / `witness`；
- 普通 object-form `spec` 实例字段读写不再经过 getter / borrow / setter 函数指针；
- `let`、`var`、普通值、托管引用、固定聚合和 reified 聚合字段保持现有值模型；
- managed type、`@value` box、默认 spec subject、父 spec、intersection spec、泛型约束和
  跨包 `.ft` 场景保持行为一致；
- 既有“object-form `spec` 值作为受约束泛型实参”的 slot witness adapter 能力不得退化；
- 优化后的生成 C 结构可由 focused codegen 测试稳定验证。

### 3.2 非目标

- 不证明实际 subject 类型，不将 witness 方法槽改为直接函数符号；
- 不对 spec 方法做单态化、内联缓存、whole-program devirtualization 或 LTO 专项优化；
- 不把 `widget.field` 的结果自动提升为 Feng 局部绑定；
- 不在多个函数之间共享字段投影；
- 不做控制流数据流分析、alias analysis、写后失效分析或循环不变量外提；
- 不缓存 `var` 字段的值；
- 不缓存 `subject + offset` 得到的字段地址；
- 不修改 callable-form `spec`；
- 不修改静态 spec 字段的 getter / setter；
- 不引入新的 runtime API、runtime descriptor 字段或动态 registry；
- 不修改无关的泛型、`fit`、`@mixable`、TUI 或标准库实现。

## 4 优化一：提升 `subject` 与 `witness`

### 4.1 适用对象

第一版只处理函数、实例方法、静态方法和 `fit` 方法中的 object-form `spec` 形参，且该
形参在函数体内必须是稳定绑定：

- 默认 `let` 形参：允许提升；
- 显式 `var` 形参：第一版不提升；
- object-form `spec` 局部变量、字段、数组元素和任意临时表达式：第一版不提升；
- `T: S` constrained generic 形参：第一版不纳入本项提升。

显式 `var` spec 形参可能在函数体内被重新赋值。若仍沿用入口处保存的旧
`subject` / `witness`，后续成员访问会落到旧值，因此第一版直接排除，不增加重写后刷新或
控制流失效逻辑。

### 4.2 发码形式

源代码示例：

```feng
func layout(widget: Widget): int {
  let width = widget.rtStyle.width;
  let height = widget.rtStyle.height;
  return width + height;
}
```

优化后的生成 C 形态：

```c
void *const _widget_subject = widget.subject;
const struct FengSpecWitness__Widget *const _widget_witness = widget.witness;

/* 后续成员访问只使用两个提升后的局部绑定。 */
```

要求：

1. 提升发生在函数入口，且每个适用参数最多生成一组绑定；
2. 生成名必须走现有 codegen 临时名唯一化机制；
3. `subject` 是借用的托管指针，提升不得增加 retain / release；
4. `witness` 是静态只读表指针，提升不参与生命周期管理；
5. 原 spec 参数仍是 subject 生命周期的所有权依据，现有参数清理约定不变；
6. 方法调用、字段访问和父 spec 视角访问统一复用提升后的绑定；
7. 参数完全未发生 spec 成员访问时，可以不生成无用绑定，但这只是发码清理，不改变入口提升语义。

### 4.3 方法调用保持间接

提升后，方法调用由：

```c
widget.witness->draw(widget.subject, frame);
```

变为：

```c
_widget_witness->draw(_widget_subject, frame);
```

调用目标仍是 witness 方法函数指针。第一版只减少胖值字段的重复读取，不宣称方法调用已
等价于具体 `type` 的直接函数调用。

## 5 优化二：实例字段 witness 改为 offset

### 5.1 核心模型

对于能够直接定位实际字段存储的 witness，实例字段槽由 getter / borrow / setter 函数指针
改为一个 `size_t` offset：

```c
struct FengSpecWitness__Widget {
    size_t offset_rtStyle;
    size_t offset_frame;
    void (*draw)(void *_subject);
};
```

具体 `(T, S)` witness 按其 subject 表示填写 offset：

```c
static const struct FengSpecWitness__Widget FengWitness__Button__as__Widget = {
    .offset_rtStyle = offsetof(struct Button, rtStyle),
    .offset_frame = offsetof(struct Button, frame),
    .draw = &FengSpecThunk__Button__as__Widget__draw,
};
```

offset 始终相对于该 witness 所配套的 `subject` 基址，而不是相对于某个统一 type 布局。
因此，不同实现 type 可以具有完全不同的字段顺序和物理布局；每张 witness 记录自己的
offset，不改变 `spec` 不约束实现布局的语义。

### 5.2 字段地址与读写

每次字段访问按需计算地址：

```c
void *_field_addr = (void *)((char *)_widget_subject +
                             _widget_witness->offset_rtStyle);
```

第一版不把 `_field_addr` 持久提升为函数局部缓存。生成表达式应直接接入现有字段值模型：

- trivial 字段读：从 typed address 直接读取；
- managed 字段读：从 typed address 读取借用值，沿用现有 `owns_ref = false` 规则；
- aggregate / reified aggregate 字段读：保留字段存储地址和现有 descriptor authority；
- trivial `var` 字段写：直接 store；
- managed `var` 字段写：继续使用 `feng_assign`；
- aggregate `var` 字段写：继续使用 `feng_aggregate_assign` / take 等现有值模型 helper；
- `let` 字段：Semantic 继续禁止写入，不因 offset 获得可写能力。

offset 只替代“如何定位字段存储”，不替代字段类型已有的复制、移动、retain、release、默认值
和 reified descriptor 规则。

### 5.3 subject 表示对应的 offset

同一 Feng type 可能因使用位置不同而具有不同 subject 表示，offset 必须由 witness 实例的
实际 subject storage kind 决定：

| subject 形态 | offset 基址 |
| --- | --- |
| 普通 managed type | managed object 起始地址 |
| 固定布局 `@value` 作为 constrained generic 值 | 当前值存储起始地址 |
| `@value` 转换为 object-form spec 后的 box | value box 起始地址，offset 包含 box 中 `value` 前缀 |
| 默认 spec 值 | 隐藏 default subject 起始地址 |
| 父 spec / intersection spec 的同一具体 subject | 继续相对于同一具体 subject 基址 |
| reified 布局 | 由已闭合实例或现有 reified field-offset authority 物化出当前 witness 所需 offset |

不得假定“同一 type 的所有 witness 共用同一 offset”，也不得把 `@value` box offset 错用于
未装箱的 constrained generic 值。

### 5.4 静态字段保持现状

静态字段没有实例 subject，不能表示为 `subject + offset`。第一版保持现有静态 getter /
setter witness 槽及其调用路径不变：

```c
witness->get_static_field();
witness->set_static_field(value);
```

这不是针对字段类型的特判，而是实例存储与静态存储具有不同寻址模型。若未来优化静态字段，
必须单独设计静态 storage address ABI，不纳入本开发项。

## 6 既有 slot witness adapter 的兼容方案

### 6.1 为什么不能全部直接写成 offset

Feng 当前允许 object-form `spec` 值本身作为受约束泛型实参。该路径中的 generic slot
witness 以“spec 胖值的地址”为 subject，再从胖值中读取动态 `subject` / `witness`，转发到
实际实现。

例如，外层 subject 的布局只有：

```c
{ subject, witness }
```

实际业务字段不在这个胖值内部，因此不存在一个相对于外层 subject 的固定 offset 可以直接
定位业务字段。直接删除 slot witness 转发会使既有泛型能力退化，不可接受。

### 6.2 建议的最小兼容表示

对含实例字段的 object-form witness 增加一个共享的可选字段地址解析器；每个实例字段仍只
保留一个 offset 槽：

```c
struct FengSpecWitness__S {
    void *(*resolve_field)(void *_subject, size_t _field_index);
    size_t offset_field_a;
    size_t offset_field_b;
    /* 方法槽、父 witness 槽保持现状。 */
};
```

规则：

- 普通 `(T, S)`、default、value box、父 spec 和 intersection witness：
  `resolve_field = NULL`，字段槽保存真实 offset；
- object-form spec 值作为受约束泛型实参时生成的 slot witness adapter：
  `resolve_field` 指向 adapter resolver，offset 槽只提供稳定字段序号；
- 普通 object-form `spec` 值成员访问只允许携带 storage-rooted witness，直接使用 offset，
  不增加运行时分支；
- constrained generic `T: S` 成员访问检查 `resolve_field`：为空时走 offset 快路径，非空时
  只在现有 spec-to-spec adapter 场景调用 resolver；
- resolver 返回实际字段存储地址，后续读写继续使用 §5.2 的同一值模型；不保留 getter 与
  setter 两套 fallback；
- resolver 必须支持嵌套 adapter，且不得分配对象、复制字段值或改变所有权。

该 resolver 是保留既有泛型能力所需的兼容机制，不是第三项优化，也不扩展语言能力。若
Review 要求 witness 中所有实例字段在所有路径下都必须是无 fallback 的纯 offset，则必须
先另行修改 constrained generic 的 subject/descriptor ABI；该修改超出本开发项范围，第一版
不得私自实施。

## 7 ABI、跨包与兼容性

### 7.1 不变项

- `FengSpecValue__S { subject, witness }` 布局不变；
- spec 值参数、返回、字段和数组元素的值模型不变；
- Feng runtime API 与私有 runtime descriptor 不变；
- Semantic witness 成员来源仍为 type field / type method / fit method；
- `.ft` 继续记录声明、成员、可见性、满足关系和生成 witness 所需的既有信息；
- 方法槽、父 witness 槽和静态成员槽的语义不变。

### 7.2 发生变化的生成 C ABI

object-form `spec` witness struct 的实例字段槽布局会变化，因此这是生成 C 层的 witness ABI
迁移。实现时必须：

1. 一次性迁移 witness struct、default witness、具体 type witness、value box witness、父
   witness、intersection witness、generic witness 和 slot witness adapter；
2. 禁止同一次编译中混用旧 getter/setter witness 与新 offset witness；
3. 验证 imported type / spec 的现有 `.ft` 信息足以在 consumer 侧生成正确 offset；
4. 如确认必须新增 `.ft` 字段或 runtime ABI，停止实施并提交人工 Review，不得隐式扩面；
5. 全量重编译参与链接的 Feng 产物，不承诺与旧生成对象文件二进制兼容。

## 8 正确性约束

### 8.1 稳定地址

offset 方案依赖 subject 所指对象或值存储在一次成员访问期间不移动。当前 getter / borrow
thunk 已经直接获取同一对象内部字段，aggregate borrow 也已经要求稳定存储，因此本方案不
新增可移动对象假设。

### 8.2 `var` 与 alias

- 不缓存 `var` 字段值，后续读取总是重新从字段地址取当前值；
- 不缓存字段地址，第一版不存在控制流写后失效问题；
- 通过其他 alias、方法调用或 setter 修改字段后，下一次读取仍从同一存储读取最新值；
- spec 参数自身为 `var` 时不提升 subject/witness，避免参数改绑后使用旧 receiver。

### 8.3 生命周期

提升的 subject 只是借用别名；字段 offset 只是整数。两者都不得新增 cleanup entry。
字段读取和写入必须继续复用当前 `ExprResult` 所有权标记、aggregate descriptor、retain/release
与 assign/take helper，不能因去掉 thunk 而把 borrowed read 错标为 owned。

## 9 预期生成代码与性能边界

以重复读取 `widget.rtStyle` 为例，普通 object-form `spec` 参数的目标形态是：

```c
void *const _widget_subject = widget.subject;
const struct FengSpecWitness__Widget *const _widget_witness = widget.witness;

struct Style *_style_a = *(struct Style **)(
    (char *)_widget_subject + _widget_witness->offset_rtStyle);
struct Style *_style_b = *(struct Style **)(
    (char *)_widget_subject + _widget_witness->offset_rtStyle);
```

相较当前实现，普通实例字段每次访问消除一次 getter / borrow / setter 间接调用，并避免重复
读取胖值的 `subject` / `witness`。仍然保留：

- 每次字段访问读取 witness 中的 offset；
- 每次字段访问执行一次地址加法；
- 字段本身是引用时的正常指针读取；
- `rtStyle.xxx` 对具体 `Style` 成员的后续访问成本；
- spec 方法的函数指针调用。

第一版不承诺固定的整程序耗时提升比例。验收以生成 C 结构、行为正确性和基准数据为证据，
不得把“字段访问已接近具体 type 的直接字段访问”误写成“spec 调用已等价于具体 type”。

## 10 实施影响面

预计只涉及以下职责，不在本草案中锁定具体函数名：

| 层次 | 变更 |
| --- | --- |
| 权威工程文档 | 更新 object-form spec witness 字段 ABI，删除“禁止 offset”现行描述 |
| Codegen spec member model | 区分实例字段 offset、静态字段 accessor、方法槽 |
| Witness struct emit | 实例字段发 offset；按需增加共享 `resolve_field` |
| Witness instance emit | 为各 subject storage kind 生成正确 offset 或 adapter resolver |
| Spec 参数函数入口 | 为稳定 object-form spec 参数发射 subject/witness 局部绑定 |
| Spec 成员读写 | 统一从字段地址进入现有值模型，不再调用实例 getter/borrow/setter |
| Generic constraint lowering | 直接 witness 走 offset，spec slot adapter 走 resolver fallback |
| 测试 | codegen 结构、Feng 行为、跨包和全量回归 |

Semantic 不应因本优化产生语言行为变更。若实施中发现必须修改满足关系、可见性或成员解析，
应视为超出范围并停止。

## 11 测试方案

### 11.1 Focused codegen 测试

必须验证生成 C：

1. 稳定 object-form spec 参数在入口只生成一组 subject/witness 绑定；
2. 重复方法和字段访问复用提升绑定；
3. 显式 `var` spec 参数不使用入口提升绑定；
4. 实例字段 witness 使用 offset，不生成对应 getter / borrow / setter thunk；
5. 静态字段继续使用现有 getter / setter；
6. trivial、managed、固定 aggregate、reified aggregate 和 object-form spec 字段的地址类型正确；
7. `var` managed 字段仍调用 `feng_assign`；
8. `var` aggregate 字段仍调用正确 aggregate helper；
9. managed type、`@value` local/box 和 default subject 的 offset 基址正确；
10. 父 spec 与 intersection witness 的字段 offset 指向同一实际 subject 存储；
11. constrained generic 的具体 type 实参走 offset 快路径；
12. object-form spec 值作为 constrained generic 实参时走 resolver fallback；
13. 嵌套 slot witness adapter 不递归错位且不生成额外分配；
14. 跨包 imported type/spec 生成的 witness offset 可编译、可链接；
15. 方法槽仍为函数指针，未发生非预期去虚或 ABI 改写。

### 11.2 FCTS / smoke 行为测试

新增用例覆盖：

- 多个不同布局 type 满足同一含字段 spec，并读到各自正确字段；
- `let` 字段读取；
- trivial / managed / aggregate `var` 字段连续读写；
- alias 或方法修改字段后，通过 spec 再次读取到最新值；
- `@value` 转 spec 后读取字段；
- 默认 spec 值字段读写；
- 父 spec / intersection spec 字段访问；
- object-form spec 值作为受约束泛型实参的字段读写；
- provider/consumer 跨包字段访问；
- 字段读写过程中的托管对象析构次数正确，防止 retain/release 退化。

### 11.3 性能验证

性能验证属于“验证”，不新增第三项优化：

- 对重复字段访问生成 C 做结构检查，确认普通路径不存在实例字段间接调用；
- 使用现有可重复执行的 benchmark（如存在）对比优化前后数据；
- 若当前仓库没有稳定 benchmark，只记录生成 C 结构与可重复的局部测量，不为噪声数据设置
  人为通过阈值；
- 编译器自身发码时间和生成 C 体积不得出现无法解释的明显退化。

### 11.4 全量回归

所有非文档实现完成后，必须在 Codex 沙箱外执行：

```text
make test
```

不得只以 focused 测试或 smoke 替代全量回归。

## 12 实施 TODO

- [ ] **D1 文档收口**：Review 通过后，先更新
  [feng-spec-codegen-delivered.md](./feng-spec-codegen-delivered.md)，把实例字段 witness 的现行 ABI
  从 getter/borrow/setter 收口为本方案；静态字段保留 accessor。
- [ ] **C1 建立字段投影抽象**：在 Codegen 内统一表示“实例字段 offset / 静态字段 accessor /
  slot adapter resolver”，禁止在各 witness 生成分支重复拼接规则。
- [ ] **C2 迁移 witness struct ABI**：实例字段发 offset，并为含实例字段的 witness 增加共享
  `resolve_field` 兼容槽；方法、父 witness、静态成员保持现状。
- [ ] **C3 迁移直接 witness**：覆盖 managed type、固定 `@value`、boxed `@value`、default
  subject、父 spec、intersection spec 和 reified 布局，确保 offset 相对正确 subject 基址。
- [ ] **C4 迁移 generic slot adapter**：直接具体实参走 offset；object-form spec 实参通过
  `resolve_field` 返回实际字段地址；验证嵌套 adapter。
- [ ] **C5 统一字段读写**：删除实例 getter/borrow/setter 调用路径，从投影地址进入现有
  trivial / managed / aggregate / reified 值模型；静态字段路径不变。
- [ ] **C6 实施参数提升**：为稳定 object-form spec 形参生成入口 subject/witness 绑定，并让
  方法、字段和父 spec 访问复用；显式 `var` 形参保持现状。
- [ ] **T1 新增 focused codegen 测试**：覆盖 §11.1 的 ABI 与生成 C 形态。
- [ ] **T2 新增 FCTS / smoke**：覆盖 §11.2 的行为、泛型、跨包和生命周期场景。
- [ ] **T3 验证性能边界**：按 §11.3 记录结构证据和可重复测量结果。
- [ ] **T4 全量回归**：在 Codex 沙箱外执行 `make test`，记录结果。
- [ ] **D2 交付收口**：回填实施差异、测试结果和性能证据；确认没有遗留旧实例字段 thunk
  路径后，将本文状态改为已交付。

实施顺序必须为 `D1 → C1 → C2 → C3 → C4 → C5 → C6 → T1 → T2 → T3 → T4 → D2`。
其中 C2—C5 是一次 witness ABI 迁移，禁止提交或交付长期混合态。

## 13 Review 检查表

- [ ] 第一版是否确认只包含 subject/witness 提升和实例字段 offset 两项优化？
- [ ] 是否确认只提升稳定 object-form spec 形参，显式 `var` 形参暂不提升？
- [ ] 是否确认不提升字段地址、不缓存字段值？
- [ ] 是否确认方法调用仍经 witness 函数指针？
- [ ] 是否确认静态字段保持 getter/setter，不纳入 offset？
- [ ] 是否接受 §6 的共享 `resolve_field` 作为保留既有 spec-as-generic 能力的兼容 fallback？
- [ ] 是否确认不修改 `.ft` 格式和 Feng runtime 私有 ABI；若实施证明必须修改，则停止 Review？
- [ ] 是否确认所有既有 object-form spec 能力必须保持，全量回归为交付条件？
