# 泛型接收者绑定修复（A10）

## 1. 状态与范围

2026-09-25，人工已批准按增加泛参描述符字段的方案修复；实施基线为
`f4209454`。问题也已在此前指定基线 `10e2403d` 复现，不是 ARC 优化引入。
此前仅验证生成 C 的分析原型不视为本修复完成。

2026-09-26，修复已完成定向、UBSan、五目标 release 和沙箱外完整 `make test`
验证。首次回归遇到的 R01 旧文本断言已按人工批准完成适配；最终 `make test`
退出码为 0。等待人工 Review，不自动提交。

描述符字段、策略及转发契约仅在[泛型主文档 §1.2](./feng-generics-delivered.md)
定义；直接调用和方法值的语言语义继续以[类型规范](../specifications/feng-type.md)
为准，生命周期以[生命周期规范](../specifications/feng-lifetime.md)为准。

本次允许扩展 `FengGenericParamDescriptor` 的私有契约，并在 Codegen 统一准备
接收者。不得新增 runtime 函数、改变 witness 调用签名、FT 格式、闭包捕获语义，
也不得开启额外的 ASan 工作流。所有受影响的生成包及 runtime 必须统一重编。
既有测试语义不变；R01 仅为五处完整描述符断言追加新字段。沿用已批准的新增
Codegen／FCTS 测试入口注册。

## 2. 复现与修复边界

`T: Read` 的共享体执行以下表达式，`first.read(1)` 为 5，`second.read(1)` 为 6：

```feng
func selected<T: Read>(first: T, second: T): int {
    var value = first;
    return value.read(if true { value = second; 1; } else { 0; });
}
```

当实际 T 是 object-form spec 时，原发码只保存 `value` 的存储地址；参数求值
覆盖了该地址中的视图，稍后进入 spec 槽适配器才读取 subject／witness，得到 6。
正确结果为 5。普通引用同样固定旧身份；普通值类型保持原存储，得到 6。

修复按既有求值顺序先准备接收者，再计算实参，最后消费现有 witness。接收者策略
在实例化时写入静态描述符，共享体按策略选择借用或固定。不得根据布局猜类型、
复制全部聚合体，或把直接调用改成语言方法值。

新固定值使用现有复制／ARC／异常清理机制；清理节点和存储必须覆盖参数求值及
调用，条件节点的 C 生命周期不能只落在分支内部。保持已有聚合体拥有者的作用域
寿命，不能重现 A11 的提前释放。已拥有或证明稳定的接收者继续免除重复保护。

开销边界：增加策略读取／判断，以及不稳定 spec 所必需的视图保存、保活与清理。
不增加堆分配、闭包或间接准备调用；普通值类型不因本修复增加值复制／ARC。
实际发码与成本在验证中记录，不承诺所有泛型调用零增量。

## 3. Todo 与验证矩阵

- [x] 记录批准范围、基线、复现及统一描述符契约。
- [x] 审计并更新所有闭合／开放描述符构造、缓存及约束投影；转发不丢策略。
- [x] 在统一接收者准备中消费策略，复用值模型和正常／异常清理。
- [x] 新增 Codegen 覆盖：策略分类、共享体顺序、节点寿命、静态描述符、O0／O2／O3。
- [x] 新增行为覆盖：引用／值／spec／intersection，不同 subject 与 witness、原槽重绑定、self 原位修改。
- [x] 覆盖函数／类型／方法泛参、嵌套泛型转发、父约束投影、Lambda／defer 及跨包。
- [x] 覆盖局部／字段／数组来源、临时和稳定接收者、多参数和嵌套调用。
- [x] 覆盖参数抛出、方法抛出、正常退出、终结器顺序与原对象存活，确认用例中无泄漏或提前释放。
- [x] 定向、UBSan 新增用例，以及五目标 release 完整 std／FCTS。
- [x] 按 R01 批准范围适配五处旧文本断言，在沙箱外通过完整 `make test`。
- [x] 记录结果、残余限制与英文 commit message，等待人工 Review，不自动提交。

## 4. 实施问题记录

遇到问题先在此记录证据，再分析并解决；不确定的语义、额外开销或范围变化由人工
决定。回归日志放工程忽略的 `third_party/llvm-c-eh/temp/receiver-binding/`，避免
`make test` 清理 `build/` 和根 `temp/` 时丢失。

### R01：既有描述符文本断言适配（2026-09-26 已批准并完成）

`test/codegen/test_codegen.c` 的五处完整初始化器断言（基线第
3642／3691／3753／3816／3870 行）固定为旧三个字段。新发码在 `.witness` 后明确
输出 `.receiver_binding = FENG_RECEIVER_BORROW_STORAGE`；原值分类、描述符符号、
witness 及静态声明断言继续保留。人工已批准仅为这五处预期字符串追加字段，
原源码与其他断言不变。适配后完整 Codegen 在 UBSan／普通阶段均通过，沙箱外
原样执行的完整 `make test` 退出码为 0。

### R02：新增行为夹具的首次语义检查（已修正夹具）

`fcts-first.log` 报 AE0104 和 AE0512：新增数组形参误写为只读 `T[]`，并遗漏
`assertEquals<T: Display>` 所需的 `std.numeric`／`std.text` 导入。按现有数组
规范改为 `T[!]`，补齐本文件导入；仅修改本次新增夹具，不改变编译器语义或原测试。

### R03：正常退出的终结器顺序对照（夹具修正）

首次完整 FCTS 为 1612／1613；仅新增的生存期对照失败：具体路径顺序为 45，
泛型路径为 54。两路径的返回值、调用期间及调用后的存活计数、最终释放次数均
通过，日志 `fcts-second.log`。生成 C 显示两条路径的生产者不同：泛型路径通过
`first()` 返回值初始化字段，已有 `_call_result` 拥有者比 holder 更早登记、
更晚释放；具体路径直接构造对象并转移拥有权，没有此临时拥有者。因此泛型路径
在释放 snapshot 后，旧 subject 仍由工厂返回值临时量保活。先将具体对照也改成
相同的工厂调用，结果仍为具体路径 45、泛型路径 54：具体表示能够转移返回值
拥有权，而开放表示沿用已有的返回值拥有者。两者并非同一拥有者序列。

接收者寿命的对照改为向两条路径传入相同布局的 holder，在被测函数内仅执行
接收者准备和重绑定，排除旧 subject 的工厂返回值拥有者。原泛型工厂场景保留为
独立用例，继续检查其既有临时量寿命和最终释放次数。不得提前释放已有临时量
以强行改变顺序；无需为夹具差异修改编译器。

`fcts-expanded-fixed.log`：对齐 holder 拥有者后的顺序、函数退出释放、调用期间
存活均通过；原工厂场景仍为 54，已有临时量未提前退休。完整 FCTS 1619／1619。

### R04：独立 Codegen 测试进程在启动时终止（标准构建目录验证通过）

等待 R01 的人工批准期间，用独立入口执行新增 Codegen 用例。首次进程返回 137
（SIGKILL），日志为空，尚无测试断言或生成 C 错误。先核对 Mach-O 签名及重试
结果，不把该终止直接归因于本修复，也不预先认定为安全软件拦截。磁盘签名验证
通过，计时显示尚无用户态 CPU 消耗。将相同对象文件重新链接到仓库要求的
`build/bin/test_receiver_binding` 后，O0／O2／O3 独立测试通过，未调整任何安全
设置。系统日志不足以确认具体拦截方；最终通过正常测试入口完成了 UBSan／
普通阶段的完整回归。

### R05：默认 spec 新夹具使用了不存在的语法（已修正夹具）

新增默认视图用例在 spec 声明中填写方法体，被既有 SE0605 拒绝。按现有默认值
规则保留纯签名，以默认返回零的方法与真实实现区分 witness；默认 spec 具有
自身的默认 subject，不能将其描述成空 subject。本项只修正新夹具和测试说明。

## 5. 验证结果

2026-09-26 验收完成。所有日志位于 §4 指定的忽略目录。

| 验证 | 结果与证据 |
| --- | --- |
| 初始九路径探针 | 全部符合预期；普通值保持 6，泛型 spec 从错误的 6 修复为 5；`initial-probe-run.log` |
| 新增 Codegen | 描述符分类、求值顺序、清理节点寿命及 O0／O2／O3 通过；`codegen-focused-build-dir.log`、`codegen-ubsan.log` |
| 新增行为覆盖 | 28 个用例；普通及 UBSan 完整 FCTS 均为 1619／1619；`fcts-expanded-fixed.log`、`fcts-ubsan.log` |
| 五目标 release | macOS ARM64、Linux ARM64／x64 的 GNU／musl 全部通过；每目标 std 607／607、FCTS 1619／1619；`release/target-status.txt` 及对应日志 |
| 完整 `make test` | 沙箱外执行，退出码 0；UBSan／普通阶段均完成，std 各 607／607、FCTS 各 1619／1619；`make-test-final.log` |

完整回归同时通过 compiler／runtime 单元测试、91 项 smoke、CLI／LSP／DAP、FT
及跨包验证、原生异常契约、性能约束、增量构建、发行流程、macOS 签名流程、预构建
工具链恢复和 LLVM C EH 集成检查。沿用现有 UBSan 和测试链接器配置，未开启 ASan。
五目标 release 使用重编后的各目标 runtime，Linux 产物在既有 Apple Container
环境执行。首次失败日志 `codegen-existing.log`／`make-test-first.log` 保留，与
R01 的已批准适配对应，不作为通过证据。

新增行为矩阵覆盖引用、值与 spec、父／交集约束、类型／方法泛参、跨包与泛型返回、
字段／数组／闭包／defer、多实参、循环、默认视图、正常和异常清理。

本机 Clang 布局检查为 `kind@0`、`receiver_binding@4`、`descriptor@8`、
`witness@16`，大小仍为 24 字节、对齐 8（`descriptor-layout.log`）。字段利用
原有 padding，但所有相关生成包和 runtime 仍须统一重编，不能混用未赋予新字段
正确语义的旧描述符。运行成本边界见 §2；本次不声称所有泛型调用零增量，也未修改
witness 调用签名、FT 格式或 runtime 函数集合。

## 6. 建议 commit message

```text
fix(codegen): preserve generic spec receivers during argument evaluation

Add explicit receiver binding metadata to generic parameter descriptors.
Snapshot and retain unstable spec views before evaluating arguments while
preserving value-self storage and existing ownership lifetimes.

Cover generic domains, cross-package dispatch, and normal and exceptional
cleanup; update descriptor assertions and document the required rebuild.

Validation: make test and release std/FCTS suites on all five targets.
```
