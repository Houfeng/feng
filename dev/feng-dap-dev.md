# Feng DAP 开发方案

> 本文档用于收敛 Feng 首版源码级调试 / DAP 开发方案。
> 第一阶段的编辑器落地仍是 VS Code，但协议适配层统一收敛到 `feng dap`。
> 原则只有三条：
>
> 1. **继续复用现有 Feng -> C -> clang/LLDB 链路**，不自建解释器或 VM。
> 2. **`.ft` 继续只承载声明级缓存**，不把局部变量、帧信息和调试显示元数据塞进 `.ft`。
> 3. **LSP 与 DAP 明确分层**；`feng lsp` 继续只负责语言服务，不承载调试协议。

## 1. 目标

本方案要解决一件事：

- 让 Feng 在 VS Code 中具备**源码级断点调试能力**，且用户看到的是 **Feng 语义视角**，而不是裸 C 视角。

本方案第一阶段明确采用以下边界：

- 平台只覆盖 **macOS**。
- 原生调试器后端只覆盖 **LLDB**，优先使用 `lldb-dap`。
- IDE 只覆盖 **VS Code**。
- 编译目标只覆盖 **`target=bin`**。
- 功能范围覆盖：
  - Feng 源码断点
  - step in / step over / step out
  - Feng 视角 stack trace
  - Feng 视角参数与局部变量
  - 只读 watch 子集

### 1.1 第一阶段明确不做

- Linux / Windows 并行支持
- attach 到已运行进程
- time-travel 或 reverse debugging
- 任意 Feng 表达式求值
- 具有副作用的 watch / evaluate
- `target=lib` 的首版调试闭环
- 把调试协议并入 `feng lsp`

### 1.2 分步完成项

- [ ] 1：先收敛调试方案与边界文档，形成单一开发方案。
- [ ] 2：补齐 codegen 的源码映射输出，让 LLDB 能直接停在 `.ff` 源码行。
- [ ] 3：新增独立调试 sidecar 产物 `.fd`，只承载 frame / variable 重写所需的最小元数据。
- [ ] 4：新增独立 `feng dap` 代理层，转发到 `lldb-dap` 并完成 Feng 语义重写。
- [ ] 5：补齐 VS Code 启动集成，以及断点、单步、堆栈、变量和只读 watch 的回归验证。

## 2. 已确认决策

### 2.1 平台与后端

- 首版只面向 macOS。
- 首版原生调试后端统一采用 `lldb-dap`。
- 不自己实现进程控制、寄存器控制、单步执行或原生断点管理。
- Feng 调试层只负责“**Feng 语义到 LLDB 能理解的信息**”之间的转换与重写。

### 2.2 架构路线

- 保持当前 **Feng -> C -> 主机编译器 -> 原生二进制** 的主链不变。
- 不新建解释执行模式，也不要求引入第二套运行时。
- 不要求编译器直接写 DWARF；DWARF 仍由主机 C 编译器读取 `#line` 与 `-g` 后生成。
- 核心编译器首版新增职责原则上只限非 `release` 构建下的两件事：发出稳定 `#line`，并导出与 binary 同级的最小 `.fd` 调试 sidecar；编译驱动只做最小的 debug 选项透传与产物编排。
- 第一阶段即引入独立 `feng dap`，由它启动并代理 `lldb-dap`；原生单步、线程、断点命中与底层变量读取仍由 `lldb-dap` 负责。
- stack、variable、watch 与 frame 语义重写统一在 `feng dap` 完成；编辑器只负责 launch 配置、task 串接和调试 UI。
- 长期链路明确为 `editor -> feng dap -> lldb-dap -> LLDB`；VS Code 只是首个接入方，后续 Zed 等编辑器复用同一层。

### 2.3 数据边界

- `build/obj/symbols/**/*.ft` 继续是**声明级 workspace cache**。
- `.ft` 中已有的 span 仍只服务于声明级定位、hover、definition、completion 等语言服务消费场景。
- 局部变量、词法作用域、帧重写、调试显示提示等调试专用信息，统一落入新的 **artifact-scoped sidecar**：`.fd`。
- `.fd` 不进入 `.fb` 分发包，不作为跨包公共接口。

### 2.4 源码映射权威来源

- 断点绑定、step 行号和 stack line 的**第一权威来源**是主机编译器生成的原生调试信息。
- 这些原生调试信息必须来自 Feng codegen 输出的稳定 `#line` 映射。
- `.fd` 只补充 frame 名称重写、变量显示名映射以及少量特殊 carrier 的读取提示，不承担“逐地址反查源码行”的主要职责，也不复制 LLDB 已能给出的 scope / type / children 信息。
- 设计约束进一步明确为：**凡是 LLDB 已能正确决定的事，`feng dap` 不重复求解；但只要 LLDB 给出的当前 frame / 可见变量集合是正确的，`feng dap` 的重写结果也必须确定且正确，不能再靠猜测。**

### 2.5 Watch / Evaluate 边界

- 第一阶段只支持**只读子集**。
- 允许的表达式类型：
  - 标识符
  - 成员访问
  - 常量整数字面量索引访问
  - 已解析叶子值上的简单算术 / 比较
- 不允许：
  - 函数调用
  - 分配
  - 赋值
  - 可能触发 retain / release 语义变化的动作
  - 任意完整 Feng 表达式求值

## 3. 现状事实

### 3.1 当前可复用能力

以下能力已经存在，DAP 首版应直接复用：

- `src/cli/compile/driver.c`
  - 非 `release` 构建当前已经向主机编译器传递 `-O0` 与 `-g`。
- `src/codegen/codegen.h`
  - 已有 `FengCodegenOptions.emit_line_directives` 选项面。
- `src/parser/parser.h`
  - `FengExpr`、`FengStmt`、`FengBinding`、`FengParameter` 等节点都带 `FengToken`，具备 line / column 来源。
- `src/semantic/semantic.h`
  - 已有 `FengSemanticTypeFact` 与 `feng_semantic_lookup_type_fact(...)`，可查询绑定或其他语义站点的类型事实。
- `src/runtime/feng_runtime.h`
  - 已有 `FengRuntimeTypeKind`、`FengTypeDescriptor`、managed field metadata，可作为值显示基础。
- `editors/feng-vscode/extension.js`
  - 现有扩展已具备独立的 VS Code 扩展宿主与语言客户端接入点。

### 3.2 当前已知缺口

- `src/codegen/codegen.c` 目前没有真正消费 `emit_line_directives`。
- 编译器当前没有 artifact-scoped 的调试 sidecar 输出。
- 当前没有稳定的“用户可见 Feng 局部变量 -> 生成 C lvalue / symbol”映射协议，尤其缺少 capture / `self` / 特殊 carrier 的统一读取约束。
- 当前没有 `feng dap`，也没有 editor 侧 debugger contribution / launch configuration provider。
- 当前 `.ft` span 只覆盖声明，不覆盖局部变量或词法作用域。

## 4. 总体方案

### 4.1 分层架构

整体链路按以下 4 层工作：

- **编译层**
- Feng parser / semantic 产生 AST、token、type facts。
- codegen 在非 `release` 构建下生成带稳定 `#line` 的 C 源码，并同时导出最小 `.fd` 元数据。

- **主机调试信息层**
- 主机 C 编译器在非 `release` 构建中，基于 `#line` 与 `-g` 生成原生调试信息。
- LLDB 直接消费二进制与其原生调试信息。

- **`feng dap` 适配层**
- `feng dap` 启动 `lldb-dap`，并作为其上层 proxy。
- `feng dap` 读取 `.fd`，把 editor 看到的 stack frame、变量名和只读 watch 结果重写为 Feng 语义视角。
- `feng dap` 只接管语义翻译，不重做 LLDB 已有的进程控制、源码定位和原始变量枚举。

- **IDE 集成层**
- `editors/feng-vscode` 首先提供 debugger contribution、launch config、preLaunchTask 与 `feng dap` 启动入口。
- 后续其他编辑器只需对接 `feng dap`，不重复实现变量映射和 frame 重写逻辑。

### 4.2 为什么不扩展 `.ft`

不把 DAP 元数据塞进 `.ft`，原因如下：

- `.ft` 的职责是**声明级跨模块事实与 workspace cache**。
- `.ft` 是模块级、声明级、可用于包分发的格式，而局部变量和词法作用域属于**产物级、调试态、不可分发**的信息。
- 把 locals / frames / display hints 混入 `.ft`，会破坏当前 `.ft` 的边界，扩大语言服务与调试系统的耦合面。
- DAP 所需数据与最终 binary 强绑定，因此应当与 binary 同级产出，而不是与模块 `.ft` 绑定。

### 4.2.1 `.fd` 与 `.ft` 的重叠与分界

`.fd` 与 `.ft` 确实存在一部分数据重叠，但重叠只限于“调试展示时会再次用到的少量命名事实”，例如：

- callable 的显示名
- 局部变量的 Feng 显示名
- 极少量调试展示需要复用的字符串标签

`.fd` 独有而 `.ft` 不应承担的信息包括：

- 与最终 binary 强绑定的 artifact 指纹
- frame 的 `backend_symbol -> display_name` 映射与 `frame_policy`
- 局部变量的 `backend_name -> display_name` 映射
- capture / `self` / 特殊 carrier 所需的可选 `read_expr`

因此，从**语义边界**上，`.ft` 不能直接充当 `.fd`。

从**纯文件格式机制**上，现有 `.ft` 的 FST1 容器确实具备 `Header + Section Directory + Sections` 的追加扩展能力，理论上可以继续新增 workspace-only / ignorable section 去描述 `.fd` 的一部分字段；但当前 `.ft` 规范已经明确把 `.ft` 定义为**模块级声明缓存**，并且明确不把局部变量、临时值、控制流图放进 `.ft`。

如果把 `.fd` 强行并入 `.ft`，会同时带来三个问题：

- 破坏当前 `.ft` consumer 对“声明查询视图”的稳定预期
- 把 module-scoped symbol cache 与 artifact-scoped debug metadata 混在一起
- 让 `pack`、provider、LSP 与 DAP 的职责边界变得含混

所以这里的结论是：**从格式机制上“能扩”，从当前 `.ft` 规范和职责边界上“不该用”**。首版仍保留独立 `.fd`。

### 4.3 为什么采用 `lldb-dap` proxy

首版不直接裸调 LLDB API，优先采用 `lldb-dap` proxy，原因如下：

- 断点、单步、线程、进程生命周期管理由成熟后端负责。
- Feng 适配层只保留“语义翻译”职责，避免把原生调试器实现带进 `feng dap` 代码库。
- 后续若需要支持 Linux / Windows，只需替换 `feng dap` 下游后端能力层，而不必重写整个 Feng 语义层。
- 后续若需要支持 VS Code 之外的编辑器，也只需新增 editor 侧接入，而不必复制一套适配逻辑。

## 5. `.fd` 调试 sidecar 设计

### 5.1 产物位置与生命周期

首版建议采用以下规则：

- `target=bin` 且非 `release` 时，编译结果除二进制外，默认并排生成 `.fd`。
- 产物路径形态：
  - 二进制：`build/bin/<artifact>`
  - 调试 sidecar：`build/bin/<artifact>.fd`
- `release` 默认不生成 `.fd`，也不额外要求保留调试态插桩。
- `target=lib` 第一阶段不生成 `.fd`。
- `feng pack` 不打包 `.fd`。
- `.fd` 属于本地调试产物，可在 clean 时与其他 build 产物一并清除。

### 5.2 文件格式

首版建议：`.fd` 采用 **私有二进制容器格式**。

原因：

- 当前工程没有现成的 JSON 序列化 / 反序列化模块，也不计划为 `.fd` 单独引入这一类依赖。
- 现有 `feng.fm` 文本语法只服务于项目 / 包清单，不应被扩展成通用调试数据语言。
- `.fd` 是编译器生成、`feng dap` 消费的私有 sidecar，不需要人工编辑，采用二进制更符合当前 C 实现习惯。
- 仓库中已经存在 `.ft` 一类“header + section directory + section payload”的二进制写出经验，可直接复用设计方法。

格式约束：

- 固定使用 little-endian。
- 不允许直接把 C struct 原样写盘；必须逐字段显式序列化，避免宿主 ABI / 对齐差异渗入格式。
- 采用 `header + section directory + sections` 的最小容器布局。
- 未识别 section 在 reader 侧必须可忽略，为后续 append-only 演进保留空间。

### 5.3 设计原则

`.fd` 的设计目标是：**只保存 LLDB 不知道、但 Feng 语义重写必须知道的最小事实**。

- `.fd` 以“名称映射 + 少量特殊读取提示”为主。
- `.fd` 不复用、也不扩展 `feng.fm` 文本格式；`feng.fm` 继续只承担项目 / 包清单职责。
- `.fd` 不复制源码映射表；源码定位继续以 `#line` + DWARF 为准。
- `.fd` 不复制词法作用域树；当前激活 scope 继续以 LLDB 返回结果为准。
- `.fd` 不复制完整类型图、显示规则库或成员布局数据库；首版优先复用 LLDB 已有值摘要与 children 枚举。
- `.fd` 不是独立的变量解析器；它只对 **LLDB 当前已经判定为可见** 的 backend variable 做确定性重写。
- 若重写阶段出现 0 个候选或多个候选，`feng dap` 必须报错为“无法唯一确定绑定”，而不是猜一个继续执行。

### 5.4 容器布局

`.fd` 容器建议只保留以下最小布局：

- `header`
  - `magic`
  - `version`
  - `flags`
  - `section_count`
  - `section_dir_offset`
- `section directory`
  - 每个 section 记录：`kind`、`offset`、`size`、`record_count`
- `META`
  - binary 绑定信息
- `STRS`
  - 字符串表
- `FRMS`
  - frame 重写记录
- `VARS`
  - variable 重写记录

首版建议 section kind 固定为：

- `META`
- `STRS`
- `FRMS`
- `VARS`

### 5.5 建议 section 语义

#### `META`

- `binary_path_strid`
- `content_fingerprint`
  - 首版固定为对目标 binary 完整文件字节做 `FNV-1a 64` 计算；version 1 不单独保留算法字段

#### `STRS`

- 采用 1-based string id。
- section 内部按 `u32 length + raw bytes` 顺序顺排，不要求 NUL 结尾。
- string id `0` 保留为“空值 / 缺失”。

#### `FRMS`

每个 frame 至少记录：

- `backend_symbol_strid`
  - 最终进入链接层、可被 `lldb-dap` 报出的稳定符号名。
- `display_name_strid`
  - 需要呈现给用户的 Feng callable 名称。
- `frame_policy`
  - `visible` / `hidden` / `collapse`。

#### `VARS`

每个变量至少记录：

- `frame_backend_symbol_strid`
  - 该映射所属的 callable / frame。
- `backend_name_strid`
  - `lldb-dap` 可见的原始变量名。
- `display_name_strid`
  - 用户在 Feng 源码中看到的名字。
- `kind`
  - `param` / `local` / `capture` / `self`。
- `read_expr_strid`
  - 可选；当 `backend_name` 指向 carrier 而非最终用户值时，提供一个最小后端读取表达式。

补充约束：

- 唯一键是 `frame_backend_symbol_strid + backend_name_strid`，不是 `display_name_strid`。
- `display_name_strid` 允许重复；同一 callable 中不同词法位置的 shadowing 变量，允许映射到不同 `backend_name_strid`。
- `feng dap` 只对 LLDB 当前返回的可见变量集合做重写，因此当 LLDB 的可见性结果正确时，同名变量不会串到未激活绑定上。

### 5.6 一个最小示例

```text
Header:
  magic = "FD01"
  version = 1
  flags = 0
  section_count = 4

Section Directory:
  META offset=0x40 size=... records=1
  STRS offset=0x60 size=... records=9
  FRMS offset=0xA0 size=... records=2
  VARS offset=0xD0 size=... records=2

STRS:
  1 -> "build/bin/demo"
  2 -> "feng_demo_main"
  3 -> "main"
  4 -> "feng_runtime_dispatch_1"
  5 -> "_l_message_1"
  6 -> "message"
  7 -> "_l_count_cell_2"
  8 -> "count"
  9 -> "(_l_count_cell_2->value)"

META:
  binary_path_strid = 1
  content_fingerprint = 0x7f23d91ab4c60218

FRMS:
  [backend_symbol_strid=2, display_name_strid=3, frame_policy=visible]
  [backend_symbol_strid=4, display_name_strid=0, frame_policy=hidden]

VARS:
  [frame_backend_symbol_strid=2, backend_name_strid=5, display_name_strid=6, kind=local,   read_expr_strid=0]
  [frame_backend_symbol_strid=2, backend_name_strid=7, display_name_strid=8, kind=capture, read_expr_strid=9]
```

这个示例表达的重点不是字节偏移本身，而是：**`.fd`` 只保留 string table + 最小记录表，不引入通用对象树或通用文本语法。**

### 5.7 为什么不是“只存名字映射”

从主路径看，`.fd` 的主体确实应该尽量接近“C 名 -> Feng 名”的简单映射；这也是首版刻意追求的最小化方向。

但只靠纯名字映射仍然不够，原因只有一条：**并不是每个用户可见变量都直接对应一个可展示的 C 局部变量**。

- 普通参数和普通局部变量，大多只需要 `backend_name + display_name`。
- `capture`、`self` 或其他特殊 carrier 场景，LLDB 看到的往往是 cell、slot、wrapper pointer，而不是最终用户值。
- 这类场景需要一个可选 `read_expr`，让 `feng dap` 能从 carrier 读出真正要展示的值。

因此，首版结论不是“做成完整调试数据库”，而是“以名字映射为主，只为特殊 carrier 多保留一个可选读取提示”。

采用二进制容器后，这个原则仍然不变：`.fd` 只是把这些最小映射记录编码成固定 section，而不是升级成更通用的数据语言。

进一步说，首版对 shadowing 的处理原则也保持最小化：

- 不在 `.fd` 中复制词法作用域树。
- 不让 `feng dap` 自己推断“当前应该选哪个同名变量”。
- 而是先信任 LLDB 给出的当前可见变量集合，再在这个集合上按 `backend_name -> display_name` 做重写。

这样可以保持实现最简，同时满足一个关键正确性约束：**只要 LLDB 当前给出的可见绑定集合是正确的，`feng dap` 的同名变量映射就必须正确。**

### 5.8 命名稳定性要求

为了让 `feng dap` 能稳定取值，codegen 必须对“用户可见变量”的后端命名建立稳定约束：

- `frames.backend_symbol` 必须稳定，不能依赖会漂移的临时命名。
- `variables.backend_name` 必须对应 `lldb-dap` 实际可见的原始变量名。
- 同一个 `frame_backend_symbol` 下，不同绑定若对应不同后端变量，必须产生不同的 `backend_name`；shadowing 不允许在后端名字层面复用同一个可见槽位。
- 纯内部临时值默认不进入 `.fd`，除非它们正是用户值的 carrier。
- 需要 `read_expr` 的特殊 carrier 必须由 codegen 显式标出，不能让 `feng dap` 反推实现细节。

### 5.9 源码映射策略

源码映射采用“**原生调试信息为主，`.fd` 为辅**”的策略：

- 可停点、step line、stack line 依赖 `#line` + `-g`。
- `.fd` 不维护“每个生成 C 行 -> Feng 行”的完整冗余表。
- `.fd` 只记录 frame 重写和变量映射最小信息。
- 若原生后端出现个别 frame 噪声，`feng dap` 再依据 `frames.frame_policy` 做折叠或过滤。
- `.fd` 的 reader 只需要顺序读取 `META` / `STRS` / `FRMS` / `VARS` 四类 section，不需要实现通用对象反序列化器。

## 6. `feng dap` 与 `lldb-dap` 适配层

### 6.1 适配层职责

`feng dap` 的职责只包括：

- 定位目标 binary 与其 `.fd`
- 启动 `lldb-dap`
- 向编辑器暴露统一 DAP 入口，并把请求转发给 `lldb-dap`
- 读取 `.fd`，把 stack / variable / evaluate 结果重写成 Feng 视角
- 隐藏或折叠对用户无意义的 runtime / generated helper frame 与内部 carrier 变量

### 6.2 适配层不负责的事

以下能力不属于首版 adapter 职责：

- 自己实现原生调试器
- 自己做源码语义分析
- 自己重新构建或链接工程
- 实现完整 Feng 表达式解释器
- 实现 editor 专属 UI 逻辑

### 6.3 DAP 请求处理策略

#### `launch`

- 编辑器把目标 binary、工作目录和 preLaunchTask 结果交给 `feng dap`。
- `feng dap` 校验 `.fd` 中记录的 `META.content_fingerprint` 与当前 binary 重新计算的内容指纹是否匹配。
- 校验通过后，由 `feng dap` 拉起并代理 `lldb-dap`。

#### `setBreakpoints`

- 优先直接把 `.ff` 文件断点交给 `lldb-dap`，让其利用原生调试信息绑定。
- 若遇到个别绑定不稳定情形，再用 `.fd.frames` 仅做诊断与命中后重写，不把 `.fd` 变成主断点数据库。

#### `stackTrace`

- 先拿到 `lldb-dap` 的原生 frame 列表。
- 再用 `.fd.frames` 的 `backend_symbol` 与 `frame_policy` 重写为 Feng callable 名称。
- 对纯 runtime / generated helper frame 默认隐藏，必要时可提供开发者模式开关显示原生 frame。

#### `scopes` / `variables`

- 作用域激活关系以 `lldb-dap` 返回结果为准，`.fd` 不再维护独立 scope 树。
- `feng dap` 先拿当前 scope 下 **LLDB 实际返回的可见变量集合**，再依据当前 frame 的 `backend_symbol` 在 `.fd.variables` 中查找可重写条目。
- 普通变量只对这个“当前可见集合”里的 `backend_name` 做 `backend_name -> display_name` 改名，不自行补推隐藏绑定。
- 遇到 `read_expr` 时，再基于 carrier 执行一次额外读取，把结果包装成用户看到的 Feng 变量。
- 若当前可见集合在重写后对某个标识符出现 0 个候选或多个候选，`feng dap` 必须报错，不做猜测性绑定。

#### `evaluate`

- 仅支持第一阶段允许的只读子集。
- `feng dap` 自带一套极小表达式解析器，只覆盖 watch 子集，不复用完整编译器 parser。
- 标识符优先在 **LLDB 当前可见变量集合** 上做解析，再用 `.fd.variables` 做显示名与 backend 名之间的双向对照。
- 如果某个标识符在当前可见集合上无法唯一落到一个 backend 变量，直接报歧义错误，而不是退化为猜测。
- 成员访问和索引访问优先沿 LLDB 返回的 children 继续下钻。
- 只有遇到特殊 carrier 时，才使用 `.fd.variables.read_expr` 参与后端读取。

### 6.4 值显示策略

第一阶段按以下原则显示：

- builtin 标量：直接复用 `lldb-dap` 原始值。
- enum：若能从现有类型信息稳定恢复展示名，则做轻量重写；否则先退回原始整数值。
- string / array / object：优先复用 LLDB 已有摘要与 children；`feng dap` 只负责隐藏明显的 runtime 噪声。
- callable / spec：首版不引入独立 `.fd` 显示数据库，必要时只提供稳定标签重命名。
- `.fd` 不新增 `types` / `display` 顶层区块；若后续确实发现 LLDB 信息不足，再单独评估增量字段。

## 7. 必须改动的代码边界

### 7.1 编译器侧

以下改动属于首版前置条件：

- `src/codegen/codegen.h`
  - 保留并正式启用 `FengCodegenOptions.emit_line_directives` 控制面。
- `src/codegen/codegen.c`
  - 真正消费 `emit_line_directives`。
  - 为 frame / variable 生成稳定调试元数据。
  - 产出 `.fd` 所需的中间调试记录。
- `src/cli/compile/direct.c`
  - 直编路径要显式把 debug-aware codegen options 传给 codegen。
- `src/cli/compile/legacy.c`
  - legacy 路径保持相同传参约束。

### 7.2 新增调试元数据模块

建议新增独立模块承载 `.fd` 数据模型与写盘逻辑，例如：

- `src/debug/`
  - 调试元数据结构
  - `.fd` binary writer
  - string table / section directory 编码
  - 编译产物与 `.fd` 的指纹绑定

原因：

- `.fd` 的职责不同于 `.ft`。
- 不应把 `.fd` writer 混入 `src/symbol/`。
- 也不应把所有序列化细节堆进 `src/codegen/codegen.c`。

### 7.3 `feng dap` 与编辑器集成侧

以下改动属于首版前置条件：

- `feng dap`
  - 新增统一 DAP proxy 入口，负责启动 / 代理 `lldb-dap`、读取 `.fd`、完成 frame / variable 重写。
- `editors/feng-vscode/package.json`
  - 新增 debugger contribution、配置 schema 与 launch 类型。
- `editors/feng-vscode/extension.js`
  - 新增 debug configuration provider、task / launch wiring、`feng dap` 启动入口。
- 未来其他编辑器
  - 只需要实现各自的 debugger client 接入，不再复制 frame / variable 映射逻辑。

### 7.4 明确不应改动的部分

以下部分不应为了首版 DAP 被改造成新的耦合中心：

- `src/cli/lsp/runtime.c`
  - 原因：LSP 与 DAP 是不同协议，不应把调试职责塞进语言服务实现。
- `src/symbol/ft_write.c`
  - 原因：`.ft` 边界不应被扩大为局部变量和帧数据库。
- `src/runtime/`
  - 第一阶段只复用既有类型描述与显示基础，不以前置新增专用调试 runtime 为条件。

## 8. 分阶段实施建议

### 8.1 Phase 1：规范与边界收敛

- 收敛本方案文档。
- 后续同步更新：
  - `docs/feng-build.md`
  - `docs/feng-cli.md`
  - `docs/feng-symbol-table.md`
- 明确 `.fd` 的产物语义、生命周期与 `.ft` 边界。

### 8.2 Phase 2：编译器源码映射与 `.fd`

- 落实 `#line` 输出。
- 明确调试可见 callable / scope / variable 的选择规则。
- 生成 `.fd`。
- 为变量和 callable 后端命名建立稳定约束。

### 8.3 Phase 3：`feng dap` 统一适配层与 VS Code 接入

- 新增 `feng dap`，启动并代理 `lldb-dap`。
- 实现 stackTrace / variables / evaluate 的最小 Feng 语义重写。
- VS Code 侧接入 debugger contribution、launch 和 preLaunchTask 联动。
- 保持 editor-neutral 边界，为后续其他编辑器接入保留复用面。

### 8.4 Phase 4：只读 watch 子集

- 支持 identifier。
- 支持成员访问。
- 支持常量整数字面量索引。
- 支持标量值上的简单算术 / 比较。
- 明确拒绝其他表达式类型。

### 8.5 Phase 5：测试与回归

- codegen / `.fd` golden tests
- adapter 协议测试
- VS Code + macOS + LLDB smoke 测试
- 现有 LSP 与编译器测试回归

## 9. 验证要求

第一阶段完成后，至少需要满足以下验证：

1. 在非 `release` 的 `target=bin` 项目中，LLDB 断点能直接落在 `.ff` 源码，而不是 `build/ir/c/feng.c`。
2. stack trace 中展示的是 Feng callable 名称，而不是裸生成 C helper 名称。
3. locals / params 以 Feng 名称显示，shadowing 情况下作用域选择正确。
4. 对 string / array / enum / object 的值显示符合 Feng 语义预期。
5. watch 子集中的 identifier / member / index / simple arithmetic 能稳定工作；不支持的表达式给出明确错误，而不是静默返回错误值。
6. release 构建行为不受影响，不额外产出 `.fd`。

## 10. 风险与缓解

### 10.1 `#line` 精度不足

风险：

- 若 codegen 的 `#line` 插入点不稳定，step 行为会出现跳行或粘连到生成 C。

缓解：

- 以“函数入口、语句入口、可停点表达式入口”为最小稳定插入点。
- 对复杂控制流先做最小精度闭环，再逐步细化。

### 10.2 局部变量后端命名漂移

风险：

- 若 `backend_name` 或 `read_expr` 不稳定，`feng dap` 将无法稳定取值，或把重写结果落到错误绑定上。

缓解：

- 明确 codegen 的调试命名规则。
- 保证 `frame_backend_symbol + backend_name` 能稳定标识一个可重写绑定。
- 对同名 shadowing 变量，始终依赖 LLDB 当前可见变量集做筛选；候选不唯一时直接报错，不猜。
- 用 golden test 固定 shadowing、嵌套 block、lambda capture 等场景。

### 10.3 `.fd` 过大或字段失控

风险：

- 若把太多运行时细节直接写入 `.fd`，或让二进制 section 无节制扩张，会导致调试产物膨胀与格式漂移。

缓解：

- `.fd` 只记录“Feng 语义显示所需的最小闭环信息”。
- 地址级源码映射继续交给原生调试信息。
- 采用 versioned header 与 append-only section 规则；新增能力优先追加 section，而不是重写已有记录语义。

### 10.4 Watch 范围失控

风险：

- 若首版就尝试支持任意表达式，将把调试适配层扩展成新的解释器。

缓解：

- 第一阶段只支持显式列出的只读子集。
- 其余表达式统一报“不支持”，不做隐式降级。

## 11. 结论

Feng 的首版源码级调试，最现实、风险最低的路径是：

- **继续复用现有 Feng -> C -> clang/LLDB 主链**；
- **用 `#line` + 主机编译器原生调试信息解决断点 / step / stack line**；
- **用最小独立二进制 `.fd` sidecar 解决 frame 名称重写、变量名称映射与少量特殊 carrier 读取提示**；
- **用 `feng dap` 统一代理 `lldb-dap`，把 editor 看到的调试对象重写为 Feng 语义体验**；
- **让 VS Code 等编辑器只负责 launch / task / UI，而不承载核心映射逻辑**。

这条路径既能保持当前编译器与 `.ft` 边界稳定，又能在不引入第二套执行模型的前提下，把核心调试适配收敛到 `feng dap`，同时为后续其他编辑器接入保留统一复用层。
