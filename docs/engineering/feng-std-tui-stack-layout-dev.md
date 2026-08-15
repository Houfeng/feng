# Feng TUI VStack/HStack 开发方案

> 状态：待人工 Review，尚未实施。
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 中轴向布局容器阶段的专项开发文档。
> Widget、Style、frame、drawFrame、组件树和事件路由仍分别以
> `docs/engineering/feng-std-tui-view-dev.md`、
> `docs/engineering/feng-std-tui-text-dev.md` 和
> `docs/engineering/feng-std-tui-focus-key-routing-dev.md` 为准；本文只定义布局测量契约及
> `VStack`/`HStack` 行为。

## 1 目标

本阶段完成：

- 实现按垂直主轴排列普通流子组件的 `VStack`；
- 实现按水平主轴排列普通流子组件的 `HStack`；
- 让容器自主测量、排列和绘制 children，ViewManager 仍只调度 root；
- 明确固定尺寸、百分比、Auto、Align、margin、padding 与轴向排列的组合规则；
- 支持 Text 等内容尺寸组件作为 Stack 的直接或嵌套子组件；
- 保证 Normal/Relative 参与普通流，Absolute/Fixed 脱离普通流；
- 使用确定的两阶段布局处理内容变化和终端 Resize，不引入不收敛的循环 Reflow；
- 保持绘制顺序、drawFrame、祖先裁剪、鼠标命中和事件冒泡的既有契约。

本阶段不实现 `spacing`、主轴内容整体对齐、权重分配、Flex、Grid、Dock、ScrollView、
虚拟滚动、Button、Input、颜色合成或布局脏标记。子组件间距继续通过 margin 表达；需要
按权重分配剩余空间时，后续设计独立的 Flex 能力，不在 VStack/HStack 中提前加入。

## 2 当前 arrange 契约的缺口

当前 Widget 只有：

```feng
func arrange(manager: ViewManager): void;
```

组件通过 parent.frame 推导完整的父 content 区域，再根据自身 Style 写入 frame。该契约
足以处理自由定位的 View 和 Text，但不能完整表达轴向容器给 child 分配的最终槽位：

- VStack 必须先知道 child 的期望高度，才能确定下一个 child 的 y；
- HStack 必须先知道 child 的期望宽度，才能确定下一个 child 的 x；
- Stack 分配槽位后，child 及其后代必须使用该槽位完成最终布局；
- 如果只调用现有 `child.arrange(manager)` 后覆盖 child.frame，嵌套容器的后代仍按覆盖前
  的区域排列，frame 与内部布局不一致；
- 如果临时修改 child.style 或 parent.frame 再恢复，会引入可观察的中间状态、重入风险和
  难以维护的隐式协议。

因此，本阶段不使用覆盖 frame、临时修改 Style、伪造 parent 或按具体组件类型判断等
处理。布局契约调整为通用的“测量 + 最终排列”两阶段，供后续所有布局容器复用。

## 3 通用两阶段布局契约

### 3.1 Size

在 `std.tui.common` 增加不分配堆对象的值类型：

```feng
/** 非负二维尺寸。 */
@value
open type Size {
  var width: int;
  var height: int;
}
```

Size 只表示尺寸，不承载坐标；最终位置和尺寸仍由 Rect 表达。

### 3.2 Widget 布局成员

Widget 的布局契约调整为：

```feng
open spec Widget {
  /** 最近一次 measure 得到的期望外部尺寸，包含 margin。 */
  var desiredSize: Size;

  /** 在给定可用尺寸内计算并缓存期望外部尺寸。 */
  func measure(manager: ViewManager, available: Size): Size;

  /** 在父容器给出的最终槽位内写入 frame，并排列自身后代。 */
  func arrange(manager: ViewManager, slot: Rect): void;
}
```

`desiredSize` 包含当前组件 frame 和两侧 margin，使父容器可以直接累加或取最大值；
`frame` 仍不包含 margin。`measure()` 返回值与写入的 `desiredSize` 相同。当前 spec seal
成员尚未落地，`desiredSize` 暂按 frame、drawFrame 的既有过渡方式进入 Widget；seal
能力落地后再收紧，不在本阶段引入旁路存储。

两阶段职责严格分离：

- measure 可以读取 Style、内容和 children，不能写入最终 frame、drawFrame 或 sequence；
- arrange 使用 measure 缓存和父容器给出的 slot 写入最终 frame，并递归排列 children；
- arrange 不重新解释百分比生成另一份期望尺寸；百分比在 measure 的 available 约束下
  解析，避免 Auto 父子之间进行不收敛的反复求值；
- draw 只读取 arrange 的最终 frame，不触发布局。

Size、desiredSize 和 Rect 均为固定大小值，不增加每次布局的堆分配，也不修改 runtime ABI。

### 3.3 ViewManager 调度

ViewManager 的布局入口改为一次完整的两阶段调度：

```text
screen Size
  -> root.measure(manager, screenSize)
  -> root.arrange(manager, screenRect)
```

ViewManager 仍只接触 root，不遍历组件树，也不理解 VStack、HStack 或 Text。容器自行递归
measure/arrange children，保持组件自治。

终端 Resize、content 或 Style 改变后，下一轮 `ViewManager.arrange()` 重新执行上述两阶段，
即得到新的布局结果。本阶段仍不增加脏标记、布局队列或异步 Reflow。

## 4 通用测量和排列规则

### 4.1 measure

每个轴独立计算。available 是父容器提供的非负外部约束，组件两侧 margin 先按该轴
available 解析，再得到可用于 frame 的尺寸约束：

- 固定 int 尺寸保持原值；desiredSize 再加两侧 margin；
- double 尺寸以扣除两侧 margin 后的 available 为基数，向零截断并保持非负；
- Auto 使用组件的内在内容尺寸；基础 View 的内在尺寸仍为 0；
- Align 不改变 desiredSize。特别是 Full 在 measure 阶段保留组件的自然期望尺寸，只在
  arrange 获得最终 slot 后决定是否拉伸；
- 固定和百分比尺寸不因内容过大而扩张；内容溢出仍由绘制裁剪或后续 ScrollView 处理。

Text.measure 使用最终可能获得的宽度约束完成 grapheme 分行并计算 Auto 高度；如果
arrange 得到的实际宽度与测量宽度不同，Text 只在宽度确实变化时重新生成分行结果，不
无条件重复扫描 content。

### 4.2 arrange

slot 是父容器分配给组件的外部区域，包含该组件 margin。组件按 measure 已缓存的
desiredSize 和现有 Align 规则得到 frame：

- Start/Center/End 使用期望 frame 尺寸在扣除 margin 的 slot 内定位；
- Full 使用扣除 margin 后的完整 slot，忽略该轴的期望 frame 尺寸；
- Relative 与 Normal 使用相同槽位规则，x/y 仍不参与自身普通流布局；
- Absolute 使用最近的非 Normal 祖先 frame 作为定位参照；
- Fixed 使用 Screen Rect 作为定位参照；
- Absolute/Fixed 仍忽略 Align，并使用 measure 已解析的尺寸与既有 x/y 规则定位。

父容器只分配 slot，不直接写 child.frame。child.arrange 负责写入自身 frame，并在自身为
容器时继续排列后代，从而保证嵌套布局始终以最终 frame 为依据。

## 5 VStack/HStack 公开类型

两个容器使用独立 spec，使各自的 `@mixable static` measure/arrange/draw 能按首参数
契约生成正确实例 wrapper，同时复用 Container 的组件树能力：

```feng
/** 垂直轴向容器契约。 */
open spec VStackWidget: ContainerWidget {}

/** 水平轴向容器契约。 */
open spec HStackWidget: ContainerWidget {}

open type VStack: VStackWidget {
  ...: Container = Container();

  @mixable
  static func measure(stack: VStackWidget,
    manager: ViewManager, available: Size): Size;

  @mixable
  static func arrange(stack: VStackWidget,
    manager: ViewManager, slot: Rect): void;

  @mixable
  static func draw(stack: VStackWidget, manager: ViewManager): void;
}

open type HStack: HStackWidget {
  ...: Container = Container();

  @mixable
  static func measure(stack: HStackWidget,
    manager: ViewManager, available: Size): Size;

  @mixable
  static func arrange(stack: HStackWidget,
    manager: ViewManager, slot: Rect): void;

  @mixable
  static func draw(stack: HStackWidget, manager: ViewManager): void;
}
```

VStackWidget/HStackWidget 本阶段不增加公开字段。方向差异只存在于实现使用的主轴和交叉轴，
不增加公开 Orientation 枚举，也不暴露通用 Stack 类型。

## 6 普通流与脱离普通流

Stack 按 children 的 List 顺序处理：

- Normal、Relative 是普通流 child，参与 Stack 的期望尺寸计算和主轴游标推进；
- Absolute、Fixed 不占用普通流空间，不影响 Stack 的 Auto 尺寸；
- Absolute、Fixed 仍执行 measure 和 arrange，使其自身及后代得到有效布局；
- 所有 child 均保留原 parent，事件路径和祖先裁剪不因布局方式改变；
- draw 始终按 children 的 List 顺序执行，不按 position 隐式重排层级。

因此，Position 只影响布局和定位，不自动改变绘制层级。需要覆盖关系时，使用方通过
children 顺序决定；未来 zIndex 仍由具体容器自行实现。

## 7 轴向测量规则

### 7.1 VStack

VStack 先从自身可用 frame 尺寸扣除非负 padding，得到 children 的内容约束，再按顺序
measure 所有普通流 child：

- 期望内容高度为普通流 children 的 desiredSize.height 之和；
- 期望内容宽度为普通流 children 的 desiredSize.width 最大值；
- 空 VStack 的期望内容宽高均为 0；
- VStack 的内在尺寸为期望内容尺寸加自身 padding；
- VStack 自身固定、百分比、Auto 和 Align 仍按第 4 节通用规则解析。

### 7.2 HStack

HStack 使用对称规则：

- 期望内容宽度为普通流 children 的 desiredSize.width 之和；
- 期望内容高度为普通流 children 的 desiredSize.height 最大值；
- 空 HStack 的期望内容宽高均为 0；
- HStack 的内在尺寸为期望内容尺寸加自身 padding。

### 7.3 padding 与 margin

- Stack 的 padding 位于自身 frame 内，缩小 children 可使用的内容区域；
- child margin 已包含在 child.desiredSize 中，主轴累加和交叉轴最大值都包含 margin；
- 相邻 child margin 不折叠，前一个末端 margin 与后一个起始 margin 均保留；
- 百分比 padding 以 Stack 测量时获得的可用 frame 约束为基数；
- child 百分比 margin 和尺寸以 Stack 传给 child.measure 的内容约束为基数；
- 所有最终 frame.width/frame.height 及 Size.width/height 保持非负。

百分比与 Auto 组合只按本次 measure 的 available 约束解析一次，不通过重复迭代求固定点。
这使布局结果确定且有上界；父级最终分配空间小于期望尺寸时产生正常溢出，不反向扩大
已经完成 arrange 的祖先。

## 8 主轴与交叉轴排列

### 8.1 主轴

Stack 拥有普通流 child 的主轴位置决定权：

- VStack 从 content.y 向下推进游标；
- HStack 从 content.x 向右推进游标；
- 每个 child 的主轴 slot 长度使用其 desiredSize 对应轴长度；
- 主轴 Align.Full 不拉伸 child，也不分配剩余空间；在 Stack 主轴上与 Start 使用相同的
  期望尺寸。这与 WPF/Avalonia StackPanel 一类轴向容器的主轴语义一致；
- Stack 主轴存在多 child 时，“每个 Full 都占满父容器”没有唯一结果，因此本阶段不隐式
  等分空间，也不引入权重；需要主轴填充时使用明确尺寸，后续由 Flex/Dock 表达；
- 当 children 的期望主轴总长度超过 content 区域时继续按期望尺寸排列，是否可见由
  overflow 和 Screen 裁剪决定。

该规则只覆盖 child 位于 VStack/HStack 主轴时的排列；Widget 在普通 Container 或 Stack
交叉轴上的 Full 语义保持不变。

### 8.2 交叉轴

每个普通流 child 都获得覆盖 Stack 完整 content 交叉轴的 slot：

- Full 扣除 margin 后填满交叉轴；
- Start/Center/End 使用 child 的期望交叉轴尺寸进行定位；
- child 期望交叉轴尺寸超过 content 时不自动压缩，允许正常溢出。

### 8.3 Relative

Relative child 与 Normal 使用完全相同的 Stack 槽位，不应用 style.x/y 偏移。它与 Normal
的唯一区别仍是可以成为 Absolute 后代的定位参照，保持现有 Position 契约不变。

## 9 Auto 与 Reflow 边界

Stack Auto 尺寸由普通流 children 的 desiredSize 和自身 padding 得到，不读取 children
的旧 frame，也不依赖上一帧布局结果。

本阶段的 Reflow 定义为“下一次完整 measure + arrange 重新计算”，而不是在一次布局中
反复遍历直到数值稳定：

- Text content 改变后，下一轮 measure 更新 Text 和祖先 Stack 的 desiredSize；
- 终端 Resize 后，新 Screen Size 作为新的根 available 传入整棵树；
- 百分比、自动换行和 Auto 高度在同一轮 measure 中自底向上形成期望尺寸；
- arrange 再自顶向下提交最终 frame；
- 不设置最大迭代次数，也不存在“迭代未收敛后使用最后结果”的隐式行为。

这一边界避免 Auto 父级和百分比/Full 子级之间形成递归求值，同时为后续布局脏标记保留
扩展空间；未来即使只重排脏子树，两阶段契约也不需要改变。

## 10 绘制、裁剪与命中

VStack/HStack.draw 按以下顺序执行：

1. 使用 View 的通用绘制逻辑处理自身背景、drawFrame 和 sequence 登记；
2. 按 children 的 List 顺序调用每个 child.draw(manager)；
3. child 自行决定是否以及如何绘制自身后代。

Stack 不根据可见区域跳过 child.draw；第一版优先保证 drawFrame、透明背景、祖先 Hidden
裁剪及 sequence 顺序完全复用现有逻辑。虚拟化和只绘制可见 children 由未来专用容器
实现，不加入通用 Stack。

透明 Stack 仍登记自身 sequence，保持 Widget 当前“实际调用 draw 且具有有效 drawFrame
即可命中”的规则。是否需要容器背景透明时穿透属于独立命中策略，不在本阶段改变。

## 11 性能与实现约束

- Size 和 Rect 均为值类型，不增加每次 measure/arrange 的对象分配；
- desiredSize 是每个 Widget 的固定布局状态，不使用 sidecar 或全局映射；
- Stack 直接遍历既有 children List，不为普通流 child 创建临时 List、slot 对象或排序副本；
- 每轮布局对每个 child 执行一次 measure 和一次 arrange，时间复杂度为 O(n)；
- draw 继续为 O(n)，不增加与颜色合成相关的处理；
- 不修改 Cell、Buffer、Screen、InputManager、runtime 或编译器 ABI；
- 不按 Text、Button 等具体类型分支，所有 child 仅通过 Widget 契约参与布局；
- 不通过临时修改公开 Style、parent 或 frame 模拟布局上下文。

## 12 测试范围

新增 std_test 用例覆盖：

- Size、View/Text measure 与 arrange 的基本契约；
- ViewManager 对 root 的 measure -> arrange 调度顺序；
- 空 VStack/HStack、单 child 和多个 child；
- VStack 主轴高度累加、交叉轴最大宽度及最终坐标；
- HStack 主轴宽度累加、交叉轴最大高度及最终坐标；
- 固定、百分比、Auto 尺寸与 Start/Center/End/Full 交叉轴排列；
- 主轴 Full 不拉伸且交叉轴 Full 拉伸；
- Stack padding、child margin、不折叠 margin 和负间距归零；
- Normal/Relative 参与普通流，Absolute/Fixed 不占流空间；
- Absolute 使用最近非 Normal 祖先、Fixed 使用 Screen 的既有定位行为；
- 嵌套 VStack/HStack 以及 Text 自动换行、Auto 高度向祖先传播；
- children 超出 Stack、Stack overflow Hidden 和 Screen 边界裁剪；
- draw 顺序、sequence 顺序、drawFrame 和逆序鼠标命中；
- content、Style 和 Screen 尺寸改变后的下一轮重新布局；
- 不修改 children、Style、parent 的临时状态，布局后组件树关系保持不变；
- std、std_test 和 tui_demo 定向构建；
- `make test` 全量回归。

测试只新增相关用例；如确需调整已有测试，先获得人工批准。仅 docs 变更不执行测试；开始
实现后，按仓库规范在非 Codex 沙箱环境执行 `make test`。

## 13 实施 TODO

- [ ] 13.1 人工 Review 并确认本文档，特别确认两阶段布局契约和 Stack 主轴 Full 语义；
- [ ] 13.2 增加 Size，并将 Widget/View/Text/ViewManager 调整为 measure + arrange(slot)；
- [ ] 13.3 补充基础两阶段布局用例，确认既有 View/Text 最终 frame 保持兼容；
- [ ] 13.4 定义 VStackWidget/HStackWidget 和 VStack/HStack 类型；
- [ ] 13.5 实现 Stack 通用内部轴向测量与排列算法；
- [ ] 13.6 实现 children 绘制、sequence、裁剪和命中集成；
- [ ] 13.7 补齐 VStack/HStack、嵌套布局、Text Auto 与脱离普通流用例；
- [ ] 13.8 更新 tui_demo，增加可人工验证的嵌套 VStack/HStack 布局；
- [ ] 13.9 构建 std、std_test 和 tui_demo，处理定向验证发现的问题；
- [ ] 13.10 在非 Codex 沙箱环境执行 `make test` 全量回归；
- [ ] 13.11 根据实现结果更新相关文档 TODO，等待人工 Review。
