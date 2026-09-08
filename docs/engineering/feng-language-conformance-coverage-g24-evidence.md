# G24 用例与验收证据

## 1 范围与阅读方式

本文只记录 [G24 实施计划](./feng-language-conformance-coverage-hardening-pending.md#28-g24复合类型诊断)
的测试映射，不另行定义语言规则。逐项范围以计划 COMPOSITE01～42 为准；语义来源为
[联合类型](../specifications/feng-union-type.md)、[spec](../specifications/feng-spec.md)、
[泛型](../specifications/feng-generics-draft.md)、[可见性](../specifications/feng-visibility.md)和
[符号表](../specifications/feng-symbol-table.md)主规范。

下列“复用”表示保留既有直接断言，不修改原用例；“新增”表示本组独立测试。
Codegen 用例还会编译生成 C，FCTS 执行语言行为断言，二者不能互相替代。
最终全量结果见第 4 节；未取得最终结果前，不把此映射当作 G24 已验收。

当前状态：本次实现的全量回归通过，但 COMPOSITE27 的交叉约束开放转传仍有
ISSUE-G24-051，相关行为／发码／成本格尚未验收，G24 不标记完成。

## 2 COMPOSITE01～42 对应证据

### COMPOSITE01：非法 union 声明

新增 [Parser](../../test/parser/test_g24.c) 的 `void`／缺项邻界和
[Semantic surface](../../test/semantic/test_g24_surfaces.c) 的未知名称、intersection member。
检查完整诊断集合，包括非法 intersection member 同时产生的 `AE0622`／`AE0603`；不构造
Parser 本来无法产生的 AST。合法声明由同文件正例及 FCTS 的 union 组配对。

### COMPOSITE02：非法 intersection 组成项

新增 Semantic surface 的内建类型、数组、普通 type、callable、union、未知名反例；
复用 [Semantic 主套件](../../test/semantic/test_semantic.c) 的
`test_intersection_spec_accepts_object_form_members`、`test_intersection_spec_accepts_nested_intersection_members`。
缺项语法与已解析 form 分别在 Parser 和 Semantic 验证。

### COMPOSITE03：union 的六类进入点

新增 `surface_entry_pairs` 对初始化、赋值、参数、返回、字段、数组逐项改变 source 类型，
核对非法进入与合法邻界；[composite surfaces FCTS](../../fcts/fcts_bin/src/test_g24_composite_surfaces.ff)
运行对应六个入口并检查 payload，嵌套／字面量选择另由 COMPOSITE10～12 验证。

### COMPOSITE04：intersection 的六类进入点

同一 `surface_entry_pairs` 区分缺一项、全部缺失、同形状无关系，逐入口断言，
不混入不可写等其他根因。composite surfaces FCTS 检查全部要求的调用结果；
[spec views](../../fcts/fcts_bin/src/test_g24_spec_views.ff) 补共享值／引用来源和普通 object 目标。

### COMPOSITE05：未收窄与错误成员视角

新增 Semantic surface 分开覆盖 union 字段读写、调用、方法值、相等与不等，以及
intersection 不存在成员、实例访问静态成员、错误的 union 式 match。
`surface_view_boundaries` 区分原变量、子集绑定与二次显式绑定；FCTS 用合法成员视角配对。

### COMPOSITE06：基础行为与表示类别

新增 [union bindings](../../fcts/fcts_bin/src/test_g24_union_bindings.ff)、
[binding edges](../../fcts/fcts_bin/src/test_g24_union_binding_edges.ff)、spec views，
覆盖标量、string、值、引用、具名 tuple、enum、数组及 object／callable 叶子。
复用 [union](../../fcts/fcts_bin/src/test_union.ff)、[intersection](../../fcts/fcts_bin/src/test_intersection.ff)
及其 generic／method-value 专项；交叉场景只使用具备合法名义关系的 subject。

### COMPOSITE07：声明语法与 AST

新增 Parser 的 11 类非法源码及合法具名 form AST；补分组 infix match 与 typed／空参数
lambda 的独立 AST 对照。Semantic surface 补 type／fit／object 父列表的非法 union 使用，
交叉关系入口复用 G23 的既有拒绝用例；不以语法失败替代满足检查。

### COMPOSITE08：规范化与顺序

复用 `test_union_form_spec_records_normalized_members`、
`test_union_spec_ft_roundtrip_preserves_normalized_members`；新增
[binding FT](../../test/symbol/test_g24_bindings.c) 同时验证短名／全名重复、`int` 与宿主规范标量
去重、`Choice<string>`／`Choice<i32>` 身份和嵌套保留，释放 provider AST 后检查真实 FT。
[composite actuals FCTS](../../fcts/fcts_bin/src/test_g24_composite_actuals.ff) 检查首个嵌套默认值、
另一闭合实例的有效 payload；binding owners 补 `Choice<bool>` 的闭合重复 member。

### COMPOSITE09：组合图与循环

新增 [graph matrices](../../test/semantic/test_g24_graphs.c) 覆盖两种 form、跨模块循环与无环共享图，
交换输入顺序；Semantic surface 覆盖直接自环。循环指复合声明图，普通 type 字段的递归默认值
继续由 G18／G21 的实例点检查，不改变其时机。

### COMPOSITE10：完整进入路径

新增 [Semantic G24](../../test/semantic/test_g24.c) 的 `g24_complete_union_paths` 检查 1／8／9／17
段完整索引；[union paths FCTS](../../fcts/fcts_bin/src/test_g24_union_paths.ff) 和 binding edges
读取非零 payload。精确内层值、叶子进入和实际嵌套 tag 由 union order／bindings 配套验证。

### COMPOSITE11：两轮首路径选择

新增 `g24_union_ordered_paths`、[union order FCTS](../../fcts/fcts_bin/src/test_g24_union_order.ff)
及 [真实 FT 选路](../../test/symbol/test_g24.c) 的 `g24_entry_order_roundtrip`。
对照深层精确／浅层满足、同层左右、交换顺序、显式中间值和无资格反例；
Codegen 的 `g24_union_entry_identity` 验证 nominal spec 身份未合并。

### COMPOSITE12：字面量贴合与入口一致性

新增 [选路 Semantic 矩阵](../../test/semantic/test_union_entry_order.h)、union order FCTS 和 FT
选路用例，覆盖六类入口及分支结果、直接／嵌套、整数／浮点、越界后续候选、全失败、纯常量
表达式及重载候选回滚。复用 if／match／try 的既有目标贴合矩阵，不更改其整体推导规则。

### COMPOSITE13：单成员绑定

新增 [binding Semantic](../../test/semantic/test_g24_bindings.c)、Semantic surface 与 union bindings／
binding edges FCTS。默认／显式 let、var、无绑定、重新赋值、引用身份、值独立与捕获均有断言；
头部／body／分支外作用域复用 G13，原 union 的成员访问失败有独立反例。

### COMPOSITE14：子集与 else

新增 `surface_view_boundaries` 同时检查普通／共享、多 member 子集及 else 剩余一个／多个
member。拒绝对原 union 或多项子集直接访问；二次绑定合法。binding edges FCTS 运行多项分支
及继续收窄。ISSUE-G24-038 明确 else 不创建绑定、不改变原变量静态类型。

### COMPOSITE15：标签与 active member

新增 Semantic surface 检查非 member、未知名、值／区间、同分支及后续分支重复标签；
binding Semantic 补四类匹配入口。union bindings 的未激活父层及同类型备用 payload 必须
进入 fallback，不能凭底层字节或重新转换误命中；普通 union 组提供独立对照。

### COMPOSITE16：逐级与链式路径

新增 binding Semantic 的首／中／末段、越级、兄弟及 spec 下探反例；union bindings／edges
逐级与链式结果配对，包含共享前缀下的不同叶子、无绑定、表达式和 infix。
8／9 段路径同时在 Semantic、Codegen、FT 和 FCTS 验证，不使用固定容量截断。

### COMPOSITE17：infix 的求值与绑定范围

新增 surface 和 binding Semantic 的 `||`／`!`／else／语句后反例，复用已有 if／while／`&&`
绑定域正例；binding edges FCTS 用计数器断言 while、布尔值、短路右侧的目标求值次数及命中。
Parser 的分组与 lambda AST 对照防止将合法 match 误分流。

### COMPOSITE18：spec 与 callable 叶子

新增 union bindings 的引用、值、既有 spec 视角配对，binding edges 的 callable 转换和调用；
Codegen 检查叶子形成路径。非法未收窄调用、spec 下探和共同视角转换由 binding Semantic／
surface 拒绝；复用 [spec leaf coercion](../../fcts/fcts_bin/src/test_union_spec_leaf_coercion.ff)。

### COMPOSITE19：交叉展平、去重与重载

复用 Semantic 的 `test_intersection_spec_flattens_multi_layer`、`test_intersection_spec_method_dedup`、
返回类型冲突及合法重载；新增字段与完整泛型实例矩阵。
[实例方法值](../../fcts/fcts_bin/src/test_intersection_spec_method_value.ff)、
[泛型方法值](../../fcts/fcts_bin/src/test_intersection_generic_method_value.ff)、
[静态方法值](../../fcts/fcts_bin/src/test_intersection_static_method_value.ff) 运行父级、菱形与精确重载。

### COMPOSITE20：字段及静态成员

新增 `g24_intersection_fields` 的实例／静态、let／var、同／异类型、直接／继承组合，
完整 owner 替换后检查 `AE0623`；spec views／binding edges 验证值 box 的可变字段。
复用 [静态调用 FCTS](../../fcts/fcts_bin/src/test_intersection_static_method_call.ff) 与对应 Semantic
surface；字段和方法的 requirement 身份不按名字简化。

### COMPOSITE21：直接调用与方法值

复用上述三个方法值 FCTS 及 Semantic 的 `test_intersection_spec_method_values_record_merged_surface`、
`test_constrained_generic_intersection_method_values_record_surface`、静态 record_surface 与
各自 invalid_sources 矩阵。新增 composite actuals 将完整 spec T 用于直接、函数值、实例／静态
方法及方法值；[Codegen views](../../test/codegen/test_g24_spec_views.c) 编译全部七类入口。

### COMPOSITE22：访问权限

复用 intersection 实例／泛型／静态方法值的 authorized_seal 与 invalid_sources，以及静态
调用 diagnostics；binding FT 独立拒绝 consumer 访问 seal requirement。
对应 FCTS 在合法实现域运行，不把 `.ft` 中保留的私有表示／实现依赖当成源码公开权限。

### COMPOSITE23：联合约束准入

新增 `g24_union_constraints` 与选路矩阵，普通绑定和约束调用配对，区分 member、内层 union、
整体、间接叶子及 spec 满足路径；错误实例／无路径在 Semantic 失败。
union projections／bindings FCTS 显式与推导使用同一共享函数，不只传完整约束 carrier。

### COMPOSITE24：联合约束体能力

新增 Semantic surface 对开放 T 未收窄读写／调用／方法值／比较分别报错；
union bindings／edges 在成功绑定后使用正确 member 并返回原 T，未命中有明确结果。
合法收窄不扩张声明体内未收窄能力。

### COMPOSITE25：交叉约束准入

新增 `g24_intersection_constraints`、`g24_intersection_view_arguments`、
`g24_descriptor_admission_pairs`，区分完整关系、缺项、同形状、另一实例及独立泛参。
[G24 intersection FCTS](../../fcts/fcts_bin/src/test_g24_intersection.ff) 和 composite actuals
验证真实值与引用来源；原获批非法正例仅按 ISSUE-G24-008 的明列范围迁移。

### COMPOSITE26：交叉约束体能力

复用泛型 intersection 方法／静态方法直接调用与方法值专项；新增 spec views、binding edges
和 composite actuals，核对泛型成员、传递父、值／引用来源及各值自己的 witness。
非法额外成员、错误赋值与权限由 Semantic surface 和既有约束体 diagnostics 配对。

### COMPOSITE27：开放转传

新增 Semantic G24 的 union／intersection 约束与完整实例证明矩阵，包含相同／蕴含关系、
错误 member、错误闭合类型、owner／实例／静态使用；binding Semantic 记录转传后的投影。
union binding edges 和三包路径运行多层传递，描述符 control 另验证无 union 的既有转传。

未完成格：`T: Both` 向组成 object-spec `Right` 的共享函数转传。独立最小程序语义通过，
Codegen 报 `CE0293`；见 ISSUE-G24-051。现有 union 和相同约束转传证据不能代替该格，
需要先 Review 描述符承载及增量成本方案，再补正式运行和诊断控制。

### COMPOSITE28：复合 owner 与实参替换

新增两类约束的 type／spec owner 语义矩阵及错误实例反例；
[binding owners FCTS](../../fcts/fcts_bin/src/test_g24_union_binding_owners.ff) 使用类型与方法两套
泛参，composite actuals 同时使用 string／i32 具化。Codegen 的独立 Child 实参程序不预先
声明目标交叉值，验证推导调用也预登记闭合约束。

### COMPOSITE29：复制、身份与生命周期

新增 union bindings／edges、spec views、composite actuals 的字段／数组／参数／返回／
捕获与异常退出断言，覆盖 active member 切换、值副本、引用共享、受管 payload 存活。
终结计数夹具明确区分语言合法默认对象与指定 payload（ISSUE-G24-048），不把普通构造
阶段的默认对象误报为重复释放。

### COMPOSITE30：默认零值

新增 binding owners 的默认 callable／intersection／非 trivial 返回，composite actuals 的
首个嵌套默认 member；复用 G18／G21 的实际默认请求及显式非递归初值正反例。
本次非空 `[value]` 修复只恢复实际 T 的元素表示，不重定义默认零值或三阶段初始化。

### COMPOSITE31：真实跨包与拒绝边界

新增 [projection FT](../../test/symbol/test_g24.c)、binding FT、
[view FT](../../test/symbol/test_g24_spec_views.c)，释放 producer AST 后通过二进制查询恢复；
核对形状、数量、槽号、owner、完整类型及非法 consumer 诊断。FCTS 的 lib／middle／bin
分别构建，不能用同一源码联合分析代替。当前必需节缺失即失败，不兼容旧制品。

### COMPOSITE32：发码与承载

新增 [binding Codegen](../../test/codegen/test_g24_bindings.c) 编译四种共享匹配入口、虚拟／
现存中间值及失败路径；[descriptor Codegen](../../test/codegen/test_g24.c) 检查静态表、
投影读取和 spec 身份；view Codegen 检查普通目标／交叉目标、单次形成及 spec-actual 的
既有单层 slot adapter。所有检查同时编译生成 C，并由对应 FCTS 执行行为断言。

### COMPOSITE33：完整复合类型作为 T

新增 composite actuals 使用完整泛型 union／intersection 作为 identity、owner、字段、数组、
返回及转传的实际 T；descriptor FCTS 的非泛型完整 union／tuple／spec 作为对照。
ISSUE-G24-050 增加泛型非空字面量的标量、string、引用与受管值控制及逐元素求值计数。

### COMPOSITE34：联合约束下的实际表示

新增 union bindings 同一入口接收裸叶子、child、middle、整个 union，各自检查 active
路径、fallback 与返回 T；binding edges 补 tuple／enum／array、开放结果继续收窄及深路径。
错误实例／不相关实参由约束和 binding Semantic 矩阵拒绝。

### COMPOSITE35：spec 实参的交叉关系

新增 composite actuals 同时验证交叉自身、组成 object 约束、包含全部契约的 Child spec，
返回类型仍为原 T；同一 T 的值／引用视角正反交换顺序，断言各自 witness。
约束 owner、函数值和方法值配套运行；Semantic 的缺项、同形状、错误实例反例保持严格。
ISSUE-G24-049 只修已有 adapter 的槽选择和 ABI，不增加第二层。

### COMPOSITE36：增量成本

descriptor Codegen 对闭合函数／类型参数描述符检查 file-scope `static const` 复用、
不同完整身份与实际 callee 槽；view Codegen 的无转换及普通转换 control 不读取新字段。
union binding Codegen 配对无绑定、直接叶子与必要中间构造；FCTS 配对值和生命周期。
已批准成本、旧 spec 转接及本轮没有新增的操作分开记录于第 3 节。

### COMPOSITE37：共享体逐级与多级收窄

新增 binding Semantic／Codegen／FT／FCTS 共同覆盖四入口、直接／逐级／链式、中间绑定、
8／9 段、未命中与重叠前缀；实际 T 的四类表示不是仅替换输入值的 tag。
普通 union 用独立函数对照，infix 有副作用目标用计数验证，不把未进入断言块算通过。

### COMPOSITE38：投影归属与槽序

Semantic 的 `g24_projection_facts`／`g24_projection_paths` 与 FT 的 slot／owner_path
roundtrip，检查 owner／方法泛参域、完整路径、用途、去重与源顺序变换；
binding owners FCTS 分别运行引用 owner、值 owner、字段／构造／静态初始化／终结器。

### COMPOSITE39：表项生成与消费

binding Codegen 检查静态不可能、现存 storage、直接 leaf 和必要物化，普通进入事实负责
构造路径；binding FCTS 的虚拟中间值继续存储、返回、捕获、二次收窄与释放。
spec views 的附加表同样检查静态配对、完整 source／target 和闭合布局，不使用占位 box。

### COMPOSITE40：三层共享调用

新增 [middle 包](../../fcts/fcts_middle/src/g24.ff) 与 lib／bin 的独立构建，三层分别匹配；
同包与 fit 入口另设 FCTS control。descriptor Codegen 检查实际 callee slot，而非仅将外层
descriptor 原样传递；递归、闭包和正常／异常生命周期由 descriptor／binding edges 组配套。

### COMPOSITE41：非共享 union 不回归

普通 union／nested union／spec leaf coercion 既有 FCTS 原样运行；新增 union order 的
普通入口与 binding edges 的 ordinary all-exit／subset／capture 函数。
四类结果入口及非法原变量访问由普通模式的 Codegen／Semantic 独立验证，不仅复用共享体。

### COMPOSITE42：无 union 共享体不回归

新增 descriptor FCTS 的 17 个控制，覆盖零依赖、普通 spec、数组可写性、完整名义身份、
类型／方法／fit、多层／递归／逃逸及生命周期；view Codegen 的 identity／普通转换不读取
union 投影。既有 G18、G21、G23、intersection 及一般泛型全套参与最终回归。

## 3 成本与制品边界

- 三种上下文描述符的 `reified_union_projections` 和 `reified_spec_view_coercions` 字段及实际
  使用的静态表，属于已批准增量；既有自动描述符随字段增加的字节／初始化／栈布局也已获批准。
- 共享收窄读取自己的投影。仅缺少现存中间表示且实际需要绑定时调用一次静态构造入口；
  无绑定、未命中及直接链式叶子不添加该调用。
- 开放值形成 object／intersection 视角复用普通一次 box 或引用 subject；不新增转换回调、
  描述符搜索、运行时满足检查或隐藏参数。无转换路径不读取转换表。
- spec 作为 T 的既有 `FengSpecSlotWitness` 层保留，ISSUE-G24-006 消除优化明确延期。
  049 修复该层的既有调用协议，没有叠加适配层；050 复用已有泛型值复制与数组 API，
  修复指针误当完整值，不增加 runtime 入口。
- `.ft` 保持 2.0；缺少必需视角依赖节直接拒绝，不增加旧版本兼容、自动迁移或桥接。
  runtime、对象文件、`.ft`／`.fb`、provider／consumer 和缓存统一重建。
- 不能将以上结论概括成“完全零开销”；历史隔离成本结果与人工批准见 G24 问题记录
  ISSUE-G24-009／015／040／041。

## 4 执行记录

### 4.1 专项阶段

Codegen 七类 spec 实参入口及独立闭合约束预登记用例通过；真实跨包 FCTS 阶段 1270／1270
通过。随后补充 COMPOSITE08 的真实 FT 去重身份与默认嵌套运行控制，纳入下方最终回归。
Parser、Semantic、Symbol 的完整阶段结果以最终日志为准，不以单个探针代替全套。

### 4.2 最终全量

沙箱外执行 `make test > local/g24-delivery/make-test.log 2>&1`，清理重建全部制品。
退出码 0。UBSan 和常规两阶段均通过 Archive、Lexer、Parser、Semantic、Runtime、Codegen、
Debug、CLI、CLI paths、Symbol；std 均为 604／604，FCTS 均为 1271／1271。
smoke、CLI direct／project／init、性能约束、增量构建、发布／安装／回滚、macOS finalize、
bundled packages 和预构建工具链测试通过。`git diff --check` 通过。

该结果覆盖当前实现与已加入的用例；最终条款核对后，051 的独立编译探针确认尚有合法程序
无法发码，因此这不是 G24 整组完成结论。051 修复后仍须加入正式回归并重新全量验收。

### 4.3 问题记录

发现、分析、决策和具体修复见 [G24 问题记录](./feng-language-conformance-coverage-hardening-issues/g24.md)。
本次只修改新增用例和已明列获批的旧用例；不会按失败数量自动迁移其他既有断言。
