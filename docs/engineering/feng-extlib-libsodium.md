# libsodium 官方白名单同步方案

当前加解密库选型固定为 `libsodium`。同步策略不是“在 Feng 仓库里继续维护一个被 patch 过的私有分叉”，而是：

- 只从 `libsodium` 官方 release tarball 拉取源码。
- 不改动任何 upstream 现有 `.c`、`.S`、`.h` 文件。
- 只保留 Feng 当前需要的现代算法闭包，不携带测试、示例、基准、构建系统与其他历史算法家族。

## 同步范围

当前同步范围只覆盖以下能力及其官方实现闭包：

- 基础设施：`core`、`runtime`、`utils`
- 随机数：`randombytes` + `sysrandom`
- AEAD：`chacha20poly1305`
- 流密码底座：`chacha20` 的 `ref` 与官方 `dolbeau` SIMD 后端
- 一次性认证：`poly1305` 的 `donna` 与官方 `sse2` 后端
- 密钥交换：`curve25519/x25519` 的 `ref10` 与官方 `sandy2x` 后端
- 签名：`ed25519` 的官方 `ref10` 签名闭包
- 哈希与 KDF：`blake2b` generichash、`blake2b` kdf、`sha512`
- 密码哈希：`argon2i`、`argon2id` 与官方 `fill-block` 后端
- 比较辅助：`crypto_verify`

这里明确不再追求“28 个 x86_64 专用源码文件”这一旧数字。原因是：

- 该数字依赖旧版本文件命名与本地 patch 约束。
- 当前目标是不改 upstream，因此最小可维护单元必须是“官方 dispatcher + 官方实现后端 + 递归 include 闭包”。
- 例如 `stream_chacha20.c` 会按官方条件编译同时引用 `ref`、`avx2`、`ssse3`；`scalarmult_curve25519.c` 会在 `HAVE_AVX_ASM` 下引用 `sandy2x`。如果继续手工砍掉这些官方后端，维护责任就会重新回到 Feng。

## 同步规则

- 脚本输入必须是官方 `libsodium` release tarball，默认版本固定为 `1.0.22-RELEASE`，但允许通过环境变量覆盖下载 URL 或 tag。
- 同步单位是“选定源码种子 + 这些文件通过 `#include "..."` 递归引用到的必要头文件闭包”。头文件属于编译闭包，但不计入源码统计。
- 不复制 `test/`、`benchmarks/`、`examples/`、`dist-build/`、`msvc/`、`configure.ac`、`Makefile.*`、`CMakeLists.txt` 等非运行时代码。
- 不复制 `sodium.h`。原因是它会暴露本次未同步的 API 面，导致头文件表面能力大于真实链接能力。
- `version.h` 允许由脚本依据 upstream `configure.ac` 与 `version.h.in` 生成；这属于生成构建闭包，不属于修改 upstream。
- 同步脚本只负责源码落盘，不负责最终构建参数求值。后续构建阶段仍需按目标平台补齐 upstream 期望的 feature 宏与逐文件 SIMD 编译参数；否则虽然可以直接编译，但 upstream 会明确提示这是“undocumented method”，且性能可能低于官方构建路径。
- 除上游 `LICENSE` 外，不保留其他非构建文件。

## 构建输出

- `scripts/fetch_libsodium.sh` 负责将官方白名单源码闭包同步到 `third_party/libsodium/`。
- `scripts/build_libsodium.sh` 负责构建静态库。
- 输出平台、bundled LLVM、SDK / sysroot、构建隔离及产物校验统一遵循
  [feng-std-extlib-build.md](./feng-std-extlib-build.md)。
- 构建脚本需要按目标平台控制源码集与逐文件 SIMD 编译参数，保证官方 dispatcher 能正确绑定已同步后端。

## 显式排除

以下能力当前明确不进入同步白名单：

- `salsa20`、`xsalsa20`、`hsalsa20`
- `secretbox`、`box`
- `aes256gcm`、`aegis`
- `hmac`、`hkdf`、`sha3`
- `scrypt`
- 泛型 `crypto_stream` API

其中泛型 `crypto_stream` 明确排除，是因为当前 upstream 的默认 primitive 仍然是 `xsalsa20`，这与本次“只保留现代算法闭包”的目标冲突。
