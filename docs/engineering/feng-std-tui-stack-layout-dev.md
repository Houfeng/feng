# Feng TUI VStack/HStack 开发方案

> 状态：待人工 Review，尚未实施。
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 中轴向布局容器阶段的专项开发文档。
> Widget、Style、frame、drawFrame、组件树和事件路由仍分别以
> `docs/engineering/feng-std-tui-view-dev.md`、
> `docs/engineering/feng-std-tui-text-dev.md` 和
> `docs/engineering/feng-std-tui-focus-key-routing-dev.md` 为准；本文只定义
> `VStack`/`HStack` 的排列和局部 Reflow 行为。

## 1 目标

本阶段完成：

- 实现按垂直主轴排列普通流子组件的 `VStack`；
- 实现按水平主轴排列普通流子组件的 `HStack`；
- 让容器自主排列和绘制 children，ViewManager 仍只调度 root；
- 明确固定尺寸、百分比、Auto、Align、margin、padding 与轴向排列的组合规则；
- 支持 Text 等内容尺寸组件作为 Stack 的直接或嵌套子组件；
- 保证 Normal/Relative 参与普通流，Absolute/Fixed 脱离普通流；
- 在 Stack 内完成子尺寸影响父尺寸、父尺寸再影响 children 的局部 Reflow；
- 保持 drawFrame、祖先裁剪、绘制顺序、鼠标命中和事件冒泡的既有契约。

本阶段不实现公共 measure 阶段、全局布局系统重构、跨帧 dirty tree、`spacing`、主轴内容
整体对齐、权重分配、Flex、Grid、Dock、ScrollView、虚拟滚动、Button、Input 或颜色合成。
子组件间距继续通过 margin 表达。

## 2 基本原则

### 2.1 保持现有 Widget 契约

Widget 的公开布局方法保持不变：

```feng
func arrange(manager: ViewManager): void;
```

不增加 `measure()`，不改变 arrange 的参数或返回值，也不要求普通 View、Text 或未来所有
Widget 实现统一测量接口。组件需要的内在尺寸仍由组件自己的 arrange 负责：

- Text 在 arrange 内完成 grapheme 分行和 Auto 高度计算；
- VStack/HStack 在 arrange 内排列 children，并根据结果处理自身 Auto 尺寸；
- 未来其他布局组件可以根据自身需求实现自己的局部布局过程。

ViewManager.arrange 仍只调用 `root.doArrange(manager)`，不遍历组件树，也不理解具体组件
类型。Stack 通过 `child.doArrange(manager)` 自治调度需要参与布局的 children。

### 2.2 不修改用户 Style

Comlet 具有独立的 `RtStyle`，VStack/HStack 可以在 Flow 阶段向 child 的运行时样式写入
主轴位置。Feng TUI 当前没有独立运行时 Style，因此不能照搬这一实现：

- Stack 不修改 child.style.x/y、position 或 Align；
- 不把 Auto 改写为计算后的固定尺寸；
- 不临时修改 parent.frame 后调用 child.arrange；
- 不在 View 中判断 parent 是否为具体 VStack/HStack；
- 不在 child.arrange 返回后递归平移整个子树。

这些做法要么改变用户声明，要么使 Absolute/Fixed 后代使用错误参照，要么形成具体组件
特判，均不作为本阶段方案。

### 2.3 Reflow 由需要它的组件负责

VStack/HStack 的 Auto 尺寸依赖 children；Stack 尺寸变化又可能改变百分比 child、Full
child 或 Text 自动换行结果。因此 Stack 复用 Widget 已有的 `doArrange()` 和
`requestReflow()` 完成局部 Reflow，不能假定单次从父到子的 arrange 总能得到最终结果。

child 的最终占位变化时只请求直接父级 Stack 重新布局；Stack 自身占位变化后才继续请求
其直接父级。更高祖先的 `SubtreeLayout` 仅用于调度进入对应子树，不表示这些祖先自身需要
重新布局。该机制不增加全局 measure/arrange 阶段。

## 3 通用排列槽位

### 3.1 必要性

Stack 必须在不改变 arrange 签名的前提下告诉普通流 child：

- 本次由父容器控制哪个轴的位置；
- child 在该轴从哪个坐标开始；
- 百分比和交叉轴 Align 使用哪个可用区域。

如果只在 `child.arrange(manager)` 返回后覆盖 child.frame，嵌套容器的后代已经基于旧
frame 完成布局；如果递归平移子树，Absolute/Fixed 后代又可能被错误移动。因此需要一个
通用、瞬时的排列槽位，而不是事后修正 frame。

### 3.2 ArrangeSlot

在 `std.tui.view` 增加值类型：

```feng
/** 父布局容器为一次 child.arrange 提供的瞬时排列槽位。 */
@value
open type ArrangeSlot {
  /** 可用区域及主轴起点，使用 Screen 绝对坐标。 */
  let frame: Rect;

  /** 父容器是否控制水平轴的普通流位置。 */
  let ownsHorizontal: bool;

  /** 父容器是否控制垂直轴的普通流位置。 */
  let ownsVertical: bool;
}
```

Widget 增加一个布局过程状态：

```feng
open spec Widget {
  /** 仅在父容器调用本次 arrange 期间有效。 */
  var arrangeSlot: Option<ArrangeSlot>;
}
```

当前 spec seal 成员尚未落地，`arrangeSlot` 暂按 frame、drawFrame、parent 的既有过渡方式
进入 Widget；seal 能力落地后应改为 seal。它是固定大小状态，不分配对象，不进入 Style，
也不改变 arrange 的公开方法签名。

父容器按以下顺序使用：

```text
child.arrangeSlot = slot
child.arrange(manager)
child.arrangeSlot = none
```

清理使用 defer 保证 child.arrange 抛错时也不会留下过期槽位。槽位只对
Normal/Relative 生效；Absolute/Fixed 忽略槽位，继续使用既有定位参照。

### 3.3 View 通用辅助方法处理槽位

View 的既有 reference、尺寸和位置辅助方法统一增加对 ArrangeSlot 的解析；
`View.arrange(manager)` 的公开实例签名保持不变。Text 已经复用这些 View 辅助方法，因而
自然获得同一槽位语义，不在 Text 中增加 Stack 分支或第二套计算：

- `ownsHorizontal == true`：frame.x 从 slot.frame.x 加 margin.left 开始，忽略
  horizontalAlign 对水平位置的 Start/Center/End 计算；
- `ownsVertical == true`：frame.y 从 slot.frame.y 加 margin.top 开始，忽略
  verticalAlign 对垂直位置的 Start/Center/End 计算；
- 父容器拥有的轴上，Full 不强制占满整个 parent content，而按 width/height 的
  固定值、百分比或组件 Auto 内在尺寸解析；
- 父容器未拥有的交叉轴继续使用现有 Start/Center/End/Full 行为；
- 百分比尺寸和 margin 以 slot.frame 的对应轴尺寸为基数；
- Relative 与 Normal 相同，x/y 仍不参与普通流排列。

这是布局容器分配普通流位置的通用机制，未来其他布局容器可以复用；View 不感知
VStack/HStack 类型，也不增加具体组件分支。

## 4 VStack/HStack 类型

两个容器使用独立 spec，使各自的 `@mixable static` arrange/draw 按首参数契约生成实例
wrapper，同时复用 Container 的组件树能力：

```feng
open spec VStackWidget: ContainerWidget {}

open spec HStackWidget: ContainerWidget {}

open type VStack: VStackWidget {
  ...: Container = Container();

  @mixable
  static func arrange(stack: VStackWidget, manager: ViewManager): void;

  @mixable
  static func draw(stack: VStackWidget, manager: ViewManager): void;
}

open type HStack: HStackWidget {
  ...: Container = Container();

  @mixable
  static func arrange(stack: HStackWidget, manager: ViewManager): void;

  @mixable
  static func draw(stack: HStackWidget, manager: ViewManager): void;
}
```

VStackWidget/HStackWidget 本阶段不增加面向使用方的布局属性，不增加公开 Orientation，也
不暴露通用 Stack 类型。两个实现内部复用相同的轴向帮助函数，避免复制两套计算规则。

## 5 普通流与脱离普通流

Stack 按 children 的 List 顺序处理：

- Normal、Relative 是普通流 child，参与 Stack 的 Auto 尺寸和主轴游标推进；
- Absolute、Fixed 不占用普通流空间，不影响 Stack 的 Auto 尺寸；
- Absolute、Fixed 仍调用 arrange，使自身及其后代得到有效 frame；
- 所有 child 保留原 parent，事件路径和祖先裁剪不因布局方式改变；
- draw 始终按 children 的 List 顺序执行，不按 position 隐式改变层级。

Position 只影响布局和定位。覆盖顺序继续由 children 顺序决定；未来 zIndex 仍由具体
容器自行实现。

## 6 轴向排列规则

### 6.1 VStack

VStack 从自身 frame 扣除非负 padding 得到 content 区域，然后按 List 顺序处理普通流
children：

1. 当前主轴游标从 content.y 开始；
2. 为 child 设置 `ownsVertical == true` 的 ArrangeSlot；
3. child 自行完成 arrange；
4. 游标增加 child 的 margin.top、frame.height 和 margin.bottom；
5. 交叉轴由 child.horizontalAlign 决定，Full 填满 content 宽度，其他 Align 使用最终宽度
   定位。

VStack 的 Auto 内在高度为所有普通流 child 外部高度之和加上下 padding；Auto 内在宽度为
所有普通流 child 外部宽度最大值加左右 padding。空 VStack 的 Auto 内在宽高均为 0。

当 VStack 自身宽度为 Auto 时，内部尺寸探测轮次暂由 VStack 同时拥有 child 的水平轴，
使 child 的水平 Full 按自身 width/Auto 内在宽度得到自然尺寸；VStack 得到最终宽度后，
最终排列轮次恢复正常交叉轴规则，让水平 Full child 填满最终 content 宽度。这是 VStack
自身 arrange 的内部过程，不形成公共 measure 阶段。

### 6.2 HStack

HStack 使用对称规则：

1. 当前主轴游标从 content.x 开始；
2. 为 child 设置 `ownsHorizontal == true` 的 ArrangeSlot；
3. child 自行完成 arrange；
4. 游标增加 child 的 margin.left、frame.width 和 margin.right；
5. 交叉轴由 child.verticalAlign 决定。

HStack 的 Auto 内在宽度为所有普通流 child 外部宽度之和加左右 padding；Auto 内在高度为
所有普通流 child 外部高度最大值加上下 padding。

当 HStack 自身高度为 Auto 时使用对称规则：内部尺寸探测轮次暂由 HStack 同时拥有 child
的垂直轴，取得自然高度；最终排列轮次再恢复垂直交叉轴 Align。

### 6.3 主轴 Full

Stack 拥有普通流 child 的主轴位置。主轴上的 Full 不拉伸 child，也不隐式分配剩余空间，
而按 child 的 width/height 声明解析；声明为 Auto 时使用 child 自身的内在尺寸。

这与 Comlet VStack/HStack 将 child 主轴 Align 改为 None 的结果一致，但 Feng 通过
ArrangeSlot 表达，不修改 Style。交叉轴 Full 保持“占满父容器 content 交叉轴”的既有
语义。

多个主轴 Full 不存在唯一的“全部占满父容器”结果。本阶段不等分剩余空间，也不引入
权重；需要该能力时由后续 Flex/Dock 单独定义。

### 6.4 margin 与 padding

- Stack padding 位于自身 frame 内，缩小 children content 区域；
- child margin 位于 child.frame 外，参与主轴推进和交叉轴占用；
- 相邻 child margin 不折叠，两侧均保留；
- 负 padding 按现有规则归零；负 margin 按现有 View 规则归零；
- 百分比 padding 以 Stack 当前 frame 为基数；
- child 百分比 margin 和尺寸以本次 ArrangeSlot 的 frame 为基数；
- 所有最终 frame.width/frame.height 保持非负。

## 7 Stack 局部 Reflow

### 7.1 触发原因

一次 children 排列后，以下结果可能要求 Stack 重新排列：

- 普通流 child 的最终主轴或交叉轴尺寸与上一轮不同；
- Stack 的 Auto width/height 根据 children 计算后发生变化；
- Stack content 区域变化，使百分比或 Full child 的可用区域变化；
- VStack 宽度变化使 Text 重新换行并改变高度；
- 嵌套 Stack 返回的新尺寸改变外层后续 siblings 的位置。

### 7.2 局部循环

VStack/HStack.arrange 在一个调用内执行：

```text
使用当前内在尺寸解析 Stack frame
  -> 按顺序 arrange children
  -> 根据普通流 children 重新计算 Auto 内在尺寸
  -> 比较 Stack frame、content 区域及 children 外部尺寸
       ├─ 未变化：本次 arrange 完成
       └─ 有变化：使用新结果重新执行本 Stack
```

嵌套 Stack 在 child.arrange 返回前已经完成自身局部循环，因此外层无需公共 measure 或
全局 Reflow API。外层每一轮都读取 child 当前最终 frame；后续 sibling 使用更新后的主轴
游标，不依赖 child 的上一帧尺寸。

普通稳定布局通常只执行一次 children 排列。Auto 初次解析、Text 换行或父区域变化时可能
产生额外轮次；只有布局相关值真实改变才继续。

### 7.3 收敛保护

Auto 与百分比可以形成循环依赖，例如 Auto parent 的尺寸由百分比 child 决定，而 child
又以 parent 当前尺寸为基数。Stack 必须以整数 frame 结果是否稳定作为收敛条件，并设置
统一的最大局部 Reflow 次数；超过上限时抛出明确的 `tui/layout/reflow` 错误，不能无限
递归、静默使用未稳定结果或依赖上一帧偶然状态。

最大次数是通用安全边界，不针对具体 Widget；其具体常量在实现前由人工 Review 确认。

跨帧 dirty tree、`requestReflow()` 和 `doArrange()` 的共同语义以
`docs/engineering/feng-std-tui-view-dev.md` 为准。Stack 可以从自身和 children 已有 frame
取得下一轮的初始内在尺寸估计，以减少稳定界面的额外 Reflow；组件修改 Style/content
后必须按共同机制标记 `Layout` 并通知父级路径。

## 8 绘制、裁剪与命中

VStack/HStack.draw 按以下顺序执行：

1. 使用 View 的通用逻辑处理自身背景、drawFrame 和 sequence 登记；
2. 按 children 的 List 顺序调用每个 child.draw(manager)；
3. child 自行决定是否以及如何绘制自身后代。

Stack 不根据可见区域跳过 child.draw。第一版保持 drawFrame、透明背景、祖先 Hidden 裁剪
和 sequence 顺序完全复用现有逻辑；虚拟化由未来专用容器实现。

透明 Stack 仍按现有 Widget 规则登记 sequence。是否需要透明容器穿透命中属于独立事件
策略，不在本阶段改变。

## 9 性能与实现约束

- 不增加公共 measure 阶段，不改变 Widget.arrange 签名；
- ArrangeSlot 为固定大小值，不产生每 child 堆分配；
- Stack 直接遍历既有 children List，不创建临时 child List、slot 对象或排序副本；
- 正常稳定布局保持 O(n)，额外遍历只在本 Stack 的布局结果真实变化时发生；
- Stack 可复用自身和 children 已有 frame 作为下轮初值，不增加独立缓存对象；
- draw 保持 O(n)，不增加颜色合成处理；
- 不修改 Cell、Buffer、Screen、InputManager、runtime 或编译器 ABI；
- 不按 Text、Button 等具体类型分支，所有 child 仅通过 Widget 契约参与布局；
- 不修改用户 Style，不通过伪造 parent 或递归平移子树完成排列。

ArrangeSlot 会给 Widget 增加固定大小布局状态，这是保持 arrange 签名且让父容器安全分配
child 槽位所需的最小公共改动。该内存变化需在实现前由人工 Review 确认。

## 10 测试范围

新增 std_test 用例覆盖：

- ArrangeSlot 仅在单次 child.arrange 内有效，异常时也正确清理；
- 无 ArrangeSlot 的 View/Text 保持现有 arrange 结果；
- 空 VStack/HStack、单 child 和多个 child；
- VStack 主轴高度累加、交叉轴最大宽度及最终坐标；
- HStack 主轴宽度累加、交叉轴最大高度及最终坐标；
- 固定、百分比、Auto 尺寸与交叉轴 Start/Center/End/Full；
- 主轴 Full 不拉伸且交叉轴 Full 拉伸；
- Stack padding、child margin 和不折叠 margin；
- Normal/Relative 参与普通流，Absolute/Fixed 不占用流空间；
- Absolute 使用最近非 Normal 祖先、Fixed 使用 Screen 的既有定位行为；
- 嵌套 VStack/HStack 以及 Text 自动换行、Auto 高度向外层传播；
- child 改变 parent、parent 再改变其他 children 的多轮 Reflow；
- Auto 与百分比依赖的稳定结果及超过 Reflow 上限的错误；
- children 超出 Stack、overflow Hidden 和 Screen 边界裁剪；
- draw 顺序、sequence 顺序、drawFrame 和逆序鼠标命中；
- content、Style 和 Screen 尺寸改变后的重新布局；
- 布局前后 Style、parent 和 children 保持不变；
- std、std_test 和 tui_demo 定向构建；
- `make test` 全量回归。

测试只新增相关用例；如确需调整已有测试，先获得人工批准。仅 docs 变更不执行测试；开始
实现后，按仓库规范在非 Codex 沙箱环境执行 `make test`。

## 11 实施 TODO

- [ ] 11.1 人工 Review 并确认本文档，特别确认 ArrangeSlot、主轴 Full 和局部 Reflow；
- [ ] 11.2 增加 ArrangeSlot 及 Widget 瞬时 arrangeSlot 状态，不改变 arrange 签名；
- [ ] 11.3 让 View 通用辅助方法解析 ArrangeSlot，Text 自然复用，并补充既有布局兼容用例；
- [ ] 11.4 定义 VStackWidget/HStackWidget 和 VStack/HStack；
- [ ] 11.5 实现共享的轴向排列与局部 Reflow 算法；
- [ ] 11.6 实现 children 绘制、sequence、裁剪和命中集成；
- [ ] 11.7 补齐嵌套布局、Text Auto、脱离普通流和 Reflow 用例；
- [ ] 11.8 更新 tui_demo，增加可人工验证的嵌套 VStack/HStack；
- [ ] 11.9 构建 std、std_test 和 tui_demo，处理定向验证发现的问题；
- [ ] 11.10 在非 Codex 沙箱环境执行 `make test` 全量回归；
- [ ] 11.11 根据实现结果更新相关文档 TODO，等待人工 Review。
