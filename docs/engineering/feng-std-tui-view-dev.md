# Feng TUI 视图机制层方案

> 状态：设计中（design）
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 第七阶段（视图机制层）的主规范。
> 当前接口基线以 `std/std/src/tui/Widget.ff`、`std/std/src/tui/Thickness.ff`、`std/std/src/tui/ViewManager.ff` 和 `std/std/src/tui/views/View.ff` 中已经定义的类型为准。

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

- `Widget` spec 与 `WidgetStyle` type；
- `WidgetFrame`、`Thickness` 及布局相关枚举；
- `std.tui.views.View` 基础组件；
- `ViewManager` 的组件树、绘制顺序、命中、焦点及事件路由；
- `ViewManager` 与 `TuiApp`、`Screen`、`InputManager` 的集成。

第七阶段不实现 Text/Button/Input/List/Table/Dialog 等高级组件，也不实现 VStack/HStack/Dock/ScrollView/Grid 等布局容器。CSS、选择器、级联样式、复杂捕获阶段、透明穿透和复杂 z-index 同样不在本阶段范围内。

后续组件直接满足 `Widget` spec，并可在内部组合 `View` 或其他组件。上层组装组件树时不需要 `asWidget()` 之类的转换 API。

## 3 已定义的核心类型

### 3.1 布局枚举

`std/std/src/tui/Widget.ff` 已定义：

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
  let frame: WidgetFrame;
  var parent: Option<Widget>;
  let children: List<Widget>;

  func arrange(manager: ViewManager): void;
  func draw(manager: ViewManager): void;

  var onKey: Action<KeyEvent>;
  var onMouseDown: Action<MouseEvent>;
  var onMouseMove: Action<MouseEvent>;
  var onMouseUp: Action<MouseEvent>;
  var onWheel: Action<MouseEvent>;
}
```

其中：

- `style` 和 `frame` 引用不可重新绑定；
- `parent` 由组件树机制维护；
- `children` 直接使用 `List<Widget>`；
- `arrange`、`draw` 使用 `func` 定义，不是可由外部替换的回调字段；
- 键盘和鼠标处理使用可配置的事件回调字段；
- 第七阶段不引入独立 `ViewNode`，`Widget` 自身就是组件树节点。

## 5 View 基础组件

基础组件定义在 `std.tui.views` 子模块中：

```feng
open type View: Widget {
  let style: WidgetStyle;
  let frame: WidgetFrame;
  var parent: Option<Widget>;
  let children: List<Widget>;

  func arrange(manager: ViewManager): void;
  func draw(manager: ViewManager): void;

  var onKey: Action<KeyEvent>;
  var onMouseDown: Action<MouseEvent>;
  var onMouseMove: Action<MouseEvent>;
  var onMouseUp: Action<MouseEvent>;
  var onWheel: Action<MouseEvent>;
}
```

`View.arrange()` 提供按 `style` 计算 `frame` 的默认入口，子组件分别通过自身 `arrange()` 计算区域。`View.draw()` 默认不绘制内容。后续组件可以组合 `View` 复用公共状态，也可以直接实现 `Widget` spec。

## 6 arrange 与 draw

每轮渲染分为固定的两个阶段：

```text
arrange 阶段
  Widget.arrange(manager)
  将 WidgetStyle 解析为 WidgetFrame

draw 阶段
  Widget.draw(manager)
  只根据 WidgetFrame 绘制
  将自身登记到 ViewManager.sequence
```

`arrange` 和 `draw` 都接收同一个 `ViewManager`。组件通过 manager 获取本轮调度所需的视图上下文；具体公开或内部辅助 API 随 `ViewManager` 完善后确定。

### 6.1 Normal 排列

`position == Normal` 时，组件由父组件安排，并根据 `margin`、`padding`、`width`、`height`、`horizontalAlign` 和 `verticalAlign` 计算最终区域。`x`/`y` 不参与计算。

`Full` 表示对应方向填满父组件分配的可用空间。`Full` 与显式 `width`/`height` 同时设置时的优先级尚未由现有类型定义确定，进入实现前需要人工确认。

### 6.2 Absolute 与 Fixed

- `Absolute` 使用 `x`/`y` 相对父组件定位，坐标不受父组件 `padding` 影响；
- `Fixed` 使用 `x`/`y` 相对屏幕定位；
- 两者均忽略 `horizontalAlign` 和 `verticalAlign`；
- `width`/`height` 仍支持固定值或百分比；
- `margin`、百分比参照范围及父级裁剪规则按 Review 后确定的 arrange 规则执行。

## 7 ViewManager 与 sequence

`ViewManager` 当前已定义绘制顺序集合：

```feng
open type ViewManager {
  seal let sequence: List<Widget>;
}
```

`sequence` 是 `ViewManager` 的内部成员，不对上层公开。每轮绘制开始时清空；组件进入自身 `draw()` 时将自身登记到 `sequence`，越靠后的组件绘制层级越高。

鼠标命中时从 `sequence` 末尾向前查找，第一个包含事件坐标的 `WidgetFrame` 即为目标组件。第七阶段不支持透明穿透，因此命中顶层组件后不继续向下查找。

当前 `ViewManager` 尚未定义 root、焦点、screen/input 引用、渲染入口和 sequence 登记方法。这些成员和方法属于第七阶段后续实现，但具体签名不能在当前文档中先行假定。

## 8 组件树与 parent

组件树直接由 `Widget.children: List<Widget>` 与 `Widget.parent` 表达，不增加 `WidgetChildren` 或 `ViewNode`。

`parent` 不能由上层调用者手动维护，添加、移除和清空子组件时必须由组件树 API 同步更新。当前源码尚未定义组件树变更 API；由于 `children` 是 `List<Widget>`，如何防止调用者绕过 parent 维护，需要在实现前确定。

## 9 事件接口

`Widget` 当前直接使用已有基础事件类型：

- `onKey: Action<KeyEvent>`；
- `onMouseDown`、`onMouseMove`、`onMouseUp`、`onWheel: Action<MouseEvent>`。

`KeyEvent` 和 `MouseEvent` 均为不可变的 `@value` 快照。当前定义不新增 `WidgetEvent`，也未定义 `target`、`currentTarget`、`stopPropagation()`、`preventDefault()`、`clone()` 或事件池化语义。

`ViewManager` 后续负责根据焦点和 `sequence` 选择起始组件，并按 `parent` 自下向上传递事件。现有回调签名没有传播状态或返回值，因此“如何阻止继续向上传递”尚未由当前接口表达，需要在实现事件冒泡前人工确定。

## 10 与 TuiApp/InputManager 的集成

第七阶段完成后，`TuiApp` 持有：

```feng
open type TuiApp {
  let screen: Screen;
  let input: InputManager;
  let view: ViewManager;
}
```

`ViewManager` 使用 `InputManager` 产生的 `KeyEvent`/`MouseEvent` 进行视图事件路由，并通过 `Screen` 完成组件绘制。`Screen` 和 `InputManager` 不在视图层重建。

## 11 文件规划

```text
std/std/src/tui/
  Thickness.ff       # 四边间距类型（已定义）
  Widget.ff          # 布局枚举、WidgetStyle、WidgetFrame、Widget（已定义）
  ViewManager.ff     # 视图管理器与 sequence（已定义骨架）
  TuiApp.ff          # 后续新增 view 成员并接入 ViewManager

std/std/src/tui/views/
  View.ff            # Widget 基础实现（已定义骨架）
```

第七阶段不新增 Text/Button 等高级组件文件。

## 12 实施步骤

1. Review 并确认现有 `Thickness`、`WidgetStyle`、`WidgetFrame`、`Widget`、`View` 和 `ViewManager.sequence` 定义；
2. 补全 `WidgetStyle` 默认值以及 `View` 的初始化；
3. 定义组件树修改 API，确保 parent 自动维护；
4. 完善 `ViewManager` 的 root、焦点、screen/input 接入及 sequence 登记机制；
5. 实现 `arrange` 阶段；
6. 实现 `draw` 阶段及 sequence 维护；
7. 实现鼠标命中和键盘焦点路由；
8. 在事件接口确定后实现自下向上的冒泡及阻止传播；
9. 集成 `ViewManager` 到 `TuiApp`；
10. 补充 std_test 用例；
11. 执行全量回归测试 `make test`；
12. 等待人工 Review，通过后再进入后续组件扩展阶段。

## 13 Review 关注点

现有定义尚未确定以下实现契约：

- `WidgetStyle` 的默认值；
- `View` 的构造和字段初始化方式；
- `u32`/`float` 尺寸、坐标及 `Thickness` 百分比的精确参照范围和取整规则；
- `Full` 与显式 `width`/`height` 同时设置时的优先级；
- `Absolute`/`Fixed` 的 margin 与裁剪规则；
- `overflow == Hidden` 的裁剪状态由何处保存和传递；
- `children: List<Widget>` 条件下如何保证 parent 只能由组件树 API 维护；
- `seal sequence` 的组件登记 API；
- root、焦点以及 `Screen`/`InputManager` 与 `ViewManager` 的具体关系；
- 现有 `@value` 事件回调如何表达阻止冒泡。
