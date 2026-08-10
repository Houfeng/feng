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
  ├─ input: InputManager     # 输入底座，负责终端字节流解析
  └─ view: ViewManager       # 视图机制层，负责组件树、arrange/draw 调度与事件路由
```

`Screen`、`InputManager`、`ViewManager` 不互相替代，也不重复创建。三者均为 `TuiApp` 的成员，分别管理画布、基础输入和视图机制。

## 2 第七阶段边界

第七阶段只完善以下机制：

- `Widget`/`ContainerWidget` spec 与 `WidgetStyle` type；
- `WidgetFrame`、`Thickness` 及布局相关枚举；
- `std.tui.widgets.View` 与 `std.tui.widgets.Container` 基础实现；
- `ViewManager` 的组件树、绘制顺序、命中及事件路由；
- `ViewManager` 与 `TuiApp`、`Screen`、`InputManager` 的集成。

第七阶段不完整实现 Text/Button/Input/List/Table/Dialog 等高级组件，也不实现 VStack/HStack/Dock/ScrollView/Grid 等布局容器。`Text` 和 `Button` 可保留最小类型骨架，仅用于验证 `Widget` 契约、`...: View` 成员展开和 `@mixable` wrapper，不在本阶段实现文本绘制或按钮交互。CSS、选择器、级联样式、复杂捕获阶段、透明穿透和复杂 z-index 同样不在本阶段范围内。

叶子组件直接满足 `Widget` spec，容器组件满足 `ContainerWidget` spec；两者分别可通过成员展开复用 `View` 或 `Container` 的状态与行为。上层组装组件树时不需要 `asWidget()` 之类的转换 API。

当前事件分发切片只实现鼠标命中、鼠标回调选择和自下向上的冒泡。焦点管理、键盘
焦点路由及多播事件均留到后续步骤，不与本次鼠标分发一起实现。

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
  var x: Union<u32, float>;
  var y: Union<u32, float>;
  var width: Union<u32, float>;
  var height: Union<u32, float>;
  var foreColor: Option<RgbColor>;
  var backColor: Option<RgbColor>;
  var padding: Thickness;
  var margin: Thickness;
  var overflow: WidgetOverflow;
  var horizontalAlign: WidgetAlign;
  var verticalAlign: WidgetAlign;
}
```

尺寸和坐标使用 `Union<u32, float>`：`u32` 表示固定终端单元数，`float` 表示百分比。`x`/`y` 仅用于 `Absolute` 和 `Fixed`，`Normal`、`Relative` 忽略二者。`Absolute` 的相对坐标以最近的非 `Normal` 祖先组件为参照，且不受该祖先组件 `padding` 影响；不存在非 `Normal` 祖先时以屏幕为参照。`Fixed` 始终以屏幕为参照。`Absolute` 和 `Fixed` 忽略 `horizontalAlign` 和 `verticalAlign`。

`foreColor`/`backColor` 为 `none` 时使用终端默认色。`overflow == Hidden` 时裁剪超出组件区域的绘制；滚动能力由后续组件实现。

### 3.3 Thickness

`Thickness` 表示矩形四边的间距，供 `padding`、`margin` 及后续边框使用：

```feng
open type Thickness {
  let top: Union<u32, float>;
  let right: Union<u32, float>;
  let bottom: Union<u32, float>;
  let left: Union<u32, float>;
}
```

当前提供六种构造形式：四边值、垂直/水平值和统一值，每种形式分别支持全部使用 `u32` 或全部使用 `float`。

### 3.4 WidgetFrame

`WidgetFrame` 是 `@value` 矩形类型，同时用于保存布局结果和本帧实际绘制区域：

```feng
@value
open type WidgetFrame {
  var x: u32;
  var y: u32;
  var width: u32;
  var height: u32;
}
```

用户声明值保存在 `WidgetStyle` 中；`arrange()` 将布局结果写入 `frame`，`draw()` 将
经过祖先和 Screen 裁剪的本帧实际绘制区域写入 `drawFrame`。事件命中只读取
`drawFrame`，不在事件阶段重新遍历祖先或解析当前样式。

## 4 Widget 契约

`Widget` 不是继承基类，而是组件参与视图树的能力契约。组件多态通过 spec 实现，代码复用通过组合实现。

```feng
open spec Widget {
  let style: WidgetStyle;
  var frame: WidgetFrame;
  var drawFrame: WidgetFrame;
  var parent: Option<ContainerWidget>;

  func arrange(manager: ViewManager): void;
  func draw(manager: ViewManager): void;
  func isAncestor(w: Widget): bool;

  var onKey: Action<KeyEvent>;
  var onMouseDown: Action<MouseEvent>;
  var onMouseMove: Action<MouseEvent>;
  var onMouseUp: Action<MouseEvent>;
  var onWheel: Action<MouseEvent>;
}
```

其中：

- `style` 引用不可重新绑定；
- `frame` 是 `@value` 布局结果，由 `arrange()` 整体写回；
- `drawFrame` 是 `@value` 绘制快照，由 `draw()` 计算并在登记 sequence 时整体写回；
- `parent` 只能是容器组件，根组件的 `parent` 为 `none`；
- `arrange`、`draw` 使用 `func` 定义，不是可由外部替换的回调字段；
- `isAncestor(w)` 判断当前组件是否为 `w` 的祖先，不把自身视为自身的祖先；
- 键盘和鼠标处理使用可配置的事件回调字段；
- 第七阶段不引入独立 `ViewNode`，`Widget` 自身就是组件树节点。

## 5 View 基础组件

基础组件定义在 `std.tui.widgets` 子模块中：

```feng
open type View: Widget {
  let style: WidgetStyle;
  var frame: WidgetFrame;
  var drawFrame: WidgetFrame;
  var parent: Option<ContainerWidget>;

  func arrange(manager: ViewManager): void;
  func draw(manager: ViewManager): void;
  func isAncestor(w: Widget): bool;

  var onKey: Action<KeyEvent>;
  var onMouseDown: Action<MouseEvent>;
  var onMouseMove: Action<MouseEvent>;
  var onMouseUp: Action<MouseEvent>;
  var onWheel: Action<MouseEvent>;
}
```

`View` 通过 `@mixable` 静态方法向展开目标提供默认实例行为。`View.arrange()` 只根据当前组件的 `style`、祖先组件区域和屏幕尺寸计算自身 `frame`；`View.draw()` 使用 `frame` 及 `backColor` 在 Screen back buffer 中填充当前组件的空白矩形，前景色使用终端默认色，并通过 `ViewManager.trace(widget, drawFrame)` 同时缓存有效绘制区域和登记绘制顺序。`foreColor` 由后续实际绘制字符的组件使用。`View.draw()` 不遍历子组件，不绘制文本或边框。有效绘制区域是自身 `frame`、Screen 与最近一个 `overflow == Hidden` 祖先组件 `frame` 的交集；不存在这样的祖先时只与 Screen 求交。

`View.isAncestor(w)` 使用循环沿 `w.parent` 向上查找，不使用递归。后续组件可以展开 `View` 复用公共状态与默认行为，也可以直接实现 `Widget` spec。

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
  ...: View;

  let children: List<Widget>;
}
```

`Container` 展开 `View` 的公共状态与 Widget 默认行为，并通过以 `ContainerWidget` 为首参数的 `@mixable` 静态方法实现 `addChild`、`removeChild` 和 `clearChildren`。`Container` 只提供树存储与修改行为，不默认布局或绘制 children。

## 7 arrange 与 draw

每轮渲染分为固定的两个阶段：

```text
arrange 阶段
  Widget.arrange(manager)
  将 WidgetStyle 解析为 WidgetFrame

draw 阶段
  Widget.draw(manager)
  根据 WidgetFrame 和非尺寸样式绘制
  缓存 drawFrame 并登记到 ViewManager.sequence
```

`ViewManager.arrange()` 和 `ViewManager.draw()` 只调用 root。每个组件自行决定是否、何时以及按什么顺序调用 children 的 `arrange()` 和 `draw()`；例如虚拟滚动容器可以只绘制当前可见的子组件。

`arrange` 和 `draw` 都接收同一个 `ViewManager`。组件通过 manager 获取本轮调度所需的视图上下文。只有实际调用 `draw()` 并执行 sequence 登记的组件才参与本帧鼠标命中。

### 7.1 Normal 与 Relative 排列

`position == Normal` 或 `Relative` 时，组件根据直接父组件 content 区域计算最终区域；父组件 content 区域是父组件 `frame` 扣除父组件 `padding` 后的区域。无 parent 的根组件以整个 Screen 为参照区域。`x`/`y` 不参与 `Normal` 或 `Relative` 布局。两者的自身布局行为一致，区别仅在于 `Relative` 可以成为后代 `Absolute` 组件的定位参照，而 `Normal` 会被查找过程跳过。

每个轴独立按以下顺序计算：

1. 百分比 margin 以该轴的参照区域尺寸为基数，先解析自身两侧 margin；
2. 从参照区域扣除两侧 margin，得到该轴的可用区域；
3. `Full` 忽略该轴的 `width`/`height` 并占满可用区域；
4. `Start`、`Center`、`End` 根据 `width`/`height` 计算尺寸，并在可用区域的起始、中间或末端定位；
5. 百分比 `width`/`height` 以扣除自身 margin 后的可用区域为基数，转换为 `u32` 时向零截断。

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

`View.draw()` 先查找离自身最近的 `overflow == Hidden` 祖先组件。自身 `frame` 与该祖先组件 `frame` 求交后，再与 Screen 求交，得到本次有效绘制区域；不存在这样的祖先时，自身 `frame` 直接与 Screen 求交。裁剪结果不写回布局 `frame`，而是在登记 sequence 时写入 `drawFrame`，作为本帧绘制与后续鼠标命中的共同快照。

裁剪使用完整矩形求交，同时计算裁剪后的 `x`、`y`、`width` 和 `height`，不使用只能处理 Screen 右侧或下侧边界的单轴长度计算。有效区域的任一尺寸为 0 时不写入 Buffer，也不登记到 `sequence`；此时旧 `drawFrame` 即使仍存在，也因组件不在本帧 sequence 中而不会参与命中。

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
  open func trace(widget: Widget, drawFrame: WidgetFrame): void;
  open func arrange(): void;
  open func draw(): void;
}
```

`ViewManager` 通过构造函数接收已有 `Screen`，不创建新的 Screen。`getScreenWidth()` 和 `getScreenHeight()` 为 `View.arrange()` 提供无 parent 组件及 `Fixed` 组件的布局参照；`getScreenBuffer()` 返回当前 Screen 的 back buffer。组件不缓存该 Buffer，避免 Screen resize 后继续使用旧引用。

`root` 默认为 `none`。`arrange()` 在无 root 时不做处理。`draw()` 每轮先清空 `sequence`；存在 root 时再清空 Screen back buffer 并从 root 开始绘制，使组件移动或缩小后不会留下旧帧内容。无 root 时不清空 back buffer，保留现有直接通过 Screen 绘制的使用方式。逐帧只清空 `screen.buffer()`，不调用同时清空 front/back 的 `Screen.clear()`，以保留正确的 diff 基准。

`sequence` 是 `ViewManager` 的内部成员，不对上层公开。每轮绘制开始时清空；组件完成有效绘制区域计算后调用 `trace(widget, drawFrame)`，该方法先将区域写入 `widget.drawFrame`，再将组件登记到 `sequence`，保证绘制快照与顺序登记不会分离。越靠后的组件实际绘制层级越高，容器对 children 的调用顺序同时决定子组件在 sequence 中的顺序。

鼠标命中时从 `sequence` 末尾向前查找，并直接使用每个组件缓存的 `drawFrame` 判断事件坐标；第一个命中的组件即为目标组件。第七阶段不支持透明穿透，因此命中顶层组件后不继续向下查找。

`drawFrame` 表示用户当前看到的上一帧区域。即使布局、样式或组件树在绘制后发生修改，事件阶段也不重新计算命中区域；下一次 draw 会更新 sequence 与 `drawFrame`。这既保持命中与实际画面一致，也避免 1003 鼠标移动事件中反复遍历祖先。

当前 `ViewManager` 已提供可选 root 的 arrange/draw 入口、back buffer 访问与 sequence 登记方法，并持有由 `TuiApp` 传入的 Screen；下一步实现逆序命中与鼠标事件路由。焦点与键盘路由不在本次实现范围内。

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

`Widget` 当前直接使用已有基础事件类型：

- `onKey: Action<KeyEvent>`；
- `onMouseDown`、`onMouseMove`、`onMouseUp`、`onWheel: Action<MouseEvent>`。

`MouseEvent` 按 `docs/engineering/feng-std-tui-input-dev.md` 定义为引用类型：输入载荷
不可变，传播状态由 `stop()` 和 `isStopped()` 管理。`stop()` 在当前 Widget 回调返回后
生效，只阻止后续父组件回调；不会中断当前回调。重复调用 `stop()` 保持停止状态。

鼠标事件按以下规则分发：

1. 从 `sequence` 末尾向前查找第一个 `drawFrame` 包含事件坐标的 Widget；
2. `wheelUp`/`wheelDown` 调用 `onWheel`，其余事件按 `press`、`move`、`release`
   分别调用 `onMouseDown`、`onMouseMove`、`onMouseUp`；
3. 当前 Widget 回调返回后检查 `event.isStopped()`；已停止则结束分发，否则沿
   `parent` 继续向上传递；
4. 未命中 Widget 时静默返回；不执行 root 兜底回调；
5. 当前不定义捕获阶段、`target`、`currentTarget`、默认行为、透明穿透、事件克隆或
   事件池化。

每个 Widget 的每类事件仍只有一个 `Action<MouseEvent>` 回调字段，InputManager 的
`onMouse` 也保持单播；本阶段不引入处理器列表或隐式多播。

## 11 与 TuiApp/InputManager 的集成

第七阶段完成后，`TuiApp` 持有：

```feng
open type TuiApp {
  let screen: Screen;
  let input: InputManager;
  let view: ViewManager;
}
```

`ViewManager` 使用 `InputManager` 产生的 `MouseEvent` 进行视图事件路由，并通过
`Screen` 完成组件绘制。`Screen` 和 `InputManager` 不在视图层重建。

`TuiApp.render()` 处理 resize 后依次调用 `view.arrange()` 和 `view.draw()`，再通过
`Screen.buildPatchBytes()` 生成终端输出。无 root 时前两步不改变 Screen back buffer，
现有直接绘制 Screen 的代码保持有效。

本次集成只将 `InputManager.onMouse` 单播回调绑定到 ViewManager 的鼠标分发入口，
不接管 `onKey`。由于 `onMouse` 是单播 `open var`，应用之后直接重新赋值会替换
ViewManager 路由；本阶段不自动组合两个回调。焦点和键盘事件在后续步骤接入。

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
3. 将 `MouseEvent` 改为引用类型，增加 `stop()` 与 `isStopped()`；
4. 在 draw 阶段缓存 `drawFrame`，实现基于该快照的 sequence 逆序命中；
5. 实现鼠标回调选择、自下向上冒泡及停止传播；
6. 将 `InputManager.onMouse` 单播回调接入 ViewManager，不接管 `onKey`；
7. 补充 MouseEvent、命中、裁剪、层级、冒泡和停止传播的 std_test 用例；
8. 执行全量回归测试 `make test`；
9. 等待人工 Review，通过后再开始焦点与键盘路由。

## 14 Review 关注点

现有定义尚未确定以下实现契约：

- `WidgetStyle` 的默认值；
- `View` 与 `Container` 的构造和字段初始化方式；
- `spec` 的 `seal` 成员落地后，如何在保持公开树操作 API 不变的前提下收紧 children/parent 存储访问；
- 后续焦点、键盘路由与 root 的具体关系。
