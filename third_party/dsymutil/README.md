# 测试与发行共用的补丁 dsymutil

固定 LLVM 23.1.3，以同版本源码加补丁构建 macOS host 的 `dsymutil`，不提交完整
上游源码。当前 macOS 可执行文件只支持 macOS host，Linux 工具链不增加本工具。
发行布局以[分发规范](../../docs/engineering/feng-release-and-install.md)为准。

`source.env` 固定上游源码地址、SHA256 和补丁修订号；采用与 LLD 相同的上游 orig
归档，不应用 Debian 修改。许可证为 LLVM Apache-2.0 WITH LLVM-exception。

补丁调整 DWARFLinkerParallel 的类型存活分析：编译单元、namespace 等类型作用域中
已发出的类型也作为类型根，复用既有递归标记、引用依赖及去重流程。这样 Clang 因
`-fno-eliminate-unused-debug-types` 输出、但没有可见变量引用的类型仍可供 LLDB
表达式求值。规则不识别语言、类型名、字段或数组布局，不改变代码、变量可见性和
runtime。经典链接器保持上游行为；Feng 使用默认的 parallel 链接器。
这是本项目维护的类型保留策略调整，不是已确认的上游修复回补。代价是可能增加
dSYM 大小和符号处理工作量，不增加被调试程序的运行时开销。

维护入口：

```sh
bash scripts/build_dsymutil.sh --llvm-root=/path/to/llvm-23.1.3
bash scripts/build_dsymutil.sh --platform=macos-arm64 --source-archive=/path/to/llvm-toolchain-23_23.1.3.orig.tar.xz
```

维护构建仅在对应 native host 执行；复用完整 SDK 的静态 LLVM 库，重建受补丁影响的
DWARFLinkerParallel 和 dsymutil。构建使用 CMake、Ninja 和 SDK 自带的 TableGen，
不重建 Clang，不修改已安装 SDK，不新增编译 pass。补丁同时移除 dsymutil 对未使用
的私有 `config.h` 的包含，以便使用安装 SDK 的公开头文件独立构建。

源码缓存位于 `local/dsymutil/`，每次验证归档散列并重新解压、应用补丁和构建；
`--source-archive` 支持离线输入，校验相同散列。产物、日志和散列位于
`build/dsymutil/<host>/`。普通构建、测试、CI 和发行组装只读取预构建工具。

维护构建验证类型保留、DWARF 输出、非 LTO／ThinLTO、可执行文件散列不变及包含
空格路径的搬移；通过版本、架构、系统动态依赖和签名检查后，才原位安装
`toolchain/llvm/<host>/bin/dsymutil`，失败保留旧工具。静态依赖的许可证随包保留，
macOS 最低版本沿用分发规范，不能依赖构建机器的 Homebrew 路径。

顺序为获取／裁剪 LLVM，再构建补丁 LLD 和 dsymutil。重新裁剪会恢复官方工具，
发布前必须重新安装补丁版本。制品沿用 `toolchain/**` 的 Git LFS 和预构建 Release
流程；维护构建检查补丁版本，发行组装检查工具存在，不增加 Makefile 前置检查。

Feng 接入规则见[构建规范](../../docs/specifications/feng-build.md)，数组展开规则见
[DAP 主规范](../../docs/engineering/feng-dap-dev.md)。验收须包含原生 LLDB 直接读取
擦除类型后的数组、本包及跨包的 Feng DAP 数组展开，以及沙箱外完整 `make test`。
`.fd` 格式、代理表达式和运行时表示均保持不变。

维护构建通过后，可直接验证原生调试器：

```sh
bash third_party/dsymutil/test/native-lldb.sh \
  toolchain/llvm/macos-arm64/bin/lldb build/dsymutil/macos-arm64/validation
```

该用例在真实断点处读取两个数组的全部元素，覆盖嵌套字段、对齐填充、枚举和 union，
不经过 Feng 代理。Feng 展开复用现有 `test_dap_array_element_types` 回归，覆盖
Debug 默认／off／thin、本包类型和源码隐藏的跨包类型，不放宽已有断言。
