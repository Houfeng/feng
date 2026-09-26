# LLVM C EH 插件与测试链接器接入方案

## 1. 状态与目标

状态：2026-09-24 本地接入及测试工具目录迁移已完成，macOS 全量回归通过，等待人工 Review。
预构建归档发布与原生 CI 验证尚未执行，具体边界见 §8。
实施遵循用户要求：复用现有代码、不扩大范围，不确定事项由人工决策。
用户已提交独立插件、LLD 补丁及预构建产物，提出先完成工具链接入，再修复 S11。
本文把接入作为独立阶段；实施授权及既有测试边界见 §6。

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
| `.github/workflows/release.yml` | checkout 使用 `lfs: false`，随后恢复完整 `toolchain` 预构建包；测试工具并入该目录后共用恢复步骤，无需单独 LFS 下载。 |
| `scripts/toolchain-prebuilt-publish.sh` | 归档整个 `toolchain`，已有机制可以携带插件；须实际发布含插件的新预构建包，不能把源码提交等同于 CI 已取得新产物。 |

macOS driver 当前未显式选择 LLD，bundled LLVM 只有 `ld.lld -> lld`，没有
`ld64.lld`。按已确认的 LLVM 编译器／链接器配套要求，接入还须补齐普通 macOS
链接路径，不能只处理 UBSan 后仍让普通 Feng 构建默认调用 Apple ld。

上述事实支持分阶段实施，但“插件能加载”与“Feng 异常后端已迁移”是两个验收结果。
本阶段现有 Feng C 尚无协议标记，默认全量测试通过也不能证明 S11 已修复。

## 3. 插件接入方案

### 3.1 定位与开发／发行布局

这里的“布局”指工具文件的目录位置、开发软链接和发行包内容，不涉及运行时数据结构。
开发侧增加一个插件子目录软链接；发行侧将对应 host 的预构建插件目录复制进安装包，
使两种环境中的 driver 都能按同一相对路径定位插件。

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

发行时复制对应目录，同时携带许可证。插件按运行 Clang 的 host 选择，交叉编译不按目标 CPU
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

使用已提交的 `toolchain/test_tools/lld/macos-arm64/bin/ld64.lld`，只在 macOS UBSan 测试
链接阶段启用，排除已知 Mach-O 展开信息错误对测试的干扰；UBSan 插桩仍由 Clang 负责。
Release 插件本身保持不带 sanitizer 插桩；被编译程序是否启用 UBSan
由测试配置决定，两者不混用。补丁 LLD 不替换 bundled LLD，也不进入 Feng 发行包。

UBSan 的 `FENG_CC` 参数传入 `make check-clang` 所检查命令的绝对路径，避免测试
启动登录 shell 后重设 `PATH`，把同名 `clang` 重新解析为 Apple Clang 等其他版本。

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
`toolchain/test_tools/lld/macos-arm64/bin` 的绝对路径。Clang 在 `-fuse-ld=lld` 下从该目录
找到 `ld64.lld`；环境由 Feng 的子进程继承，无需新增 `FENG_LD` 或 driver 测试选项。
测试配置须先验证补丁文件的可执行性、版本，并确认实际选择的链接器；不能依赖缺失目录
下的 Clang 回退。普通阶段不新增该环境覆盖，生成 C 使用 bundled Clang 及其原版
LLD。既有参数记录包装器保留包装前的编译器选择：普通为 bundled，UBSan 为已指定的
host Clang，不再固定转发给 `cc`；日志格式和用例断言不变。

2026-09-24 本机 LLVM Clang 22.1.8 的 `-###` 命令展开验证：设置上述环境并用
`-fuse-ld=lld -fsanitize=undefined` 时，最终链接命令指向补丁 `ld64.lld`；同一环境
下的 `-c -Werror -fsanitize=undefined` 不生成链接命令、无未使用参数错误。
相反，在 `-c -Werror` 中直接加入 `--ld-path` 或 `-fuse-ld=lld` 会报未使用参数。
该验证只证明参数与工具选择，正式实施仍须执行完整编译、链接及运行回归。

测试配置按 host 和 sanitizer 阶段注入上述环境，driver 本身不判断“是否正在跑测试”，
也不负责定位、下载或构建补丁 LLD。保留全部 UBSan 检查，不通过关闭 function
sanitizer、减少用例或换成发行包的不完整 sanitizer 资源取得成功。

### 4.2 本地和 CI

本地复用 `toolchain/test_tools/` 中的预构建工具。工具链发布脚本归档整个 `toolchain/`，
现有下载脚本恢复该目录；测试工具因此共用已有预构建 Release 与恢复步骤，普通 CI
不再单独下载测试工具的 LFS 实体文件。发布工具链预构建归档时仍需恢复 LFS 内容。
不新增发布／下载脚本，不在 CI 源码重建 LLD；macOS 测试前继续检查补丁版本、
可执行性与实际链接器选择。构建记录的存放规则见[插件开发方案 §7](./c-ir-llvm-exception-plugin-dev.md#7-手工构建与分发流程)，测试不依赖预构建目录中的记录文件。

仓库 `toolchain/` 包含发行工具和测试工具。Feng 发行包按[发行规范](./feng-release-and-install.md)
只复制当前 host 的 LLVM、插件及目标 sysroot，排除 `test_tools/` 子目录；因此无需
修改现有发行组装逻辑。补充预构建打包／恢复和发行排除的回归，保留原用例及断言。

普通配置仍验证原版 LLD；Linux 流程不注入 macOS 工具。测试阶段结束后不得把
显式补丁链接器选项带入普通构建或发行流程。预构建恢复或测试工具校验失败时，应在
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

整体接入已获开始实施授权。用户已明确批准 R03 的既有测试参数补齐，要求“不改用例
语义，仅补齐参数”；该授权不包括既有测试夹具、源码语义、断言或性能门槛的变更。

| 编号 | Review 内容 | 建议／决策 |
| --- | --- | --- |
| R01 | 接入范围是否包含开发布局、发行组装、CI 工具获取和普通 macOS LLD 入口 | 包含；这些是 driver 接入在本地、CI、发行后都能工作的必要路径。普通 macOS LLD 补齐标准别名及选择，保留原版二进制。 |
| R02 | macOS UBSan 选择补丁链接器的方式 | 按 §4.1 修改现有测试 Makefile／CI 配置，使用 Clang 的 `COMPILER_PATH` 和仅用于链接的 LLD 参数；不新增 Feng API 或环境变量。 |
| R03 | 既有测试参数补齐 | 已获人工批准：保留直接调用 C 编译器的方式，仅在已有测试编译辅助函数中补齐必要参数，不改用例语义、夹具或原断言。旧 LSDA 迁移仍留到 S11。 |

按以下顺序交付；遇到未确定的问题先记录、再分析，涉及范围或取舍时由人工决策。

- [x] T01：R01–R02 随开始实施获批；R03 已批准，仅补齐下列既有测试的编译参数。
- [x] T02：更新构建／发行主规范的工具链消费边界；协议与工具维护规则继续引用插件主文档。
- [x] T03：补齐开发插件布局、host 产物定位与错误处理；bin／lib 共用参数构造并实际加载插件。
- [x] T04：按批准方案接入普通 LLVM LLD；测试配置使用现有 Clang 环境选择补丁 LLD，保证编译／链接参数分离。
- [x] T05：补齐发行组装、插件依赖与签名后加载验证；确认现有预构建发布脚本会归档新增资源。
- [ ] T05-P：提交后由维护者发布包含插件、测试工具及 macOS 链接器别名的新 toolchain 预构建归档，再由原生 CI 验证。
- [x] T06：按批准范围适配已有直接 C 测试辅助函数；接入 macOS UBSan 两条链接路径。
- [x] T06-M：按人工批准迁移至 `toolchain/test_tools/`，复用预构建发布／恢复；补齐归档包含、发行排除验证并重新执行全量回归。
- [x] T07：新增 §5 的接入覆盖，核对原用例和断言未减少；所有失败先记录，再分析、解决。
- [x] T08：在沙箱外执行全量 `make test`，记录本地各平台实际加载／执行、发行搬移结果及未完成的原生 CI 边界（§8）。
- [x] T09：回填结果与编译耗时，输出英文 commit message，等待人工 Review，不自动提交。
- [ ] T10：本阶段验收后，按 S11 文档另行开始 Codegen／runtime 改造。

## 7. 实施记录

已批准的既有单测参数改动：`test/codegen/test_codegen.c`、
`test/codegen/test_defer_generic_context.c`、`test/debug/test_debug.c` 的生成 C
编译命令补齐同一 host 的插件加载与头文件搜索路径；保留 `-c`、`-Werror`、优化级别、
目标选择、源码及全部断言。Makefile 复用现有 host 变量和测试目标传递所需参数。

| 编号 | 问题、分析与处理 |
| --- | --- |
| I01 | `scripts/run_release_scripts.sh:create_source_root` 的虚拟发行源只含 LLVM／sysroot，不含插件目录或 macOS `ld64.lld`。发行组装补齐插件的必需输入后，该旧夹具不完整。用户于 2026-09-24 批准补齐三 host 的插件占位文件、头文件、许可证与来源文件，以及 macOS 链接器别名；保留全部测试操作及断言。 |
| I02 | 新集成测试首次链接失败：复用整个 CLI 测试对象集合带入了选项解析器，其 `feng_cli_print_usage` 原由旧测试主文件提供。新测试只使用 driver，应收敛链接依赖为 driver、CLI 公共路径、archive／platform 及必要解析对象，不增加产品 API 或无关 stub。 |
| I03 | 新集成测试的最小 Feng 源码先后因缺少 `module`、入口签名不合法触发 SE0901／AE0909／AE0910。按现有语法补齐新夹具的模块声明和 `main(args: string[]): void`；不修改编译规则或既有用例。 |
| I04 | 新增“不兼容插件接口”负例的测试动态库使用 bundled Clang／LLD 时因未传 SDK，找不到 `libSystem`。该辅助编译也显式传入测试已取得的 macOS SDK；保持测试动态库不带 sanitizer，不改变 driver 或工具链行为。 |
| I05 | 首轮全量回归在 UBSan `cli-project-tests/default_path` 失败：Clang 加载插件缺少 LLVM 符号。实测当前 shell 的 `clang` 是 Homebrew 22.1.8，既有用例的 `bash -lc` 则将同名命令解析为 Apple Clang 17。将现有 UBSan `FENG_CC` 参数固定为版本检查已选定的编译器绝对路径，不修改用例操作或断言。 |
| I06 | 第二轮全量回归中 UBSan 阶段全部通过；普通阶段 std／FCTS／发行／新增集成测试通过后，`test_cli.c:2996` 的原生库链接包装器报 `cc: invalid linker name ... -fuse-ld=lld`。实测既有 `create_logging_cc_wrapper` 固定 `exec cc`，转到没有 LLD 的 Homebrew 安装。用户于 2026-09-24 确认普通阶段使用 bundled Clang；包装器在覆盖 `FENG_CC` 前复用现有 CLI 工具选择取得原本编译器，再记录并转发参数。撤销普通阶段临时增加的 `COMPILER_PATH`，UBSan 仍使用 host Clang／补丁 LLD；全部日志格式和原断言保持不变。 |
| I07 | Linux 容器准备时，ARM64 镜像没有 `/usr/bin/time`，改用 shell 计时后普通／UBSan 全部接入用例通过。x64 Rosetta 的 GNU tar 解压返回 `Function not implemented`，改用 `cp` 准备源码后 Feng 构建通过，但新测试的归档解压也触发同一环境限制；该镜像没有 zip／unzip。保留归档用例，不添加 Rosetta 特判或安装工具。独立 driver 验证已通过 bin／lib、O0／O2、普通／UBSan 的真实异常执行及五 target 交叉编译；完整归档验证留给原生 CI，不能标记为全部通过。 |
| I08 | 维护者重新发布预构建归档后，macOS CI 在 `test-sanitize` 的链接器可执行性检查失败。补丁 `lld` 首次加入时 Git 模式即为 `100644`，目录迁移保留该模式；本地文件为 `0755`，且 `core.filemode=false` 隐藏了差异。从提交重新还原完整 LFS 实体文件后，文件和 `ld64.lld` 别名均存在，但 `test -x` 失败，确认是执行权限遗漏。此前本地全量回归和虚拟归档测试未覆盖实际产物的 Git 模式。本次仅将 `toolchain/test_tools/lld/macos-arm64/bin/lld` 的 Git 模式改为 `100755`，保持二进制内容、别名、脚本和测试用例不变；验证从 Git 索引重新检出、预构建打包及解包后的权限、校验和和版本，再执行沙箱外全量 `make test`。提交后须重新发布包含权限修复的新预构建归档，并重跑 macOS CI。 |
| I09 | 维护者要求 `toolchain/llvm-c-eh/` 与 `toolchain/test_tools/lld/` 不再包含 `build-info.txt`、`SHA256SUMS`，随后确认插件的 `source-files.sha256` 也不随工具链保留，已有文件均须删除。移除三平台插件的六份记录及补丁 LLD 的两份记录；维护脚本仅在 `build/` 保留这些记录，插件安装时清理旧同名文件。发行组装／安装验证不再要求插件构建记录；macOS UBSan 去除对已删除清单的读取，保留可执行性、补丁版本及实际链接器选择检查。二进制、协议头文件、许可证及其他发行 component 的校验清单不变。维护者已批准同步移除发行测试夹具中的两类插件记录，并增加三平台发行包不含这些记录的断言，保留原测试步骤及断言；完成专项验证和沙箱外全量回归后记录结果。 |

2026-09-24 用户批准将测试工具迁至 `toolchain/test_tools/`，更新路径、移除独立 LFS
获取步骤和旧目录规则，并补充归档包含／恢复及发行排除验证。迁移不重编译工具；
发布、下载和发行组装主脚本保持原有机制，具体边界见 §4.2。

实现复用记录：

- driver 使用 `feng_platform_detect_host_platform`、现有动态库后缀函数及
  `feng_cli_require_install_path`，未增加路径 API、driver 选项或环境变量。
- `bin`／`lib` 在分支前共用插件参数；Makefile 的三个开发子链接仍保持原有只读增量规则。
- 项目编译及本地依赖构建会调用现有 direct compile／driver；每次原生编译读取当前插件，
  不新增原生产物缓存，也不改变预编译 `.fb` 的复用或 FT 版本。
- macOS 签名脚本现有 Mach-O bundle 识别可覆盖 `llvm_c_eh.dylib`，无需修改。
- 预构建发布脚本已归档完整 `toolchain`，无需修改。提交后须沿用
  `toolchain-prebuilt/*` tag 流程发布包含插件、`test_tools` 及 `ld64.lld` 别名的新归档；
  本轮不会自动创建 tag、上传 Release 或触发远程 CI。

## 8. 验收记录（2026-09-24）

| 环境／入口 | 实际结果 |
| --- | --- |
| macOS ARM64，沙箱外 `make test` | 测试工具迁移后重新执行，退出码 0；UBSan 和普通阶段全部通过，总耗时 660.08 秒。两阶段分别为 std 607/607、FCTS 1508/1508，失败和跳过均为 0；CLI、DAP、编译器单测、性能约束、空增量及发行脚本测试通过。 |
| macOS 普通 CLI | 清除测试进程的 `FENG_CC`、`FENG_CC_FLAGS`、`COMPILER_PATH` 后，`test_cli` 单独运行通过。参数记录包装器复用原本的 bundled Clang；未修改原断言。 |
| macOS 插件集成 | 全量测试的两个阶段均通过 `test/cli/llvm_c_eh.sh`：真实 driver 的 bin／lib、O0／O2、五 target 交叉编译、空格路径、归档解压搬移、签名后加载、缺失／错误插件和无效链接器。无协议生成 C 的 LLVM IR／汇编对照一致。 |
| macOS UBSan | 版本、校验和及实际链接命令确认使用 `test_tools` 补丁 LLD；真实 UB 检测、异常处理体、间接调用、恢复和终止模式通过。Release 插件本身未重编译或增加 sanitizer 插桩。 |
| 测试工具迁移与分发 | 补丁 LLD 及构建记录／许可证校验和与迁移前一致；新路径由 `toolchain/**` LFS 规则覆盖。真实发布脚本对测试夹具生成的预构建归档可完整恢复测试工具，保留文件内容、可执行位及 `ld64.lld` 别名。三 host 发行归档均排除测试工具；专项测试与全量回归均通过。 |
| macOS CI 执行权限修复（I08） | Git 索引模式已改为 `100755`，LFS 指针 blob 与二进制内容未变。从索引重新检出实际产物，经现有发布脚本打包、解包后，`ld64.lld` 可执行，别名、全部校验和及补丁版本验证通过。沙箱外重新完整执行 `make test`，退出码 0，耗时 581.00 秒；UBSan 和普通阶段分别为 std 607/607、FCTS 1508/1508，失败和跳过均为 0，编译器、CLI／DAP、插件接入及发行／预构建回归全部通过。远程 macOS CI 仍待维护者提交、重新发布预构建后重跑。 |
| 维护记录清理（I09） | 已删除两个预构建目录内的 8 份记录。三个 host 的插件及补丁 LLD 实际归档、解包后均不含 `build-info.txt`、`SHA256SUMS`、`source-files.sha256`；二进制内容、协议头文件、许可证、别名及 LLD 执行权限保持不变。维护和发行脚本语法检查通过；获批的三平台发行包排除断言通过。沙箱外全量 `make test` 退出码 0，耗时 581.39 秒，UBSan／普通阶段分别通过 std 607/607、FCTS 1508/1508，失败和跳过均为 0。未重建插件或 LLD；目录清理需随下次 toolchain 预构建发布分发。 |
| Linux ARM64，Ubuntu 26.04 容器原生执行 | 普通及 UBSan 插件接入用例通过，包含真实异常执行、五 target 编译及归档搬移。隔离源码构建加普通接入测试耗时 23.075 秒；未在该容器执行完整 Feng `make test`。 |
| Linux x64，Ubuntu 26.04 容器／Rosetta | Feng 构建及独立 driver 的 bin／lib、O0／O2、普通／UBSan 真实执行、五 target 编译通过。完整接入脚本在归档解压处受 I07 环境限制，未通过该项；未作为原生 x64 或全量回归结果。 |

新增集成测试复用独立插件的协议夹具和测试 runtime，没有提前改造 Feng 的异常发码。
既有测试包含已批准的编译参数、发行夹具资源和参数记录包装器调整，以及预构建打包／
恢复和发行排除覆盖；用例语义、原断言及性能门槛均保留。Codegen、runtime、ABI、FT
和插件源码没有变更。

发行组装／安装／签名脚本使用既有测试夹具验证；新增集成测试对真实插件执行归档
搬移及 macOS ad-hoc 签名后加载。未发布实际 Feng 发行包，也未执行真实公证或远程 CI。
维护者提交后仍须完成 T05-P；新归档必须包含 `toolchain/llvm-c-eh`、
`toolchain/test_tools` 和 macOS `ld64.lld` 别名，否则 CI 恢复旧工具链时会明确失败。
S11 修复继续按其独立文档实施。

## 9. 追加 ASan（2026-09-26）

用户批准仅追加参数开启 ASan：在现有 `test-sanitize` 的编译、链接和
`FENG_CC_FLAGS` 中保留 `-fsanitize=undefined`，追加 `-fsanitize=address`，
同一阶段同时启用 UBSan 与 ASan。插件集成脚本的直接编译／链接同步传递该参数；
普通阶段、测试源码和断言、优化级别、编译器及链接器选择保持不变。
参数接入本身不修改产品代码、runtime、ABI、插件或工具链产物，
不增加 sanitizer 屏蔽选项；下述测试发现的问题按用户后续批准单独修复。

验收：沙箱外执行完整 `make test`，记录实际结果；若失败，先记录、分析，
超出参数调整范围的修复由人工决定。

首次执行退出码为 2：ASan＋UBSan 的 archive、lexer、parser、semantic、runtime
及原生 EH 契约检查通过；`test_codegen` 在
`test_generic_sibling_constraint_codegen` 中报告 `heap-use-after-free`，
读取位置为 `cg_register_generic_spec_instance_shell`。ASan 的释放栈显示同一函数
递归注册时发生 `realloc`。完整原始日志保存在
`third_party/llvm-c-eh/temp/asan-enablement/make-test.log`。

源码与符号定位确认：函数在 `codegen.c:12833` 保存 `UserSpec *s`，
`13025` 行递归注册父 spec 时由 `12822` 行扩容 `cg->user_specs`，旧存储已释放；
递归返回后，`13036` 行仍通过原局部指针读取 `s->form`。现有全局引用刷新未更新
该 C 局部变量。定位记录为同目录的 `codegen-locations.txt`。
这是编译器执行期间的内存访问错误，追加参数时未改动该代码。用户随后批准修复，
方案与验证见[泛型 spec 注册寿命修复](./feng-spec-registration-lifetime-bugfix.md)；
保留 ASan 及原测试。

注册寿命修复后的第二次全量回归通过全部 Codegen 用例，随后发现泛型方法
重复源码断点，普通阶段尚未执行。用户批准先修复该问题，见
[sanitizer 调试位置修复](./feng-sanitizer-debug-location-bugfix.md)。
该修复补齐共享方法与 fit 的既有调试上下文，不改代理或原断点断言。

两处修复后，完整 `make test-normal` 退出 0。sanitizer 下 Codegen、DAP/LSP、
std 607/607、FCTS 1619/1619、性能门槛及插件集成均通过，但当时完整 `make test`
在 CLI release 符号清理断言处失败：原断言扫描全文件字节，误将 ASan
全局元信息中的诊断名称当成残留符号。用户随后批准优化，已将该用例的 6 处
符号断言改为读取真实符号表，保留源码、运行结果及普通字符串断言。

最终在沙箱外重新执行完整 `make test`，退出 0，ASan＋UBSan 与普通阶段均通过。
日志为 `third_party/llvm-c-eh/temp/asan-enablement/make-test-symbol-table.log`；
过程与验证记录见上述调试位置修复文档。本地验收完成，等待人工 Review。

### 9.1 统一泄漏检测（2026-09-26）

用户确认全部修复，已批准下述 22 个既有测试的资源清理、Parser 新用例注册，
以及关联文档中的 argv runtime 修复。保留原有用例源码、行为断言与顺序。

上述 macOS 回归使用了 ASan 的平台默认配置，未开启泄漏检测，不能作为
LeakSanitizer 验收结果。[LLVM 文档](https://clang.llvm.org/docs/AddressSanitizer.html#memory-leak-detection)
说明 Linux 默认开启泄漏检测，而 macOS 需要显式设置 `detect_leaks=1`。
后续 Linux CI 暴露的 Parser 泄漏及跨平台复现见
[Parser LeakSanitizer 修复记录](./feng-parser-leak-sanitizer-bugfix.md)。

用户要求各平台一致开启。统一在 `Makefile` 的 `test-sanitize` 目标导出
`ASAN_OPTIONS`：保留已有配置，在末尾追加 `detect_leaks=1`，由测试命令、
递归 Make 和其子进程继承。本地与 CI 使用同一入口，不按平台分别设置，
不向普通测试阶段额外导出该选项，也不改变编译／链接参数或既有用例。

验证要求：在沙箱外运行 `make test`，记录实际失败位置；在已知 Parser
泄漏修复前，不得将预期的泄漏报错记为回归通过。

继续检测时，Semantic 与 Codegen 也报告资源释放遗漏，分别记录于
[Semantic 修复文档](./feng-semantic-leak-sanitizer-bugfix.md)和
[Codegen 修复文档](./feng-codegen-leak-sanitizer-bugfix.md)。
Runtime 测试另外报告 373 字节／10 次分配：四个用例调用 `feng_string_literal`
创建 immortal 字符串后没有销毁测试夹具。该 API 明确使用 immortal 引用计数，
普通 `feng_release` 不应释放它；产品 runtime 语义保持不变。已在测试结束时
回收测试自行分配的内存，嵌套实参先保存指针以便清理。涉及
`test_string_literal_immortal`、`test_string_concat`、
`test_string_utf8_length_contract`、`test_expression_equal_contract_uses_descriptor`，
均在 `test/runtime/test_runtime.c`；修改已获人工批准，原断言保留。

CLI 继续检测发现动态 `argv` 被错误构造成 immortal 字符串。根因、同一产物
在不同路径下的执行记录及已批准的 runtime 方案见
[main 参数生命周期修复](./feng-argv-string-lifetime-bugfix.md)。
截至上述 argv 定位阶段，完整 `make test` 尚未通过，不能将当时的针对性结果作为整体交付。

#### DAP 与 LeakSanitizer 的工具限制

Linux ARM64 的完整回归已通过 Archive、Lexer、Parser、Semantic、Runtime、
Codegen、Debug。修正新增 argv 用例的入口类型后，CLI 的 argv 四种组合及
S11 跨包矩阵通过，随后 DAP try 断点用例运行到进程退出时报：
`LeakSanitizer has encountered a fatal error`，紧接着明确提示
`LeakSanitizer does not work under ptrace (strace, gdb, etc)`，退出码为 1。
该结果不是“检测到泄漏”：Linux LSan 需要借助 ptrace 暂停被检查线程，
LLDB 已占用同一进程的调试接口。上游对此限制有
[明确记录](https://lists.llvm.org/pipermail/llvm-commits/Week-of-Mon-20161010/397398.html)。
日志保留于 Linux 隔离工作树 `/repo/temp/dap-try-breakpoints-J1JzI2/dap.log`。

2026-09-26 用户批准分开执行：仅在 DAP 测试的被调试进程中关闭 LSan，保留 ASan、
UBSan 和全部原断点／退出码断言；同一测试程序在调试器外独立执行泄漏检查。
编译器、测试进程、非调试执行的生成程序继续 `detect_leaks=1`。不在 Feng
DAP 产品实现中屏蔽 sanitizer，不降低整体检查标准，也不按具体语言用例特判。
在公共 DAP 测试入口实现统一配置，并核对各程序的实参、工作目录和
预期退出码；协议模拟程序不作为真实被调试程序执行。

实现位于测试的 `dap_test_launch_native`：sanitizer 阶段先 fork/exec 原产物，
保持同一工作目录和无附加实参，显式开启泄漏检查并核对原退出码；随后仅向
DAP launch 的 debuggee 环境追加 `detect_leaks=0`。协议模拟测试继续使用原入口。
原 `_exit(7)` 用例仍按原语义直接退出，无法执行退出时的 LSan 扫描；它保留
ASan／UBSan 执行检查和退出码断言，正常退出的对照用例执行完整泄漏检查。
不改变 `_exit` 或 finalizer 行为来迁就检测器。

#### 后续项目构建检测

继续执行 std/FCTS 时，两平台的 CLI 均报告 52 字节／3 次分配泄漏，分配点为
`feng_cli_compile_driver_invoke` 构造 `-l` 参数。根因：先分配 flag，再检查
该库是否已由包内静态库满足；满足时直接 continue，遗漏释放。将分配移至
该检查之后，仅在确实需要生成 `-l` 时创建；复用原有参数列表与释放路径，
不改变链接参数、库解析或程序运行行为。既有项目及 std/FCTS 回归覆盖该路径。

Linux FCTS 的 1619 项行为断言通过，CLI 退出另报 180 字节／5 次分配，
其中 52 字节为上述参数问题，另外两处各 64 字节是符号图构建时的诊断对象。
符号导出的既有名字解析回退可在最终成功时仍留下诊断对象；direct compile
只在失败分支销毁 `symbol_error`。在成功离开该资源作用域时也调用已有析构
函数，保留原有名字解析、FT 内容及诊断输出行为。

Linux 首次后续 std 运行另有 12 个字符宽度断言失败；隔离容器尚未设置 CI
采用的 `LANG=C.UTF-8`、`LC_ALL=C.UTF-8`。先核对 locale 并按 CI 环境重跑，
不据此改动 TUI 实现或断言。

核对确认容器初始 locale 为 POSIX。补齐上述环境并应用两处 CLI 清理后，
macOS ARM64 与 Linux ARM64 的 std 均为 607/607、FCTS 均为 1619/1619，
失败及跳过为 0，ASan／UBSan／LSan 无报告，两条重跑命令退出码均为 0。
日志：`third_party/llvm-c-eh/temp/parser-leaks/library-lsan-fixed.log` 与
`linux-arm64-library-lsan-fixed.log`；这些是专项验收，不能替代完整 `make test`。

macOS 的完整普通阶段通过 91 项 smoke、CLI direct/project 后，在
`run_cli_init_bundled_packages.sh` 中连续四次出现复制出的 `feng init` 被
SIGKILL 终止，后续项目文件缺失。该阶段未开启 sanitizer，未据此改动产品
或测试路径；已请求维护者核对本机安全软件。日志为同目录
`make-test-normal.log`，普通阶段也暂不能记为完整通过。

另外尝试为 Linux x64 Rosetta 准备 8 GB 独立容器，GNU tar 的文件 stat／创建
操作返回 `Function not implemented`，未进入编译测试。临时容器已移除，既有
容器恢复停止状态；不修改脚本添加 Rosetta 特判，也不把该尝试记为 x64 验收。

Linux ARM64 随后执行完整 `make test-normal`，退出码 0；包括 91 项 smoke、
std 607/607、FCTS 1619/1619、编译器单测、CLI／LSP／DAP、增量构建、性能
约束、发行与工具链获取测试。完整日志为
`third_party/llvm-c-eh/temp/parser-leaks/linux-arm64-make-test-normal.log`。

该阶段剩余交付项：用户已批准二元操作临时值的必要清理及 DAP 的 LSan 分离执行；
macOS SIGKILL 按用户要求先重试，不因疑似环境问题随意修改代码。全部解决后必须重新执行
完整 `make test`，不能合并专项通过结果声称该入口已经通过。

#### 全量 CLI 后续发现（2026-09-26）

DAP 分离后，Linux 全量 CLI 越过原阻塞点，随后在既有跨包泛型环回收用例失败。
生成程序报告 400 字节／7 次分配，包含未回收的环及断言失败路径中的 TestState；
需要先核对生命周期断言、描述符和回收触发，再决定修复位置，不能直接归因于 LSan。
宿主 CLI 退出另报 9,846 字节／11 次分配，分别指向 DAP stackTrace JSON 改写
和依赖包读取错误信息；先核对各资源的转移与统一释放路径，保留所有既有断言。
日志：`third_party/llvm-c-eh/temp/parser-leaks/linux-arm64-make-test-latest.log`。

已确认两处宿主泄漏：stackTrace 连续改写 frames 与 totalFrames，最终只移交后一份
JSON，却同时清空两份指针，导致前一份丢失；改为仅清空实际移交的指针，另一份
由原 cleanup 释放。依赖包读取失败时，`set_errorf` 已复制 ZIP 错误文本，但调用方
提前返回而未释放原文本；在两条错误路径复制诊断后释放原字符串。两项均不改
协议内容、诊断文本或原测试断言，由既有失败／改写用例继续验证。

跨包环的原因已定位：`cyc_collect_nodes` 无条件跳过 imported-package 模块，
而这些模块的合成 AST 已包含 FT 提供的字段类型。消费者具化的两端描述符因此
均生成 `is_potentially_cyclic=false`，根本不进入既有回收路径；本轮之前的 HEAD
也保留此跳过逻辑。建议把导入类型纳入同一个字段图／SCC 分析，复用现有标记和
描述符字段，不增加 runtime API、ABI、FT 或新的回收算法。有环类型将恢复必要
的候选登记和回收成本，无环类型仍由图分析排除；此运行成本提交人工确认。
用户随后要求继续修复所有问题，包含上述已说明的必要回收成本，现按授权实施。
新增私有字段测试的初稿遗漏 `seal`，触发既有 AE0327 可见性规则；补齐测试
声明后继续验证，不修改可见性语义。
补充源码／FT、泛型／非泛型、自环／互环、数组边／无环对照测试，并用独立的
非零退出失败机制检查行为。既有 CLI 用例的 std.test.assert 在测试框架外只记录
失败，不使进程失败，故原来的退出码断言没有暴露回收遗漏；不私自改旧用例语义。

另一个 TestState 泄漏已用不依赖 std 的程序复现，来自描述符布局 tuple 重复
默认初始化；构造中途异常也存在独立遗漏，详见
[Tuple 初始化生命周期修复](./feng-tuple-initialization-lifetime-bugfix.md)。
重复初始化已修复，构造期清理随后按用户“继续修复所有问题”的授权完成。

两处宿主释放修复后，Linux ARM64 的原依赖包错误用例、DAP stackTrace 列映射
用例均通过 ASan／UBSan／LSan。另以临时测试入口执行跨包泛型环用例之后的
原 CLI 用例，全部通过；保留全部原用例源码和断言，这些专项结果不代表完整
CLI 或 `make test` 已通过。日志为 `linux-host-focus-run.log`、`linux-cli-tail.log`。
修复导入环前的最近一次完整 `make test` 在 macOS／Linux 均停在上述跨包泛型环用例；
macOS 已越过 argv 和 DAP，随后独立最小程序重试仍有 SIGKILL，未据此改代码。

导入类型纳入原 SCC 分析后，源码／真实 FT 往返用例在两平台 Semantic 全套
通过，包含私有字段、数组边、泛型、自环／互环和无环对照。Linux 上原跨包
泛型环 CLI 用例已通过 LSan；新增源码隐藏包默认／release 四种组合也通过，
以独立硬失败断言确认显式创建的对象全部且仅释放一次，不依赖 std.test 上下文。
专项日志为 `linux-remaining-focus.log`；继续执行最终完整 `make test`。

#### 最终验收（2026-09-26）

全部修复及新增用例完成后，在沙箱外分别从完整入口重新执行，不以分段重跑
代替全量结果：

| 环境 | 命令 | ASan／UBSan／LSan 阶段 | 普通阶段 | 退出码 |
| --- | --- | --- | --- | --- |
| macOS ARM64，Clang 22.1.8 | `make test` | 通过 | 通过 | 0 |
| Apple Container Linux ARM64，Clang 22.1.8 | `make test` | 通过 | 通过 | 0 |

两平台各阶段均通过 smoke 91/91、std 607/607、FCTS 1619/1619，以及对应的
编译器单测、CLI／LSP／DAP、性能门槛和插件集成检查；普通阶段另通过增量构建、
发行脚本、签名流程夹具和工具链预构建获取检查。没有新的 sanitizer 失败。
DAP 按上文已批准的方式分离泄漏检查，所有既有行为断言保留。

新增覆盖包含 Parser 56 种签名清理场景反复执行、动态 UTF-8／argv 所有权、
二元表达式正常及异常清理、固定／共享体 tuple 构造、源码／FT 导入的环分析、
源码隐藏包默认／release 四种组合下的自环／互环回收。泛型、spec、value、
数组、闭包、嵌套、提前返回和无环对照均包含在相关矩阵中。

本次 macOS 完整重试越过了此前的 SIGKILL 位置，包括复制发行布局后的
`feng init`；没有为此调整代码、测试路径或安全配置。Linux x64 的 Rosetta
环境准备失败仍按上文保留，未计为本地全量通过，留待原生 CI 验证。

完整日志位于 `third_party/llvm-c-eh/temp/parser-leaks/`：

- `make-test-delivery.log`：macOS 完整回归。
- `linux-arm64-make-test-delivery.log`：Linux ARM64 完整回归。
- `linux-arm64-container-logs.tar`：停止本轮临时容器前保存的排查日志。

实现及本地验收完成，等待人工 Review；未自动提交。
