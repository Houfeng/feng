# Feng 语言符号表规范

本文档用于补充 [feng-package.md](./feng-package.md) 与 [feng-build.md](./feng-build.md) 中与 `.ft` 相关的说明,聚焦 `.ft` 符号表、workspace cache profile 以及二进制布局。

规则边界如下:

- `.fb` 包结构、`feng.fm` 字段语义以及编译器可从 `.fb` 读取哪些元信息,仍以 [feng-package.md](./feng-package.md) 为准。
- 编译器与构建工具的职责划分、参数协议与构建路径,仍以 [feng-build.md](./feng-build.md) 为准。
- 本文档只定义 `.ft` 的用途、产物分层和二进制格式。

当前仓库中仍出现的 `.fi` 仅视为旧命名; 本规范定义统一迁移到 `.ft`。

## 1 结论

Feng 在跨模块边界上采用“**编译产出符号表,消费侧走查询**”的方向,不再把公开接口视为需要重新解析的文本。

核心判断如下:

- 跨模块边界后,编译器与语言服务真正需要的是“已完成语义收敛的公开事实”,不是再次解析实现源码。
- 对依赖包,`use` 的核心路径应当是“模块名 -> `.ft` 路径 -> 符号表查询”,也就是“**把类型解析变成类型查询**”。
- 对当前工程本身,编译成功后的符号信息也应缓存在 `build/` 目录,供 IDE/LSP 直接复用,减少重复解析与重复语义分析成本。
- `pack` 不应再单独重做一次接口提取; 应直接复用 `build/` 下已生成的公开符号表并写入 `.fb`。
- 是否携带源码位置、依赖指纹等本地缓存信息,不应再体现在扩展名上; 这些差异应由目录位置与文件头 `profile` 区分。

因此,`.ft` 采用“两层产物、单一扩展名”:

1. **公开包表 `.ft`**: 面向 `.fb` 分发与编译器跨包消费,只包含公开且可导出的接口事实。
2. **本地缓存 `.ft`**: 面向当前项目的 IDE/LSP 与增量构建,在同一格式上保留额外的私有声明、源码位置和失效校验信息。

## 2 目标与非目标

### 2.1 目标

- 让 `.ft` 成为 Feng 包与本地缓存统一使用的标准符号表扩展名。
- 让包兼容性由 `.ft` 格式版本与 ABI 兼容契约决定,而不是由生产者编译器版本号直接决定。
- 让 `build`、`check`、`pack` 在一次语义分析后同时产出可复用的符号信息。
- 让编译器消费依赖包时走“查表”路径,不再重解析接口源码。
- 让语言服务优先消费 `build/` 目录中的本地缓存,提升 hover、completion、definition 等类型感知速度。
- 让 `fit`、`spec`、`@bounded`、文档注释等影响语义与 IDE 展示的事实都能进入符号表。
- 让 C 实现保持简单: 固定小端编码、固定宽度整数、分节布局、字符串池去重,避免引入过重序列化依赖。

### 2.2 非目标

- 本阶段不把函数体、语句树、表达式树或运行时值写入符号表。
- 本阶段不把局部变量、临时值、控制流图放入 `.ft`; 如后续需要语句级缓存,应另行设计,不把接口表做成“大而全 IR”。
- 本阶段不让核心分析器感知 `.ft` 的导出、写入、section 常量、路径布局或二进制编码细节; 这些属于符号表模块与外层编排职责。
- 本阶段不把“外部类型信息来自 `.ft`”硬编码进核心分析器; 分析器只依赖抽象查询接口,由外层把源码结果、本地缓存或 `.ft` 读取结果适配后注入。
- 本阶段不承诺“任意未来新增公开语义”都能在不增加 kind 或扩展属性的情况下表达。
- 本阶段不为公开包表和本地缓存再拆成两个扩展名; 二者统一使用 `.ft`。

## 3 产物分层与目录布局

### 3.1 公开接口产物

编译器在语义分析成功后,按模块名输出公开接口符号表:

```text
build/
  mod/
    mylib/
      api.ft
      model.ft
```

规则:

- 一个公开模块恰好对应一个 `.ft` 文件。
- 路径仍由模块名唯一决定: `mylib.api` -> `build/mod/mylib/api.ft`。
- `pack` 时直接把 `build/mod/**/*.ft` 复制进 `.fb` 的 `mod/` 目录,不重新建模、不重新序列化。

### 3.2 本地缓存产物

为加快当前工程的 IDE/LSP 与增量编译,编译器额外输出本地缓存:

```text
build/
  obj/
    symbols/
      mylib/
        api.ft
        model.ft
```

规则:

- 本地缓存仍使用 `.ft`,不再单独引入其他缓存扩展名。
- 公开包表与本地缓存表共享同一套核心节格式,但 `profile` 不同,允许包含额外节。
- 本地缓存 `.ft` 可以保留当前包内不可导出的声明、源码位置、失效校验指纹等本地信息。
- `.ft` 的用途由“目录位置 + Header.profile”共同决定,而不是由扩展名决定。

### 3.3 消费优先级

- **编译器消费依赖包**: 读取 `.fb/mod/**/*.ft`。
- **语言服务消费当前项目**: 优先读取 `build/obj/symbols/**/*.ft`。
- **语言服务消费外部依赖**: 读取 `.fb/mod/**/*.ft`。
- **当前文件存在未保存修改**: 以内存中的当前文档 AST/语义结果覆盖磁盘 cache,但其他未改动模块仍尽量复用磁盘 `.ft`。

## 4 生成与消费流程

### 4.0 分层边界

`.ft` 相关实现必须遵守以下分层规则:

- **核心分析器** 只负责消费 AST、当前工程语义上下文与“已导入模块查询接口”,并产出语义分析结果; 不得直接感知 `.ft` writer、reader、Header、Section、字符串池、压缩包 entry 路径等文件格式细节。
- **符号表导出模块** 只消费核心分析器已经产出的语义结果或由其收敛出的模块符号图; 不得反向把“如何导出 `.ft`”的决策塞回核心分析器。
- **符号表读取模块** 负责把 `.ft` 解析成统一的“已导入模块查询视图”; 核心分析器只能通过抽象查询接口访问这些结果,不得直接依赖 `.ft` reader API。
- **编译驱动 / 构建层** 负责在语义分析成功后触发导出,并在分析前把源码模块、workspace cache 或 `.ft` 读取结果适配为统一查询接口后注入分析器。
- **`pack`** 只复用已生成的公开 `.ft`; 不得在打包阶段临时重做一套接口抽取或让核心分析器为打包感知 `.ft` 导出逻辑。

### 4.1 生成流程

建议在现有前端/语义分析后,由**外层编排**增加统一的“模块符号图”构建步骤; 核心分析器本身只返回语义结果,不负责决定 `.ft` 的导出时机与写入格式:

1. 解析 `.ff` 源文件,完成语义分析。
2. 编译驱动把语义结果交给符号表模块,以“模块”为单位收敛可导出声明与本地声明,生成内存中的模块符号图。
3. 符号表模块对每个公开模块输出 `build/mod/<module>.ft`。
4. 符号表模块对当前项目内模块输出 `build/obj/symbols/<module>.ft`。
5. `pack` 直接复用 `build/mod/**/*.ft` 与库文件生成 `.fb`。

### 4.2 编译器消费流程

对 `--pkg <xxx.fb>`:

1. 先只扫描 `mod/` entry 路径,建立“模块名 -> entry path”的索引。
2. 遇到 `use mylib.api;` 时,直接定位 `mod/mylib/api.ft`。
3. 由 `.ft` 读取模块把该文件解析为统一的“已导入模块查询视图”,其中包含公开 `type`、`spec`、`fit`、顶层 `fn`、模块级 `let` / `var`、公开成员等声明级事实。
4. 编译驱动把这些查询视图通过抽象查询接口注入核心分析器。
5. 后续类型检查、名称查找、契约关系判断、`@bounded` 重复绑定检查都基于该抽象查询接口进行,不重解析文本接口,也不让核心分析器直接依赖 `.ft` 模块。

### 4.3 IDE/LSP 消费流程

1. 打开当前项目时,扫描 `build/obj/symbols/**/*.ft`。
2. 若本地缓存 `.ft` 指纹有效,直接读取声明、签名、文档注释、源码位置用于 hover / completion / definition。
3. 若本地缓存 `.ft` 缺失或失效,退回源码分析,并在下一次成功 `check` / `build` 后重新生成缓存。
4. 对外部依赖,语言服务与编译器可共用 `.ft` 读取器,但对上层都只暴露统一查询接口或查询视图,不把 `.ft` 文件格式细节扩散到核心分析逻辑。

## 5 符号表内容范围

### 5.1 公开包表 `.ft` 必须包含的事实

公开包表 `.ft` 至少需要覆盖以下公开语义事实:

- 模块信息: 模块名、可见性、模块级文档注释。
- 公开 `type`。
- 公开 `spec`。
- 公开 `fit` 及其建立的契约关系。
- 公开顶层 `fn`。
- 公开模块级 `let` / `var`。
- 公开成员字段与成员方法。
- 公开构造函数与终结器函数。
- 公开 `extern fn` 与 `@fixed` 声明所需的 ABI 元信息。
- 公开 `let` 成员的 `@bounded` 声明事实,以及构造函数 `@bounded(...)` 绑定关系。
- 公开声明的文档注释。

公开包表 `.ft` 明确不包含:

- 函数体、语句、表达式。
- 绑定初值、常量值、运行时数据。
- 私有声明。
- 源码绝对路径、源码行列号等闭源分发不应泄露的信息。

### 5.2 本地缓存 `.ft` 相比公开 `.ft` 的增量信息

本地缓存 `.ft` 在公开 `.ft` 基础上可额外包含:

- 当前包内的私有模块级声明与私有成员。
- 符号到源码文件/行列的映射。
- 当前模块的源文件指纹、依赖指纹、编译器构建指纹。
- 仅用于 IDE 的补充查询信息。

本规范不建议把局部变量与表达式级类型结果写入本地缓存 `.ft`; 先把跨文件、跨模块的声明级语义缓存立住,避免缓存格式膨胀过快。

## 6 二进制容器格式

### 6.1 总体布局

`.ft` 的底层二进制容器格式暂称 **FST1**（Feng Symbol Table v1）。

基本规则:

- 全部整数使用 **little-endian**。
- 使用固定宽度整数 (`u8`、`u16`、`u32`、`u64`),便于 C 端直接解码。
- 文件由 **Header + Section Directory + Sections** 组成。
- Header 固定 64 字节,Section Directory 单项固定 32 字节。
- 各 section 的 `offset` 必须 8 字节对齐。
- 所有名字、文档、文件路径统一放入字符串池,其他记录只保留字符串 ID。
- `str_id = 0` 与 `symbol_id = 0` 都保留为“空值/不存在”,真实记录从 `1` 开始编号。

#### 6.1.1 文件总体结构图

下面的文本图表示一个 `.ft` 文件在磁盘上的实际组织方式:

```text
+--------------------------------------------------------------+
| .ft file                                                     |
+--------------------------------------------------------------+
| Header                                                       |
|  - fixed 64 bytes                                            |
|  - magic / version / profile / flags                         |
|  - root_symbol_id                                            |
|  - section_count / payload_offset                            |
|  - content_fingerprint / dependency_fingerprint              |
+--------------------------------------------------------------+
| Section Directory                                            |
|  - section_count entries                                     |
|  - each entry fixed 32 bytes                                 |
|  - kind / flags / count / offset / size / entry_size         |
+--------------------------------------------------------------+
| Section Payloads (8-byte aligned)                            |
|  +--------------------------------------------------------+  |
|  | STRS  | string pool                                    |  |
|  | SYMS  | symbol records                                 |  |
|  | TYPS  | type nodes                                     |  |
|  | SIGS  | callable signatures                            |  |
|  | PRMS  | parameters                                     |  |
|  | RELS  | semantic relations                             |  |
|  | DOCS? | doc map                                         |  |
|  | ATRS? | extensible attributes                           |  |
|  | SPNS? | source spans          (workspace-cache only)    |  |
|  | USES? | dependency index     (workspace-cache only)     |  |
|  | META? | cache validation info (workspace-cache only)    |  |
|  +--------------------------------------------------------+  |
+--------------------------------------------------------------+
```

若是 package-public profile,通常只会出现 core section 与少量可选 section:

```text
.ft (package-public)
|- Header(profile = FT_PROFILE_PACKAGE_PUBLIC)
|- Section Directory
`- Payloads
  |- STRS
  |- SYMS
  |- TYPS
  |- SIGS
  |- PRMS
  |- RELS
  |- DOCS? 
  `- ATRS?
```

若是 workspace-cache profile,则在上述基础上追加本地缓存专用 section:

```text
.ft (workspace-cache)
|- Header(profile = FT_PROFILE_WORKSPACE_CACHE)
|- Section Directory
`- Payloads
  |- STRS
  |- SYMS
  |- TYPS
  |- SIGS
  |- PRMS
  |- RELS
  |- DOCS?
  |- ATRS?
  |- SPNS?
  |- USES?
  `- META?
```

### 6.2 Header

#### 6.2.1 Header 常量

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `FT_MAGIC_BYTES` | `46 53 54 31` | ASCII `FST1` |
| `FT_BYTE_ORDER_LE` | `0x01` | v1 仅支持 little-endian |
| `FT_VERSION_MAJOR` | `0x01` | 主版本 |
| `FT_VERSION_MINOR` | `0x00` | 次版本 |
| `FT_PROFILE_PACKAGE_PUBLIC` | `0x01` | `.fb/mod/**/*.ft` 公开包表 |
| `FT_PROFILE_WORKSPACE_CACHE` | `0x02` | `build/obj/symbols/**/*.ft` 本地缓存 |
| `FT_HEADER_V1_SIZE` | `64` | Header 固定大小 |
| `FT_SECTION_DIR_V1_ENTRY_SIZE` | `32` | Section Directory 单项固定大小 |
| `FT_FLAG_HAS_DOCS` | `0x00000001` | 文件包含 `DOCS` 节 |
| `FT_FLAG_HAS_SPANS` | `0x00000002` | 文件包含 `SPNS` 节 |
| `FT_FLAG_HAS_USES` | `0x00000004` | 文件包含 `USES` 节 |
| `FT_FLAG_HAS_META` | `0x00000008` | 文件包含 `META` 节 |
| `FT_FLAG_HAS_ATTRS` | `0x00000010` | 文件包含 `ATRS` 节 |

#### 6.2.2 Header 固定布局

FST1 的 Header 固定为 64 字节,字段布局如下:

| 偏移 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- |
| `0x00` | `magic` | `u8[4]` | 固定为 `FT_MAGIC_BYTES` |
| `0x04` | `byte_order` | `u8` | 固定为 `FT_BYTE_ORDER_LE` |
| `0x05` | `major` | `u8` | 固定为 `FT_VERSION_MAJOR` |
| `0x06` | `minor` | `u8` | 固定为 `FT_VERSION_MINOR` |
| `0x07` | `profile` | `u8` | 取值见 profile 常量 |
| `0x08` | `header_size` | `u16` | 固定为 `FT_HEADER_V1_SIZE` |
| `0x0A` | `section_entry_size` | `u16` | 固定为 `FT_SECTION_DIR_V1_ENTRY_SIZE` |
| `0x0C` | `section_count` | `u16` | Section Directory 项数 |
| `0x0E` | `reserved0` | `u16` | 固定写 `0` |
| `0x10` | `flags` | `u32` | 文件级标志位 |
| `0x14` | `root_symbol_id` | `u32` | 根模块符号 ID |
| `0x18` | `section_dir_offset` | `u64` | v1 固定为 `64` |
| `0x20` | `payload_offset` | `u64` | 第一个 section payload 的偏移,必须等于 `64 + section_count * 32` |
| `0x28` | `content_fingerprint` | `u64` | 对 `[payload_offset, EOF)` 做 FNV-1a 64 计算 |
| `0x30` | `dependency_fingerprint` | `u64` | package-public profile 固定为 `0`; workspace-cache profile 为已排序依赖指纹集合的 FNV-1a 64 |
| `0x38` | `reserved1` | `u64` | 固定写 `0` |

说明:

- 读取器若发现 `magic`、`byte_order`、`header_size`、`section_entry_size` 任一不匹配,应立即拒绝该文件。
- `section_dir_offset` 在 v1 固定为 64,实现中不再单独做可变头大小分支。
- `payload_offset` 让读取器可以一次性跳过 Header 与目录区,直接进入 section 数据。
- `content_fingerprint` 只依赖 payload 字节,不依赖 Header 中的运行时字段,更容易稳定复用。

#### 6.2.3 Header 文本结构图

Header 可按 8 字节分块理解为:

```text
offset  size  content
------  ----  -----------------------------------------------
0x00    8     magic[4] | byte_order | major | minor | profile
0x08    8     header_size(u16) | section_entry_size(u16)
          section_count(u16) | reserved0(u16)
0x10    8     flags(u32) | root_symbol_id(u32)
0x18    8     section_dir_offset(u64)
0x20    8     payload_offset(u64)
0x28    8     content_fingerprint(u64)
0x30    8     dependency_fingerprint(u64)
0x38    8     reserved1(u64)
```

也可以把它理解成:

```text
Header
|- identity
|  |- magic
|  |- byte_order
|  |- version
|  `- profile
|- shape
|  |- header_size
|  |- section_entry_size
|  `- section_count
|- semantic root
|  |- flags
|  `- root_symbol_id
`- file navigation
  |- section_dir_offset
  |- payload_offset
  |- content_fingerprint
  `- dependency_fingerprint
```

### 6.3 Section Directory

#### 6.3.1 section kind 常量

| 常量 | 值 | 是否必需 | 适用 profile | 用途 |
| --- | --- | --- | --- | --- |
| `FT_SEC_STRS` | `0x0001` | 必需 | 全部 | 字符串池 |
| `FT_SEC_SYMS` | `0x0002` | 必需 | 全部 | 符号记录 |
| `FT_SEC_TYPS` | `0x0003` | 必需 | 全部 | 类型节点 |
| `FT_SEC_SIGS` | `0x0004` | 必需 | 全部 | 可调用签名 |
| `FT_SEC_PRMS` | `0x0005` | 必需 | 全部 | 参数记录 |
| `FT_SEC_RELS` | `0x0006` | 必需 | 全部 | 关系记录 |
| `FT_SEC_DOCS` | `0x0007` | 可选 | 全部 | 文档注释 |
| `FT_SEC_ATTRS` | `0x0008` | 可选 | 全部 | 扩展属性 |
| `FT_SEC_SPNS` | `0x0010` | 可选 | workspace-cache | 源码位置 |
| `FT_SEC_USES` | `0x0011` | 可选 | workspace-cache | 依赖模块与指纹 |
| `FT_SEC_META` | `0x0012` | 可选 | workspace-cache | 缓存失效信息 |

保留规则:

- `0x0009` 至 `0x000F` 预留给未来核心节。
- `0x0013` 至 `0x001F` 预留给未来 workspace-cache 专用节。
- package-public profile 不得出现 `0x0010` 以上的 workspace-only 节。

#### 6.3.2 section flag 常量

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `FT_SEC_FLAG_REQUIRED` | `0x0001` | 读取器遇到缺失该节必须报错 |
| `FT_SEC_FLAG_FIXED_ENTRY` | `0x0002` | `count * entry_size == size` |
| `FT_SEC_FLAG_SORTED` | `0x0004` | 记录按主键升序排序 |
| `FT_SEC_FLAG_WORKSPACE_ONLY` | `0x0008` | 只允许出现在 workspace-cache profile |
| `FT_SEC_FLAG_IGNORABLE` | `0x0010` | 对未知 section 可安全跳过 |

#### 6.3.3 目录项固定布局

Section Directory 单项固定为 32 字节,字段布局如下:

| 偏移 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- |
| `0x00` | `kind` | `u16` | section kind 常量 |
| `0x02` | `flags` | `u16` | section flag 位图 |
| `0x04` | `count` | `u32` | 记录数 |
| `0x08` | `offset` | `u64` | payload 中该节起始偏移 |
| `0x10` | `size` | `u64` | 该节总字节数 |
| `0x18` | `entry_size` | `u32` | 固定记录节填实际大小,变长节填 `0` |
| `0x1C` | `reserved` | `u32` | 固定写 `0` |

目录约束:

- Section Directory 必须按 `kind` 升序排列。
- 任意两个 section 的 `[offset, offset + size)` 区间不得重叠。
- `offset` 必须满足 8 字节对齐。
- `STRS` 与 `META` 可使用变长布局,其 `entry_size` 为 `0`; 其他 v1 已定义节默认使用固定记录布局。

#### 6.3.4 Section Directory 文本结构图

一个目录项的实际形状如下:

```text
SectionDirEntry (32 bytes)

+0x00  kind        : u16
+0x02  flags       : u16
+0x04  count       : u32
+0x08  offset      : u64
+0x10  size        : u64
+0x18  entry_size  : u32
+0x1C  reserved    : u32
```

整个目录区可以理解成一个“payload 索引表”:

```text
Section Directory
|- entry(kind = STRS,  offset = ..., size = ...)
|- entry(kind = SYMS,  offset = ..., size = ...)
|- entry(kind = TYPS,  offset = ..., size = ...)
|- entry(kind = SIGS,  offset = ..., size = ...)
|- entry(kind = PRMS,  offset = ..., size = ...)
|- entry(kind = RELS,  offset = ..., size = ...)
|- entry(kind = DOCS,  offset = ..., size = ...) ?
|- entry(kind = ATRS,  offset = ..., size = ...) ?
|- entry(kind = SPNS,  offset = ..., size = ...) ?
|- entry(kind = USES,  offset = ..., size = ...) ?
`- entry(kind = META,  offset = ..., size = ...) ?
```

读取器的实际读取顺序通常也是:

```text
read header
  -> validate version/profile
  -> read section directory
  -> locate STRS/SYMS/TYPS/SIGS/PRMS/RELS
  -> load optional DOCS/ATRS as needed
  -> if workspace-cache profile, load SPNS/USES/META
```

#### 6.3.4 兼容性规则

`.ft` 的兼容性判断必须基于**格式版本与 ABI 兼容契约**,而不是基于“生产者编译器版本号是否完全相同”。

规则如下:

- 编译器不得仅因包由不同版本的 Feng 编译器生成而拒绝使用。
- `FT_VERSION_MAJOR` 相同表示 core 外壳兼容: Header 布局、Section Directory 布局以及 core required section 的固定记录外形保持稳定。
- 在相同 `FT_VERSION_MAJOR` 内,`FT_VERSION_MINOR` 只允许做追加式演进: 新增可选 section、新增 attr key、新增 flag bit、新增 append-only kind 常量; 不得改写既有 required section 的固定记录布局。
- 若新增语义会让旧 consumer 在“忽略后仍可能编译错误或链接错误”,则不得作为同 major 的 silently-optional 扩展发出; 此类变化必须提升 major,或通过新的 required section 让旧 consumer 明确拒绝。
- 新er 编译器必须能够读取并消费旧的同 major `.ft` 包; 旧编译器是否能读取较新的同 major `.ft`,取决于该包是否只使用了其可安全忽略的追加扩展。
- 是否可链接、是否可运行,除 `.ft` 格式外还取决于对应平台库文件、运行时 ABI 和 `@fixed` / bridge 规则是否兼容; 这些不由编译器版本号单独决定。

#### 6.3.5 `ATRS` 扩展属性节

为尽量避免“出现一个新注解或新修饰就改 core 记录布局”,v1 预留 `FT_SEC_ATTRS` 作为统一扩展槽。

`ATRS` 的用途:

- 承载新语法对应的声明级附加语义,例如未来新增的修饰符、ABI 扩展元信息或额外约束。
- 承载当前已存在但不适合硬塞进 `SYMS` 固定字段的元信息,例如 `@union`、调用约定、外部库来源。
- 让 future feature 尽量通过“追加 attr key”演进,而不是频繁改动 Header 或 core section 布局。

`ATRS` 的记录策略如下:

- `ATRS` 为可选 section。
- 未识别的 attr key,只有在其所在 section 标记 `FT_SEC_FLAG_IGNORABLE` 时,consumer 才可安全跳过。
- 若某个新增语义对正确编译是强制性的,则不得仅以“可忽略 attr”形式发布给旧 consumer。

### 6.4 `STRS` 字符串池

字符串池统一保存:

- 模块名
- 符号名
- 文档注释文本
- 文件路径（仅 workspace-cache profile）
- ABI 库名/调用约定字符串

为便于实现,v1 建议 `STRS` 的 payload 布局固定为:

- 先写入 `count + 1` 个 `u32` 偏移表。
- 再写入连续 UTF-8 字节区。
- `str_id = 1` 对应偏移表第 `0` 项到第 `1` 项之间的字节区。
- `str_id = 0` 永远保留为“空字符串/空引用”,不占用 payload 条目。

建议字符串池做去重,并保持 UTF-8 原文。

### 6.5 `SYMS` 符号记录

每条符号记录建议采用固定结构:

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `u32` | 符号 ID |
| `owner_id` | `u32` | 所属符号; 顶层归根模块 |
| `name_str` | `u32` | 名称字符串 ID |
| `kind` | `u16` | 符号类型 |
| `flags` | `u16` | 符号标志 |
| `type_ref` | `u32` | 绑定/字段的类型 ID; 非类型类符号为 `0` |
| `sig_ref` | `u32` | 函数/方法/构造等签名 ID; 非 callable 为 `0` |
| `extra_ref` | `u32` | kind 专用辅助引用 |
| `doc_ref` | `u32` | 文档记录 ID,无则为 `0` |

建议支持的 `kind`:

- `module`
- `type`
- `spec`
- `fit`
- `top_fn`
- `extern_fn`
- `ctor`
- `dtor`
- `field`
- `method`
- `top_let`
- `top_var`

建议支持的 `flags`:

- `public`
- `mutable`
- `fixed`
- `extern`
- `bounded_decl`
- `has_doc`

说明:

- `owner_id` 负责表达层级关系,例如字段/方法归属于某个 `type` 或某个 `fit`。
- `fit` 作为独立符号存在,便于记录“由哪个 `fit` 建立了哪些契约关系与扩展方法”。
- 被语义分析判定为“不得导出”的声明,不进入公开 `.ft`; 本地缓存 `.ft` 可按本地需要保留。

#### 6.5.1 core section 引用关系图

从“谁引用谁”的角度看,core section 大致是这个结构:

```text
STRS
 ^   ^        ^         ^
 |   |        |         |
 |   |        |         +------------------ DOCS.doc_str_id
 |   |        +---------------------------- PRMS.name_str
 |   +------------------------------------- SYMS.name_str
 +----------------------------------------- module names / ABI strings / paths

SYMS
 |- owner_id  ------> SYMS.id
 |- type_ref  ------> TYPS.id
 |- sig_ref   ------> SIGS.id
 `- doc_ref   ------> DOCS.id

SIGS
 |- return_type_id --> TYPS.id
 `- params ---------> PRMS range

RELS
 `- left/right/owner -> SYMS.id
```

也就是说,实际消费时通常先读 `STRS`,再读 `SYMS`,然后按需解开 `TYPS`、`SIGS`、`PRMS`、`RELS` 与 `DOCS`。

### 6.6 `TYPS` 类型节点

`TYPS` 用于表达已解析后的类型结构,避免在消费端再次解析类型文本。

建议第一版至少支持:

- `builtin`
- `named`
- `array`
- `callable-spec`
- `c-pointer`

类型节点建议以 DAG 形式表达,供多个符号和签名共享引用。复杂类型一律通过子节点 ID 组合,不在字符串里重新编码一份“类型文本”。

当前 Feng 已有但必须被 `TYPS` 覆盖的类型特征包括:

- `string` 作为 builtin 类型节点处理。
- `*T` 作为 `c-pointer` 类型节点处理。
- 数组的层数与逐层可写性必须进入类型节点,不能只把 `T[]![]` 抹平成一个字符串。

因此,`array` 类型节点至少应编码:

- 元素类型 ID。
- 数组层数 `rank`。
- 逐层可写位图 `mutability_bitmap`,用于覆盖 `T[]`、`T[]!`、`T[][]!`、`T[]![]`、`T[]![]!` 等现有语义。

未来若出现新的不可约类型构造,优先追加新的 `FT_TYPE_*` kind 常量,而不是改动既有 `TYPS` 记录壳。

### 6.7 `SIGS` 与 `PRMS`

`SIGS` 表达 callable 的签名轮廓,`PRMS` 表达参数列表。

`SIGS` 最少应包含:

- 返回类型 ID
- 参数起始偏移
- 参数数量
- 调用约定/ABI 辅助字段

`PRMS` 最少应包含:

- 参数名字符串 ID
- 参数类型 ID
- 参数标志位（`let` / `var` 等）

方法的 `self` 不作为显式参数写入 `PRMS`; 它由 `owner_id` 与符号 `kind` 隐式表达。

### 6.8 `RELS` 关系记录

`RELS` 负责表达“不是单个符号属性、而是两个符号之间”的语义关系。

建议第一版至少覆盖:

- `type_implements_spec`: `type A: B`
- `fit_implements_spec`: `fit A: B`
- `fit_extends_type`: `fit A { ... }` 或 `fit A: B { ... }` 对目标类型 `A` 的扩展归属
- `ctor_binds_member`: 构造函数 `@bounded(foo, bar)` 绑定了哪些公开 `let` 成员

无参数 `@bounded` 的成员绑定状态由 `SYMS.flags.bounded_decl` 表达,无需额外 relation。

### 6.9 `DOCS` 文档注释

`DOCS` 保存“符号 ID -> 文档字符串 ID”的映射。

文档注释采集规则保持一致:

- **紧邻声明前的连续注释块即为文档注释**。
- 注释块与声明之间若出现空行或其他声明,则不再绑定。
- 注释块位于注解（如 `@fixed`、`pu`）之前时,仍绑定到该声明。

建议在写入 `DOCS` 前做统一归一化:

- 去掉注释前缀。
- 保留注释块内的相对换行。
- 统一为 UTF-8。

### 6.10 `SPNS` / `USES` / `META`（仅 workspace-cache profile）

这些节只属于本地缓存,不进入分发接口:

- `SPNS`: `symbol_id -> 文件路径 + 起止行列`,供 definition / peek / outline 使用。
- `USES`: 当前模块依赖了哪些模块,以及对应的依赖指纹。
- `META`: 当前模块源文件指纹、manifest 指纹、编译器构建指纹、生成时间等。

`META` 的目的不是对外兼容,而是让语言服务和构建工具快速判断 cache 是否可复用。

## 7 一个最小示例

示例源码:

```feng
pu mod mylib.api;

# 用户模型
pu type User: Named {
    pu var name: string;

    @bounded
    pu let id: int;

    pu fn get_info(): string;
}

pu fit User: Auditable {
    pu fn audit(): string;
}

pu fn add_user(u: User): bool;
```

对应的符号表最少应出现:

- `module(mylib.api)`
- `type(User)`
- `field(name)`
- `field(id)` with `bounded_decl`
- `method(get_info)`
- relation `type_implements_spec(User -> Named)`
- `fit(User: Auditable)`
- relation `fit_extends_type(fit#1 -> User)`
- relation `fit_implements_spec(fit#1 -> Auditable)`
- `method(audit)` owned by `fit#1`
- `top_fn(add_user)`
- `doc(User) = "用户模型"`

这意味着消费端不需要再从文本里推导“`User` 是否满足 `Named` / `Auditable`、`id` 是否已在声明阶段绑定、`audit` 是来自 `fit` 还是来自原始 `type`”。这些都已经固化为查询事实。

## 8 失效与复用策略

### 8.1 公开 `.ft`

- 公开 `.ft` 的生成以“模块公开语义变化”为失效条件。
- 若模块只改了私有实现且不影响公开接口,理论上可复用旧公开 `.ft`。
- `pack` 只接受与当前公开语义一致的公开 `.ft`; 若缺失或过期,应重新执行语义分析与导出。

### 8.2 本地缓存 `.ft`

- 本地缓存 `.ft` 以源文件指纹、依赖模块指纹、`feng.fm` 相关字段、编译器构建指纹共同决定是否失效。
- LSP 读到失效本地缓存 `.ft` 时直接忽略并回退到源码分析,而不是冒险继续使用旧缓存。
- `clean` 时统一删除 `build/mod/` 与 `build/obj/symbols/`。

## 9 推荐落地顺序

建议按以下顺序推进:

1. **先定内存模型**: 在前端/语义分析后构建统一“模块符号图”,不要直接在 writer 里拼字节。
2. **先落公开 `.ft`**: 打通 `build/mod/**/*.ft` 生成、读取与 `pack` 复用,先把跨包消费闭环立住。
3. **再在同一格式上补 workspace-cache profile**: 增加 `SPNS`、`USES`、`META`,供当前项目语言服务复用。
4. **再接编译器读取器**: 依赖包消费改走 `.ft` 二进制查询。
5. **最后接 LSP**: 当前项目优先读 `build/obj/symbols/**/*.ft`,外部依赖读 `.fb/mod/**/*.ft`。

## 10 需要评审确认的点

当前仍有以下待确认项:

1. Phase 3 的本地缓存 `.ft` 是否只缓存声明级符号,还是要把局部符号也一并纳入。当前建议是**先不纳入局部符号**。
2. 公开 `.ft` 是否在第一版就强制包含 `DOCS`; 当前建议是**应包含**,这样外部依赖包的 hover 才不需要额外侧车文件。
3. `ATRS` 第一版是先只覆盖 `@union`、调用约定与外部库来源,还是同时预留更多 attr key。当前建议是**先覆盖当前已存在且确定属于声明级的 ABI 元信息**。
4. 若实现阶段需要兼容读取旧 `.fi`,是否只作为临时 reader 兼容而不进入规范。当前建议是**即使短期兼容读取,仓库文档与新产物也只使用 `.ft`**。

## 11 统一迁移策略

`.ft` 生效后,应做一次**全仓统一替换**,不保留“部分文件写 `.fi`,部分文件写 `.ft`”的长期状态。

统一替换范围至少包括:

- `docs/` 下所有把 `.fi` 视为标准接口文件的规范文件。
- `dev/` 下所有仍以 `.fi` 描述 Phase 3 / Phase 4 任务的施工文档。
- `editors/feng-vscode/` 下的扩展名注册、README 与示例说明。
- `src/` 下的注释、诊断文本、路径常量与测试数据。
- 未来新增的 smoke / unit test 中涉及 `mod/` 路径与符号表文件名的断言。

迁移原则:

- 规范层面只保留 `.ft` 作为唯一标准扩展名。
- 目录语义保持不变: `mod/<module>.ft` 仍由模块名唯一定位。
- 公开包表与本地缓存都使用 `.ft`,通过目录位置与 `profile` 区分。
- 若实现层需要短暂兼容旧 `.fi`,也应限制在 reader 兼容层,不再继续写出 `.fi`。

## 12 对 Phase 3 的影响

若按本规范推进,Phase 3 中 `.ft` 相关任务的边界将变为:

- `build/check/pack` 负责产出并复用符号表。
- `.fb` 中 `mod/` 放的是**二进制公开符号表 `.ft`**,不再是假想的文本接口源码。
- 编译器消费外部包时,核心路径从“解析接口文本”改成“按模块名查 `.ft` 并查询符号”。
- 本地工程的 IDE 类型感知优先复用 `build/obj/symbols/**/*.ft`,减少重复工作。

这条路径与主流编译型语言的做法一致,也更适合后续把跨模块语义分析稳定收敛为查询模型。

## 13 与相关规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范与文件扩展名总览。
- [feng-package.md](./feng-package.md): `.fb` 包结构、`feng.fm` 字段语义以及编译器可从 `.fb` 读取哪些元信息。
- [feng-build.md](./feng-build.md): 编译器与构建工具的职责划分、`.fb` 消费路径与构建协议。
- [feng-module.md](./feng-module.md): 模块名、`use`、公开导出与模块级初始化规则。
- [feng-fit.md](./feng-fit.md): `fit` 的语义边界与公开导出规则。
- [feng-function.md](./feng-function.md): 顶层 `fn`、成员方法、构造函数与终结器的语义规则。
