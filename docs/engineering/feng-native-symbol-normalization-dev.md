# macOS 原生函数符号名称还原插件

状态：2026-09-29，独立插件与验证已完成；macOS nbody 尚未稳定达到 Swift，未默认接入。

## 1. 目标与交付范围

保留 Feng external 的独立 C 名称、声明类型及原生链接目标，解决 macOS 上
`__asm__("_name")` 生成的 LLVM 名称阻碍标准库识别的问题。external 的语言与
发码规则继续由[原生符号别名方案](feng-extern-native-symbol-alias-dev.md)定义。

本轮交付 `third_party/llvm-native-symbols/` 独立插件，通过 Clang 的
`-fpass-plugin` 或 Feng 已有 `FENG_CC_FLAGS` 加载。不修改 `llvm_c_eh`，不默认
接入 driver 或 Feng 发行包；默认接入在本轮性能结果明确后另行决定。
不实现 `@inline`，沿用上一轮诊断中的函数体可见方式评估组合效果。

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
Swift 的目标，也没有据此默认启用插件。

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
仍调用外部 sqrt。已证实位置相关的性能波动，尚未证实是哪一条访存指令或
哪个 CPU 调度选择导致退化。下一步应在固定数据位置下对齐目标 CPU 配置，
核对访存序列并做受控比较，不能由当前结果推出全局优化参数或 runtime 布局改动。

本地完整表格和原始数据位于 §6 证据目录的 `RESULTS.md`、`summary.json`，
跨平台逐行比较位于 `platform-ir-comparison.json`；LLVM IR、汇编和逐次运行
记录位于相邻 `inline-runtime-validation/` 的 `linux-release/`、
`macos-native-symbols/` 与 `macos-native-symbols-layout/`。
