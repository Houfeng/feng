# 跨语言黑盒性能评测

本文件是评测方法的主规范。首轮比较 Feng、C++、Rust、Go、Swift，包含
`nbody`、`binary-trees`、`text-scan` 三个独立程序。用例目录的 README 只定义任务、
输入和输出，不重复公共规则。本套件不接入 `make test`，也不作为其依赖。

## 测量口径

- 比较相同输入、任务规则和正确性要求下的实际程序表现。各语言使用正常的
  标准库、运行时和惯用实现；不统一 ARC、GC、对象布局或具体分配次数。
- 所有程序只有一个业务执行线程。Go 设置 `GOMAXPROCS=1`，保留默认 GC；
  运行时自身的辅助线程不等同于业务并行。不强制 GC，不使用自定义内存池。
- 采用发布优化：Feng `--release`，C++ `-O2`，Rust `-C opt-level=3`，
  Go 默认优化构建，Swift `-O`。优化等级名称不代表各编译器优化力度相等。
  不使用 sanitizer、手写 SIMD、fast-math、PGO 或关闭安全检查的额外参数。
- 外部 POSIX 测量器在创建子进程之前读取单调时钟，等待程序退出后停止计时。
  计时包含进程创建、运行时初始化、输入读取、计算、输出及退出，不包含编译、
  数据生成、正确性验证和报告生成。峰值 RSS 来自该子进程的 `wait4` 资源统计，
  统一换算为字节，不包含 Node.js 调度器的内存。
- 每次运行是新进程；默认预热两轮、正式采样七轮。按轮次轮换并反转语言顺序，
  顺序执行，禁止并发测量。输入文件采用预热后的正常系统文件缓存，不清理磁盘缓存。
- 预热、正确性检查及每次正式采样都必须校验输出。错误、超时、缺少工具链或
  缺少语言实现均使本次命令失败，不跳过后继续生成完整对比结论。
- 报告逐项给出耗时中位数、四分位范围、最小/最大值、吞吐量、峰值 RSS 的中位数
  和最大值，以及相对 C++ 的耗时倍数。倍数大于 1 表示用时更多。不生成语言总排名，
  不把任务差距解释为某个内部机制的单独成本；短于 100 ms 的项目标注启动及噪声影响。

## 目录与依赖

`cases/<case>/{feng,cpp,rust,go,swift}/` 保存实现；`scripts/` 提供构建、输入生成、
独立参考结果、采样与报告；`results/` 保存明确归档的报告与原始证据。
用例输入、规模与校验通过 `scripts/definitions.mjs` 登记，调度和测量逻辑不依赖具体任务。
自动生成的程序、缓存、输入和临时结果全部位于仓库 `build/benchmarks/`。
`make test` 会清理 `build/`，需要长期保存的结果应先归档。

要求 macOS 或 Linux、Node.js 22+、C/C++ 编译器、Rust、Go、Swift，以及与 runtime、
标准库相匹配的 Feng 编译器。默认使用 `build/bin/feng` 和 PATH 中的工具。
可通过 `FENG`、`CC`、`CXX`、`RUSTC`、`GO`、`SWIFTC` 指定可执行文件。
构建脚本为当前平台重新打包仓库 std；不安装工具、不下载包，不修改 Feng 编译器或 runtime。

## 独立运行

在仓库根目录执行：

```sh
node benchmarks/scripts/bench.mjs build
node benchmarks/scripts/bench.mjs verify
node benchmarks/scripts/bench.mjs run --profile standard --rounds 7 --warmup 2
node benchmarks/scripts/bench.mjs report build/benchmarks/runs/<运行标识>
node benchmarks/scripts/bench.mjs archive build/benchmarks/runs/<运行标识>
node --test benchmarks/scripts/test.mjs
```

`verify` 覆盖边界输入和小规模参考结果；`run` 先验证，再执行指定规模的五语言测量。
`quick` 用于检查流程，`standard` 用于首轮正式对比。支持 `--case <用例名称>`；
仍必须包含全部五种语言。`--timeout` 指定每个子进程的秒数，默认 120 秒。
超时或中断后保留已写出的日志和采样，但没有完成标记的运行不能作为正式报告归档。

构建记录包含完整参数数组、编译器版本、主机、源码和程序 SHA-256、Feng runtime/
标准库身份及 Git 状态。运行前验证源码与程序哈希未变化；每次运行保存构建记录、
参数、输入哈希、输出、原始样本及报告。归档包含源码快照、构建记录、校验记录、
带 stdout/stderr 的逐样本 JSONL 和报告；逐进程的独立日志留在运行目录。
输入可按记录确定性重建。
主机信息与绝对工具路径会进入本地报告，分享前可审阅。

## 验证边界

本套件新增独立的调度器/校验器测试，以及 15 个程序的端到端校验。
按本次任务的明确授权，开发本套件不要求运行与之无关的 `make test`。
性能数字只用于描述本次机器、版本、参数与工作负载，不设自动通过阈值。
