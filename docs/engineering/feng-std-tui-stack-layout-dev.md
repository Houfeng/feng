# Feng TUI VStack/HStack 开发方案

> 状态：已实施，待人工 Review。
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 中轴向布局容器阶段的专项开发文档。
> Widget、Style、frame、组件树及布局脏标记的共同契约以
> `docs/engineering/feng-std-tui-view-dev.md` 为准。

## 1 目标与范围

本阶段实现 `VStack` 和 `HStack`：

- 按 children 的 List 顺序沿各自主轴排列直接子组件；
- 轴向容器直接决定普通流 child 的最终 `frame.x/frame.y`；
- 轴向容器自主调度直接 children 的布局和绘制，ViewManager 仍只调度 root；
- 复用现有 `DirtyMark`、`requestReflow()`、`doArrange()` 和 `doDraw()`；
- 支持固定、百分比和 Auto 尺寸，以及 padding、margin 和交叉轴 Align；
- Normal/Relative 参与普通流，Absolute/Fixed 脱离普通流；
- 保持现有裁剪、绘制顺序、命中和事件冒泡契约。

本阶段不实现公共 measure 阶段、ArrangeSlot、全局布局重构、spacing、权重分配、
Flex、Grid、Dock、ScrollView、虚拟滚动或其他 Widget。

## 2 层级职责

VStack/HStack 只处理自己的直接 children，不跨级遍历或修改 descendants：

- 轴向容器调用每个直接 child 的 `doArrange(manager)`；
- child 自行计算尺寸和完成自身布局；
- 轴向容器根据 child 的最终尺寸写入该直接 child 的 `frame.x/frame.y`；
- 如果直接 child 本身是容器，由该 child 自行处理自己的直接 children；
- 轴向容器绘制时只按顺序调用直接 child 的 `doDraw(manager)`。

child 不感知 VStack，不增加父布局槽位，不修改 `Widget.arrange()` 的参数，也不增加公共
measure 接口。

## 3 类型结构

VStack 使用独立 spec，使专用 `@mixable static` 布局方法以自身契约生成实例方法，同时
复用 Container 的组件树和默认阶段调度能力：

```feng
open spec VStackWidget: ContainerWidget {}

open type VStack: VStackWidget {
  ...: Container = Container();

  @mixable
  static func prepareChildOverrideStyle(stack: VStackWidget, child: Widget): void;

  @mixable
  static func arrange(stack: VStackWidget, manager: ViewManager): void;
}
```

VStackWidget 本阶段不增加公开布局属性。

HStack 使用完全对称的独立契约，同样只覆盖 child 样式钩子和专用布局：

```feng
open spec HStackWidget: ContainerWidget {}

open type HStack: HStackWidget {
  ...: Container = Container();

  @mixable
  static func prepareChildOverrideStyle(stack: HStackWidget, child: Widget): void;

  @mixable
  static func arrange(stack: HStackWidget, manager: ViewManager): void;
}
```

HStackWidget 本阶段同样不增加公开布局属性。

## 4 布局规则

### 4.1 自身布局

VStack 复用 View 的尺寸与定位规则计算自身 frame。Auto 内在尺寸来自上一轮已布局的直接
children：

- 内在宽度为普通流 children 外部宽度的最大值加左右 padding；
- 内在高度为普通流 children 外部高度之和加上下 padding；
- 空 VStack 的 Auto 内在宽高为 padding 占用；
- 自身有效尺寸变化时标记 Layout，由既有 `doArrange()` 在本地继续布局，并通过
  `requestReflow()` 通知直接父级。

不增加独立测量阶段。固定尺寸或 Full 的稳定布局只遍历直接 children 一次；Auto 或参照
尺寸变化时由现有布局脏机制完成必要的后续轮次。

VStack 继承 `Container.doStyling()`，只覆盖 `prepareChildOverrideStyle()`：先调用
Container 的默认实现清理 child patch，再通过
`overrideStyle.verticalAlign = Align.Start` 追加主轴覆盖。该覆盖不修改用户 `style`，并使
child 在自身布局阶段自然按声明的 height/Auto 计算高度；VStack 随后只决定普通流 child
的最终 `frame.y`。Absolute/Fixed child 本就忽略 Align，因此同一覆盖不改变其定位语义。

VStack 同时继承 `Container.doArrange()` 的脏子树调度和 `Container.draw()` 的默认 children
绘制，只覆盖 `arrange()` 实现垂直布局。纯 `SubtreeLayout` 不重新布局干净的兄弟子树；
VStack 自身为 Layout 时，专用 arrange 仍完整处理直接 children 和 Auto 收敛。

HStack 复用同一 Container 管线，只把主轴换为水平方向：通过
`overrideStyle.horizontalAlign = Align.Start` 保持 child 声明的 width/Auto，由 HStack
决定最终 `frame.x`；交叉轴继续使用 child 的 `verticalAlign`。HStack 的 Auto 内在宽度是
普通流 children 外部宽度之和加左右 padding，Auto 内在高度是普通流 children 外部高度的
最大值加上下 padding。

### 4.2 content 区域

VStack 从自身 frame 扣除非负 padding 得到 content 区域。padding 百分比继续使用自身
frame 的相应轴作为基数；扣除结果保持非负。

### 4.3 普通流 child

Normal/Relative child 参与普通流：

1. 当前纵向游标从 content.y 开始；
2. 调用 child.doArrange(manager) 获得最终尺寸；
3. child.frame.y 设为游标加非负 margin.top，忽略 child.style.y；
4. child.frame.x 由 content 区域、左右 margin、最终宽度及 horizontalAlign 决定，忽略
   child.style.x；
5. 游标增加 margin.top、child.frame.height 和 margin.bottom。

交叉轴规则保持现有 Align 语义：Start/Full 位于 content 起始侧，Center/End 根据最终宽度
定位；Full 的宽度仍由 child 自身的 View/Text arrange 按现有规则解析。

主轴尺寸保持 child 声明的 height/Auto 解析结果。VStack 不改写用户 style、width 或
height，而是通过运行时 overrideStyle 将 verticalAlign 设为 Start，避免 Full 改写高度；
最终主轴位置仍由 VStack 决定。

### 4.4 浮动 child

Absolute/Fixed child 脱离普通流：

- 仍调用 child.doArrange(manager)；
- VStack 不覆盖其 frame.x/frame.y；
- 不影响纵向游标和 VStack 的 Auto 内在尺寸。

### 4.5 margin

- child margin 以 VStack content 区域的对应尺寸为百分比基数；
- 负 margin 按现有 View 规则归零；
- 相邻 child margin 不折叠；
- child 固定尺寸不扣除 margin，百分比和 Full 尺寸继续使用现有 View 规则。

### 4.6 HStack 普通流规则

HStack 沿水平方向执行与 VStack 对称的排列：

1. 横向游标从 content.x 开始；
2. child.frame.x 设为游标加非负 margin.left，忽略 child.style.x；
3. child.frame.y 根据 content 区域、上下 margin、最终高度及 verticalAlign 决定，忽略
   child.style.y；
4. 游标增加 margin.left、child.frame.width 和 margin.right；
5. Absolute/Fixed child 仍完成自身布局，但不推进横向游标，也不参与 Auto 内在尺寸。

HStack 不修改用户 style、width 或 height；主轴覆盖只写入每轮复用的 overrideStyle。

## 5 绘制与事件

VStack/HStack 都继承 `Container.draw()`：

1. 调用 View.draw 绘制自身背景；
2. 按 children 的 List 顺序调用每个直接 child.doDraw(manager)。

由 `doDraw()` 统一计算和缓存各 Widget 的 clippedFrame、登记 sequence。轴向容器不跨级处理
descendants，不改变祖先 overflow 裁剪、逆序命中或事件冒泡行为。

## 6 性能与约束

- 不新增 Widget 字段、ArrangeSlot、临时集合或每 child 堆分配；
- 每轮只顺序遍历既有直接 children，时间复杂度 O(n)；
- 布局稳定且未标脏时继续由 `doArrange()` 跳过；
- 不修改 Cell、Buffer、Screen、InputManager、runtime 或编译器；
- 不按 Text、Button 等具体 child 类型分支；
- 不修改用户 Style，不处理 grandchildren。

## 7 测试范围

新增 std_test 用例覆盖：

- 空、单 child 和多个 child；
- VStack/HStack 直接覆盖普通流 child 的主轴位置并解析交叉轴位置；
- 固定、百分比和 Auto 尺寸；
- padding、margin、Start/Center/End/Full；
- Normal/Relative 参与普通流，Absolute/Fixed 不占用普通流；
- Auto 内在尺寸和重新布局；
- 直接 children 的绘制、裁剪、sequence 和命中顺序；
- Style、parent 和 children 在布局后保持不变。

HStack 用例按水平方向覆盖与 VStack 对称的固定尺寸、百分比、Auto、padding、margin、
Align、Collapse、浮动 child、Reflow、绘制、裁剪和命中，并增加 VStack/HStack 嵌套验证。

本阶段只修改 std、相关开发文档和新增的 std_test；定向构建并运行
`./build/bin/feng run std/std_test`。发现超出当前范围的问题时暂停并交由人工决策。

人工 Review 阶段在 `examples/tui_demo` 中使用 VStack 组成左侧栏，并嵌套一行 HStack，
用于直接观察两个轴向容器的排列、交叉轴布局和全高背景绘制结果。

## 8 实施 TODO

- [x] 8.1 确认不使用 ArrangeSlot，VStack 只处理直接 children；
- [x] 8.2 定义 VStackWidget 与 VStack；
- [x] 8.3 实现自身 Auto 尺寸和直接 children 垂直排列；
- [x] 8.4 实现直接 children 绘制及既有裁剪/命中集成；
- [x] 8.5 补充 std_test；
- [x] 8.6 构建 std 并运行 std_test；
- [x] 8.7 根据实现结果更新 TODO，等待人工 Review。
- [x] 8.8 在 tui_demo 中增加 VStack 示例并完成定向验证。
- [x] 8.9 基于生产级 Container 调度重构 VStack，只保留 child 样式钩子和专用 arrange。
- [x] 8.10 定义 HStackWidget 与 HStack，只扩展 Container 的 child 样式钩子和 arrange；
- [x] 8.11 实现 HStack 固定、百分比、Auto、margin、padding、Align、浮动和 Collapse 布局；
- [x] 8.12 补充 HStack 及 VStack/HStack 嵌套 std_test；
- [x] 8.13 构建 tui_demo、运行 std_test 并更新实施状态。
