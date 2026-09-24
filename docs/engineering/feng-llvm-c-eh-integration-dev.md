# LLVM C EH 插件与测试链接器接入方案

## 1. 状态与目标

状态：2026-09-24 基于实际代码整理的待 Review 方案，尚未实施接入。
用户已提交独立插件、LLD 补丁及预构建产物，提出先完成工具链接入，再修复 S11。
本文把接入作为独立阶段；具体接口及既有测试适配须按 §6 Review 后实施。

| 阶段 | 主文档与职责 |
| --- | --- |
| 独立工具 | [插件开发方案](./c-ir-llvm-exception-plugin-dev.md)：协议、插件、LLD 补丁、手工预构建及独立验收 |
| 工具链接入 | 本文：Feng driver、开发与发行布局、测试编译入口、macOS UBSan 链接器及 CI 资源获取 |
| S11 修复 | [S11 修复方案](./feng-release-exception-unwind-bugfix.md)：Feng 协议发码、原生异常表解析、runtime 交接和 release 行为验证 |

本阶段使 Feng 的编译与测试实际消费已提交的工具，保持现有异常发码及 runtime 行为。
不改变对象、描述符、runtime API、ABI 或 FT，不先迁移旧 LSDA 夹具，也不实现 S11。
插件协议与补丁机制只在插件主文档定义，本文只规定使用方如何接入。

## 2. 代码核对与可行性

| 已核对位置 | 当前行为与接入要求 |
| --- | --- |
| `third_party/llvm-c-eh/src/Plugin.cpp` | 只转换带配置标记的函数；无协议输入保持不变，已有独立对照测试。因此可先加载插件，再由 S11 引入协议发码。 |
| `src/cli/compile/driver.c` | `bin`、`lib` 两条路径均已有 `-fexceptions`，均读取 `FENG_CC_FLAGS`；尚未加载插件。须共用插件参数构造，不能只覆盖最终可执行文件。 |
| `Makefile:toolchain-layout` | 当前只映射 LLVM 与 sysroot；须补齐同一 host 的插件布局。 |
| `scripts/release_assemble.sh` | 当前只复制 host LLVM 与 sysroot；须把插件及配套文件装入发行包。 |
| `scripts/release_finalize_macos.sh` | 已按文件格式识别 Mach-O executable、dylib 和 bundle。应先验证现有签名遍历能覆盖插件，不因扩展名增加专门签名分支。 |
| `test/codegen/test_codegen.c`、`test/codegen/test_defer_generic_context.c`、`test/debug/test_debug.c` | 存在直接调用 `cc`／bundled Clang 编译生成 C 的入口，绕过 driver；也须加载插件。 |
| `Makefile:test-sanitize` | 分别构建带 UBSan 的 Feng／C 测试程序，以及通过 `FENG_CC`、`FENG_CC_FLAGS` 编译 Feng 生成代码；补丁 LLD 须覆盖两类实际链接。 |
| `.github/workflows/release.yml` | checkout 使用 `lfs: false`，随后只恢复 `toolchain` 预构建包；`test_tools` 尚无获取步骤。 |
| `scripts/toolchain-prebuilt-publish.sh` | 归档整个 `toolchain`，已有机制可以携带插件；须实际发布含插件的新预构建包，不能把源码提交等同于 CI 已取得新产物。 |

macOS driver 当前未显式选择 LLD，bundled LLVM 只有 `ld.lld -> lld`，没有
`ld64.lld`。按已确认的 LLVM 编译器／链接器配套要求，接入还须补齐普通 macOS
链接路径，不能只处理 UBSan 后仍让普通 Feng 构建默认调用 Apple ld。

上述事实支持分阶段实施，但“插件能加载”与“Feng 异常后端已迁移”是两个验收结果。
本阶段现有 Feng C 尚无协议标记，默认全量测试通过也不能证明 S11 已修复。

## 3. 插件接入方案

### 3.1 定位与开发／发行布局

仓库预构建来源仍为 `toolchain/llvm-c-eh/<host>/`，复用现有 CLI 安装相对路径解析。
建议开发和发行共用如下相对布局：

```text
<安装根>/bin/feng
<安装根>/toolchain/llvm/...
<安装根>/toolchain/llvm-c-eh/lib/llvm_c_eh.so 或 llvm_c_eh.dylib
<安装根>/toolchain/llvm-c-eh/include/llvm_c_eh.h
```

开发时 `build/toolchain` 保持实体目录，已有 `llvm`、`sysroot` 子目录分别软链接；
不把 `build/toolchain` 整体改成软链接。新增 `llvm-c-eh` 子链接后布局为：

```text
build/toolchain/llvm       -> ../../toolchain/llvm/<host>
build/toolchain/sysroot    -> ../../toolchain/sysroot
build/toolchain/llvm-c-eh  -> ../../toolchain/llvm-c-eh/<host>
```

发行时复制对应目录，
同时携带许可证与来源记录。插件按运行 Clang 的 host 选择，交叉编译不按目标 CPU
或 GNU／musl target 改选插件。头文件仅增加编译搜索路径，S11 才在生成 C 中引用它。

插件及 LLVM 继续通过既有预构建流程分发；普通 make、CI 和发行组装不源码构建插件。
安装路径必须可搬移且支持空格；不在 driver 中加入仓库源码绝对路径或 `test_tools` 路径。

### 3.2 driver 与直接 C 测试入口

- `bin` 和 `lib` 的生成 C 编译统一加入插件加载与头文件目录参数，保留现有
  `-fexceptions`、优化级别、诊断及目标参数。直接编译、项目构建、run、包构建均覆盖。
- 保持 `FENG_CC` 的现有选择优先级；所选编译器必须兼容 LLVM 22.1.8 的配套插件。
  插件缺失、路径错误或真实加载失败时明确报错，不静默跳过插件或更换编译器。
- 协议版本和 LLVM 插件接口版本是不同概念，复用插件主文档的校验边界；不重复定义协议。
- 直接编译生成 C 的测试保留现有调用 C 编译器的方式，在已有测试编译辅助函数中统一
  补齐插件参数，覆盖 `-c`、宿主与交叉目标。本阶段不为这些测试扩展 driver API 或
  CLI 选项；保留 `-Werror` 等诊断要求，不改现有夹具、断言或性能门槛。
- 编译 Feng 编译器自身的普通 C 源码不需要加载插件；插件服务于生成 C 的编译。
  S11 阶段才迁移手写旧 LSDA 的 runtime 测试夹具。
- 核对实际原生产物复用与构建依赖，保证更新插件后下一次相关编译使用新产物；空增量
  构建不重写工具。沿用现有机制，不引入新的缓存系统或 FT 版本变化。

### 3.3 普通链接路径

建议沿用 Linux 已有的 `-fuse-ld=lld` 策略，把 macOS 普通链接也指向 LLVM LLD。
macOS bundled LLVM 补齐 `ld64.lld -> lld` 标准入口，剪裁与发行校验同步维护该入口；
不替换其原版 LLD 二进制。此项是 §6 的明确 Review 项。

插件安装和实际加载须覆盖发行归档、解压搬移、macOS 签名后的产物。
插件现有产物最低 macOS 为 26.0，当前 macOS CI 使用 `macos-26`；本阶段不据此
扩展旧系统支持承诺。各平台的已验证边界继续引用插件文档，Linux x64 原生验收由
对应 CI／主机补齐，不能把既有 Rosetta 结果记为原生通过。

## 4. macOS UBSan 测试链接器

### 4.1 使用范围与参数边界

使用已提交的 `test_tools/lld/macos-arm64/bin/ld64.lld`，只在 macOS UBSan 测试
链接阶段启用，排除已知 Mach-O 展开信息错误对测试的干扰；UBSan 插桩仍由 Clang 负责。
Release 插件本身保持不带 sanitizer 插桩；被编译程序是否启用 UBSan
由测试配置决定，两者不混用。补丁 LLD 不替换 bundled LLD，也不进入 Feng 发行包。

需覆盖两条路径：

1. 构建带 UBSan 的 Feng 编译器及 C 单元测试程序时，测试 Makefile 的 `LDFLAGS`
   使用 `-fuse-ld=lld`，由下述测试环境选择补丁 LLD；runtime 静态库的对象编译／归档
   不需要调用链接器。
2. CLI、std、FCTS 等测试通过 Feng 编译带 UBSan 的生成 C 时，沿用 §3.3 的普通
   LLD 选择，由同一测试环境让 Clang 找到补丁版本；生成静态库的 `-c` 不传链接参数。

现有 `FENG_CC_FLAGS` 同时进入 `bin` 和 `lib`，且按空白拆分，因此不建议把
`--ld-path=<路径>` 直接塞入该变量作为正式方案：它会进入不执行链接的 `-c`，
也不能正确表示包含空格的链接器路径。

使用 Clang 已有的 `COMPILER_PATH`，仅在 macOS UBSan 测试命令的环境中指向仓库
`test_tools/lld/macos-arm64/bin` 的绝对路径。Clang 在 `-fuse-ld=lld` 下从该目录
找到 `ld64.lld`；环境由 Feng 的子进程继承，无需新增 `FENG_LD` 或 driver 测试选项。
测试配置须先验证补丁文件、版本与校验和，并确认实际选择的链接器；不能依赖缺失目录
下的 Clang 回退。普通配置不注入此环境，避免影响后续发行构建。

2026-09-24 本机 LLVM Clang 22.1.8 的 `-###` 命令展开验证：设置上述环境并用
`-fuse-ld=lld -fsanitize=undefined` 时，最终链接命令指向补丁 `ld64.lld`；同一环境
下的 `-c -Werror -fsanitize=undefined` 不生成链接命令、无未使用参数错误。
相反，在 `-c -Werror` 中直接加入 `--ld-path` 或 `-fuse-ld=lld` 会报未使用参数。
该验证只证明参数与工具选择，正式实施仍须执行完整编译、链接及运行回归。

测试配置按 host 和 sanitizer 阶段注入上述环境，driver 本身不判断“是否正在跑测试”，
也不负责定位、下载或构建补丁 LLD。保留全部 UBSan 检查，不通过关闭 function
sanitizer、减少用例或换成发行包的不完整 sanitizer 资源取得成功。

### 4.2 本地和 CI

本地复用仓库内的预构建工具。macOS CI 在全量测试之前定向取得 `test_tools/**` 的
Git LFS 实体文件，并检查补丁版本、可执行性与校验和；不在 CI 源码重建 LLD。
建议复用 Git LFS 获取，不为这一步新增独立测试工具 Release 或下载脚本。

普通配置仍验证原版 LLD；Linux 流程不注入 macOS 工具。测试阶段结束后不得把
显式补丁链接器选项带入普通构建或发行流程。测试工具及其 LFS 获取失败时，应在
进入完整测试前明确报错，不能把未下载的 LFS 指针当作有效程序。

## 5. 接入阶段验收

| 范围 | 验收内容 |
| --- | --- |
| 插件实际消费 | 验证真实命令加载了正确 host 插件；增加可确认转换发生的受控协议集成夹具，配独立测试 runtime，不提前依赖尚未迁移的 Feng personality；仅验证无标记 Feng 程序成功不充分。 |
| 无协议代码 | 对照加载前后生成 C、原生 IR／汇编及行为；使用相同编译器、链接器和参数进行比较，避免把链接器切换误算为 Pass 的影响。 |
| 编译入口 | bin／lib、直接／项目／包构建、默认／release、bundled／显式宿主 Clang、直接 C 测试入口、五个现有 target 的适用编译路径。 |
| 错误与路径 | 缺失／不可加载插件、错误工具版本／架构、无效链接器、LFS 指针未恢复、空格路径、搬移、纯编译不传链接参数、空增量构建。 |
| sanitizer | 同一 Release 插件支持普通及 UBSan 使用方；macOS 两条测试链接路径均实际使用补丁 LLD，真实 UB 仍能被检测，关闭 UBSan 后不残留测试工具依赖。 |
| 发行 | 归档含对应 host 插件及头文件，签名后实际加载，安装后能编译 bin／lib；发行目录不包含补丁 LLD。 |
| 回归 | 沙箱外完整 `make test`，保留所有原断言；原生平台结果与交叉编译、转译执行分别记录。 |

本阶段保留 S11 的现象与 release 复现，不把它作为必须已修复的验收条件，也不删除
相关源码。S11 后续单独验收 release std／FCTS、原生异常控制流、runtime 交接及成本。
插件加载可能增加编译时间，应记录构建耗时；如发现新增的程序运行开销，先记录分析，
由人工审定后继续，不以工具链接入为理由跳过成本决策。

## 6. Review 决策与实施 Todo

以下是拟议方案，不代表本轮获得了修改代码或既有测试的授权。

| 编号 | 待 Review 内容 | 建议 |
| --- | --- | --- |
| R01 | 接入范围是否包含开发布局、发行组装、CI 工具获取和普通 macOS LLD 入口 | 包含；这些是 driver 接入在本地、CI、发行后都能工作的必要路径。普通 macOS LLD 补齐标准别名及选择，保留原版二进制。 |
| R02 | macOS UBSan 选择补丁链接器的方式 | 按 §4.1 修改现有测试 Makefile／CI 配置，使用 Clang 的 `COMPILER_PATH` 和仅用于链接的 LLD 参数；不新增 Feng API 或环境变量。 |
| R03 | 既有测试的工具调用及发行布局适配 | 保留直接调用 C 编译器的方式，只适配已有测试编译辅助函数、路径／归档／签名资源预期；列明涉及文件和保留断言。旧 LSDA 发码与行为断言迁移留到 S11。 |

按以下顺序交付；遇到未确定的问题先记录、再分析，涉及范围或取舍时由人工决策。

- [ ] T01：Review 本文 R01–R03；实施前明确既有测试文件及每处迁移内容。
- [ ] T02：更新构建／发行主规范的工具链消费边界；协议与工具维护规则继续引用插件主文档。
- [ ] T03：补齐开发插件布局、host 产物定位与错误处理；bin／lib 共用参数构造并实际加载插件。
- [ ] T04：按批准方案接入普通 LLVM LLD；测试配置使用现有 Clang 环境选择补丁 LLD，保证编译／链接参数分离。
- [ ] T05：补齐发行组装、插件依赖与签名后加载验证；更新既有 toolchain 预构建发布内容。
- [ ] T06：按批准范围适配已有直接 C 测试辅助函数；接入 macOS UBSan 两条链接路径及 CI LFS 获取。
- [ ] T07：新增 §5 的接入覆盖，核对原用例和断言未减少；所有失败先记录，再分析、解决。
- [ ] T08：在沙箱外执行全量 `make test`，完成各平台实际加载／执行与发行搬移验证，记录边界。
- [ ] T09：回填结果与编译耗时，输出英文 commit message，等待人工 Review，不自动提交。
- [ ] T10：本阶段验收后，按 S11 文档另行开始 Codegen／runtime 改造。
