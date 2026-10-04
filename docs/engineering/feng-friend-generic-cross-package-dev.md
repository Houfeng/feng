# Feng `@friend` 第一阶段：泛型与跨包支持开发方案

> **状态**：开发草案，待 Review；尚未修改正式规范、编译器或测试。
>
> **日期**：2026-09-30。
> **最近核对**：2026-10-04。
>
> **已确认范围**：在现有具体 `type` 主体范围内支持类型级泛参与跨包。
> `fit F` 使用目标类型 `F` 的 friend 身份，该规则同样适用于跨包 fit，
> 不再按 fit 与成员或 friend 类型是否同包增加授权限制。
>
> **文档定位**：本文独立记录第一阶段的需求、实现方案、验证计划与分步 TODO。
> 已确认的授权方向与待 Review 的设计细节分开记录。实施时先更新主规范，
> 再实施代码和测试；本文不替代正式语言规范。

## 1. 目标、范围与规范归属

当前 `@friend(Reader<T>)` 可以引用 owner 的泛参，但不允许直接使用
`@friend(T)`。具体 friend 类型必须在成员声明位置解析，且 friend 事实不进入
package-public `.ft`，因此现有能力不能供外包代码消费。

第一阶段允许先用类型级泛参表达授权对象，再在成员所属类型的使用视角下代入。
提供方不需要反向依赖实际 friend 类型所在包，包依赖继续遵循既有无环规则。

本次保持 `@friend` 的成员级定向授权模型：类型实现上下文使用自身身份，
fit 方法使用目标类型身份；泛型和包边界不改变身份匹配的含义。

### 1.1 本阶段范围

- 支持 `@friend(F)`、具化检查及条件转传，支持跨包类型实现和 `fit F` 消费授权。
- 最终 friend 主体保持现有具体 `type` 范围，包括其泛型实例；开放主体按第 3.5 节
  保留检查条件。
- 完整交付 Semantic、Symbol/`.ft`、Codegen、LSP 和测试链路。
- 保留 `fit int` 等既有 fit 能力；主体准入限制只涉及这些类型能否成为 friend，
  不限制既有 fit 成员向合法主体授权。

主体扩展在本阶段交付后按
[第二阶段开发方案](./feng-friend-fit-target-types-dev.md) 推进；其设计细节不作为
本阶段的前置条件。

### 1.2 规范归属

正式规范的职责保持收敛：

| 内容 | 唯一主规范 |
| --- | --- |
| friend 主体、授权上下文、fit 授权、成员签名可见性 | [可见性规范](../specifications/feng-visibility.md) |
| 泛参作用域、类型代入、类型身份及泛型转传的通用规则 | [泛型规范](../specifications/feng-generics-draft.md) |
| 受限成员的收录范围、friend 元数据、读取校验和格式兼容性 | [符号表规范](../specifications/feng-symbol-table.md) |
| `.fb` 的接口与制品消费边界 | [包规范](../specifications/feng-package.md) |

关联的 type、spec、fit、注解文档只引用 friend 主规则，不重复定义授权条件。
历史工程说明中的同包限制和不导出说明应在实施时标明适用阶段，并引用更新后的
主规范，不再维护另一份现行规则。

## 2. 当前实现依据

以下是本次整理时核对到的实现事实，不代表本方案已经实现：

| 层次 | 当前事实与入口 |
| --- | --- |
| Parser / AST | [parser.c](../../src/parser/parser.c) 的 `parse_annotations` 已将 friend 参数作为 `FengTypeRef` 解析；[parser.h](../../src/parser/parser.h) 已有类型位注解参数表示 |
| 参数准入 | [analyzer.c](../../src/semantic/analyzer.c) 的 `normalize_friend_member_annotations` 要求参数根节点解析为 `FENG_DECL_TYPE`，直接泛参在这里被拒绝 |
| 类型身份 | 同文件的 `FriendTypeIdentity` 已有 owner 泛参槽位和活动泛参身份；`normalized_friend_type_matches_subject` 已支持按 owner 实参代入 |
| fit 授权 | 同文件的 `friend_seal_member_is_accessible_from` 要求当前 fit 与被访问成员均来自本地编译输入，即现有同包限定 |
| 元数据 | `FengFriendMemberInfo` 当前保存在 Semantic 分析结果中；现有规范明确不向 package-public `.ft` 导出 friend 事实 |
| 成员收录 | [ft_write.c](../../src/symbol/ft_write.c) 的 `writer_should_export_decl` 按既有公开、布局、mix 和 spec 实现依赖规则选取声明，不因 friend 收录普通 seal 方法 |
| 链接 | [codegen.c](../../src/codegen/codegen.c) 的 `cg_member_uses_package_callable_surface`、`cg_member_uses_package_static_binding_codegen` 决定跨包方法和静态字段路径，需与新增导出事实一致 |
| 工具 | [service.c](../../src/cli/lsp/service.c) 已通过 `feng_semantic_member_has_friend_access` 复用源码成员的 Semantic 授权判断 |

现有主规范的直接泛参禁用、fit 同包限定和 friend 不导出条款，是本次需要更新的
明确边界。已有 owner 泛参代入和词法主体识别可以继续复用。

本次补充核对了两个既有行为：

- `fit int` 已合法，但 `@friend(int)` 不合法。可见性主规范明确排除内建标量，
  `normalize_friend_member_annotations` 也要求根主体解析为 `FENG_DECL_TYPE`；
  [test_semantic.c](../../test/semantic/test_semantic.c) 的
  `test_friend_declaration_and_access_diagnostics` 对 `@friend(int)` 断言 `AE1336`。
  `fit int` 的正例见
  [fit_builtin_direct_call.ff](../../test/smoke/phase1a/fit_builtin_direct_call.ff)，
  其目标类型规则见 [内建类型 fit 规范](../specifications/feng-fit-builtin-type.md)。
  因此，让 `int` 成为 friend 是主体范围的新增扩展，不能描述为保留已支持行为。
- friend 无法使用成员签名时，即使没有代码访问该成员，声明也会被拒绝；对应
  `test_friend_signature_visibility_rules` 的跨 module 私有返回类型负例断言
  `AE1338`。friend 声明合法后，fit 还需在实际访问点检查自身上下文的签名可见性。
  两层规则以 [可见性规范 §11.1](../specifications/feng-visibility.md#111-friend-成员签名可见性)
  为准，新增泛参不改变各自职责。

## 3. 已确认的授权规则

### 3.1 支持类型级泛参

新增允许在已有合法成员注解位置书写 `@friend(F)`，其中 `F` 来自成员 owner
的有效类型级泛参作用域。type、object-form spec 和 fit 目标提供的泛参绑定统一
处理；fit 不因此获得新的泛参声明语法。

既有 `@friend(Reader)`、`@friend(Reader<T>)` 等写法继续使用同一模型。
本次不扩展到方法自身声明的泛参，不允许通过注解创建未绑定泛参。

friend 参数依旧使用类型位语义。开放条件按第 3.5 节保留并在具化点检查；最终
主体须符合第 1.1 节的第一阶段范围。不把“当前尚未解析为具体声明”直接当成
非法，也不把未知类型当成已经匹配的 friend。

### 3.2 fit 与类型实现使用统一身份

访问点的 friend 主体由词法实现上下文确定：

- type 的既有类型实现上下文使用当前 type 的完整语义身份。
- fit 的实例方法和静态方法使用 fit 目标类型的完整语义身份；嵌套 lambda
  沿用其词法实现上下文。
- 不属于上述上下文的代码不因持有一个 friend 对象或泛参而取得身份。

对合法 friend 主体 `F`，`fit F` 使用 `F` 的 friend 身份，不要求 fit 与被访问
成员同包，也不要求 fit 与 `F` 的声明同包。第三个包中的合法 `fit F` 同样可以
使用该身份。

直接写出的具体 friend 与泛参代入所得 friend 适用同一规则，不按注解是否含有
泛参建立不同的跨包授权路径。

fit 的两个方向必须保持清晰：

| 方向 | 含义 |
| --- | --- |
| fit 成员标注 `@friend(F)` | 该 fit 成员向 `F` 授权；成员声明位置属于 fit 所在 module/package，泛参绑定来自 fit 目标 |
| `fit F` 消费其他成员的授权 | 当前方法以 `F` 为授权主体；不因此成为 `F` 自身，也不取得未授权 seal 成员的访问权 |

module、owner、被访问 fit 的可见性和激活规则继续成立。去掉 friend 专属的同包
限定，不等于使不可访问的声明或未生效的 fit 成为成员候选。

### 3.3 按成员所属实例精确代入

设成员为 `M`，其声明时 friend 类型表达式集合为 `Friends(M)`，访问视角下的
owner 绑定为 `θ`，当前类型实现或 fit 目标身份为 `S`。friend 分支的身份条件为：

```text
存在 f ∈ Friends(M)，使 substitute(f, θ) 与 S 的语义类型身份完全相等。
```

这只是成员 seal 授权条件；签名可见性及普通访问规则还需通过各自检查。

代入必须使用被访问成员的实际 owner 绑定。spec 父级成员先投影到原声明 spec
视角，fit 成员按其目标绑定恢复；不能仅取同名泛参或泛型模板来比较。

`Vault<Reader>` 的授权不能用于 `Vault<Other>`；泛型实参属于身份的一部分。
短名、别名和完整路径仍按现有解析结果归一化，不能按字符串决定授权。

不要求所有泛参完全闭合才比较身份。例如当前主体为 `Reader<T>`，被访问类型
为 `Vault<Reader<T>>`，代入后已能证明身份相等。若被访问类型是 `Vault<F>`，
又无法证明 `F` 等于 `Reader<T>`，则不能因为将来某次具化可能相等而放行。

最终生成 wrapper 或具化代码的包不成为新的授权主体。已在 `Reader<T>` 的实现
中合法选中的访问，跨包具化后仍保留原词法主体和所选成员身份。

### 3.4 保留既有成员访问行为

合法注解位置、类型实现上下文范围、非传递性、重载过滤顺序、spec 访问视角、
方法值形成点检查及形成后 callable 的使用规则，继续引用
[可见性规范](../specifications/feng-visibility.md) 中的 friend 规则。

本次不扩大 friend 以外的 seal 权限，不改变 requirement 满足和 witness 选择
规则，也不从 friend 身份推导出可变性、静态性或 owner 可见性等其他能力。

### 3.5 保留开放检查条件，延续签名检查行为

允许声明含有开放泛参的类型或函数；主体合法性、成员签名可用性等尚不能判定的
条件作为编译期事实保留，在具化点完成检查。若继续转传开放泛参，则条件随绑定
一起转传；跨包时也必须保留，不能因为 provider 已通过编译而丢弃。声明时已经
能够判定的错误仍在声明处报告。

签名检查沿用既有行为：对有声明 module 的 friend，使用其声明上下文检查成员
签名，每个 friend 都须通过；不能把声明合法性推迟到该成员首次被访问时才检查。
当该条件依赖 owner 泛参时，在具化点检查对应 owner 实例，即使尚未访问该成员。
合法声明的 `fit F` 访问仍另外检查 fit 的实际上下文；这一层失败只拒绝该次访问。

保留检查条件不等于暂时授予访问权。第 3.3 节的身份匹配要求保持不变。

## 4. 目标行为示例

以下代码展示第一阶段目标行为，尚未作为当前实现可通过的测试。

### 4.1 通过泛参向依赖方类型授权

提供方包 `friend_core`：

```feng
open module friend_core;

/** 将 value 的访问权交给类型参数 F 指定的类型。 */
open type Vault<F> {
  @friend(F)
  seal var value: int = 7;
}
```

消费方包 `friend_app` 依赖 `friend_core`：

```feng
open module friend_app;

import friend_core;

/** 消费 Vault<Reader> 的定向授权。 */
open type Reader {
  /** 当前类型身份与代入后的 friend 相等。 */
  func read(vault: Vault<Reader>): int {
    return vault.value;
  }
}
```

第三个包 `friend_extension` 依赖前两个包：

```feng
module friend_extension;

import friend_core;
import friend_app;

/** fit 使用 Reader 的 friend 身份，不要求与成员或 Reader 同包。 */
fit Reader {
  /** 通过目标类型身份消费跨包授权。 */
  func readFromFit(vault: Vault<Reader>): int {
    return vault.value;
  }
}
```

上述依赖没有反向边；`friend_core` 不需要引用 `friend_app.Reader`。

| 访问上下文 | 对 `Vault<Reader>.value` 的 friend 授权 |
| --- | --- |
| `Reader` 自身类型实现上下文 | 允许 |
| `friend_app` 中的 `fit Reader` | 允许 |
| `friend_extension` 中的 `fit Reader` | 允许 |
| `Other` 或 `fit Other` | 拒绝 |
| 顶层函数，即使参数类型为 `Vault<Reader>` | 拒绝 |

表中允许项均以普通访问规则和签名检查通过为前提。

### 4.2 开放泛型身份仍可证明相等

沿用上一节的 `Vault`，以下主体无需等待 `T` 闭合：

```feng
/** 用完整泛型实例身份取得授权。 */
type GenericReader<T> {
  /** 代入后的 friend 就是当前 GenericReader<T>。 */
  func read(vault: Vault<GenericReader<T>>): int {
    return vault.value;
  }
}
```

直接具体 friend 的跨包 fit 也应覆盖。例如包 B 定义 `Reader`，包 A 依赖 B，
并在自己的成员上声明 `@friend(B.Reader)`；包 C 依赖 A 和 B 后，其 `fit B.Reader`
可以使用该授权。该场景不含直接泛参，但必须遵循同一跨包规则。

### 4.3 主体准入示例

沿用第 4.1 节的 `Vault<F>`，以下结果均以其他语义检查通过为前提：

| 写法或使用 | 本阶段结果 |
| --- | --- |
| `@friend(Reader)`、`@friend(GenericReader<int>)` | 允许 |
| `Vault<Reader>`、`Vault<GenericReader<int>>` | 允许 |
| 含有 `Vault<T>` 的开放泛型声明 | 允许声明，保留检查条件 |
| `@friend(int)`、`@friend(int[])` | 声明处拒绝 |
| `Vault<int>`、`Vault<int[]>` | 具化点拒绝，即使未访问 `value` |

## 5. 编译器实施方案

### 5.1 统一编译期事实与身份绑定

沿用类型位 AST 和现有 owner 泛参身份模型，把 friend 事实组织为成员附属的
类型表达式集合，供本地源码与 imported 声明共同查询。

每项事实应能恢复成员及 owner 声明身份、类型表达式、泛参声明绑定或槽位，
以及声明和签名检查所需的上下文。包表不保存进程内 AST 指针，不在 consumer
的名称环境中重新解释 provider 已绑定的名称。

Semantic 负责归一化、授权判定与检查条件；Symbol 层负责序列化和恢复；Codegen
消费已选成员和导出事实；LSP 调用同一授权接口。不得为 imported 成员、静态方法、
字段或 fit 分别增加一套 friend 规则。

主体准入、身份代入匹配和签名检查分别承担各自职责，共用统一的编译期事实。

### 5.2 语义接入

1. 在 `normalize_friend_member_annotations` 接纳有效 owner 泛参根引用，保留
   普通具体类型参数和复合泛型类型的现有校验；开放主体按第 3.5 节保留条件，
   具化后的主体仍须符合第一阶段准入范围。
2. 复用 `build_current_friend_subject`、`build_friend_owner_type_args` 和身份比较
   流程；保证原声明 spec 投影、fit 目标绑定和活动泛参身份准确。
3. 从 `friend_seal_member_is_accessible_from` 移除 fit 的本地／同包准入限制，
   将 imported friend 事实接入同一查询。
4. 对本地与跨包访问统一执行候选过滤和选择；候选探测不提交实际访问的签名诊断，
   避免未选中的 seal 重载遮蔽合法公开候选或产生无关错误。
5. 多项注解及代入后等价项继续归一化；如引入缓存，结果必须区分完整 owner
   实例、主体和实际访问上下文，不能只按成员或泛型声明缓存授权。

参数主体是否合法、签名是否可用，与“当前访问主体是否匹配”属于不同检查。
对前两者按第 3.5 节保留开放条件，身份不能证明相等时不得暂时授权。

### 5.3 签名可见性

现有声明期与 fit 访问点的签名检查继续承担各自职责。跨包后应按有效可见范围
检查完整签名：provider 的某个类型即使声明为 `open`，其 module 为 `seal` 时，
也不能据此向外包暴露。

声明时可确定的事实立即检查。若 friend 主体或签名的可用性依赖 owner 泛参，
保留待验证条件并在具化点完成检查，延续第 3.5 节的声明与访问检查边界。

检查递归范围引用可见性主规范，不重新维护简化的 friend 专用类型清单。推导
类型在推导完成后参与相同检查。

fit 使用授权时，仍从该 fit 的实际声明上下文检查签名。`F` 自身能够使用的
私有签名不必然能被第三个包中的 `fit F` 使用，主体相等不能替代签名检查。

### 5.4 Symbol、`.ft` 与导入恢复

对按既有规则可导出的 owner，将需要跨包消费的 friend 成员作为受限接口收录：

- 成员保留 `seal` 可见性；friend 不反向把不可访问的 owner 提升为公开 API。
- 字段布局、方法签名、泛参和必要的可具化依赖与 friend 类型表达式一起保留。
- 已收录的 spec requirement、mix 或 spec 实现依赖与 friend 事实可以共存，
  不能互相覆盖原有属性和授权来源。
- 具体 friend 与泛参 friend 使用同一收录和消费模型；类型引用依赖闭包只服务
  身份、签名和代码生成，不因此授予私有声明的普通名称访问权。
- package-public 与 workspace-cache 两种 profile 都完整保留各自需要的 friend
  事实；导出、读取、import 恢复和缓存比较保持一致。

`.ft` 保存的是 `F`、`Reader<T>` 等声明级类型表达式及绑定，不是 provider 某次
具化得到的 friend 集合。consumer 只依赖 `.ft` 和正式库完成消费，不读取 provider
源码，也不要求 provider 预先生成某个 consumer 类型的实例。

开放检查条件也必须随其泛型绑定保存和恢复，包括只出现在泛型函数体内部的
owner 类型使用。只导出成员上的 friend 类型表达式不足以覆盖这种情况：即使
外部函数签名未提及该 owner，消费方具化函数时仍须完成其内部依赖的检查。

编码优先考虑复用结构化类型引用、类型序列和扩展属性机制。具体 attr/section、
格式版本及兼容策略待 Review，不在本文分配未经确认的编号。读取器需验证成员
归属、参数作用域、类型引用完整性和记录合法性，缺失事实不得解释为已授权。

### 5.5 Codegen 与运行时边界

Semantic 放行后，provider 必须提供 consumer 真正可以调用或访问的制品：

- 实例和静态方法的稳定跨包入口，包括泛型 owner、泛型方法及 fit 共享入口。
- 实例字段所需的布局事实，以及静态字段的存储和初始化访问路径。
- spec 视角下既有 witness 和原声明成员身份。
- 方法值形成及共享泛型体中的 callable/reifiable 依赖。

成员的收录选择与链接符号归属应共用编译期事实，不能仅导出签名却保留不可链接
的内部实现。provider 和 consumer 必须使用一致的符号身份，不能依赖未导出的
私有声明顺序来推算入口。

编译期已选中的 friend 访问，在其他包具化时恢复其原始成员和词法上下文，不以
最终调用点所在包重新授予或撤销权限。

本次沿用既有布局、调用约定、描述符及共享泛型模型，不增加运行时权限表、检查、
分派层或 runtime API，不依赖全量单态化成立。新增可链接成员属于编译产物导出
范围调整，不能把它等同于成员变为 `open`。

如实现发现必须增加运行时开销或变更私有 runtime ABI，应提交人工决策，不在本
方案下自行扩展。

### 5.6 LSP 与诊断

补全、Hover、定义跳转和正常编译共享 Semantic 的主体、代入和授权判断，覆盖
源码与 `.ft` 导入成员。只有导出记录或原始注解、尚无有效语义事实时，不据此
放行 seal 成员。

现有 friend 参数、注解目标、签名以及成员访问诊断继续复用其职责。新增具化
诊断应指出非法类型实参或签名，并关联授权声明；是否扩展现有码的模板在主规范
阶段确定，不预先虚构诊断编号。

## 6. 验证计划

### 6.1 验证矩阵

| 验证层次 | 必须覆盖的内容 |
| --- | --- |
| Parser / Semantic | 直接类型级泛参、多个 friend、别名与重复项、未绑定或错误作用域的泛参；诊断码、位置及归一化身份 |
| 主体准入 | 保留 `@friend(int)`、数组等既有非法主体断言；补充泛参代入相同非法主体时的具化诊断，包括未访问成员以及跨包泛型转传的场景 |
| 授权精确性 | 正确与错误主体、不同 owner 实例、开放泛型身份、构造/终结/字段初始化及嵌套 lambda、顶层代码拒绝、未授权 seal 成员拒绝 |
| fit 双向行为 | fit 成员向外包授权；本包、friend 所属包和第三包的 fit 使用目标身份；直接具体 friend 与泛参 friend 结果一致 |
| 泛型与签名 | type/spec/fit owner 代入、父 spec 投影、泛型转传、包内私有签名、跨包不可见签名、实际 fit 上下文；开放条件在具化点检查，成员未被访问也不能绕过声明合法性检查 |
| Symbol | 两种 profile 往返、成员 seal 标记、friend 类型及泛参身份、开放检查条件及转传绑定、必要依赖、畸形记录拒绝、缓存一致性 |
| Codegen | 字段读写、静态存储与初始化、普通及泛型方法、fit、spec/witness、方法值、共享泛型依赖和稳定链接；无新增授权运行时检查 |
| LSP | 本地及导入成员的授权补全、Hover、定义跳转；无授权或事实未就绪时不放行 |
| FCTS | 语言可观察行为；仅提供 `.fb` 的两包和三包消费；第三包 fit、第三包具化，以及不同泛型实例的成员和静态状态隔离 |

编译器测试放在 `test/`，关注诊断、语义事实、符号表和生成代码；FCTS 关注语言
行为。跨包验证必须包含 provider 源码不可用的消费过程，不能只使用同次编译的
多 module 代替。

### 6.2 既有测试调整边界

现有测试中存在本阶段需要调整的旧契约断言：

- [test_semantic.c](../../test/semantic/test_semantic.c) 的
  `test_friend_declaration_and_access_diagnostics` 将 `@friend(T)` 判为非法。
  其中 `@friend(int)`、数组等非法主体断言在本阶段保留。
- [test_symbol.c](../../test/symbol/test_symbol.c) 的
  `test_friend_metadata_is_not_exported_to_ft` 验证不导出 friend 和外包 fit 不获权。

这些既有测试的修改需获得明确批准；本次文档整理不修改测试。保留无关回归覆盖，
不能通过删除旧负例覆盖来代替新增身份不匹配、签名不可见等负例。

[test_cli.c](../../test/cli/test_cli.c) 的
`test_lsp_external_package_hover_docs_and_completion` 还验证未授权上下文不补全
`hiddenFriend` 字段。这条不可见断言在扩展后仍应保留，并补充授权上下文的正例；
不能把元数据可导出误认为普通成员补全应当公开它。

## 7. 分步 TODO

以下任务按步骤推进，完成并验证对应交付后再勾选；当前均未实施。

### 步骤 1：完成实施前 Review

- [ ] 确认第 8 节的 `.ft` 记录布局、格式兼容与旧制品/缓存处理方案。
- [ ] 根据第 6.2 节列出既有测试的必要修改清单，并取得明确批准。

### 步骤 2：更新正式规范

- [ ] 更新可见性主规范，纳入直接类型级泛参、跨包 fit 授权和相应签名检查边界。
- [ ] 更新泛型主规范，定义开放检查条件的保留、绑定转传及具化点检查。
- [ ] 更新符号表主规范和相关包制品约定，明确成员收录、friend 事实及检查条件的保存和恢复。
- [ ] 更新关联文档引用和历史说明，保持规则只在所属主规范中定义。

### 步骤 3：实现 Semantic

- [ ] 接纳合法类型级泛参，统一主体准入、owner 绑定、原声明 spec 投影和精确身份匹配。
- [ ] 实现主体与签名条件的保留、转传及具化检查，保留声明期和实际 fit 访问点的检查职责。
- [ ] 移除 friend 专属的 fit 同包限制，统一源码与 imported 成员的候选过滤、授权和诊断。

### 步骤 4：实现 Symbol 与导入恢复

- [ ] 按确认后的格式收录受限成员、friend 类型表达式、开放检查条件及必要依赖。
- [ ] 实现 package-public 与 workspace-cache 的写入、读取、校验、import 恢复和缓存比较。
- [ ] 保留泛型函数体内部的检查依赖，确保消费方仅凭 `.ft` 和正式库即可在具化点检查。

### 步骤 5：接通 Codegen 与 LSP

- [ ] 补齐字段、静态存储、普通及泛型方法、fit、spec/witness 和方法值所需的跨包制品路径。
- [ ] 在共享泛型体及跨包具化中保留已选成员与原词法主体，遵守第 5.5 节的运行时边界。
- [ ] 使补全、Hover 和定义跳转复用同一 Semantic 授权，覆盖源码与 `.ft` 导入成员。

### 步骤 6：补齐测试与制品验证

- [ ] 按第 6.1 节补齐编译器用例，验证诊断码、位置、语义事实、符号表和生成代码。
- [ ] 按批准清单调整既有断言，保留本阶段非法主体及未授权访问的负例覆盖。
- [ ] 补齐 FCTS，覆盖两包、三包、第三包 fit 和第三包具化，并验证 provider 源码不可用时的消费。

### 步骤 7：完成回归与交付

- [ ] 完成相关专项验证，并在沙箱外执行完整 `make test`。
- [ ] 运行 `git diff --check`，记录验证结果及交付状态，提供英文 commit message，不自动提交。

编译及执行产物放在工程 `build/` 或 `temp/` 下。仅文档阶段不运行全量回归；任何
后续非文档变更均按仓库要求完成全量回归，不自动提交代码。

## 8. 待 Review 的设计细节

本阶段的主体范围、跨包 fit 身份、开放条件检查及既有签名行为已经确认。
实施前尚需明确以下制品设计；后续主体扩展的设计问题不属于本阶段前置条件。

| 项目 | 建议方向 | 需要确认的设计 |
| --- | --- | --- |
| `.ft` 编码、兼容与重建 | 优先复用结构化类型引用及扩展属性，完整保存 friend 事实、开放检查条件及其绑定，并保证库入口可用 | 按符号表主规范确定记录布局、格式演进及旧制品/缓存处理 |

## 9. 本次交付状态

本次仅拆分并整理第一阶段开发文档及 TODO，供 Review。正式规范、编译器、
runtime、标准库和测试均未修改；文中示例及验证矩阵是后续实施目标，
不是已通过的验证结果。
