# Reified 元信息表 `_count` 补齐方案

## 1 状态与范围

日期：2026-09-09。状态：独立方案记录，待 Review，尚未实施。

本文汇总 G24 后关于三张 `reified_*` 元信息表补齐 `_count` 的讨论。本次只记录方案，不修改
runtime、Codegen、测试或现行 ABI；后续实施前须核对批准范围和实际成本。

目标是让已有元信息表具备一致的“数量 + 指针”描述，不增加语言能力、运行时检查或新的描述符
传递通道。不合并 `reified_spec_view_coercions` 重命名、spec 泛型转接层消除、数组描述符静态化
或 G25 泛型诊断用例补齐。

## 2 已确认的现状

[runtime 头文件](../../src/runtime/feng_runtime.h) 中，`FengTypeDescriptor`、
`FengAggregateDescriptor`、`FengFunctionDescriptor` 均已有以下三个字段，但没有对应数量字段：

- `reified_union_projections`；
- `reified_spec_view_coercions`；
- `reified_constraint_projection_descriptors`。

同一批描述符的 `reified_agg_deps`、`reified_type_deps`、`reified_callable_deps` 已有对应 `_count`。
当前三张表由 Codegen 按编译期固定槽位读取，不在运行时遍历，因此缺少 `_count` 本身不是已经
确认的语言行为错误，也不表示现有路径缺少编译期数量校验。

[Semantic 依赖集](../../src/semantic/semantic.h) 已记录 `union_projection_count`、
`spec_view_coercion_count`、`constraint_projection_count`；[符号表](../../src/symbol/internal.h)
及 `.ft` 也有相应数量与槽位校验。现行制品规则仅由
[符号表规范](../specifications/feng-symbol-table.md) 定义。

## 3 拟补齐的字段与含义

### 3.1 三种上下文描述符统一补齐

在 `FengTypeDescriptor`、`FengAggregateDescriptor`、`FengFunctionDescriptor` 中，分别在
对应表指针前增加一个 `size_t` 字段，共涉及三个结构体、九个新增字段：

```c
/* Number of owner-local union projection slots; zero when unused. */
size_t reified_union_projections_count;
const FengUnionProjection *reified_union_projections;

/* Number of owner-local spec view formation slots; zero when unused. */
size_t reified_spec_view_coercions_count;
const FengSpecCoercionDescriptor *reified_spec_view_coercions;

/* Number of owner-local constraint projection slots, not distinct records. */
size_t reified_constraint_projection_descriptors_count;
const struct FengGenericParamDescriptor *const *reified_constraint_projection_descriptors;
```

命名采用“现有指针字段名 + `_count`”，只有一个下划线，不使用 `__count`。指针字段、表项结构、
`FengGenericParamDescriptor` 与 `FengTrivialDescriptor` 均不因本项改变。

### 3.2 数量是当前依赖域的槽位数

- count 表示对应数组的条目数，不是字节数、容量、泛参数量或递归依赖总数。
- 三张表分别计数，类型初始化域与 callable 域也分别计数；沿用现有依赖归属与排序，
  不改成按某个 T 计数或重新分配 index。
- 闭合后两个槽位内容相同，或者约束投影槽位指向同一静态泛参描述符，仍分别占一个槽位；
  count 不能按去重后的描述符地址数计算。
- union 投影闭合后为 `possible = false`，该既有槽位仍保留并计入数量，不能据此压缩数组。
- 无本域直接依赖时为 `count = 0`、指针 `NULL`；非空表的 count 与实际数组长度一致。
  仅向内层共享体转传的 caller 不汇总 callee 的表，callee 继续使用自己的描述符与 count。

### 3.3 生成与读取方式保持不变

Codegen 使用生成对应表的同一份已验证依赖集填写 count；无表的初始化器保持零值，不建立第二套
数量推导或排序逻辑。现有编译期数组生成循环不变，新增字段的值在具化点确定。

共享体仍按固定 index 访问原表，不读取新 count，不增加边界检查、空表分支、运行时循环、
动态查询或新的隐式参数。count 不参与值复制、ARC、默认零值、union 收窄或约束满足判断。
数量字段只补齐描述信息，不以“有了 count”为理由增加新的运行时消费者。

## 4 成本、私有 ABI 与独立优化边界

### 4.1 静态记录的成本

每个受影响描述符增加三个 `size_t` 字段。在 `sizeof(size_t) = 8` 的目标上，新增字段共
24 字节；最终结构体大小和偏移按目标 ABI 核验，不能把本项写成总共只增加一份 24 字节。
空表的 count 虽然为零，字段仍占描述符空间；该空间不是按每个 Feng 对象实例增加。

对于原本文件级 `static const` 的描述符，count 作为常量随制品保存，调用点仍传原描述符指针，
不按值传递整份记录。因此方案不要求额外的逐次初始化、参数传递或 count 读取指令。
静态数据体积和缓存布局仍可能变化，不能进一步承诺所有程序耗时绝对不变。

### 4.2 现有临时描述符不能忽略

当前开放数组泛型实参路径仍会生成函数内的 `FengTypeDescriptor` 复合字面量。
即使新 count 全为零，结构体增大也可能增加自动记录的初始化字节或栈空间，不能将这一条路径
误称为纯静态空间成本，也不能复用 G24-015 旧探针数字作为本次 `_count` 的测量结果。

该静态化问题由[开放数组泛型实参描述符静态化优化](./feng-generic-array-descriptor-static-optimize.md)
独立跟踪。本项实施前核对该专项是否已完成，并检查仍存在的自动描述符初始化路径。
若补字段会增加执行成本或栈空间，应先解决相应静态化，或列出实际差异交人工决策；
不能把此前对其他字段扩展的批准当成本项成本批准，也不在补 count 时私自合并静态化实现。

### 4.3 制品与兼容边界

这是 runtime 私有 C 描述符布局的变更，需要统一重建 runtime、编译器生成代码、provider／consumer
及相关缓存制品，不能混用新旧布局。沿用此前决定，不增加旧制品兼容分支。

`.ft` 已有三类数量与槽位记录，本项只把现有数量写入生成的 C 描述符，不新增序列化字段，
也不改 `.ft` 版本。若实施发现超出此范围的必要变更，先记录原因并由人工决策。

## 5 分步实施与验收 TODO

以下步骤尚未开始；本文件不授权修改未明列并获批的既有测试，也不扩展运行时成本许可。

### 5.1 实施前核验与文档对齐

- [ ] 核对三个描述符的布局、所有初始化器和三张表的生成入口，保存静态及自动记录的基线。
- [ ] 确认第 4 节成本边界；若仍有自动记录受影响，先记录测量与人工决定。
- [ ] 实施获批后，先将相关设计文档中的当前字段描述对齐；尤其更新
  [联合投影设计](./feng-generic-union-match-draft.md) 的“不新增 count”表述，避免把新旧方案并列为现行规则。

### 5.2 字段与静态发码

- [ ] 三个描述符各补齐三个 count，并注释数量含义、零值规则及“不用于运行时检查”的边界。
- [ ] 三类表生成与描述符初始化使用一致的依赖数量；覆盖普通静态记录、默认／内建描述符
  及仍存在的自动初始化器，不增加运行时计算 count 的代码。
- [ ] 保持共享体读取、传递、槽位排序和现有表项复用逻辑不变；不附带重命名或无关重构。

### 5.3 编译器、跨包与行为验证

- [ ] `test/` 验证三种描述符上的三张表分别为零项、单项、多项时，count 与实际长度一致；
  覆盖无依赖、三表并存、闭合后相同记录复用及不可能 union 投影仍保留槽位。
- [ ] 验证类型级／方法级泛参、成员绑定初始化、构造／析构、fit 与多层共享调用的依赖归属；
  caller 仅转传时不错误汇总 callee 数量。
- [ ] 两种 `.ft` profile 真实往返后数量与槽位一致，consumer 不依赖 provider 源码；
  复用或补充既有数量不符、槽位越界等制品反例，不把校验移到生成程序运行时。
- [ ] `fcts/` 复用或新增同包／跨包联合收窄、spec 视角形成、约束转传与非相关共享体的行为
  对照，验证返回结果、所有权与闭包逃逸；不以 count 文本断言代替语言行为。
- [ ] 现有用例保持不变；如结构体大小或生成文本断言确需迁移，逐条列出后由人工批准。

### 5.4 独立回归与交付

- [ ] 比较生成 C 与独立调用边界的汇编：无新增 count 读取、检查、调用、ARC 或描述符逐次
  组装；记录实际静态体积、字段偏移与栈空间差异，不仅比较源码中的显式赋值。
- [ ] 在 Codex 沙箱外独立执行 `make test`，记录日志与退出状态；制品在工程目录内运行。
- [ ] 执行 `git diff --check`，填写实现、测试、成本和问题关闭结果，再更新交付状态。
- [ ] 建议实施 commit message：`feat: add counts to reified metadata tables`；不自动提交。

## 6 实施过程问题记录

当前尚未实施，无本专项新发现的 Bug。第 4.2 节记录的是已知临时描述符路径及待核验的成本影响。

实施中发现问题时，在本节先记录最小复现、实际与预期结果，再补分析依据、方案、是否影响既有
测试／运行时成本／ABI、人工决策与验证结果。不能将未经验证的风险写成已发生或已修复的问题。
