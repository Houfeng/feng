# Feng TUI 开发计划

> 状态：设计中（design）
>
> 本文档用于规划 `std.tui` 模块的完整实现路线，是实现的唯一权威依据。

## 1 目标

构建一个极完整、好用且好看的终端用户界面（TUI）框架，作为 `std.tui` 标准库的一部分。

## 2 分层架构

```mermaid
┌──────────────────────────────────────────────┐
│  5. TuiApp (应用控制层)                       │
│     生命周期 / Raw Mode / 信号 / 事件路由      │
├──────────────────────────────────────────────┤
│  4. Widget Tree (视图逻辑层)                   │
│     组件树 / 状态 / 样式 / 布局（后续设计）      │
├──────────────────────────────────────────────┤
│  3. Screen (渲染底座 - 差异同步)               │
│     双缓冲 / Diff 引擎 / ANSI 序列生成          │
├──────────────────────────────────────────────┤
│  2. Buffer (渲染底座 - 数据容器)               │
│     Cell 矩阵 / 值语义 / 连续内存              │
├──────────────────────────────────────────────┤
│  1. Cell (渲染底座 - 最小单元)                 │
│     码点 + 样式 / 16 字节 / @value             │
└──────────────────────────────────────────────┘
```

### 2.1 渲染底座（第 1-3 层）

- **Cell**：纯粹的数据容器。`@value` 类型，16 字节布局（`u64 value` + `u64 style`），值语义保证内存绝对连续排列。成员为 `open var`，可直接字段赋值修改；同时提供 foreColor/backColor/bold/dim 等便利 getter/setter 方法对。
- **Style**：`open enum`，9 个枚举项（none/bold/dim/italic/underline/blink/reverse/hidden/strikethrough），使用小整数序号。类型安全，调用方只能传入合法样式。
- **RgbColor**：`@value` 类型，3 个 `u8` 字段（r/g/b），表示 RGB 颜色。配合 `Option<RgbColor>` 使用，`none` 表示终端默认色。
- **Buffer**：管理 `Cell[]` 矩阵。通过直接字段赋值（`cells[idx].value = ...`）就地修改元素，利用 `@value` 类型的语义，不需要可写数组。提供统一的 `draw` 重载体系（文本/码点 × 单点/矩形 × 无色/前景/前景+背景）、`fill`、`clear` 绘制原语，内部通过 `styleToBits`/`combineStyles`/`packStyle` 将 Style 枚举与 RgbColor 打包为 Cell 的样式编码。
- **Screen**：封装双缓冲内存同步与差异比对（Diff）引擎。向下对接 stdout，生成 ANSI 转义序列，通过批量 I/O 冲刷。不关心业务逻辑。

### 2.2 视图逻辑层（第 4 层）

- **组件树**：承载业务状态、样式定义以及排版约束（如 Flexbox）。
- **第一步**：先实现简单文本和按钮，后续再专门设计完整布局引擎。

### 2.3 应用控制层（第 5 层）

- **生命周期**：启动时进入 Raw Mode，注册 `SIGWINCH` 响应 Resize，程序退出或崩溃时保证终端状态绝对恢复。
- **输入流解析与路由**：读取 stdin 字节流，通过状态机解析为 `KeyEvent` 或 `MouseEvent`，下发给焦点节点，计算状态变更后触发 `Screen.render()`。

## 3 关键设计决策

### 3.1 Cell 设计

- `@value open type Cell`，两个成员：`open var value: u64`（码点）、`open var style: u64`（样式编码）。成员为 `open var`，允许直接字段赋值修改。
- 样式布局：`[ 16 bits 样式标志 ] [ 24 bits 背景RGB ] [ 24 bits 前景RGB ]`，RGB 编码为 `0xRRGGBB`（R 在高位，B 在低位）。
- 静态常量：`MASK_FG`（低 24 bits 前景色掩码）、`MASK_BG`（中 24 bits 背景色掩码）、`MASK_FLAGS`（高 16 bits 样式标志掩码）、`STYLE_BOLD` 至 `STYLE_STRIKETHROUGH`（各样式位值）。
- 样式标志位（48-63 位）：`BOLD=48, DIM=49, ITALIC=50, UNDERLINE=51, BLINK=52, REVERSE=53, HIDDEN=54, STRIKETHROUGH=55`。
- 便利方法：`foreColor()`/`foreColor(value)`、`backColor()`/`backColor(value)` 色读写方法对，`bold()`/`bold(value)` 至 `strikeThrough()`/`strikeThrough(value)` 样式读写方法对，均通过 `self` 就地修改 `var` 成员。
- 构造函数：`Cell()`（空白）、`Cell(value)`（指定码点）、`Cell(value, style)`（指定码点和样式）。
- **不加 width 字段**。复杂字符（如 ZWJ 组合 emoji `👨‍👨‍👦‍👦`）在输入层拆分为多个 Cell，每个 Cell 持有单个 Unicode 码点。
- `@value` 类型的 `self` 方法可就地修改 `var` 成员，值语义保证内存连续排列。

### 3.2 Buffer 设计

- `open type Buffer`，成员：`let width: u32`、`let height: u32`、`let cells: Cell[]`。尺寸创建后不可变，终端尺寸调整由上层（Screen）重新创建新实例处理。
- **使用不可变数组 `Cell[]`**，通过直接字段赋值（如 `cells[idx].value = 65; cells[idx].style = s`）就地修改元素，不需要可写数组 `Cell[!]`。利用 `@value` 类型的语义，只读数组的元素字段仍可写。
- 通过 `cells[y * width + x]` 线性索引访问。
- **内部样式打包方法（seal）**：
  - `styleToBits(s: Style): u64` — 将 Style 枚举值映射为 Cell 的 `STYLE_*` 位值。
  - `combineStyles(styles: Style[]): u64` — 将多个 Style 按位或组合为单个 u64 样式编码。
  - `packStyle(fg: Option<RgbColor>, bg: Option<RgbColor>, styles: Style[]): u64` — 将前景色、背景色和样式数组打包为完整的 u64 样式编码。`none` 的颜色不写入对应位段，表示终端默认色。RGB 编码为 `0xRRGGBB`。
- **绘制原语**：
  - `draw`（4 个 seal 数组版本 + 12 个 open 变长版本）。
    - seal 数组版本（内部实现，参数为 `fg: Option<RgbColor>, bg: Option<RgbColor>, styles: Style[]`）：
      1. `draw(x, y, text, fg, bg, styles)` — 单行文本，使用 `u8_next` 零分配解码 UTF-8 码点。
      2. `draw(x, y, w, h, text, fg, bg, styles)` — 矩形区域文本，自动换行。
      3. `draw(x, y, value: u64, fg, bg, styles)` — 单个码点。
      4. `draw(x, y, w, h, value: u64, fg, bg, styles)` — 矩形区域码点填充。
    - open 变长版本（公开 API，3 种颜色组合 × 4 种形状 = 12 个重载，参数为 `styles: Style...`）：
      - 无颜色：`draw(x, y, text, styles...)` 等 4 个，使用终端默认色。
      - 带前景色：`draw(x, y, text, fg: RgbColor, styles...)` 等 4 个。
      - 带前景及背景色：`draw(x, y, text, fg: RgbColor, bg: RgbColor, styles...)` 等 4 个。
  - `fill(value, fg, bg, styles...)` / `fill(value, styles...)` — 用指定码点和样式填充整个矩阵，委托 `draw(0, 0, width, height, ...)`。
  - `clear()` — 清空矩阵，委托 `fill(0)`。
- 所有绘制方法均做边界裁剪，超出 Buffer 范围的内容被跳过。

### 3.3 Screen 设计

- `open type Screen`，持有 front buffer 和 back buffer。
- **双缓冲 + Diff 引擎**：
  - `render()` 时逐 cell 比较 front 与 back，只对变化的 cell 发射 ANSI 序列。
  - **SGR 状态机优化**：连续相同 style 的 cell 只发射一次 SGR 序列，不逐 cell 重发。
  - 光标移动优化：跳过连续相同区域，批量定位。
- **I/O 批处理**：积累 64KB 输出后一次性 `write()` 刷新，减少 syscall 次数。
- 提供终端尺寸查询（`ioctl TIOCGWINSZ`）和光标控制。

### 3.4 组件树设计（后续专门设计）

- Feng 没有继承，组件多态通过 `spec` 契约 + `fit` 实现：定义 `spec Widget` 声明组件行为契约（渲染、事件处理、尺寸等），具体组件 `type Text: Widget` / `type Button: Widget` 通过声明头满足契约。
- 第一步只实现简单文本（`Text`）和按钮（`Button`）。
- 完整 Flexbox 布局引擎后续专门设计。

### 3.5 TuiApp 设计

- **Raw Mode**：通过 `@cdecl("libc")` 导入 `tcgetattr`/`tcsetattr`。
- **SIGWINCH**：通过 `@cdecl("libc")` 导入 `signal`/`sigaction`，配合 `ioctl TIOCGWINSZ`。
- **终端恢复保证**：正常路径用 `defer` 保证恢复；异常/信号路径用 `atexit` 注册清理钩子。
- **输入解析**：VT100/xterm 转义序列状态机，纯 Feng 实现。
- **事件路由**：解析为 `KeyEvent`/`MouseEvent`，下发给焦点节点。

## 4 实施路线

每个阶段遵循统一流程：实现代码 → 补充 std_test 用例 → 全量回归测试 → 等待人工 Review。

- **std_test 用例**：在 `std_test/src/test_tui.ff` 中新增对应测试函数，注册到 `run_tui_tests()`，并在 `z_main.ff` 中调用。
- **全量回归测试**：执行 `make test`，确保所有测试套件通过
- **人工 Review**：变更完成后通知开发者审查，审查通过后方可进入下一阶段。严禁跳过 Review 直接开始下一阶段。

### 第一阶段：Cell（渲染底座 - 最小单元）

- [x] 4.1 完善 Cell：构造函数、静态常量、工厂方法
- [x] 4.2 补充 std_test 用例：新增 `std_test/src/test_tui.ff`，覆盖 Cell 样式读写（前景色/背景色/粗体/斜体等各标志位）；注册 `run_tui_tests()` 并在 `z_main.ff` 中调用
- [x] 4.3 全量回归测试：执行 `make test`，确认全部通过
- [x] 4.4 等待人工 Review：开发者审查 Cell 实现与测试用例，通过后方可进入第二阶段

### 第二阶段：Buffer（渲染底座 - 数据容器）

- [x] 4.5 实现 Buffer：Cell 矩阵管理 + 绘制原语（`setCell`、`drawText`、`fill`、`clear`）
- [x] 4.6 补充 std_test 用例：在 `test_tui.ff` 中新增 Buffer 矩阵索引、绘制原语、边界校验等测试
- [x] 4.7 全量回归测试：执行 `make test`，确认全部通过
- [x] 4.8 等待人工 Review：开发者审查 Buffer 实现与测试用例，通过后方可进入第三阶段

### 第三阶段：Screen（渲染底座 - 差异同步）

- [ ] 4.9 实现 Screen：双缓冲 + Diff 引擎 + ANSI 序列生成 + I/O 批处理 + SGR 状态机优化
- [ ] 4.10 补充 std_test 用例：在 `test_tui.ff` 中新增 Screen Diff 输出、SGR 状态机、双缓冲同步等测试
- [ ] 4.11 全量回归测试：执行 `make test`，确认全部通过
- [ ] 4.12 等待人工 Review：开发者审查 Screen 实现与测试用例，通过后方可进入第四阶段

### 第四阶段：视图逻辑层（第 4 层 - 简化版）

- [ ] 4.13 实现 Widget spec 契约 + Text + Button
- [ ] 4.14 实现简单的线性布局（Row / Column / Stack）
- [ ] 4.15 补充 std_test 用例：在 `test_tui.ff` 中新增 Widget 契约满足、Text 内容绘制、Button 状态切换等测试
- [ ] 4.16 全量回归测试：执行 `make test`，确认全部通过
- [ ] 4.17 等待人工 Review：开发者审查 Widget 契约设计与实现，通过后方可进入第五阶段

### 第五阶段：应用控制层（第 5 层）

- [ ] 4.18 实现 TuiApp：Raw Mode + 终端恢复 + SIGWINCH
- [ ] 4.19 实现输入解析状态机 + 事件路由
- [ ] 4.20 补充 std_test 用例：在 `test_tui.ff` 中新增输入解析状态机（VT100/xterm 转义序列）、事件路由等测试
- [ ] 4.21 全量回归测试：执行 `make test`，确认全部通过
- [ ] 4.22 等待人工 Review：开发者审查 TuiApp 生命周期与终端恢复机制，通过后方可进入第六阶段

### 第六阶段：测试与验证

- [ ] 4.23 补充 std_test 用例：审查前五阶段测试覆盖度，补充遗漏的边界用例与集成场景测试
- [ ] 4.24 全量回归测试：执行 `make test`，确认全部通过
- [ ] 4.25 等待人工 Review：开发者审查全部 TUI 实现与测试覆盖，通过后交付完成

> **fcts 用例策略**：std 中的功能默认使用 std_test 用例，不需要 fcts 用例。仅当遇到 Feng 语言层面的问题（如语法、语义、类型系统等编译器行为）时，才在 `fcts/fcts_bin/src/` 中新增 fcts 用例进行兼容性验证。

## 5 文件组织

```text
std/src/tui/
  Cell.ff          # Cell（最小单元）
  Buffer.ff        # Buffer（Cell 矩阵 + 绘制原语）
  Screen.ff        # Screen（双缓冲 + Diff）（后续）
  Style.ff         # Style 枚举（样式类型安全）
  RgbColor.ff      # RgbColor 结构（RGB 颜色）
  Ansi.ff          # ANSI 转义序列生成器（后续）
  Widget.ff        # Widget spec 契约（后续）
  Text.ff          # 文本组件（后续）
  Button.ff        # 按钮组件（后续）
  TuiApp.ff        # 应用程序入口与主循环（后续）
  KeyEvent.ff      # 键盘事件（后续）
  MouseEvent.ff    # 鼠标事件（后续）
```

## 6 约束

- 遵循 Feng 代码风格：类型名大驼峰，成员名小驼峰。
- 所有函数和结构体都需要有注释。
- 生产级高标准实现：注意性能、安全性、复用性、可维护性。
- runtime 是私有 ABI，仅在 Feng + C ABI 无法实现时才考虑。
- 未经允许，禁止修改已有的测试用例。
- 每个阶段实现后必须先补充 std_test 用例，再执行全量回归测试，最后等待人工 Review。
- 严禁跳过人工 Review 直接进入下一阶段。
- 全量回归测试命令统一使用 `make test`，必须确认全部通过（EXIT:0）。
- std_test 用例文件统一放在 `std_test/src/test_tui.ff`，遵循现有 `test_*.ff` 命名与 `run_*_tests()` 注册模式。
- std 中的功能默认使用 std_test 用例，不需要 fcts 用例；仅当遇到 Feng 语言层面的问题（语法、语义、类型系统等编译器行为）时，才新增 fcts 用例。
