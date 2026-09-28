# binary-trees 性能调查

## 调查范围与方法

本次调查解释首轮跨语言黑盒评测中 `binary-trees` 的耗时与峰值内存差距，
不修改正式用例、编译器、标准库或 runtime。公共评测口径以
[评测主规范](../../benchmarks/README.md) 为准，任务定义见
[binary-trees](../../benchmarks/cases/binary-trees/README.md)。

基线采用 [2026-09-28 归档](../../benchmarks/results/2026-09-28T03-55-09-135Z/report.md)。
调查先核对制品身份和正常优化配置，再通过外部 CPU 时间、启动基线、工作量变化、
源码及生成代码交叉验证。必要的诊断副本放在 `temp/`，其结果与正式黑盒排名分开。
采样和诊断执行保持串行，不更改系统安全设置，不运行与本次调查无关的 `make test`。

## 结论

状态：**机制调查完成**。日期：2026-09-28。本文保留优化前的调查证据；后续通用
实现、回归及性能结果见[聚合值所有权优化](feng-aggregate-ownership-performance-dev.md)。

本例的主要差距来自两部分：

1. **遍历时的临时拥有者、聚合值生命周期处理和清理登记。** 原本只读的递归
   `check` 保留了大量这类操作。仅在诊断副本中改成已证明树根存活期间的只读借用，
   整程序耗时从 1210.568 ms 降到 693.010 ms，约减少 42.8%。
2. **节点布局和构造／销毁路径。** 本机 Feng 节点为 104 字节，分配器实际占用
   112 字节；C++ 节点为 16 字节。将 C++ 节点补齐到相同尺寸，其 RSS 从约
   5.41 MiB 增至 29.55 MiB，接近 Feng 的 29.73 MiB。

生成描述符明确将 `Node.is_potentially_cyclic` 设为 `false`，节点释放绕过循环
回收锁和候选处理。没有证据支持“循环 GC 拖慢了这棵树”的解释。
补测中 CPU 时间接近整程序耗时，启动安全检查也不是已复现差距的主要来源。

这些结论适用于当前具体实现。诊断副本用到了本例的不可变树、无用户终结器等
事实，不能直接作为通用编译器优化提交，也不能用其数字替换正式语言对比结果。

## 基线与制品身份

原始 [报告](../../benchmarks/results/2026-09-28T03-55-09-135Z/report.md)、
[逐次样本](../../benchmarks/results/2026-09-28T03-55-09-135Z/samples.jsonl)、
[构建记录](../../benchmarks/results/2026-09-28T03-55-09-135Z/build.json)
保存了完整参数和身份信息。最大深度 16，对应任务定义的 14,985,902 次节点构造，
各组正式采样 7 次；该节点数是工作量，不是运行时插桩统计。

| 语言 | 耗时中位数 ms | 峰值 RSS 中位数 MiB |
| --- | ---: | ---: |
| Feng | 1205.888 | 29.734 |
| C++ | 332.126 | 5.406 |
| Rust | 294.711 | 5.594 |
| Go | 244.708 | 18.750 |
| Swift | 530.843 | 13.781 |

本次调查工作区版本为 `86a84381caab724e1d1c62ca963eb0422220e10d`。
相对原始构建记录的 `ef1dfe62d67513274e38a69652c4b889ba69bded`，提交差异仅为
新增评测套件。已核验编译器、runtime 静态库、安装头文件、EH 插件、std 包和
15 个程序的 SHA-256 均与原始构建记录一致。

另外，以同一工具链和 std 包增加 `--keep-ir` 重建本例，输出程序与原程序逐字节
相同。因此，下文生成 C 的观察对应原始程序，而不是另一版编译器的推测。

环境为 Apple M4 Pro、macOS arm64，Feng 发布模式由宿主 Clang 执行 `-O2`。
诊断执行在沙箱外，保持系统安全设置和正常分配器配置。

## 启动与系统干扰复核

外部测量器的诊断副本额外保存 `wait4` 的 `ru_utime`、`ru_stime`，没有在原程序
内部计时。下表为一次预热后三轮的中位数，CPU 列是每次 user + system 之和的
中位数，不是两个中位数的简单相加。

| 程序／输入 | 整程序 ms | CPU ms |
| --- | ---: | ---: |
| Feng，深度 6 | 5.204 | 1.989 |
| Feng，深度 16 | 1196.441 | 1189.788 |
| C++，深度 16 | 329.441 | 325.431 |
| Rust，深度 16 | 288.210 | 284.529 |
| Go，深度 16 | 238.158 | 235.481 |
| Swift，深度 16 | 531.760 | 527.114 |

Feng 深度 16 的三组 wall／CPU 毫秒分别为
`1191.431/1186.157`、`1196.441/1192.808`、`1196.629/1189.788`。
这组差距主要发生在执行中，不能由启动前的几毫秒等待解释。

本机确实对新诊断程序产生过明显的首次运行延迟。例如 C++ 补齐节点副本第一次
运行 wall 为 5072.745 ms、CPU 为 381.865 ms，后续正式样本为 382～390 ms。
这些首次样本作为预热保留，不进入中位数。新采样器的一次启动失败也未纳入数据。
安全检查的日志和边界说明见 [text-scan 调查](feng-text-scan-performance-investigation.md#安全检查与采样边界)。

这不能回溯证明原始每个样本均没有系统干扰；它证明相同制品在排除首次启动后，
仍可重复出现接近原始结果的 CPU 成本。CPU 时间本身也不是硬件调度完全受控的证明。

## 节点布局解释了主要内存差距

[用例源码](../../benchmarks/cases/binary-trees/feng/main.ff) 的两个子节点字段都是
`Option<Node>`。`Option` 是 [None 与 T 的 union](../../std/std/src/basic/Option.ff)，
当前通过通用 union 布局实现：

```text
FengManagedHeader                 24 bytes
Option<Node>.tag + alignment        8 bytes
Option<Node>._fwd                  24 bytes
Option<Node>.payload                8 bytes
Option<Node> total                 40 bytes
Node = header + left + right      104 bytes
malloc_size(calloc(1, 104))        112 bytes（本机实测）
```

`_fwd` 是每个 union 值携带的托管槽描述，不是额外分配的对象。
布局发码入口为 [cg_emit_union_spec_struct_body](../../src/codegen/codegen.c)，
对象头定义见 [FengManagedHeader](../../src/runtime/feng_runtime.h)。

伸展树有 `2^18-1 = 262143` 个同时存活的节点。仅按节点分配块计算，
Feng 约需 28 MiB，C++ 的两个 `unique_ptr` 共 16 字节、约需 4 MiB，尚未加进程
和分配器其他开销。该量级与正式 RSS 差距吻合。

进一步的控制实验只在 C++ `Node` 中增加 88 字节填充，将请求尺寸从 16 改为
104 字节，保留原算法、所有权、输入和输出：

| 实现 | 耗时中位数 ms | 峰值 RSS MiB |
| --- | ---: | ---: |
| 原 C++，本次三轮复测 | 329.441 | 5.406 |
| C++，104 字节节点，五轮 | 385.467 | 29.547 |
| 原 Feng，同组五轮 | 1210.568 | 29.734 |

尺寸控制能重现大部分内存差距，却不能单独重现 Feng 的耗时。因此，内存布局和
执行路径需要分别处理。这里不据 RSS 推断泄漏，也不把 Swift 的 ARC 与 Feng
视为相同的对象布局和生命周期实现。

## 耗时定位

### 遍历保留了大量通用生命周期操作

生成代码在 `check` 中先将 subject 保存为 `_umt2`，执行一次
`feng_aggregate_retain` 并登记清理；进入非空分支后，再为左右子字段分别创建
带 retain 和清理登记的参数临时值，递归返回后逆序 release。
空分支也会为 subject 经过通用聚合生命周期入口。

这不是只在未优化 C 中看到的现象：本机 `-O2` 汇编仍保留这些调用，`check`
函数的栈空间为 `0x1c0`，即 448 字节，并调用 `feng_frame_push/pop`、
`feng_cleanup_push_aggregate/pop` 和聚合 retain/release。
union subject 的物化入口可从 [cg_emit_match_expr](../../src/codegen/codegen.c)
追到 `cg_materialize_to_local(..., "_umt")`。

[聚合生命周期实现](../../src/runtime/feng_aggregate.c) 通过描述符逐槽遍历，
`FENG_SLOT_FORWARD` 还要读取当前值携带的 `_fwd`；非空指针最终进入
[feng_retain／feng_release](../../src/runtime/feng_object.c) 的原子引用计数操作。
它承担的是通用生命周期语义，不能只按照“复制一个指针”的成本理解。

### 构造与销毁也有可见成本

`makeTree` 先分配对象，再分别调用两个字段的聚合默认初始化。叶节点随后又
构造 `none` 并调用两次聚合赋值；非叶节点使用两次聚合 take 转移递归结果。
这里没有重复分配默认子树；多做的工作是字段初始化和通用生命周期处理。

销毁由 `release_children` 对两个 Option 字段分别调用聚合 release，沿通用
托管槽遍历进入下一层节点。`Node` 的 `.is_potentially_cyclic = false`，
所以这些节点的释放不走循环回收候选路径。

### 阶段计时与调用栈交叉验证

诊断副本仅在 `temporary` 的构造、遍历、释放边界增加时间戳，不在每个节点中
插入计时。五轮中位数如下：

| 累计阶段 | 秒 |
| --- | ---: |
| 构造 | 0.396413 |
| 遍历及调用参数的拥有者准备 | 0.549230 |
| 调用参数清理、树释放与函数收尾 | 0.229935 |

统计覆盖伸展树和临时树，不包含 `main` 中长期存活树的构造、最终遍历和释放，
也不包含进程启动、格式化等，因此不能强制与整程序中位数相加对齐。
带计时副本整程序为 1210.446 ms，同组原程序为 1210.568 ms，未出现足以改变
主要结论的计时扰动。

另对保留符号的发布副本、深度 18 进行三秒调用栈采样。栈顶最常见的是
`feng_visit_aggregate_managed_slots`，随后可见分配器 free、`feng_release`、
聚合清理登记等。这支持生成代码和阶段计时的方向；采样次数不换算成精确成本比例。

## 控制实验

以下均为 `temp/` 下的生成 C 副本，链接同一 std 和 runtime，保持 104 字节
节点以及实际节点构造、遍历、释放。除 C++ 尺寸控制外，没有改变对象布局。
每项一次预热、五次正式采样，轮次交替正反顺序，全部输出通过独立校验。

| 诊断副本 | 唯一改变的范围 | 中位数 ms | 最小～最大 ms |
| --- | --- | ---: | ---: |
| 原程序 | 对照 | 1210.568 | 1208.841～1222.817 |
| 只读借用遍历 | 仅替换 `check`，去掉其临时拥有者和清理路径 | 693.010 | 683.267～697.701 |
| 直接初始化字段 | 仅替换 `makeTree` 中的字段初始化／转移 | 964.048 | 952.016～973.161 |
| 直接释放已知字段 | 仅替换 `release_children` 的聚合遍历 | 1153.023 | 1148.327～1157.314 |
| 三项组合 | 合并上述三个诊断改变 | 383.775 | 374.488～388.380 |

所有 Feng 副本的 RSS 中位数仍约 29.73～29.75 MiB。组合结果接近同尺寸 C++
节点的 385.467 ms，但这只是用例专属诊断，不是已实现的通用 Feng 性能。
各项收益不能简单相加：内联、尾递归、寄存器与清理边界也可能随拥有者消除而变化。

只读遍历副本的逻辑如下，原函数签名和 Option 按值传递保持不变：

```c
/* Diagnostic only: the caller keeps the immutable tree alive throughout check. */
if (node.tag != 1U) return 0;
return 1 + check(node.payload.m1->left) + check(node.payload.m1->right);
```

直接初始化副本仍调用原 `feng_object_new`，给两个新字段设置正确的 tag、
`_fwd` 和 payload；直接释放副本仍对每个实际子节点调用原 `feng_release`。
因此组合实验保留分配器和节点最终引用释放，不能解读为“关掉 ARC 后的收益”。
这也不证明可以对所有 union／所有调用直接删掉 retain 或清理。

## 建议的后续工作与决策边界

1. **优先研究可证明的短期借用。** 在只读 match、字段投影和同步调用中复用
   已有拥有者，避免重建临时拥有者及清理登记。沿用
   [ARC 所有权优化方案](feng-arc-ownership-performance-dev.md) 的证明边界，
   覆盖别名写入、回调、异常、终结器和逃逸反例，不能按 `Node` 或 `Option` 名称特判。
2. **消除新鲜存储上的重复工作。** 研究通用 aggregate 的初始化／转移发码，
   以及闭合静态元信息下的生命周期展开。不得删除有可观察行为的默认初始化或终结器。
3. **单独评估 union 实例布局。** 每个 Option 的 40 字节是内存问题的核心。
   若将每值 `_fwd` 转为静态信息，或设计有证明的紧凑表示，须先评审 union、泛型、
   跨包制品和调试约定；这涉及布局／ABI 决策，不能作为局部补丁私自实施。

本轮没有验证这些通用方案的完整正确性，也没有修改生产实现。没有依据优先修改
循环回收、强制更换 allocator，或引入只供这个基准使用的节点类型。

## 复现与证据

先确认原始构建记录中的身份，再在仓库根目录保留生成 C：

```sh
mkdir -p temp/benchmark-investigation/tmp
FENG_TEMP_DIR="$PWD/temp/benchmark-investigation/tmp" \
  build/bin/feng benchmarks/cases/binary-trees/feng/main.ff \
  --target=bin --release --keep-ir --platform=macos-arm64 \
  --pkg="$PWD/std/std/build/pkg/std-0.1.0.fb" \
  --out="$PWD/temp/benchmark-investigation/tree" --name=binary-trees
```

本次诊断文件位于 `temp/benchmark-investigation/`：

- `tree/ir/c/feng.c`、`tree-*.c`：原发码及上述控制副本；
- `cc-commands.jsonl`、`*.command.json`：完整编译／链接参数，使用原 O2 和 EH 插件；
- `samples/baseline-v2/`、`samples/tree-variants/`：CPU、wall、RSS、输出与逐样本记录；
- `logs/tree.asm`、`logs/tree-symbols.sample.txt`：发布汇编与采样栈；
- `verification.json`：诊断副本的独立正确性结果。

本地可复跑 `node temp/benchmark-investigation/run.mjs temp/benchmark-investigation/tree-variants.json`。
`temp/` 不属于长期版本化证据，可能被清理；本文已固化关键参数、变体定义、
测量范围和结果，正式基线则长期保存在 `benchmarks/results/`。

六个树诊断副本另以原校验器的深度 6、7、8 输入完成 **18/18** 次校验，
所有深度 16 的计时样本也逐一校验输出；采样用深度 18 同样通过校验。
本轮交付只新增调查文档，正式用例、编译器、标准库、runtime 均未修改，未执行 `make test`。
