# Feng TUI 视图机制层方案

> 状态：设计中（design）
>
> 本文档是 `dev/feng-std-tui-dev.md` 阶段七（视图逻辑层）的实现方案细化。
> 本阶段只定义并实现 `ViewManager` + `Widget` 机制层，不实现 Text/Button/Input/List/ScrollView 等高级组件。

## 1 总体目标

在现有 `TuiApp`、`Screen`、`InputManager` 基础上，引入一层 retained-mode 的类型化 TUI 组件机制：

```text
TuiApp
  ├─ screen: Screen          # 渲染底座，负责双缓冲与 diff 输出
  ├─ input: InputManager     # 输入底座，负责终端字节流解析
  └─ view: ViewManager       # 视图机制层，负责组件树、布局、绘制与事件路由
```

`Screen`、`InputManager`、`ViewManager` 不互相替代、不重复创建。三者作为 `TuiApp` 的成员分管不同逻辑：

- `Screen` 只负责画布与差异同步；
- `InputManager` 只负责基础输入解析；
- `ViewManager` 负责组件树、布局调度、绘制调度、焦点管理、事件路由。

## 2 阶段边界

第七阶段只交付视图机制层：

- 实现 `ViewManager`；
- 定义 `Widget` spec；
- 提供 `BaseWidget` 默认实现；
- 定义 `WidgetStyle`、`WidgetFrame`、布局调度流程；
- 定义事件引用语义、事件冒泡与焦点规则；
- 定义 `paintList` 命中机制；
- 将 `ViewManager` 集成到 `TuiApp`。

第七阶段不实现以下内容：

- 不实现 Text/Button/Input/List/Table/Dialog 等高级组件；
- 不实现 VStack/HStack/Dock/ScrollView/Grid 等布局容器；
- 不实现 CSS、选择器、级联样式系统；
- 不实现复杂捕获阶段、透明穿透、复杂 z-index 规则；
- 不实现事件对象池化，池化只作为未来内部优化策略。

后续组件均基于本阶段机制扩展。高级组件可以组合 `BaseWidget`，也可以自行实现 `Widget` spec。

## 3 核心模型

### 3.1 Widget spec

`Widget` 不是继承基类，而是组件参与视图树的能力契约。Feng 没有继承，视图多态通过 `spec Widget` 实现，组件复用通过组合实现。

`Widget` spec 只包含 `ViewManager` 必须依赖的字段。Widget 的成员以字段为主，不通过 `handleEvent()` 这类统一方法做事件分发：

```feng
spec Widget {
  /** 用户声明的布局与样式约束 */
  style: WidgetStyle;

  /** 布局阶段计算后的最终区域信息 */
  frame: WidgetFrame;

  /** 父组件；仅由组件树 API 维护 */
  parent: Option<Widget>;

  /** 子组件集合；集合内部负责维护 parent 引用 */
  children: WidgetChildren;

  /** 布局回调；普通 Widget 可使用默认布局，布局组件可在此计算子组件 frame */
  layout: Action<WidgetLayoutContext>;

  /** 绘制当前组件；绘制阶段只使用 frame，不重新解释 style */
  draw: Action<WidgetDrawContext>;

  /** 键盘事件回调 */
  onKey: Action<WidgetEvent>;

  /** 鼠标按下事件回调 */
  onMouseDown: Action<WidgetEvent>;

  /** 鼠标移动事件回调 */
  onMouseMove: Action<WidgetEvent>;

  /** 鼠标释放事件回调 */
  onMouseUp: Action<WidgetEvent>;

  /** 鼠标滚轮事件回调 */
  onWheel: Action<WidgetEvent>;
}
```

> 说明：上述字段是设计目标，实际实现时如受当前 spec/泛型能力约束，可在不改变语义的前提下调整具体 API 形态。若字段需要可选回调，具体类型可按 Feng 现有 `Option<T>` 或空引用检查能力落地。

### 3.2 BaseWidget

`BaseWidget` 是标准库提供的默认实现，保存通用组件状态：

```text
BaseWidget
  style: WidgetStyle
  frame: WidgetFrame
  parent: Option<Widget>
  children: WidgetChildren
  focusable: bool
  layout/draw/onKey/onMouseDown/onMouseMove/onMouseUp/onWheel 等回调字段
```

简单组件可以直接创建并配置 `BaseWidget`；复杂组件可以内部持有 `BaseWidget`，并通过方法转发满足 `Widget` spec。用户组装组件树时不需要 `asWidget()` 之类的转换 API，组件自身只要满足 `Widget` 即可加入树。

### 3.3 无 ViewNode

第七阶段不引入 `ViewNode`。`Widget` 自身就是组件树节点：

- 树结构由 `Widget.children` 和 `Widget.parent` 表达；
- 布局声明由 `WidgetStyle` 表达；
- 布局结果由 `WidgetFrame` 表达；
- 布局、绘制和事件由 `Widget.layout` / `Widget.draw` / `Widget.onXXX` 字段表达。

这样避免在组件对象之外再维护一棵节点树，降低组件树 API 的复杂度。

## 4 布局模型

### 4.1 声明值与计算值分离

组件布局分为两个阶段：

```text
声明阶段：用户设置 WidgetStyle
布局阶段：ViewManager 调度组件布局，组件或布局组件计算 WidgetFrame
绘制阶段：Widget.draw() 只读取 WidgetFrame
```

`WidgetStyle` 是用户意图，`WidgetFrame` 是本轮布局后的事实。绘制、命中测试、事件坐标转换均以 `WidgetFrame` 为准，不在绘制阶段重新解释百分比、margin、padding、align。

### 4.2 WidgetStyle

`WidgetStyle` 保存用户声明的布局约束：

```text
WidgetStyle
  position: normal | absolute | fixed
  x: WidgetOffset
  y: WidgetOffset
  width: WidgetSize
  height: WidgetSize
  margin: WidgetInsets
  padding: WidgetInsets
  hAlign: start | center | end | full
  vAlign: start | center | end | full
```

`width`/`height` 是重要字段，支持数字或百分比。百分比的参照系为父容器内容区尺寸，即父组件 `frame` 扣除 `padding` 后的区域。

`x`/`y` 用于浮动元素，支持数字或百分比：

- `absolute`：相对父容器内容区定位；
- `fixed`：相对 screen 区域定位；
- `normal`：忽略 `x`/`y`，通过 align 定位。

### 4.3 padding 与 margin

盒模型按以下语义定义：

```text
parent frame
  └─ parent padding 后得到 parent content rect
       └─ child margin box
            └─ child frame
                 └─ child padding 后得到 child content rect
```

- `padding` 属于组件内部，影响子组件的可布局区域；
- `margin` 属于组件外部，影响当前组件在父容器分配区域内的位置与尺寸；
- 命中测试默认使用组件最终可见区域，不包含 margin。

### 4.4 普通定位

`position == normal` 的组件通过 align 在父容器内容区内定位：

```text
available = parent content rect - child margin
size = resolve(width/height, available)
position = align(size, available, hAlign/vAlign)
frame = resolved position + resolved size
```

`hAlign.full` 表示横向填满可用区域，`vAlign.full` 表示纵向填满可用区域。若对应方向同时设置了 `width`/`height`，`full` 优先，尺寸字段在该方向被忽略。

### 4.5 浮动定位

`position == absolute` 或 `position == fixed` 的组件使用 `x`/`y` 定位：

- `absolute` 的 `x`/`y` 相对父容器内容区；
- `fixed` 的 `x`/`y` 相对 screen 区域；
- `width`/`height` 仍按数字或百分比解析；
- `margin` 仍影响最终 frame；
- `align` 不参与浮动元素定位。

第七阶段不支持透明穿透。浮动组件一旦绘制并命中，即作为鼠标事件 target。

### 4.6 WidgetFrame

`WidgetFrame` 保存布局阶段计算后的区域信息，至少包含：

```text
WidgetFrame
  x/y/width/height             # 组件最终绝对区域
  contentX/contentY/...        # 扣除 padding 后的内容区
  clipX/clipY/...              # 有效裁剪区
```

布局阶段由 `ViewManager` 发起调度，具体 `WidgetFrame` 由对应组件或布局组件计算并写入。组件绘制和事件命中只读取 `WidgetFrame`。

## 5 ViewManager

`ViewManager` 是 `TuiApp` 的成员，负责连接组件树、`Screen` 和 `InputManager`：

```text
ViewManager
  screen: Screen
  input: InputManager
  root: Widget
  focusedWidget: Option<Widget>
  paintList: List<Widget>
```

### 5.1 渲染流程

每轮渲染按固定流程执行：

```text
ViewManager.render()
  1. paintList.clear()
  2. dispatchLayout(root, screen rect)
  3. draw(root)
  4. Screen.buildPatchBytes() 由 TuiApp 负责输出
```

layout 阶段由 `ViewManager` 负责调度，不把具体布局算法写死在 `ViewManager` 中。普通组件可使用默认布局逻辑，VStack/HStack/Dock/ScrollView 等后续布局组件可在自身 `layout` 字段中负责子组件的 `WidgetFrame` 计算。draw 阶段只按 `WidgetFrame` 绘制。

### 5.2 paintList

`paintList` 是 `ViewManager` 的成员，每轮渲染开始时清空。绘制遍历过程中，每进入一个组件的 `draw()` 之前，将该组件自身追加到 `paintList`：

```text
drawWidget(widget)
  paintList.add(widget)
  widget.draw(ctx)
  draw children
```

`paintList` 只保存 `Widget`，不引入独立 `PaintEntry`。命中测试所需区域来自 `widget.frame`。

`paintList` 中越靠后的组件越晚绘制，视觉层级越高。鼠标事件分发时从 `paintList` 末尾向前扫描，找到第一个命中的组件作为 target。

第七阶段暂不支持组件透明，因此不存在穿透规则：最上层命中组件即为鼠标事件 target。

## 6 组件树与 parent

`parent` 引用必须由组件树 API 自动维护，用户不能手动维护。

`WidgetChildren` 负责添加、移除、清空子组件时维护 parent：

```text
children.add(child)
  child.setParent(owner)
  append child

children.remove(child)
  remove child
  child.setParent(none)

children.clear()
  for child in children:
    child.setParent(none)
  clear list
```

`WidgetChildren` 的元素类型为 `Widget`，因此任何满足 `Widget` spec 的组件都能加入组件树。

## 7 事件模型

### 7.1 事件引用语义

视图层事件使用普通 `type`，不是 `@value`，按引用语义传递。

同一次事件分发过程中，`ViewManager` 将同一个 `WidgetEvent` 实例沿组件树传递，避免在组件树中逐层复制事件对象。

事件对象生命周期定义为：

- 事件对象只保证在当前同步分发过程中有效；
- 组件不应在分发结束后保存事件引用；
- 如需异步保存事件内容，应调用 `clone()` 或复制必要字段；
- 是否池化是内部优化策略，不进入上层语义；
- 未来如实现对象池，必须在借用前 `reset`，在分发后归还。

### 7.2 传播状态

`WidgetEvent` 持有分发状态：

```text
WidgetEvent
  target: Widget
  currentTarget: Widget
  propagationStopped: bool
  defaultPrevented: bool
```

并提供方法：

```text
stopPropagation()
preventDefault()
clone()
```

`clone()` 生成独立事件快照，用于用户需要在分发结束后保存事件数据的场景。`clone()` 不共享传播状态。

### 7.3 鼠标事件分发

鼠标事件基于上一轮渲染生成的 `paintList` 命中：

```text
dispatchMouse(event)
  for widget in paintList reverse:
    if hit(event.x, event.y, widget.frame):
      event.target = widget
      bubble(widget, event)
      return
```

命中后只支持自下向上的冒泡：

```text
bubble(widget, event)
  current = widget
  while current exists:
    event.currentTarget = current
    dispatch current.onXXX(event)
    if event.propagationStopped:
      return
    current = current.parent
```

第七阶段不设计捕获阶段，不设计透明穿透，不设计复杂事件路径缓存。

### 7.4 键盘事件分发

键盘事件不走 `paintList`，由 `ViewManager.focusedWidget` 决定起点：

```text
dispatchKey(event)
  if focusedWidget exists:
    event.target = focusedWidget
    bubble(focusedWidget, event)
  else:
    event.target = root
    bubble(root, event)
```

焦点由 `ViewManager` 统一维护。鼠标点击可根据 target 的 `focusable` 状态更新焦点。

## 8 与 TuiApp/InputManager 的集成

第七阶段集成后，`TuiApp` 持有：

```feng
open type TuiApp {
  let screen: Screen;
  let input: InputManager;
  let view: ViewManager;
}
```

`ViewManager` 注册到 `InputManager` 的回调字段，将基础 `KeyEvent`/`MouseEvent` 转换或包装为 `WidgetEvent` 后，按事件类型路由到命中组件或焦点组件的 `onKey`/`onMouseDown`/`onMouseMove`/`onMouseUp`/`onWheel` 字段。

`TuiApp.run()` 中 stdin 解析完成后仍由主循环触发渲染；区别是渲染入口从直接使用 `Screen` 扩展为调用 `ViewManager.render()`。

## 9 文件规划

第七阶段预计新增或修改：

```text
std/src/tui/
  Widget.ff          # Widget spec
  BaseWidget.ff      # BaseWidget 默认实现
  WidgetStyle.ff     # 声明式布局约束
  WidgetFrame.ff     # 布局调度后的计算结果
  WidgetEvent.ff     # 视图层事件对象
  WidgetChildren.ff  # 子组件集合与 parent 维护
  ViewManager.ff     # 组件树、布局、绘制、事件路由、焦点
  TuiApp.ff          # 新增 view 成员并接入 ViewManager
```

不新增 Text/Button 等高级组件文件。

## 10 实施步骤

第七阶段按以下顺序实施：

1. 定义 `WidgetStyle`、`WidgetFrame`、基础布局类型（size/offset/insets/align/position）；
2. 定义 `Widget` spec 与 `BaseWidget`；
3. 实现 `WidgetChildren`，确保 parent 由树 API 自动维护；
4. 实现 `WidgetEvent` 引用语义、传播状态与 clone；
5. 实现 `ViewManager` 的 root/focus/paintList 基础结构；
6. 实现 layout 调度阶段：由 `ViewManager` 发起，组件或布局组件将声明值解析为 `WidgetFrame`；
7. 实现 draw 阶段：清空 `paintList`、遍历组件树、draw 前追加组件；
8. 实现鼠标 hit test 与自下向上冒泡；
9. 实现键盘焦点路由与自下向上冒泡；
10. 集成 `ViewManager` 到 `TuiApp`；
11. 补充 std_test 用例；
12. 执行全量回归测试 `make test`；
13. 等待人工 Review，通过后再进入后续组件扩展阶段。

## 11 Review 关注点

进入实现前需要确认：

- `Widget` spec 具体签名是否符合当前 Feng spec 能力；
- `WidgetStyle` 中 `full` 与 `width/height` 同时设置时，是否确定为 `full` 优先；
- `absolute` 是否固定为相对父容器 content rect；
- `fixed` 是否固定为相对 screen rect，且是否不受父级 clip 影响；
- `WidgetEvent` 是否采用统一事件类型，还是按 key/mouse 派生多个事件类型；
- `paintList` 存 `Widget` 是否满足第一版命中需求；
- `WidgetChildren` 的公开 API 是否足够防止用户绕过 parent 维护。
