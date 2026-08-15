# Feng TUI Text 与自动尺寸开发方案

> 状态：已实施并通过 std_test、全量回归及人工显示验证。
>
> 本文档是 `docs/engineering/feng-std-tui-dev.md` 中 Text 组件阶段的专项开发文档。
> Widget、Style、frame、drawFrame 和祖先裁剪的既有契约仍以
> `docs/engineering/feng-std-tui-view-dev.md` 为准；本文只定义自动尺寸和 Text 新增行为。

## 1 目标

本阶段完成：

- 使用零元素具名 tuple 表达 `Auto` 尺寸，不使用字符串或特殊数值；
- 让 `Style.width`、`Style.height` 支持固定尺寸、百分比尺寸和自动尺寸；
- 将当前 Text 验证骨架实现为具有公开 `content` 字段的实际 Widget；
- 根据 Text 的最终内容宽度完成硬换行、自动换行、自动高度和裁剪绘制；
- 支持每一行文本在 Text frame 内左对齐、居中和右对齐；
- 支持内容超出 Text 自身固定高度时裁剪或在最后可显示行绘制 `...`；
- 复用 `std.text` 的 grapheme 能力确定用户可见字符边界，支持 Emoji 与组合字符；
- 通过终端显示列宽度处理双列码点，使 Text 布局、Buffer 绘制与 Screen diff
  对同一列占用关系保持一致；
- 测量和绘制使用一致的分行结果，不创建逐行字符串副本；
- 保持现有 View、Buffer、Cell 的颜色、裁剪和字符存储契约。

本阶段不实现 VStack/HStack、Input、文本选择、光标、富文本、单词级断行、垂直文本对齐
或滚动。这些能力在 Text 基础行为稳定后分别设计；富文本由未来的 RichText 负责，滚动
由未来的 ScrollView 负责。文本选择后续由组件自身实现，不依赖终端原生选择兜底。

## 2 当前基线

当前实现具有以下限制：

- `Style.width`、`Style.height` 为 `Union<int, double>`，不能表达内容决定尺寸；
- `Align.Full` 只表示占满可用区域，不等同于自动尺寸；
- `View.arrange()` 只解析固定尺寸、百分比尺寸和 Full，不执行内容测量；
- `Text` 只有 `...: View = View()` 骨架，没有 `content`、测量和文本绘制；
- `Buffer` 的矩形文本绘制可以按区域连续换行，但没有 Text 布局所需的硬换行、
  自动高度和被裁剪后的内容偏移语义。

因此，Text 不能只在现有 `View.draw()` 后追加一次 `Buffer.draw()`：布局和绘制必须共享
同一套分行规则，裁剪也不能改变文本原本的行列位置。

## 3 Auto 的类型与值

自动尺寸使用与 `None` 相同的零元素具名 tuple 方案：

```feng
/** 根据组件内容计算尺寸。 */
open type Auto();

/** 自动尺寸值。 */
open let auto: Auto = ();
```

`Style` 调整为：

```feng
open type Style {
  var width: Union<Auto, int, double>;
  var height: Union<Auto, int, double>;
}
```

规则如下：

- `Auto` 不携带数据，不引入字符串分配或字符串比较；
- 对外使用具名值 `auto`，不要求使用方直接写缺少语义信息的 `()`；
- `Auto` 位于 Union 第一项，使字段默认零值自然表示 `auto`；
- `int` 和 `double` 的现有固定尺寸、百分比尺寸、margin 计算规则保持不变；
- `x`、`y` 和 `Thickness` 不增加 Auto，它们仍只接受固定值或百分比；
- Auto 只表示尺寸来源，不改变 `Rect.width`、`Rect.height` 的最终 `int` 表示。

## 4 Auto 与 Align 的关系

现有布局优先级保持不变：

1. Normal/Relative 组件在对应轴为 `Align.Full` 时占满扣除两侧 margin 后的可用区域，
   忽略该轴的 `width` 或 `height`，包括 `auto`；
2. Normal/Relative 组件在 Start/Center/End 时解析实际尺寸，再使用最终尺寸计算位置；
3. Absolute/Fixed 不使用 Align，`auto` 按组件的内容尺寸解析；
4. 固定值和百分比结果不因内容过大而自动扩张；超出 frame 的内容由绘制裁剪处理；
5. 不具有内在内容尺寸的基础 View 在 Auto 轴上的内在尺寸为 0。容器的 Auto 尺寸由后续
   布局容器阶段定义，本阶段不提前推断子组件总体尺寸。

这意味着默认 Align 为 Full 的普通流 View 仍保持当前占满行为；Auto 不会把现有默认
View 意外缩小为零。

## 5 Text 公开契约

Text 定义继承 Widget 的 `TextWidget` 组件契约，再通过 View 获得
Widget 基础状态、通用行为和事件：

```feng
open enum TextAlign {
  Left,
  Center,
  Right
}

open enum TextOverflow {
  Clip,
  Ellipsis
}

open spec TextWidget: Widget {
  /** 需要显示的文本内容。 */
  var content: string;

  /** 每一行文本在 frame 内的水平对齐方式。 */
  var textAlign: TextAlign;

  /** 内容超出 Text 自身高度时的显示方式。 */
  var textOverflow: TextOverflow;

  /** arrange 生成、draw 复用的内部分行信息。 */
  let lines: List<TextLine>;
}

open type Text: TextWidget {
  ...: View = View();

  /** 需要显示的文本内容。 */
  open var content: string;

  /** 每一行文本在 frame 内的水平对齐方式，默认左对齐。 */
  open var textAlign: TextAlign = TextAlign.Left;

  /** 内容超出 Text 自身高度时的显示方式，默认直接裁剪。 */
  open var textOverflow: TextOverflow = TextOverflow.Clip;

  /** arrange 生成、draw 复用的内部分行信息。 */
  let lines: List<TextLine> = List<TextLine>();

  @mixable
  static func arrange(text: TextWidget, manager: ViewManager): void;

  @mixable
  static func draw(text: TextWidget, manager: ViewManager): void;
}
```

Text 对外新增 `content` 和 `textAlign`。`lines` 是 arrange/draw 共享的内部状态；
当前与 Widget 的 `frame`/`drawFrame`/`parent` 一样，在 spec `seal` 成员尚未
落地时暂时作为 spec 成员，后续应改为 `seal`。

`Widget.style.horizontalAlign` 决定 Text 整个 frame 在布局参照区域中的位置；
`Text.textAlign` 只决定每一条最终显示行在该 frame 内的水平位置，二者互不替代。
`TextAlign.Left` 是默认值。Center/Right 按各行实际 Cell 数分别计算，因此不同长度的行
可以具有不同的起始列。本阶段不增加 Text 内部的垂直对齐；需要垂直定位时使用 Auto
高度的 Text，并由外层容器通过 Widget Align 定位。

Text 不增加面向使用方的自定义 `draw` API。`arrange` 和 `draw` 均为
`@mixable static` 方法，以 `TextWidget` 为首参契约，并按目标显式优先规则
替换 View 的对应实现。ViewManager 仍通过 Widget 的 `arrange(manager)`、
`draw(manager)` 调度 Text；`isAncestor` 等 Text 未覆盖的 Widget 行为继续由
View 自然 mix，Text 不单独重复实现。

Text 不是 ContainerWidget，也不持有子组件。Button 等上层组件需要文本时，将 Text
作为其内部子组件；事件仍按普通 Widget 路径命中和冒泡，不增加 Text/Button 特判。

## 6 Text 测量和自动换行

Text 按“先确定宽度，再确定分行和高度”的顺序布局：

1. 按现有 reference、position、margin 和 Align 规则确定宽度来源；
2. 固定、百分比或 Full 宽度形成有限的内容宽度，文本按该宽度自动换行；
3. Auto 宽度取内容中最长逻辑行的 Cell 数，此时没有额外的软换行；
4. 宽度确定后扫描 content，生成后续绘制复用的行信息；
5. Auto 高度取最终分行数；固定、百分比或 Full 高度保持现有解析结果；
6. Center/End 必须使用已经解析完成的 Auto 最终尺寸计算 x/y。

首版分行规则为：

- `\n` 是硬换行；`\r\n` 作为一个硬换行处理；
- 有限宽度内每写满一行即软换行，不增加单词边界或语言相关断行规则；
- 空 content 的内在宽高均为 0；末尾硬换行产生一个空的最终逻辑行；
- 内容宽度为 0 时不绘制字符，自动高度为 0；
- Text 的布局通过现有 `std.text` `GraphemeView`/`GraphemeIterator` 识别 grapheme
  边界；绘制阶段按布局记录的原字符串字节区间零分配重放，并复用与 `std.text`
  相同的 libunistring grapheme 边界能力，不在 `std.tui` 中重写分段算法；
- `TextUtil.init()` 在进程启动阶段通过 `setlocale(LC_CTYPE, "")` 启用当前环境的
  Unicode locale；`TuiApp.init()` 自动调用一次，直接使用 TextUtil/Buffer/Screen
  的调用方应在自身启动阶段显式调用；
- `TextUtil.measureCellWidth()` 通过 POSIX `wcwidth` 获取码点列宽，并按 grapheme
  汇总最终显示列数；组合码点的 0 列宽不再被提升为独立 Cell。测量热路径不执行
  locale 初始化，也不增加每次调用的初始化状态判断；
- Cell 的字符存储遵循 `feng-std-tui-dev.md` 3.1 节：不超过 8 字节的完整 UTF-8
  grapheme 内联保存在主 Cell 的 `u64 value` 中，不增加字段或改变 Cell 大小；
  占多列的 grapheme 在后续 Cell 的 `value` 中写入 `Cell.CONTINUATION`；
- `Cell.CONTINUATION` 不是空白字符，也不得编码输出；它只表示当前终端列
  属于前面的多列码点；
- Text 在上层按 grapheme 生成 Cell 值；超过 8 字节时在合法码点边界拆分，Buffer
  和 Screen 不参与拆分。没有背景色时向 Buffer 传入 `a == 0` 的 `RgbColor`，由
  Buffer 的通用颜色合成保留画布原背景；
- 一个已拆分 Cell 值能完整放入一行时不在中间软换行；其显示列数超过剩余宽度时
  才换行，保证有限宽度下布局始终能够推进；
- `TuiApp.exit()` 不恢复 locale；locale 是进程级状态，恢复可能覆盖应用后续主动
  设置的 locale，而退出 TUI 也不代表进程不再处理 Unicode 文本。

`Style.padding` 的现有含义是为子组件定义 content 区域。Text 不包含子组件，因此本阶段
不重新解释 Text 自身的 padding，也不把 padding 计入 Text 的内在尺寸；Button 等容器
可通过自身 padding 约束内部 Text。

## 7 布局结果与 Reflow 边界

Text 的 `arrange()` 负责把测量结果写入自身 frame，不要求 ViewManager 理解文本，也不
通过 ViewManager 布局子组件。本阶段保持组件自治：

- content、可用宽度或 Style 改变后，下一次 arrange 重新得到正确 frame；
- Auto 高度变化只改变 Text 自身 frame；普通 Container 不负责排列子组件；
- VStack/HStack 后续可调用子组件 arrange 并读取最终 frame，按容器规则决定是否再次
  排列，这一 Reflow 协议不在本文提前定义；
- 本阶段不增加全局脏标记、布局队列或 ViewManager 文本专用路径。

View 与 Text 必须复用统一的 reference、margin、Align、位置和裁剪计算，不复制两套
容易分叉的布局公式。本阶段不重构 View；Text 需要复用的现有 View `seal static`
帮助方法暂改为 `open static`。Widget 的公开 arrange 签名、固定尺寸和百分比尺寸的
现有结果均保持不变。

固定值和百分比的基础换算统一由 `View.resolveLength` 完成。该方法保留正负号；
`x/y` 直接使用结果，margin/padding 和 width/height 在各自布局语义中限制为
非负。`Auto` 不属于 length，只在 `resolveWidth`/`resolveHeight` 中转为内在尺寸；
不再另设 `resolveSize` 或 `resolveDimension`。

## 8 Text 绘制与裁剪

Text 使用完整 frame 进行分行，使用 drawFrame 只做可见区域裁剪：

- drawFrame 继续由自身 frame、Screen 和最近的 `overflow == Hidden` 祖先 frame 求交；
- Left 从 frame 左侧开始绘制；Center 使用 `(frame.width - line.cells) / 2`；Right 使用
  `frame.width - line.cells`，所有偏移均按每一行独立计算；
- 左侧或上侧被裁剪时，跳过对应的原始文本列或行，不能把被裁剪内容重新排到
  drawFrame 左上角；
- 水平裁剪以对齐后的行起点为基准，只绘制该行与 drawFrame 的交集；裁剪不能改变
  对齐结果，也不能把可见字符重新贴到 frame 或 drawFrame 左侧；
- 裁剪宽度不重新触发自动换行，换行宽度始终来自布局阶段的完整内容宽度；
- `TextOverflow.Clip` 保持直接裁剪；`TextOverflow.Ellipsis` 仅在最终分行数大于 Text
  自身 frame 高度时生效，不因 Screen 或祖先造成的外部裁剪而单独触发；
- Ellipsis 在最后可显示行的 frame 最右侧预留最多三个 Cell 并绘制 ASCII `...`；
  frame 宽度小于 3 时绘制能够容纳的点，宽或高为 0 时不绘制；
- 省略号的位置由完整 frame 决定，drawFrame 只裁剪，不把省略号移动到外部可见区域内；
- Text 仍通过 `ViewManager.trace(widget, drawFrame)` 登记绘制区域和命中顺序；
- frame 或 drawFrame 任一轴为 0 时不写入 Buffer。

Buffer 和 Screen 同时维护列占用不变量，不仅依赖 Text 调用方正确：

- Buffer 的文本和 Cell 值 draw API 自行测量列宽；例如在 4 列区域绘制
  `中文` 时，主 Cell 分别位于第 0、2 列，第 1、3 列为续占 Cell；
- Buffer 覆盖多列 grapheme 的主 Cell 或任一续占 Cell 时，按完整 grapheme 清理旧的
  占用关系，不允许遗留孤立续占或只覆盖宽字符的一半；
- Screen diff 仅对主 Cell 编码输出，跳过 back buffer 中的续占 Cell；
- 宽 Cell 与窄 Cell 互相覆盖时，Buffer 先修正占用关系，Screen 再根据修正后的
  front/back 差异清除或重画完整字形；不在 SGR 或 Text 中增加 Emoji 特判。

颜色规则保持此前确定的组件层语义，不改变 Buffer 现有 draw API：

- `style.backColor` 有值时，Text 的完整可见 frame 使用该背景色；
- `style.backColor == none` 时，Text 不覆盖底层 Cell 已有背景色；
- 字符前景色使用 `style.foreColor`，none 表示终端默认前景色；
- 字符写入只更新 Text 负责的字符、前景色和明确指定的背景色，不通过新增 `drawCore`
  或修改现有 Buffer.draw 的 none 语义实现。

## 9 内部数据与性能约束

- arrange 扫描 content 时生成行起点、长度等内部元数据，draw 直接复用，不重复执行
  完整分行；Text 在 arrange/draw 中使用同一套 grapheme 拆分规则；
- 行信息只记录原 content 中的位置，不为每行创建 substring；
- 布局阶段复用 `std.text` 当前公开 grapheme API；绘制阶段不再次创建 grapheme
  substring，而是以行的原始字节区间和同一底层边界规则零分配重放；本次不为此
  扩大 `std.text` 的公开 API；
- 内部行缓冲可清空后复用已有容量，不在每帧为每行创建对象；
- 绘制按行起始字节位置一次定位，再按 `skip`、可见宽度和行 Cell 数控制解码次数；
  热路径不为每个码点重复计算相对原字符串的字节偏移；
- 绘制直接写入 Screen back buffer，不创建中间字符矩阵；
- locale 初始化仅发生在 `TextUtil.init()`，不进入逐码点测量热路径；
- 完整 grapheme 直接复用 Cell 的 `u64 value`，续占继续复用既有保留值；不增大
  Cell 和 front/back buffer，也不为普通或组合字符增加间接存储；
- Auto 不增加装箱、字符串判定或 runtime ABI；
- 不为了 Text 修改 Cell 的既有存储取舍，也不增加 C runtime 特判。

## 10 测试范围

新增 std_test 用例覆盖：

- Auto 零值、`auto` 赋值以及 int/double/Auto 三分支解析；
- Full 优先于 Auto，Start/Center/End 使用 Auto 最终尺寸定位；
- 基础 View 的 Auto 内在尺寸为 0，固定值和百分比现有行为不变；
- Text 空内容、单行、硬换行、末尾换行和有限宽度自动换行；
- 单码点 Emoji、组合字符与 ZWJ Emoji 的 grapheme 边界，8 字节内联编码，以及
  超出内联容量后由 Text 在码点边界拆分；
- TextUtil 初始化后 ASCII、中文和 Emoji 的列宽测量；
- Buffer 绘制中文/Emoji 时的主 Cell 与续占 Cell 布局；
- 宽码点被窄码点覆盖、从续占列开始覆盖，以及 Screen diff 不独立输出续占 Cell；
- 固定/百分比/Full 宽度配合 Auto 高度，以及 Auto 宽度；
- 固定高度裁剪、Screen 裁剪和最近 Hidden 祖先裁剪；
- 左侧、顶部被裁剪后仍保持原始文本行列偏移；
- Left/Center/Right 对齐按每行实际 Cell 数计算，并在 Screen/祖先裁剪后保持原始位置；
- Clip、Ellipsis、多行固定高度、窄于三个 Cell 以及仅发生外部裁剪的行为；
- 前景色、指定背景色和 none 背景不覆盖已有 Cell 背景；
- content 或可用宽度改变后重新 arrange 得到新的 frame 和分行；
- Text 的 drawFrame、trace 和鼠标命中区域与实际可见区域一致。

测试优先新增到 `std/std_test/src/test_tui.ff`，不修改无关既有用例。若实施只修改 docs、
std 和 std_test 且不涉及 C、compiler 或 runtime，则构建 std 并完整回归 std_test；若
暴露语言或底层实现问题，暂停本阶段，先记录问题并按实际影响扩大验证范围。

## 11 实施 TODO

- [x] 11.1 人工 Review 并确认本文档，特别确认 Auto 默认值、换行、padding 边界和背景保留语义；
- [x] 11.2 定义 `Auto`/`auto`，扩展 Style.width/height 并保持既有尺寸回归；
- [x] 11.3 保留 View 原有结构，以最小可见性和尺寸参数调整让 Text 复用布局、定位及 drawFrame 逻辑；
- [x] 11.4 为 Text 增加公开 content 以及无逐行字符串副本的行信息复用；
- [x] 11.5 实现 Text 固定/百分比/Full/Auto 尺寸测量和自动换行；
- [x] 11.6 实现 Text 绘制、原始内容偏移裁剪和 none 背景保留；
- [x] 11.7 新增 Auto、Text 测量、换行、裁剪、颜色和重排 std_test 用例；
- [x] 11.8 构建 std 并完整回归 std_test；
- [x] 11.9 根据实现结果更新 TODO，等待人工 Review；
- [x] 11.10 为 TextWidget/Text 增加 TextAlign 与 textAlign，并实现逐行水平对齐及裁剪测试；
- [x] 11.11 复用 `std.text` grapheme API，实现组合字符与 Emoji 的测量、换行和绘制；
- [x] 11.12 增加 TextOverflow.Clip/Ellipsis，并覆盖自身高度溢出与外部裁剪边界；
- [x] 11.13 实现 TextUtil 显式 locale 初始化和 POSIX 码点列宽测量；
- [x] 11.14 在不改变 Cell 存储大小的前提下实现多列码点续占表示；
- [x] 11.15 实现 Buffer 宽度感知绘制、覆盖修正与 Screen 续占 diff；
- [x] 11.16 Text 复用 TextUtil 的列宽测量和 Buffer 的通用 Cell 绘制；
- [x] 11.17 更新并新增宽字符 std_test，保留 tui_demo 的宽字符与色块覆盖验证场景；
- [x] 11.18 通过 tui_demo 人工验证宽字符被色块反复覆盖后的显示；
- [x] 11.19 执行 `make test` 全量回归；
- [x] 11.20 为 RgbColor 增加 alpha，令 Buffer 负责透明颜色通道合成，以 Cell 颜色存在位区分显式黑色与终端默认色，并移除 Text 对 Cell 的直接操作；
- [ ] 11.21 Text 已通过 Review；VStack/HStack 已进入
  `docs/engineering/feng-std-tui-stack-layout-dev.md` 独立设计，Input 仍待后续设计。
