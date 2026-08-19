# Feng TUI Input 组件开发方案

> 状态：已实施并通过 std_test、全量回归与真实终端验证，待人工 Review。
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 中单行 Input 组件阶段的
> 唯一专项规范。终端输入解析仍以 `docs/engineering/feng-std-tui-input-dev.md`
> 为准；焦点、键盘路由与 `Pseudo.Focus` 同步仍以
> `docs/engineering/feng-std-tui-focus-key-routing-dev.md` 为准；本文只定义
> Input Widget 的公开契约和组件行为。

## 1 目标与范围

本阶段实现生产可用的单行文本 Input：

- 公开受控的字符串值、grapheme caret 位置、`change` 和 `submit` 事件；
- 默认可聚焦，并复用现有焦点、键盘冒泡和 `Pseudo.Focus` 样式管线；
- 按 extended grapheme cluster 移动和删除，避免拆开组合字符或 Emoji；
- 支持可打印 Unicode 输入、Left/Right、Home/End、Backspace/Delete 和 Enter；
- 固定宽度不足时按 grapheme 边界水平滚动，并始终尽力保持 caret 可见；
- 复用 Text/Buffer 的显示列宽度与 8 字节 Cell 内联规则；
- 支持鼠标左键按显示列定位 caret，并继续使用 ViewManager 的默认聚焦行为；
- 保持逐帧绘制热路径无 substring、List 或逐 grapheme 堆分配。

本阶段不实现多行文本、文本选择、Shift 选择、单词级移动、剪贴板、撤销/重做、
placeholder、password、输入法预编辑（IME composition）、验证规则、Tab 焦点遍历或
终端原生物理光标。上述能力分别在后续专项阶段设计，不在首版中预留特判。

## 2 分层边界

Input 是 `std.tui.widgets` 中的 Widget，不是 `std.tui.input.InputManager` 的替代品：

- InputManager 只把终端字节流解析成 `KeyEvent<Widget>` / `MouseEvent<Widget>`；
- ViewManager 选择焦点或命中目标并完成事件冒泡与默认聚焦；
- Input 只在自身成为事件目标时解释编辑按键、维护文本编辑状态并绘制内容；
- Screen、Buffer 和 Cell 不感知 Input，也不增加 Input 专用分支；
- Input 不直接更改 `Pseudo.Focus`，该状态继续由 ViewManager 根据真实焦点统一同步。

Input 的内部键盘编辑与鼠标 caret 定位复用 Widget 已公开的 `key` / `mouseDown` Event，
并在构造时注册为首个监听器，不额外扩展 Widget/ViewManager 的事件钩子。该内部监听器
计入 Event 的监听器数量；调用方对这两个 Event 执行 `clear()` 时会连同 Input 内部行为
一起清除。这是公共 Event 的既有统一语义，不为 Input 增加隐藏监听器层。

Tab 保持未消费，由后续 ViewManager 焦点遍历统一解释。Input 不插入制表符，也不在
组件内部自行查找下一个焦点目标。

## 3 公开契约

```feng
open spec InputWidget: Widget {
  /** 当前单行值。 */
  func value(): string;

  /** 程序化设置单行值；不触发 change，并将 caret 移到末尾。 */
  func value(value: string): void;

  /** 返回 caret 前的 extended grapheme cluster 数。 */
  func caret(): int;

  /** 按 extended grapheme cluster 索引设置 caret。 */
  func caret(index: int): void;

  /** 用户编辑实际改变值后触发。 */
  let change: Event<string>;

  /** 用户按下无修饰键 Enter 时触发。 */
  let submit: Event<string>;
}

open type Input: InputWidget {
  ...: View = View();
}
```

Input 构造后的默认配置为：

- `tabIndex = 0`，因此可以通过 `ViewManager.focus()` 或鼠标默认行为获得焦点；
- `style.height = auto`、`style.verticalAlign = Align.Start`，默认内在高度为一行；
- `style.cursor = Cursor.Text`，只描述支持该协议的终端中的鼠标指针形状；
- 宽度保留 View 的默认声明：普通流中默认横向 Full，Absolute/Fixed 或显式
  `Align.Start/Center/End` 时可使用 Input 的内在宽度。

`Cursor.Text` 与文本 caret 无关。Input 的 caret 由组件绘制，不能用鼠标指针状态代替。

## 4 值、caret 与事件

### 4.1 单行值不变量

Input 内部保存 `_value: string`，不公开可直接赋值的 `var`，从而保证所有修改都经过
同一条校验、caret 和 reflow 管线。

- 值允许为空并允许任意合法 UTF-8 Unicode scalar；
- 值不得包含 CR (`\r`) 或 LF (`\n`)；程序化 setter 遇到二者时抛出
  `"tui/input/multiline-value"`，原值、caret 和滚动位置保持不变；
- 程序化 setter 与当前值相同时不做任何修改；
- 程序化 setter 成功后将 caret 移到新值末尾、重置水平滚动基点并请求 reflow；
- 程序化 setter 不触发 `change`，避免数据绑定写回形成事件循环。

### 4.2 caret 表示

公开 caret 位置是“caret 之前的 extended grapheme cluster 数”，范围为
`0..graphemeCount`。setter 超出该范围时抛出 `"tui/input/caret-out-of-bounds"`，
已有状态保持不变。

内部使用 `_caretOffset` 保存 UTF-8 字节偏移，并始终维持在 grapheme 边界。这样插入、
删除和局部移动不需要把整个字符串转换为码点或 grapheme 集合。公开索引与内部偏移只在
显式调用 `caret()` 方法时转换。

### 4.3 组件事件

- `change` 只在用户插入或删除导致值实际变化后触发一次，载荷是修改后的完整值；
- 边界处的 Backspace/Delete 不改变值，也不触发 `change`；
- `submit` 在无修饰键 Enter 到达 Input 时触发一次，载荷是触发瞬间的完整值；
- caret 移动、鼠标定位和程序化 setter 不触发 `change`；
- 组件先完成内部状态修改，再同步触发事件，因此监听器读取 `value()` / `caret()` 时
  能看到最新状态。

## 5 键盘编辑

Input 只处理 `mods == 0` 的首版编辑按键：

| 按键 | 行为 |
| --- | --- |
| `u32` 可打印分支 | 校验为 Unicode scalar 后编码为 UTF-8，在 caret 处插入 |
| Left / Right | 移到前一个 / 后一个 grapheme 边界 |
| Home / End | 移到值开头 / 末尾 |
| Backspace | 删除 caret 前一个 grapheme |
| Delete | 删除 caret 后一个 grapheme |
| Enter | 触发 `submit` |

已识别的编辑按键无论是否真正移动或删除，都调用 `stop()` 和 `preventDefault()`：

- `stop()` 防止父 Widget 把同一按键再次解释为导航或编辑；
- `preventDefault()` 保留未来 ViewManager 键盘默认行为的扩展边界；
- 现有 TuiApp 契约保持不变，应用级 `TuiApp.key` 仍会收到事件。

Tab、带 Control/Shift/Alt 标志的事件、未列出的特殊键和无效 Unicode scalar 均不消费，
也不改变 Input 状态。首版不把 Shift+方向键降级为普通移动，不提前替代未来选择语义。

每次文本修改只创建最终新字符串，不维护可变 gap buffer 或 rope。单行 Input 的编辑
复杂度为 O(n) 字节复制；后续若要支持大文本或多行编辑，应独立设计编辑存储结构，不能
把该复杂度扩散到当前通用 Widget/Screen 契约。

## 6 grapheme 与 Cell 规则

Input 必须复用 libunistring 的 `u8_grapheme_next` / `u8_grapheme_prev` 识别 extended
grapheme cluster，并复用 `TextUtil` 的 UTF-8 Cell 编码和显示列宽度规则：

- Left/Right、Backspace/Delete 和鼠标定位均只产生 grapheme 边界；
- 一个 grapheme 不超过 8 字节时完整编码到一个主 Cell；
- 超过 8 字节时只在合法码点边界拆成多个 Cell 值，不能截断 UTF-8；
- 多列 Cell 的续占仍由 Buffer 写入 `Cell.CONTINUATION`；
- Input 不新增 runtime ABI，只复用 std.text 已使用的指针 helper 与随 libunistring
  已提供的 grapheme API。

Input 值本身是合法 Feng string。只有手工构造的 `KeyEvent` 可能携带 surrogate 或大于
`0x10FFFF` 的无效值；Input 对这类事件保持未消费，不尝试修复或插入替代字符。

## 7 布局与 Reflow

Input 按 View 的 reference、position、margin 和 Align 规则计算 frame，只覆写自身的
内在宽高：

- 内在高度恒为 1；
- 内在宽度为完整值的显示列数加 1，额外一列保证 Auto 宽度下末尾 caret 可见；
- 空值的内在宽度为 1；
- 显式固定、百分比或 Full 尺寸继续服从 View 的既有规则，不因内容增长自动扩张；
- `style.padding` 仍只用于容器 child content 区域，Input 不重新解释 padding。

程序化 setter 和用户文本修改会给 Input 标记 `DirtyType.Layout`，并通过既有
`requestReflow()` 通知直接父组件。纯 caret 移动不改变内在尺寸，不请求布局。

## 8 水平滚动

Input 使用 `_scrollOffset` 保存当前可见内容起点的 UTF-8 字节偏移，并保持在 grapheme
边界。滚动不是公开 ScrollView，也不改变 frame：

1. caret 位于滚动起点左侧时，起点直接回退到 caret；
2. 从起点到 caret 的显示列数加 caret 当前占用宽度超过 frame 时，逐 grapheme 前移
   起点，直到 caret 能完整显示或已到 caret；
3. 仍有剩余宽度时，从当前起点向前回填尽可能多的完整 grapheme，使变宽后的 Input
   自动恢复更多上下文；
4. caret 在值末尾时占 1 个反色空白 Cell；位于内容前时使用下一个 grapheme 的显示
   宽度；
5. 若固定 frame 窄于下一个 grapheme 的显示宽度，内容无法完整落入时不拆开该
   grapheme，焦点状态下改绘 1 个反色空白 Cell 表示 caret；
6. `frame.width <= 0` 时不绘制内容，也不产生无效 Buffer 坐标。

滚动调整与绘制均直接扫描原字符串，不创建可见 substring 或中间集合。

## 9 绘制与可见 caret

Input 使用完整 frame 确定内容原点，使用 `clippedFrame` 只做祖先和 Screen 裁剪：

- 先按 View 的背景色语义填充完整可见 frame；
- 没有不透明背景色时，仍用透明前景/背景的空 Cell 清理第一行可见编辑区域，清除上一帧
  的字符与 caret 样式并保留既有颜色通道，避免删除、滚动或失焦留下残影；
- 内容只绘制在 `frame.y` 的第一行，额外 frame 高度保持背景；
- 普通文本使用 `rtStyle.foreColor`，透明背景保留已经填充或底层的背景色；
- 只绘制完整落入水平 `clippedFrame` 的 Cell，外部左裁剪不能把内容重新贴到裁剪边界；
- 只有 `ViewManager.focused()` 的真实目标就是当前 Input 时才绘制 caret；
- caret 使用 `CellStyle.Reverse` 绘制下一个完整 grapheme；位于末尾或内容无法完整放入时
  绘制一个反色空白 Cell；
- TuiApp 继续隐藏终端原生物理光标。Input 不改变 Screen diff 的最终物理光标位置，
  从而避免多个组件竞争全局终端光标，也不新增光标闪烁计时器。

组件绘制的 caret 属于 back buffer 内容，因此自动参与现有双缓冲 diff 和祖先裁剪。
焦点变化后下一帧会移除旧 Input 的 caret，并在新 Input 绘制 caret；`Pseudo.Focus`
样式与 caret 都读取同一真实焦点，不建立第二套焦点状态。

## 10 鼠标定位

Input 在自身 `mouseDown` 中只处理左键 Press：

- 根据 `event.x - frame.x` 和当前滚动起点换算为可见显示列；
- 点击位于 grapheme 前半区域时选择其前边界，后半区域时选择其后边界；
- 点击超过可见内容末尾时选择值末尾；
- 宽 grapheme 仍作为一个不可拆分的定位单位；
- 处理后调用 `event.stop()`，避免父组件重复解释同一左键按下；
- 不调用 `preventDefault()`，让 ViewManager 在路由结束后继续执行既有鼠标默认聚焦。

Input 不主动调用 `ViewManager.focus()`，也不复制可聚焦祖先查找规则。右键、中键、滚轮、
move 和 release 保持未处理。

## 11 文件与测试

新增实现文件：

```text
std/std/src/tui/widgets/Input.ff
```

在 `std/std_test/src/test_tui.ff` 只新增 Input 测试与注册，不修改既有测试语义。至少覆盖：

- 默认配置、空值、程序化 setter、CR/LF 拒绝和 caret 越界；
- ASCII、中文、组合字符、Emoji 与超过 8 字节 grapheme 的插入、移动和删除；
- Home/End、边界 Backspace/Delete、change/submit 次数与事件后状态；
- 修饰键、Tab 和无效 scalar 不消费；
- Auto 尺寸、固定宽度水平滚动、变宽回填、宽 grapheme 窄 frame；
- 焦点 caret、失焦 caret、`Pseudo.Focus` 样式和 Buffer Cell 样式；
- 鼠标定位、停止冒泡以及未阻止 ViewManager 默认聚焦；
- Screen/Hidden 祖先裁剪保持内容原点。

`examples/tui_demo` 增加一个可实际输入和提交的 Input，用于人工验证焦点样式、Unicode
编辑、水平滚动和 caret 显示；Tab 切换不作为本阶段验收项。

实施顺序固定为：规范 → 公共契约与编辑状态 → grapheme/滚动/绘制 → 新增测试 →
demo → 定向测试 → 沙箱外 `make test` 全量回归 → 人工 Review。

若实施中需要新增 runtime ABI、增加逐帧堆分配或其他运行时开销、加入非通用特判，或
确认触发编译器缺陷，则停止实施并交由开发者决策。
