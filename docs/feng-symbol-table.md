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
- 对依赖包,`import` 的核心路径应当是“模块名 -> `.ft` 路径 -> 符号表查询”,也就是“**把类型解析变成类型查询**”。
- 对当前工程本身,编译成功后的符号信息也应缓存在 `build/` 目录,供 IDE/LSP 直接复用,减少重复解析与重复语义分析成本。
- `pack` 不应再单独重做一次接口提取; 应直接复用 `build/` 下已生成的公开符号表并写入 `.fb`。
- 是否携带源码位置、依赖指纹等本地缓存信息,不应再体现在扩展名上; 这些差异应由目录位置与文件头 `profile` 区分。

因此,`.ft` 采用“两层产物、单一扩展名”:

1. **公开包表 `.ft`**: 面向 `.fb` 分发与编译器跨包消费,包含公开接口事实及编译这些接口所需的私有表示依赖。
2. **本地缓存 `.ft`**: 面向当前项目的 IDE/LSP 与增量构建,在同一格式上保留额外的私有声明、源码位置和失效校验信息。

## 2 目标与非目标

### 2.1 目标

- 让 `.ft` 成为 Feng 包与本地缓存统一使用的标准符号表扩展名。
- 让包兼容性由 `.ft` 格式版本与 ABI 兼容契约决定,而不是由生产者编译器版本号直接决定。
- 让 `build`、`check`、`pack` 在一次语义分析后同时产出可复用的符号信息。
- 让编译器消费依赖包时走“查表”路径,不再重解析接口源码。
- 让语言服务优先消费 `build/` 目录中的本地缓存,提升 hover、completion、definition 等类型感知速度。
- 让 `fit`、`spec`、type 实例成员绑定推断事实、文档注释等影响语义与 IDE 展示的事实都能进入符号表。
- 让 C 实现保持简单: 固定小端编码、固定宽度整数、分节布局、字符串池去重,避免引入过重序列化依赖。

### 2.2 非目标

- 本阶段不把函数体、语句树、表达式树或运行时值写入符号表。
- 本阶段不把局部变量、临时值、控制流图放入 `.ft`; 如后续需要语句级缓存,应另行设计,不把接口表做成“大而全 IR”。
- 本阶段不把 DAP 所需的局部变量映射、frame 重写、watch 提示或 artifact-scoped 调试元数据写入 `.ft`; 这类信息统一由独立 `.fd` sidecar 承担。
- 本阶段不让核心分析器感知 `.ft` 的导出、写入、section 常量、路径布局或二进制编码细节; 这些属于符号表模块与外层编排职责。
- 本阶段不把“外部类型信息来自 `.ft`”硬编码进核心分析器; 分析器只依赖抽象查询接口,由外层把源码结果、本地缓存或 `.ft` 读取结果适配后注入。
- 本阶段不承诺“任意未来新增公开语义”都能在不增加 kind 或扩展属性的情况下表达。
- 本阶段不为公开包表和本地缓存再拆成两个扩展名; 二者统一使用 `.ft`。

## 3 产物分层与目录布局

### 3.1 公开接口产物

编译器在语义分析成功后,按模块名输出公开接口符号表:

```text
build/
  <platform>/
    mod/
      mylib/
        api.ft
        model.ft
```

规则:

- 在一个目标平台的开发构建根内,一个公开模块恰好对应一个 `.ft` 文件；`.fb` 内仍只保留多平台校验后的一份。
- 项目级 `feng build` 将 `build/<platform>` 作为该平台直编的 `--out`,因此开发态路径为 `build/<platform>/mod/mylib/api.ft`；核心直编只按自身收到的 `<out>/mod/mylib/api.ft` 输出,不自行拼接平台目录。
- `pack` 校验各目标平台 `build/<platform>/mod/**/*.ft` 的模块集合与公开语义事实等价后,从其中提取一套写入 `.fb` 的 `mod/` 目录,不重新建模、不重新序列化。

### 3.2 本地缓存产物

为加快当前工程的 IDE/LSP 与增量编译,编译器额外输出本地缓存:

```text
build/
  <platform>/
    obj/
      symbols/
        mylib/
          api.ft
          model.ft
```

规则:

- 本地缓存仍使用 `.ft`,不再单独引入其他缓存扩展名。
- 公开包表与本地缓存表共享同一套核心节格式,但 `profile` 不同,允许包含额外节。
- 发布到 `.fb` 的公开包表候选（`build/<platform>/mod/**/*.ft`）不得包含 span、源码位置或其他 workspace-only 节; 这些信息只允许出现在同平台的 `build/<platform>/obj/symbols/**/*.ft`。
- 本地缓存 `.ft` 可以保留当前包内不可导出的声明、源码位置、失效校验指纹等本地信息。
- `.ft` 的用途由“目录位置 + Header.profile”共同决定,而不是由扩展名决定。

补充边界:

- `.ft` 继续只承担模块级声明缓存与 workspace cache 职责; 与最终 binary 绑定的调试信息必须落入独立 `.fd` sidecar,而不是塞进 `build/<platform>/mod/**/*.ft` 或 `build/<platform>/obj/symbols/**/*.ft`。
- `.fd` 即使复用与 `.ft` 类似的二进制容器思路,也不属于本规范定义的 `.ft` profile/section 集合; `pack`、provider、LSP 与跨包编译都不得把 `.fd` 当成 `.ft` 的一种 profile 来消费。

### 3.3 消费优先级

- **编译器消费依赖包**: 读取 `.fb/mod/**/*.ft`。
- **语言服务消费当前项目**: 当前 `feng lsp` 未接受目标平台参数,优先读取 `build/<归一化 host 平台>/obj/symbols/**/*.ft`。
- **语言服务消费外部依赖**: 读取 `.fb/mod/**/*.ft`。
- **当前文件存在未保存修改**: 以内存中的当前文档 AST/语义结果覆盖磁盘 cache,但其他未改动模块仍尽量复用磁盘 `.ft`。
- **调试器消费当前产物**: 读取最终 binary 同级 `.fd`; 不从 `.ft` 推导局部变量、frame 或 watch 元数据。

## 4 生成与消费流程

### 4.0 分层边界

`.ft` 相关实现必须遵守以下分层规则:

- **核心分析器** 只负责消费 AST、当前工程语义上下文与“已导入模块查询接口”,并产出语义分析结果; 不得直接感知 `.ft` writer、reader、Header、Section、字符串池、压缩包 entry 路径等文件格式细节。
- **符号表导出模块** 只消费核心分析器已经产出的语义结果或由其收敛出的模块符号图; 不得反向把“如何导出 `.ft`”的决策塞回核心分析器。
- **符号表读取模块** 负责把 `.ft` 解析成统一的“已导入模块查询视图”; 核心分析器只能通过抽象查询接口访问这些结果,不得直接依赖 `.ft` reader API。
- **符号 Provider** 是外部包 source 的唯一注册入口; 编译驱动可以向 Provider 注册 `.fb` 包或已展开的 `.ft` 根目录,但 Provider 的内部索引、预加载或懒加载策略不得暴露给核心分析器。
- Provider 暴露给上层的模块查询视图必须是中立的声明事实视图,支持按名查询与枚举公开顶层声明; 不得把 `.ft` section、bundle entry、zip reader 或缓存策略作为查询接口的一部分。
- **`src/symbol/`** 建议作为符号表核心层,统一承载中立模块符号图、导出器、读取器与查询视图适配; CLI、`pack` 与语言服务只调用其公共入口,不内嵌实现。
- **编译驱动 / 构建层（例如 CLI）** 只负责在语义分析成功后触发导出,并在分析前把源码模块、workspace cache 或 `.ft` 读取结果适配为统一查询接口后注入分析器; 自身不承载分析实现或符号表核心逻辑。
- **`pack`** 只复用已生成的公开 `.ft`; 不得在打包阶段临时重做一套接口抽取或让核心分析器为打包感知 `.ft` 导出逻辑。

### 4.1 生成流程

建议在现有前端/语义分析后,由**外层编排**调用 `src/symbol/` 增加统一的“模块符号图”构建步骤; 核心分析器本身只返回语义结果,不负责决定 `.ft` 的导出时机与写入格式:

1. 解析 `.ff` 源文件,完成语义分析。
2. 编译驱动把语义结果交给 `src/symbol/`,以“模块”为单位收敛可导出声明与本地声明,生成内存中的模块符号图。
3. `src/symbol/` 对每个公开模块输出 `build/<platform>/mod/<module>.ft`。
4. `src/symbol/` 对当前项目内模块输出 `build/<platform>/obj/symbols/<module>.ft`。
5. `pack` 校验各目标平台公开 `.ft` 候选的模块集合与公开语义事实等价后,提取一套与分平台库文件生成 `.fb`。

### 4.2 编译器消费流程

对 `--pkg <xxx.fb>`:

1. 先只扫描 `mod/` entry 路径,建立“模块名 -> entry path”的索引。
2. 遇到 `import mylib.api;` 时,直接定位 `mod/mylib/api.ft`。
3. 由 `src/symbol/` 中的 `.ft` 读取模块把该文件解析为统一的“已导入模块查询视图”,其中包含公开 `type`、公开 `enum`、`spec`、`fit`、顶层 `func`、模块级 `let` / `var`、公开成员等声明级事实。
4. 编译驱动把这些查询视图通过抽象查询接口注入核心分析器。
5. 后续类型检查、名称查找、契约关系判断、`let` 成员重复绑定检查都基于该抽象查询接口进行,不重解析文本接口,也不让核心分析器直接依赖 `.ft` 模块。

Provider 的第一版实现可以在注册 `.fb` 时预加载其中的公开 `.ft` entry; 该策略只属于 Provider 内部实现。未来切换为按 `import` 懒加载时,上层接口与核心分析器语义不得改变。

### 4.3 IDE/LSP 消费流程

1. 打开当前项目时,扫描 `build/<归一化 host 平台>/obj/symbols/**/*.ft`。
2. 若本地缓存 `.ft` 指纹有效,直接读取声明、签名、文档注释、源码位置用于 hover / completion / definition。
3. 若本地缓存 `.ft` 缺失或失效,退回源码分析,并在下一次成功 `check` / `build` 后重新生成缓存。
4. 对外部依赖,语言服务与编译器可共用 `src/symbol/` 中的 `.ft` 读取器,但对上层都只暴露统一查询接口或查询视图,不把 `.ft` 文件格式细节扩散到核心分析逻辑。

## 5 符号表内容范围

### 5.0 导出与可见性的职责分离

"是否进入 `.ft`"与"是否对外可见"是两个独立职责:

- **导出决策**由技术需要驱动: 链接、对象布局、ABI 元信息等场景都可能要求非公开声明进入 `.ft`。例如非公开 `type` 字段需要为跨包泛型实例化提供布局信息,非公开 `@abi extern func` 需要为链接器提供符号事实。
- **可见性控制**由语义属性驱动: `.ft` 中每条符号记录都携带完整的可见性标志（`SYMS.flags` 中的 `public` 位）,核心编译器在跨模块名称解析与类型检查时严格按此标志执行访问控制。

一条声明进入 `.ft` 不意味着它对 consumer 可见。consumer 编译器读取 `.ft` 后,仍然只能通过可见性检查访问标记为公开的声明; 非公开声明虽然存在于 `.ft` 中,但仅供链接器、运行时布局等底层机制使用,不会在 consumer 的语义分析中变成可访问成员或可引用符号。

### 5.1 公开包表 `.ft` 必须包含的事实

公开包表 `.ft` 至少需要覆盖以下公开语义事实:

- 模块信息: 模块名、可见性、模块级文档注释。
- 公开 `type`。
- 公开 `enum`。
- 公开 `spec`。
- 公开 `fit` 及其建立的契约关系。
- 公开顶层 `func`。
- 公开模块级 `let` / `var`。
- 公开成员字段与成员方法。
- 公开 `type` 的全部字段布局声明; 非公开字段只作为对象布局与跨包泛型实例化的 ABI 元信息导出,不得在 consumer 中变成可访问成员。
- 公开构造函数与终结器函数。
- 公开泛型声明的类型参数列表、参数顺序、参数约束目标与未实例化签名骨架。
- 公开泛型父 `spec` 使用、泛型 `fit` 契约使用以及其有序类型实参事实。
- 支撑跨包重载决议与泛型推导所需的泛型参数数量、参数类型与约束事实。
- 公开 `extern func` 与必要的非公开 `extern` 链接事实所需的 ABI 元信息; 非公开 `extern` 只作为链接事实,不得在 consumer 中变成可访问声明。
- 公开 `let` 成员从源码推断出的已绑定声明事实,以及构造函数体赋值推断出的成员绑定关系。
- 公开声明的文档注释。

#### 5.1.1 私有表示依赖闭包

package-public `.ft` 在公开声明集合上计算最小私有表示依赖闭包。

闭包根为:

- 已收录 `type` 的全部字段类型,不区分字段可见性、静态与否及字段类型是否泛型。
- 已收录 `type`、函数和 `fit` 的 reifiable 依赖。

闭包按以下规则递归:

- 泛型实参、数组元素和指针目标继续参与遍历。
- 具名目标是当前模块的 `type`、`enum` 或 `spec` 时,收录其声明骨架,不区分可见性和是否泛型。
- `type` 骨架包含类型参数及约束、Tuple / `@value` 标记、父 `spec`、全部字段和 reifiable 依赖。
- `enum` 骨架包含全部枚举项及其值。
- `spec` 骨架包含类型参数及约束、父 `spec`、form 及该 form 的成员类型或 callable 签名。
- 仅保留两端声明都已收录且 codegen 需要的现有关系; 不因目标类型被收录而自动收录私有 `fit`。

函数体和初始化器不在闭包阶段重新遍历; 仅由其引用且未形成 reifiable 依赖的私有声明不收录。内建类型、类型参数、数组和指针节点本身不生成顶层声明。无关私有声明不收录。

收录的私有声明必须保留私有标记。读取器和 imported-module cache 可以按声明身份供编译器内部使用,但用户名称查询、`use` 和补全不得返回这些声明。

`NAMED`、`NAMED_GENERIC` 和 `TYPE_PARAM_REF` 类型节点必须保存目标声明身份。writer 必须先确定收录闭包并为全部声明分配 symbol id,再用 `sym_ref` 序列化类型; reader 必须先创建全部声明,再按 `sym_ref` 恢复类型目标,不得把已绑定类型降格为纯名称。

公开包表 `.ft` 明确不包含:

- 函数体、语句、表达式。
- 绑定初值、常量值、运行时数据。
- 私有表示依赖闭包和必要 `extern` 链接事实之外的私有模块级声明与私有成员。
- 源码绝对路径、源码行列号等闭源分发不应泄露的信息。

针对泛型,公开包表还必须满足以下约束:

- `.ft` / `.fb` 导出的是**声明级泛型事实**,而不是某次调用点推导结果或某个单态化实例的专用实现。
- 公开泛型 `type`、`spec`、顶层 `func` 与成员方法,都必须以“声明符号 + 有序类型参数 + 约束 + 未替换签名骨架”的形式导出,使 consumer 仅依赖 `.ft` 就能完成名称解析、约束检查、重载决议与泛型推导。
- 跨包消费泛型时,consumer 不得以“重新读取 provider 源码”或“包内一定存在某个已单态化实例”作为成立前提。
- 当前阶段公开包表无需导出 variance 元信息; 泛型实例兼容性统一按语言规范中的不变规则处理。

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
|  | TSEQ  | type sequence elements                         |  |
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
  |- TSEQ
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
  |- TSEQ
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
| `FT_PROFILE_WORKSPACE_CACHE` | `0x02` | `build/<platform>/obj/symbols/**/*.ft` 本地缓存 |
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
| `FT_SEC_TSEQ` | `0x0004` | 必需 | 全部 | 类型序列元素 |
| `FT_SEC_RELS` | `0x0005` | 必需 | 全部 | 关系记录 |
| `FT_SEC_DOCS` | `0x0006` | 可选 | 全部 | 文档注释 |
| `FT_SEC_ATTRS` | `0x0007` | 可选 | 全部 | 扩展属性 |
| `FT_SEC_SPNS` | `0x0010` | 可选 | workspace-cache | 源码位置 |
| `FT_SEC_USES` | `0x0011` | 可选 | workspace-cache | 依赖模块与指纹 |
| `FT_SEC_META` | `0x0012` | 可选 | workspace-cache | 缓存失效信息 |

保留规则:

- `0x0008` 至 `0x000F` 预留给未来核心节。
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
|- entry(kind = TSEQ,  offset = ..., size = ...)
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
  -> locate STRS/SYMS/TYPS/TSEQ/RELS
  -> load optional DOCS/ATRS as needed
  -> if workspace-cache profile, load SPNS/USES/META
```

#### 6.3.4 兼容性规则

`.ft` 的兼容性判断必须基于**格式版本与 ABI 兼容契约**,而不是基于“生产者编译器版本号是否完全相同”。

规则如下:

- 编译器不得仅因包由不同版本的 Feng 编译器生成而拒绝使用。
- `FT_VERSION_MAJOR` 相同表示 core 外壳兼容: Header 布局、Section Directory 布局以及 core required section 的固定记录外形保持稳定。
- 在相同 `FT_VERSION_MAJOR` 内,`FT_VERSION_MINOR` 只允许做追加式演进: 新增可选 section、新增 attr key、新增 flag bit、新增 append-only kind 常量; 不得改写既有 required section 的固定记录布局。
- 泛型进入公开 `.ft` 时,必须优先通过“追加新的 `FT_SYM_*` / `FT_TYPE_*` / `FT_REL_*` kind 常量与追加 attr key”表达新增语义,让不理解这些语义的旧 consumer 显式拒绝; 不得把泛型结构偷偷折叠进旧 `FT_TYPE_KIND_NAMED` 的字符串文本或其他会被旧 consumer 误读的既有字段语义中。
- 若新增语义会让旧 consumer 在“忽略后仍可能编译错误或链接错误”,则不得作为同 major 的 silently-optional 扩展发出; 此类变化必须提升 major,或通过新的 required section 让旧 consumer 明确拒绝。
- 新er 编译器必须能够读取并消费旧的同 major `.ft` 包; 旧编译器是否能读取较新的同 major `.ft`,取决于该包是否只使用了其可安全忽略的追加扩展。
- 是否可链接、是否可运行,除 `.ft` 格式外还取决于对应平台库文件、运行时 ABI 和 `@abi` / bridge 规则是否兼容; 这些不由编译器版本号单独决定。

#### 6.3.5 `ATRS` 扩展属性节

为尽量避免“出现一个新注解或新修饰就改 core 记录布局”,v1 预留 `FT_SEC_ATTRS` 作为统一扩展槽。

`ATRS` 的用途:

- 承载新语法对应的声明级附加语义,例如未来新增的修饰符、ABI 扩展元信息或额外约束。
- 承载当前已存在但不适合硬塞进 `SYMS` 固定字段的元信息,例如调用约定、外部库来源。
- 让 future feature 尽量通过“追加 attr key”演进,而不是频繁改动 Header 或 core section 布局。

`ATRS` 的记录策略如下:

- `ATRS` 为可选 section。
- 未识别的 attr key,只有在其所在 section 标记 `FT_SEC_FLAG_IGNORABLE` 时,consumer 才可安全跳过。
- 若某个新增语义对正确编译是强制性的,则不得仅以“可忽略 attr”形式发布给旧 consumer。

`ATRS` 的固定记录结构建议与当前实现保持一致:

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `symbol_id` | `u32` | attr 所属 `SYMS.id` |
| `kind` | `u16` | attr key 常量 |
| `reserved0` | `u16` | 固定写 `0` |
| `value0` | `u32` | key 专用值 |
| `value1` | `u32` | key 专用值 |
| `value2` | `u32` | key 专用值 |

attr key 常量建议如下:

| 常量 | 值 | owner | 说明 |
| --- | --- | --- | --- |
| `FT_ATTR_DECLARED_SPECS` | `0x0001` | `type` / `spec` / `fit` | 声明级 `spec` 使用范围；`value0 = first_type_id`, `value1 = count` |
| `FT_ATTR_CALL_CONV` | `0x0002` | `extern_fn` | ABI 调用约定枚举值；`value0` = 调用约定枚举 |
| `FT_ATTR_ABI_LIBRARY` | `0x0003` | `extern_fn` | ABI 库名字符串；`value0` = `STRS.id` |
| `FT_ATTR_ENUM_ITEM_VALUE` | `0x0004` | `enum_item` | 归一化后的枚举项底层值；`value0` = 按二补码解释的 `int32` 原始位模式 |
| `FT_ATTR_STATIC_MEMBER` | `0x0005` | `field` / `method` | `type` 静态成员标记 |

补充规则:

- `FT_ATTR_DECLARED_SPECS` 在 `type` 上表示 `type A: B, C` 头部的 `spec` 使用列表,在 `spec` 上表示 `spec Child: Parent, Other` 的父 `spec` 使用列表,在 `fit` 上表示右侧 `spec` 使用列表; `fit` 目标类型自身继续走 `SYMS.extra_ref`。
- `FT_ATTR_CALL_CONV` 与 `FT_ATTR_ABI_LIBRARY` 仅出现在 `extern_fn` 符号上; 普通函数与方法无需这两个 attr。
- `FT_ATTR_ENUM_ITEM_VALUE` 只出现在 `enum_item` 子符号上,记录 consumer 恢复 `Enum.Item` 所需的稳定值事实; 不再额外导出“原本是显式赋值还是隐式赋值”的源码细节。
- `FT_ATTR_STATIC_MEMBER` 只出现在 `type` 的静态字段或静态方法符号上,表示 consumer 恢复成员时必须设置 `static` 语义。
- 泛型第一阶段不要求新增其他 attr key。类型参数声明、类型参数引用、泛型类型实参与泛型 callable 骨架通过 `SYMS` / `TYPS` / `TSEQ` 表达,而不是把核心语义塞进 `ATRS`。

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
| `kind` | `u16` | 符号种类 |
| `flags` | `u16` | 符号标志 |
| `type_ref` | `u32` | 符号的完整类型节点 ID（`TYPS.id`）; MODULE / FIT 为 `0` |
| `extra_ref` | `u32` | kind 专用辅助引用: FIT 为目标类型 `TYPS.id`; MODULE 为模块名字符串 ID; 其余 `0` |
| `doc_ref` | `u32` | 文档记录 ID,无则为 `0` |

`type_ref` 对各 kind 的含义:

| kind | `type_ref` 指向 |
| --- | --- |
| `binding` / `field` | 值类型的 `TYPS.id` |
| `func` / `method` / `ctor` / `dtor` | TYPS callable 节点（含所有参数 + 返回类型） |
| `spec` | TYPS spec 节点（TYPS.kind 区分 form: object / callable / union） |
| `type` | 类型自身对应的 `TYPS.id` |
| `fit` | `0`（fit 无自己的"类型"；目标类型走 `extra_ref`） |
| `module` | `0` |

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
- `enum`
- `enum_item`
- `type_param`

建议第一版 `SYMS.kind` 固定常量值如下:

- `FT_SYM_KIND_MODULE = 1`
- `FT_SYM_KIND_TYPE = 2`
- `FT_SYM_KIND_SPEC = 3`
- `FT_SYM_KIND_FIT = 4`
- `FT_SYM_KIND_TOP_FN = 5`
- `FT_SYM_KIND_EXTERN_FN = 6`
- `FT_SYM_KIND_CTOR = 7`
- `FT_SYM_KIND_DTOR = 8`
- `FT_SYM_KIND_FIELD = 9`
- `FT_SYM_KIND_METHOD = 10`
- `FT_SYM_KIND_TOP_LET = 11`
- `FT_SYM_KIND_TOP_VAR = 12`
- `FT_SYM_KIND_ENUM = 13`
- `FT_SYM_KIND_ENUM_ITEM = 14`
- `FT_SYM_KIND_TYPE_PARAM = 15`

建议支持的 `flags`:

- `public`
- `mutable`
- `abi`
- `extern`
- `bounded_decl`
- `has_doc`

说明:

- `owner_id` 负责表达层级关系,例如字段/方法归属于某个 `type` 或某个 `fit`。
- `abi` 表示该声明携带 `@abi` 兼容性元信息。
- `bounded_decl` 仅用于 `type` 实例 `let` 字段,表示该字段已在成员声明初始化阶段完成最终显式绑定; 顶层 `let`、`static let` 以及其他非实例成员不使用该标志记录绑定状态。
- `enum_item` 是 `enum` 的子符号而不是独立顶层声明: `owner_id` 指向所属 `enum`, `name_str` 表达 item 名称, `extra_ref` 固定表达其 0-based 声明顺序。
- `enum` 自身作为 type-like 顶层声明导出; `enum_item` 的 `type_ref` 固定写 `0`, 因为其归属 enum 已由 `owner_id` 唯一给出。
- `fit` 作为独立符号存在,便于记录“由哪个 `fit` 建立了哪些契约关系与扩展方法”。
- 被语义分析判定为“不得导出”的声明,不进入公开 `.ft`; 本地缓存 `.ft` 可按本地需要保留。

#### 6.5.1 enum / enum_item 导出与查询视图补充

enum item 的 `.ft` 形状在 v1 中固定为“**顶层 `enum` 符号 + 子符号 `enum_item`**”,不采用“把所有 item 平铺成 enum owner 上的 attr 列表”这种形状。原因如下:

- `enum_item` 需要同时承载名称、所属 enum、声明顺序与底层值事实,其中名称与 owner 天然属于 `SYMS` 子符号关系。
- workspace-cache profile 需要让 LSP 对 `Enum.Item` 做精确 definition; 将 item 建模成独立子符号后,可直接复用现有 `SPNS` 节为 item 记录源码位置,无需为 item 额外设计一套 span attr。
- package-public profile 仍不泄露源码位置,因为公开 `.ft` 本就不包含 `SPNS`; 它只导出 item 名称、顺序和值,满足跨包语义恢复所需的最小事实。

固定规则如下:

- `enum` 使用 `FT_SYM_KIND_ENUM`, 作为模块根下的公开 type-like 顶层声明参与公开 decl 枚举与重名检查。
- 每个 `enum_item` 使用 `FT_SYM_KIND_ENUM_ITEM`, `owner_id` 指向所属 `enum`, `extra_ref` 固定保存 0-based 声明顺序。
- `enum_item` 的归一化底层值通过 `FT_ATTR_ENUM_ITEM_VALUE` 导出; `value0` 存放该值按 `int32_t` 二补码重解释后的 `u32` 位模式。consumer 读取时必须按 `int32_t` 语义恢复,不得把它解释成无符号业务值。
- `enum_item` 不进入模块级“公开 value”集合; consumer 恢复 `Enum.Item` 时,必须先解析 owner `enum`, 再在其子符号中按名称查找 `enum_item`。
- imported-module 查询视图必须至少提供三类能力: 1) 顶层按名查找公开 `enum`; 2) 按 owner 枚举/按名查找其 `enum_item`; 3) 返回 item 的声明顺序与归一化底层值,以恢复 `Enum.Item` 语义、completion 候选与 hover 文本。
- workspace-cache `.ft` 可以像其他 decl 一样为 `enum_item` 写入 `SPNS`; package-public `.ft` 不得为 item 暴露源码路径或行列号。
- v1 不为 `enum_item` 单独导出文档注释、显式/隐式赋值来源或其他源码细节; 这些不属于跨包消费恢复 `Enum.Item` 所需的最小事实。

针对泛型,`SYMS` 还必须满足以下规则:

- 泛型声明的类型参数必须作为独立符号导出,并使用 `FT_SYM_KIND_TYPE_PARAM`; 其 `owner_id` 指向所属声明符号。合法 owner 仅包括 `type`、`spec`、`top_fn` 与 `method`; `fit` 不定义新的类型参数,因此不得拥有 `type_param` 子符号。
- `type_param` 符号的 `name_str` 表达参数名,`extra_ref` 固定表达其在声明头中的 0-based 有序位置,`type_ref` 在有约束时指向约束目标对应的 `TYPS.id`,无约束时为 `0`,`doc_ref` 固定为 `0`。
- `type_param` 是导出辅助符号,用于恢复泛型声明头; consumer 不得把它当作普通可枚举成员方法、字段或顶层声明对外展示。
- consumer 必须按 `owner_id + extra_ref` 恢复类型参数顺序; 不得按名称重排或重编号。
- 具名泛型声明在跨包查询时的 identity 仍按“名称 + 泛型参数数量”判断; `.ft` 读取器不得仅因参数名或约束目标不同就把同名同参数数量声明视为不同实体。
- `fit` 符号的 `extra_ref` 继续表示目标类型的 `TYPS.id`; 当目标是 `UserType<T, U>` 这类泛型使用时,该 `TYPS.id` 必须指向结构化泛型类型节点,而不是纯文本名字。

#### 6.5.1 core section 引用关系图

从“谁引用谁”的角度看,core section 大致是这个结构:

```text
STRS
 ^        ^
 |        |
 |        +--------------------------------- DOCS.doc_str_id
 +----------------------------------------- SYMS.name_str / MODULE name / ABI library str

SYMS
 |- owner_id  ------> SYMS.id
 |- type_ref  ------> TYPS.id
 `- doc_ref   ------> DOCS.id

TYPS
 `- elem_start ------> TSEQ range (elem_count 个元素)

TSEQ
 `- type_id ---------> TYPS.id

RELS
 `- left/right/owner -> SYMS.id
```

也就是说,实际消费时通常先读 `STRS`,再读 `SYMS`,然后按需解开 `TYPS`、`TSEQ`、`RELS` 与 `DOCS`。

### 6.6 `TYPS` 类型节点

`TYPS` 用于表达已解析后的类型结构,避免在消费端再次解析类型文本。

`TYPS` 的固定记录结构如下:

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `kind` | `u16` | 类型节点 kind 常量 |
| `reserved0` | `u16` | 固定写 `0` |
| `string_ref` | `u32` | kind 专用字符串引用（见各 kind 说明） |
| `sym_ref` | `u32` | kind 专用声明符号引用（见各 kind 说明） |
| `elem_start` | `u32` | TSEQ 第一个元素的 0-based 下标（0 = 无） |
| `elem_count` | `u32` | TSEQ 元素数量（array: rank） |
| `reserved1` | `u32` | 固定写 `0` |

建议第一版 `TYPS.kind` 固定常量值如下:

- `FT_TYPE_KIND_BUILTIN = 1`
- `FT_TYPE_KIND_NAMED = 2`
- `FT_TYPE_KIND_ARRAY = 3`
- `FT_TYPE_KIND_C_POINTER = 4`
- `FT_TYPE_KIND_TYPE_PARAM_REF = 5`
- `FT_TYPE_KIND_NAMED_GENERIC = 6`
- `FT_TYPE_KIND_CALLABLE = 7`
- `FT_TYPE_KIND_SPEC_OBJECT = 8`
- `FT_TYPE_KIND_SPEC_CALLABLE = 9`

若出现新的不可约类型构造，优先追加新的 `FT_TYPE_*` kind 常量，而不是改动既有 `TYPS` 记录壳。本轮确认新增：

- `FT_TYPE_KIND_SPEC_UNION = 10`（union-form spec，本轮交付）

预留位置：

- `FT_TYPE_KIND_TUPLE = 11`（未来元组）

类型节点建议以 DAG 形式表达,供多个符号共享引用。复杂类型一律通过子节点 ID 组合,不在字符串里重新编码一份"类型文本"。

各 kind 的字段使用规则:

**`FT_TYPE_KIND_BUILTIN`**（内置类型）:

- `string_ref`：内置类型名字符串；其余字段为 `0`。

**`FT_TYPE_KIND_NAMED`**（具名非泛型类型）:

- `string_ref`：按 `.` 连接的规范名称；`sym_ref`：声明符号 ID；其余字段为 `0`。

**`FT_TYPE_KIND_NAMED_GENERIC`**（泛型具名使用，如 `Box<T>`）:

- `string_ref`：不含类型实参的规范名称；`sym_ref`：基声明符号 ID；
- `elem_start` / `elem_count`：在 TSEQ 中的类型实参范围（各元素 `name_str=0`）。
- consumer 不得把它降格为纯文本名字；必须使用新增 kind 让旧 consumer 显式拒绝。

**`FT_TYPE_KIND_CALLABLE`**（函数/方法签名）:

- `elem_start` / `elem_count`：在 TSEQ 中的有序元素范围。
  - 前 N-1 个元素为参数（`name_str` = 参数名字符串 ID）；
  - 最后 1 个元素为返回类型（`name_str = 0`）。
- 方法的 `self` 不写入 TSEQ；由 `owner_id` 与符号 `kind` 隐式表达。
- `string_ref`、`sym_ref`、`reserved1` 固定为 `0`。

**`FT_TYPE_KIND_SPEC_OBJECT`** / **`FT_TYPE_KIND_SPEC_CALLABLE`**（spec 不同 form）:

- spec 的 form 通过 TYPS.kind 区分，不通过 SYMS.kind；
- `sym_ref`：spec 声明符号 ID；其余字段见 spec 具体编码规则。

**`FT_TYPE_KIND_SPEC_UNION`**（union-form spec）:

- spec 的 form 通过 TYPS.kind 区分，不通过 SYMS.kind；
- `sym_ref`：spec 声明符号 ID；
- `elem_start` / `elem_count`：在 TSEQ 中的归一化 member 类型范围；每个元素 `name_str = 0`，`type_id` = 对应 member 的 TYPS.id，按声明顺序排列。

**`FT_TYPE_KIND_TYPE_PARAM_REF`**（类型参数引用）:

- `string_ref`：参数名字符串；`sym_ref`：被引用的 `type_param` 符号 ID；其余字段为 `0`。
- consumer 不得把它降格为纯文本名字。

**`FT_TYPE_KIND_ARRAY`**（数组类型）:

- `string_ref`：逐层可写位图 `mutability_bitmap`（覆盖 `T[]`、`T[!]`、`T[!][]` 等语义）；
- `elem_count`：数组层数 `rank`；`elem_start`：元素类型 TYPS.id；`sym_ref`、`reserved1` 为 `0`。

**`FT_TYPE_KIND_C_POINTER`**（C 指针类型）:

- `elem_start`：指向的元素类型 TYPS.id；其余字段为 `0`。

各 kind 的字段汇总:

| kind | string_ref | sym_ref | elem_start | elem_count |
| --- | --- | --- | --- | --- |
| BUILTIN | 类型名字符串 | 0 | 0 | 0 |
| NAMED | 规范名称字符串 | 声明符号 ID | 0 | 0 |
| NAMED_GENERIC | 基名称字符串 | 声明符号 ID | TSEQ 起始下标 | 类型实参数量 |
| CALLABLE | 0 | 0 | TSEQ 起始下标 | 参数数量 + 1 |
| SPEC_OBJECT / SPEC_CALLABLE | 0 | spec 声明符号 ID | 0 | 0 |
| SPEC_UNION | 0 | spec 声明符号 ID | TSEQ 起始下标 | member 数量 |
| TYPE_PARAM_REF | 参数名字符串 | type_param 符号 ID | 0 | 0 |
| ARRAY | mutability_bitmap | 0 | 元素类型 TYPS.id | rank |
| C_POINTER | 0 | 0 | 指向元素 TYPS.id | 0 |

针对泛型,`TYPS` 还必须覆盖以下类型特征:

- 类型参数引用必须以独立类型节点表达,使用 `FT_TYPE_KIND_TYPE_PARAM_REF`。
- 具名泛型使用必须保留"目标声明符号 + 有序类型实参列表"，使用 `FT_TYPE_KIND_NAMED_GENERIC` + TSEQ。
- 同一个具名声明在不同类型实参数量下是不同的类型使用；consumer 按"名称 + 类型实参数量"精确匹配。
- 泛型可调用签名的参数/返回类型，使用 `FT_TYPE_KIND_CALLABLE` + TSEQ；类型参数引用在 TSEQ 元素的 `type_id` 中以 `TYPE_PARAM_REF` 节点表达。
- 导出的泛型 callable 签名必须保持未实例化声明骨架；不得在导出时提前替换为某个调用点的推导结果。

当前 Feng 已有但必须被 `TYPS` 覆盖的类型特征包括:

- `string` 作为 builtin 类型节点处理。
- `T*` 作为 `c-pointer` 类型节点处理。
- 数组的层数与逐层可写性必须进入类型节点,不能只把 `T[!][]` 抹平成一个字符串。

### 6.7 `TSEQ` 类型序列元素

`TSEQ` 是通用有序类型列表，供 `TYPS` 节点（callable 参数列表、泛型实参、元组元素等）共享使用。

`TSEQ` 的固定记录结构如下:

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `name_str` | `u32` | 元素名称字符串 ID（0 = 匿名） |
| `type_id` | `u32` | 元素类型 TYPS.id |
| `flags` | `u16` | 元素标志位（`let`/`var` 可写性等） |
| `reserved0` | `u16` | 固定写 `0` |

`TSEQ` 元素的使用场景:

| 场景 | TYPS kind | TSEQ 内容 |
| --- | --- | --- |
| 函数/方法签名 | CALLABLE | 前 N 个是参数（`name_str` = 参数名），最后 1 个是返回类型（`name_str = 0`） |
| 泛型类型实参 | NAMED_GENERIC | 各实参（`name_str = 0`） |
| 元组（未来） | TUPLE | 各元素（`name_str = 0` 或有标签） |
| union spec 成员列表 | SPEC_UNION | 各归一化 member 类型（`name_str = 0`，`type_id` = member 的 TYPS.id） |

补充规则:

- `TSEQ` 记录全局顺序存储；`TYPS` 节点通过 `elem_start` + `elem_count` 引用一段连续范围。
- writer 必须保证同一 `TYPS` 节点引用的 `TSEQ` 元素是连续的。
- `CALLABLE` 类型的返回类型元素永远是 TSEQ 范围中的最后一个（`name_str = 0`）；consumer 可直接取最后元素作为返回类型，前面所有元素为参数。
- `CALLABLE` 的参数/返回 `TSEQ` 区间中不得夹杂其他 `TYPS` 节点（例如 `NAMED_GENERIC` 类型实参）占用的槽位；即使参数或返回类型本身包含泛型使用，也必须保证 callable 区间自身连续且可独立解码。
- 顶层泛型函数与泛型方法的类型参数列表不平铺进 TSEQ；它们通过 owner 为该 callable 的 `type_param` 子符号表达，callable TSEQ 只引用这些参数参与构成的参数类型与返回类型。
- 若某方法定义在泛型 `type` 内，其 TSEQ 中的参数类型和返回类型可同时引用外层 `type` 的 `type_param` 与当前方法自己的 `type_param`；consumer 必须保持两层作用域可区分。
- `flags` 的具体位定义与 SYMS.flags 保持一致的规范子集，第一版只使用 bit 0（`0x0001` = `var`，即可变参数）；未使用位固定写 `0`。

### 6.8 `RELS` 关系记录

`RELS` 负责表达“不是单个符号属性、而是两个符号之间”的语义关系。

`RELS` 的固定记录结构建议与当前实现保持一致:

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `kind` | `u16` | relation kind 常量 |
| `reserved0` | `u16` | 固定写 `0` |
| `left_symbol_id` | `u32` | 关系左端声明符号 |
| `right_symbol_id` | `u32` | 关系右端声明符号 |
| `owner_symbol_id` | `u32` | 关系归属声明符号 |

建议第一版至少覆盖:

- `type_implements_spec`: `type A: B`
- `spec_extends_spec`: `spec Child: Parent`
- `fit_implements_spec`: `fit A: B`
- `fit_extends_type`: `fit A { ... }` 或 `fit A: B { ... }` 对目标类型 `A` 的扩展归属
- `ctor_binds_member`: 构造函数体赋值推断出绑定了哪些公开 `let` 成员

建议第一版 `RELS.kind` 固定常量值如下:

- `FT_REL_TYPE_IMPLEMENTS_SPEC = 1`
- `FT_REL_FIT_IMPLEMENTS_SPEC = 2`
- `FT_REL_FIT_EXTENDS_TYPE = 3`
- `FT_REL_CTOR_BINDS_MEMBER = 4`
- `FT_REL_SPEC_EXTENDS_SPEC = 5`（追加常量）

带初始化器的 type 实例成员绑定状态由 `SYMS.flags.bounded_decl` 表达,无需额外 relation。读取 `.ft` 时,`bounded_decl` 与 `ctor_binds_member` 都必须还原为语义分析可直接消费的 type 实例成员绑定事实,保证跨包三段式构造的重复绑定检查与本包一致。顶层 `let`、`static let` 以及其他非实例成员的绑定状态不参与对象字面量初始化约束,不得为此写入 `bounded_decl` 或构造绑定 relation。

针对泛型,`RELS` 还必须满足以下规则:

- `RELS` 本身继续只保存声明符号关系,不新增额外字段。关系右侧若存在结构化 `spec` 使用,其具体 `TYPS` 负载通过 owner 上的 `FT_ATTR_DECLARED_SPECS` 范围表达。
- 对同一个 owner,对应 attr 范围中的 `TYPS` 节点顺序必须与同 owner、同 relation kind 的 `RELS` 记录顺序严格一致。consumer 通过“owner + relation kind + ordinal”配对恢复每条关系对应的结构化使用。
- 因而,`spec Child<T>: Parent<T>` 与 `spec Child<T>: Parent<int>` 必须导出为两条 `FT_REL_SPEC_EXTENDS_SPEC` relation,并在 owner 为 `Child` 的 `FT_ATTR_DECLARED_SPECS` 范围中按源代码顺序提供各自的 `TYPS` 使用节点。
- `fit UserType<T, U>: UserSpec<T, U>` 这类泛型 `fit` 必须同时导出“fit 扩展哪个目标 `type`”和“fit 建立了哪个泛型 `spec` 使用关系”两类事实: 目标 `type` 使用继续保存在 `fit` 符号的 `extra_ref`,右侧 `spec` 使用则按 `FT_ATTR_DECLARED_SPECS` 范围与 `FT_REL_FIT_IMPLEMENTS_SPEC` 顺序配对恢复。
- 若关系右侧无类型实参,则对应 attr 范围中的 `TYPS` 节点使用普通 `FT_TYPE_KIND_NAMED`; 不得为兼容旧 consumer 而隐式补出不存在的泛型参数。

### 6.9 `DOCS` 文档注释

`DOCS` 保存“符号 ID -> 文档字符串 ID”的映射。

建议 `DOCS` 使用固定记录结构:

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `u32` | 文档记录 ID |
| `symbol_id` | `u32` | 所属符号 ID |
| `doc_str_id` | `u32` | 文档正文字符串 ID |

约束:

- `SYMS.doc_ref` 指向 `DOCS.id`。
- `DOCS.symbol_id` 必须回指对应的 `SYMS.id`。
- `DOCS.doc_str_id` 指向 `STRS.id`。

文档注释采集规则保持一致:

- 只有单个紧邻声明的 `/** */` 块才是文档注释。
- 文档注释与声明之间不得出现空行。
- 文档注释与声明之间允许出现声明级前缀,即注解、`open` / `seal` 与 `extern`。
- `//` 与普通 `/* */` 不参与文档注释绑定; 若它们出现在 `/** */` 与声明之间,则绑定失效。
- 一个声明最多绑定一个 `/** */` 文档块。

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
open module mylib.api;

# 用户模型
open type User: Named {
    open var name: string;

    open let id: int = 0;

    open func get_info(): string;
}

open fit User: Auditable {
    open func audit(): string;
}

open func add_user(u: User): bool;
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

若把示例扩展为:

```feng
open spec Reader<T> {
  func get(): T;
}

open type Box<T> {
  open let value: T;
}

open fit Box<T>: Reader<T> {
  open func get(): T;
}
```

则公开 `.ft` 还最少应出现:

- `type(Box)`
- `type_param(T)` owned by `Box`,其顺序为 `0`
- `spec(Reader)`
- `type_param(T)` owned by `Reader`,其顺序为 `0`
- `fit(Box<T>: Reader<T>)`
- `fit.extra_ref -> TYPS(NAMED_GENERIC, string_ref='Box', sym_ref=Box符号ID, elem_start=K, elem_count=1)`，TSEQ[K] = `{name_str=0, type_id=TYPS(TYPE_PARAM_REF, 'T')}`
- attr `declared_specs(fit#2)` 记录一个长度为 `1` 的 `TYPS` 范围,其中唯一节点为 `TYPS(NAMED_GENERIC, string_ref='Reader', elem_start=M, elem_count=1)`，TSEQ[M] = `{name_str=0, type_id=TYPS(TYPE_PARAM_REF, 'T')}`
- relation `fit_extends_type(fit#2 -> Box)`
- relation `fit_implements_spec(fit#2 -> Reader)`,并按 attr 顺序与 `Reader<T>` 这一结构化使用配对
- `method(get)` owned by `fit#2`,其 `type_ref` 为 `TYPS(CALLABLE, elem_start=P, elem_count=1)`，TSEQ[P] = `{name_str=0, type_id=TYPS(TYPE_PARAM_REF, 'T')}` （返回类型 T）

这样外部 consumer 在只读取 `.ft` 的情况下,就能知道这是一个泛型 `fit`,并能恢复 `Box<T>` 与 `Reader<T>` 的声明级关系,而不需要读取 provider 源码或假设包内存在某个 `Box<int>` 的专门实例。

## 8 失效与复用策略

### 8.1 公开 `.ft`

- 公开 `.ft` 的生成以“模块公开语义变化”为失效条件。
- 若模块只改了私有实现且不影响公开接口,理论上可复用旧公开 `.ft`。
- `pack` 只接受与当前公开语义一致的公开 `.ft`; 若缺失或过期,应重新执行语义分析与导出。

### 8.2 本地缓存 `.ft`

- 本地缓存 `.ft` 以源文件指纹、依赖模块指纹、`feng.fm` 相关字段、编译器构建指纹共同决定是否失效。
- LSP 读到失效本地缓存 `.ft` 时直接忽略并回退到源码分析,而不是冒险继续使用旧缓存。
- `clean` 时统一删除各目标平台的 `build/<platform>/mod/` 与 `build/<platform>/obj/symbols/`；清理某一平台时不得删除其他平台目录。

## 9 推荐落地顺序

建议按以下顺序推进:

1. **先定内存模型**: 在前端/语义分析后构建统一“模块符号图”,不要直接在 writer 里拼字节。
2. **先落公开 `.ft`**: 打通 `build/<platform>/mod/**/*.ft` 生成、读取以及 `pack` 的多平台校验与提取,先把跨包消费闭环立住。
3. **再在同一格式上补 workspace-cache profile**: 增加 `SPNS`、`USES`、`META`,供当前项目语言服务复用。
4. **再接编译器读取器**: 依赖包消费改走 `.ft` 二进制查询。
5. **最后接 LSP**: 当前项目优先读 `build/<归一化 host 平台>/obj/symbols/**/*.ft`,外部依赖读 `.fb/mod/**/*.ft`。

## 10 需要评审确认的点

当前仍有以下待确认项:

1. Phase 3 的本地缓存 `.ft` 是否只缓存声明级符号,还是要把局部符号也一并纳入。当前建议是**先不纳入局部符号**。
2. 公开 `.ft` 是否在第一版就强制包含 `DOCS`; 当前建议是**应包含**,这样外部依赖包的 hover 才不需要额外侧车文件。
3. `ATRS` 第一版是否把泛型所需 attr key 一并纳入,还是继续只覆盖非泛型 ABI 元信息。当前建议是**一并纳入**,由 `FT_ATTR_DECLARED_SPECS` 覆盖 `type` / `spec` / `fit` 的泛型结构化 `spec` 使用范围。
4. 泛型 capability 是否直接沿用当前 `FST1` major 的 append-only 演进方式。当前建议是**可以**,但必须通过新增 `FT_SYM_KIND_TYPE_PARAM`、`FT_TYPE_KIND_TYPE_PARAM_REF`、`FT_TYPE_KIND_NAMED_GENERIC`、`FT_TYPE_KIND_CALLABLE`、`FT_REL_SPEC_EXTENDS_SPEC` 与新增 TSEQ section、新增 attr key 明确表达,让旧 consumer 显式拒绝,不得把泛型结构偷偷编码进旧 kind 的字符串语义里。
5. 若实现阶段需要兼容读取旧 `.fi`,是否只作为临时 reader 兼容而不进入规范。当前建议是**即使短期兼容读取,仓库文档与新产物也只使用 `.ft`**。

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
- 本地工程的 IDE 类型感知优先复用 `build/<归一化 host 平台>/obj/symbols/**/*.ft`,减少重复工作且不得跨目标平台读取 workspace cache。

这条路径与主流编译型语言的做法一致,也更适合后续把跨模块语义分析稳定收敛为查询模型。

## 13 与相关规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范与文件扩展名总览。
- [feng-package.md](./feng-package.md): `.fb` 包结构、`feng.fm` 字段语义以及编译器可从 `.fb` 读取哪些元信息。
- [feng-build.md](./feng-build.md): 编译器与构建工具的职责划分、`.fb` 消费路径与构建协议。
- [feng-module.md](./feng-module.md): 模块名、`import`、公开导出与模块级初始化规则。
- [feng-fit.md](./feng-fit.md): `fit` 的语义边界与公开导出规则。
- [feng-function.md](./feng-function.md): 顶层 `func`、成员方法、构造函数与终结器的语义规则。
