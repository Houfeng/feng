# llvm-c-eh

将 GNU C11 中的编译期异常标记转换为 LLVM 原生异常控制流的独立 LLVM Pass 插件。
协议及职责边界以[开发方案](../../../docs/engineering/c-ir-llvm-exception-plugin-dev.md)为准。

本项目使用 LLVM 22.1.8 完整 SDK 构建。插件由 Clang 加载，不链接到目标程序；
目标程序使用其自身的 personality、异常记录与原生展开器。`test/runtime.c` 仅用于测试，
不是产品 runtime。

## 独立构建

```sh
make -C third_party/llvm-c-eh LLVM_ROOT="$LLVM_SDK"
make -C third_party/llvm-c-eh LLVM_ROOT="$LLVM_SDK" test
make -C third_party/llvm-c-eh LLVM_ROOT="$LLVM_SDK" test-ubsan
```

独立 Makefile 可显式接收 `LLVM_ROOT`，默认使用 PATH 中 `llvm-config` 对应的 SDK；
构建过程不下载、安装或切换 LLVM，不依赖 CMake／Ninja。
`LDFLAGS` 用于插件链接；`TEST_LINK_OPTIONS` 和 `UBSAN_TEST_LINK_OPTIONS` 分别接收
普通／UBSan 独立测试的 `--link-option=...` 参数，只传给链接命令。macOS 优先使用
下面的维护入口，它会自动选择经过验证的配套链接器。

预构建维护入口支持全部或单个平台，自动查找已安装的完整 SDK：

```sh
bash scripts/build_llvm_c_eh.sh
bash scripts/build_llvm_c_eh.sh --platform=macos-arm64
bash scripts/build_llvm_c_eh.sh --platform=linux-arm64-gnu
bash scripts/build_llvm_c_eh.sh --platform=linux-x64-gnu --llvm-root=/usr/lib/llvm-22
```

无参数时，以 macOS ARM64 为维护入口尝试全部三个 host，逐项报告结果；任一失败则
最终返回非零，仅安装各自通过全部检查的产物。`--llvm-root` 是所选构建环境内的路径，
与 `--platform` 一起使用；省略时查找 `llvm-config`、`llvm-config-22` 或已安装的
Homebrew／Linux LLVM 22 目录，并校验精确版本及开发组件。

Linux 可以直接在对应架构执行单平台命令。macOS 调度 Linux 时使用预先准备的本机
Apple Container：`llvm-c-eh-arm64`（arm64）及 `llvm-c-eh-x64`（amd64），两者均须将
当前仓库挂载到 `/work`，安装完整 LLVM 22.1.8 SDK、make 和 git。脚本检查架构和挂载，
不自动创建容器或安装依赖；启动原本停止的容器，并在结束时恢复停止状态，原本运行的
容器保持运行。x64 在 Apple Silicon 上使用 Rosetta，不能代替原生 x64 验收。

普通构建不运行维护脚本。
macOS 维护入口显式区分普通验证与 UBSan 验证所用链接器，规则见
[开发方案 §7.3](../../../docs/engineering/c-ir-llvm-exception-plugin-dev.md#73-macos-插件产物补齐)。
执行前须已有 `toolchain/test_tools/lld/macos-arm64/` 补丁 LLD；缺失时按
[LLD 维护说明](../lld/README.md)手工预构建，插件脚本不会自动构建它。
维护脚本将中间产物和验证日志保存在仓库 `build/llvm-c-eh/` 中。
仓库全量回归会清理 `build/` 和根 `temp/`，须与独立插件验证顺序执行；需要保留的
日志先复制到本项目的 `temp/`，不要从该日志目录执行二进制。

## 使用预构建插件

```sh
clang -std=gnu11 -fexceptions -O2 \
  -fpass-plugin=/path/to/llvm_c_eh.dylib \
  -I/path/to/include generated.c consumer_runtime.c -o program
```

Linux 产物名为 `llvm_c_eh.so`，macOS 为 `llvm_c_eh.dylib`。macOS 使用剪裁版 Clang 编译 C 程序时，需照常显式提供
`-isysroot "$(xcrun --show-sdk-path)"`。插件所在路径取决于编译器的 host，不是输出 target。

独立测试也可直接消费插件，不触发构建：

```sh
bash third_party/llvm-c-eh/test/run.sh "$CLANG" "$PLUGIN" temp/llvm-c-eh/test
bash third_party/llvm-c-eh/test/run.sh "$HOST_CLANG" "$PLUGIN" temp/llvm-c-eh/ubsan --sanitizer
bash third_party/llvm-c-eh/test/ir.sh "$LLVM_SDK/bin" "$PLUGIN" temp/llvm-c-eh/ir
bash third_party/llvm-c-eh/test/passthrough.sh "$HOST_CLANG" "$PLUGIN" temp/llvm-c-eh/passthrough --sanitizer
bash third_party/llvm-c-eh/test/sanitizer.sh "$HOST_CLANG" "$PLUGIN" temp/llvm-c-eh/sanitizer-checks
bash third_party/llvm-c-eh/test/benchmark.sh "$CLANG" "$PLUGIN" temp/llvm-c-eh/benchmark
```

测试包括实际抛出、跨函数／翻译单元捕获、父区域传播、嵌套异常存续、并发、间接调用、
聚合参数返回、优化内联、非法输入、LLVM verifier、调用属性与调试元数据。
三个优化级别均执行行为测试；变更后的插件必须重新通过 bundled／宿主编译器及目标平台验收。
当前验收结果与未决问题见开发方案实施记录，不能仅凭普通行为测试通过认为所有平台已交付。

## 源码组织

- `include/llvm_c_eh.h`：使用方包含的协议声明。
- `src/Protocol.*`：协议识别、结构及输入校验。
- `src/Lowering.cpp`：CFG 区域分析及原生异常转换。
- `src/Plugin.cpp`：LLVM 管线接入与模块处理。
- `test/`：独立使用方、测试 runtime 和验证入口。

项目沿用仓库 MIT License；独立分发时保留 `LICENSE`。
