# Parser LeakSanitizer 泄漏修复

## 现场与范围

2026-09-26，启用 ASan 的提交 `cca35c64` 在 Linux CI 的 `test_parser`
失败：LeakSanitizer 报告 5 处各 144 字节的直接泄漏，以及 5 次分配共
320 字节的间接泄漏，总计 1,040 字节／10 次分配。

同一日志报告 external symbolizer 路径无效，因此只有二进制偏移，缺少
函数名和源码行号。泄漏报告与符号化警告分别定位，不将警告当成泄漏原因。
此前本机 macOS 完整 `make test` 通过，不等于 Linux 的泄漏检测已通过。

## 已完成的复现

使用未修改的 `src/lexer/*.c`、`src/parser/*.c` 和 `test/parser/*.c`，
以 Clang 22.1.8、`-fsanitize=address,undefined -g -std=c11` 编译并运行
完整 Parser 测试集。结果如下：

| 环境 | 优化与泄漏检查配置 | 结果 |
| --- | --- | --- |
| macOS ARM64 | `-O1`，默认配置 | 退出码 0 |
| macOS ARM64 | 同一二进制，`ASAN_OPTIONS=detect_leaks=1` | 退出码 1，1,040 字节／10 次分配 |
| Apple Container Linux ARM64 | `-O1`，默认开启泄漏检查 | 退出码 1，1,040 字节／10 次分配 |
| Apple Container Linux x64（Rosetta） | `-O0`，默认开启泄漏检查 | 退出码 1，1,040 字节／10 次分配 |

Linux x64 容器仅配置 1 GB 内存。`-O1` 编译被容器 OOM killer 终止，
`memory.events` 和内核日志均确认内存不足；该次尚未执行测试。因此，
x64 的 `-O0` 结果只作为泄漏复现，不能记作 CI `-O1` 配置验证通过。
该复现阶段尚未执行 Linux 全量 `make test`；后续执行结果见“验证进展”。

`detect_leaks=1` 开启进程退出时的 LeakSanitizer 检查。此前启用 ASan 的
本地完整回归没有显式开启该选项，也没有执行 Linux 容器验证，留下了这项
验证缺口。它不改变 Feng 异常处理语义。

原始 CI 日志、三种环境的符号化报告及 x64 OOM 记录保存在本机忽略目录
`third_party/llvm-c-eh/temp/parser-leaks/`。

## 根因与修复方向

三个环境的泄漏调用栈一致。`parse_callable_signature` 返回按值持有资源的
`FengCallableSignature`；解析失败时，它仍可能拥有已经分配的泛参、参数和
返回类型。`parse_type_declaration`、`parse_spec_member`、
`parse_fit_method_member` 的错误返回路径未释放这个局部签名，而该签名尚未
移交给 AST，释放外层声明也无法释放它。

当前五个触发用例分别来自 spec 静态成员错误、spec 实例成员签名错误、
type 成员缺少函数体，以及 G12 的 spec／fit 错误语法矩阵。每处遗留一个
144 字节的返回类型对象及其 64 字节名称数组，合计 1,040 字节。

`cca35c64` 未修改 Parser 实现或 Parser 测试；本次开启 ASan 暴露了已有的
资源释放遗漏。计划统一 callable 签名的资源释放函数，让签名移交 AST 前的
失败路径与 AST 销毁使用同一实现，覆盖各字段所有权，而不是针对五个用例
分别补丁。

用户已批准修复检测出的真实缺陷。实现将保留“即使失败也返回局部签名所有权”
的现有约定，在三种成员入口的失败路径调用统一清理函数；签名解析成功后被
构造／析构规则拒绝的路径、成员分配失败路径，以及函数／成员 AST 销毁同样
复用它。清理涵盖泛参及约束、参数、返回类型、函数体、绑定成员名称；token
等借用源码的值不释放，诊断位置和语义保持不变。

新增独立 C 测试覆盖局部签名尚未移交 AST 的各个失败阶段、解析成功后拒绝的
签名、合法签名移交后销毁，以及反复失败后继续解析。保留原有五个触发用例。
用户随后批准全部修复，新测试已注册到既有 Parser 入口。

CI 符号化警告是另一项环境缺口：现有安装步骤只显式安装 `clang-22`、
`libclang-rt-22-dev` 和 `lld-22`，而容器内 `dpkg-query -S` 确认
`/usr/lib/llvm-22/bin/llvm-symbolizer` 属于 `llvm-22`。本次容器具备完整
LLVM，并通过 `ASAN_SYMBOLIZER_PATH` 指向该工具取得调用栈。CI 将在现有同源、
同精确版本的安装列表补上 `llvm-22`，复用既有 PATH，不新增安装源或改变版本。

跨平台泄漏检查统一由[集成文档第 9.1 节](./feng-llvm-c-eh-integration-dev.md#91-统一泄漏检测2026-09-26)
定义。修复后先重跑定向检测，再继续沙箱外全量 `make test`；新发现的失败先
记录、定位，再处理，不通过关闭 sanitizer、忽略泄漏或降低断言解决。

## 处理顺序

- [x] 保存原始日志，在现有环境复现并取得符号化调用栈。
- [x] 确认分配与释放的所有权路径，记录根因及通用修复方案。
- [x] 修复实际泄漏，保持语法、诊断、AST 与既有用例行为。
- [x] 核对 CI 符号化工具缺口；原安装列表补 `llvm-22`，沿用精确版本与源。
- [x] 保留既有触发用例，补齐必要覆盖；入口注册已获批并完成。
- [x] 运行针对性泄漏检测与沙箱外完整 `make test`，记录实际结果。

不得关闭泄漏检测、添加 suppression 或放宽原断言来通过测试；不修改
runtime、ABI、FT 或插件协议。遇到额外范围或不确定方案由人工决定。

## 验证进展

- 统一入口后，沙箱外 `make test` 在未修复的 Parser 二进制处以退出码 2
  结束，再次报告 1,040 字节／10 次分配，确认 macOS 检查已生效。
- 修复后，原 Parser 测试集开启 ASan、UBSan 和泄漏检查通过。
- Linux ARM64 独立容器（8 GB，复用已安装 LLVM 22.1.8）执行 `make test`，
  Archive、Lexer、Parser 通过，随后停在 Semantic 的 40,205 字节／221 次
  分配泄漏，与 macOS 剩余的测试清理报告一致。此结果不是完整回归通过。
- 新增 56 个签名清理场景重复 3 轮，修复后通过；同一组新用例使用 HEAD
  原 Parser 时报告 156,624 字节／1,017 次分配。新增用例验证了原有泛参等
  清理缺口；没有更改旧用例。随后获批并完成正式入口注册。
- 新增场景也在 Linux ARM64、Clang 22.1.8、`-O1`、ASan／UBSan／泄漏检测下
  通过。临时入口只用于定位与验证，正式注册后的全量结果另行记录。
- 继续执行 Semantic 测试，全部行为断言通过，退出时另报
  411,002 字节／1,680 次分配泄漏。用户已批准一并修复检测出的真实缺陷；
  后续处理见[Semantic 泄漏修复记录](./feng-semantic-leak-sanitizer-bugfix.md)。

上述日志均位于先前记录的 `third_party/llvm-c-eh/temp/parser-leaks/`。
新增程序最初在该日志目录执行时被终止（退出码 137）；将同一二进制复制到
仓库规定的 `build/` 执行后通过。后续测试程序均放入 `build/`，日志仍保留。

最终 macOS ARM64、Linux ARM64 完整 `make test` 均退出 0，包含显式泄漏检测
及普通阶段。结果与日志见[最终验收](./feng-llvm-c-eh-integration-dev.md#最终验收2026-09-26)。
