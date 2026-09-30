# Feng `@friend` 类型级泛参与跨包支持开发方案

> **状态**：开发草案，待 Review；尚未修改正式规范、编译器或测试。
>
> **日期**：2026-09-30。
>
> **已确认方向**：`@friend` 支持引用类型级泛参，并支持跨包消费授权。
> `fit F` 使用目标类型 `F` 的 friend 身份，该规则同样适用于跨包 fit，
> 不再按 fit 与成员或 friend 类型是否同包增加授权限制。
>
> **文档定位**：本文记录本次扩展的需求、实现方案和验证计划。已确认的授权方向
> 与待 Review 的检查时机、制品格式分开记录。Review 通过后先更新主规范，
> 再实施代码和测试；本文不替代正式语言规范。

## 1. 目的与规范归属

当前 `@friend(Reader<T>)` 可以引用 owner 的泛参，但不允许直接使用
`@friend(T)`。具体 friend 类型必须在成员声明位置解析，且 friend 事实不进入
package-public `.ft`，因此现有能力不能供外包代码消费。

本次允许先用类型级泛参表达授权对象，再在成员所属类型的使用视角下代入。
提供方不需要反向依赖实际 friend 类型所在包，包依赖继续遵循既有无环规则。

本次保持 `@friend` 的成员级定向授权模型：类型实现上下文使用自身身份，
fit 方法使用目标类型身份；泛型和包边界不改变身份匹配的含义。

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

## 3. 已确认的授权方向

### 3.1 支持类型级泛参

新增允许在已有合法成员注解位置书写 `@friend(F)`，其中 `F` 来自成员 owner
的有效类型级泛参作用域。type、object-form spec 和 fit 目标提供的泛参绑定统一
处理；fit 不因此获得新的泛参声明语法。

既有 `@friend(Reader)`、`@friend(Reader<T>)` 等写法继续使用同一模型。
本次不扩展到方法自身声明的泛参，不允许通过注解创建未绑定泛参。

friend 参数依旧使用类型位语义。最终主体的合法性和开放泛参检查时机按第 7 节
完成 Review，不把“当前尚未解析为具体声明”直接当成非法，也不把未知类型当成
已经匹配的 friend。

### 3.2 fit 与类型实现使用统一身份

访问点的 friend 主体由词法实现上下文确定：

- type 的既有类型实现上下文使用当前 type 的完整语义身份。
- fit 的实例方法和静态方法使用 fit 目标类型的完整语义身份；嵌套 lambda
  沿用其词法实现上下文。
- 不属于上述上下文的代码不因持有一个 friend 对象或泛参而取得身份。

`fit F` 使用 `F` 的 friend 身份，不要求 fit 与被访问成员同包，也不要求 fit
与 `F` 的声明同包。第三个包中的合法 `fit F` 同样可以使用该身份。

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

## 4. 目标行为示例

以下代码展示目标行为，尚未作为当前实现可通过的测试。

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

### 5.2 语义接入

1. 在 `normalize_friend_member_annotations` 接纳有效 owner 泛参根引用，保留
   普通具体类型参数和复合泛型类型的现有校验。
2. 复用 `build_current_friend_subject`、`build_friend_owner_type_args` 和身份比较
   流程；保证原声明 spec 投影、fit 目标绑定和活动泛参身份准确。
3. 从 `friend_seal_member_is_accessible_from` 移除 fit 的本地／同包准入限制，
   将 imported friend 事实接入同一查询。
4. 对本地与跨包访问统一执行候选过滤和选择；候选探测不提交实际访问的签名诊断，
   避免未选中的 seal 重载遮蔽合法公开候选或产生无关错误。
5. 多项注解及代入后等价项继续归一化；如引入缓存，结果必须区分完整 owner
   实例、主体和实际访问上下文，不能只按成员或泛型声明缓存授权。

参数主体是否合法、签名是否可用，与“当前访问主体是否匹配”属于不同检查。
对前两者保留开放条件的机制按第 7 节确认，身份不能证明相等时不得暂时授权。

### 5.3 签名可见性

现有声明期与 fit 访问点的签名检查继续承担各自职责。跨包后应按有效可见范围
检查完整签名：provider 的某个类型即使声明为 `open`，其 module 为 `seal` 时，
也不能据此向外包暴露。

声明时可确定的事实立即检查。若 friend 主体或签名的可用性依赖 owner 泛参，
建议保留待验证条件，在代入后能够判定时完成检查；报错边界见第 7 节。

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

## 6. 验证与实施顺序

### 6.1 验证矩阵

| 验证层次 | 必须覆盖的内容 |
| --- | --- |
| Parser / Semantic | 直接类型级泛参、多个 friend、别名与重复项、未绑定或错误作用域的泛参；诊断码、位置及归一化身份 |
| 授权精确性 | 正确与错误主体、不同 owner 实例、开放泛型身份、构造/终结/字段初始化及嵌套 lambda、顶层代码拒绝、未授权 seal 成员拒绝 |
| fit 双向行为 | fit 成员向外包授权；本包、friend 所属包和第三包的 fit 使用目标身份；直接具体 friend 与泛参 friend 结果一致 |
| 泛型与签名 | type/spec/fit owner 代入、父 spec 投影、泛型转传、包内私有签名、跨包不可见签名、实际 fit 上下文；第 7 节确认后的报错边界 |
| Symbol | 两种 profile 往返、成员 seal 标记、friend 类型及泛参身份、必要依赖、畸形记录拒绝、缓存一致性 |
| Codegen | 字段读写、静态存储与初始化、普通及泛型方法、fit、spec/witness、方法值、共享泛型依赖和稳定链接；无新增授权运行时检查 |
| LSP | 本地及导入成员的授权补全、Hover、定义跳转；无授权或事实未就绪时不放行 |
| FCTS | 语言可观察行为；仅提供 `.fb` 的两包和三包消费；第三包 fit、第三包具化，以及不同泛型实例的成员和静态状态隔离 |

编译器测试放在 `test/`，关注诊断、语义事实、符号表和生成代码；FCTS 关注语言
行为。跨包验证必须包含 provider 源码不可用的消费过程，不能只使用同次编译的
多 module 代替。

现有测试中存在本次需要调整的旧契约断言：

- [test_semantic.c](../../test/semantic/test_semantic.c) 的
  `test_friend_declaration_and_access_diagnostics` 将 `@friend(T)` 判为非法。
- [test_symbol.c](../../test/symbol/test_symbol.c) 的
  `test_friend_metadata_is_not_exported_to_ft` 验证不导出 friend 和外包 fit 不获权。

这些既有测试的修改需获得明确批准；本次文档整理不修改测试。保留无关回归覆盖，
不能通过删除旧负例覆盖来代替新增身份不匹配、签名不可见等负例。

[test_cli.c](../../test/cli/test_cli.c) 的
`test_lsp_external_package_hover_docs_and_completion` 还验证未授权上下文不补全
`hiddenFriend` 字段。这条不可见断言在扩展后仍应保留，并补充授权上下文的正例；
不能把元数据可导出误认为普通成员补全应当公开它。

### 6.2 实施顺序

1. Review 本文，确认第 7 节检查时机和制品方案，以及既有测试的必要调整范围。
2. 先更新可见性、泛型和符号表主规范；其他规范、手册与历史说明只更新关联引用。
3. 实施统一 friend 事实、Semantic 代入和授权、Symbol 往返及 imported 恢复。
4. 补齐 Codegen 链接入口、共享泛型依赖和 LSP 消费。
5. 新增各层测试，并在获准后调整受影响的既有断言，完成跨包制品验证。
6. 沙箱外执行完整 `make test`，通过后运行 `git diff --check`，记录交付证据。

编译及执行产物放在工程 `build/` 或 `temp/` 下。仅文档阶段不运行全量回归；任何
后续非文档变更均按仓库要求完成全量回归，不自动提交代码。

## 7. 待 Review 的细节

跨包 fit 使用目标类型 friend 身份已经确认，不属于待决策项。以下内容尚未在
讨论中明确最终检查时机或具体编码，不能作为已批准实现直接落地。

| 项目 | 建议方案 | 需要确认的边界 |
| --- | --- | --- |
| 泛参代入后不是合法 friend 主体 | 沿用现有具体 `type` 主体限制；闭合后的 `Vault<int>` 等非法使用在编译期报错，不静默变为空授权 | 建议在形成可判定的 owner 类型使用时检查，即使尚未访问该成员；需要确认是否按此作为具化合法性条件 |
| 主体或签名条件仍依赖开放泛参 | 将未决合法性条件作为编译期泛型事实保留并转传，在可判定的代入点检查；不放宽无法证明相等的访问主体 | 需确认允许这种延后验证及其跨包传递；覆盖只出现在签名或预编译泛型体内部的使用，避免仅在构造或访问点检查 |
| 代入后签名对某个 friend 不可用 | 延续现有每个 friend 都须可用的原则，建议使该次 owner 类型使用非法；fit 额外的上下文不可用只拒绝该次 fit 访问 | 确认前者是否在具化时拒绝，而非等对应成员被访问；两类诊断不得混淆 |
| `.ft` 编码、兼容与重建 | 优先复用结构化类型引用及扩展属性，遵循符号表主规范的兼容规则 | Review 确定记录布局、格式演进及旧制品/缓存处理；不得静默丢失会影响正确编译的事实，也不能只更新 `.ft` 而漏掉所需库入口 |

## 8. 本次交付状态

本次仅新增开发草案，供 Review。正式规范、编译器、runtime、标准库和测试均未
修改；文中示例及验证矩阵是后续实施目标，不是已通过的验证结果。
