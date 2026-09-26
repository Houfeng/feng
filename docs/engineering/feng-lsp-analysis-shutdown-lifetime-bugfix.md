# LSP 后台分析退出时的快照泄漏修复

## 问题与证据

2026-09-26，GitHub Actions 的 macOS CLI 测试输出 `cli tests passed` 后，
LeakSanitizer 报告 392 字节／4 次分配泄漏。分配栈均位于
`background_analyzer_main`，其中直接泄漏 120 字节，间接泄漏
115、82、75 字节。[失败任务](https://github.com/Houfeng/feng/actions/runs/36233818518/job/108381851541)。

`analysis_task_clone` 为每个文档分配一份 `FengLspDocument`，再复制 URI、
路径和源码。macOS ARM64 上该结构大小为 120 字节，与报告的分配形态吻合。
任务快照属于分析线程的局部 `FengLspAnalysisTask`，不属于服务保存的实时文档。

退出竞态如下：

1. 分析线程在锁内取出任务并完成文档快照复制，然后释放锁。
2. 关闭线程取得 `analysis_mutex`，设置 `analysis_stop_requested`。
3. 分析线程在锁外读取该标志并直接 `break`，跳过循环末尾的
   `analysis_task_dispose`，丢失快照所有权。
4. 服务销毁等待线程退出并释放实时文档，但无法释放已丢失的局部快照。

锁外读取同时违反了停止标志的互斥访问约定。该退出判断在
`e8f7fdf48`（2026-07-17）已存在；不是开启 LSan 的提交引入的。
这是依赖线程交错的真实泄漏，单次全量测试成功不能证明该交错已被覆盖。

## 修复边界与方案

沿用正在分析的任务完成后再退出的既有策略，并将所有权边界统一到任务领取：
已领取任务完成分析及清理，未领取任务由服务销毁入口释放。只在
`analysis_mutex` 内决定是否领取新任务，外层以本线程局部的
`task_ready` 判断是否退出，不在锁外再次读取停止标志。这样领取任务与停止
请求之间有明确顺序，每份已领取快照均到达既有的统一清理入口。

同时检查该所有者的构造失败路径：目前在文档数组分配前就设置非零
`document_count`，数组分配失败后调用销毁函数会访问空指针。将计数设置移到
数组分配成功后，保证零初始化及部分字符串分配失败均可复用同一销毁函数。
这属于同一快照所有权的失败清理，不引入新的分配或恢复机制。

不改变 LSP 协议、分析结果、队列优先级、Feng 发码、runtime、ABI 或 FT。
不增加生产测试钩子，不以延时、重试或关闭泄漏检查掩盖问题。

## 验证计划

新增独立 C 测试入口，仅在测试编译单元中控制互斥锁边界及分配失败，复用真实
服务实现。使用线程同步精确构造退出交错，不依赖随机睡眠；不修改既有用例。
将新入口接入 `make test` 的 sanitizer 与普通阶段。

- [x] 在修复前版本重现已领取任务退出泄漏，并核对分配形态。
- [x] 覆盖空队列停止、待领取任务停止、领取前停止、领取后停止及正常完成后停止。
- [x] 覆盖单／多文档快照、文档／项目目标及多次服务创建销毁。
- [x] 覆盖快照正常释放和每个分配失败位置，检查失败后可再次正常构造。
- [x] macOS ASan／UBSan／LSan 专项检查及完整 `make test`。
- [x] Apple Container Linux ARM64 专项检查及完整 `make test`。

## 结果

独立用例 `test/cli/test_lsp_analysis_lifetime.c` 覆盖 10 种退出场景各执行
4 次、31 个快照分配失败位置，以及 16 次真实服务创建／销毁。测试使用额外的
条件变量控制交错，30 秒仅作为卡死失败上限，不用于安排线程顺序。项目目标
包含零／多文档及项目加载失败后的清理；正常分析完成与发布由文档目标覆盖。
普通阶段也断言任务身份被清除、未领取队列被保留，不仅依赖 LSan 发现问题。

以 `2cd609b8` 的未修改 `service.c` 替换测试编译单元所包含的实现后，Linux
ARM64 上第一项“领取后停止”即失败，并报告 392 字节／4 次分配泄漏。
为便于与 CI 对照，该用例把 URI、路径、源码长度设置成对应报告的长度；
带源码行号的栈明确指向 `analysis_task_clone` 的文档数组及三个字符串复制。
同一测试使用修复后实现，在 macOS ARM64 和 Linux ARM64 的
ASan／UBSan／LSan 下全部通过。

macOS 的旧实现对照程序两次被 SIGKILL，未产生程序输出，不能据此判断泄漏；
没有为此修改程序或安全配置。旧实现的确定性复现由上述 Linux 验证完成。
初次专项链接漏传既有补丁 LLD 搜索路径，补齐与 `make test` 相同的
`COMPILER_PATH` 后通过；这仅是临时验证命令修正，没有改变构建配置。

Linux 首轮全量的编译器、CLI 与新增生命周期用例通过，随后标准库的 Unicode
宽度相关用例失败。先检查环境，发现容器未设置 `LANG`／`LC_ALL`，实际为
POSIX；CI 原有配置则为 `C.UTF-8`。相关宽度函数使用 `setlocale`／`wcwidth`。
仅补齐 CI 的既有 locale 配置，原标准库测试即全部通过（607/607），随后从
`make test` 入口重新全量执行；未修改相关源码或用例。
随后交互中断使宿主日志转发进程退出，容器内子进程仍在继续。该次结果不计入
完整验收；停止本次测试的独立进程组后，再次从完整入口执行，并在容器内
保存日志和退出码，避免终端会话中断丢失验收记录。

记录位于 `third_party/llvm-c-eh/temp/parser-leaks/`：

- `lsp-lifetime-linux-before.log`：旧实现确定性失败及带源码行号的泄漏报告。
- `lsp-lifetime-asan.log`：macOS 修复后专项验证。
- `lsp-linux-lifetime.log`：Linux 修复后专项验证。
- `lsp-shutdown-make-test-macos.log`：macOS 完整 `make test`，退出码 0。
- `lsp-shutdown-make-test-linux-arm64.log`：Linux 完整 `make test`，退出码 0。
- `lsp-shutdown-make-test-linux-arm64.exit`：容器内保存并取回的完整退出码。

最终验收使用 Clang 22.1.8，两平台都从完整入口执行，不以分段通过代替全量：

| 环境 | 命令 | ASan／UBSan／LSan 阶段 | 普通阶段 | 退出码 |
| --- | --- | --- | --- | --- |
| macOS ARM64 | `make test` | 通过 | 通过 | 0 |
| Apple Container Linux ARM64，`C.UTF-8` | `make test` | 通过 | 通过 | 0 |

两平台每个阶段均包含新增生命周期测试以及原有编译器／CLI／LSP／DAP、
smoke 91/91、std 607/607、FCTS 1619/1619；普通阶段的增量构建、发行脚本和
工具链检查也通过。Linux 验证所用 Makefile、实现和测试文件已与本地逐一核对
SHA-256，一致；保存日志和退出码后已停止本轮临时容器。Linux x64 未在本地
执行全量，仍由原生 CI 验证。未修改既有用例或断言，未自动提交代码。
