# Linux：Feng builtin 与上一轮 system 对照

本轮完整运行五项、九种配置，Feng 使用 Release + `--alloc=builtin`。其他八种配置沿用上一轮完全相同的制品；用例、输入规模、编译器、runtime、std 和测量方法不变。

system 基线：[上一轮报告](../2026-10-19T08-05-53-748Z/report.md)。builtin：[本轮九配置报告](report.md)。

| 用例 | system 中位数 ms | builtin 中位数 ms | 耗时变化 | system RSS MiB | builtin RSS MiB |
|---|---:|---:|---:|---:|---:|
| nbody | 651.839 | 659.545 | 1.18% | 1.80 | 1.89 |
| binary-trees | 330.695 | 151.864 | -54.08% | 17.20 | 18.03 |
| text-scan | 103.791 | 107.898 | 3.96% | 245.48 | 249.91 |
| fannkuch-redux | 1665.795 | 1689.751 | 1.44% | 1.56 | 1.90 |
| spectral-norm | 101.607 | 103.962 | 2.32% | 1.80 | 2.02 |

耗时变化为 builtin/system − 1；负数表示减少。RSS 为各进程峰值的中位数。
这是先后两轮完整评测的比较，不是 system/builtin 成对交错实验；小幅差异需结合四分位范围与其他语言的波动判断，不能全部归因于分配器。未执行全量回归。

构建命令及 mimalloc SHA-256 见 build.json；准备脚本和 malloc/free/calloc/realloc 定义证据见 evidence/。
