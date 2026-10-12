# LLVM 23 工具链升级

状态：2026-10-12 人工批准升级；不必要的额外改动已撤回，进入逐文件 Review。
按人工要求，`toolchain/` 暂时恢复升级前版本；Review 通过后再执行维护脚本
重新生成制品并全量回归。下文已完成的实测属于清理前记录，不代表当前验收完成。

## 原因与范围

Xcode 27 的 SDK 在 `libSystem.tbd` 中增加 `arm64e.x1`。仓库原版 LLVM
22.1.8 的 LLD 无法解析该架构名，连最小纯 C 程序也会链接失败；同一程序
使用 SDK 26.5 可以链接。该问题不由 text-scan 的 std／runtime 优化引入。

[LLVM 23.1.3 正式发行](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.3)
已包含 [上游修复 #222721](https://github.com/llvm/llvm-project/pull/222721/files)。
本次升级到该版本，继续使用官方预构建包，经现有维护脚本拉取、裁剪。
不增加编译器或 SDK 包装脚本，不修改 SDK，不改变默认优化选项。
发行布局与工具来源仍以 [分发与安装规范](./feng-release-and-install.md) 为准。
本次仅升级版本，不扩充分发工具或功能。裁剪结果须与升级前逐项比较；新增文件或
运行依赖必须说明其对新版现有功能的必要性。维护 SDK 的可选依赖不能自动成为
Feng 分发依赖；不因 Homebrew 的链接配置而增加 Z3。
人工确认继续沿用 LLVM 22 的裁剪粒度：Clang 内建头文件目录仍完整保留，随
Clang 同步更新；本轮不另做头文件裁剪，不按新增文件逐项修改原有复制规则。

本次变更仅包含 text-scan 优化和 LLVM 升级。text-scan 的实现及测试范围以
[优化方案](./feng-text-scan-performance-optimize.md) 为准；本文件只记录 LLVM
升级。与两项工作无关的修复、重构或构建机制调整不得混入。除版本替换外的
适配必须说明升级必要性，不能仅凭“升级过程中遇到”就作为保留依据。
工具在各场景的选择遵循原分发规范；仓库根 `local/` 仅供下载和裁剪脚本使用，
不能通过执行命令的 PATH 或 SDK 参数让其他构建、测试流程依赖该目录。

### 额外改动的清理结果

按人工要求移除非必要改动；保留项仅限新版依赖或构建所需适配。Review 通过前
不重新生成工具链，不以此前回归通过替代清理后的验收。

| 项目 | 清理后处理 | 与升级的关系及验证依据 |
| --- | --- | --- |
| C1 | 撤回无主版本的 Homebrew formula，改为 `llvm@23` | 本机已安装 `llvm@23` 别名及对应 opt 路径；沿用原先指定主版本的方式，仍校验精确版本 23.1.3 |
| C2 | 撤回源码来源切换，继续使用 Debian orig 归档 | Debian 已发布 23.1.3 原始归档；仅更新版本、路径及 `.dsc` 中的 SHA-256，不应用 Debian 修改 |
| C3 | 人工已批准保留测试 LLD 的 `-dead_strip_dylibs` | Homebrew 23 的 LLVMSupport 链接配置带入 Z3；此前按原方式构建已在非系统动态依赖检查处失败。此选项用于移除未使用依赖，保留原发行依赖约束，重建后仍须验收 |
| C4 | 移除既有 `third_party/lld/cmake/LLVMConfig.cmake` 中本轮增加的显式系统 libc++ 链接选项 | Homebrew Clang 默认链接命令不带官方下载 SDK 的私有 libc++ 搜索路径；恢复默认链接选择并重建验证。该文件仅保留 LLVM 版本更新 |
| C5 | 移除两个插件维护脚本本轮增加的 `MACOSX_DEPLOYMENT_TARGET` 覆盖 | 恢复继承调用者环境。维护构建使用既有 macOS 26 基线，不能由脚本覆盖调用者设置；制品兼容性另行验证 |
| C6 | 撤回测试 LLD 版本校验的后缀兼容及附带重构 | Homebrew SDK 的 VCSRevision.h 未定义 LLVM_REVISION／LLVM_REPOSITORY，恢复原精确文本校验，只更新版本号 |
| C7 | EH 插件 `BranchInst` 改为 `CondBrInst`／`UncondBrInst` | LLVM 23 API 迁移，保留原条件与无条件分支判断；不扩展异常插件职责 |
| C8 | Linux LLDB 私有 Python 更新到 3.14，并从 AlmaLinux 9.8 获取 | 对应新版 LLDB 依赖；保留原初始化子集和 glibc 下限，不增加 Python 功能 |

正常构建／测试源码未新增固定的仓库根 `local/` 路径。此前新增的实际依赖来自
执行命令：macOS 回归将本地下载 SDK 加入 PATH，两个插件及测试 LLD 维护构建
将其作为 SDK 参数。该执行方式不再使用，相关 macOS 制品及回归结果须按原
工具来源重新核验。

清理后已做静态核对：两个插件维护入口等 11 个文件与 HEAD 相比仅替换版本；
测试 LLD 构建脚本除版本外只保留上述 C3；Debian 源码 SHA-256 与 `.dsc` 一致。
Shell 语法和 `git diff --check` 通过。未执行维护重建或新的 `make test`，
这些步骤按人工要求留到逐文件 Review 通过后进行。

## 实施顺序

1. 获取并校验 macOS ARM64、Linux ARM64 和 Linux x64 的同版本官方预构建。
   复用 `scripts/fetch_llvm.sh`、`scripts/trim_llvm.sh`，保留完整 SDK 与裁剪产物的区别。
   新版 Linux LLDB 依赖 Python 3.14，私有运行库及 `encodings` 同步到 AlmaLinux
   9.8 的 Python 3.14.7；两架构的库均仅依赖系统 libc／libm，最高 glibc 符号为
   2.34。继续仅分发初始化子集，不增加 Python 可执行程序或脚本功能。
2. 核实新版 LLD 的 FDE 重定位实现。测试专用 LLD 通过现有
   `scripts/build_test_lld.sh` 用同版本源码和完整 SDK 重新构建；仍有原缺陷时迁移
   原补丁，并运行既有上游 Mach-O、入口前缀、原生异常和搬移验证。
   静态 SDK 须满足测试工具既有 macOS 26 最低要求；链接时移除没有被引用的
   动态库，最终依赖检查仍要求只保留系统库，不将 SDK 的可选 Z3 等依赖带入产物。
   不增加强制选择 libc++ 的 CMake 适配；沿用已安装 Homebrew SDK 的默认选择，
   通过原有最终依赖检查验证产物。
   维护与测试入口保留原版本文本精确校验，只更新 LLVM 版本号。
   若上游已修复，先记录证据，再决定是否取消测试专用产物。
3. 用新 SDK 分别重建三个 host 的两个插件，经现有维护脚本验证 bundled Clang
   和宿主 Clang 的加载兼容性、普通／sanitizer 行为及搬移后的依赖。
   不改变异常协议、原生符号映射规则、host／target 分工或发行包布局。
   macOS 插件维护脚本继续继承调用者的部署目标；维护环境须满足原有 macOS 26
   基线，重建后检查制品最低版本，不在脚本中新增强制覆盖。
   LLVM 23 将条件与无条件分支拆为 `CondBrInst`／`UncondBrInst`；异常插件的
   保留边检查迁移到对应类型，不放宽原有 CFG 条件或关闭弃用诊断。
4. 同步维护脚本、构建版本检查、CI 宿主 LLVM 版本和当前主规范。
   历史测试与性能记录保留原版本，不能将旧结果改写为新版本结果。
5. 验证 SDK 27 的最小链接及既有异常插件集成用例，再执行完整 `make test`。
   在工程内隔离副本、Codex 沙箱外执行；macOS 安全软件拦截的失败不得记作通过。
6. 重新检查 text-scan 性能目标及其余基准的明显退化。新旧工具链结果分别记录。

升级不授权修改已有测试的行为断言。若新 LLVM 暴露兼容问题，先记录原始失败，
分析后修正实现；确需修改既有断言时另行取得人工批准。

## 验收清单

- [x] 三个 host 官方包完成校验、裁剪和工具启动检查。
- [x] 与升级前文件及动态依赖清单逐项比较；内建头文件按人工确认保持完整组件。
- [x] macOS SDK 27 的最小 C 链接与新增架构名 `.tbd` 检查通过。
- [ ] 测试专用 LLD 按原工具来源完成新版本维护构建及全部既有验收。
- [ ] 两个 LLVM 插件在三个 host 按原工具来源完成维护构建及加载、行为、搬移验证；macOS 待重做。
- [x] 当前版本检查与 CI 配置保持一致，不混用 LLVM 主版本。
- [ ] Linux ARM64 与 macOS ARM64 按原工具来源完整执行 `make test`；Linux 已通过，macOS 待重做。
- [x] Linux x64 维护验证结果区分转译运行与原生运行。
- [x] 性能结果使用新制品记录；历史证据不冒充本轮验证。

维护构建记录沿用现有 `build/` 位置，下载缓存位于 `local/llvm/`，
临时诊断和回归日志位于 `temp/`。不自动提交或发布工具链预构建 Release。

## 当前核实结果

以下制品和回归记录均先于本次范围清理。按人工要求，已生成的 LLVM 23 制品
备份至 `temp/llvm23-scope-cleanup-20261012/toolchain-backup/`，`toolchain/`
工作区及暂存区恢复 HEAD，暂不与升级脚本配套。Review 通过后，按保留的原
下载来源与构建方式重新生成制品；不得将备份直接恢复后当作维护脚本的新产物。

2026-10-12 的升级前制品备份位于 `temp/llvm23-upgrade/baseline/`。
文件清单与实际 Mach-O／ELF 动态依赖对照位于同目录上一级的
`inventory-<host>.json`、`dependencies-<host>.json`。

- 三个裁剪包的可执行工具集合不变，均不包含 Z3。
- macOS 系统动态依赖集合不变；新版 LLDB 的 install name 为
  `liblldb.23.1.dylib`，保留相应上游符号链接。
- Linux 的 `libpython3.14.so.1.0` 替换旧版 Python；新版 LLDB 不再依赖
  libxml2，原有依赖闭包解析规则已自动将其两个库文件移除。
- Clang 内建头文件沿用整目录复制规则，新增 23 个、移除 1 个；Python
  `encodings` 也沿用原复制规则，版本更新后增加一个模块。没有扩大组件集合。
- 两个插件的 Linux ARM64 和 x64 维护构建已通过原有全部验收。
  x64 是 Apple Container 的 Rosetta 转译执行，不能视为原生 x64 验收。
- macOS 两个插件及测试 LLD 也已通过维护验收，最低系统版本均保持 26.0。
  LLD 的 376 项上游 Mach-O 测试通过、3 项不适用；10 项 FDE 与 18 组
  原生异常验证，在安装前及搬移后均通过。
- Linux ARM64 的完整 `make test` 已以退出码 0 完成，ASan/UBSan 与普通阶段
  均通过 std 610 项及 FCTS 1676 项，并包含实际 LLDB/DAP 调试验证。
  日志为 `temp/apple-container-verify-20260929/llvm23-make-test.log`。

本机为 macOS 27。制品最低系统版本及 SDK 链接检查，不等同于在 macOS 26 上
实际运行；CI 的 macOS 26 基线保持不变，本轮尚未执行该环境的完整回归。
SDK 26.5 和 SDK 27 的最小 C 链接均成功，产物均在本机运行通过；两个插件的
系统依赖另经 SDK 26.5 严格链接检查通过，仅允许 LLVM 宿主提供的符号待加载时
解析。此前“调用者设置 27.0、脚本强制覆盖为 26.0”的检查属于已撤回的脚本行为，
不能作为当前继承环境行为的验收依据。

macOS 首轮全量回归的 ASan/UBSan 阶段通过；普通阶段 std 610 项和 FCTS 1676 项
通过后，CLI 的既有 DAP 夹具在 `setpgid(child, child) == 0` 断言处退出。
该函数与升级前 HEAD 完全相同，日志没有记录 errno，不能确定此次系统调用失败
的原因，也不能直接归因于安全软件或 LLVM。现场保存在
`temp/llvm23-upgrade/dap-setpgid-failure/`，完整日志为同目录上一级的
`macos-make-test.log`。未修改或放宽既有测试，随后完整重跑以退出码 0 结束；
ASan/UBSan 和普通阶段的 CLI/DAP 均通过，每阶段 std 610 项、FCTS 1676 项
全部通过。重跑日志为 `temp/llvm23-upgrade/macos-make-test-retry.log`。
回归副本与当前 29 个变更中的代码／脚本文件逐字节一致，工具链直接使用当前
仓库制品。重跑成功不能证明首次 `setpgid` 失败原因已经查明或修复。
上述 macOS 回归构建 Feng 自身及 sanitizer 程序时，实际使用
`local/llvm/LLVM-23.1.3-macOS-ARM64` 中的 Clang；普通 Feng 用户程序仍使用
bundled Clang。这不符合本轮要求的工具来源边界，因此仅保留为历史实测记录，
不能代替使用 Homebrew host Clang 的最终回归。

## 新工具链性能复测

macOS 五语言三项目均用当前工具重新构建，预热两轮、正式七轮，输入校验全部
通过。按人工要求，本轮报告已移出 `benchmarks/results/`，不纳入提交；完整
分布、构建命令和制品哈希仅保存在本地
`temp/benchmark-results-review/2026-10-12T07-59-01-172Z/`。
text-scan 的中位数为 Feng 102.736 ms、Swift 104.529 ms，峰值 RSS 中位数分别
为 245.95 MiB、494.19 MiB。本轮达到“不慢于 Swift”的目标，但用时分布重叠，
约 1.7% 的中位数优势不代表稳定的大幅领先。

Linux ARM64 在同轮交替执行升级前后制品，预热两轮、正式七轮。两版 Feng 均
包含同一项 text-scan 优化；此次比较的是工具链升级，不是原始读取实现与优化
实现的差异。测量期间没有并行构建或另一组性能采样。

| 项目 | LLVM 22.1.8 中位数 s | LLVM 23.1.3 中位数 s |
| --- | ---: | ---: |
| text-scan / Feng | 0.151376 | 0.154456 |
| text-scan / C++ O2 | 0.164537 | 0.165472 |
| nbody / Feng | 1.622052 | 1.612103 |
| binary-trees / Feng | 0.391122 | 0.397896 |

同轮 Swift 6.2.4 的 text-scan 中位数为 0.192633 s，新 Feng 约快 19.8%，
仍达到目标。Feng 三项新旧中位数分别变化约 +2.0%、−0.6%、+1.7%；各项
样本范围重叠，这组七轮采样没有确认稳定退化，也不足以证明所有应用无性能
变化。绝对时间不能与此前不同时段的采样直接相减后归因于工具链。
原始数据与汇总位于
`temp/apple-container-verify-20260929/llvm23-performance/{results,summary}.json`。
