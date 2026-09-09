# 开放泛型值形成 spec 视角的发码修复

## 1 范围与批准

2026-09-09 人工批准 `reified_spec_view_coercions` 方案，修复 G24 ISSUE-G24-040。
语言满足、可见性与所有权规则不变；本设计只补共享发码所需的具化信息。
object-form 与 intersection 复用同一通路，不针对具体类型、包或 std 特判。
普通闭合转换、已有子 spec 到父 spec 的 witness 路径、联合收窄和 callable 适配保持原路径。

## 2 静态元信息

在 `FengTypeDescriptor`、`FengAggregateDescriptor`、`FengFunctionDescriptor` 各增加
`const FengSpecCoercionDescriptor *reified_spec_view_coercions`，无直接使用时为 NULL。
对应 `reified_spec_view_coercions_count` 的统一含义、零值和非运行时消费者边界由
[Reified 元信息表 `_count` 补齐方案](./feng-reified-metadata-count-dev.md) 定义。
表项包含闭合 `box_descriptor`、box 内 `payload_offset`、目标 `witness`。
引用 subject 不装箱，box 字段为 NULL、偏移为零；发码根据已知源类别选择操作，不增加运行时
“是否装箱”的判断。字段与条目不携带实参值或动态满足关系，不枚举全部类型／契约组合。

值复制与生命周期使用源／目标已有描述符；新表不替代 `FengGenericParamDescriptor`，
不把原值改为 spec，也不改变默认零值。转换完成后结果必须清除原值的地址／动态存储标记，
按目标 spec 的固定 `{subject, witness}` 表示参与绑定、返回和 cleanup。

## 3 编译期收集与归属

Semantic 在现有合法转换站点记录独立持有的完整 source／target 类型引用。
只为含开放泛参的 object-form／intersection 形成记录依赖；源级别名在解析上下文中规范化。
同一 owner 内按完整开放身份去重并稳定排序，保留数组可写性、所有实参和泛参声明归属。
闭合后不重新压缩槽位。源值布局与目标 spec 的既有具体化依赖同时保留。
普通布局依赖与视角记录中的布局依赖在收集前使用同一声明词法环境规范化，不能把短名／别名／
全名重复计数后，在制品恢复阶段才合并；provider 和 consumer 的槽集合必须一致。

字段初始化、构造、终结器及对应静态初始化的依赖归类型描述符；顶层函数、类型／fit 方法
归相应函数描述符。归属按转换使用位置，不按某个泛参的声明位置判断。
多层共享调用、递归与闭包使用现有 callable 描述符依赖及捕获机制，不增加隐藏参数；
中间层自身不转换时不复制内层表，但必须向内层传递正确的既有描述符。

## 4 发码

具化点生成闭合 box／witness 和 static const 转换表；完整类型身份决定表项。
共享转换直接读取自身槽位，值源复用普通一次装箱及初始化，引用源保持 subject 身份；
不增加转换函数间接调用、额外装箱、动态类型搜索或满足检查。
保留源表达式单次求值和正常引用持有；不能先用占位布局复制，再用具体布局补救。
已是 spec 的子到父转换继续复用源值的 witness 父链接，不进入新表。

## 5 制品契约

按人工决定保持 `.ft` 现有 2.0 版本，不兼容旧制品，不增加迁移桥接；Header 外壳保持不变。
新增必需 `SPEC_VIEW_COERCIONS` 节和 owner 数量属性，具体 wire 定义仅见符号表主规范。
缺少新必需信息的制品按格式校验失败处理，不回退到旧表示。
runtime、生成 C／对象、`.ft`／`.fb`、provider／consumer 和构建缓存一起重建。
既有版本断言保持原样，撤回迁移提议，决策记录见 ISSUE-G24-041。

## 6 成本与验收

已批准的增量为三个上下文描述符各一指针、实际使用的静态表及共享转换处的表读取；
包括既有自动描述符随字段增加的字节／初始化／栈布局变化。不得概括为零开销。
无转换路径不读取新表；非共享转换和子到父投影不增加新操作。
若发现额外分配、额外转换调用、额外 adapter、额外 ARC 或其他未批准成本，记录后由人工决策。

- [x] Semantic：普通／交叉目标、开放／闭合源、合法／非法契约、重复与不同转换身份。
- [x] Codegen：固定与动态值布局、引用源、字段／方法／静态成员、源单次求值和表示标记。
- [x] Symbol：两个 profile 真实往返、私有表示依赖、槽号／类型／数量／版本／截断反例。
- [x] FCTS：标量／string／tuple／值／引用、绑定／参数／返回／字段／数组、所有权及异常。
- [x] FCTS：类型／方法／fit 归属、同名泛参、多层／递归／闭包、真实跨包及闭合对照。
- [x] 成本：检查静态生成、无额外调用／分配、无转换与子到父路径对照。
- [x] 沙箱外 `make test`：退出 0；UBSan／常规 std 各 604／604、FCTS 各 1271／1271。

具体用例及日志见 [G24 验收证据](./feng-language-conformance-coverage-g24-evidence.md)。
本视角形成修复已验收；G24 仍有独立的 ISSUE-G24-051 约束开放转传缺口，整组暂不关闭。
