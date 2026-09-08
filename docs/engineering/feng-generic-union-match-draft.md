# 共享泛型体内联合类型收窄设计方案

> 状态：描述符承载、依赖归属与稳定槽位方向已确认；未实施，未验证性能。
>
> 更新：2026-09-08。沿用 2026-07-07 草案的描述符驱动方向，修正把裸 T 当作完整 union
> 读取的假设。投影依赖收敛到现有类型／函数描述符，不再采用独立投影上下文隐藏参数。
>
> 所属任务：[G24](./feng-language-conformance-coverage-hardening-pending.md#28-g24复合类型诊断)；
> 问题：[ISSUE-G24-004](./feng-language-conformance-coverage-hardening-issues/g24.md#issue-g24-004union-约束泛型体的合法-match-未正确发码)。

## 1. 结论与批准边界

采用“原始 T + 描述符上的 reified_union_projections + 普通联合匹配后端”。

参数仍按实际 T 传递；匹配时消费编译期确定的判别与取值信息。若收窄结果已经实际存在，
直接使用对应存储；若绑定要求一个原存储中不存在的中间联合值或 spec 值，才为该绑定
执行正常值构造。不统一包装入参，不改变 T 的默认零值，不把支持范围缩减为直接 member。

人工已确认三种上下文描述符增加同名投影表入口，并按投影依赖的归属而非泛参声明位置选择
读取来源；各归属内使用稳定槽位。字段及表项见 §5、§7，不增加独立隐藏参数，也不借机迁移
方法级泛参。这个方向确认不等于产品实现或性能验收；准入边界、私有 ABI／格式迁移及具体
成本门槛分别见 G24 主文档 §28.1.3。不得把“运行时开销尽可能小”解释为任意新增成本均获批准。

语言规则仅引用以下主规范，本文定义实现分工与验收，不另立一套收窄规则：

- [联合类型 §3.6、§3.8、§3.12、§3.13](../specifications/feng-union-type.md)。
- [spec form 与成员合法性](../specifications/feng-spec.md)。
- [泛型类型保持与开销要求](../specifications/feng-generics-draft.md#4-语义)。
- [绑定规则](../specifications/feng-binding.md)与[流程控制规则](../specifications/feng-flow.md)。

## 2. 已有基础与不变项

### 2.1 已有基础

- Semantic 已有联合成员信息、进入路径选择、标签检查及收窄绑定类型。
- Codegen 已有普通联合的 tag 条件、链式 payload 路径，以及共享体内真正的泛型联合值存储。
- FengGenericParamDescriptor 已能按实际 T 完成复制、释放、大小查询与默认初始化。
- 具体化依赖及真实 package-public .ft 已有导出、恢复与闭合生成通路。
- 现有缺口是开放 T 的匹配发码不能消费 union 约束视角，而不是缺少整个泛型调用机制。

FengGenericParamDescriptor 是执行时实际 T 的值分类、值描述符与约束 witness 信息，不是等待
运行时解析的类型占位。reified_* 是当前类型／callable 具体化后需要的上下文，其中
reified_generic_params 收纳泛参自身的描述符，其余依赖数组收纳该上下文实际使用的闭合依赖。
本方案的投影表属于后一类：每项对应一条收窄使用路径，不是给每个 T 建立完整反射视图。

### 2.2 本方案不改变

- Lexer、Parser 与源级 AST 的语法形态。
- T 的类型身份、原始参数／返回表示、字段／数组元素布局及默认零值。
- FengGenericParamDescriptor 的 kind、descriptor、witness 字段与含义，以及 FengTrivialDescriptor。
- 原有类型级泛参和方法级泛参的获取通路、共享入口的参数列表；不增加投影专用隐藏参数。
- union 现有 tag、转发槽与 payload 布局，及 ARC、默认初始化算法。
- union 约束的 witness 仍为空；不把路径、偏移或回调塞进 witness。
- 不修改 runtime 函数的调用 API、finalizer 调用 ABI，不新增运行时类型搜索、缓存或路径解释器。

FengTypeDescriptor、FengAggregateDescriptor、FengFunctionDescriptor 的结构不在上述不变项内：
三者各增加一个可选投影表指针，属于显式的私有描述符 ABI 变更，不能称为“runtime ABI 不变”。

以上是设计约束，需通过实现与测试证明，不是声称当前实现已满足全部目标。

## 3. Semantic：一次确定规则，后端只消费事实

### 3.1 准入证明与匹配路径分开

ISSUE-G24-001 负责实际类型是否满足约束；ISSUE-G24-004 负责合法值如何匹配和取值。
二者复用联合主规范已有的类型关系、精确匹配优先级、合法进入路径及歧义判定，不能各自猜测。

设计必须处理普通联合规则允许的具体类型、子级联合、完整联合及间接叶子；不为简化
Codegen 排除其中一类。也不能把普通联合的字面量贴合直接等同于任意类型转换：
泛型准入证明消费类型关系，字面量仍在原有目标类型与参数推导位置处理。
不同 union 的任意结构兼容、泛型 variance 不由本文新增。

### 3.2 声明期使用事实

为共享体中的匹配建立编译期 sidecar，记录：

- subject 的声明类型表达式、约束 union 的完整类型表达式。
- 标签的完整类型路径、member 子集、绑定种类及绑定结果类型。
- 判别与绑定用途，以及关联的源码位置。
- 投影依赖的 owner、当前 callable／owner 泛参映射，以及构造绑定结果需要的既有类型依赖。

语句、值表达式、所有分支终止的表达式、infix 入口使用同一事实结构。
类型、成员和可写性错误在 Semantic 拒绝；Codegen 不通过尝试发码重新决定合法性。

### 3.3 闭合期投影

在实际类型已知的编译期实例化点，为使用事实生成闭合投影：

1. 确定从约束 union 到实际 T 的静态进入路径及必要的叶子转换。
2. 对每条匹配路径，区分静态不匹配、静态已匹配及仍需读取真实 tag 的层。
3. 计算真实 tag／payload 相对于实际 subject 存储的偏移。
4. 判断绑定结果的完整表示是否已经存在；不存在时生成该结果的构造入口。

路径应复用 ISSUE-G24-002 修复后的通用可变长表示；不得再引入固定层数限制。
约束含泛参时在完整闭合类型上计算索引和布局，不复用另一实例的归一化索引。
事实的完整类型身份不能用值分类、类型名字符串或描述符地址碰巧相同代替。

## 4. 完整嵌套场景的处理

以下示例均以准入检查已按普通联合规则证明唯一合法路径为前提：

~~~feng
spec Inner: i32 | string;
spec Middle: Inner | bool;
spec Outer: Middle | f64;
~~~

对 Outer 视角的 Middle -> Inner -> i32 路径：

### 4.1 实际 T 为 i32

Outer -> Middle -> Inner -> i32 的成员选择全部静态确定。
参数只有 i32 存储；判别不得读取 union tag。末端绑定可直接定位到 subject 存储。

### 4.2 实际 T 为 Inner

Outer -> Middle -> Inner 静态确定；Inner 本身的 active member 仍由当前参数值决定。
只读取真实 Inner 的 tag，命中后取得 i32 payload。两个同为 Inner 的参数可以命中不同叶子，
不能在 T 的元信息中缓存某个值的 active member。

### 4.3 实际 T 为 Middle 或 Outer

从参数本身开始，按普通联合规则读取剩余真实层级。不同前缀、兄弟路径和 fallback
仍按原有匹配顺序判定。不得在父层尚未命中时读取其内层 payload。

### 4.4 绑定停在虚拟中间层

实际 T 为 i32，但分支绑定为 Inner 时，原存储没有 Inner 的完整表示。
必须构造正常的 Inner 绑定，然后才能继续 match、赋值、返回、存储或捕获。

这不是把原参数改为 Inner：原参数和 return value 仍按实际 T 操作。
直接使用 -> 到叶子时，不需要为没有被绑定的中间层构造临时联合值。

## 5. 投影数据与共享体发码

### 5.1 最小逻辑结构

以下是本次确认的逻辑字段；实际布局与私有 ABI 迁移须按 §7.5 验收。
投影类型是 generated C 与描述符之间的私有静态元信息，不是 Feng 用户类型。
公共结构声明供共享体和闭合生成方一致使用；runtime 的 ARC／aggregate walker 不解释这些表项。

~~~c
/* One layer of a source-written match path. Offsets address the real subject. */
typedef struct FengUnionTagProbe {
    bool required;
    size_t source_offset;
    uint32_t expected_tag;
} FengUnionTagProbe;

/* Initialize a fresh result from a borrowed subject; never default-init first. */
typedef void (*FengUnionMaterializeFn)(const void *source, void *result);

/* Closed facts for one predicate / binding projection. */
typedef struct FengUnionProjection {
    bool possible;
    const FengUnionTagProbe *probes;
    size_t result_offset;
    FengUnionMaterializeFn materialize;
} FengUnionProjection;
~~~

- possible 为 false 时不读 subject。
- probes 的逻辑长度来自声明期标签路径，生成 C 按已知长度展开；静态层 required 为 false。
- 每个 required 层只在此前各层成立后读取 source_offset 处的 tag。
- 在已命中的有绑定路径上，materialize 为空时，完整绑定值位于 source + result_offset。
- materialize 非空时，只对已命中的有绑定分支调用，构造该绑定的完整正常值。
- 无绑定匹配不需要构造结果。静态 false 投影的偏移和构造入口不可被消费。

表只收集该依赖 owner 的实际使用。槽位先按开放使用身份确定，闭合后保持原槽位，具体规则见
§7.3；不得因某个实例的路径静态不匹配或类型去重而压缩、重排。大小、生命周期、默认零值
仍来自实际 T 或绑定结果类型的现有描述符，不在投影表中重复保存。

### 5.2 不使用运行时路径解释器

Codegen 按源码中的有限路径直接生成短路判断，不在运行时遍历类型图或递归解释投影节点。
投影仅提供常量判定、是否需要读 tag、偏移和期望索引；未知的真实 active member 仍必须判别。

普通非共享 union 直接使用既有确定表达式，不先绕到上述间接数据通路。
共享体的多个分支可以复用同一次 subject 求值及已安全读取的公共前缀，但不得改变分支顺序，
提前读取未命中的 payload，或跨不同值／不同求值缓存 tag。

### 5.3 四个匹配入口共用后端

将“subject 的匹配视角、条件构造、绑定取值”收敛为 Codegen 内部公共接口。
普通 union 与受约束 T 提供不同的数据来源，其余分支控制流、结果类型、return／throw、
作用域及 cleanup 继续使用现有实现。

这不是重构整个表达式系统；仅提取补齐此次匹配所必需的共同部分。
不能只修复 cg_emit_match_expr，漏掉语句、infix 或全终止分支路径。

## 6. 绑定构造、值语义与生命周期

### 6.1 真实结果直接复用普通绑定处理

subject 先按原有规则求值一次，取得生命周期稳定的存储。
直接投影绑定接入普通 member 绑定逻辑，不额外创造一种可逃逸的“视图值”。

保持值副本独立、引用身份、let／var 可写性及作用域。
不能因为取得了一个地址，就把原本的值绑定改成指向调用者原变量的可写引用。

### 6.2 原存储缺少结果时，仅构造该绑定

为虚拟中间 union、需要普通 spec 转换的末端等情况，编译器在闭合点生成专用 C 构造入口。
入口从借用 subject 直接初始化全新的目标存储，复用既有 union 路径写入、spec 转换和值操作。
不调用目标默认零值，不先建立首成员再覆盖，不创建无关的最外层 union。

目标存储的大小、对齐与 cleanup 以绑定结果类型为准，复用既有具体化依赖与存储规则。
例如从 i32 构造 Inner 时，不能按原始 T 的大小分配 Inner；含泛参的结果也必须取得其闭合描述符，
不能按开放类型的 C 占位布局分配。构造完成后才登记该绑定的正常清理责任。

优先在绑定形成点获得正常值，随后所有返回、存储和捕获走既有机制。
不为此次修复新增跨作用域懒物化、逃逸视图、运行时自修改或第二套 ARC。

闭合类型在当前发码位置已确定时，可直接展开相同构造操作。
共享体无法静态展开的闭合转换，通过投影中的构造入口完成；这是一次明确的新增间接调用，
必须单独验收成本，不能称为原有分派或零开销。不得给直接投影也增加该调用。

### 6.3 spec 末端与子集

- 真实 union payload 已含 object-form spec 时，使用该值自己的 subject／witness，
  不能替换成按 T 共用的固定 witness。
- 从具体 T 构造 spec 结果时，使用编译期选定的已有转换和生命周期策略；不得动态查找 fit。
- callable member 保留完整 callable 值及 invoke 语义，不把它当普通对象方法表。
- 多 member 子集、else 的静态视角及继续收窄遵循普通联合规则。需要实体值时使用正常承载，
  不能把裸 leaf 地址强当完整子集联合值；相关投影／构造事实同样按完整类型闭合。
- 成员 form 的合法性仍按 spec 主规范；本方案不顺带开放 intersection-form 作为 union member。

## 7. 元信息通道：描述符的 reified_union_projections

### 7.1 三种上下文描述符各增加一个字段

FengTypeDescriptor、FengAggregateDescriptor、FengFunctionDescriptor 各增加同名字段：

~~~c
/* Closed union projections owned by this reification context; NULL if unused. */
const FengUnionProjection *reified_union_projections;
~~~

字段指向编译期生成的 static const 表，表项结构见 §5.1。没有直接投影依赖时为 NULL；
只有 callee 需要投影、不在本体读取投影的 caller 也可以保持 NULL。长度和槽位在编译期／
符号记录中验证，运行时不新增 count 字段或搜索接口，也不为没有相关使用的共享体增加空指针判断。

只扩展上述三种上下文描述符。FengGenericParamDescriptor 的实际 T 描述及 witness 不变；
FengTrivialDescriptor 不变。不把整个方法级泛参数组迁入 FengFunctionDescriptor，
不为投影增加独立隐藏参数，也不把表塞入 witness 或其他现有依赖数组。

### 7.2 按投影依赖归属选择描述符

复用 FengReifiableDepSet 的 owner 划分和现有 TYPE／FUNCTION 依赖来源：

- 类型字段初始化、构造、终结器及相应静态初始化 helper 的 owner 依赖，使用类型描述符。
  引用类型为 FengTypeDescriptor，值类型为 FengAggregateDescriptor。
- 顶层函数、实例／静态方法和 fit 方法的 callable 依赖，使用相应 FengFunctionDescriptor。
- 不能根据投影中 T 的声明位置选择来源。例如 Box<T>.read<U> 中，构造阶段使用 T 的投影
  归类型；read 方法中使用 T 或 U 的投影均归 read，不把方法依赖并入 Box 的 owner 表。

生成 C 按已选定的来源和固定槽位读取，例如：

~~~c
/* Type-owned and callable-owned reads use separate, statically assigned slots. */
const FengUnionProjection *type_projection =
    &_td->reified_union_projections[type_slot];
const FengUnionProjection *method_projection =
    &_desc->reified_union_projections[method_slot];
~~~

上例的 type_slot／method_slot 表示发码时替换的整数常量，不是运行时决议的索引。
不要求一个共享体同时生成两次读取；只读取该使用点所属的来源。

### 7.3 归属内稳定排序与闭合

在现有具体化依赖收集、导出、恢复链路中增加投影依赖这一类别，不建立第二套依赖图：

1. 在声明期按 owner 收集开放使用事实。身份包含 subject／约束的完整类型表达式、标签路径、
   绑定用途和结果类型；泛参引用区分 owner 与方法作用域及其槽位，不仅比较参数名。
2. 在同一 owner 内按规范化身份去重并稳定排序，分配投影类别自己的槽位。
   不使用文件遍历顺序、调用点发现顺序或源码行号，也不与 aggregate／type／callable 数组混用索引。
3. 最终具体化点替换实际类型、计算真实布局和判别信息，按上述槽位生成静态表。
   某条路径静态不可能、两个开放类型闭合后相同、或闭合 union 成员发生归一化去重，
   都不能删除或重排已有槽位；只更新该槽位对应的闭合事实。
4. producer 与真实 .ft consumer 使用同一 owner 身份、开放类型映射和排序规则。
   同一投影的只读 probe 数据／闭合构造代码可去重，但不能据此改变共享体约定的槽位。

类型、方法的实际参数在闭合时共同参与替换；依赖归属不等于限制只能引用该层声明的泛参。
例如方法依赖可以同时引用 owner 的 T 和方法的 U，整个替换结果仍由方法描述符承载。

### 7.4 开放转传、方法值与回调

- callee 身份、owner／方法实参替换及递归边继续使用现有 reified_callable_deps。
  被调用函数在自己的描述符中持有自己的投影表，caller 不额外保存一条 callee 投影上下文链。
- 依赖判断须把“callee 只有投影依赖”也计入已有 callable 描述符需求，不能因没有其他类型依赖
  就漏掉该槽。开放调用只转传现有闭合描述符，不在运行时计算路径或组装投影表。
- 递归调用与循环依赖复用已有静态描述符节点预声明／去重方式，不为每次调用复制元信息。
- finalizer／方法值的闭合 wrapper 继续绑定现有类型／函数描述符，再调用共享体；
  finalizer 和普通 callable 的调用 ABI 不增加参数。
- 闭包如需保留相关具体化信息，复用现有描述符捕获机制，不另加投影指针；不能在调用结束后
  继续使用栈上临时描述符。若原通路尚未捕获所需描述符，应单列必要捕获的体积与成本，见 §8.2。

本节不授权重做全部 wrapper 或描述符生成器，只补齐投影沿既有通道的承载。

### 7.5 真实跨包与 ABI／格式边界

consumer 没有 provider 函数体，不能从泛型签名猜测其中使用了哪些匹配路径。
符号侧及 .ft 因此需要记录投影依赖的 owner、开放类型表达式、路径、绑定用途与所需类型依赖，
同时保留恢复稳定槽位所需的信息。不导出运行时地址、原始 C 偏移，也不扩大成员的源级可见性。

consumer 在最终闭合点生成自己的静态投影表，经现有类型／函数描述符传入共享体。
格式读取阶段验证条目引用、数量、路径和 owner；缺失或不支持的必需信息必须被明确拒绝，
不能把错误槽位留到生成 C 或运行时才发现。

三种描述符增加字段改变其 sizeof 和私有二进制契约；原有调用参数列表不变并不意味着 ABI 兼容。
需要明确版本／能力识别与编译器、runtime、provider、consumer 相关制品的重建范围。
目前 .ft 头部读取检查 major，但没有拒绝更高 minor，因此不能仅递增 minor 就宣称旧编译器会
拒绝新契约；这是本方案的迁移风险，不把现有兼容策略本身记为已证实的产品缺陷。
精确迁移方案须在改变相关 ABI／格式前 Review；本次文档更新不擅自指定版本号或修改格式规范。

## 8. 成本预算与不采用的替代方案

### 8.1 不增加的操作与保持不变的值表示

- 实际 T 的参数、局部、字段、数组、返回及默认初始化。
- 不需要投影的既有共享体不增加投影读取、空表判断或独立投影参数。
- 普通具体 union 的匹配快路径。
- 不增加入参统一包装、动态约束检查、运行时类型搜索、堆上投影表或逐层函数分派。

上述不变项不能代替描述符结构变大的成本核验，尤其不能预先保证所有既有调用路径总成本不变。

### 8.2 必须明确计入的增量

- 三种上下文描述符各增加一个指针字段；核对目标 ABI 下的 sizeof 和制品体积。
  不是每个用户对象增加一个指针，但已有自动生命周期／复合字面量描述符也可能受结构大小影响，
  必须检查其栈空间和初始化发码，不能只核对静态数据区。
- 匹配时从已有描述符读取投影表及表项，以及跳过静态层所需的判断；可在同一稳定上下文中复用
  已取得的表指针，不把这种复用当作无需验证的零读取保证。
- 缺少完整绑定表示时的目标存储、tag／payload 写入及既有必要值操作。
- 不能直接展开的绑定构造调用其闭合入口一次。
- 投影静态数据、闭合构造代码，以及原通路未捕获所需描述符时必要捕获增加的体积。

字段承载方向已经确认，具体成本仍须按 G24 §28.1.3 完成门槛确认和实际验证，
不能用“静态生成”或“此前无法编译”豁免；超过本方案已列明边界的成本必须再次交人工决策。
同包与真实二进制跨包分别核对生成 C；不以同包内联偶然消除操作当作保证。
数据量按去重后的实际使用路径计，不与所有 union 类型、全部成员和全部类型实参做无条件笛卡尔积。

### 8.3 为什么选择这一折中

- 不把 T 改成约束 union：避免改变默认值、返回类型与容器含义。
- 不统一包装入参：不让无匹配调用或直接叶子读取承担整条联合构造。
- 不只支持根层：完整处理间接叶子、子级 union 与中间层绑定。
- 描述符只增加可选投影表入口，不扩展成枚举全部类型、成员和路径的通用反射表。
- 不增加投影专用隐藏参数或并行上下文链，复用既有描述符传递与 callable 依赖。
- 不用描述符前缀强转／尾部外挂假装原结构未变，也不复用 witness 或无关依赖槽。
- 不对整个泛型函数按 T 复制主体：仅闭合必要的数据和绑定构造入口。
- 不承诺这是未经测量的全局最优方案；如果描述符读取／构造入口成本不能接受，应重新 Review，
  不能私自回退为统一包装、减少支持范围或大规模专用化。

## 9. 实际变更范围

### 9.1 文档与编译期语义信息

主规范只补确有遗漏的已批准规则；本设计不复制类型推导、绑定和 ARC 定义。
analyzer、semantic.h、联合路径信息及 reifiable_deps 增加使用事实、闭合校验与依赖传播。
ISSUE-G24-001／002 的通用修复与此配合，ISSUE-G24-003 仍独立修复并验收。

### 9.2 Codegen

匹配视角与投影读取、四个匹配入口、正常结果绑定／构造、静态投影表及描述符初始化。
依赖归属和槽位复用现有机制；补齐只有投影依赖的 callable、owner helper 与闭包的既有描述符通路。
复用既有联合 header／path store、值复制和 cleanup。
只有发现必须共享的重复逻辑才作局部提取，不安排无关的大范围文件拆分。

### 9.3 Symbol 与 .ft

符号使用事实、导出、恢复、格式能力识别，以及依赖排序／所有权释放。
已知主要入口是 symbol/export.c、imported_module.c、内部视图与 ft 读写；
具体字段和版本按 §7.5 Review，不能仅改 writer 而漏掉独立 consumer。

### 9.4 runtime 私有结构与不变边界

runtime 头文件中补充投影元信息声明，三种上下文描述符各增加 §7.1 的一个字段；同步生产／
初始化这些描述符的路径，按 §7.5 处理制品契约。这是本次明确列出的结构变更，不能遗漏 runtime
内建静态描述符或自动生命周期初始化点。

不计划改动 runtime 的 ARC／aggregate walker 等算法、FengGenericParamDescriptor、
FengTrivialDescriptor、Lexer／Parser、T 的默认零值机制、普通类型实例布局、既有 spec
转接层及不相关标准库实现。若测试证明必须触及，先记录原因并重新批准。

## 10. 实施步骤与验收

### 10.1 步骤一：Review 并验证成本原型

- [ ] 按 G24 §28.1.3 确认准入边界、描述符／.ft 迁移与 §8.2 的成本门槛；不重复决定已确认的
  字段方向、依赖归属和槽位原则，未解决前不将 G24 标为整体无阻塞。
- [ ] 准备裸 i32、Inner、Middle、Outer 同一共享体的完整路径原型，配对普通联合程序。
- [ ] 原型必须包含虚拟 Inner 绑定返回／存储及含泛参的绑定结果，不能只做直接 leaf。
- [ ] 核验直接投影不包装，不匹配路径不读 payload，不需要投影的身份函数不增加投影读取，
  并对照既有静态／临时描述符的大小及初始化，排查间接引入的栈空间和写入成本。
- [ ] 核验跨包通路；若成本或通道不成立，先更新方案，不扩大隐式批准范围。

### 10.2 步骤二：Semantic 与符号事实

- [ ] 完成类型级准入证明、完整路径及声明使用事实，非法程序在 Semantic 拒绝。
- [ ] 用真实 .ft 往返验证 owner 归属、开放身份排序、路径、绑定用途、callee 替换与槽位稳定；
  闭合后静态 false、类型相同和 union 成员去重均不改变已分配槽位。
- [ ] 覆盖不同文件顺序、同名不同实例、错误／缺失元信息及不支持的契约版本。

### 10.3 步骤三：四个发码入口与完整绑定

- [ ] 单层、逐级、->、多 member 子集、else 和 infix 全部接入公共视角。
- [ ] 正确处理直接结果、虚拟中间层、spec／callable 末端及必要构造。
- [ ] 无绑定、let／var、重新赋值、返回、字段／数组存储、捕获与 throw／return 清理有证据。
- [ ] 开放转传、owner／方法泛参、构造／静态初始化 helper、finalizer 与方法值通路完整。

### 10.4 步骤四：行为、成本及全量交付

- [ ] FCTS 覆盖具体／子级／完整 union、所有实际 active member、正向结果及失败分支；
  同一实际 T 的两个值必须能命中不同分支。
- [ ] 测试默认零值仍按实际 T；显式初值不请求 T 或约束 union 的默认零值。
- [ ] 含受管字段的值、引用身份、绑定副本独立及离开作用域后的存活／终结次数符合原规则。
- [ ] 深度 8／9 及更长可变路径不截断，匹配顺序和单次求值不因共享而变化。
- [ ] test/ 验证三种描述符的空／非空字段、依赖归属、四个发码入口、投影表项消费、闭合构造
  与真实 .ft 恢复；确认没有增加投影隐藏参数或迁移方法级泛参。
- [ ] 按 G24 COMPOSITE24、31、32、34、36～39 建立具体证据映射；既有测试不擅自修改。
- [ ] 专项通过后，在沙箱外执行完整 make test；任何发现的问题先记录、再分析、再修复，
  不确定语义、现有测试迁移或新增成本再次交人工决策。

## 11. Review 结论记录

- 语言目标：完整联合收窄、实际 T 类型与默认值保持，已确认。
- 承载方向：三种上下文描述符增加 reified_union_projections，按投影依赖归属和稳定槽位读取，
  已确认；不新增独立隐藏参数，不改变 FengGenericParamDescriptor，不迁移方法级泛参。
- 整体开工门槛：COMPOSITE23／27 的未明准入关系、私有描述符 ABI／.ft 迁移、§8.2 具体成本
  接受边界仍须完成，详见 G24 §28.1.3；不能把方向确认记成 ABI 兼容或性能已经通过。
- 原型与性能结果：未执行。
- 产品实现、既有测试修改与全量回归：未执行。
- ISSUE-G24-001／002／003：修复要求仍全部保留，不被本方案替代或延期。
