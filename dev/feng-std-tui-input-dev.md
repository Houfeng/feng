# Feng TUI 输入支持方案

> 状态：设计中（design）
>
> 本文档是 `dev/feng-std-tui-dev.md` 阶段五（应用控制层 - 输入支持）的实现方案细化。
> 仅在此记录事件类型设计、VT100/xterm 输入解析状态机、事件路由机制，
> 不重复 dev 主文档的路线规划。

## 1 总体策略

**InputManager 独立 + spec 回调注入**：InputManager 作为独立 `open type`
负责将 stdin 字节流通过状态机解析为 `KeyEvent`/`MouseEvent`，并在内部直接
分发到 `onKey`/`onMouse` 回调。TuiApp 持有 `input: InputManager` 公开成员，
在 `run()` 中将 stdin 字节逐个喂入 `feed()`。应用层在回调中修改状态，
回调返回后 `run()` 自动调用 `render()`。

```
stdin 可读
  → c_read → byte[]
    → input.feed(byte) → void
      → 状态机解析（内部）
      → 解析完成 → onKey(keyEvent) / onMouse(mouseEvent)（内部直接分发）
      → render()
```

### 1.1 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| Ctrl+C 退出 | 交给应用回调 | InputManager 只产出 KeyEvent，退出语义由应用 onKey 决定 |
| 鼠标支持 | 完整支持 | 第五阶段含鼠标（init 时发送启用序列，exit 时发送禁用序列） |
| 事件路由 | InputManager 内部分发（onKey/onMouse） | feed 解析完成后直接调 onKey/onMouse，无需中间 TuiEvent 包装 |
| 回调类型 | 普通 open spec（非 @abi） | feed 内部分发是 Feng→Feng 调用，不经 C ABI。@abi 仅用于 C 回调 Feng 的场景 |
| UTF-8 解码 | InputManager 内部处理 | 解析器负责完整解码，应用拿到的就是 Unicode 码点 |
| Modifiers | u8 位标志 + 快捷方法 | 位标志简单高效，与 Cell 的 style 编码风格一致 |

## 2 事件类型（Event.ff）

### 2.1 Key 枚举

不可打印的特殊键通过 `Key` 枚举表示。可打印字符通过 `codepoint` 字段传递，
此时 `key` 为 `None`。

```feng
open enum Key {
  none = 0,
  escape = 1,
  tab = 2,
  enter = 3,
  backspace = 4,
  insert = 5,
  delete = 6,
  home = 7,
  end = 8,
  pageUp = 9,
  pageDown = 10,
  arrowUp = 11,
  arrowDown = 12,
  arrowLeft = 13,
  arrowRight = 14,
  f1 = 15,
  f2 = 16,
  f3 = 17,
  f4 = 18,
  f5 = 19,
  f6 = 20,
  f7 = 21,
  f8 = 22,
  f9 = 23,
  f10 = 24,
  f11 = 25,
  f12 = 26
}
```

### 2.2 Modifiers 常量与快捷方法

修饰键用 `u8` 位标志表示，定义在 Event.ff 顶层（与 Cell 的 STYLE_* 常量同模式）：

```feng
/** 修饰键位标志（u8 位标志，可组合） */
seal let MOD_CTRL: u8 = 1;
seal let MOD_ALT: u8 = 2;
seal let MOD_SHIFT: u8 = 4;
```

### 2.3 KeyEvent

```feng
@value
open type KeyEvent {
  /** 特殊键（none 表示可打印字符，数据走 codepoint） */
  open var key: Key;
  /** 可打印字符的 Unicode 码点（不可打印为 0） */
  open var codepoint: u32;
  /** 修饰键位标志（MOD_CTRL | MOD_ALT | MOD_SHIFT 的组合） */
  open var mods: u8;

  func KeyEvent() {
    self.key = Key.none;
    self.codepoint = 0;
    self.mods = 0;
  }

  func KeyEvent(key: Key, codepoint: u32, mods: u8) {
    self.key = key;
    self.codepoint = codepoint;
    self.mods = mods;
  }

  // ─── 快捷判定方法 ─────────────────────────────────────────────────────

  /** 是否按住 Ctrl */
  func isCtrl(): bool { return (self.mods & MOD_CTRL) != 0; }
  /** 是否按住 Alt */
  func isAlt(): bool { return (self.mods & MOD_ALT) != 0; }
  /** 是否按住 Shift */
  func isShift(): bool { return (self.mods & MOD_SHIFT) != 0; }
  /** 是否为可打印字符（key == none 且 codepoint != 0） */
  func isPrintable(): bool { return self.key == Key.none && self.codepoint != 0; }
}
```

### 2.4 MouseAction / MouseButton 枚举

```feng
open enum MouseAction {
  press = 0,
  release = 1,
  move = 2
}

open enum MouseButton {
  none = 0,
  left = 1,
  middle = 2,
  right = 3,
  wheelUp = 4,
  wheelDown = 5
}
```

### 2.5 MouseEvent

```feng
@value
open type MouseEvent {
  /** 鼠标动作类型 */
  open var action: MouseAction;
  /** 按下的按钮（move 时为 none） */
  open var button: MouseButton;
  /** 列坐标（1-based，与终端行号一致） */
  open var x: u32;
  /** 行坐标（1-based） */
  open var y: u32;
  /** 修饰键位标志 */
  open var mods: u8;

  func MouseEvent() {
    self.action = MouseAction.press;
    self.button = MouseButton.none;
    self.x = 0;
    self.y = 0;
    self.mods = 0;
  }

  func MouseEvent(action: MouseAction, button: MouseButton, x: u32, y: u32, mods: u8) {
    self.action = action;
    self.button = button;
    self.x = x;
    self.y = y;
    self.mods = mods;
  }

  func isCtrl(): bool { return (self.mods & MOD_CTRL) != 0; }
  func isAlt(): bool { return (self.mods & MOD_ALT) != 0; }
  func isShift(): bool { return (self.mods & MOD_SHIFT) != 0; }
}
```

## 3 InputManager — VT100/xterm 状态机（InputManager.ff）

### 3.1 设计目标

- 纯 Feng 实现，不依赖外部库
- 喂入单字节，无返回值——解析完成后内部直接分发到 `onKey`/`onMouse`
- 处理三类输入：单字节字符、CSI 转义序列、SS3 转义序列、鼠标 SGR 序列
- 内部处理 UTF-8 多字节解码，产出完整 Unicode 码点
- 持有 `onKey`/`onMouse` 回调字段，用户直接赋值注册（`manager.onKey = ...`）

### 3.2 状态机状态

```feng
open enum ParserState {
  ground = 0,     // 地面态：等待新字节
  esc = 1,        // 收到 ESC，等待下一个字节
  csi = 2,        // 收到 ESC [，收集参数
  ss3 = 3,        // 收到 ESC O，等待功能键字节
  utf8 = 4        // UTF-8 多字节续传
}
```

### 3.3 类型设计

```feng
/** 键盘事件回调契约（普通 open spec，非 @abi） */
open spec KeyHandler(event: KeyEvent): void;

/** 鼠标事件回调契约（普通 open spec，非 @abi） */
open spec MouseHandler(event: MouseEvent): void;

open type InputManager {
  /** 状态机当前状态 */
  seal var state: ParserState;
  /** 转义序列参数累积缓冲（CSI 参数） */
  seal var params: i32[];
  /** 当前正在解码的 UTF-8 字节的累积缓冲 */
  seal var utf8Buf: byte[];
  /** UTF-8 期望续传字节数 */
  seal var utf8Remaining: i32;
  /** UTF-8 解码出的码点（中间累积用） */
  seal var utf8Codepoint: u32;
  /** 键盘事件回调（用户直接赋值注册，未设置时键盘事件丢弃） */
  open var onKey: KeyHandler;
  /** 鼠标事件回调（用户直接赋值注册，未设置时鼠标事件丢弃） */
  open var onMouse: MouseHandler;

  func InputManager() { ... }

  /**
   * 喂入一个字节，无返回值。
   * 状态机解析，序列完成后内部直接调用 onKey/onMouse 分发。
   * 序列未完成时不产生任何输出。
   */
  open func feed(b: u8): void;
}
```

> **KeyHandler/MouseHandler 是普通 `open spec`，非 `@abi`**：`feed()` 是
> Feng 函数，内部调用 `self.onKey(event)` 是 Feng→Feng 调用，不经 C ABI。
> `@abi` 仅用于 C 代码回调 Feng 的场景（如 TuiApp 的 `handleSigwinch`）。
> 此模式与 Thread 模块的 `ThreadFn`（普通 `open spec`）一致。
>
> **onKey/onMouse 为 `open var`**：用户直接赋值注册
> （`manager.onKey = func(event: KeyEvent) { ... };`），不需要 setter 方法。

### 3.4 feed() 处理逻辑

`feed()` 是状态机驱动入口。根据当前 state 用 `match` 分派，各 handler
内部解析完成后直接调用 `self.onKey(...)` 或 `self.onMouse(...)` 分发：

```feng
open func feed(b: u8): void {
  match self.state {
    ParserState.ground { self.handleGround(b); }
    ParserState.esc { self.handleEsc(b); }
    ParserState.csi { self.handleCsi(b); }
    ParserState.ss3 { self.handleSs3(b); }
    ParserState.utf8 { self.handleUtf8(b); }
    else { self.reset(); }
  }
}
```

> 各 handler 的分发方式：`handleGround` 产出 KeyEvent 时调
> `self.emitKey(key, codepoint, mods)`，`emitKey` 内部检查 onKey 是否
> 已设置，已设置则调用。鼠标同理用 `self.emitMouse(...)`。

#### 3.4.1 handleGround — 地面态

| 字节范围 | 处理 |
|----------|------|
| 0x1B (ESC) | 切换到 esc 态，返回 none |
| 0x00–0x1F (控制字符) | 映射为 KeyEvent（Enter=0x0D, Tab=0x09, Backspace=0x7F 或 0x08 等） |
| 0x20–0x7F (ASCII 可打印) | 直接产出 KeyEvent(codepoint=b) |
| 0x80–0xBF (UTF-8 续传字节) | 不应在 ground 态出现，重置状态机 |
| 0xC0–0xDF (2 字节 UTF-8 首字节) | 进入 utf8 态，期望 1 个续传 |
| 0xE0–0xEF (3 字节 UTF-8 首字节) | 进入 utf8 态，期望 2 个续传 |
| 0xF0–0xF7 (4 字节 UTF-8 首字节) | 进入 utf8 态，期望 3 个续传 |

控制字符映射表：

| 字节 | KeyEvent |
|------|----------|
| 0x08 (BS) | Key.backspace |
| 0x09 (HT) | Key.tab |
| 0x0D (CR) | Key.enter |
| 0x1B (ESC) | 进入 esc 态（不立即产出） |
| 0x7F (DEL) | Key.backspace |

> Ctrl+字母（0x01–0x1A）映射为 KeyEvent(codepoint=字母, mods=MOD_CTRL)。
> 例如 0x01 → codepoint='a', mods=MOD_CTRL；0x03 → codepoint='c', mods=MOD_CTRL。

#### 3.4.2 handleEsc — ESC 态

| 字节 | 处理 |
|------|------|
| 0x5B `[` | 进入 csi 态，清空 params |
| 0x4F `O` | 进入 ss3 态 |
| 其他 | 产出 Alt+该字节（mods=MOD_ALT），回到 ground 态 |

#### 3.4.3 handleCsi — CSI 态

收集参数字节（0x30–0x3F 数字和分隔符），终态字节（0x40–0x7E）触发解析：

| 终态字节 | 参数 | 产出 |
|----------|------|------|
| `A` | 无 | Key.arrowUp |
| `B` | 无 | Key.arrowDown |
| `C` | 无 | Key.arrowRight |
| `D` | 无 | Key.arrowLeft |
| `H` | 无 | Key.home |
| `F` | 无 | Key.end |
| `P`~`S` | 无 | F1~F4（xterm CSI 方式） |
| `~` | 0 | 无效 |
| `~` | 1 | Key.home |
| `~` | 4 | Key.end |
| `~` | 5 | Key.pageUp |
| `~` | 6 | Key.pageDown |
| `~` | 2 | Key.insert |
| `~` | 3 | Key.delete |
| `~` | 15 | Key.f5 |
| `~` | 17 | Key.f6 |
| `~` | 18 | Key.f7 |
| `~` | 19 | Key.f8 |
| `~` | 20 | Key.f9 |
| `~` | 21 | Key.f10 |
| `~` | 23 | Key.f11 |
| `~` | 24 | Key.f12 |
| `M` | — | 隐式鼠标事件（旧式） |
| `<` | — | SGR 鼠标序列起始，继续收集 |

> Shift 修饰键通过 CSI 序列的隐式参数标记（方向键 Shift+Up = `ESC[1;2A`，
> 参数 2 = Shift）。解析时检查第二个参数：2=Shift, 3=Alt, 4=Alt+Shift,
> 5=Ctrl, 6=Ctrl+Shift, 7=Ctrl+Alt, 8=Ctrl+Alt+Shift。

#### 3.4.4 handleSs3 — SS3 态

| 字节 | 产出 |
|------|------|
| `P` | Key.f1 |
| `Q` | Key.f2 |
| `R` | Key.f3 |
| `S` | Key.f4 |
| `A` | Key.arrowUp（某些终端） |
| `B` | Key.arrowDown |
| `C` | Key.arrowRight |
| `D` | Key.arrowLeft |
| `H` | Key.home |
| 其他 | 无效，丢弃 |

#### 3.4.5 handleUtf8 — UTF-8 续传态

每收到一个续传字节（0x80–0xBF），拼入 `utf8Codepoint`，`utf8Remaining` 减 1。
`utf8Remaining == 0` 时解码完成，产出 KeyEvent(codepoint=码点)，回到 ground 态。

UTF-8 解码规则：

| 首字节范围 | 续传数 | 码点公式 |
|------------|--------|----------|
| 0xC0–0xDF | 1 | `((b0 & 0x1F) << 6) | (b1 & 0x3F)` |
| 0xE0–0xEF | 2 | `((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F)` |
| 0xF0–0xF7 | 3 | `((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F)` |

> 续传字节不在 0x80–0xBF 范围内时为非法序列，重置状态机回到 ground，
> 已收到的字节丢弃。

### 3.5 鼠标 SGR 序列解析

现代终端使用 SGR 鼠标模式（`\x1b[?1006h` 启用）。格式：

```
ESC [ < button ; x ; y M   (press)
ESC [ < button ; x ; y m   (release)
```

button 值映射：

| button 值 | MouseButton | 修饰键 |
|-----------|-------------|--------|
| 0 | left | |
| 1 | middle | |
| 2 | right | |
| 64 | wheelUp | |
| 65 | wheelDown | |
| 0 + 4 | left | Shift |
| 0 + 8 | left | Alt |
| 0 + 16 | left | Ctrl |

> button 值 32 = 拖动（move 事件）。button 的低 5 位标识按钮，高位标识修饰键。
> 32 = move, 64 = wheelUp, 65 = wheelDown。

### 3.6 params 数组解析

CSI 参数以 `;` 分隔。例如 `ESC[1;2A` → params = [1, 2]。
缺省参数补 0。`InputManager` 在 csi 态收集数字字符到 params，遇到 `;` 分隔。

## 4 TuiApp 集成

### 4.1 TuiApp 新增公开成员

InputManager 作为 TuiApp 的公开成员，用户通过 `app.input.onKey = ...` 注册回调：

```feng
open type TuiApp {
  // ... 阶段四已有字段 ...

  /** 输入管理器（公开只读成员，构造时创建，用户通过此注册回调） */
  let input: InputManager;
}
```

> **`input` 为 `let`**：引用不可变——构造时创建 InputManager 实例，之后不替换
> 整个实例。用户通过 `app.input.onKey = ...` 和 `app.input.onMouse = ...`
> 修改的是 InputManager **内部**的 `open var` 字段，不是 `input` 字段本身。
> 这与 Buffer 的 `let width: u32` 同理——引用不可变，但内部可变字段仍可修改。

### 4.2 run() 修改

阶段四的 stdin drain 替换为逐字节喂入 InputManager，由其内部解析并分发：

```feng
// stdin 读取缓冲区
let readBuf: byte[!] = byte[:256];
while self.running {
  // 重置 revents
  for var i: int = 0; i < total; i += 1 {
    pfds[i].revents = 0;
  }
  let rc = c_poll(&pfds, (i32)total, POLL_TIMEOUT_BLOCK);
  if rc > 0 {
    // 处理 sigpipeR（信号到达）
    if pfds[1].revents & POLLIN != 0 {
      let dummy: byte[!] = byte[:1];
      c_read(self.sigpipeR, &dummy, 1);
      self.resizeRequested = true;
    }
    // 处理 stdin（阶段五：逐字节喂入 InputManager 解析分发）
    if pfds[0].revents & POLLIN != 0 {
      let n = c_read(STDIN_FD, &readBuf, (uint)256);
      if n > 0 {
        for var i: int = 0; i < n; i += 1 {
          self.input.feed((u8)readBuf[i]);
        }
      }
    }
  }
  self.render();
}
```

> 与阶段四相比：`c_read` 后不再丢弃字节，而是逐个喂入 `self.input.feed()`。
> `feed()` 无返回值，内部解析完成后直接调用 `onKey`/`onMouse` 分发。
> 回调返回后 `run()` 继续处理下一字节，本轮 poll 的字节全部处理完后调 `render()`。

### 4.3 鼠标模式启用/禁用

init() 中进入 Raw Mode 后发送鼠标启用序列：

```
\x1b[?1000h  — 启用鼠标报告（基本模式）
\x1b[?1006h  — 启用 SGR 鼠标模式（精确坐标 + press/release 区分）
```

exit() 中发送禁用序列：

```
\x1b[?1006l
\x1b[?1000l
```

> 通过 `c_write(STDOUT_FD, ...)` 直接发送，与 Screen 的 buildPatchBytes
> 路径独立（这些是终端模式协商序列，不参与 diff 渲染）。

## 5 文件组织

```text
std/src/tui/
  Event.ff          # 新增：Key / MouseAction / MouseButton
  #                 #       KeyEvent / MouseEvent / Modifiers 常量
  InputManager.ff   # 新增：VT100/xterm 状态机 + KeyHandler/MouseHandler spec
  #                 #       onKey/onMouse 字段 + feed(byte): void
  TuiApp.ff         # 修改：新增 input: InputManager 公开成员
  #                 #       run() drain → input.feed() 逐字节喂入
  #                 #       init() 增加鼠标启用序列
  #                 #       exit() 增加鼠标禁用序列
  Cell.ff           # 不变
  Buffer.ff         # 不变
  Screen.ff         # 不变
  Style.ff          # 不变
  RgbColor.ff       # 不变
```

## 6 测试策略

### 6.1 InputManager 单元测试（纯逻辑，不依赖终端）

InputManager 的 `feed()` 无返回值，测试通过 mock 回调验证解析结果：
注册 `onKey`/`onMouse` 回调，回调中捕获事件到测试变量，喂入字节后检查捕获的事件。

| 测试项 | 喂入字节 | 期望回调事件 |
|--------|----------|-------------|
| 可打印 ASCII | `0x41` (A) | onKey(KeyEvent(codepoint=0x41)) |
| Enter | `0x0D` | onKey(KeyEvent(key=enter)) |
| Tab | `0x09` | onKey(KeyEvent(key=tab)) |
| Backspace (BS) | `0x08` | onKey(KeyEvent(key=backspace)) |
| Backspace (DEL) | `0x7F` | onKey(KeyEvent(key=backspace)) |
| Ctrl+C | `0x03` | onKey(KeyEvent(codepoint='c', mods=MOD_CTRL)) |
| Ctrl+A | `0x01` | onKey(KeyEvent(codepoint='a', mods=MOD_CTRL)) |
| Esc | `0x1B` | 进入 esc 态，单独 Esc 需 timeout 或后续字节区分 |
| 方向键 Up | `ESC [ A` | onKey(KeyEvent(key=arrowUp)) |
| 方向键 Down | `ESC [ B` | onKey(KeyEvent(key=arrowDown)) |
| 方向键 Right | `ESC [ C` | onKey(KeyEvent(key=arrowRight)) |
| 方向键 Left | `ESC [ D` | onKey(KeyEvent(key=arrowLeft)) |
| Home (CSI) | `ESC [ H` | onKey(KeyEvent(key=home)) |
| End (CSI) | `ESC [ F` | onKey(KeyEvent(key=end)) |
| Delete | `ESC [ 3 ~` | onKey(KeyEvent(key=delete)) |
| F1 (SS3) | `ESC O P` | onKey(KeyEvent(key=f1)) |
| F5 (CSI) | `ESC [ 1 5 ~` | onKey(KeyEvent(key=f5)) |
| Shift+Up | `ESC [ 1 ; 2 A` | onKey(KeyEvent(key=arrowUp, mods=MOD_SHIFT)) |
| Ctrl+Up | `ESC [ 1 ; 5 A` | onKey(KeyEvent(key=arrowUp, mods=MOD_CTRL)) |
| UTF-8 中文 | `0xE4 0xBD 0xA0` | onKey(KeyEvent(codepoint=0x4F60=你)) |
| UTF-8 emoji | `0xF0 0x9F 0x98 0x80` | onKey(KeyEvent(codepoint=0x1F600=😀)) |
| 鼠标左键点击 | `ESC [ < 0 ; 10 ; 5 M` | onMouse(MouseEvent(action=press, button=left, x=10, y=5)) |
| 鼠标右键释放 | `ESC [ < 2 ; 10 ; 5 m` | onMouse(MouseEvent(action=release, button=right, x=10, y=5)) |
| 鼠标滚轮上 | `ESC [ < 64 ; 1 ; 1 M` | onMouse(MouseEvent(action=press, button=wheelUp, x=1, y=1)) |
| 非法 UTF-8 | `0xC0 0x00` | 重置状态机，无回调触发 |
| 未知 CSI | `ESC [ Z` | 无回调触发，丢弃 |

### 6.2 回调注册与分发测试

| 测试项 | 方法 |
|--------|------|
| onKey 回调触发 | 赋值 onKey，feed 可打印字节，验证回调被调用且参数正确 |
| onMouse 回调触发 | 赋值 onMouse，feed 鼠标序列，验证回调被调用且参数正确 |
| 未注册回调时事件丢弃 | 不赋值 onKey/onMouse，feed 字节，不崩溃 |

### 6.3 不可测试项（需真实终端）

| 测试项 | 原因 |
|--------|------|
| 鼠标模式启用序列实际生效 | 需真实终端 |
| Ctrl+C 实际退出 | 需真实终端 + 应用回调 |

## 7 平台注意

- **鼠标 SGR 模式（1006）**：现代终端（xterm、iTerm2、GNOME Terminal、Alacritty、kitty）
  均支持。老旧终端不支持时鼠标事件解析失败，丢弃，不影响键盘。
- **F1–F4 双路径**：部分终端用 SS3（`ESC O P`），部分用 CSI（`ESC [ P`）。
  InputManager 两条路径都处理。
- **Backspace 键**：不同终端发送不同字节。macOS 默认发送 0x7F (DEL)，
  部分配置发送 0x08 (BS)。InputManager 两者都映射为 Key.backspace。
- **Alt 键**：在 Raw Mode 下，Alt+字母通常表示为 ESC 后跟字母
  （如 Alt+A = `0x1B 0x61`）。InputManager 在 esc 态收到非 `[`/`O` 字节时
  产出 Alt+该字节。但这也可能与真正的 Esc 键冲突——单独的 Esc 键需要
  等待后续字节或 timeout 区分。阶段五采用后续字节区分策略：
  若 `feed()` 在 esc 态收到非转义起始字节，产出 Alt+该字节；
  若调用方需要区分单独 Esc，可在应用层用 timeout 判断。

## 8 阶段五实施步骤

1. 实现 Event.ff — 事件类型 + Modifiers 常量 + 快捷方法
2. 实现 InputManager.ff — VT100/xterm 状态机 + KeyHandler/MouseHandler spec + feed/onKey/onMouse
3. 修改 TuiApp.ff — 新增 input 公开成员 + run() 集成 + 鼠标启用/禁用
4. 补充 std_test 用例 — InputManager 单元测试 + 回调分发测试
5. 全量回归测试 — `make test`
6. 等待人工 Review
