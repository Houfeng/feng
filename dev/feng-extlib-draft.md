为了配合 `feng` 追求极致精简、利落的工业调性，我把之前提到的核心 C 库（包含之前确定的 `mimalloc` 和 `libunistring`）重新为你梳理了一版清单。

这里特别增加了编译后二进制体积（x86_64 静态链接预估）这一列。你会发现，这套精选出来的工业级大闸，不仅性能残暴，在代码体积的控制上也都极为克制，非常符合你的极简主义美学：

---

## 📦 `feng` 标准库底层 C 库全景清单（含体积指标）

| 库名称 | 对应标准库模块 | 静态编译体积预估 | 选型核心理由 / 行业地位 |
| --- | --- | --- | --- |
| **`mimalloc`** | 核心内存管理 (`alloc`) | **~100 KB - 150 KB** | 微软出品。现代堆分配器的性能天花板，完美承载 `feng` 的高频 ARC 动态分配。 |
| **`libunistring`** | 字符串高级处理 (`string`) | **~500 KB** *(按需裁剪后)* | 严格遵循 Unicode 规范。处理复杂的字符边界、大小写转换和规范化（Normalization）。 |
| **`libuv`** | 异步 I/O 与网络 (`io` / `net`) | **~250 KB - 300 KB** | Node.js / Julia 核心。抹平跨平台 `epoll/kqueue/IOCP` 差异，驱动底层的事件循环。 |
| **`yyjson`** | JSON 序列化 (`json`) | **~50 KB - 80 KB** | 地表最快纯 C JSON 库。只有一对 `.c/.h` 文件，支持零内存分配（Zero-allocation）的高能解析模式。 |
| **`openlibm`** | 高精度数学库 (`math`) | **~200 KB - 300 KB** | Julia 语言核心。完全独立的数学库，避免系统自带 `libm` 的平台差异，保证跨平台计算精度绝对一致。 |
| **`PCRE2`** | 正则表达式 (`regex`) | **~300 KB - 500 KB** | Nginx / Git 的选择。正则表达式的行业法律，原生支持 UTF-8 且自带极为锋利的 JIT 编译引擎。 |
| **`zstd`** | 数据压缩 (`compress`) | **~200 KB - 400 KB** | Meta (Facebook) 标杆。现代压缩算法之王，解压速度无限逼近物理内存的传输极限。 |
| **`linenoise`** | 交互式命令行 (`repl`) | **~15 KB** | Redis 作者的极简主义神作。区区上千行代码，完美替代几十 MB 臃肿的老旧 `readline` 库。 |

---

## 当前已落地的 vendoring 约定

- `libunistring` 与 `PCRE2` 的同步脚本都只允许把 Feng 当前实际消费的最小源码闭包落到 `third_party/`，不保留测试、工具链文件、16/32-bit 变体或其他无关目录。
- `PCRE2` 当前先固定为 **8-bit 静态库 + Unicode/UTF 支持**；同步时生成并保留 `config.h`、`pcre2.h` 与 `pcre2_chartables.c`，但不携带 POSIX wrapper、shared library 产物和 SLJIT JIT 依赖树。
- `libunistring` 的构建产物统一写入 `std/lib/libfeng_std_unistring.a`，`PCRE2` 的构建产物统一写入 `std/lib/libfeng_std_pcre2.a`，避免在标准库侧直接依赖上游默认库名。

---

## libsodium

加解密库选型已固定为 `libsodium`。

- `libsodium` 的官方白名单同步规则、同步范围、显式排除项与构建约束，统一收敛到 [feng-extlib-libsodium.md](./feng-extlib-libsodium.md)。
- `feng-extlib-draft.md` 只保留总览结论，不再承载 `libsodium` 的细节闭包规则，避免这里继续膨胀成专项设计文档。
- 后续若调整 `libsodium` 的同步版本、算法闭包、构建宏或输出物路径，只修改 [feng-extlib-libsodium.md](./feng-extlib-libsodium.md)。
