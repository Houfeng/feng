# Feng `@friend` 第一阶段：泛型与跨包支持开发方案

> **状态**：阶段一实现、P12／P17 修复、P18／P19 收敛及 Apple container 全量回归已完成。
> P21 Review 修复及对应的 Apple container 完整回归已完成。
> 开发者于 2026-10-08 确认本轮优化后的 macOS 本机手动验证已完成。
> 后续统一依赖优化已完成；两项既有公共问题按用户决定移交独立 bugfix，
> 不作为本次完整交付的条件，详见[优化方案](./feng-annotation-dependency-optimize-dev.md)。
>
> **日期**：2026-09-30。
> **最近核对**：2026-10-08。
>
> **已确认范围**：在现有具体 `type` 主体范围内支持类型级泛参与跨包。
> `fit F` 使用目标类型 `F` 的 friend 身份，该规则同样适用于跨包 fit，
> 不再按 fit 与成员或 friend 类型是否同包增加授权限制。
>
> **文档定位**：本文独立记录第一阶段的需求、实现方案、验证计划与分步 TODO。
> 已确认的授权方向与实施决策分别记录。实施时先更新主规范，
> 再实施代码和测试；本文不替代正式语言规范。

## 1. 目标、范围与规范归属

实施前，`@friend(Reader<T>)` 可以引用 owner 的泛参，但不允许直接使用
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

## 2. 实施前的实现依据

以下记录实施前核对的基线；实施进度和验证结果以第 7、9、10 节为准：

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

以下代码展示第一阶段目标行为；各层实际验证结果见第 9 节。

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

声明检查、具化条件收集和只读查询共用成员签名遍历及显式／推断类型读取。
各调用方保留原有检查顺序、类型绑定和诊断时机；只读查询不记录新的语义事实。

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

本阶段复用现有 `.ft` 固定结构，不修改文件头、section 目录或既有记录布局，
不新增 section，也不提升格式版本。语义信息通过现有载体表达：

| 语义信息 | 复用载体 |
| --- | --- |
| 成员与泛参声明归属 | `SYMS` 的符号及 owner 关系 |
| `F`、`Reader<T>` 等类型表达式及绑定 | `TYPS` 的类型参数引用、泛型类型节点和 `TSEQ` 类型序列 |
| 成员的 friend 集合及泛型声明的待检查依赖 | `ATRS` 新增属性类型，引用已有符号和结构化类型记录 |

新增属性类型需要实现相应写入、读取、导入恢复和校验，不改变固定记录的外形。
属性编号及 `value` 字段含义在符号表主规范中定义。读取器需验证成员归属、参数
作用域、类型引用完整性和记录合法性，缺失事实不得解释为已授权。

语言尚未公开发布，本阶段不提供旧制品兼容、迁移或桥接，也不承诺旧 reader
消费新属性。更新编译器后，统一重编 provider、consumer 的 `.ft`、`.fb`、配套库
及缓存，不能仅更新符号文件后链接旧库；不为旧格式额外增加兼容处理。

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

构造调用与导入方法共用原声明的共享 ABI 原型生成入口及去重机制，保留既有
参数顺序、描述符域和 P10 的参数适配，不改变生成调用的行为。

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

以下两类既有测试调整已获用户明确批准：

- [test_semantic.c](../../test/semantic/test_semantic.c) 的
  `test_friend_declaration_and_access_diagnostics` 中，将类型级 `@friend(T)` 的
  声明拒绝预期改为允许声明，并补齐条件保留及具化检查。
  其中 `@friend(int)`、数组等非法主体断言在本阶段保留。
- [test_symbol.c](../../test/symbol/test_symbol.c) 的
  `test_friend_metadata_is_not_exported_to_ft` 中，将不导出 friend、外包 fit 不获权
  的预期改为验证授权事实经 `.ft` 保存和恢复，身份匹配的跨包 fit 可以消费授权。

实施时可在上述批准范围内直接调整，无需重复确认。
无关回归覆盖继续保留，不能通过删除旧负例覆盖来代替新增身份不匹配、签名不可见
等负例。第二阶段的主体扩展测试不属于本次批准范围。

[test_cli.c](../../test/cli/test_cli.c) 的
`test_lsp_external_package_hover_docs_and_completion` 还验证未授权上下文不补全
`hiddenFriend` 字段。这条不可见断言在扩展后仍应保留，并补充授权上下文的正例；
不能把元数据可导出误认为普通成员补全应当公开它。

## 7. 分步 TODO

以下任务按步骤推进，完成并验证对应交付后再勾选。步骤 1 已确认，后续规范、
代码与测试任务按实际验证结果更新。

### 步骤 1：完成实施前 Review

- [x] 确认复用现有 `.ft` 固定结构及扩展属性，不做旧制品兼容，统一重编制品和缓存。
- [x] 明确并批准第 6.2 节的两类既有测试调整，保留本阶段仍适用的负例覆盖。

### 步骤 2：更新正式规范

- [x] 更新可见性主规范，纳入直接类型级泛参、跨包 fit 授权和相应签名检查边界。
- [x] 更新泛型主规范，定义开放检查条件的保留、绑定转传及具化点检查。
- [x] 更新符号表主规范和相关包制品约定，明确成员收录、friend 事实及检查条件的保存和恢复。
- [x] 更新关联文档引用和历史说明，保持规则只在所属主规范中定义。

### 步骤 3：实现 Semantic

- [x] 接纳合法类型级泛参，统一主体准入、owner 绑定、原声明 spec 投影和精确身份匹配。
- [x] 实现主体与签名条件的保留、转传及具化检查，保留声明期和实际 fit 访问点的检查职责。
- [x] 移除 friend 专属的 fit 同包限制，统一源码与 imported 成员的候选过滤、授权和诊断。

### 步骤 4：实现 Symbol 与导入恢复

- [x] 通过既有 `.ft` 结构和扩展属性收录受限成员、friend 类型表达式、开放检查条件及必要依赖。
- [x] 实现 package-public 与 workspace-cache 的写入、读取、校验、import 恢复和缓存比较。
- [x] 保留泛型函数体内部的检查依赖，确保消费方仅凭 `.ft` 和正式库即可在具化点检查。

### 步骤 5：接通 Codegen 与 LSP

- [x] 补齐字段、静态存储、普通及泛型方法、fit、spec/witness 和方法值所需的跨包制品路径。
- [x] 在共享泛型体及跨包具化中保留已选成员与原词法主体，遵守第 5.5 节的运行时边界；
  P17 的额外依赖已移除，实际使用所需的原有槽位与访问映射已验证保留。
- [x] 使补全、Hover 和定义跳转复用同一 Semantic 授权，覆盖源码与 `.ft` 导入成员。

### 步骤 6：补齐测试与制品验证

- [x] 按第 6.1 节补齐编译器用例，验证诊断码、位置、语义事实、符号表和生成代码。
- [x] 按批准清单调整既有断言，保留本阶段非法主体及未授权访问的负例覆盖。
- [x] 补齐 FCTS，覆盖两包、三包、第三包 fit 和第三包具化，并验证 provider 源码不可用时的消费。
- [x] 补充仅出现在 friend 注解及其他纯检查位置的类型不产生运行时依赖的用例，修复 P17 后重新全量回归。

### 步骤 7：完成回归与交付

- [x] 完成最终实现的相关专项验证，并在沙箱外的 Apple container 执行完整
  `make test`；含 P18／P19 收敛的 Linux ARM64 ASan/UBSan 与普通构建两阶段均已通过。
- [x] 开发者于 2026-10-08 确认本轮优化后的 macOS 本机手动验证已完成。
  P14 保留原始失败及复核记录，不据此宣称其首次失败根因已修复。
- [x] 运行 `git diff --check`，记录验证结果及交付状态，提供英文 commit message，不自动提交。

编译及执行产物放在工程 `build/` 或 `temp/` 下。仅文档阶段不运行全量回归；任何
后续非文档变更均按仓库要求完成全量回归，不自动提交代码。

## 8. 已确认的实施决策

本阶段的主体范围、跨包 fit 身份、开放条件检查及既有签名行为已经确认。
实施前的两项确认已完成，后续按 TODO 更新主规范并实现；属性编号和字段映射
在主规范阶段落实。后续主体扩展的设计问题不属于本阶段前置条件。

| 项目 | 确认结果 |
| --- | --- |
| `.ft` 结构与制品处理 | 按第 5.4 节复用现有固定结构及扩展属性，不做旧制品兼容或迁移，统一重编相关制品和缓存 |
| 既有测试调整 | 第 6.2 节列出的两类调整已获批准，不扩大到第二阶段或无关用例 |

## 9. 本次交付状态

2026-10-08：本次第一阶段 friend 泛型与跨包支持、P21 Review 修复及后续统一依赖
优化均已完成。最新实现与 Linux 完整回归结果见
[优化方案](./feng-annotation-dependency-optimize-dev.md#13-实际变更与验证记录)；
开发者已确认本轮优化后的 macOS 本机手动验证完成。

按用户决定，[泛型依赖无限增长](./feng-generic-reification-growth-bugfix.md)与
[参数包过度传播](./feng-generic-arguments-propagation-bugfix.md)均作为独立 bugfix
后续处理，不作为本次完整交付的条件。两项问题仍然存在，未标记为已修复。
本节以下保留 P21 之前的历史验证证据，P21 结果见对应问题记录；未提交代码。

截至 2026-10-08：

- P21 前的代码（含 P12、P16、P17、P18、P19）的完整 `make test` 已在沙箱外的 Apple
  container Linux ARM64 GNU 环境中通过，退出码为 0。使用 Clang 22.1.8、
  `LANG=C.UTF-8 LC_ALL=C.UTF-8` 和独立源码、构建目录；按 SHA-256 核对 1435 个
  源码、构建及测试文件，确认容器回归覆盖最终工作区实现。
- ASan/UBSan 与普通构建两个完整阶段均通过：每阶段标准库 607/607、FCTS
  1674/1674；包含 Semantic、Symbol、Codegen、CLI/LSP 和生命周期测试。普通阶段
  的增量构建、发布脚本、打包、工具链和插件检查也全部通过。
- 新增验证包括两种 FT profile、每种 27 类畸形 friend 属性、源码不可用的
  provider/middle/consumer、第三包 fit/具化、8 项 LSP 查询、静态状态隔离和精确
  诊断位置及原授权声明关联信息。
- P12 补充验证包括跨模块最小私有依赖闭包、不同导入包的可见性、缺失依赖拒绝、
  完整签名的只读授权查询、私有泛型布局与调试字段映射，以及 provider 源码删除后
  的真实制品编译和运行。短名、别名、完整路径均覆盖普通访问及 LSP 隔离；新增
  两项 FCTS 验证 provider friend、外包 fit、第三包具化和 managed 字段生命周期。
- P14 在本轮 Linux 两阶段均未复现；开发者反馈的优化前 macOS 完整回归末尾也
  正常完成。但首次 macOS 失败根因仍未确定，不能据此标为已修复。
- 完整回归日志：`temp/friend-optimization-20261008/make-test.log`；退出码及源码
  校验清单：同目录的 `make-test.exit`、`source-sha256.txt`。结束后再次核对容器
  及工作区均一致；上述证据对应当轮 Linux 验证。
- P17 已通过变更前基线及成对生成代码确认并实现修复。新增用例验证纯检查不占用
  运行时依赖槽位、真实字段及方法依赖保留、两种 FT profile 的具化检查继续生效；
  全部 Codegen 用例及新增跨包 FCTS 均在两阶段完整回归中通过。
- P18 的 6 组优化前后对照中，可执行 C、去重后的完整共享 ABI 原型集合及 public
  FT 均一致；3 组声明、具化及只读查询样本的诊断输出一致。新增签名组成部分、
  首个诊断顺序、只读事实以及共享构造用例在完整回归的两阶段均通过。
- P19 的负向用例同时断言非零退出和预期诊断码；完整回归两阶段日志均不再直接
  输出新增 friend 用例的预期诊断。专项日志、生成产物及对照摘要保存在上述目录，
  摘要为 `comparison-summary.txt`。
- 本轮 `git diff --check` 通过；未提交代码。

## 10. 实施问题记录

按“先记录、再分析、修复并补齐用例”维护本节；已确认的功能缺口由上述 TODO 跟踪。

### P1：导入注解数组所有权核对

- 初步记录：曾将 `free_synthetic_annotations` 仅释放 `args` 判断为遗漏 `type_args`。
- 核实结论：`FengAnnotation.args` 与 `type_args` 是 union，共用一个数组指针；原实现正确。
- 处理：撤销误加的第二次释放。该判断错误导致的验证失败与修复见 P3。

### P2：新检查器的查询表所有权与只读上下文不匹配

- 发现：增量构建拒绝将 `ResolveContext` 的只读表地址传入分配接口。
- 分析：查询表由检查器拥有，解析上下文仅借用；分配应使用独立的可写局部指针。
- 处理：分离分配与借用，统一释放查询表；重新构建并进行内存生命周期验证。

### P3：Symbol 专项异常退出

- 发现：Semantic 全部通过后，`build/bin/test_symbol` 以 134 退出，尚无失败断言文本。
- 分析：沙箱外 LLDB 栈定位到 `free_synthetic_annotations`；本次误加的释放对 union
  的同一指针释放了两次，与 P1 的初步判断相反。
- 修复：恢复数组仅释放一次；由 friend FT 往返及完整 Symbol 测试验证。

### P4：外包函数体内的泛型条件未在调用点触发

- 发现：新增仅依赖 FT 的 consumer 用例中，`Vault<int>` 正确拒绝，但调用 provider
  的 `forward<int>()` 未报 `AE1336`；provider 函数体包含 `Vault<T>`。
- 分析：原恢复入口只在 Semantic 成功后由 CLI 为 Codegen 调用，Semantic 阶段没有
  provider 函数体依赖。导出结果正确，丢失发生在阶段时序边界。
- 修复：通过 provider-neutral module 回调在 Semantic 检查前恢复事实；记录已恢复
  provider，保持后续 Codegen 调用幂等。负例现已产生 `AE1336`，继续覆盖两种 profile。

### P5：导入私有签名类型缺少声明身份绑定

- 发现：`Sensitive<F>` 的 friend 字段使用 provider 的私有 `Hidden`，消费方具化
  `Sensitive<Local>` 未报 `AE1338`，同一源码路径的检查与制品路径不一致。
- 分析：合成 AST 只保留完整类型名，查询完整路径会过滤私有声明，导致签名扫描
  得不到已被 FT 保存的私有类型身份；不能借此把该类型当作无需检查。
- 处理：导入时从结构化 Symbol 类型引用恢复声明身份，并在 friend 归一化前完成
  编译期事实恢复；该绑定用于 provider 的既有引用，不开放 consumer 的普通名称访问。

### P6：跨包泛型静态 friend 字段缺少初始化入口声明

- 发现：三包 FCTS 构建 middle 包时，C 编译器报告
  `FengGenericStaticEnsure__...__marker` 未声明；Semantic 和 FT 已允许该字段访问。
- 原因与修复：导入的开放泛型静态 binding 路径遗漏 ensure 原型；补齐同一 binding
  路径的声明。三包静态存储、初始化和方法值用例已通过，继续验证状态隔离。

### P7：普通具化依赖被误扩展为公开链接入口

- 发现：既有 Codegen 回归要求仅作为具化依赖选中的 private shared body 保持 static；
  本次把 package selection 直接作为 callable 导出条件后，该断言失败。
- 分析：符号依赖闭包与受限可调用接口是不同事实；只有 friend、mix、已选 spec
  实现等受限接口需要跨包链接，普通具化依赖不因此获得外部链接。
- 修复：在既有接口判定上仅增加归一化 friend 事实，保持原依赖链接策略和既有断言。

### P8：第三包具化泛型 spec 方法值失败

- 发现：新增 FCTS 的 middle 包泛型 reader 从 `GenericFriendSurface<Reader<T>>`
  形成 `read` 方法值；在 bin 包具化时报 `IE0002`，提示 owner 不是直接调用方泛参。
- 原因：方法值依赖收集仅检查接收者是否含泛参，将对象 spec 实例误记为受约束泛参。
- 修复：只有直接泛参接收者使用该具化依赖；对象 spec 值继续使用已有 witness
  方法值路径。补充普通泛型 spec 方法值 Codegen 用例和三包 friend FCTS。

### P9：CLI 专项回归读取 std 调试边车失败

- 发现：既有 DAP 栈列号用例读取 `std/std/build/.../libstd.a.fd` 失败，尚未运行到
  新增 friend CLI 用例。
- 结果：完整 `make test` 的 clean/build 后，该既有用例已通过，日志确认三个真实
  栈停止位置均为第 1 列。未修改既有用例或编译器行为。

### P10：部分开放 owner 的构造调用使用了错误 ABI

- 发现：源码不可用制品用例中的 `forward<T>` 构造 `Vault<T,int>(1)`，生成的
  构造原型把共享入口的泛型参数指针写成 `int64_t`，与定义冲突。
- 修复：构造原型及参数适配均使用原声明的共享 ABI，复用普通泛型调用的参数
  适配器；源码删除后的 provider/middle/consumer 制品编译与执行已通过。

### P11：缓存导入泛型静态成员补全缺失

- 发现：源码删除后的 LSP 用例中，实例字段、方法和 fit 授权补全通过，
  `Vault<Reader<T>,T>.shared` 静态成员补全未出现。
- 原因：接收者已有语义类型事实时，泛型类型目标被提前作为实例返回；此外缓存
  补全未借用当前语义快照。
- 修复：类型目标先保留静态身份；仅在所有打开文档版本一致时，缓存查询借用完整
  已分析 AST 进行权限判断。8 项源码不可用补全、Hover、定义跳转专项已通过。

### P12：私有 module 的签名依赖在公开制品中丢失（已修复，容器全量通过）

- 复现：provider 的 `seal module hidden` 定义 `open type Payload`；公开模块
  `api` 中声明 `Vault<F> { @friend(F) seal let value: Payload; }`。
  打包后 consumer 声明自己的 `Reader`，仅使用 `Vault<Reader>`，修复前检查错误地通过。
- 原因：现有公开包导出只写公开 module，`hidden.Payload` 的身份和有效可见性
  未进入制品，恢复条件时无法判断该签名对外包 friend 不可见。
- 已确认处理（2026-10-05）：复用已有私有依赖收录机制，补齐必要类型和模块元信息，
  导入后执行既有检查。收录边界统一定义在[符号表规范 §5.1.1](../specifications/feng-symbol-table.md#511-私有表示依赖闭包)。
  friend 只提供词法上下文并复用通用可见性判断，不自行感知包归属；不增加孤儿 friend
  规则，也不另建一套签名可见性条件编码。
- 具体丢失链路：`feng_symbol_export_graph` 跳过 seal module；
  `writer_select_type_dependencies` 只收录当前 module 内的名义类型依赖。因此
  `api.ft` 中的 `hidden.Payload` 引用没有对应的隐藏模块声明供消费端恢复。
  P5 能从当前模块的 FT 恢复私有类型；本问题中整个目标模块未被输出，不能复用
  P5 的绑定修复就获得缺失声明。当前检查器跳过无法恢复的签名目标，形成漏报。
- 为什么泛型暴露此边界：原有具体 friend 的签名检查可在声明包处理；泛型 friend
  的实际身份要到消费包具化时才确定，检查所需的声明事实必须随制品传递。即使
  consumer 只在参数类型中使用 `Vault<Reader>`，尚未访问成员，也必须执行条件检查。
- 分步 TODO：
  - [x] 更新主规范，明确必要私有依赖与普通源码可见性的边界。
  - [x] 将现有依赖闭包扩展到同包模块，复用 FT 2.0 格式及稳定声明身份。
  - [x] 恢复必要依赖与通用可见性上下文，替换 friend 内的包来源比较及缺失事实跳过。
  - [x] 新增源码、两种 FT profile、独立包及源码不可用用例，覆盖合法与非法具化、
    实际 fit 访问、递归依赖、最小收录、普通名称隔离及缺失制品依赖。
  - [x] Apple container 中运行完整 `make test`，记录结果；本机验证按步骤 7 单独跟踪。
- 容器复核（2026-10-05）：当前代码在 Linux ARM64 下打包成功，`.fb` 目录中仅有
  `mod/api.ft`，没有 `hidden` 模块表；consumer 的 `func accept(value: Vault<Reader>) {}`
  检查退出码为 0，再次确认该漏报。此复现与 macOS 本机的执行拦截无关。
- 修复过程记录：首次增量构建发现旧 `visibility_is_public` 辅助函数随统一闭包入口
  替换后已无调用，被 `-Werror` 拒绝；删除该冗余函数后继续验证。
- 首轮专项：现有 Semantic 测试全部通过；新增递归依赖用例误用了禁止的开放泛型
  指针 `Payload<T>*`，触发既有 `AE0333`。调整新用例为递归引用与闭合指针两条依赖，
  保持既有指针规则不变，再继续 Symbol 验证。
- 第二轮新用例构造递归字段时触发既有 `AE0332`（默认零值不终止）。将递归边改为
  数组，保留闭包递归覆盖，并让默认构造合法；未调整旧用例或零值语义。
- 第三轮新用例未给方法值指定 callable-form spec，先触发 `AE0523`。该用例改为
  直接调用返回私有签名类型的方法，以单独验证目标场景的 `AE1338`。
- 第四轮发现真实边界缺口：私有模块进入制品后，消费者的完整路径类型引用
  `privateft.hidden.Payload<int>` 被接受。必须检查普通类型解析入口，防止内部身份
  恢复绕过模块可见性；该负例保留，修复后与显式 import、alias、LSP 一并验证。
- 完整路径漏报原因：正常类型查找已拒绝不可见模块，但泛型诊断的 arity 回退查询
  又取出了其中的同名声明，并在实参数量匹配时返回成功。回退查询与 alias 路径统一
  复用模块可见性判断；不可见类型沿用既有 `AE1013`，不增加 friend 专用规则。
- CLI 新增私有泛型值类型运行用例在 provider 编译时触发 `CE0066`：访问
  `Vault<Reader<T>, T>.payload.value` 时缺少 descriptor dependency。先保留复现，
  追踪共享泛型字段读取的描述符依赖；修复只能补齐编译期事实，不能增加运行时开销。
- 已确认该错误的原因：语义依赖保留了正确的 `resolution_decl`，Codegen 却将已绑定
  的完整路径重新按公开名称查找；私有模块使查询失败，留下空描述符映射槽。类型和
  spec 的内部查找应优先使用已绑定声明身份，未绑定引用仍走原名称解析。此修复遵循
  [符号表规范的内部身份恢复规则](../specifications/feng-symbol-table.md#511-私有表示依赖闭包)，
  不改变源级可见性、描述符布局或运行时 ABI。
- 该修复后最小 provider 已能编译。CLI 回归先在既有 DAP 用例遇到标准库 `.fd`
  字段映射冲突（`List<std.Action<T>>.items`）；尚不能断定是旧产物还是实现问题，
  需要重建标准库后复核，不能跳过该用例作为回归通过。
- 清理并重建标准库及标准库测试后，`.fd` 冲突仍可复现，已排除旧产物因素；继续
  核对声明身份优先查找与原泛型类型身份、调试字段映射之间的不一致。
- 冲突原因已定位：从 Codegen 类型重建的名义引用保留了 `resolution_program`，
  调试名称规范化却忽略该来源，用容器类型的声明文件解析其字段实参，分别产生
  `Action<T>[!]` 与 `std.Action<T>[!]`。调试名称规范化同样遵循引用自身的来源信息，
  不改变字段布局或调试信息合并规则；补充跨模块 callable 实参的字段映射断言。
- 修复后标准库 607/607 通过。新增 Codegen 用例的 callable spec 漏写 `:void`，
  被既有 `SE0607` 拒绝；只修正该新用例语法，继续执行后续检查。
- Codegen 全套及新增依赖、调试字段断言已通过。真实 `.fb` 消费端随后触发
  `CE0031`（`Payload<...>` 未注册），说明源码内的绑定恢复还不足以覆盖跨模块 FT
  依赖。保留无 provider 源码的用例，继续追踪导入引用的来源与实例注册路径。
- 导入路径存在同类遗漏：签名与 friend 条件已恢复 `resolution_decl`，运行时聚合、
  managed、callable 及投影依赖仍只合成名称树。统一这些依赖的类型合成入口，在所有
  模块骨架可用后恢复其声明身份，使实例注册与签名解析使用同一目标。
- 真实制品消费者已能编译并运行，但 LSP 在外包 `fit Reader<T>` 中仍补全 `payload`；
  该字段的私有模块签名对 fit 不可见。原因是补全只复用了旧的“成员所属模块中的
  seal type”扫描，没有检查具化后签名涉及的其他模块。补全应只读检查完整签名，
  与具化诊断共用声明可见性谓词，保留现有诊断阶段和未选中重载不报错的行为。
- 新增只读查询矩阵在复用 imported-module cache 后，仅 import provider 的合法 fit
  分析失败且没有诊断。保留该用例，先检查缓存中已加载但本次未引用的模块是否被
  错误附加到当前 analysis，避免把内部失败掩盖成语义拒绝。
- 已确认缓存恢复遍历了全部历史 entry，为本次未加载模块附加了依赖；构建条件图时
  找不到这些声明的词法 module/program，返回失败。恢复入口限定为当前 analysis
  已纳入的 program，缓存仍可保留并复用其他 entry，不改变导入语义。
- 缓存修复后两种 FT profile 和真实 bundle 的完整签名查询均通过。CLI 新测试的
  就绪条件误把泛型补全标签写作 `Reader`；现有 LSP 返回 `Reader<T>`，因此等待超时。
  修正新测试的就绪条件及对应 `Payload<T>` 排除断言，保留协议就绪等待。
- 更正泛型标签后，负例实际检出了普通补全泄漏：无 alias 的非法私有模块 import
  已报 `AE0902`，但文件级补全仍列出 `Payload<T>`。继续修复普通模块候选的可见性
  过滤，不能依赖整文件分析成功来隔离已随 FT 收录的私有声明。
- 补全修复后，补充的 Hover/Definition 负例继续检出 AST 名称查询的相同遗漏；
  普通类型和值查询的符号缓存、AST 回退两条路径统一检查模块可见性。
- 最终验证（2026-10-07）：两种 FT profile 与独立 bundle 的诊断、只读查询矩阵通过；
  真实制品运行和 LSP 正反例通过。随后完整 `make test` 两阶段均通过，每阶段标准库
  607/607、FCTS 1672/1672；结果及日志见第 9 节。本次修复保留原可见性、FT 2.0
  结构与运行时 ABI，friend 复用通用可见性机制。

### P13：推断数组构造遗漏 array fit 的声明条件

- 发现：`fit T[] { @friend(T) seal func hidden() {} }` 下，泛型体中的
  `let xs = T[:1]` 未把该 fit 的主体条件传到 `create<int>()`。
- 原因：只收集了数组元素类型及运行时数组依赖，未记录数组表达式本身的编译期条件。
- 修复：保留数组表达式的完整推断类型，并纳入独立的编译期依赖；直接构造、跨泛型
  调用和两种 FT profile 的跨包转传负例已通过。

### P14：ASan/UBSan 全量回归的既有 LSP 响应断言失败

- 发现：沙箱外 `make test` 的 `test-sanitize` 在 `test_cli.c:12001` 失败，
  断言为 `next_response == NULL || match < next_response`。该轮 Semantic、Codegen
  与新增 friend 制品及 8 项 LSP 用例已通过；失败发生在 Symbol sanitizer 执行之前，
  当时 Symbol 仅有普通构建专项通过的证据。
- 状态：保留既有断言，定位具体请求和响应内容后修复；该轮回归未进入普通构建阶段，
  不能记为全量通过。
- 复核：定位到既有注解参数 Hover/定义跳转矩阵；单独连续重放 10 轮、共 40 组
  源码/制品与正常/错误恢复组合，以及原 LSP 用例顺序重放均通过。尚未确定首次
  失败根因，未将其标为已修复。
- 容器复核（2026-10-05）：完整 `make test` 的 ASan/UBSan 与普通构建两阶段均通过，
  包括该既有矩阵；未复现原失败。macOS 本机复核按 P15 的人工安排等待授权窗口。
- P12 修复后复核（2026-10-07）：Linux 完整 `make test` 两阶段再次通过，包括原
  LSP 矩阵；仍不据此判定 macOS 首次失败已修复。
- 最新确认（2026-10-08）：开发者已手动完成本轮优化后的 macOS 本机验证。
  本机验证待办已关闭；此反馈不等同于定位或修复首次失败根因。

### P15：sanitizer CLI 的既有 argv 运行断言失败

- 发现：第二轮沙箱外 `make test` 的 `test_cli` 在 `argv_lifetime.inc:20` 失败，
  `system(command)` 返回非零；同一生成程序单独运行成功。此前临时完整 CLI 重放
  也在同一处失败，尚不能归因于临时调试驱动。
- 状态：先捕获原始返回状态和 errno，核对编译产物及运行环境；保留原断言，未更改
  语言行为或 runtime。第二轮同样未进入普通构建阶段。
- 证据：临时观测驱动保留原用例和断言，捕获 `system result=9`、`errno=0`、
  `WIFSIGNALED=1`、`WTERMSIG=9`；对应产物代码签名校验通过，系统日志显示执行
  获准。该阶段的进程证据尚不能确定 SIGKILL 的发起方。
- 后续：单独执行完整普通构建回归，继续验证不依赖 sanitizer 进程终止问题的部分；
  不删除原断言、不忽略非零退出，也不据此声称完整 `make test` 已通过。
- 普通构建也出现同类终止：`make test-normal` 的 91 项 smoke、CLI direct/project
  检查通过后，`run_cli_init_bundled_packages.sh:46` 中的 `feng init` 被 signal 9
  杀死，后续 manifest 不存在。当前现象不限于 sanitizer 产物，继续核对该原始进程。
- 隔离证据：仅包含 `int main(void) { return 0; }` 的 C 源文件，经 host Clang
  输出到仓库 `temp/` 后立即执行，同样收到 `Killed: 9`、脚本退出码 137。该复现
  不包含 Feng 编译器生成代码或 Feng runtime，已将回归阻塞提交人工处理执行环境。
  没有更改系统执行策略，也没有将已失败的完整回归计为通过。
- 人工确认（2026-10-05）：本机安全软件要求开发者在 7 秒内授权，否则会拦截执行。
  开发者安排先在本机 Apple container 中执行回归，待其方便及时授权时再执行 macOS
  本机回归。容器使用独立源码副本和构建目录，完整运行 `make test`；Linux 结果与
  尚未通过的 macOS 本机结果分别记录。
- 容器结果：Linux ARM64 GNU 的完整 `make test` 退出码为 0，两个阶段均通过，
  包括原 argv 生命周期与 `feng init` 用例。未修改系统安全设置或放宽测试断言。

### P16：具化诊断缺少授权声明关联信息

- 发现：`AE1336` 具化错误当前仅报告非法主体类别，`AE1338` 仅报告成员及不可见
  类型，未提供第 5.6 节要求的授权声明关联信息。
- 处理范围：复用既有 related-information 诊断机制，保留具化点主诊断，并补齐
  非法实参文本与原授权声明位置；不改变诊断码或授权判断。
- 修复与验证：有限条件仅额外借用实参文本用于错误显示；`AE1336`、`AE1338`
  均关联原授权声明。源码精确主/关联位置、实参文本和两种 FT profile 的关联信息
  均随普通 Semantic/Symbol 专项通过，Codegen 普通专项也已重跑通过。
- 最终代码复核（2026-10-05）：上述诊断变更已包含在本轮 Linux 容器完整
  `make test` 的两个通过阶段中。

### P17：纯 friend 注解产生非必要的运行时依赖（已修复，全量回归通过）

> 后续状态（2026-10-08）：已批准按[统一依赖优化方案](feng-annotation-dependency-optimize-dev.md)
> 调整本节实现；下列结果保留为历史记录，不作为新方案的验证结论。

- 发现（2026-10-07）：审查是否存在非必要改动时，发现 `collect_for_type` 将
  friend 注解参数传入 `try_collect_type_ref`；该入口同时记录编译期检查依赖和
  aggregate/managed 运行时依赖。
- 最小验证：`Reader<T>` 为空类型，`Vault<T>` 只有 `int` 字段，代码只构造
  `Vault<int>`。对比该字段有无 `@friend(Reader<T>)` 的两份源码，两者均在 Apple
  container 编译成功；增加注解后，`Vault<int>` 描述符多出
  `reified_type_deps_count = 1`，并额外生成 `Reader<int>` 描述符及默认构造代码。
  `Reader<int>` 没有运行时使用，该差异不属于授权检查的必要成本。
- 证据：`temp/friend-scope-audit-20261007/with_friend.ff`、`without_friend.ff`
  及 `generated-c.diff`。已确认额外元数据和生成代码，未进行执行耗时测量。
- 变更前复核：独立构建 `baf61052`，两版编译器对同组 8 份输入均编译成功。
  纯 friend 注解的 owner 运行时依赖从 0 增至 1，确认是本次新增条目。
  当 `Reader<T>` 同时用于实际字段时，两版均为 1；当实际字段使用
  `Storage<T>` 时，本次把原有 `[Storage<T>]` 扩成了 `[Reader<T>, Storage<T>]`。
  普通方法体使用 `Reader<T>` 的原有独立函数描述符槽位也不依赖注解。
  对比证据见同目录 `baseline-comparison.log`、`*-baseline.diff`。
- 同源入口复核：同一基线对照还确认类型约束、函数约束和声明父 spec 各误加了
  一个 aggregate 条目，见 `compile-positions-baseline.diff`。这些入口一并只记录
  编译期检查依赖；实际约束调用和 spec 转换仍使用原有 witness 及运行时依赖路径。
- 修复方向：纯检查入口只保留编译期依赖，不追加运行时槽位；同一轮新增的约束、
  父 spec 等收集入口需按相同职责核对。保持第 5.5 节既定边界，无需扩展授权语义。
- 保留边界：字段、签名和表达式实际使用的运行时依赖仍按原路径收集、去重及排序。
  不得按 friend 类型身份从最终运行时集合删除条目；同一类型同时被实际代码使用时，
  必须保留原有槽位。补充纯注解与实际使用并存时的正向断言及生成代码对照。
- 已实现：同一递归收集逻辑按使用目的保留检查事实，只有实际运行时使用才追加
  原有 runtime 依赖；spec 直接记录编译期依赖，无需先生成再丢弃 runtime 临时表。
- 专项验证：6 组字段注解有无对照生成 C 完全一致，覆盖嵌套泛型与数组、value
  friend、同一类型的真实字段、其他聚合字段、普通方法独立依赖；方法授权单独验证
  零额外槽位并保留必要的受限导出入口。类型／函数约束、父 spec 及两种 FT profile
  下的合法／非法具化均通过；全部 Codegen 用例通过。
- 完整验证：新增两项跨包 FCTS 覆盖 managed／aggregate 字段、共享构造器、方法、fit
  及同时作为 friend 与字段的类型。Apple container 完整 `make test` 退出码 0；
  ASan/UBSan 与普通构建每阶段标准库 607/607、FCTS 1674/1674，编译器及工程检查均通过。
  修复后的生成代码与变更前基线相比，实际依赖条目及共享体访问索引一致；纯注解样本
  生成 C 完全一致。具体证据见 `temp/friend-scope-audit-20261007/comparison-summary.txt`
  和同目录 `*-fixed.diff`，完整回归及源码校验见第 9 节。

### P18：签名遍历与共享 ABI 原型生成重复（已收敛，全量回归通过）

- 发现（2026-10-08）：声明检查、具化条件收集和只读 friend 查询分别枚举字段、
  参数、方法约束和返回类型，并分别读取推断类型；构造调用的共享原型生成又重复
  了导入方法已有的原声明参数解析与 ABI 输出。
- 人工批准：仅收敛上述重复实现，遵循第 5.3、5.5 节。保留既有检查顺序、诊断
  时机、参数绑定和运行时行为，不调整导出范围或链接符号编号。
- 已实现：声明检查、具化条件收集和只读查询共用成员签名遍历，以现有
  `FengSemanticTypeFact` 统一读取显式与推断类型；调用方保留各自的绑定规则。
  声明检查仍先检查方法约束，具化与只读查询仍先检查参数，保留原有首个诊断。
- 已实现：构造调用复用导入泛型方法的原型输出及按最终 C 符号去重机制，统一为
  `cg_emit_generic_member_shared_proto`；保留 P10 参数适配和原声明 ABI。
- 验证：新增声明／具化诊断顺序、推断字段／返回类型、8 项只读签名查询，以及
  部分开放 owner 的普通、value 和可变参数共享构造用例。优化前后产物、诊断
  对照及完整 `make test` 两阶段均通过，证据见第 9 节。

### P19：新增 CLI 负向用例未捕获预期诊断（已处理，全量回归通过）

- 发现：新增 friend 制品用例直接调用 `feng_cli_project_check_main`，预期的
  `AE1336`、`AE0902`、`AE0001` 被打印到终端，且仅断言非零退出。
- 人工批准：参考既有负向测试，复用 `run_project_check_capture_stderr`，同时
  断言退出状态和预期诊断码；不更改其他既有用例的输出约定。
- 已实现并验证：上述新增负向用例复用现有捕获辅助函数，保留原有源码输入，
  增加对应诊断码断言。完整 `make test` 两阶段均通过，日志中不再直接输出这组
  预期诊断；其他既有负向用例的输出约定保持原样。

### P20：`sprintf` 弃用告警的基线核对（非本次引入）

- 对照本次实施前的 `baf61052`：`test/cli/test_lsp_fit_completion.c` 第 74 行已
  使用相同的 `sprintf`，该文件与当前版本完全一致。
- 基线中该文件最近一次变更为 `ef1dfe62`。本轮按人工要求仅核对来源，不修改该
  既有测试辅助代码。

### P21：Review 确认的条件传播与 FT 校验遗漏（已修复，全量回归通过）

- 人工确认：修复 package-public 私有依赖骨架丢失 friend 方法条件、fit 声明级
  条件与 type 路径不一致、friend 签名依赖的 owner 实例与成员归属未校验的问题。
- 规范依据：主体准入及具化检查引用[可见性规范 §10.3](../specifications/feng-visibility.md#103-friend-seal-成员)，
  开放条件转传引用[泛型规范 §6.1](../specifications/feng-generics-draft.md#61-开放检查条件)，
  制品闭包及 owner 绑定校验引用[符号表规范 §6.3.5](../specifications/feng-symbol-table.md#635-atrs-扩展属性节)。
- 实施范围：统一保留必要私有依赖的 friend 成员；以编译期依赖收集 fit 注解与
  声明级 spec 使用；FT 读取与导入复用 owner 实例校验。保持原可见性、诊断码、
  检查时机和运行时依赖槽位，不变更 runtime ABI。
- 专项验证：新增独立用例，原有用例及断言保持不变。11 组声明条件样本分别经过
  源码与两种 FT profile，共 33 项；覆盖私有包含关系、seal 访问边界、fit 注解与
  spec 条件转传，并断言新增 fit 声明检查不占用运行时依赖槽位。
- FT 校验：两种 profile 均验证本地／外部成员的合法 type、spec 投影、普通 fit、
  数组及嵌套数组 owner；共 28 项畸形记录覆盖内建标量、错误具名类型和缺少泛型
  实参，均在有效校验和下拒绝。本机新增专项及原有 friend FT 专项通过。
- 全量验证：在沙箱外的 Apple container Linux ARM64 GNU 独立源码目录执行完整
  `make test`，使用 Clang 22.1.8 和 `LANG=C.UTF-8 LC_ALL=C.UTF-8`，退出码为 0。
  ASan/UBSan 与普通构建两阶段均通过，每阶段标准库 607/607、FCTS 1674/1674，
  包含 Semantic、Symbol、Codegen、CLI/LSP、生命周期及构建发布相关检查。
- 环境准备中的两次中断已保留日志：首次选用了缺少 ASan 运行库的裁剪版 Clang，
  改用容器已安装的完整 Clang 22.1.8；第二次隔离副本遗漏 `std/std/extlib`，
  补齐资产后从头执行完整回归，未修改用例或跳过检查。
- 证据：`temp/friend-review-20261008/make-test.log`、`make-test.exit`；同目录
  `source-sha256.txt`、`tested-source-check.log`、`workspace-source-check.log`
  确认 1434 个源码、测试和构建相关文件在容器与最终工作区一致。该结果不代表本轮
  macOS 本机完整回归；未提交代码。

### P22：分离检查依赖与具化依赖（已完成，全量回归通过）

> 后续状态（2026-10-08）：已批准按[统一依赖优化方案](feng-annotation-dependency-optimize-dev.md)
> 调整本节实现；下列结果保留为历史记录，不作为新方案的验证结论。

- 人工确认：将 `validation_type_deps` 与 `friend_signature_deps` 迁出
  `FengReifiableDepSet`；检查语义引用[泛型规范 §6.1](../specifications/feng-generics-draft.md#61-开放检查条件)，
  制品契约引用[符号表规范 §6.3.5](../specifications/feng-symbol-table.md#635-atrs-扩展属性节)。
- `FengSemanticAnalysis` 独立持有 `FengValidationDepSet`，仍以
  `owner_decl + owner_member` 标识声明或方法的检查上下文；具化侧表保留原有
  类型、callable 和投影依赖。两表共享类型身份比较与已解析调用事实。
- 收集继续复用同一次遍历，检查数据仅进入检查侧表；friend 条件传播从该表读取
  类型及签名依赖，并复用既有 callable 依赖。已闭合条件和开放条件保持原检查时机。
- Symbol 分别导出、恢复两类依赖，并保留原有类型引用生命周期；不变更 FT 编码、
  runtime ABI、运行时槽位或诊断规则。`fit_accesses` 保持现状。
- 专项验证：现有回归样本和预期结果保持不变，仅迁移必要的内部侧表查询。新增
  `test_validation_dependencies_ft`，验证纯检查声明、闭合 fit 访问的独立归属、
  方法内去重、方法间隔离、两种 FT profile 无源码恢复及重复恢复的幂等性。
  本机新用例与既有 friend FT 专项通过；固定声明条件样本的两种 FT 输出与迁移前
  逐字节一致，原 Codegen 槽位与生成代码一致性用例通过。
- 全量验证：在沙箱外的 Apple container Linux ARM64 GNU 独立源码目录执行
  完整 `make test`，退出码为 0；ASan/UBSan 与普通构建两阶段均通过，每阶段
  标准库 607/607、FCTS 1674/1674。本轮新增的是 Symbol 编译器测试入口，未增加
  FCTS 用例；`fit_accesses` 的重叠检查路径未调整。
- 证据：`temp/friend-validation-20261008/` 中的 `make-test.log`、`make-test.exit`、
  `focused.log`、`ft-parity.log` 与源码校验记录。1438 个源码、测试及构建相关文件
  已核对；测试后仅删除 `validation_deps.c` 文件末尾一个多余空行，最终差异记录在
  `final-source-check.log` 与 `post-test-formatting.log`。未提交代码。
