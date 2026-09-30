# macOS 原生函数符号名称还原插件

状态：2026-09-30，已接入 macOS 目标的默认编译流程与 host 发行包组装；macOS ARM64、Linux ARM64 完整回归通过。尚未发布远端发行包，macOS nbody 尚未稳定达到 Swift。

## 1. 目标与交付范围

保留 Feng external 的独立 C 名称、声明类型及原生链接目标，解决 macOS 上
`__asm__("_name")` 生成的 LLVM 名称阻碍标准库识别的问题。external 的语言与
发码规则继续由[原生符号别名方案](feng-extern-native-symbol-alias-dev.md)定义。

插件位于 `third_party/llvm-native-symbols/`，保持独立于 `llvm_c_eh`。
2026-09-29 完成独立交付，通过 Clang 的 `-fpass-plugin` 或 Feng 已有
`FENG_CC_FLAGS` 手工加载；2026-09-30 维护者批准默认接入，规则见 §8。
本方案不实现 `@inline`；§7 的性能结果沿用诊断中的函数体可见方式。

## 2. 转换规则

插件在正常优化开始前执行，另提供显式 `feng-native-symbols` opt pipeline。
只处理目标 triple 为 macOS、DataLayout 使用 Mach-O 下划线前缀的模块；Linux
及其他目标保持原样。判断编译目标，不判断运行插件的主机。

对外部函数声明，尝试将 `@"\01_name"` 还原为 `@name`。必须使用 LLVM Mangler
核对还原前后最终机器符号完全相同。仅更改名称，不改变函数类型、调用指令、
调用约定、属性、链接属性或地址使用；不生成 wrapper，不增加运行时操作。
函数定义、全局变量、别名定义、模块内联汇编不在转换范围内。

以下情况保持原声明，不报告语言错误：

- 不是普通 external linkage 的声明，或不满足上述目标／名称规则。
- 还原名为空、含控制字符或占用 LLVM intrinsic 的保留名称空间。
- 模块中已有同名 GlobalValue。禁止合并声明或由 LLVM 自动追加 `.1`。
- 已知直接调用（包括 invoke 和可去除的指针转换）与声明的函数类型或调用约定不一致。
- LLVM TargetLibraryInfo 认识该标准库名称，但声明原型或调用约定不满足其识别规则。

这些判断仅决定是否适用优化，不限制同一原生符号的多种 Feng 声明。
未知的间接调用不改写；取函数地址仍指向相同机器符号。标准库优化复用 LLVM
已有规则，不维护 sqrt 白名单、不直接替换 builtin、不添加 const、pure、nounwind、
memory 等属性。保留已有 nobuiltin 限制及 errno／浮点语义。

## 3. 构建与使用

工程组织与维护要求遵循 [llvm-c-eh 工程规范 §6–§7](c-ir-llvm-exception-plugin-dev.md#6-独立源码与预构建产物)：
源码使用 `third_party/llvm-native-symbols/{src,test,Makefile,README.md,LICENSE}`，
手工入口为 `scripts/build_llvm_native_symbols.sh`，构建与记录保存在
`build/llvm-native-symbols/maintainer-<host>/`，验证后安装到
`toolchain/llvm-native-symbols/<host>/lib/llvm_native_symbols.{dylib,so}` 并携带许可证。
插件没有运行时头文件，不额外创建空的 include 目录或协议版本。

固定 LLVM 22.1.8；不链接私有 libLLVM、不携带 SDK RPATH。无参数手工入口尝试
macos-arm64、linux-arm64-gnu、linux-x64-gnu，支持 `--platform` 与 `--llvm-root`，
复用已经准备的 `llvm-c-eh-arm64`／`llvm-c-eh-x64` 维护容器及其 `/work` 挂载。
不隐式安装依赖，不从普通 make、make test 或 CI 源码重建插件。每个 host 均需验证
bundled／完整 SDK Clang、O0/O2/O3、sanitizer、搬移、导出与依赖，成功后才安装。
Linux 主机构建的插件也通过跨目标 IR 检查 macOS 转换。Linux x64 转译执行与原生
执行分别记录；本轮不发布远端预构建归档。
维护入口在三个 host 均显式选择、校验并记录 LLVM 22.1.8 LLD；macOS UBSan
沿用既有测试专用 LLD，不依赖 Clang 的系统链接器默认值。

```sh
scripts/build_llvm_native_symbols.sh --platform=macos-arm64
make -C third_party/llvm-native-symbols LLVM_ROOT=/path/to/llvm-22.1.8 test
FENG_CC_FLAGS='-fpass-plugin=/absolute/path/llvm_native_symbols.dylib' feng ...
```

本轮独立测试入口不依赖生产 driver 的默认加载，不修改已有用例。
完整验收包含独立维护入口的全部插件测试，以及仓库根目录沙箱外的完整 `make test`。

## 4. 验证计划

- IR：macOS ARM64／x64、Linux／其他目标、DataLayout 不匹配、名称边界、冲突、
  不同 linkage、定义／数据不变、混合原型／调用约定、invoke、地址及重复执行。
- C：独立名称与多签名声明、O0／O2／O3、函数指针与原生符号、nobuiltin，
  加载前后正确链接目标一致；未知函数不被赋予额外语义。
- 行为：普通调用、同一原生函数的别名地址相等、库函数的正常值与浮点边界。
  Linux 主机验证不处理 Linux IR，macOS 主机验证实际转换后的链接与执行。
- 性能：沿用 Linux 发布版 std 的比较方式，Feng／C++ O2、Swift -O、无 LTO、
  默认向量化及浮点参数；数组 runtime 与 Math.sqrt 原函数体在诊断 C 中可见。
  同批比较有／无插件及 C++／Swift，20M 步、两次预热、七次正式交错运行，
  同时记录 wall、CPU、RSS、输出和数组位置。新程序授权在正式计时前完成。
  自然分配有明显位置波动时，沿用已记录的固定位置对照。

名称恢复不等于完全消除 libm 调用。此前最小实验仍保留可能写 errno 的回退，
性能是否达标必须由本轮实际汇编与时间数据判断。

## 5. Todo

- [x] 明确转换边界、独立交付方式和验收口径。
- [x] 实现独立插件及构建入口。
- [x] 补齐 IR、C 和执行用例，验证主机与目标平台区别。
- [x] 完成插件测试和完整回归，记录准确平台范围。
- [x] 完成 macOS nbody 同口径重测及汇编核对，报告是否达到 Swift。

## 6. 独立验收与回归记录

2026-09-29，无参数 `scripts/build_llvm_native_symbols.sh` 的最终执行返回 0，
三个 host 均完成构建、独立验收和预构建安装。macOS ARM64、Linux ARM64 为
原生执行；Linux x64 在 Rosetta 容器中执行，不代替 x64 原生主机验收。

各 host 均通过 IR、bundled／SDK Clang 的 O0/O2/O3、严格浮点、UBSan、
带空格路径搬移及导出／依赖检查；Linux 另通过 ASan＋UBSan。52 组数值与舍入
组合覆盖零、负零、有限数、负数、无穷、quiet/signaling NaN、最小正常数、
最大有限数和最小次正规数，记录返回值、符号、errno 与浮点异常，与未加载
插件的同配置基线一致。Linux IR 及无 asm 别名的普通 C 输入逐字比较一致。

根目录完整 `make test` 在既有 Apple Container 的 Linux ARM64 副本中执行，
返回 0。执行前核对 947 个编译器、用例、std、脚本等源码文件与当前仓库一致。
ASan＋UBSan／普通两阶段的 std 均为 607/607，FCTS 均为 1666/1666，
其他编译器、CLI、DAP、FT 及工程回归均完成。此项是 Linux 全量回归，
不宣称本轮另跑了 macOS 根目录全量回归。没有修改任何既有用例或断言。

本地证据：`temp/apple-container-verify-20260929/native-symbols/` 下的
`all-hosts-final.log`、`make-test.log`、`make-test-exit.txt`、`source-comparison.json`。
各 host 的源码、维护脚本、SDK、插件散列和链接器记录保留于 §3 的维护构建目录。

## 7. 性能结果与跨平台 IR 对比

本节的“组合版”均指数组 runtime 与 Math.sqrt 原函数体在诊断 C 中可见，
并非正式交付了 `@inline` 或调整了 runtime。macOS 有／无插件组合版 C 源码
逐字相同，只改变插件加载参数；std 使用同一个原有发布版，不重编译。

20M 步自然分配的 wall 中位数如下。Linux 数字来自上一轮已验证发布版 std
的 ARM64 容器实验，本轮未重新计时 Linux；独立测试已确认插件对 Linux IR
不做修改。两平台均使用 Clang 22.1.8 O2、Swift 6.2.4 -O，但不是同一批运行。

| 配置 | Linux ARM64/s | macOS ARM64/s |
|---|---:|---:|
| Feng 组合版（macOS 加载插件） | 0.652909 | 1.781647 |
| C++ O2 | 0.649145 | 0.669326 |
| Swift -O | 0.858568 | 0.869684 |

Linux 的组合版七次正式样本为 0.647762–0.682758 秒，数组页内偏移稳定为
912；macOS 为 0.802461–2.547411 秒，页内位置随分配变化。Linux 页大小
4096，macOS 为 16384，且 Linux 运行在虚拟机中；不能仅据此把差距归因于系统。

macOS 固定相同数组数据页内偏移后，有／无插件配对如下；同批 Swift 为
0.874530 秒，C++ 为 0.680926 秒。五处位置中三处快于 Swift，两处仍慢，
其中一处相较同位置的无插件组合版退化约 15%。因此尚未达到稳定持平或超越
Swift 的目标，当轮没有据此默认启用插件；后续独立接入决定见 §8。

| 页内偏移/字节 | 无插件/s | 有插件/s | 耗时变化 |
|---:|---:|---:|---:|
| 15896 | 1.417848 | 0.808300 | -43.0% |
| 16184 | 1.592897 | 1.831872 | +15.0% |
| 16216 | 1.418472 | 0.809245 | -42.9% |
| 16248 | 2.832861 | 0.790842 | -72.1% |
| 16296 | 2.550314 | 1.806572 | -29.2% |

固定位置使用既有诊断的冷路径迁移助手，未改变生产 allocator。两批共完成
57 项小输入检查、171 次长运行（其中 133 次正式计时），全部成功，所有 Feng
长运行输出逐字相同，未剔除样本。正式慢样本的 CPU 时间也明显增加；最大
wall−CPU 差约 64.3 ms，人工授权等待不能解释这些秒级差距。

对两平台组合版优化后的 LLVM IR 逐行核对得到：

- 最内层基本块 `189` 的 55 条 IR 指令逐字相同，包括 `llvm.sqrt.f64`、
  `llvm.fmuladd.f64/v2f64`、`<2 x double>` 访存及 `align 8`。
- 更大的循环区域（基本块 `174` 至 `301` 之前）除属性组引用编号外相同；
  这项比较不表示两平台的属性组定义相同。
- 循环入口的数据指针计算仍有差异：Linux 的数组数据起始偏移为 80 字节，
  macOS 为 72 字节。原 runtime 按 `sizeof(FengArray)` 与
  `_Alignof(max_align_t)` 计算偏移，插件未改变该规则。
- target triple／DataLayout 不同。Linux 函数属性为 `target-cpu=generic`，
  macOS 为 `target-cpu=apple-m1`，target-features、栈保护与 unwind 属性也不同。
- 最终最内层汇编为 Linux 41 条、macOS 40 条；两边均无外部调用、无栈访存，
  有一条硬件 `fsqrt` 和六条向量浮点指令。但指令顺序、寄存器分配及成对加载
  不同，例如 macOS 将相邻两个 double 读为 `ldp`，Linux 此处为分开的读。

因此，插件已消除本例的标准库名称识别障碍，剩余差距不能继续解释为 macOS
仍调用外部 sqrt。本节首轮结果确认位置相关波动；后续固定地址及等长汇编
干预已定位到一条成对读取和跨页宽访存，Linux 也复现相同退化，详见
[插件后 nbody 性能差距归因](feng-nbody-post-plugin-performance-investigation.md)。
不能由局部诊断直接推出全局优化参数或 runtime 布局改动。

本地完整表格和原始数据位于 §6 证据目录的 `RESULTS.md`、`summary.json`，
跨平台逐行比较位于 `platform-ir-comparison.json`；LLVM IR、汇编和逐次运行
记录位于相邻 `inline-runtime-validation/` 的 `linux-release/`、
`macos-native-symbols/` 与 `macos-native-symbols-layout/`。

## 8. 默认接入与 host 分发

原生符号识别有独立价值；本次接入不以 nbody 达到 Swift 为交付前提，也不改变
§2 转换边界、external 语义、ABI、优化级别、向量化或 LTO 配置。

**是否需要插件看 target；加载 `.so` 还是 `.dylib` 看 host。**

- driver 对 macOS **目标**的 bin／lib 编译默认添加 `-fpass-plugin`，包含调试与
  release 模式。Linux 目标不加载、不查找此插件，仍正常加载独立的异常插件。
- 插件运行于编译器进程，文件必须匹配 **host**。Linux host 交叉编译 macOS
  静态库时加载 Linux `.so`；macOS host 编译 Linux 目标时不加载原生符号插件。
- 安装位置为 `<安装根>/toolchain/llvm-native-symbols/lib/llvm_native_symbols`
  加 host 动态库后缀。开发布局增加 `build/toolchain/llvm-native-symbols` 到
  `../../toolchain/llvm-native-symbols/<host>` 的链接；不改变整个 toolchain 目录。
- 三个平台的发行包各自仅携带一份 host 插件及 LICENSE，不按五个 target 复制。
  不携带 SDK、头文件或构建记录；打包校验格式与架构，macOS 沿用统一签名遍历。
  LLVM 异常插件继续保持同样的 host 分发规则。
- macOS 目标需要的插件缺失、损坏、架构或 LLVM API 不兼容时，必须报告错误，
  不静默跳过。继续支持 `FENG_CC` 和 `FENG_CC_FLAGS`，不源码重建预构建插件。
- CI 使用的完整 toolchain 预构建归档必须包含三个 host 的原生符号插件。
  归档发布／恢复入口已操作整个 toolchain，无须增加单独下载流程；正式发布前
  由维护者更新远端归档，本次本地接入不自动发布。

验收在新增独立测试中覆盖 bin／lib、调试／release、五个 target、host 路径选择、
空格路径和搬移、缺失及无效插件、Linux 不依赖此插件、真实 Feng external 发码的
LLVM IR 名称恢复、两插件共存、发行包 host 唯一性与许可证。已有测试仅在获得
批准后补充安装／发行夹具，保留原断言。新增测试进入 `make test` 的相应阶段；
全量回归须在沙箱外执行，分开记录 Linux 容器与 macOS 的实际验收范围。

- [x] 默认 driver 加载与开发布局。
- [x] 发行组装与安装校验。
- [x] 新增集成用例及批准范围内的夹具补充。
- [x] 专项验证、全量回归与交付记录。

### 8.1 证据保留记录

2026-09-30 首次默认接入回归执行了既有 `test/cli/test_cli.c` 中的
`rm -rf temp`，清除了仓库根目录的旧诊断工件及当轮日志。§6–§7 的历史路径
因此不再代表当前可读取的证据；文中历史结果保留，原始工件尚未完整恢复。
该次日志不完整的运行不计为本次验收通过。

后续回归重新执行。macOS 证据保存于
`third_party/llvm-native-symbols/temp/default-integration-20260930/`，Linux
执行中保存于容器 `/workspace/native-symbols-default-evidence/`，结束后再复制
到同一本地证据目录；两者均避开根目录 `make clean` 与 CLI 临时目录清理。

### 8.2 默认接入验收

2026-09-30，macOS ARM64 本机与 Apple Container 中的 Linux ARM64 隔离副本
均在沙箱外完整执行 `make test`，退出码均为 0。两平台的 ASan/UBSan、普通
两阶段各自均通过 std 607/607、FCTS 1666/1666，编译器、CLI、DAP、FT、
异常插件集成及其他既有回归完成。容器的 1008 个源码文件已与本机逐文件比较一致。

新增 `test/cli/native_symbols.sh` 已在两平台的两个阶段通过，覆盖真实 driver 的
默认加载、host／五 target 矩阵、搬移及空格路径、真实 Feng external 的 IR
名称恢复、缺失／损坏／错误 host／不兼容 LLVM API，以及 macOS 重签名后加载。
Linux 目标在缺少原生符号插件时仍通过编译，macOS 目标明确失败。

新增 `test/cli/native_symbols_release.sh` 已在两平台普通阶段通过，实际调用
发行组装脚本验证三个 host 发行包中两种 LLVM 插件均只有对应 host 的一份，
内容与来源一致，且不包含维护记录；缺失插件、错误格式／架构和缺少许可证
被组装或安装校验拒绝。既有增量构建、发行、macOS 签名流程测试也通过。
这些结果不代表本轮在 Linux x64 原生主机执行了全量回归。

已有用例只按维护者明确批准补充 `test/cli/llvm_c_eh.sh` 与
`scripts/run_release_scripts.sh` 两处夹具，原步骤和断言保持不变。插件源码、
预构建二进制与 runtime 未修改；没有重测 nbody，也不据此宣称性能目标达成。

完整证据保存在 §8.1 的本地目录：`macos-make-test.log`、
`linux-make-test.log`、对应的开始／结束时间和退出码，以及
`source-comparison.txt`。发行脚本已接入，但未执行远端预构建或正式发行发布。
