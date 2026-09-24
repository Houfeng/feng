# UBSan 测试专用 LLD

固定 LLVM/LLD 22.1.8，维护 Mach-O FDE 重定位修复，不提交完整上游源码。
规则与验收边界见[插件开发方案 §7.2](../../docs/engineering/c-ir-llvm-exception-plugin-dev.md#72-ubsan-专用预构建-lld)。

`source.env` 固定源码地址、SHA256 和补丁修订号。源码是 Debian 发布的上游 orig
归档，散列来自同目录的 `llvm-toolchain-22_22.1.8-1.dsc`；不应用 Debian 修改。
`LICENSE.TXT` 为 LLVM 的 Apache-2.0 WITH LLVM-exception 许可证。

`patches/0001-fix-macho-fde-relocation.patch` 保留 Mach-O 配对重定位的完整
“符号＋偏移”语义，修复函数入口前有数据时的 FDE 关联。它不识别 Feng、sanitizer
或固定前缀长度，不向被链接程序增加正常路径指令。

维护入口：

```sh
bash scripts/build_test_lld.sh
bash scripts/build_test_lld.sh --llvm-root=/path/to/llvm-22.1.8
bash scripts/build_test_lld.sh --source-archive=/path/to/llvm-toolchain-22_22.1.8.orig.tar.xz
```

省略 SDK 时自动查找已安装的完整 LLVM 22.1.8 SDK；不自动安装依赖。
构建需要 macOS ARM64、CMake、Ninja、完整 SDK、macOS SDK，以及运行上游 lit
所需的 Python 解释器。本目录不引入自行编写的 Python 脚本。

采用 CMake 是因为 **LLD 上游本身使用 CMake**。直接复用上游的生成文件、LLVM
库依赖及测试配置，比另写 Makefile 并重复维护这些规则更简单。Ninja 执行 CMake
生成的构建任务；两者仅用于维护者手工预构建 LLD，不改变 Feng 的 Makefile 或
llvm-c-eh 插件的 Makefile。`cmake/LLVMConfig.cmake` 只为独立构建选择 SDK 的
静态库依赖，不修改已安装 SDK。

既有 `scripts/fetch_llvm.sh` 获取的是 LLVM 预编译 SDK，不是构建补丁需要的源码。
本入口复用已安装 SDK，只下载 `source.env` 固定的源码归档；也可用
`--source-archive` 复用本地归档，两种路径均校验同一 SHA256。

脚本只解压 `lld/`、公共 CMake 文件及同版本 lit，使用 SDK 已构建的 LLVM 库。
静态链接 LLVM 及可用的静态依赖，验证运行依赖闭包后生成可搬移的预构建工具。
通过 Mach-O 上游测试、本缺陷回归及搬移验证后，安装至 `toolchain/test_tools/lld/macos-arm64/`。
最终产物仅依赖 macOS 系统动态库，并附 LLVM、Zstandard 许可证、构建记录和散列。
当前构建目标为 macOS ARM64，最低 macOS 26.0。

源码、补丁或配套 LLVM 版本变化时重新预构建。校验属于构建过程，没有独立 check
脚本或编译器代理。产物目录的 LFS 规则见开发方案 §7.2，Feng 测试及预构建分发接入见
[接入方案](../../docs/engineering/feng-llvm-c-eh-integration-dev.md#4-macos-ubsan-测试链接器)。
