# Feng TUI 视图机制层方案

> 状态：设计中（design）
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 第七阶段（视图机制层）的主规范。
> 当前接口基线以 `std/std/src/tui/view/` 和 `std/std/src/tui/widgets/` 中已经定义的骨架类型为准。

## 1 总体目标

在现有 `TuiApp`、`Screen`、`InputManager` 基础上，引入 retained-mode 的类型化 TUI 组件机制：

```text
TuiApp
  ├─ screen: Screen          # 渲染底座，负责双缓冲与 diff 输出
  ├─ input: InputManager<Widget> # 输入底座，负责终端字节流解析
  └─ view: ViewManager       # 视图机制层，负责组件树、样式/布局/绘制调度与事件路由
```

`Screen`、`InputManager`、`ViewManager` 不互相替代，也不重复创建。三者均为 `TuiApp` 的成员，分别管理画布、基础输入和视图机制。

## 2 第七阶段边界

第七阶段只完善以下机制：

- `Widget`/`ContainerWidget` spec 与 `WidgetStyle` type；
- `WidgetFrame`、`Thickness` 及布局相关枚举；
- `std.tui.widgets.View` 与 `std.tui.widgets.Container` 基础实现；
- `ViewManager` 的组件树、绘制顺序、命中及事件路由；
- `ViewManager` 与 `TuiApp`、`Screen`、`InputManager` 的集成。

第七阶段不完整实现 Text/Button/Input/List/Table/Dialog 等高级组件，也不实现 VStack/HStack/Dock/ScrollView/Grid 等布局容器。`Text` 和 `Button` 可保留最小类型骨架，仅用于验证 `Widget` 契约、`...: View = View()` 成员展开和 `@mixable` wrapper，不在本阶段实现文本绘制或按钮交互。CSS、选择器、级联样式、复杂捕获阶段、透明穿透和复杂 z-index 同样不在本阶段范围内。

叶子组件直接满足 `Widget` spec，容器组件满足 `ContainerWidget` spec；两者分别可通过成员展开复用 `View` 或 `Container` 的状态与行为。上层组装组件树时不需要 `asWidget()` 之类的转换 API。

本阶段事件分发切片实现鼠标命中、目标绑定、锁定、Widget 多播事件触发和自下向上的
冒泡。后续焦点管理、ViewManager 事件与键盘焦点路由已经实施，其新增和调整的契约统一
由 `docs/engineering/feng-std-tui-focus-key-routing-dev.md` 定义。

## 3 已定义的核心类型

### 3.1 布局枚举

`std/std/src/tui/view/Widget.ff` 已定义：

```feng
open enum WidgetPosition {
  Normal,
  Relative,
  Absolute,
  Fixed
}

open enum WidgetAlign {
  Full,
  Start,
  Center,
  End
}

open enum WidgetOverflow {
  Visible,
  Hidden
}
```

- `Normal`：由父组件的排列逻辑定位；
- `Relative`：与 `Normal` 使用相同的普通流布局，但可作为后代 `Absolute` 组件的定位参照；
- `Absolute`：相对最近的非 `Normal` 祖先组件定位；不存在时相对屏幕定位；
- `Fixed`：相对屏幕定位；
- `Full`/`Start`/`Center`/`End`：分别表示填满、起始、居中和末端对齐；
- `Visible`/`Hidden`：分别表示允许显示溢出内容和裁剪溢出内容。滚动由后续 `ScrollView` 实现，不加入 `WidgetOverflow`。

### 3.2 WidgetStyle

`WidgetStyle` 是保存组件布局与基础颜色声明的普通引用类型：

```feng
open type WidgetStyle {
  var position: WidgetPosition;
  var x: Union<int, double>;
  var y: Union<int, double>;
  var width: Union<int, double>;
  var height: Union<int, double>;
  var foreColor: Option<RgbColor>;
  var backColor: Option<RgbColor>;
  var padding: Thickness;
  var margin: Thickness;
  var overflow: WidgetOverflow;
  var visibility: Visibility;
  var pointerEvents: PointerEvents;
  var horizontalAlign: WidgetAlign;
  var verticalAlign: WidgetAlign;
}
```

尺寸和坐标使用 `Union<int, double>`：`int` 表示固定终端单元数，`double` 表示百分比。`x`/`y` 仅用于 `Absolute` 和 `Fixed`，`Normal`、`Relative` 忽略二者。`Absolute` 的相对坐标以最近的非 `Normal` 祖先组件为参照，且不受该祖先组件 `padding` 影响；不存在非 `Normal` 祖先时以屏幕为参照。`Fixed` 始终以屏幕为参照。`Absolute` 和 `Fixed` 忽略 `horizontalAlign` 和 `verticalAlign`。

`foreColor`/`backColor` 为 `none` 时使用终端默认色。`overflow == Hidden` 时裁剪超出组件区域的绘制；滚动能力由后续组件实现。

`visibility` 和 `pointerEvents` 的定义如下：

- `Visibility.Visible`：正常参与布局、绘制和事件路由；
- `Visibility.Hidden`：继续参与布局并保留占位，但不绘制自身及其子树，不参与事件命中；事件沿既有目标向上路由时跳过该 Widget；
- `Visibility.Collapse`：不占位，公共布局调度将最终 `frame.width` 和 `frame.height` 置为 0；绘制与事件行为同 `Hidden`；
- `PointerEvents.All`：正常参与鼠标命中和鼠标事件路由；
- `PointerEvents.None`：仍参与布局和绘制，但自身不作为鼠标命中目标，鼠标事件冒泡时跳过该 Widget。该字段不影响键盘和焦点事件。

`Visibility` 默认值为 `Visible`，`PointerEvents` 默认值为 `All`。`Collapse` 组件的
`margin` 也不占用父布局空间；父布局组件在排列直接 children 时必须排除该 child。

#### 3.2.1 StylePatch

`StylePatch` 是稀疏样式覆盖，只记录明确设置的字段，不改变用户声明的
`Style`。Pseudo 样式和布局强制样式共用该表示，并在 styling 阶段按优先级合并。

`StylePatch` 与 `Style` 保持同一组字段，每个字段均使用
`Option<T>` 表示是否参与本层覆盖；`none` 表示不覆盖该字段。`clear()` 将全部字段恢复为
`none`，供框架逐轮复用同一个 Patch 对象。

`Style` 提供 `clear()` 和两个 `apply()` 重载。`clear()` 将实例恢复为默认样式；
`apply(Style)` 应用完整样式，`apply(StylePatch)` 只应用 Patch 中存在的字段。
两个 `apply()` 在比较和赋值的同时返回 `StyleApplyResult`。该 `@value` 类型通过一个
内部整数按位记录 `StyleChangeType.Layout` 和 `StyleChangeType.Draw`，可同时表达两类变化。
`Visibility` 在进入或离开 `Collapse` 时同时产生 Layout 和 Draw 变化，在 `Visible` 与
`Hidden` 之间切换时只产生 Draw 变化；`PointerEvents` 不改变布局或绘制结果，因此不产生
这两类变化，但仍同步写入最终 `rtStyle`。

### 3.3 Thickness

`Thickness` 表示矩形四边的间距，供 `padding`、`margin` 及后续边框使用：

```feng
open type Thickness {
  let top: Union<int, double>;
  let right: Union<int, double>;
  let bottom: Union<int, double>;
  let left: Union<int, double>;
}
```

当前提供六种构造形式：四边值、垂直/水平值和统一值，每种形式分别支持全部使用 `int` 或全部使用 `double`。

### 3.4 WidgetFrame

`WidgetFrame` 是 `@value` 矩形类型，同时用于保存布局结果和本帧实际绘制区域：

```feng
@value
open type WidgetFrame {
  var x: int;
  var y: int;
  var width: int;
  var height: int;
}
```

用户声明值保存在 `WidgetStyle` 中；`arrange()` 将布局结果写入 `frame`，`draw()` 将
经过祖先和 Screen 裁剪的本帧实际绘制区域写入 `clippedFrame`。事件命中只读取
`clippedFrame`，不在事件阶段重新遍历祖先或解析当前样式。

### 3.5 布局脏标记

`DirtyType` 使用独立枚举定义合法的脏状态类型，`DirtyMark` 使用一个内部整数按位保存多个
状态。Widget 只持有一个 `dirty` 字段；未来增加绘制等其他脏状态时扩展 `DirtyType`，不再
增加 Widget 字段。

```feng
open enum DirtyType {
  Layout = 1,
  SubtreeLayout = 2
}

@value
open type DirtyMark {
  seal var value: int;

  open func DirtyMark(): void;
  open func DirtyMark(first: DirtyType, rest: DirtyType...): void;
  open func has(dirtyType: DirtyType): bool;
  open func add(dirtyType: DirtyType): void;
  open func remove(dirtyType: DirtyType): void;
  open func clear(): void;
  open func isEmpty(): bool;
}
```

- 无参构造创建空标记；变参构造至少接受一个 `DirtyType`，重复项不影响结果；
- `has`、`add` 和 `remove` 始终接受 `DirtyType`，不向外暴露内部整数编码；
- `DirtyMark` 是 `@value` 类型，复制后各副本独立修改；
- `View.dirty` 初始包含 `Layout` 和 `SubtreeLayout`，保证新组件及其初始子树进入后续布局；
- 组件直接通过 `widget.dirty.add(DirtyType.Layout)` 标记自身需要重新布局，不增加语义
  重复的 `requestArrange()` 或 `markLayoutDirty()`；
- `requestReflow()` 将直接父级标记为 `Layout`，并将更高祖先标记为
  `SubtreeLayout`。只有直接父级需要重新布局，更高祖先的标记仅用于让调度进入对应子树；
- `doArrange()` 消费这两个布局标记并统一调度 `arrange()`，不增加独立的
  `reflowRequested` 状态字段；
- 绘制脏标记及具体布局容器仍由后续阶段实现。

## 4 Widget 契约

`Widget` 不是继承基类，而是组件参与视图树的能力契约。组件多态通过 spec 实现，代码复用通过组合实现。

```feng
open spec Widget {
  let style: Style;
  let draftStyle: Style;
  let rtStyle: Style;
  let overrideStyle: StylePatch;
  var dirty: DirtyMark;
  var frame: Rect;
  var clippedFrame: Rect;
  var parent: Option<ContainerWidget>;

  func requestReflow(): void;
  func doStyling(manager: ViewManager): void;
  func doArrange(manager: ViewManager): void;
  func arrange(manager: ViewManager): void;
  func doDraw(manager: ViewManager): void;
  func draw(manager: ViewManager): void;
  func isAncestor(w: Widget): bool;

  let onFocus: Event<FocusEvent<Widget>>;
  let onBlur: Event<FocusEvent<Widget>>;
  let key: Event<KeyEvent<Widget>>;
  let mouseDown: Event<MouseEvent<Widget>>;
  let mouseMove: Event<MouseEvent<Widget>>;
  let mouseUp: Event<MouseEvent<Widget>>;
  let wheel: Event<MouseEvent<Widget>>;
}
```

其中：

- `style` 是引用不可重新绑定的用户样式；
- `draftStyle` 是每轮完整合并样式时复用的草稿，`rtStyle` 是布局与绘制读取的最终结果；
- `overrideStyle` 是框架和父布局提供的最高优先级稀疏覆盖；
- `dirty` 保存当前组件的组合脏状态，基础 View 初始包含 `Layout` 和 `SubtreeLayout`；
- `frame` 是 `@value` 布局结果，由 `arrange()` 整体写回；
- `clippedFrame` 是 `@value` 绘制快照，由 `doDraw()` 计算并在登记 sequence 时整体写回；
- `parent` 只能是容器组件，根组件的 `parent` 为 `none`；
- `requestReflow()` 只让直接父级进入 `Layout`，更高祖先只进入
  `SubtreeLayout`；根组件调用时静默返回；
- `doStyling()` 清理并合并 `draftStyle`，最后通过 `rtStyle.apply(draftStyle)` 的结果决定
  是否增加 `Layout` 并请求父级 Reflow；
- `doArrange()` 是框架布局调度入口，ViewManager 和容器通过它进入组件布局；
- `doDraw()` 是框架绘制调度入口，统一处理裁剪、`clippedFrame` 缓存和 sequence 登记；
- `arrange`、`draw` 使用 `func` 定义，不是可由外部替换的回调字段；
- `isAncestor(w)` 判断当前组件是否为 `w` 的祖先，不把自身视为自身的祖先；
- 键盘和鼠标处理使用不可重新绑定的多播事件字段，监听器通过 `on()`/`off()` 管理；
- 第七阶段不引入独立 `ViewNode`，`Widget` 自身就是组件树节点。

## 5 View 基础组件

基础组件定义在 `std.tui.widgets` 子模块中：

```feng
open type View: Widget {
  let style: Style;
  let draftStyle: Style;
  let rtStyle: Style;
  let overrideStyle: StylePatch;
  var dirty: DirtyMark = DirtyMark(DirtyType.Layout, DirtyType.SubtreeLayout);
  var frame: Rect;
  var clippedFrame: Rect;
  var parent: Option<ContainerWidget>;

  func doStyling(manager: ViewManager): void;
  func doDraw(manager: ViewManager): void;
  func arrange(manager: ViewManager): void;
  func draw(manager: ViewManager): void;
  func isAncestor(w: Widget): bool;

  let onFocus: Event<FocusEvent<Widget>> = Event<FocusEvent<Widget>>();
  let onBlur: Event<FocusEvent<Widget>> = Event<FocusEvent<Widget>>();
  let key: Event<KeyEvent<Widget>> = Event<KeyEvent<Widget>>();
  let mouseDown: Event<MouseEvent<Widget>> = Event<MouseEvent<Widget>>();
  let mouseMove: Event<MouseEvent<Widget>> = Event<MouseEvent<Widget>>();
  let mouseUp: Event<MouseEvent<Widget>> = Event<MouseEvent<Widget>>();
  let wheel: Event<MouseEvent<Widget>> = Event<MouseEvent<Widget>>();
}
```

`View` 通过 `@mixable` 静态方法向展开目标提供默认实例行为。`View.doStyling()` 按
`style`、后续状态 Patch、`overrideStyle` 的顺序生成 `draftStyle`，最后只根据
`draftStyle -> rtStyle` 的最终变化标记布局。`View.arrange()` 和 `View.draw()` 只读取
`rtStyle`。`View.doDraw()` 计算有效绘制区域，缓存 `clippedFrame`，排除空区域并通过
`ViewManager.trace()` 登记绘制顺序，然后调用组件自己的 `draw()`；组件及容器不再自行感知 sequence 跟踪。`View.draw()` 使用已缓存的 `clippedFrame` 及 `backColor` 在 Screen back buffer 中填充当前组件的空白矩形，前景色使用终端默认色。`foreColor` 由后续实际绘制字符的组件使用。`View.draw()` 不遍历子组件，不绘制文本或边框。有效绘制区域是自身 `frame`、Screen 与最近一个 `overflow == Hidden` 祖先组件 `frame` 的交集；不存在这样的祖先时只与 Screen 求交。

`View.isAncestor(w)` 使用循环沿 `w.parent` 向上查找，不使用递归。后续组件可以展开 `View` 复用公共状态与默认行为，也可以直接实现 `Widget` spec。复用 `View` 状态的组件使用 `...: View = View();`，通过普通来源构造语义完整初始化 `Event<T>` 等字段；`...: View;` 只展开定义并执行字段类型的默认零值初始化，不适用于这些需要执行 `View` 字段初始化器的组件。

## 6 ContainerWidget 与 Container

`Widget` 不定义子组件能力。只有容器组件满足 `ContainerWidget` spec：

```feng
open spec ContainerWidget: Widget {
  let children: List<Widget>;

  func addChild(child: Widget): void;
  func removeChild(child: Widget): void;
  func clearChildren(): void;
}
```

基础实现定义在 `std.tui.widgets` 子模块中：

```feng
open type Container: ContainerWidget {
  ...: View = View();

  let children: List<Widget>;
}
```

`Container` 构造并展开 `View` 的公共状态与 Widget 默认行为，并通过以 `ContainerWidget` 为首参数的 `@mixable` 静态方法实现 `addChild`、`removeChild` 和 `clearChildren`。`Container.doStyling()` 默认合并自身，再逐个清理直接 child 的 `overrideStyle` 并调用其 `doStyling()`；具体容器仍可定义同签名 `@mixable` 方法改变处理范围和顺序。`Container` 不默认布局或绘制 children。

## 7 styling、arrange 与 draw

每轮渲染分为固定的三个阶段：

```text
styling 阶段
  Widget.doStyling(manager)
  style + StylePatch -> draftStyle -> rtStyle

arrange 阶段
  Widget.doArrange(manager)
    -> Widget.arrange(manager)
  将 WidgetStyle 解析为 WidgetFrame

draw 阶段
  Widget.doDraw(manager)
    -> 计算并缓存 clippedFrame
    -> 登记到 ViewManager.sequence
    -> Widget.draw(manager)
  根据 clippedFrame 和非尺寸样式绘制
```

`ViewManager.doStyling()` 为 root 写入强制铺满 Screen 的 `overrideStyle`，再调用
`root.doStyling()`。`ViewManager.doArrange()` 只调用 `root.doArrange()`，`ViewManager.doDraw()` 只调用 root 的
`doDraw()`。每个组件自行决定是否、何时以及按什么顺序调用 children 的
`doArrange()` 和 `doDraw()`；例如虚拟滚动容器可以只布局和绘制当前可见的子组件。

`doArrange()` 在 `Layout` 和 `SubtreeLayout` 都不存在时跳过当前子树；否则先移除本轮
消费的两个标记，再调用一次组件自己的 `arrange()`。如果布局过程中子组件通过
`requestReflow()` 重新为当前组件添加了 `Layout`，`doArrange()` 继续调用
`arrange()`，直到当前组件不再请求重排。只重新出现 `SubtreeLayout` 时不重复当前组件，
本轮自治的 children 布局完成后由 `doArrange()` 一并消费该路径标记。

当最终 `rtStyle.visibility == Visibility.Collapse` 时，公共 `doArrange()` 不调用组件自己的
`arrange()`，直接将 `frame.width` 和 `frame.height` 置为 0 并消费当前布局标记。父布局在
排列 children 时还必须跳过该 child 的 margin，保证 Collapse 完全不占位。

组件在布局阶段之外修改自身布局输入时，先直接添加 `Layout`，再调用
`requestReflow()` 通知父级路径。布局阶段内，子组件的最终占位改变时直接调用
`requestReflow()`；父级重新布局后，只有父级自身占位也改变时才继续向上请求。

`arrange` 和 `draw` 都接收同一个 `ViewManager`。组件通过 manager 获取本轮调度所需的视图上下文。只有实际进入 `doDraw()`、具有非空有效绘制区域并完成 sequence 登记的组件才参与本帧鼠标命中。

### 7.1 Normal 与 Relative 排列

`position == Normal` 或 `Relative` 时，组件根据直接父组件 content 区域计算最终区域；父组件 content 区域是父组件 `frame` 扣除父组件 `padding` 后的区域。无 parent 的根组件以整个 Screen 为参照区域。`x`/`y` 不参与 `Normal` 或 `Relative` 布局。两者的自身布局行为一致，区别仅在于 `Relative` 可以成为后代 `Absolute` 组件的定位参照，而 `Normal` 会被查找过程跳过。

每个轴独立按以下顺序计算：

1. 百分比 margin 以该轴的参照区域尺寸为基数，先解析自身两侧 margin；
2. 从参照区域扣除两侧 margin，得到该轴的可用区域；
3. `Full` 忽略该轴的 `width`/`height` 并占满可用区域；
4. `Start`、`Center`、`End` 根据 `width`/`height` 计算尺寸，并在可用区域的起始、中间或末端定位；
5. 百分比 `width`/`height` 以扣除自身 margin 后的可用区域为基数，转换为 `int` 时向零截断。

`padding` 不改变组件自身的 `frame`，由具体容器在计算其 children 的参照区域时使用。组件自行决定是否及如何触发 children 的布局，`ViewManager` 不参与子组件布局。

### 7.2 Absolute 与 Fixed

- `Absolute` 沿 parent 链查找最近的非 `Normal` 祖先组件，并使用 `x`/`y` 相对该祖先组件的 `frame` 定位；`Relative`、`Absolute` 和 `Fixed` 祖先均可成为参照；
- `Absolute` 不受定位参照组件的 `padding` 影响；不存在非 `Normal` 祖先组件时以屏幕为参照；
- `Fixed` 使用 `x`/`y` 相对屏幕定位；
- 两者均忽略 `horizontalAlign` 和 `verticalAlign`；
- `width`/`height` 仍支持固定值或百分比；
- 自身 margin 先从参照区域扣除，百分比 `width`/`height` 再以剩余区域为基数；
- `x`/`y` 的百分比仍以未扣除自身 margin 的参照区域为基数，最终位置叠加起始侧 margin；
- 裁剪不改变 arrange 产生的 `frame`，只在 `draw` 阶段计算有效绘制区域。

### 7.3 绘制裁剪

`View.doDraw()` 在自身或任一祖先不是 `Visibility.Visible` 时，将 `clippedFrame` 置为空且不调用
`draw()`，从而统一截断该组件子树的绘制。可见组件再查找离自身最近的
`overflow == Hidden` 祖先组件。自身 `frame` 与该祖先组件本帧已经缓存的
`clippedFrame` 求交，得到本次有效绘制区域；该祖先的快照已经包含更高层 Hidden 祖先和
Screen 的剪裁。不存在 Hidden 祖先时，自身 `frame` 直接与 Screen 求交。该规则依赖既有的
祖先 `doDraw()` 先缓存自身快照、再调度 descendants 的绘制顺序。裁剪结果不写回布局
`frame`，而是在登记 sequence 时写入 `clippedFrame`，作为本帧绘制与后续鼠标命中的共同
快照。组件的 `draw()` 直接使用该快照，不重复计算裁剪或登记 sequence。

裁剪使用完整矩形求交，同时计算裁剪后的 `x`、`y`、`width` 和 `height`，不使用只能处理 Screen 右侧或下侧边界的单轴长度计算。有效区域的任一尺寸为 0 时不写入 Buffer，也不登记到 `sequence`；此时旧 `clippedFrame` 即使仍存在，也因组件不在本帧 sequence 中而不会参与命中。

## 8 ViewManager 与 sequence

`ViewManager` 当前已定义 root 调度与绘制顺序集合：

```feng
open type ViewManager {
  seal let screen: Screen;
  seal let sequence: List<Widget>;
  open var root: Option<Widget>;

  open func getScreenWidth(): u32;
  open func getScreenHeight(): u32;
  open func getScreenBuffer(): Buffer;
  open func trace(widget: Widget, clippedFrame: WidgetFrame): void;
  open func doStyling(): void;
  open func doArrange(): void;
  open func doDraw(): void;
}
```

`ViewManager` 通过构造函数接收已有 `Screen`，不创建新的 Screen。`getScreenWidth()` 和 `getScreenHeight()` 为 `View.arrange()` 提供无 parent 组件及 `Fixed` 组件的布局参照；`getScreenBuffer()` 返回当前 Screen 的 back buffer。组件不缓存该 Buffer，避免 Screen resize 后继续使用旧引用。

`root` 默认为 `none`。`doStyling()` 在存在 root 时先清理其 `overrideStyle`，强制写入
Screen 原点、宽高和零 margin，再从 root 开始合并组件树样式。`doArrange()` 在无 root 时不做处理，存在 root 时调用
`root.doArrange()`。`doDraw()` 每轮先清空 `sequence`；存在 root 时再清空 Screen back
buffer 并从 root 开始绘制，使组件移动或缩小后不会留下旧帧内容。无 root 时不清空
back buffer，保留现有直接通过 Screen 绘制的使用方式。逐帧只清空
`screen.buffer()`，不调用同时清空 front/back 的 `Screen.clear()`，以保留正确的 diff
基准。

`sequence` 是 `ViewManager` 的内部成员，不对上层公开。每轮绘制开始时清空；`Widget.doDraw()` 完成有效绘制区域计算后调用 `trace(widget, clippedFrame)`，该方法先将区域写入 `widget.clippedFrame`，再将组件登记到 `sequence`，保证绘制快照与顺序登记不会分离。组件自己的 `draw()` 不感知 sequence。越靠后的组件实际绘制层级越高，容器对 children 的 `doDraw()` 调用顺序同时决定子组件在 sequence 中的顺序。

鼠标命中时从 `sequence` 末尾向前查找，并直接使用每个组件缓存的 `clippedFrame` 判断事件坐标；
不可见或 `pointerEvents == PointerEvents.None` 的组件不能成为目标，查找继续向下层组件进行。
找到的第一个可响应组件即为目标。鼠标事件沿 parent 冒泡时，逐节点跳过不可见或
`PointerEvents.None` 的 Widget，但继续向其父级路由；正常到达 root 后仍触发
ViewManager 对应事件。键盘事件冒泡只跳过不可见 Widget，不受 `PointerEvents` 影响。
锁定目标同样遵循该规则；若其祖先在锁定期间变为不可见，隐藏子树内的目标和祖先均被
跳过，事件从隐藏边界之外的首个可见祖先继续冒泡。

`clippedFrame` 表示用户当前看到的上一帧区域。即使布局、样式或组件树在绘制后发生修改，事件阶段也不重新计算命中区域；下一次 draw 会更新 sequence 与 `clippedFrame`。这既保持命中与实际画面一致，也避免 1003 鼠标移动事件中反复遍历祖先。

当前 `ViewManager` 已提供可选 root 的 doStyling/doArrange/doDraw 入口、back buffer 访问、sequence
登记、逆序命中及鼠标事件路由，并持有由 `TuiApp` 传入的 Screen。焦点与键盘路由是在
本阶段基础上的后续扩展，以 `docs/engineering/feng-std-tui-focus-key-routing-dev.md` 为准。

## 9 组件树与 parent

组件树由 `ContainerWidget.children: List<Widget>` 与 `Widget.parent` 表达，不增加 `WidgetChildren` 或 `ViewNode`。叶子 Widget 不具有 children 或组件树修改 API。

`Container.addChild(container, child)` 按以下顺序修改树：

1. 若 `child == container` 或 `child.isAncestor(container)`，则抛出 `"tui/widget/cycle"`，且不修改现有树；
2. 若 child 已有 parent，先调用旧 parent 的 `removeChild(child)`；
3. 将 child 追加到当前 children 末尾；
4. 将 `child.parent` 设为当前容器。

因此，向新容器添加已归属的 child 会自动迁移；向同一容器重复添加 child 会先移除再追加，从而把 child 移动到 children 末尾。

`removeChild(child)` 为幂等操作：找到 child 时将其从 children 移除并把 `child.parent` 设为 `none`；child 不存在或已被删除时静默返回。`clearChildren()` 先将所有直接 child 的 parent 设为 `none`，再清空 children。

`Widget.isAncestor(w)` 沿 `w.parent` 非递归向上遍历，用于在树变更前检查循环。循环检查必须在旧 parent 移除之前完成，保证失败时原树不变。

在 `spec` 的 `seal` 成员能力落地前，`children` 暂时直接使用公开 `List<Widget>`，`parent` 也保持当前可写契约。标准树操作会同步维护双向关系，但调用者仍可绕过它们直接修改 List 或 parent；这是当前阶段明确接受的限制。后续收紧底层存储访问时，保持 `addChild`、`removeChild` 和 `clearChildren` 的公开 API 不变。

## 10 事件接口

`Widget` 使用以自身契约为目标类型的 `Event<T>` 多播事件：

- `key: Event<KeyEvent<Widget>>`；
- `onFocus`、`onBlur: Event<FocusEvent<Widget>>`；
- `mouseDown`、`mouseMove`、`mouseUp`、`wheel: Event<MouseEvent<Widget>>`。

各事件字段不可重新绑定。调用方通过 `on(listener)` 和 `off(listener)` 订阅或退订；
`ViewManager` 通过 `emit(event)` 按注册顺序触发当前 Widget 的全部监听器。
`Event<T>` 是共享监听器列表的值类型，因此通过 `Widget` spec 读取事件字段后进行订阅，
仍会修改该 Widget 所持有的同一监听器列表。

`KeyEvent<T>`、`MouseEvent<T>` 的事件载荷和基础接口由
`docs/engineering/feng-std-tui-input-dev.md` 定义。`MouseEvent<T>` 是引用类型，
传播状态由 `stop()` 和 `isStopped()` 管理。当前 Widget 的 `emit()` 会先完成其全部
监听器调用，随后 `ViewManager` 检查 `isStopped()`；停止状态只阻止后续父组件事件，
不会中断当前 Widget 尚未执行的其他监听器。重复调用 `stop()` 保持停止状态。

`FocusEvent<T>` 与 `onFocus`/`onBlur` 的路径差分、共同祖先和不可停止规则由
`docs/engineering/feng-std-tui-focus-key-routing-dev.md` 统一定义。

鼠标事件按以下规则分发：

1. 若 `MouseEvent<Widget>` 已有锁定目标，直接选择该目标，不执行坐标命中；否则从
   `sequence` 末尾向前查找第一个 `clippedFrame` 包含事件坐标的 Widget；
2. 普通命中使用 `bindTarget()`，锁定路由使用 `bindLockedTarget()`，将选中的 Widget
   写入同一事件实例的 `target`，并由后者记录当前事件属于锁定目标。Widget 回调中
   `hasTarget()` 必为 true；事件冒泡期间 `target` 保持不变，
   表示最初命中或锁定的事件来源，不随当前接收回调的 Widget 改变；
3. `wheelUp`/`wheelDown` 触发 `wheel.emit(event)`，其余事件按 `press`、`move`、
   `release` 分别触发 `mouseDown`、`mouseMove`、`mouseUp`；
4. 当前 Widget 的全部事件监听器返回后检查 `event.isStopped()`；已停止则结束 Widget
   传播，否则沿 `parent` 继续向上传递；
5. ViewManager 事件、未命中路径、`preventDefault()` 与自动聚焦属于后续焦点路由扩展，
   统一以 `docs/engineering/feng-std-tui-focus-key-routing-dev.md` 为准；
6. 当前不定义捕获阶段、`currentTarget`、透明穿透、事件克隆或事件池化。

Widget 层事件使用 `Event<T>` 多播；`InputManager<Widget>.onMouse` 仍保持
`Action<MouseEvent<Widget>>` 单播，只负责把解析后的输入直接交给 ViewManager。
两层职责不合并：InputManager 不维护订阅集合，ViewManager 不改变输入解析回调模型。

`MouseEvent<Widget>.lock()` 由当前 `target` 请求锁定。首个目标取得锁定后，后续鼠标
事件即使移出该 Widget 的 `clippedFrame`，仍从锁定目标开始分发并沿其当前 parent 链冒泡。
同一目标重复 lock 成功，其他目标不能抢占；`unlock()` 仅允许取得锁定的事件，或由
锁定路由绑定到该目标的后续事件释放。
锁定状态保存在闭合事件类型 `MouseEvent<Widget>` 的静态字段中，不放入 ViewManager，
不记录鼠标按钮，也不在 release 时自动解除。调用方必须显式 unlock；这允许拖动等交互
按自身状态决定结束时机。

## 11 与 TuiApp/InputManager 的集成

第七阶段完成后，`TuiApp` 持有：

```feng
open type TuiApp {
  let screen: Screen;
  let input: InputManager<Widget>;
  let view: ViewManager;
}
```

`ViewManager` 使用 `InputManager<Widget>` 产生的 `MouseEvent<Widget>` 进行视图事件
路由，并通过
`Screen` 完成组件绘制。`Screen` 和 `InputManager` 不在视图层重建。

`TuiApp.render()` 处理 resize 后依次调用 `view.doStyling()`、`view.doArrange()` 和
`view.doDraw()`，再通过 `Screen.buildPatchBytes()` 生成终端输出。无 root 时三个步骤不改变 Screen back buffer，
现有直接绘制 Screen 的代码保持有效。

本阶段集成最初只将 `InputManager.onMouse` 单播回调绑定到 ViewManager。焦点与键盘路由
实施后，`TuiApp` 同时接管 `InputManager.onKey`，先调用 `ViewManager.dispatchKey()`，再
触发应用级 `TuiApp.key`。当前完整规则以
`docs/engineering/feng-std-tui-focus-key-routing-dev.md` 为准。

## 12 文件规划

```text
std/std/src/tui/
  TuiApp.ff          # 组装 Screen、InputManager 和 ViewManager

std/std/src/tui/view/
  Thickness.ff       # 四边间距类型（已定义）
  Widget.ff          # 布局枚举、WidgetStyle、WidgetFrame、Widget、ContainerWidget
  ViewManager.ff     # 视图管理器、Screen 引用与 sequence

std/std/src/tui/widgets/
  View.ff            # Widget 基础实现（已定义骨架）
  Container.ff       # ContainerWidget 基础实现
  Text.ff            # 仅用于验证 View 展开，不完整实现
  Button.ff          # 仅用于验证 View 展开，不完整实现
```

第七阶段只保留 Text/Button 的契约与成员展开验证骨架，完整组件行为留到后续阶段。

## 13 实施步骤

1. 已完成 `Widget`/`ContainerWidget`、`View`/`Container`、组件树和 `ViewManager.sequence` 基础机制；
2. 已完成 `View.arrange()`、`View.draw()`、root 调度及 TuiApp 渲染集成；
3. 已将 `MouseEvent` 改为引用类型，增加 `stop()` 与 `isStopped()`；
4. 在 draw 阶段缓存 `clippedFrame`，实现基于该快照的 sequence 逆序命中；
5. 实现鼠标回调选择、自下向上冒泡及停止传播；
6. 本阶段先将 `InputManager.onMouse` 单播回调接入 ViewManager，当时未接管 `onKey`；
7. 将 KeyEvent、MouseEvent 与 InputManager 泛型化，通过 `T` 表达路由目标类型；
8. 为 MouseEvent 增加 target、lock()/unlock()/isLocked()，并让 ViewManager 优先向
   锁定目标路由；
9. 补充 MouseEvent、target、lock、命中、裁剪、层级、冒泡和停止传播的 std_test 用例；
10. 已将 Widget/View 的键盘和鼠标回调字段改为 `Event<T>`，由 ViewManager 触发鼠标事件；
11. 已执行全量回归测试 `make test`；
12. 已完成人工 Review；
13. 已按 `docs/engineering/feng-std-tui-focus-key-routing-dev.md` 实现焦点与键盘路由。
14. 为 Widget 增加组合式 `DirtyMark` 状态；
15. 增加 `requestReflow()` 和 `doArrange()`，接入直接父级 Reflow、祖先子树标记、
    clean 子树跳过及 ViewManager root 调度。
16. 增加 `doDraw()`，统一处理绘制裁剪、`clippedFrame` 缓存及 sequence 登记；保持完整帧
    绘制，不增加 Draw dirty。
17. 为 Style/StylePatch 增加 `Visibility` 和 `PointerEvents`，接入 Collapse 零占位、
    Hidden 绘制截断、鼠标命中过滤及鼠标/键盘冒泡跳过规则。

## 14 Review 关注点

现有定义尚未确定以下实现契约：

- `WidgetStyle` 的默认值；
- `spec` 的 `seal` 成员落地后，如何在保持公开树操作 API 不变的前提下收紧 children/parent 存储访问；
- Tab 正向/反向焦点切换规则（由后续专项步骤单独设计）。
