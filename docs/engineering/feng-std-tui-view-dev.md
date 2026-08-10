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
- `ViewManager` 的组件树、绘制顺序、命中、焦点及事件路由；
- `ViewManager` 与 `TuiApp`、`Screen`、`InputManager` 的集成。

第七阶段不完整实现 Text/Button/Input/List/Table/Dialog 等高级组件，也不实现 VStack/HStack/Dock/ScrollView/Grid 等布局容器。`Text` 和 `Button` 可保留最小类型骨架，仅用于验证 `Widget` 契约、`...: View` 成员展开和 `@mixable` wrapper，不在本阶段实现文本绘制或按钮交互。CSS、选择器、级联样式、复杂捕获阶段、透明穿透和复杂 z-index 同样不在本阶段范围内。

叶子组件直接满足 `Widget` spec，容器组件满足 `ContainerWidget` spec；两者分别可通过成员展开复用 `View` 或 `Container` 的状态与行为。上层组装组件树时不需要 `asWidget()` 之类的转换 API。

## 3 已定义的核心类型

### 3.1 布局枚举

`std/std/src/tui/view/Widget.ff` 已定义：

```feng
open enum WidgetPosition {
  Normal,
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
- `Absolute`：相对父组件定位；
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

尺寸和坐标使用 `Union<u32, float>`：`u32` 表示固定终端单元数，`float` 表示百分比。`x`/`y` 仅用于 `Absolute` 和 `Fixed`，`Normal` 忽略二者。`Absolute` 的相对坐标以父组件为参照，且不受父组件 `padding` 影响；`Fixed` 以屏幕为参照。浮动组件忽略 `horizontalAlign` 和 `verticalAlign`。

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

`WidgetFrame` 是 `@value` 类型，只保存 `arrange` 后的最终矩形：

```feng
@value
open type WidgetFrame {
  var x: u32;
  var y: u32;
  var width: u32;
  var height: u32;
}
```

用户声明值保存在 `WidgetStyle` 中，计算结果写入 `WidgetFrame`。`draw`、命中测试和事件坐标判断均使用 `WidgetFrame`，不在绘制阶段重新解析声明值。

## 4 Widget 契约

`Widget` 不是继承基类，而是组件参与视图树的能力契约。组件多态通过 spec 实现，代码复用通过组合实现。

```feng
open spec Widget {
  let style: WidgetStyle;
  var frame: WidgetFrame;
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

`View` 通过 `@mixable` 静态方法向展开目标提供默认实例行为。`View.arrange()` 只根据当前组件的 `style`、父组件区域和屏幕尺寸计算自身 `frame`；`View.draw()` 使用 `frame` 及 `backColor` 在 Screen back buffer 中填充当前组件的空白矩形，前景色使用终端默认色，并将组件登记到 `ViewManager.sequence`。`foreColor` 由后续实际绘制字符的组件使用。`View.draw()` 不遍历子组件，不绘制文本或边框。超出 Screen 的部分被裁剪，父容器的 `overflow` 裁剪不在当前实现范围内。

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
  将自身登记到 ViewManager.sequence
```

`ViewManager.arrange()` 和 `ViewManager.draw()` 只调用 root。每个组件自行决定是否、何时以及按什么顺序调用 children 的 `arrange()` 和 `draw()`；例如虚拟滚动容器可以只绘制当前可见的子组件。

`arrange` 和 `draw` 都接收同一个 `ViewManager`。组件通过 manager 获取本轮调度所需的视图上下文。只有实际调用 `draw()` 并执行 sequence 登记的组件才参与本帧鼠标命中。

### 7.1 Normal 排列

`position == Normal` 时，组件根据父组件 content 区域计算最终区域；父组件 content 区域是父组件 `frame` 扣除父组件 `padding` 后的区域。无 parent 的根组件以整个 Screen 为参照区域。`x`/`y` 不参与 Normal 布局。

每个轴独立按以下顺序计算：

1. 百分比 margin 以该轴的参照区域尺寸为基数，先解析自身两侧 margin；
2. 从参照区域扣除两侧 margin，得到该轴的可用区域；
3. `Full` 忽略该轴的 `width`/`height` 并占满可用区域；
4. `Start`、`Center`、`End` 根据 `width`/`height` 计算尺寸，并在可用区域的起始、中间或末端定位；
5. 百分比 `width`/`height` 以扣除自身 margin 后的可用区域为基数，转换为 `u32` 时向零截断。

`padding` 不改变组件自身的 `frame`，由具体容器在计算其 children 的参照区域时使用。组件自行决定是否及如何触发 children 的布局，`ViewManager` 不参与子组件布局。

### 7.2 Absolute 与 Fixed

- `Absolute` 使用 `x`/`y` 相对父组件定位，坐标不受父组件 `padding` 影响；
- `Fixed` 使用 `x`/`y` 相对屏幕定位；
- 两者均忽略 `horizontalAlign` 和 `verticalAlign`；
- `width`/`height` 仍支持固定值或百分比；
- 自身 margin 先从参照区域扣除，百分比 `width`/`height` 再以剩余区域为基数；
- `x`/`y` 的百分比仍以未扣除自身 margin 的参照区域为基数，最终位置叠加起始侧 margin；
- 父级裁剪不属于本轮 `View.arrange()` 的实现范围。

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
  open func trace(widget: Widget): void;
  open func arrange(): void;
  open func draw(): void;
}
```

`ViewManager` 通过构造函数接收已有 `Screen`，不创建新的 Screen。`getScreenWidth()` 和 `getScreenHeight()` 为 `View.arrange()` 提供无 parent 组件及 `Fixed` 组件的布局参照；`getScreenBuffer()` 返回当前 Screen 的 back buffer。组件不缓存该 Buffer，避免 Screen resize 后继续使用旧引用。

`root` 默认为 `none`。`arrange()` 在无 root 时不做处理。`draw()` 每轮先清空 `sequence`；存在 root 时再清空 Screen back buffer 并从 root 开始绘制，使组件移动或缩小后不会留下旧帧内容。无 root 时不清空 back buffer，保留现有直接通过 Screen 绘制的使用方式。逐帧只清空 `screen.buffer()`，不调用同时清空 front/back 的 `Screen.clear()`，以保留正确的 diff 基准。

`sequence` 是 `ViewManager` 的内部成员，不对上层公开。每轮绘制开始时清空；组件进入自身 `draw()` 时将自身登记到 `sequence`，越靠后的组件实际绘制层级越高。容器对 children 的调用顺序同时决定子组件在 sequence 中的顺序。

鼠标命中时从 `sequence` 末尾向前查找，第一个包含事件坐标的 `WidgetFrame` 即为目标组件。第七阶段不支持透明穿透，因此命中顶层组件后不继续向下查找。

`sequence` 只记录本帧实际绘制顺序，并用于鼠标命中；它不保存也不处理剪裁状态。剪裁规则属于具体组件的绘制实现。

当前 `ViewManager` 已提供可选 root 的 arrange/draw 入口、back buffer 访问与 sequence 登记方法，并持有由 `TuiApp` 传入的 Screen；尚未实现逆序命中、焦点、input 引用与事件路由。

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

`KeyEvent` 和 `MouseEvent` 均为不可变的 `@value` 快照。当前定义不新增 `WidgetEvent`，也未定义 `target`、`currentTarget`、`stopPropagation()`、`preventDefault()`、`clone()` 或事件池化语义。

`ViewManager` 后续负责根据焦点和 `sequence` 选择起始组件，并按 `parent` 自下向上传递事件。现有回调签名没有传播状态或返回值，因此“如何阻止继续向上传递”尚未由当前接口表达，需要在实现事件冒泡前人工确定。

## 11 与 TuiApp/InputManager 的集成

第七阶段完成后，`TuiApp` 持有：

```feng
open type TuiApp {
  let screen: Screen;
  let input: InputManager;
  let view: ViewManager;
}
```

`ViewManager` 使用 `InputManager` 产生的 `KeyEvent`/`MouseEvent` 进行视图事件路由，并通过 `Screen` 完成组件绘制。`Screen` 和 `InputManager` 不在视图层重建。

当前验证阶段在 `TuiApp.render()` 处理 resize 后依次调用 `view.arrange()` 和 `view.draw()`，再通过 `Screen.buildPatchBytes()` 生成终端输出。无 root 时前两步不改变 Screen back buffer，现有直接绘制 Screen 的代码保持有效。本轮不接入 InputManager。

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

1. Review 并确认 `Widget`/`ContainerWidget`、`View`/`Container` 和 `ViewManager.sequence` 契约；
2. 实现 `View.arrange()` 的自身布局计算，并通过 `ViewManager` 读取 Screen 尺寸；
3. 实现 `Container` 的 children 存储、自动迁移、幂等移除和 parent 同步维护；
4. 实现非递归 `isAncestor` 与 `"tui/widget/cycle"` 循环阻止；
5. 完善 `ViewManager` 的 root、sequence 登记、arrange/draw 根调度与逆序命中；
6. 由具体组件实现 children 的 arrange/draw 调度策略，验证只有实际绘制组件进入 sequence；
7. 补充焦点管理，从 `InputManager` 接收事件并实现鼠标命中与键盘焦点路由；
8. 在事件接口确定后实现自下向上的冒泡及阻止传播；
9. 集成 `ViewManager` 到 `TuiApp`；
10. 使用 Text/Button 最小骨架验证 `...: View` 与 `@mixable` wrapper；
11. 补充 std_test 用例；
12. 执行全量回归测试 `make test`；
13. 等待人工 Review，通过后再进入后续组件扩展阶段。

## 14 Review 关注点

现有定义尚未确定以下实现契约：

- `WidgetStyle` 的默认值；
- `View` 与 `Container` 的构造和字段初始化方式；
- `Absolute`/`Fixed` 的父级裁剪规则；
- `overflow == Hidden` 的裁剪状态由何处保存和传递；
- `spec` 的 `seal` 成员落地后，如何在保持公开树操作 API 不变的前提下收紧 children/parent 存储访问；
- root、焦点以及 `InputManager` 与 `ViewManager` 的具体关系；
- 现有 `@value` 事件回调如何表达阻止冒泡。
