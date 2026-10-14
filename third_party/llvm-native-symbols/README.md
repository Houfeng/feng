# LLVM native symbols

独立的 macOS 原生函数符号名称还原插件。设计、边界及使用方式见
[主开发文档](../../docs/engineering/feng-native-symbol-normalization-dev.md)。

手工维护入口与 [llvm-c-eh](../llvm-c-eh/README.md#独立构建) 一致，在当前 host
构建、验证和安装。SDK 自动发现，`--llvm-root` 可覆盖。
容器挂载和调用示例见 [LLD 维护说明](../lld/README.md)，执行命令改为
`/work/scripts/build_llvm_native_symbols.sh`。

```sh
scripts/build_llvm_native_symbols.sh
scripts/build_llvm_native_symbols.sh --platform=macos-arm64
scripts/build_llvm_native_symbols.sh --platform=linux-arm64-gnu --llvm-root=/usr/lib/llvm-23
```

验证成功才安装到 `toolchain/llvm-native-symbols/<host>/lib/`，macOS 基名为
`llvm_native_symbols.dylib`，Linux 为 `llvm_native_symbols.so`，并携带许可证。
构建记录只保留在 `build/llvm-native-symbols/maintainer-<host>/`。

独立开发也可使用本目录 Makefile，通过 `LLVM_ROOT`、`BUILD_DIR`、`TEST_CLANG` 和
`TEST_LINK_OPTIONS` 指定 SDK、输出与消费方；正式预构建必须经过上述维护入口。
用 `-fpass-plugin=/absolute/path/to/plugin` 加载；opt 的显式 pass 名称为
`feng-native-symbols`。普通 Feng 构建与 CI 不从源码构建此插件。

Feng driver 默认加载及 host 发行布局统一见
[开发方案 §8](../../docs/engineering/feng-native-symbol-normalization-dev.md#8-默认接入与-host-分发)。
