# Feng TUI 焦点与键盘路由开发方案

> 状态：基础焦点及 Tab 正向/反向切换均已实施，并通过 std_test 与全量回归，等待人工 Review。
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

本专项开始实施前具有以下基础能力：

- `InputManager<Widget>` 通过单播 `onKey`、`onMouse` 输出解析后的输入事件；
- `TuiApp` 已将 `input.onMouse` 绑定到 `ViewManager.dispatchMouse`，但尚未接管
  `input.onKey`；
- `Widget` 已提供 `key`、`mouseDown`、`mouseMove`、`mouseUp`、`wheel` 多播事件；
- `ViewManager` 已根据 lock 或本帧 `clippedFrame` 确定鼠标 target，并沿 parent 冒泡；
- `MouseEvent<Widget>` 已是引用类型，具有稳定 target 以及 `stop()`/`isStopped()`；
- `KeyEvent<Widget>` 当时仍是 `@value` 类型，尚无传播停止状态；
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

焦点变化通知在基础路由稳定后按 7.1 节追加实现；具体组件的焦点视觉效果仍由组件自行
处理。

## 5 ViewManager 多播事件

`ViewManager` 提供与 `Widget` 完全相同的事件集合和事件类型：

```feng
open type ViewManager {
  let onFocus: Event<FocusEvent<Widget>>;
  let onBlur: Event<FocusEvent<Widget>>;
  let key: Event<KeyEvent<Widget>>;
  let mouseDown: Event<MouseEvent<Widget>>;
  let mouseMove: Event<MouseEvent<Widget>>;
  let mouseUp: Event<MouseEvent<Widget>>;
  let wheel: Event<MouseEvent<Widget>>;
}
```

键盘和鼠标事件位于 Widget 传播链的 root 之后；焦点事件遵循 7.1 节的路径差分规则：

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

1. 根据 lock 或 `clippedFrame` 命中确定 target；
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
  seal var prevented: bool;

  func stop(): void;
  func isStopped(): bool;
  func preventDefault(): void;
  func isPrevented(): bool;
}
```

`preventDefault()`/`isPrevented()` 与 `stop()`/`isStopped()` 相互独立：停止传播不自动
阻止默认行为，阻止默认行为也不停止传播。后续 Tab 切换作为键盘默认行为实现时，输入框
等组件可以在 key 监听器中调用 `preventDefault()`，保留 Tab 输入而不切换焦点。

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

### 7.1 FocusEvent 与焦点路径事件

焦点事件不携带可变传播状态，使用无额外对象分配的值类型：

```feng
@value
open type FocusEvent<T> {
  let target: T;
}
```

`FocusEvent` 不提供 `stop()`/`isStopped()` 或
`preventDefault()`/`isPrevented()`。Widget 与 ViewManager 增加：

```feng
let onFocus: Event<FocusEvent<Widget>>;
let onBlur: Event<FocusEvent<Widget>>;
```

焦点事件表达“焦点进入或离开当前 Widget 子树”，按新旧焦点的共同祖先路径差分触发：

1. ViewManager 分别维护旧焦点和新焦点从 target 到 root 的有序路径；
2. 从 root 端比较两条路径，找到最近公共祖先；
3. 先从旧 target 向上触发 `onBlur`，到最近公共祖先之前结束；
4. 再从新 target 向上触发 `onFocus`，到最近公共祖先之前结束；
5. 公共祖先及其上层不重复触发事件，焦点事件不可停止；
6. `FocusEvent.target` 在 blur 路径中始终是旧焦点，在 focus 路径中始终是新焦点；
7. 从无焦点进入组件树时，focus 路径最终触发 `ViewManager.onFocus`；清除或失效焦点时，
   blur 路径最终触发 `ViewManager.onBlur`；两个有效焦点之间切换时 ViewManager 不重复触发；
8. 重复聚焦同一 Widget 不触发事件；焦点切换过程中监听器再次请求焦点时，ViewManager
   完成本轮不可停止事件后串行处理最后一次请求，避免递归破坏焦点状态。

`tabIndex < 0` 的 Widget 不能作为焦点 target：显式 `focus(widget)` 返回 false；鼠标
默认行为继续向上查找最近的可聚焦祖先。不可聚焦 Widget 仍可作为焦点路径中的祖先，
在焦点首次进入或最终离开其子树时收到 `onFocus`/`onBlur`。焦点 Widget 脱离组件树或
变为不可聚焦时，ViewManager 使用已缓存的旧路径完整触发 blur，不依赖已经变化的 parent。

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
  `hasTarget()`、`target`、`isStopped()` 和 `isPrevented()`；
- Ctrl+C、退出应用等不能被局部 Widget 阻止的快捷键订阅 `TuiApp.key`；
- `examples/tui_demo` 从直接覆盖 `app.input.onKey` 改为订阅 `app.key`；
- `InputManager.onKey` 继续保持单播，不改造成 Event，也不维护监听器集合。

本阶段不为 TuiApp 增加鼠标事件。鼠标仍由 `InputManager.onMouse` 直接进入
`ViewManager.dispatchMouse`。

## 9 Tab 切换契约

### 9.1 顺序来源与候选资格

Tab 始终使用 `ViewManager.sequence` 作为顺序来源，不遍历组件树，也不因未来引入
`zIndex` 改用其他顺序。`sequence` 是上一轮完整绘制形成的交互快照；尚未绘制、完全
裁剪或没有有效绘制区域的 Widget 不参与本次切换。

每个候选项的顺序键为 `(tabIndex, sequenceIndex)`：先按非负 `tabIndex` 从小到大排序，
同值时按 `sequence` 登记位置保持稳定顺序。反向切换使用该顺序的完全逆序。候选 Widget
还必须满足当前 `rtStyle.visibility == Visibility.Visible` 且仍属于当前 root；
`pointerEvents` 只控制鼠标，不影响键盘焦点资格。

### 9.2 正向、反向与循环

- Tab 从当前候选的顺序键移动到下一个候选，到达末尾后循环到最小候选；
- Shift+Tab 移动到上一个候选，到达开头后循环到最大候选；
- 没有当前焦点，或当前焦点不再是 `sequence` 中的有效候选时，Tab 选择最小候选，
  Shift+Tab 选择最大候选；
- 只有一个候选时允许选择自身，由既有 `focus()` 幂等语义保证不重复触发焦点事件；
- 没有候选时保持当前焦点，不产生焦点事件。

终端普通 Tab 字节 `0x09` 继续产出 `SpecialKey.Tab`；反向 Tab 的标准 `CSI Z`
（`ESC [ Z`）产出 `SpecialKey.Tab + MOD_SHIFT`。键盘默认行为只处理无修饰 Tab 和仅带
`MOD_SHIFT` 的 Tab；含其他修饰位的合成事件不触发焦点切换。

### 9.3 默认行为时序

Tab 切换是 `ViewManager.dispatchKey()` 的默认行为：先使用按键发生时的旧焦点完成 Widget
和 ViewManager 路由，再在路由结束后检查 `event.isPrevented()` 并切换焦点。因此事件的
`target` 始终保持旧焦点；切换完成后 `focused()` 返回新焦点。`stop()` 只停止后续视图
传播，不阻止默认行为；Widget 或 ViewManager 需要保留 Tab 时必须调用
`preventDefault()`。`TuiApp.key` 位于 ViewManager 默认行为之后，能够观察切换结果，
但不能事后取消已经完成的切换。

## 10 性能约束

- 每次鼠标或键盘分发复用 InputManager 创建的同一事件，不克隆载荷；
- ViewManager 构造时创建并复用两条有序焦点路径；焦点切换不创建 List、HashSet 或临时
  组件集合，并可在组件脱离树后使用旧路径完成 blur；
- ViewManager 的多播 Event 在 ViewManager 构造时创建，不在每次分发时创建；
- Tab 切换不排序、不从 root 枚举组件树，也不创建临时集合；先定位当前焦点的
  `sequenceIndex`，再顺序扫描候选，同时选择相邻项和循环项。顺序选择为 O(n)；候选
  通过既有 parent 链确认当前 root 归属，最坏复杂度为 O(nh)，其中 h 为组件树深度；
- `FocusEvent` 使用值类型；除已经明确列出的 KeyEvent 引用类型分配外，不增加新的
  每事件运行时分配。

## 11 测试范围

新增 std_test 用例覆盖：

- `tabIndex` 默认值、可聚焦判断、不可聚焦拒绝和重复 focus；
- `focus()`、`clearFocus()`、`focused()` 以及脱离 root 后的失效焦点；
- 鼠标点击 target、最近可聚焦祖先和无可聚焦祖先；
- `preventDefault()` 的幂等性，以及它与 `stop()` 的独立语义；
- Widget 鼠标事件到 root、ViewManager 的顺序和停止规则；
- 未命中 Widget 时直接触发 ViewManager 鼠标事件；
- KeyEvent target、逐级冒泡、停止传播、ViewManager.key 和无焦点路径；
- KeyEvent 阻止默认行为的幂等性，以及它与停止传播的独立语义；
- FocusEvent target、blur-before-focus 顺序、共同祖先不重复触发和 ViewManager 边界；
- 不可聚焦 target、可聚焦祖先、重复 focus、clearFocus 和失效焦点的事件行为；
- 焦点 Widget 脱离组件树后仍按缓存旧路径完整 blur，以及监听器内重入切换的串行行为；
- Widget 停止键盘传播后，`TuiApp.key` 仍被触发；
- TuiApp 多个 key 监听器、退订及 Ctrl+C 所需的应用级路径；
- InputManager 单播出口、ViewManager 路由和 TuiApp.key 的集成；
- `CSI Z` 的 Shift+Tab 解析，以及真正未知 CSI 仍被丢弃；
- `tabIndex` 数值顺序、同值 `sequence` 稳定顺序、正反向循环和无焦点起点；
- 不可聚焦、未登记、不可见、脱离 root 和 `pointerEvents == None` 的候选规则；
- Tab 默认行为与 `stop()`、`preventDefault()`、旧事件 target 及新焦点的时序；
- `examples/tui_demo` 改用 `app.key` 后可以构建并正常退出。

人工终端验证在 `examples/tui_demo` 的同一 VStack 中连续放置三个默认
`tabIndex == 0` 的 Input：首个 Input 初始获得焦点，Tab 应按 `sequence` 自上而下循环，
Shift+Tab 应按相反方向循环；每个 Input 保持独立编辑值、焦点样式和 submit 行为。

测试只新增或按人工批准调整相关 TUI 用例。此前基础阶段已经执行 `make test` 全量回归；
本轮仅变更 std 及 std_test，按人工确认构建 std 并完整回归 std_test。

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
- [x] 12.15 为 KeyEvent 增加 `preventDefault()`/`isPrevented()` 及独立状态用例；
- [x] 12.16 增加不可停止、不可阻止默认行为的值类型 `FocusEvent<T>`；
- [x] 12.17 为 Widget、ViewManager 增加 `onFocus`/`onBlur` 多播事件；
- [x] 12.18 使用 ViewManager 构造期创建的两条有序路径实现共同祖先差分和失效 blur；
- [x] 12.19 串行处理焦点事件监听器中的重入焦点请求；
- [x] 12.20 补齐 KeyEvent 默认行为和 FocusEvent 路径切换 std_test 用例；
- [x] 12.21 构建 std 并回归 std_test，更新实施状态；
- [x] 12.22 将真实焦点目标同步到 `Pseudo.Focus`，覆盖切换、清除、失效、重入和状态样式时序；
- [x] 12.23 确认 Tab 始终使用 `sequence`，并收敛顺序、候选、循环和默认行为契约；
- [x] 12.24 将 `CSI Z` 解析为 `SpecialKey.Tab + MOD_SHIFT`；
- [x] 12.25 实现 ViewManager 无排序、无临时集合的 Tab 正向/反向焦点切换；
- [x] 12.26 补齐 Tab 解析、顺序、循环、过滤和默认行为 std_test 用例；
- [x] 12.27 执行定向测试与全量回归，更新实施状态并等待人工 Review。
- [x] 12.28 在 `tui_demo` 中加入三个连续 Input，构建并交付人工终端验证 Tab/Shift+Tab。

## 13 实施结论与后续 Review

- `focus(widget)` 使用 `bool` 返回是否成功；不可聚焦或不属于 root 时保持原焦点；
- 点击未命中或不存在可聚焦祖先的区域时保持原焦点；
- 自动聚焦覆盖除滚轮外的全部 `MouseAction.Press`；
- KeyEvent 使用引用类型，共享 target、停止状态和默认行为状态；该分配变化已经人工确认；
- Widget 与 ViewManager 已提供不可停止的 `onFocus`/`onBlur`，共同祖先不重复触发；
- `Pseudo.Focus` 只表示真实焦点目标，不传播到祖先；祖先焦点子树语义未来使用独立的
  `Pseudo.FocusWithin` 表达；
- 此前 `make test` 已完整通过，其中 std_test 521/521、fcts 768/768；本轮按人工确认仅
  构建 std 并回归 std_test，结果为 524/524；
- `Pseudo.Focus` 状态同步新增用例后，std_test 580/580、fcts 816/816，沙箱外完整
  `make test` 通过；
- Tab 顺序、循环和阻止规则已按第 9 节完成实施；新增用例后 std_test 594/594、
  fcts 816/816，沙箱外完整 `make test` 通过。
