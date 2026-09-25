# 原生异常性能与契约验证

范围与验收以 [开发文档](../../docs/engineering/feng-native-exception-performance-dev.md)
为准。这些基准不设置共享 CI 的毫秒数门槛。

`contracts.sh CLANG PLUGIN OUTPUT_DIR` 自动检查 runtime 非外抛声明及必须保留的
异常边，已接入 `make test` 的普通和 sanitizer 阶段。只生成 IR，不执行其中用于
声明检查的虚拟 runtime 调用。

在工程 `build/` 中构建和运行性能基准：

```sh
bash test/exception_perf/build.sh build/bin/feng /opt/homebrew/opt/llvm@22 build/exception-perf/current
bash test/exception_perf/measure.sh build/exception-perf/results.tsv build/exception-perf/baseline build/exception-perf/current
node test/exception_perf/report.js build/exception-perf/results.tsv
# 按相同轮次生成相对于 baseline 的逐项差值与差值汇总（候选减基线）。
node test/exception_perf/report.js build/exception-perf/results.tsv baseline build/exception-perf/paired.tsv
```

先冻结基线，再构建候选。编译器旁的 runtime、共享头文件与插件必须与该版本配套。
使用完整 LLVM 22.1.8 SDK，C++ 和真实 Feng driver 使用同一个 Clang；macOS 普通
性能对照都用原 bundled LLD，不使用 UBSan 补丁链接器。主计时关闭 sanitizer。
保存源码、完整命令、工具及产物哈希、IR、汇编及二进制。Clang 保存的 `.bc`／`.ll`
是插件降级前的输入；`.lowered.ll` 是单独重放插件得到的检查产物，实际编译的汇编为
`.s`，不能混同。全量回归会清理 `build/`，运行之前应
归档证据。不能把重新构建后的不同产物与原计时混用。

每个构建包含 O0、O2、O3，两种布局：同翻译单元与独立包／独立 C++ 翻译单元。
独立包直接封装刚生成的库与 FT，因为 `feng pack` 会重建 release 产物，不能用于
保存 O0 生产者。双方均不使用 LTO 或人为禁内联。

| 模式 | 测量内容 |
| --- | --- |
| 0 | 无调用的标量递推控制组 |
| 1 | 调用不外抛的标量函数 |
| 2 | 含条件 throw 的函数，全部输入正常 |
| 3 | 同一函数，加具体类型 catch |
| 4 | 同一函数，加兜底 catch |
| 5 | 同一函数，加不匹配内层 catch 与匹配外层 catch |
| 6 | 每轮抛出 u64 并捕获 |
| 7 | 每轮经八层递归抛出并捕获 |
| 8 | 每 1000 轮抛出一次 |

`EH_COUNT`、`EH_SEED`、`EH_MODE` 在运行时读取，输入读取和 checksum 验证不在计时
区间内；每个计时结果都需通过独立 C oracle。普通循环默认为 2000 万轮，浅层密集
抛出为 20 万轮，递归诊断为 2 万轮。先预热，再交替各构建运行七轮；`EH_ROUNDS`
可增加采样次数，`EH_MODES` 可选择待单独复测的模式。
递归实际保留的物理帧数以汇编为准，不能把源码深度直接当作相同展开深度。

Feng 的既有函数 marker／TLS 成本、普通发码差异保留在原始结果中。try 与不带 try
的配对差值用于定位，不能凭减法精确分摊成本。真实抛出包含各自完整的载荷分配、
展开与销毁；C++ 使用平台 libc++／异常 runtime，Feng 使用自身 runtime 与随包
libunwind。平台依赖的差异也需如实报告，不能将两者说成同一个异常 runtime。

`native-control.sh LLVM_ROOT PLUGIN OUTPUT_DIR` 单独对比协议 C 和原生 C++ 的异常
发码。两侧链接同一个 C++ producer／计时对象、使用同一个 C++ personality 和异常
runtime；覆盖无 try、try 正常返回和每轮抛出。默认预热后交替采样七轮；
`EH_BUILD_ONLY=1` 仅构建并验证行为，方便在其他测试结束后再测性能。这项对照用来
定位插件转换成本，不代表真实 Feng 的异常总成本。
