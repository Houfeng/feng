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
- launch 入口只覆盖 **`target=bin`**，但允许单步进入同一调试闭包内的本地路径依赖源码。
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
- 独立 `target=lib` 调试会话
- 把调试协议并入 `feng lsp`

### 1.2 里程碑 TODO

- [x] 收敛调试方案、实施边界与文档约束。
- [x] 在非 `release` 发码链路补齐 `#line` 与抽象调试信息输出。
- [x] 在 `src/debug/` 中生成并汇总 `.fd`。
- [x] 新增 `feng dap` 代理层并消费 `.fd`。
- [ ] 补齐 VS Code 集成与回归验证。

### 1.3 本次实施硬约束

- 本次 DAP 支持禁止改动核心编译器的**词法、语法、语义**层；允许的编译器改动只限非 `release` 发码链路中补充稳定 `#line` 与导出生成 `.fd` 所需的抽象调试信息。
- codegen 不直接感知、命名或生成 `.fd`；它只输出与容器格式解耦的抽象调试信息结构，这些结构的命名也不应直接绑定 `.fd`。
- `src/debug/` 负责基于 codegen 输出的抽象调试信息生成与汇总最终 `.fd`；`src/dap/` / `feng dap` 只消费 `.fd`，不把 `.fd` 容器细节反向渗入 codegen。
- 其他不相关代码本次不得直接变动；若确实需要扩大修改面，必须由开发者决策后再继续。

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
- 核心编译器首版新增职责原则上只限非 `release` 构建下的两件事：发出稳定 `#line`，并导出当前产物所需的抽象调试信息；编译驱动只做最小的 debug 选项透传与产物编排。
- 第一阶段即引入独立 `feng dap`，由它启动并代理 `lldb-dap`；原生单步、线程、断点命中与底层变量读取仍由 `lldb-dap` 负责。
- stack、variable、watch 与 frame 语义重写统一在 `feng dap` 完成；编辑器只负责 launch 配置、task 串接和调试 UI。
- 长期链路明确为 `editor -> feng dap -> lldb-dap -> LLDB`；VS Code 只是首个接入方，后续 Zed 等编辑器复用同一层。
- 对本地路径依赖场景，`feng dap` 仍只读取“被 launch 的最终 binary 对应的单个 `.fd`”；主项目与递归本地依赖的调试元数据汇总由构建阶段完成，不把多 sidecar 追踪逻辑下放到编辑器或运行期适配层。

### 2.3 数据边界

- `build/obj/symbols/**/*.ft` 继续是**声明级 workspace cache**。
- `.ft` 中已有的 span 仍只服务于声明级定位、hover、definition、completion 等语言服务消费场景。
- 局部变量、词法作用域、帧重写、调试显示提示等调试专用信息，统一落入新的 **artifact-scoped sidecar**：`.fd`。
- 对 `target=bin` 调试会话而言，这个 artifact-scoped `.fd` 描述的是**最终被 launch binary 的本地调试闭包**，可覆盖主项目与递归本地路径依赖，但仍只服务当前本地构建结果。
- `.fd` 不进入 `.fb` 分发包，不作为跨包公共接口。

### 2.4 源码映射权威来源

- 断点绑定、step 行号和 stack line 的**第一权威来源**是主机编译器生成的原生调试信息。
- 这些原生调试信息必须来自 Feng codegen 输出的稳定 `#line` 映射。
- `#line` 中的文件名参数不应写宿主磁盘路径，而应固定写成逻辑源码 URI：`PKG_NAME://<package-relative path>`。
- `PKG_NAME` 取源码所属包的 `feng.fm` 中 `name` 字段；`<package-relative path>` 相对包根目录计算，统一使用 `/`，例如 `std://src/foo/bar.ff`。
- 这样做的直接原因是：monorepo / 本地路径依赖调试时，宿主磁盘路径会随工作树、缓存位置或依赖展开方式漂移；固定使用逻辑源码 URI，才能把 LLDB 看到的源码标识从宿主路径解耦出来，保留可移植性与后续 editor-neutral 转换空间。
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
- 当前没有“主项目 + 本地路径依赖项目”的统一调试闭包产物编排。
- 当前 `.ft` span 只覆盖声明，不覆盖局部变量或词法作用域。

## 4. 总体方案

### 4.1 分层架构

整体链路按以下 4 层工作：

- **编译层**
- Feng parser / semantic 产生 AST、token、type facts。
- codegen 在非 `release` 构建下生成带稳定 `#line` 的 C 源码，并同时导出与 `.fd` 容器解耦的抽象调试信息；`src/debug/` 再基于这些抽象信息为当前产物生成 `.fd`，并在顶层可调试 `target=bin` 构建中把主项目与递归本地依赖的 `.fd` 汇总成最终 binary 对应的单个 `.fd`。

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

- 非 `release` 的 `target=bin` 与 `target=lib` 都生成 `.fd`。
- 这个最终 `.fd` 必须覆盖当前 `target=bin` 工程以及其递归本地路径依赖项目中会进入最终 binary 的 Feng 代码，从而支持 monorepo / workspace 内跨项目单步进入。
- 本地 `target=lib` 依赖在非 `release` 构建下也直接产出普通 `.fd`，但该 `.fd` 属于构建内部中间产物，不是 `feng dap` 直接消费的首版 launch artifact。
- 扩展名仍然直接使用 `.fd`，不为本地依赖中间产物再发明新的专有扩展名。
- 产物路径形态：
  - `target=bin`：`build/bin/<artifact>` 与 `build/bin/<artifact>.fd`
  - `target=lib`：库产物与同级 `.fd`
- `release` 默认不生成 `.fd`，也不额外要求保留调试态插桩。
- 首版不提供“单独启动一个 `target=lib` 项目”的调试入口；`target=lib` 在首版只承担“为顶层 `target=bin` 调试闭包产出可合并中间 `.fd`”的职责。
- `feng pack` 不打包 `.fd`。
- `.fd` 属于本地调试产物，可在 clean 时与其他 build 产物一并清除。

#### 5.1.1 本地依赖 `.fd` 汇总

建议把“单个 `.fd` 覆盖本地依赖”拆成两层产物语义：

- **最终 `.fd`**：只在非 `release` 的 `target=bin` 构建完成后生成，位于最终 binary 同级，供 `feng dap` 独占消费。
- **中间 `.fd`**：本地 `target=lib` 依赖在非 `release` 构建下生成的普通 `.fd`，只用于被上层构建汇总，不直接暴露为首版调试入口契约。

建议的汇总流程如下：

1. 主项目与每个递归本地 `target=lib` 依赖在各自 codegen / 编译阶段先导出抽象调试信息。
2. `src/debug/` 基于这些抽象调试信息为当前产物写出普通 `.fd`；对本地 `target=lib` 来说，这个 `.fd` 与 `target=bin` 使用同一容器布局，只是首版不会被 `feng dap` 直接作为 launch sidecar 消费。
3. 顶层 `target=bin` 构建在生成自身 `.fd` 时，收集整条本地依赖图中的依赖 `.fd`，提取其中可合并的 `PKGS` / `FRMS` / `VARS` section，重新做 string interning 后并入自己的 `.fd`。
4. 汇总器对冲突做显式校验：若两个输入 `.fd` 给同一 `frame_backend_symbol + backend_name` 提供不一致映射，或给同一 `PKG_NAME` 提供不一致的本地包根，构建立即报错，不把歧义留到运行期。
5. 依赖 `.fd` 的 `META` 不参与合并；最终只保留顶层 `target=bin` 自身的 `META.binary_path_strid` 与 `META.content_fingerprint`。

实现边界补充：构建链必须把递归本地依赖生成的 `.fd` 路径作为**独立输入**传给顶层 `target=bin` 的最终 sidecar 汇总步骤，不能试图从 `--pkg <.fb路径>` 或 `.fb` 内容反推依赖 `.fd`；`.fd` 不进入 `.fb`，而 `.fb` 也不承担本地调试 sidecar 的定位职责。

这样做的原因是：

- `feng dap` 继续只读一个 sidecar，运行时复杂度最低。
- 不需要再定义第二种“调试片段专用扩展名”或第二套 reader / writer；实现上直接复用同一个 `.fd` 容器更简单。
- 子 `.fd` 的 `META` 可以绑定它自己的库产物；顶层汇总时丢弃这些子 `META` 并重写最终 binary 的 `META` 即可，不会把“库产物指纹”错误冒充成“最终可执行文件指纹”。
- 不应把这些内容做成完整 `.fb`：`.fb` 是分发包接口，而 `PKGS` 本地包根、frame 重写和局部变量映射都属于本地调试态私有信息，不应进入发布工件。
- 合并发生在构建期，错误能尽早暴露，不需要把冲突处理推迟到调试会话里。

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
- `.fd` 对编辑器与 `feng dap` 始终表现为“当前 launch binary 的单一 sidecar”，不要求运行期再递归发现或拼接多个依赖 sidecar。
- 若构建阶段采用“本地依赖中间 `.fd` -> 最终 `.fd`”两层模型，`.fd` reader 不需要感知来源层级；汇总后的结果必须与“单项目直接产出的 `.fd`”具有同一消费语义。
- `.fd` 不复用、也不扩展 `feng.fm` 文本格式；`feng.fm` 继续只承担项目 / 包清单职责。
- `.fd` 不复制源码映射表；源码定位继续以 `#line` + DWARF 为准。
- `.fd` 可以包含极小的 `PKG_NAME -> local package root` 映射，但这只服务编辑器路径转换，不等于复制 line table。
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
- `PKGS`
  - package URI 到本地包根的最小映射
- `FRMS`
  - frame 重写记录
- `VARS`
  - variable 重写记录

首版建议 section kind 固定为：

- `META`
- `STRS`
- `PKGS`
- `FRMS`
- `VARS`

### 5.5 建议 section 语义

#### `META`

- `binary_path_strid`
- `content_fingerprint`
  - 首版固定为对目标 binary 完整文件字节做 `FNV-1a 64` 计算；version 1 不单独保留算法字段
- 每个实际落盘的 `.fd` 都带自己的 `META`，并绑定它同级的本地产物。
- 顶层 `target=bin` 汇总时只保留最终 binary 的 `META` 语义；子 `target=lib` `.fd` 的 `META` 只用于中间产物自描述，不参与最终 `.fd` 的绑定语义。

#### `STRS`

- 采用 1-based string id。
- section 内部按 `u32 length + raw bytes` 顺序顺排，不要求 NUL 结尾。
- string id `0` 保留为“空值 / 缺失”。

#### `PKGS`

每个 package root 至少记录：

- `package_name_strid`
  - 对应 `#line` 逻辑 URI 中的 `PKG_NAME`。
- `local_root_path_strid`
  - 当前本地调试闭包内该包的实际包根目录；用于把 `PKG_NAME://<package-relative path>` 还原成编辑器可打开的本地路径。

补充约束：

- `local_root_path_strid` 允许只出现在最终 `.fd` 或中间 `.fd` 这样的本地调试构建产物里；`feng pack` 不分发这一信息。
- 同一最终 `.fd` 中，`package_name_strid` 必须唯一。

#### `FRMS`

每个 frame 至少记录：

- `backend_symbol_strid`
  - 最终进入链接层、可被 `lldb-dap` 报出的稳定符号名。
  - 当前应直接记录 codegen 产出的 callable C 符号本体，例如模块名参与 mangle 后的函数名；若该 callable 还带 overload / 参数签名后缀，也必须记录 LLDB 实际看到的完整符号。
- `display_name_strid`
  - 需要呈现给用户的 Feng callable 名称。
- `frame_policy`
  - `visible` / `hidden` / `collapse`。

补充约束：

- `frame_backend_symbol` 是对“正式代码生成符号”的引用，不再额外人为补一个仅供 debug 使用的包名前缀。
- 在当前规则下，最终依赖图中的模块名冲突会在编译器 / 构建阶段直接报错，因此基于模块 mangle 的 callable C 符号应在同一调试闭包内保持唯一。
- 若未来语言层允许不同包中同模块名并存，则需要调整 codegen 的正式符号 mangle 规则；`.fd` 继续跟随 LLDB 实际看到的符号，而不是单独引入另一套 debug-only 命名。

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
  section_count = 5

Section Directory:
  META offset=0x40 size=... records=1
  STRS offset=0x60 size=... records=11
  PKGS offset=0xA0 size=... records=1
  FRMS offset=0xC0 size=... records=2
  VARS offset=0xF0 size=... records=2

STRS:
  1 -> "build/bin/demo"
  2 -> "demo"
  3 -> "/workspace/demo"
  4 -> "feng_demo_main"
  5 -> "main"
  6 -> "feng_runtime_dispatch_1"
  7 -> "_l_message_1"
  8 -> "message"
  9 -> "_l_count_cell_2"
  10 -> "count"
  11 -> "(_l_count_cell_2->value)"

META:
  binary_path_strid = 1
  content_fingerprint = 0x7f23d91ab4c60218

PKGS:
  [package_name_strid=2, local_root_path_strid=3]

FRMS:
  [backend_symbol_strid=4, display_name_strid=5, frame_policy=visible]
  [backend_symbol_strid=6, display_name_strid=0, frame_policy=hidden]

VARS:
  [frame_backend_symbol_strid=4, backend_name_strid=7, display_name_strid=8, kind=local,   read_expr_strid=0]
  [frame_backend_symbol_strid=4, backend_name_strid=9, display_name_strid=10, kind=capture, read_expr_strid=11]
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
- `#line` 的文件名参数固定写成 `PKG_NAME://<package-relative path>`，不写宿主磁盘路径。
- `feng dap` 通过 `.fd.PKGS` 把这些逻辑 URI 还原成本地文件路径，再交给编辑器展示与断点绑定。
- `.fd` 不维护“每个生成 C 行 -> Feng 行”的完整冗余表。
- `.fd` 只记录 package-root 映射、frame 重写和变量映射这些最小信息。
- 若原生后端出现个别 frame 噪声，`feng dap` 再依据 `frames.frame_policy` 做折叠或过滤。
- `.fd` 的 reader 只需要顺序读取 `META` / `STRS` / `PKGS` / `FRMS` / `VARS` 五类 section，不需要实现通用对象反序列化器。

## 6. `feng dap` 与 `lldb-dap` 适配层

### 6.1 适配层职责

`feng dap` 的职责只包括：

- 定位目标 binary 与其 `.fd`
- 启动 `lldb-dap`
- 向编辑器暴露统一 DAP 入口，并把请求转发给 `lldb-dap`
- 在编辑器本地文件路径与 `#line` 使用的 `PKG_NAME://...` 逻辑源码 URI 之间做双向转换
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
- `feng dap` 只加载目标 binary 同级的单个 `.fd`，并校验其中记录的 `META.content_fingerprint` 与当前 binary 重新计算的内容指纹是否匹配。
- 校验通过后，由 `feng dap` 拉起并代理 `lldb-dap`。
- 当前已落地的 Phase 4 基线切面为：`feng dap` 本地响应 `initialize`，在 `launch` 前校验目标 binary 同级 `.fd` 与内容指纹，仅在校验通过后再拉起并接管 `lldb-dap`；stackTrace / variables / evaluate 的 Feng 语义重写继续在后续子项叠加。

#### `setBreakpoints`

- 编辑器仍以本地 `.ff` 文件路径下断点；`feng dap` 先依据 `.fd.PKGS` 把该路径转换成对应的 `PKG_NAME://<package-relative path>`，再交给 `lldb-dap` 利用原生调试信息绑定。
- 若某个本地文件路径无法唯一落到当前调试闭包中的一个 package URI，`feng dap` 立即报配置错误，不做猜测性绑定。
- 若遇到个别绑定不稳定情形，再用 `.fd.frames` 仅做诊断与命中后重写，不把 `.fd` 变成主断点数据库。

#### `stackTrace`

- 先拿到 `lldb-dap` 的原生 frame 列表。
- 若 frame source 来自 `PKG_NAME://...` 逻辑 URI，先依据 `.fd.PKGS` 还原为本地文件路径，再返回给编辑器。
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
- `src/codegen/mapping.h`
  - 承载与 `.fd` 容器解耦的抽象源码 / frame / variable 映射模型。
- `src/codegen/codegen.c`
  - 真正消费 `emit_line_directives`。
  - 为 frame / variable 生成稳定、与 `.fd` 容器解耦的抽象调试信息。
  - 不直接感知、命名或写出 `.fd`。
- `src/cli/compile/direct.c`
  - 直编路径要显式把 debug-aware codegen options 传给 codegen，并在非 `release` 时把 codegen 输出的抽象调试信息交给 `src/debug/`。
- `src/cli/project/compile.c` / `src/cli/deps/manager.c`
  - 项目构建链要把递归本地依赖的 `.fd` 路径与 `.fb` `package_paths` 分开传递；前者只服务最终 debug sidecar 汇总，后者继续只服务编译期包输入。
- `src/cli/compile/legacy.c`
  - legacy 路径保持相同传参约束。

### 7.2 新增调试元数据模块

建议新增独立模块承载 `.fd` 数据模型与写盘逻辑，例如：

- `src/debug/`
  - `.fd` binary writer
  - 中间 `.fd` / 最终 `.fd` reader / writer
  - string table / section directory 编码
  - 编译产物与 `.fd` 的指纹绑定
  - 本地路径依赖 `.fd` 到最终 `.fd` 的汇总

原因：

- `.fd` 的职责不同于 `.ft`。
- 不应把 `.fd` writer 混入 `src/symbol/`。
- 也不应把所有序列化细节堆进 `src/codegen/codegen.c`。
- codegen 只负责输出抽象映射模型，`.fd` 容器化与汇总应由 `src/debug/` 负责。
- `src/debug/` 应单向依赖 `src/codegen/mapping.h`，而不是反过来让 codegen 依赖 `.fd` 侧头文件。

### 7.3 `feng dap` 与编辑器集成侧

以下改动属于首版前置条件：

- `src/dap/` / `feng dap`
  - 新增统一 DAP proxy 入口，负责启动 / 代理 `lldb-dap`、读取 `.fd`、完成 frame / variable 重写。
- `editors/feng-vscode/package.json`
  - 新增 debugger contribution、配置 schema 与 launch 类型。
- `editors/feng-vscode/extension.js`
  - 新增 debug configuration provider、task / launch wiring、`feng dap` 启动入口。
- 未来其他编辑器
  - 只需要实现各自的 debugger client 接入，不再复制 frame / variable 映射逻辑。

### 7.4 明确不应改动的部分

以下部分不应为了首版 DAP 被改造成新的耦合中心：

- `src/lexer/`、`src/parser/`、`src/semantic/`
  - 原因：本次实施明确禁止改动词法、语法、语义层；DAP 支持不应借机扩大到前端核心语义面。

- `src/cli/lsp/runtime.c`
  - 原因：LSP 与 DAP 是不同协议，不应把调试职责塞进语言服务实现。
- `src/symbol/ft_write.c`
  - 原因：`.ft` 边界不应被扩大为局部变量和帧数据库。
- `src/runtime/`
  - 第一阶段只复用既有类型描述与显示基础，不以前置新增专用调试 runtime 为条件。
- 其他与本次 DAP 方案无直接关系的代码
  - 原因：若确需扩大修改面，必须由开发者先行决策，不能在本次实施中顺手改动。

## 8. 分步任务 TODO

- [ ] Phase 1：规范与边界收敛
  - [x] 收敛本方案文档。
  - [x] 同步更新 `docs/feng-build.md`。
  - [x] 同步更新 `docs/feng-cli.md`。
  - [x] 同步更新 `docs/feng-symbol-table.md`。
  - [x] 明确 `.fd` 的产物语义、生命周期与 `.ft` 边界。
  - [x] 把“禁止改动词法 / 语法 / 语义层、禁止顺手改 unrelated 代码”的实施约束写死。

- [x] Phase 2：编译器非 `release` 发码链路
  - [x] 落实基于 `PKG_NAME://<package-relative path>` 的 `#line` 输出。
  - [x] 定义与 `.fd` 容器解耦的抽象调试信息结构。
  - [x] 仅在 codegen 中输出抽象调试信息，不直接写 `.fd`。
  - [x] 为变量和 callable 后端命名建立稳定约束。

- [x] Phase 3：`src/debug/` 生成与汇总 `.fd`
  - [x] 基于 codegen 输出的抽象调试信息生成当前产物 `.fd`。
  - [x] 支持本地 `target=lib` 的非 `release` `.fd` 写出。
  - [x] 支持顶层 `target=bin` 提取并合并依赖 `.fd` 的 `PKGS` / `FRMS` / `VARS` section。
  - [x] 明确并实现最终 `META` 重写规则。

- [ ] Phase 4：`src/dap/` / `feng dap` 与 VS Code 接入
  - [x] 新增 `feng dap`，启动并代理 `lldb-dap`。
  - [ ] 实现 stackTrace / variables / evaluate 的最小 Feng 语义重写。
  - [ ] 实现本地文件路径与 `PKG_NAME://...` 逻辑源码 URI 的双向转换。
  - [ ] VS Code 侧接入 debugger contribution、launch 和 preLaunchTask 联动。
  - [ ] 保持 editor-neutral 边界，为后续其他编辑器接入保留复用面。

- [ ] Phase 5：只读 watch 子集
  - [ ] 支持 identifier。
  - [ ] 支持成员访问。
  - [ ] 支持常量整数字面量索引。
  - [ ] 支持标量值上的简单算术 / 比较。
  - [ ] 明确拒绝其他表达式类型。

- [ ] Phase 6：测试与回归
  - [ ] 补齐 codegen / 抽象调试信息 / `.fd` 的 golden tests。
  - [ ] 补齐 adapter 协议测试。
  - [ ] 补齐 VS Code + macOS + LLDB smoke 测试。
  - [ ] 执行现有 LSP 与编译器测试回归。
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
6. 本地路径依赖（例如 `feng.fm` 中的相对路径依赖）在同一非 `release` 调试会话中可被正常 step into，stack / locals 仍展示 Feng 语义名称，而不是退回裸 C 名称。
7. 生成的原生调试信息中，`#line` 文件名使用 `PKG_NAME://<package-relative path>` 逻辑 URI，而不是宿主磁盘路径；同时编辑器对本地文件下的断点仍能正常命中。
8. release 构建行为不受影响，不额外产出 `.fd`。

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
- **用带 `PKG_NAME://<package-relative path>` 逻辑源码 URI 的 `#line` + 主机编译器原生调试信息解决断点 / step / stack line**；
- **用最小独立二进制 `.fd` sidecar 解决 frame 名称重写、变量名称映射与少量特殊 carrier 读取提示**；
- **让最终 binary 对应的单个 `.fd` 覆盖本地路径依赖调试闭包，从而支持 monorepo 场景跨项目单步进入**；
- **用 `feng dap` 统一代理 `lldb-dap`，把 editor 看到的调试对象重写为 Feng 语义体验**；
- **让 VS Code 等编辑器只负责 launch / task / UI，而不承载核心映射逻辑**。

这条路径既能保持当前编译器与 `.ft` 边界稳定，又能在不引入第二套执行模型的前提下，把核心调试适配收敛到 `feng dap`，同时为后续其他编辑器接入保留统一复用层。
