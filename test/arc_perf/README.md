# ARC 所有权基准

范围与验收见 [开发文档](../../docs/engineering/feng-arc-ownership-performance-dev.md)。
使用同一源码、动态输入、相同对象规模与销毁事件，对比优化前后的 Feng；不设置
共享 CI 的耗时门槛，不将手动释放或 C++ `shared_ptr` 当作等价语义。

```sh
bash test/arc_perf/build.sh build/arc-baseline/bin/feng /opt/homebrew/opt/llvm@22 build/arc-perf/baseline
bash test/arc_perf/build.sh build/bin/feng /opt/homebrew/opt/llvm@22 build/arc-perf/current
bash test/arc_perf/measure.sh build/arc-perf/results.tsv build/arc-perf/baseline build/arc-perf/current
node test/arc_perf/report.js build/arc-perf/results.tsv
node test/arc_perf/structure.js /opt/homebrew/opt/llvm@22 build/arc-perf/baseline build/arc-perf/current
```

编译器旁必须有配套 runtime、共享头、插件和工具链布局。构建使用完整 Clang
22.1.8，macOS 普通测量使用 bundled LLD；正式计时关闭 sanitizer。保存工具哈希、
命令、生成 C、IR、汇编及二进制。执行产物仅放在工程 `build/` 或根 `temp/`。
`make test` 会清理两者，执行前必须另行归档证据。

O0／O2／O3 各包含同翻译单元和仅 FT／静态库可见的独立包。独立包直接封装刚生成
的库，避免 `feng pack` 自动重建 release 改变 O0 基线。没有 LTO 或人为禁内联。

| 模式 | 场景 |
| --- | --- |
| 0 | 直接返回的新建对象，对照 |
| 1 | 局部构造并返回 |
| 2 | 连续不可变别名与返回 |
| 3 | 分支局部、合流结果及返回 |
| 4 | 数组引用与元素所有权 |
| 5 | 固定布局聚合体及别名 |
| 6 | tuple 只读转换 |
| 7 | 开放泛型复制，保守对照 |
| 8 | defer 观察返回源，保守对照 |
| 9 | 每轮真实抛出，检验异常清理 |
| 10 | 动态 spec，保守对照 |

每轮恰好构造并销毁一个业务 `Resource`，校验最终存活数 0、峰值 1、销毁数与
迭代数一致。计时包含真实对象分配、ARC、清理与终结器；不能把总耗时差全部归因于
单次 retain。独立 C oracle 在每次采样后验证 checksum。默认正常路径 100 万轮，
真实抛出 10 万轮，预热后交替七次；可用 `ARC_COUNT`、`ARC_THROW_COUNT`、
`ARC_ROUNDS`、`ARC_MODES` 覆盖。Feng 内的生命周期计数是同一业务工作的一部分，
不修改 runtime 或添加统计 API。

聚合体模式还按语言规则默认构造成员，然后用业务资源替换；这类默认实例另行
计数并校验每轮一次，其他模式为零。基线和当前版本使用相同构造流程，不省略
这些默认实例，也不把其销毁混入业务资源的计数。

`structure.js` 输出协议 lowering 后、宿主优化前的静态操作数量，以及实际汇编
的栈用量和二进制 section 大小；不将前者当作 O2／O3 内联后的动态调用数量。
