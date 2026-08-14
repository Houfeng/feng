# Feng TUI 焦点与键盘路由开发方案

> 状态：已实施，等待人工 Review。
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 中焦点管理与键盘焦点路由阶段的
> 专项开发文档。既有鼠标命中、target、lock 和 Widget 冒泡规则仍以
> `docs/engineering/feng-std-tui-view-dev.md` 为准；本文只定义本阶段新增或调整的契约。

## 1 目标

本阶段在现有 `InputManager<Widget>`、`ViewManager` 和 Widget 多播事件基础上完成：

- 使用 `tabIndex` 表达 Widget 是否可以获得焦点，并为后续 Tab 顺序提供依据；
- 由 `ViewManager` 保存和切换当前焦点；
- 鼠标点击可聚焦 Widget 时自动切换焦点；
- 允许鼠标事件阻止自动切换焦点，但不混淆默认行为与事件传播；
- 将键盘事件发送给当前焦点 Widget，并沿 parent 链向上冒泡；
- 让 `ViewManager` 提供与 `Widget` 完全相同的多播事件；
- 让 `TuiApp` 提供不可被视图传播阻止的应用级键盘多播事件，用于 Ctrl+C 等全局快捷键；
- 保持 `InputManager` 的 `onKey`、`onMouse` 为单播解析出口。

本阶段不完善 Text、Button 等具体组件，也不引入捕获阶段、焦点作用域、焦点陷阱或
复杂快捷键系统。

## 2 实施前基线

当前实现具有以下基础能力：

- `InputManager<Widget>` 通过单播 `onKey`、`onMouse` 输出解析后的输入事件；
- `TuiApp` 已将 `input.onMouse` 绑定到 `ViewManager.dispatchMouse`，但尚未接管
  `input.onKey`；
- `Widget` 已提供 `key`、`mouseDown`、`mouseMove`、`mouseUp`、`wheel` 多播事件；
- `ViewManager` 已根据 lock 或本帧 `drawFrame` 确定鼠标 target，并沿 parent 冒泡；
- `MouseEvent<Widget>` 已是引用类型，具有稳定 target 以及 `stop()`/`isStopped()`；
- `KeyEvent<Widget>` 目前仍是 `@value` 类型，尚无传播停止状态；
- `examples/tui_demo` 当前直接给 `app.input.onKey` 赋值处理 Ctrl+C。

焦点路由接入后，应用不应继续直接替换 `app.input.onKey`，否则会绕过 ViewManager 的
键盘路由。应用级按键处理统一订阅 `TuiApp.key`。

## 3 Widget 的焦点标识

`Widget` 增加公开的 `tabIndex: int`：

```feng
open spec Widget {
  var tabIndex: int;
  // 其他现有成员保持不变
}
```

规则如下：

- 默认值为 `-1`，表示不能获得焦点；
- `tabIndex >= 0` 表示可以通过鼠标或 `ViewManager.focus(widget)` 获得焦点；
- 后续实现 Tab 切换时，`tabIndex` 同时作为焦点顺序依据；
- `tabIndex` 直接使用 `int`，不使用 `Option<int>`，避免额外的收窄判断；
- `View` 提供默认值，展开 `View` 的 Widget 自动获得同一基础行为；具体组件自行决定
  是否把默认值改为可聚焦值。

本阶段的基础焦点与键盘路由不依赖 Tab 遍历实现。Tab 顺序和同值排序在后续步骤中
基于实际组件登记机制实现，不阻塞本阶段的鼠标聚焦与键盘路由。

## 4 ViewManager 焦点状态

`ViewManager` 独立保存每个 TUI 应用的当前焦点，不使用事件类型的静态字段：

```feng
open type ViewManager {
  seal var _focused: Option<Widget>;

  open func focus(widget: Widget): bool;
  open func clearFocus(): void;
  open func focused(): Option<Widget>;
}
```

接口语义如下：

- `focus(widget)` 仅在 `widget.tabIndex >= 0` 且 Widget 属于当前 root 组件树时设置焦点；
- 设置成功返回 `true`，不可聚焦或不属于当前组件树时保持原焦点并返回 `false`；
- 对已经获得焦点的 Widget 重复调用 `focus()` 是幂等成功；
- `clearFocus()` 清除当前焦点，重复调用不报错；
- `focused()` 返回当前焦点；没有焦点时返回 `none`；
- 键盘分发前需要确认保存的 Widget 仍属于当前 root 组件树；已经被移除或 root 已变更时
  清除失效焦点，再按无焦点规则处理本次按键。

本阶段不增加 focus/blur 事件。焦点变化通知与具体组件的焦点视觉效果在基础路由稳定后
另行设计。

## 5 ViewManager 多播事件

`ViewManager` 提供与 `Widget` 完全相同的事件集合和事件类型：

```feng
open type ViewManager {
  let key: Event<KeyEvent<Widget>>;
  let mouseDown: Event<MouseEvent<Widget>>;
  let mouseMove: Event<MouseEvent<Widget>>;
  let mouseUp: Event<MouseEvent<Widget>>;
  let wheel: Event<MouseEvent<Widget>>;
}
```

这些事件位于 Widget 传播链的 root 之后：

- Widget 传播正常到达 root 后，再触发 ViewManager 对应事件；
- Widget 调用 `stop()` 后，不再触发后续父 Widget、root 或 ViewManager 对应事件；
- 当前 Widget 的全部多播监听器仍先执行完成，`stop()` 不打断同一 Event 的其他监听器；
- 鼠标没有命中 Widget 时，不存在 Widget 传播链，直接触发 ViewManager 对应事件；
- 键盘没有焦点 Widget 时，同样直接触发 `ViewManager.key`；
- ViewManager 事件使用路由中的同一事件实例，不克隆事件载荷。

ViewManager 事件属于视图路由，因此可以被 Widget 的 `stop()` 阻止。不可阻止的应用级
键盘处理由 `TuiApp.key` 承担，不改变 `stop()` 的传播语义。

## 6 MouseEvent 默认行为

`MouseEvent<T>` 增加独立的默认行为状态：

```feng
open type MouseEvent<T> {
  seal var prevented: bool;

  func preventDefault(): void;
  func isPrevented(): bool;
}
```

语义如下：

- `preventDefault()` 阻止 ViewManager 在本次鼠标事件后的默认行为；本阶段默认行为只有
  鼠标按下后的自动焦点切换；
- `isPrevented()` 返回是否已经调用 `preventDefault()`；
- 重复调用 `preventDefault()` 保持已阻止状态；
- `preventDefault()` 不停止传播，也不跳过当前 Event 中尚未执行的其他监听器；
- `stop()` 只停止 Widget 传播，不自动阻止默认行为；
- 需要同时停止传播并阻止焦点切换时，监听器分别调用 `stop()` 和
  `preventDefault()`。

鼠标事件处理顺序为：

1. 根据 lock 或 `drawFrame` 命中确定 target；
2. 从 target 开始沿 parent 向 root 分发，或在 `stop()` 后提前结束 Widget 传播；
3. Widget 传播正常到达 root 时触发 ViewManager 对应事件；未命中时直接触发
   ViewManager 对应事件；
4. 对鼠标按下事件，如果 `isPrevented()` 为 `false`，从 target 开始沿 parent 查找
   最近的 `tabIndex >= 0` Widget；
5. 找到后调用 `focus(widget)`；找不到时保持原焦点不变。

默认行为放在 ViewManager 事件之后，使 Widget 或 ViewManager 的鼠标监听器都可以调用
`preventDefault()`。停止 Widget 传播但未阻止默认行为时，仍按 target 执行自动聚焦。
滚轮、移动和释放事件不触发自动聚焦。

## 7 KeyEvent 与键盘焦点路由

键盘事件需要在 target、父 Widget、ViewManager 和 TuiApp 之间共享同一 target 和停止
状态。因此，`KeyEvent<T>` 从 `@value` 类型改为普通引用类型，并增加与 MouseEvent
一致的传播接口：

```feng
open type KeyEvent<T> {
  // 现有 target/content/mods 等成员保持语义不变
  seal var stopped: bool;

  func stop(): void;
  func isStopped(): bool;
}
```

`ViewManager.dispatchKey(event)` 按以下规则处理：

1. 检查当前焦点是否仍有效；失效时清除焦点；
2. 存在焦点时，将该 Widget 绑定为 `event.target`；
3. 从焦点 Widget 开始触发 `key`，沿 parent 向 root 冒泡，target 始终保持为最初焦点；
4. 任一 Widget 调用 `stop()` 后停止 Widget 传播，并且不触发 `ViewManager.key`；
5. 正常到达 root 后触发 `ViewManager.key`；
6. 没有焦点时不绑定 target，直接触发 `ViewManager.key`，此时
   `event.hasTarget()` 为 `false`。

改为引用类型会改变 KeyEvent 的运行时表示，并使输入解析时创建键盘事件对象。该变化是
共享传播状态所需的通用语义调整，不通过 sidecar、装箱或事件克隆规避；该运行时分配
变化已在实施前 Review 中确认。

## 8 TuiApp 应用级键盘事件

`TuiApp` 增加应用级多播事件：

```feng
open type TuiApp {
  let key: Event<KeyEvent<Widget>>;
}
```

TuiApp 在构造时独占 `InputManager.onKey` 的单播出口，其内部处理顺序为：

```text
InputManager.onKey
  -> TuiApp 内部键盘分发
     -> ViewManager.dispatchKey(event)
     -> TuiApp.key.emit(event)
```

规则如下：

- `TuiApp.key` 不属于 Widget 视图树，不是 Widget 传播链的一部分；
- 无论 Widget 是否调用 `stop()`，`TuiApp.key` 都会触发；
- TuiApp 监听器收到 ViewManager 已经处理过的同一事件，可以读取
  `hasTarget()`、`target` 和 `isStopped()`；
- Ctrl+C、退出应用等不能被局部 Widget 阻止的快捷键订阅 `TuiApp.key`；
- `examples/tui_demo` 从直接覆盖 `app.input.onKey` 改为订阅 `app.key`；
- `InputManager.onKey` 继续保持单播，不改造成 Event，也不维护监听器集合。

本阶段不为 TuiApp 增加鼠标事件。鼠标仍由 `InputManager.onMouse` 直接进入
`ViewManager.dispatchMouse`。

## 9 Tab 切换的阶段边界

`tabIndex` 在本阶段先完成公开契约、默认值和焦点资格判断。Tab 切换作为焦点基础能力
稳定后的独立步骤，至少需要覆盖：

- 按 `tabIndex` 数值确定切换顺序；
- 同一 `tabIndex` 下保持稳定顺序；
- 跳过不可聚焦及当前帧不可交互的 Widget；
- 到达末尾后的循环规则；
- 终端能够可靠表达的反向切换输入；
- Tab 默认切换与键盘事件传播之间的阻止规则。

上述细节尚未在本轮讨论中全部确定，因此不在基础焦点与键盘路由步骤中提前实现。

## 10 性能约束

- 每次鼠标或键盘分发复用 InputManager 创建的同一事件，不克隆载荷；
- 基础焦点切换只沿 parent 链查找，不为单次事件创建 List 或临时组件集合；
- ViewManager 的多播 Event 在 ViewManager 构造时创建，不在每次分发时创建；
- 本阶段不为 Tab 顺序引入每次按键排序；其数据结构和更新策略在 Tab 步骤实现前单独
  Review；
- 除已经明确列出的 KeyEvent 引用类型分配外，不增加新的每事件运行时分配。

## 11 测试范围

新增 std_test 用例覆盖：

- `tabIndex` 默认值、可聚焦判断、不可聚焦拒绝和重复 focus；
- `focus()`、`clearFocus()`、`focused()` 以及脱离 root 后的失效焦点；
- 鼠标点击 target、最近可聚焦祖先和无可聚焦祖先；
- `preventDefault()` 的幂等性，以及它与 `stop()` 的独立语义；
- Widget 鼠标事件到 root、ViewManager 的顺序和停止规则；
- 未命中 Widget 时直接触发 ViewManager 鼠标事件；
- KeyEvent target、逐级冒泡、停止传播、ViewManager.key 和无焦点路径；
- Widget 停止键盘传播后，`TuiApp.key` 仍被触发；
- TuiApp 多个 key 监听器、退订及 Ctrl+C 所需的应用级路径；
- InputManager 单播出口、ViewManager 路由和 TuiApp.key 的集成；
- `examples/tui_demo` 改用 `app.key` 后可以构建并正常退出。

测试只新增或按人工批准调整相关 TUI 用例。完成每个实现小组后运行相关定向测试；全部
完成后在非 Codex 沙箱环境执行 `make test` 全量回归。

## 12 实施 TODO

- [x] 12.1 人工 Review 并确认本文档，特别确认 KeyEvent 改为引用类型的分配变化；
- [x] 12.2 为 Widget/View 增加 `tabIndex: int` 及默认值，并补充基础用例；
- [x] 12.3 为 MouseEvent 增加 `preventDefault()`/`isPrevented()` 及状态用例；
- [x] 12.4 将 KeyEvent 改为引用类型，增加 `stop()`/`isStopped()`，更新对应输入用例；
- [x] 12.5 为 ViewManager 增加焦点状态和 `focus()`/`clearFocus()`/`focused()`；
- [x] 12.6 为 ViewManager 增加与 Widget 完全相同的五类多播事件；
- [x] 12.7 重构鼠标分发顺序，接入 ViewManager 事件和自动聚焦默认行为；
- [x] 12.8 实现 `ViewManager.dispatchKey()`、target 绑定、冒泡及停止传播；
- [x] 12.9 为 TuiApp 增加 `key` 多播事件，接管 `InputManager.onKey` 并组合视图与应用分发；
- [x] 12.10 更新 `examples/tui_demo`，将 Ctrl+C 改为订阅 `TuiApp.key`；
- [x] 12.11 补齐焦点、鼠标默认行为、键盘路由和三层事件集成的 std_test 用例；
- [x] 12.12 运行 std、std_test 和 tui_demo 定向构建与验证；
- [x] 12.13 在非 Codex 沙箱环境执行 `make test` 全量回归；
- [x] 12.14 根据实现结果更新 TODO 状态，等待人工 Review；
- [ ] 12.15 基础阶段通过 Review 后，单独设计并实施 Tab 正向/反向焦点切换。

## 13 实施结论与后续 Review

- `focus(widget)` 使用 `bool` 返回是否成功；不可聚焦或不属于 root 时保持原焦点；
- 点击未命中或不存在可聚焦祖先的区域时保持原焦点；
- 自动聚焦覆盖除滚轮外的全部 `MouseAction.Press`；
- KeyEvent 使用引用类型，共享 target 与停止状态；该分配变化已经人工确认；
- `make test` 已完整通过，其中 std_test 521/521、fcts 768/768；
- Tab 同值顺序、循环和阻止规则仍留到 12.15 单独决策与实施。
