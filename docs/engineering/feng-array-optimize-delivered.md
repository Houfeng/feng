# Feng 数组运行时优化（待评审草案）

> 状态：第一步已完成（待 review）
> 目标：先收敛数组 runtime 的“真正尾部内联”方案与第一步实施范围，再进入实现与测试拆解。
> 说明：本文只覆盖第一步：把固定长度数组的数据区改为内联；不包含 list 迁移 API、批量迁移实现，也不包含后续容器优化。

## 1. 当前结论（拟采用方向）

本轮收敛的方向是：

1. `FengArray` 改为真正的尾部内联布局，不再保留 `items` 指针作为运行时存储来源。
2. `feng_array_data()` 改为按 `header + aligned offset` 计算 payload 起始地址；`length == 0` 时继续返回 `NULL`，保持现有可观察行为不变。
3. 数组语义不变：仍然是受管引用对象、创建后长度固定、赋值/传参/返回默认复制引用，不复制元素内容。
4. codegen 侧数组访问形状不变：继续通过 `feng_array_data()` / `feng_array_length()` / `feng_array_check_index()` 访问，不因为本次布局变化而重写调用点。
5. list 的内部迁移 API 不是本阶段内容；后续若继续做容器优化，基于新数组布局单独设计。

### 1.3 本阶段结构决策（最终口径）

为避免歧义，本阶段对数组数据区的结构决策固定为：

1. 采用“完全不占位”方案：`struct FengArray` 不引入 `items` 字段，也不引入 `item[]` / `data[]` 之类占位成员。
2. 不采用 `alignas(max_align_t)` 占位成员方案；对齐统一由内部 helper 通过 offset 计算保证。
3. 所有 payload 地址计算必须收敛到单一 helper，禁止在不同路径重复手写偏移计算。
4. 关键注释必须与实现保持一致：结构体注释、创建路径注释、`feng_array_data()` 注释、finalize 注释、collector 数组路径注释，都要明确“同一块 allocation 尾部内联”的语义与边界。

### 1.1 为什么本阶段不保留 `items` 指针

如果本阶段仍保留 `items` 指针，只把它改成“指向同一块 allocation 尾部数据区”，那么只能拿到：

- 普通数组少一次分配

但仍然拿不到：

- 访问元素时少一次通过 `items` 取地址的间接访问

当前数组读写发码仍然是：先经由 `feng_array_data(array)` 拿到数据地址，再做下标访问，见 `src/codegen/codegen.c` 约 9530-9555 行、10524-10708 行。若 `feng_array_data()` 继续只是返回 `arr->items`，那这次额外 load 仍然存在。

因此，若本轮目标是同时做到：

1. 普通数组少一次分配
2. 访问元素时不多一次指针跳转

那么第一步就应直接切到真正的尾部内联布局。

### 1.2 为什么改为内联

本阶段把 `FengArray` 改为尾部内联，核心原因有四点：

1. 语言层数组已经是固定长度语义，不再需要为了“未来可能扩容且头指针不变”让所有普通数组长期承担 split layout 的额外成本。
2. 非空数组当前会固定支付两次分配；改为内联后，普通数组可以直接减少一次堆分配与对应释放路径。
3. 当前 split layout 还让每次元素访问都多一次通过 `items` 取 payload 地址的间接访问；真正尾部内联后，这层间接可以一起去掉。
4. 文档层当前已经把数组描述成尾部 `items[]` 布局；把实现改为内联后，运行时实现与生命周期文档会重新一致。

因此，本阶段优先把语言层固定数组收敛到“真正内联的定长数组”模型；list 的增长与迁移优化留到下一阶段，通过私有迁移 API 单独解决，而不是继续让固定数组 runtime 同时承担两种目标。

## 2. 改造前锚点

### 2.1 改造前 split layout

当前 runtime 的数组布局在 `src/runtime/feng_runtime_internal.h` 约 14-31 行：

- `FengManagedHeader header`
- `size_t length`
- `size_t element_size`
- `const FengTypeDescriptor *element_desc`
- `FengValueKind element_kind`
- `const FengAggregateValueDescriptor *element_aggregate`
- `void *items`

也就是说，当前数组对象本体和元素区是两块内存。

### 2.2 改造前创建路径

`src/runtime/feng_array.c` 约 53-151 行的 `feng_array_new_kinded()` 当前执行两次分配：

1. `calloc(1, sizeof(*a))` 分配 header
2. `calloc(length, element_size)` 分配 `items`

`length == 0` 时，`items = NULL`。

### 2.3 改造前释放路径

`src/runtime/feng_array.c` 约 16-49 行的 `feng_array_finalize_internal()` 当前逻辑为：

1. 按 `element_kind` 做逐元素 release / aggregate release
2. `free(a->items)`
3. header 再由 object core 最终 `free(header)`

### 2.4 改造前 cycle collector 相关路径

`src/runtime/feng_cycle.c` 中当前仍直接依赖 `arr->items`：

- 约 282-318 行：数组扫描路径使用 `arr->items == NULL` 作为空数组判断，并据此遍历元素
- 约 480-510 行：另一处数组扫描路径同样依赖 `arr->items`
- 约 783-805 行：Phase 2 free white set 时显式 `free(arr->items)`
- 约 807-825 行：no-finalizer fast path 同样显式 `free(arr->items)`

### 2.5 当前文档状态

文档存在实现与表述不一致：

- `docs/feng-lifetime.md` 约 135-150 行把数组描述成概念上的尾部内联 `items[]`
- 真实 runtime 当前却仍是 split layout

这意味着：若本轮按“真正尾部内联”实施，生命周期文档反而会更接近真实实现；只需要把“概念表示”收紧为“当前实现表示”。

## 3. 本阶段设计目标

### 3.1 需要达到的目标

1. 非空固定数组从两次分配降为一次分配。
2. 数组元素访问不再多一次 `items` 指针跳转。
3. 零长度数组的可观察行为保持不变：`feng_array_data(array) == NULL`。
4. 数组公开 API 不变：`feng_array_new_kinded()`、`feng_array_new()`、`feng_array_data()`、`feng_array_length()`、`feng_array_check_index()` 的签名不变。
5. managed pointer / aggregate 元素的释放、默认初始化、cycle collector 行为保持正确。

### 3.2 本阶段不做

1. 不做 list 的内部迁移 API。
2. 不做 list grow/shrink 路径优化。
3. 不做 codegen 级数组读写协议改造。
4. 不引入新的用户可见数组语义。

## 4. 拟定实现方案

### 4.1 新的内存布局

本阶段建议把 `FengArray` 改成：

- header + metadata 固定头部
- 头部之后是对齐后的尾部数据区
- 不再保存 `void *items` 字段
- 不新增占位成员（包括 `alignas(max_align_t)` 的占位写法）

逻辑布局可表示为：

```c
struct FengArray {
	FengManagedHeader header;
	size_t length;
	size_t element_size;
	const FengTypeDescriptor *element_desc;
	FengValueKind element_kind;
	const FengAggregateValueDescriptor *element_aggregate;
};

/* allocation layout:
 * [FengArray header][padding for alignment][items payload]
 */
```

### 4.2 payload 地址的计算方式

建议在 `src/runtime/feng_array.c` 中新增内部 helper，例如：

```c
static size_t feng_array_data_offset(void);
static void *feng_array_data_inline(struct FengArray *a);
static const void *feng_array_const_data_inline(const struct FengArray *a);
```

思路：

1. 使用 `alignof(max_align_t)` 或同级别对齐基准
2. 将 `sizeof(struct FengArray)` 向上对齐到该边界
3. `length == 0` 时继续返回 `NULL`
4. `length > 0` 时返回 `((unsigned char *)a) + offset`

建议对齐公式（示意）：

1. `align = alignof(max_align_t)`
2. `offset = (sizeof(struct FengArray) + align - 1) & ~(align - 1)`
3. `data = (length == 0) ? NULL : ((unsigned char *)a + offset)`

说明：当前 Feng 运行时元素类型应不超过 `malloc` / `max_align_t` 能保证的对齐范围；若未来引入 over-aligned 类型，再单独扩展该设计。

### 4.3 创建路径

`feng_array_new_kinded()` 应改为：

1. 检查 `length * element_size` 溢出
2. 计算 `payload_size`
3. 计算 `data_offset`
4. 计算 `total_size = data_offset + payload_size`
5. 一次 `calloc(1, total_size)`
6. 初始化 header 与 metadata
7. 对 aggregate + `FENG_DEFAULT_INIT_FN` 继续做逐元素默认初始化

这里继续保留：

- `length == 0` 时 `feng_array_data() == NULL`
- `AGGREGATE + ZERO_BYTES` 仍依赖 `calloc` 的清零语义

### 4.4 释放路径

`feng_array_finalize_internal()` 只保留：

1. trivial：无逐元素处理
2. managed pointer：逐元素 `feng_release`
3. aggregate：逐元素 `feng_aggregate_release`

然后不再 `free(items)`；最终仍由 object core 统一释放整个对象块。

### 4.5 cycle collector 路径

collector 中数组遍历与释放都要同步改成基于：

- `arr->length == 0` 判断是否为空
- `feng_array_data_inline(arr)` 获取元素区地址
- 不再显式 `free(arr->items)`

### 4.6 codegen 层预期

本阶段预期无需改 `src/codegen/codegen.c` 的数组读写发码逻辑，因为当前相关路径都通过 runtime accessor：

- 数组下标读：约 9530-9555 行
- 数组下标写：约 10524-10708 行
- `&array` 取元素区指针：约 6330-6370 行

只要 `feng_array_data()` 的语义保持不变，codegen 就能自动适配新布局。

## 5. 实施步骤

按“先文档、后代码、再测试”的顺序，建议分 6 步实施。

### Step 1：更新方案文档与运行时注释

先改文档和注释，明确本阶段真正要落地的是“尾部内联，不保留 items 作为字段”。

优先文件：

- `dev/feng-array-optimize-delivered.md`（本文）
- `docs/feng-lifetime.md` 约 135-150 行
- `src/runtime/feng_array.c` 约 1-3 行文件头注释
- `src/runtime/feng_runtime_internal.h` 约 14-31 行数组布局注释

### Step 2：改 `struct FengArray` 内部布局

在 `src/runtime/feng_runtime_internal.h` 约 14-31 行：

1. 删除 `void *items`
2. 更新布局注释
3. 明确当前数据区来自同一 allocation 的尾部内联区域
4. 明确“不使用任何占位成员，数据区地址仅由 helper 计算”

### Step 3：改 `feng_array_new_kinded()` 和 accessor

在 `src/runtime/feng_array.c` 的以下区域：

- 约 53-151 行：重写创建逻辑为单次分配
- 约 158-163 行：重写 `feng_array_data()` 为按 offset 计算
- 可在该文件新增 1-3 个静态 helper，建议放在 `feng_array_finalize_internal()` 之后或 `feng_array_new_kinded()` 之前

### Step 4：改 finalize 路径

在 `src/runtime/feng_array.c` 约 16-49 行：

1. 保留逐元素 release
2. 删除 `free(a->items)`
3. 必要时补一行注释，说明 payload 与 header 同块释放

### Step 5：改 cycle collector 数组路径

在 `src/runtime/feng_cycle.c` 中至少要改四块：

- 约 282-318 行：数组扫描路径，`arr->items == NULL` 改为基于 `arr->length == 0` 或 accessor 结果判断
- 约 480-510 行：另一处数组扫描路径同样改动
- 约 783-805 行：删除 `free(arr->items)`
- 约 807-825 行：删除 `free(arr->items)`

### Step 6：补验证与回归

优先验证：

1. `make runtime`
2. `./build/bin/test_runtime`
3. `make test`

必要时再补一个 runtime 对齐测试，验证 `feng_array_data()` 返回地址满足本阶段承诺的对齐边界。

## 5A. 分步 Todo（执行清单）

以下 Todo 按“可独立提交、可独立验收”拆分。每个任务完成后都应先通过对应最小验证，再进入下一个任务。

- [x] Todo-01：冻结方案文档与边界
	目标：文档明确“第一步只做尾部内联，不做 list 迁移 API”。
	交付：更新本文件；同步确认 `docs/feng-builtin-type.md` 与 `docs/feng-lifetime.md` 的职责边界描述正确。
	验收：文档评审通过（不改代码）。

- [x] Todo-02：更新 runtime 注释与布局说明
	目标：实现前先让注释表达和目标方案一致。
	交付：更新 `src/runtime/feng_runtime_internal.h`、`src/runtime/feng_array.c` 文件头和数组布局注释。
	验收：注释中必须明确“完全不占位（无 `items` 字段、无占位成员）+ helper 统一对齐与偏移计算”；仅注释改动，构建无回归。

- [x] Todo-03：改 `FengArray` 内部布局（移除 `items` 字段）
	目标：结构体层完成“尾部内联”前置改造。
	交付：`src/runtime/feng_runtime_internal.h` 中 `struct FengArray` 去掉 `void *items` 字段，并补充新的 allocation layout 注释。
	验收：`make runtime` 可编译通过（允许功能未完整，禁止编译错误扩散到无关模块）。

- [x] Todo-04：实现 payload 地址计算 helper
	目标：统一 offset 计算，避免散落重复逻辑。
	交付：在 `src/runtime/feng_array.c` 增加 `data_offset` / `data_inline` helper（命名可微调），覆盖对齐与 `length == 0` 返回 `NULL` 逻辑。
	验收：helper 被 `feng_array_data()` 和数组内部遍历路径复用；无重复手写偏移计算。

- [x] Todo-05：重写数组创建路径为单次分配
	目标：`feng_array_new_kinded()` 从“两次分配”变为“一次分配”。
	交付：改 `src/runtime/feng_array.c` 创建逻辑，保留现有 precondition/overflow/aggregate-init 语义。
	验收：`./build/bin/test_runtime` 中数组创建相关测试通过。

- [x] Todo-06：重写 finalize 路径（去掉 `free(items)`）
	目标：内联布局下不再独立释放 payload。
	交付：改 `src/runtime/feng_array.c` 的 `feng_array_finalize_internal()`，保留逐元素 release，删除 `free(a->items)`。
	验收：managed pointer 与 aggregate 释放测试通过，无 double-free。

- [x] Todo-07：同步改 cycle collector 数组路径
	目标：collector 不再依赖旧 split-layout 假设。
	交付：改 `src/runtime/feng_cycle.c` 数组扫描与 white set free 路径，删除 `free(arr->items)`，空数组判断改为 `length == 0` 或统一 helper。
	验收：`make test_runtime` + `make test` 通过；无 collector 崩溃/误回收。

- [x] Todo-08：补对齐与零长度行为回归
	目标：锁定本阶段最关键行为边界。
	交付：完善 `test/runtime/test_runtime.c`，确保零长度数组仍断言 `feng_array_data(array) == NULL`；必要时新增对齐断言测试。
	验收：`./build/bin/test_runtime` 全绿。

- [x] Todo-09：全量回归与文档收口
	目标：完成第一步交付闭环。
	交付：更新 `docs/feng-lifetime.md` 运行时表示文字；执行全量回归。
	验收：`make test` 通过；本文件状态更新为“第一步已完成”。

## 6. 需要改动的文件与大体行数

### 6.1 必改文件

| 文件 | 大体行数 | 主要改动 |
| --- | --- | --- |
| `dev/feng-array-optimize-delivered.md` | 全文 | 记录本方案、边界、步骤与风险 |
| `docs/feng-lifetime.md` | 约 135-150 行 | 将数组运行时布局说明收紧为真实实现 |
| `src/runtime/feng_runtime_internal.h` | 约 14-31 行 | 删除 `items` 字段，更新数组内部布局注释 |
| `src/runtime/feng_array.c` | 约 1-3 行、16-49 行、53-163 行 | 更新注释、重写创建/访问器/释放路径 |
| `src/runtime/feng_cycle.c` | 约 282-318 行、480-510 行、783-825 行 | 更新数组遍历与 free 路径 |

### 6.2 需要验证但大概率不改的文件

| 文件 | 大体行数 | 核对目的 |
| --- | --- | --- |
| `src/codegen/codegen.c` | 约 6330-6370 行、9530-9555 行、10524-10708 行 | 确认数组访问统一经由 accessor，无需因布局变化改调用点 |
| `docs/feng-builtin-type.md` | 约 181-183 行 | 确认数组 runtime 描述仍通过 lifecycle 文档引用，不需要补语义内容 |
| `Makefile` | 约 69-91 行 | 确认 runtime / 全量回归入口即可覆盖本次改动 |

### 6.3 必查测试文件

| 文件 | 大体行数 | 关注点 |
| --- | --- | --- |
| `test/runtime/test_runtime.c` | 约 249-254 行 | 普通零长度数组 `feng_array_data(array) == NULL` |
| `test/runtime/test_runtime.c` | 约 227-244 行 | managed pointer 数组释放路径 |
| `test/runtime/test_runtime.c` | 约 1305-1325 行 | aggregate 默认初始化路径 |
| `test/runtime/test_runtime.c` | 约 1369-1377 行 | aggregate 零长度数组仍返回 `NULL` |

## 7. 关键风险与处理原则

### 7.1 对齐

这是本阶段最关键的实现风险。

当前 split layout 的一个直接好处是：独立 `malloc(length * element_size)` 天然吃到 `malloc` 的最大对齐保证。改成尾部内联后，需要显式计算 payload offset 的对齐。

处理原则：

1. 用统一 helper 计算 offset
2. 只在一处定义对齐规则
3. 所有数组遍历与访问都走同一 helper / accessor，不要散落重复计算

### 7.2 零长度数组语义

本阶段建议保留现有语义：

- `feng_array_length(array) == 0`
- `feng_array_data(array) == NULL`

这样可以继续复用已有测试与空数组判断，不额外制造行为 churn。

### 7.3 aggregate 默认初始化

本阶段不能破坏现有语义：

- `FENG_DEFAULT_ZERO_BYTES` 继续依赖一次性 `calloc`
- `FENG_DEFAULT_INIT_FN` 继续逐元素执行

这部分的实现不应该因为布局变化而改语义，只改 base pointer 的来源。

### 7.4 collector 双重释放风险

只要改了数组 finalize 而没同步改 collector，极易出现：

- 旧路径仍 `free(arr->items)`
- 新布局下 payload 与 header 同块
- 最终导致双重释放或非法释放

因此 `src/runtime/feng_array.c` 与 `src/runtime/feng_cycle.c` 必须同一批修改、同一批验证。

## 8. 验证口径

### 8.1 最小验证

```sh
make runtime
./build/bin/test_runtime
```

### 8.2 全量回归

```sh
make test
```

### 8.3 应重点关注的回归点

1. 零长度数组依旧返回 `NULL`
2. managed pointer 数组释放不泄漏、不双 free
3. aggregate 默认初始化仍逐元素执行
4. collector 含数组路径不崩溃、不误 free
5. codegen 生成的数组下标访问无需改动仍能工作

## 9. 后续边界（本稿之外）

本稿完成后，后续可以继续讨论第二步：

- 为 list 引入内部迁移 API
- 在“消费旧数组”的前提下做批量迁移
- 迁移时避免保留前缀元素的 ARC 增减

但这些内容都不应和本阶段的“尾部内联”布局改造绑定在同一批实现中。

后续 array storage runtime contract API 及 `List<T>` 优化方案统一见
`dev/feng-array-storage-dev.md`；本文不重复定义其容量模型、迁移语义与实施步骤。
