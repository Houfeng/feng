# text-scan 性能调查

## 调查范围与方法

本次调查解释首轮跨语言黑盒评测中 `text-scan` 的耗时与峰值内存差距，
不修改正式用例、编译器、标准库或 runtime。公共评测口径以
[评测主规范](../../benchmarks/README.md) 为准，任务定义见
[text-scan](../../benchmarks/cases/text-scan/README.md)。

基线采用 [2026-09-28 归档](../../benchmarks/results/2026-09-28T03-55-09-135Z/report.md)。
调查先核对制品身份和正常优化配置，再区分启动、文件读取与字节扫描的成本，
结合标准库实现、生成代码、外部 CPU 时间和诊断副本建立证据链。
诊断文件放在 `temp/`，结果不替代正式黑盒排名；测量保持串行，不更改系统安全设置，
不运行与本次调查无关的 `make test`。

## 结论

状态：**机制调查完成，尚未实施优化**。日期：2026-09-28。

本例的主要耗时发生在 `readBytes` 把文件组织成字节数组的过程中，扫描循环只是
次要部分：阶段诊断中读取约 **8.824 秒**、扫描约 **0.284 秒**，读取约占两阶段
之和的 **96.9%**。因而，首轮约 78 倍的整程序差距，不能解释为 ASCII 解析循环
本身慢了 78 倍。

具体证据指向：

1. `readAllBytes` 以 4096 字节分块读取，却逐字节调用 `List<byte>.add`，本次共
   256,000,000 次。发布代码仍包含每字节的引用保活、清理登记、泛型包装和存储检查。
2. `List.entries()` 再逐元素复制整个列表。这个阶段单独约需 1.1 秒，并同时
   保留列表后备存储和结果数组。
3. 本机大块分配释放后仍保留了大量驻留页。读取完成后存活堆约 244 MiB，
   `vmmap` 另外显示约 498 MiB 的大块空闲区域仍驻留，解释了 RSS 为什么接近
   746 MiB；不能把它解读为三份仍存活的数组或直接认定泄漏。
4. 原扫描循环还保留了逐元素的数组访问辅助函数调用。仅用于诊断的跨翻译单元
   可见性实验能进一步减少这部分成本，说明它也有后续优化空间。

只替换诊断副本的读取路径，保留原 Feng 扫描发码，整程序从约 9.1 秒降到
**0.330 秒**，RSS 降到 **245.66 MiB**。这验证了主要定位，但该读取副本仅针对
本次稳定的普通文件，并非可直接提交的标准库修复。

## 基线与身份

正式 [报告](../../benchmarks/results/2026-09-28T03-55-09-135Z/report.md)、
[运行参数](../../benchmarks/results/2026-09-28T03-55-09-135Z/run.json)、
[构建身份](../../benchmarks/results/2026-09-28T03-55-09-135Z/build.json)
可追溯本次输入、源码、编译器和库。

输入为 32,000,000 条、每条 8 字节的 ASCII 记录，总计 256,000,000 字节，
即 256 MB 或约 244.141 MiB；输入 SHA-256 为
`9fac96966a4f9f1c7cb1584d94adbf010145c8d183182e05408d679c3a4d524c`。
预期输出为 `32000000 10666667 10666666 159984000000`。

| 语言 | 原始耗时中位数 ms | 原始峰值 RSS 中位数 MiB |
| --- | ---: | ---: |
| Feng | 9198.751 | 746.234 |
| C++ | 117.493 | 245.563 |
| Rust | 80.450 | 245.703 |
| Go | 129.775 | 248.469 |
| Swift | 123.719 | 494.094 |

调查工作区为 `86a84381caab724e1d1c62ca963eb0422220e10d`。已核验原始
15 个程序、Feng 编译器、runtime、安装头文件、EH 插件和 std 包的 SHA-256
均与归档一致。该版本相对原始构建 revision 只增加评测套件。
用相同发布参数增加 `--keep-ir` 重建 `text-scan` 后，程序也与原制品逐字节相同。

本机为 Apple M4 Pro／macOS arm64；Feng 使用原 `-O2`、原 EH 插件及库。
所有性能执行均在沙箱外串行进行，保留正常安全设置、分配器和预热文件缓存。
本例不是冷盘吞吐测试，也不覆盖 Unicode、正则表达式或 JSON 解析。

## 安全检查与采样边界

外部采样器诊断副本从同一次 `wait4` 中补采 user／system CPU 时间。
一次预热后三轮中位数如下，CPU 是逐次 user + system 的中位数：

| 程序／输入 | 整程序 ms | CPU ms |
| --- | ---: | ---: |
| Feng，空文件 | 6.589 | 1.848 |
| Feng，256 MB | 9117.923 | 9091.287 |
| C++，256 MB | 110.507 | 107.393 |
| Rust，256 MB | 76.209 | 71.039 |
| Go，256 MB | 126.030 | 123.300 |
| Swift，256 MB | 121.680 | 110.022 |

Feng 三组 wall／CPU 毫秒为 `9117.923/9091.287`、`9081.274/9060.133`、
`9131.312/9113.019`。进程的 CPU 时间占绝大部分，无法用数秒的进程外启动等待
解释这组稳定差距；后面的阶段探针和调用栈也将成本定位到读取及集合处理。

本机安全检查确实发生过。12:20:55（UTC+8）的 `syspolicyd` 日志记录了新编译的
`temp/benchmark-investigation/bin/measure-cpu` 的 `GK Xprotect results` 和
`scan finished, waking up any waiters`。新诊断副本第一次启动也观察到明显 wall／CPU
分离：批量读取副本为 4120.044／333.621 ms，随后三次为 327.936～330.873 ms。
该现象和 [Apple 对首次启动及变更后检查的说明](https://support.apple.com/zh-cn/guide/security/sec469d47bd8/web)
一致，但本轮没有将每个延迟样本逐一与安全日志关联，不能全部归因为同一种检查。

所有首次样本作为预热保留，未计入正式诊断统计。最初一次新采样器启动失败没有
产生被测程序指标，该次整体复测作废；随后独立验证采样器并完整重跑。
当前签名／隔离属性也不能回溯证明原始运行完全没有安全检查。
本次结论是“稳定差距主要为程序内 CPU 工作”，不是“本机不存在安全检查开销”。

## 读取路径的实际工作

调用路径为：

```text
text-scan.scan
  -> std.fs.readBytes
     -> readAllBytes
        -> File.read(chunk[4096])
        -> 对每个字节：List<byte>.add(chunk[i])
        -> List<byte>.entries()
  -> 原始 ASCII 扫描循环
```

入口见 [File.ff](../../std/std/src/fs/File.ff) 的 `readAllBytes`／`readBytes`，
容器见 [List.ff](../../std/std/src/collections/List.ff) 的 `add`／`grow`／`entries`。

### 每字节追加的固定成本被放大了 2.56 亿次

`List.add` 在源码中只是取长度、查容量、必要时扩容并插入。但其泛型共享体的
发布汇编仍有以下工作：

- 读取后备数组长度、容量及插入操作周围，三组数组 retain／release 和清理登记；
- 函数级 `feng_frame_push/pop`，以及清理链使用的线程局部存储访问；
- 按泛型描述符准备元素临时存储、复制一个元素，再调用 `feng_array_storage_insert`；
- 插入入口检查数组与泛型描述符、元素尺寸、索引、容量及元素类别。

相关实现见 [feng_array.c](../../src/runtime/feng_array.c)、
[feng_object.c](../../src/runtime/feng_object.c) 和
[feng_exception.c](../../src/runtime/feng_exception.c)。
这不表示正常路径反复抛异常；成本包括为拥有者登记清理的普通执行路径。

以三组保活为例，256,000,000 次追加对应约 768,000,000 组 retain／release
调用位置。这是从当前发布汇编及输入推导的数量，不是动态计数器测得的调用次数。

对保留符号的发布副本采样五秒，采样窗口主要覆盖追加阶段。常见栈顶包括
`feng_release`（794 次）、`feng_cleanup_push`（412 次）、线程局部访问（405 次）、
`feng_cleanup_pop`（351 次）、`feng_retain`（311 次）和存储兼容性检查（275 次）；
`read` 为 22 次。这只能作为热点方向证据，不把采样次数转换成精确百分比。

### 最后还执行了一次逐元素复制

`entries()` 创建长度等于列表长度的新数组，再以循环复制每个元素。
共享体中仍可见两个下标检查、数据地址及元素尺寸查询、按元素种类分派和复制。
这里的 `byte` 不需要元素 ARC，但仍经过泛型复制路径。

在生成 C 中，`return (T[])result` 返回同一个结果数组引用，**没有再复制第三个
结果数组**。因此不能依据 746 MiB 的 RSS 推断这个转换又做了一次整文件复制。

扩容采用容量翻倍和前缀迁移，不是每次追加都复制全部已有元素；没有证据说明
该读取路径是二次复杂度。问题是线性工作中的巨大常数及额外复制。

## 阶段计时与控制实验

阶段探针只放在读取前、读取后和扫描后；std 深入探针另在大容量扩容及 entries
边界记录统计。普通阶段副本三次读／扫秒数为：

| 轮次 | 读取并形成数组 s | 扫描 s |
| --- | ---: | ---: |
| 1 | 8.824301 | 0.287148 |
| 2 | 8.949293 | 0.283450 |
| 3 | 8.805888 | 0.283764 |
| 中位数 | 8.824301 | 0.283764 |

该副本整程序中位数为 9139.029 ms，与无插桩的 9117.923 ms 接近。
std 深入探针的一个完整样本进一步将读取阶段分为约 7.751 秒的读入／逐字节追加，
和 1.083 秒的 entries 复制；其他格式化用的小列表记录不计入这两个大阶段。

两个反事实实验定位收益边界：

| 诊断副本 | 改变范围 | 中位数 ms | 峰值 RSS MiB |
| --- | --- | ---: | ---: |
| 原程序，本次复测 | 无 | 9117.923 | 746.219 |
| 批量读取＋原扫描 | 仅替换 `readBytes` 调用 | 329.656 | 245.656 |
| 批量读取，同组再次对照 | 同上 | 328.766 | 245.641 |
| 批量读取＋数组实现可内联 | 将原 `feng_array.c` 与程序编译在同一翻译单元 | 113.539 | 245.656 |

每项一次预热后三次正式采样。首次批量读取副本的原始 ms 为
`330.873, 327.936, 329.656`。最后一组配对原始 ms 为：

- 仅批量读取：`332.028, 328.766, 323.472`；
- 再增加数组实现可见性：`112.699, 115.472, 113.539`。

批量读取副本对稳定的普通文件取得长度，用现有 `feng_array_new` 分配一份 byte
数组，以 C `fread` 读入后交给原扫描代码；没有生成预期结果、跳过解析或修改
有效记录校验。它用于回答“若去掉 List 构建路径会怎样”，不是标准库完整替代物。

第二个实验没有改写 `feng_array.c`，没有关掉索引检查，也没有改变优化等级。
它让原辅助函数定义对 O2 可见，减少跨翻译单元调用，并允许宿主在循环中继续优化。
生成 C 的每字节访问原本分别调用 `feng_array_check_index` 和 `feng_array_data`；
本次有效输入需要读取全部 256,000,000 个字节，辅助调用成本因此可见。
该实验不能解释为所有收益都来自某一条边界检查，也不能把直接包含 runtime C
文件当作生产方案；它说明通用数组访问发码／跨模块可优化性值得后续研究。

## 峰值内存的组成

对于本输入，列表最后的容量为 `2^28 = 268435456` 字节，即 256 MiB，
结果数组为 256,000,000 字节，即约 244.141 MiB。复制期间这两块同时存活，
理论 payload 合计约 500.141 MiB。

原分配器设置下的探针观测与之对应：

| 时点 | malloc 当前存活字节 | 约 MiB | 进程峰值 RSS 约 MiB |
| --- | ---: | ---: | ---: |
| 大列表 entries 开始 | 268494608 | 256.056 | 502.1 |
| entries 复制结束、列表仍活着 | 524510992 | 500.213 | 746.3 |
| readBytes 返回、列表已释放 | 256053936 | 244.192 | 746.3 |

进一步将一个独立诊断进程在读取完成后暂停，仅执行 `vmmap -summary` 和
`heap -s` 快照，再恢复并校验输出。该暂停运行不进入任何耗时表。观测为：

```text
MALLOC_LARGE             resident 244.2M
MALLOC_LARGE (empty)     resident 498.3M
MALLOC_SMALL (empty)     resident 2112K
malloc zone live bytes           244.2M
heap: one large live block       250016 KiB（含分配尺寸取整）
```

读取完成后，列表缓冲已经释放，当前大块存活分配主要是一份结果数组；扩容和
复制形成的大块空闲区域仍有驻留页。结合扩容轨迹、存活堆变化及 VM 区域，这解释了
“当前活数据约 244 MiB，进程 RSS 仍约 746 MiB”的现象。
没有将每个空闲 VM 区域逐一对应到某次扩容地址，也没有承诺其他系统或内存压力下
会维持相同驻留量。该数据不支持直接宣称有三份存活数组或存在泄漏。

`malloc` 存活字节、VM 区域驻留量和进程峰值 RSS 是不同指标，不能互相替换。
Apple 的 [XNU 资源统计实现](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_resource.c)
将 `ru_maxrss` 取自 `resident_size_max`；
[malloc 统计定义](https://github.com/apple-oss-distributions/libmalloc/blob/main/include/malloc/malloc.h)
另外区分当前使用量与保留空间。公开源码仅用于说明指标含义，不能视为本机内核
每条实现细节都已核验。

## 建议的后续工作与决策边界

1. **首先调整读取／收集路径。** 优先研究通过 Feng 和现有 C ABI 批量读取及
   构建字节数组，避免“一个字节一次 List.add”。必须覆盖短读、读取失败、空文件、
   文件变化和不能预先确定长度的输入，不能直接照搬本次普通文件诊断 reader。
2. **研究通用容器批量操作与快照复制。** 复用已有数组复制能力，避免对平凡元素
   做逐元素泛型包装；同时保留非平凡元素的初始化、引用和异常安全语义。
   不能按 `byte`、文件名或本基准名称添加特判，也不能把 `entries` 的独立副本
   私自改成共享可变存储。
3. **再处理通用发码成本。** 对已有拥有者支持可证明的借用，减少共享体中的
   冗余保活／清理；评估数组长度、数据地址及检查对宿主优化器的可见性。
   可参考 [ARC 所有权优化方案](feng-arc-ownership-performance-dev.md)，
   但跨模块内联／布局暴露属于单独设计问题，不能直接把 runtime 私有结构变成公共契约。

本轮没有修改 runtime 或增加私有 ABI。若未来方案确实需要新增 runtime 能力，
必须先证明 Feng + C ABI 无法承载并交人工决策。也没有通过强制清理分配器空闲区、
关闭安全机制、unchecked 或 fast-math 获得表中结果。

## 复现与证据

首先核验正式构建记录及输入哈希，再保留原生成 C：

```sh
mkdir -p temp/benchmark-investigation/tmp
FENG_TEMP_DIR="$PWD/temp/benchmark-investigation/tmp" \
  build/bin/feng benchmarks/cases/text-scan/feng/main.ff \
  --target=bin --release --keep-ir --platform=macos-arm64 \
  --pkg="$PWD/std/std/build/pkg/std-0.1.0.fb" \
  --out="$PWD/temp/benchmark-investigation/text" --name=text-scan
```

本次临时证据位于 `temp/benchmark-investigation/`：

- `text/ir/c/feng.c`、`std/ir/c/feng.c`：原程序及同版本标准库的生成 C；
- `text-*.c`、`probe*.h`、`cc-commands.jsonl`、`*.command.json`：诊断修改和编译参数；
- `samples/baseline-v2/`、`samples/text-variants/`、`samples/text-extra/`：逐次指标及输出；
- `logs/text.asm`、`logs/text-symbols.sample.txt`：发布汇编和采样栈；
- `logs/text-vmmap.txt`、`logs/text-heap.txt`、`logs/text-vm-snapshot.phases.txt`：内存快照；
- `verification.json`：诊断副本正确性结果。

本地可分别执行 `node temp/benchmark-investigation/run.mjs temp/benchmark-investigation/text-variants.json`
和 `node temp/benchmark-investigation/run.mjs temp/benchmark-investigation/text-extra.json`。
`temp/` 不是长期归档，可能被清理；本文已保存关键实验定义、参数、原始数列、
阶段和内存快照数值，正式基线长期保存在 `benchmarks/results/`。

五个日志诊断副本复用原校验器的空文件、单条、千条、独立样例及五类错误输入，
完成 **45/45** 次校验；计时、调用栈采样及内存快照使用的完整输入均另行校验输出。
本轮只新增调查文档，正式用例、编译器、标准库和 runtime 均未修改，未执行 `make test`。
