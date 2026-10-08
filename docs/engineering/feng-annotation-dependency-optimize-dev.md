# 注解参数依赖收集与 friend 访问记录优化方案

日期：2026-10-08。状态：本轮约定范围内的优化及 Linux 全量回归已完成，开发者已确认
macOS 本机手动验证完成；无限增长与参数包过度传播均按用户决定移交独立 bugfix，
不作为本次完整交付的条件，详见第 12 节。

本文整理本轮讨论确认的优化方向：注解参数接入既有类型／表达式依赖收集，复用
`FengReifiableDepSet`；保留 friend 自身的语义检查，并增强原有 `fit_accesses`。
目标是删除 `validation_type_deps`、`friend_signature_deps` 两套独立依赖记录。

用户已批准开始优化。本文描述实现迁移和验证安排，语言语义以主规范为准；实施边界
集中列于第 10 节。按“先规范、再实现、后测试”推进，完成情况见第 11 节。

## 1. 现状与优化原因

核对基线为 `baf61052` 及当前未提交变更；下表描述优化前的工作区实现。

| 机制 | 当前职责 | 来源及问题边界 |
| --- | --- | --- |
| `FengReifiableDepSet` | 按声明／成员保存类型、callable 等具化依赖，供 Semantic、Symbol 和 Codegen 使用 | 既有公共机制，应继续复用 |
| `validation_type_deps` | 保存含开放泛参的类型使用，让 friend 检查沿类型使用传播条件，同时避免追加描述符依赖 | 本次新增，与普通具化依赖另行收集、保存及恢复 |
| `friend_signature_deps` | 保存 fit 实际选中的 friend 成员及其完整 owner 实例 | 本次新增，与 `fit_accesses` 重复记录同一次访问，但补充了泛型绑定和跨包所需信息 |
| `fit_accesses` | 在被访问的 friend 成员上记录访问方模块、程序及 fit 方法，供签名推断完成后检查 | 原有机制，由 `73f4195d` 引入，应增强并复用 |
| `FengValidationDepSet` | 承载前述两套新增 deps，按 `owner_decl + owner_member` 组织 | 上一轮只完成存储分离；本方案迁移完两类信息后删除该容器 |

代码依据见 [Semantic 结构](../../src/semantic/semantic.h)、
[依赖收集](../../src/semantic/reifiable_deps.c)、
[friend 检查器](../../src/semantic/detail/friend_validation.c)。
优化前的独立存储位于 `src/semantic/validation_deps.c`，本轮迁移已删除该文件。

最小改动范围补充（2026-10-08）：独立检查容器删除后，收集上下文直接复用
`dep_set` 的 owner 信息，恢复原收集入口名称。撤销本轮新增的
`src/semantic/dependency_types.c` 文件拆分及比较函数调用点的批量改名；
依赖去重和访问去重分别复用所在模块的既有类型比较函数，不为单个 helper 新建模块。
继续收回 `SynthDecl` 中 `reifiable_dep_refs`、`reifiable_dep_ref_count` 及其追加 helper
的纯重命名；Codegen 的共享原型缓存及原有发出 helper 也保留原名。扩展既有入口的
调用来源不要求批量改名，必要行为变更和对应注释保留。
Symbol 导出保留原 `fill_reifiable_deps` 入口，friend 访问事实只在方法导出位置追加，
不增加替换全部声明调用点的 `fill_decl_dependencies` 包装层。原有 writer helper 保留
定义位置和函数体结构，仅补充新调用所需的前置声明；同时清理单一 friend 属性列表
遗留的临时别名及无关格式差异。

friend 的主体准入、授权和签名可见性检查属于该功能自身的职责。
优化针对重复的依赖收集、存储和传递机制，不因其具有 friend 语义而删除必要检查。

### 1.1 本次实施中的路径演变

本次实施中，确实曾将 friend 参数接入普通类型依赖收集，随后为避免新增 slots 改为
仅记录检查依赖。依据见原实施记录的
[P17](feng-friend-generic-cross-package-dev.md#p17纯-friend-注解产生非必要的运行时依赖已修复全量回归通过)及
[P22](feng-friend-generic-cross-package-dev.md#p22分离检查依赖与具化依赖已完成全量回归通过)。

| 阶段 | 收集与存储方式 | 与本方案的关系 |
| --- | --- | --- |
| P17 调整前 | `collect_for_type` 将 friend 参数交给 `try_collect_type_ref`，同时记录检查类型依赖和普通 aggregate／managed 依赖 | 已走普通收集路径，但仍有独立检查记录，尚未完成统一 |
| P17 | 改用 `collect_validation_type_ref`，经 `collect_type_ref(..., false)` 保留检查信息、不追加普通类型依赖 | 以避免纯注解新增描述符及 slots 为当时的优化目标 |
| P22 | 将 `validation_type_deps`、`friend_signature_deps` 从 `FengReifiableDepSet` 迁至 `FengValidationDepSet` | 分离存储职责，继续保留两套独立 deps |
| 本方案 | 注解参数复用普通收集路径；检查消费统一依赖，实际访问事实归入增强后的 `fit_accesses` | 允许既有机制生成静态描述符及 slots，并删除两套独立 deps |

因此，本方案不是简单撤销 P17：还需通用化注解参数入口、迁移检查消费者及访问记录，
消除重复存储。P17 验证了生成元数据和代码的增量，未测量执行耗时；不能据此把静态
slot 增加等同于运行时执行开销。原 P17、P22 保留为历史记录，不代表本方案已实施或验证。

## 2. 已确认的设计方向

1. 注解的完整参数类型表达式与字段类型共用依赖收集入口，表达式参数共用表达式入口。
2. `FengReifiableDepSet` 不增加字段，`FengReifiableDepKind` 不增加“仅检查”类别。
3. 复用现有参数绑定、具化、去重、FT 依赖表示、静态描述符生成和 slots 处理。
4. 接受按既有规则生成参数类型的静态描述符及依赖 slots；不要求纯注解维持零新增 slot。
5. friend 自身的检查、成员授权事实及实际访问上下文继续保留。
6. `fit_accesses` 作为 friend 访问事实的唯一 Semantic 记录，补齐泛型和跨包信息。
7. 删除两套独立 deps 及失去用途的容器、接口、导入导出和释放路径。

静态 slots 已获明确允许，不再作为待决策项；原“纯注解零新增 slot／生成 C 完全相同”
不再是本方案的验收要求。对应主规范及测试的迁移安排见第 8、9 节。

本次不建设新的通用注解执行框架，不重写类型系统，不新增语言注解。未来注解复用参数
收集与具化入口，其具体语义由对应实现处理；公共入口不预设所有注解都只进行检查。

语言行为分别引用：

- [可见性规范 §10.3](../specifications/feng-visibility.md#103-friend-seal-成员)：注解准入、作用域及授权。
- [可见性规范 §11.1](../specifications/feng-visibility.md#111-friend-成员签名可见性)：签名检查及词法上下文。
- [泛型规范 §6.1](../specifications/feng-generics-draft.md#61-开放检查条件)：开放条件的处理时机与传播。
- [符号表规范](../specifications/feng-symbol-table.md)：两种 FT profile、身份绑定及私有依赖闭包。

## 3. 注解参数进入公共依赖收集

### 3.1 统一入口

公共注解遍历按 `FengAnnotation.argument_kind` 分派，不按 `FENG_ANNOTATION_FRIEND`
筛选参数是否进入依赖收集：

| 参数种类 | 收集方式 |
| --- | --- |
| `TYPE` | 将 `type_args` 交给既有类型引用收集入口，保留完整嵌套结构 |
| `EXPRESSION` | 将 `args` 交给既有表达式收集入口，复用其中的类型及 callable 收集 |
| `NONE` | 无参数可收集 |

收集发生在语义绑定之后，沿用已有合法性判断、显式／推导类型事实及参数作用域。
遍历参数不表示在运行时执行注解，也不改变已有注解参数的求值规则。

优化前 `collect_member_validation_annotations` 中的 friend 筛选和专用检查入口已被替换。
新增入口复用 `try_collect_type_ref`、`collect_from_expr` 等既有遍历；不得复制另一套
类型递归、参数代入或去重逻辑。

### 3.2 依赖归属

当前 friend 参数按成员所属声明的类型参数作用域解析，具体准入引用主规范。
收集时保持该归属：

| 声明位置 | 参数依赖归属 |
| --- | --- |
| 泛型 type 的字段／方法上的 friend | 所属 type 的声明级依赖域 |
| object-form spec 成员上的 friend | 所属 spec 的声明级依赖域 |
| fit 成员上的 friend | fit 声明及其目标类型绑定的参数域 |
| 普通方法签名、函数体和方法级泛参使用 | 继续归对应 callable 的既有依赖域 |

公共遍历接收正确的 owner 和参数环境。不能为了复用入口，把方法体依赖全部并入类型域，
也不能因目前 spec／fit 声明级只创建检查容器而跳过这些声明；应使其使用统一依赖容器。
其他现有注解沿其已定义的绑定作用域接入，不在本次新增方法级注解泛参准入规则。

### 3.3 示例与 slots

```feng
type Reader<U> {}

type Vault<T> {
    @friend(Reader<T>)
    seal let value: int = 0;
}
```

`Reader<T>` 作为完整类型表达式进入 `Vault<T>` 的类型依赖。具化 `Vault<User>` 时，
沿公共路径代入得到 `Reader<User>`，注册／复用该实例的静态描述符，依赖 slot 保存其地址。
同一类型表达式同时出现在注解和字段中时，沿既有依赖身份去重，不生成两套用途专属记录。

直接使用类型参数的参数表达式，例如 `@friend(T)`，沿用 owner 的既有泛参绑定。
“进入收集”不要求为直接泛参额外伪造一个 managed／aggregate 依赖；friend 参数和
检查条件仍由注解语义保存，不能以是否产生额外 slot 判断是否需要处理该注解。

已经闭合的参数类型，例如 `Reader<User>`，也要按普通类型使用核对实例注册入口。
已有开放依赖列表不会记录所有闭合类型；应补齐注解在通用类型遍历中的入口，不能为
绕过该边界人为追加开放依赖，也不能只验证带 `T` 的示例。

## 4. 删除 validation_type_deps

`validation_type_deps` 保存的是类型使用关系，不是检查结果。当前 friend 检查器沿这些
类型使用关系传播声明条件。新方案从统一具化依赖读取同样的类型使用关系。

迁移必须覆盖现有全部来源：

- 注解参数中的类型表达式。
- 字段、参数、返回类型以及函数体中的显式／推导类型使用。
- 类型／函数泛型约束、类型声明的 spec 列表、fit 的 spec 列表及 spec 父声明。
- spec 的成员签名、callable-form 签名、union／intersection 成员类型。
- 推导得到的数组等类型事实，包含相应泛型 fit 的声明条件。

处理已由普通收集覆盖的来源时直接复用；缺少公共入口的来源接入该入口。
直接泛参及无需独立描述符的叶节点沿既有绑定表达，不能机械地把每一项都追加成新 slot。

friend 检查器继续在 Semantic 阶段执行，调整其类型依赖读取位置，不把检查推迟到 Codegen。
它的声明条件、调用转传和有限条件传播算法继续复用；普通 `feng check` 也必须完整检查。
递归／互递归场景需同时验证条件传播与具化依赖展开的终止性。

迁移完成后删除：

- `validation_type_deps` 及其 count／capacity 字段、追加和查询路径。
- Symbol 中对应的独立类型列表、复制／比较／释放路径。
- 新制品的 `FT_ATTR_VALIDATION_TYPE_DEP` 独立写出路径；类型使用复用普通 reifiable 编码。

旧制品处理沿用符号表主规范的统一重建要求，见第 10 节，不保留第二套 Semantic
依赖表作为兼容机制。

## 5. 用 fit_accesses 承接 friend_signature_deps

是否需要修改 `fit_accesses`，取决于本次同时完成的两个目标：

- 仅将注解参数接入公共类型／表达式依赖收集，本身不要求修改 `fit_accesses`。
- 同时删除 `friend_signature_deps` 并保留当前泛型、跨包签名检查能力，需要保留它
  承载的实际访问事实及泛型绑定。本方案将这些信息合并到原有 `fit_accesses`。

因此，增强 `fit_accesses` 是消除重复访问记录的直接方案，不是注解参数进入
`FengReifiableDepSet` 的前置要求。必须保留的是完整信息；具体字段按最小需要设计，
能够通过现有稳定关联取得的信息直接复用，不要求逐项新增字段。

### 5.1 保留一份访问事实

现有 `fit_accesses` 挂在被访问成员的 `FengFriendMemberInfo` 上。原 `FriendFitAccess`
仅保存访问方 module、program 和 callable member，被访问成员和声明 owner 可由所属
记录确定，但没有保存被访问成员的完整 owner 实例。当前
`record_selected_friend_fit_access` 同时写入它和 `friend_signature_deps`，后者补充了
实例绑定；直接删除后者而保持前者不变会丢失该信息。

建议按下表合并，替代并行的 `FengFriendSignatureDep`：

| 信息 | 处理方式 |
| --- | --- |
| 被访问成员及其声明 owner | 复用所属 `FengFriendMemberInfo` |
| 访问方 module、program、fit 方法 | 保留既有字段 |
| 访问方 fit 声明及参数域 | 补齐稳定关联，供条件归属及跨包恢复使用 |
| 被访问成员的完整 owner 实例 | 保存已选成员对应的类型表达式及参数绑定 |

例如，同一个 fit 方法分别在 `Vault<T>` 和 `Vault<Box<T>>` 实例上访问同一个 `read`
成员，成员声明相同，但代入后的签名可能不同。原记录按“module + callable”去重，
会合并这两次访问；增强后必须保留完整 owner 实例来区分。类型依赖只说明使用了某个
类型，不能替代“实际选中了哪个成员、采用哪组绑定”的访问事实。

继承 spec 成员的 owner 实例转换复用现有解析结果。
保存的类型引用必须在分析／导入缓存生命周期内有效，不借用临时解析上下文。

只在成员／重载最终选中后记录访问，候选可见性查询继续保持只读。
本地及导入访问都进入这份记录，反向或导出查询如需索引，只建立由该记录派生的索引。

### 5.2 检查与导出

friend 签名检查从增强后的 `fit_accesses` 读取访问方词法上下文和完整实例绑定。
已有推导后复查与泛型条件传播共用这些事实，避免针对同一次访问维护两套状态。
无泛参的 fit 访问仍需检查，不能以是否存在具化依赖集合决定是否保留访问记录。

导出某个 fit 方法时，从这份访问记录生成该方法的制品访问事实；导入后恢复到同一记录。
目标成员身份、owner 实例及参数域仍需完整校验，复用现有 FT 校验能力。

完成后删除 `FengFriendSignatureDep`、`friend_signature_deps` 及其独立追加／消费接口。
两类 deps 均迁移完成后，删除 `FengValidationDepSet`、`analysis->validation_dep_sets`
和不再使用的 `validation_deps.c`。共有类型比较等仍有消费者的工具继续复用。

## 6. Symbol 与 FT 的迁移边界

### 6.1 类型依赖和注解语义

类型使用统一走现有 aggregate／managed reifiable 依赖导出、恢复及类型编码。
继续复用 `SYMS`、`TYPS`、`TSEQ` 和既有类型参数引用；不新增注解专属依赖种类。

friend 参数的声明语义仍需保留。现有 `friend_types`／`FT_ATTR_FRIEND_TYPE` 承载
“哪个成员向哪些类型授权”，不是被删除的两套 deps，继续沿原有注解语义路径处理。

### 6.2 fit 访问事实

跨包消费不包含 provider 函数体，不能仅凭类型依赖反推出实际选中了哪个成员。
增强后的 `fit_accesses` 必须有对应的持久化表示。

`0x0013` 是本次未提交 friend 实现新增的 FT 属性编号，优化前名为
`FT_ATTR_FRIEND_SIGNATURE_DEP`，本轮更名为 `FT_ATTR_FRIEND_FIT_ACCESS`。
它不是运行时依赖 slot，也不是 HEAD 中已经存在的
编码。它挂在访问方 fit 方法上，保存目标成员的模块名、符号 ID，以及目标成员所属
类型的完整实例和泛型实参。具体字段布局只在
[符号表主规范 §6.3.5](../specifications/feng-symbol-table.md#635-atrs-扩展属性节)定义。

“复用现有编码布局”指复用本轮已经新增的数值与布局，改由增强后的 `fit_accesses`
导出、恢复这些访问事实。Symbol 保留必要的序列化视图，Semantic 不再维护独立的
`friend_signature_deps`。这是当前方案的最小编码改动建议，不能把“删除独立 deps”
理解为删除跨包访问事实，也不需要因合并内存记录另加一套 FT 属性。

两种 profile 均需保留相同的绑定和访问语境。私有依赖闭包、friend 成员骨架、完整实例
校验、缓存比较及重复恢复幂等性一并迁移，不能因收集方式改变而扩大源码可见性。

## 7. Codegen 与成本判定

复用现有“收集具体实例 → 生成／复用静态描述符 → 在依赖表中引用地址”的通道。
参考 [Codegen](../../src/codegen/codegen.c) 中的
`cg_collect_closed_reifiable_dep_instances_inner`、`cg_resolve_dep_descriptor_name`
及类型依赖表输出逻辑。

slots 是静态初始化的指针数组，不是在每次构造对象时动态填充的表；多生成描述符或
构造函数也不意味着执行了对象构造。生命周期边界引用
[runtime 描述符定义](../../src/runtime/feng_runtime.h)，本方案不变更 runtime ABI。

验收区分三类结果：

| 观察项 | 判定方式 |
| --- | --- |
| 新增静态描述符、依赖指针或关联代码 | 属于本方案允许的编译产物变化，记录体积及复用情况 |
| 原描述符已存在，仅新增静态 slot 引用 | 检查实例去重和 slots 身份正确，不当作动态构造成本 |
| 新增运行时加载、调用、临时对象或参数传递 | 单独核对生成代码，不能凭描述符数量推断存在或不存在 |

尤其检查 [泛型参数包分析](../../src/codegen/detail/generic_arguments.c) 对依赖图的消费。
若接入后确实引入新的执行成本或需要调整调用 ABI，提供具体差异并按仓库规则交人工
决策，不新增 friend 专用跳过逻辑。当前生成 C 的对照发现了参数包传递变化，见第 12.2 节；
用户已决定本轮沿用既有公共处理，将过度传播另作 bugfix，不作为本次完整交付的条件。
尚未测量执行耗时，不能宣称整体运行时开销不变。

## 8. 规范与文件改动范围

本轮先调整了主规范中的实现契约；既有语言授权和诊断规则只引用，不在工程文档重定义。

| 文件／模块 | 计划改动 |
| --- | --- |
| [泛型规范](../specifications/feng-generics-draft.md) §6.1 | 调整“检查条件自身不占用运行时依赖槽位”的表述，区分编译期检查与类型依赖的静态描述符 |
| [符号表规范](../specifications/feng-symbol-table.md) | 类型依赖统一编码；说明 fit 访问事实，沿用制品统一重建要求 |
| `src/semantic/reifiable_deps.c` | 注解参数统一遍历，补齐已有类型使用来源和 owner 域 |
| `src/semantic/analyzer.c`、`detail/friend_validation.c` | 增强 `fit_accesses`，检查器消费统一事实，保留检查时机及诊断上下文 |
| `src/semantic/semantic.h`、`validation_deps.c` | 删除两套 deps、容器及失效接口，保持 `FengReifiableDepSet` 字段和类别不变 |
| `src/symbol/` | 迁移导出、恢复、制品校验、依赖闭包及缓存生命周期 |
| `src/codegen/` | 复用实例注册、静态描述符和 slots；若普通类型遍历尚未覆盖注解，补齐公共入口，包含闭合参数类型 |
| `test/`、`fcts/` | 同步调整本轮新增测试的结构断言，补足行为和跨包覆盖 |

[原开发文档](./feng-friend-generic-cross-package-dev.md) 的 P17、P22 是此前约束下的实施记录。
新方案获准实施时更新其状态及链接，不抹去历史验证记录，也不把旧回归结果作为新方案已通过的证据。

## 9. 验证计划

### 9.1 覆盖矩阵

| 维度 | 必须验证的内容 |
| --- | --- |
| 公共参数遍历 | 现有类型位、表达式位及无参注解进入正确入口；不新增注解名称特判 |
| 参数绑定 | 直接类型参数、开放／闭合构造类型、嵌套实参、数组、别名和推导类型；参数身份与作用域正确 |
| 去重 | 同一类型同时用于注解和字段；重复注解使用；不同完整实例保持区分 |
| owner 域 | 泛型／非泛型 type、object-form spec、fit、数组 fit、函数和普通方法；方法依赖不混入类型域 |
| 原有类型使用来源 | 约束、父 spec、成员签名、函数体内部及推导数组均保留条件传播 |
| friend 行为 | 主规范规定的合法／非法主体、签名可见性、seal 边界及未访问成员时的具化检查 |
| 实际访问 | 闭合／开放 fit、多次访问、多个 owner 实例、重载候选未选中、推导后复查 |
| 调用传播 | 直接、嵌套、有限递归／互递归；检查与具化不丢失必要条件。无限增长按第 12.1 节单独跟踪 |
| FT | 两种 profile、销毁 provider AST 后导入、私有骨架、稳定成员身份、重复恢复及畸形绑定拒绝 |
| Codegen | 描述符复用、slots 身份与静态初始化；区分静态数据增量和执行语句／隐藏参数变化 |
| 工具和兼容性 | `feng check`、实际构建运行、LSP 源码／缓存／外包路径及已有 FCTS 行为 |

`test/` 验证诊断码、绑定结构、AST／IR／生成代码及 FT 契约；`fcts/` 验证语言行为。
不以测试总数相同证明已新增覆盖，交付时分别列明新增、迁移用例及执行结果。

### 9.2 本轮新增测试的同步调整范围

下列三份文件均为本次未提交变更新增，相对 HEAD `baf61052` 的
`git diff HEAD --name-status` 状态均为 `A`，不是历史基线中的测试文件。
这里是让本轮新增用例随已确认的新方案同步调整，不再单列为修改历史测试的批准项。

- [test_friend_dependency_slots.c](../../test/codegen/test_friend_dependency_slots.c)：原“注解不增加 slot／生成 C 完全相同”断言与新方向冲突，已改为公共依赖去重、静态描述符及生成代码验证。
- [test_validation_dependencies.c](../../test/symbol/test_validation_dependencies.c)：原独立容器及零运行时依赖断言已迁移到统一依赖和增强后的访问记录；保留源码销毁、两种 profile 及重复恢复场景。
- [test_friend_generic.c](../../test/symbol/test_friend_generic.c)：迁移内部容器查询及与方案对应的 FT 编码断言；保留合法／非法具化和畸形 owner 实例覆盖。

本节不扩大对 HEAD 中已有测试的修改范围；若实施中确需调整历史测试，另行列明并遵循
仓库规则。现有语言行为预期不因存储重构而削弱。新增有独立验证价值的用例，避免只
重复新结构的实现步骤。上述同步调整已纳入本次获准实施范围。
实施完成后运行沙箱外 `make test`；产物和执行文件放在仓库 `build/` 或 `temp/`。

### 9.3 Review 后补充的覆盖

2026-10-08，用户批准基于 `24e9de44` 补齐以下四类用例。允许在对应已有测试中
追加样本及增强断言，保留原有覆盖，不修改语言行为或放宽断言：

- 跨包身份拒绝：销毁 provider 源码后，在两种 FT profile 下验证同一泛型 friend
  类型的不同完整实参不能共享授权；匹配实例继续通过。
- fit 访问记录：逐条核对实际访问所属方法、目标声明／成员及完整 owner 实参，
  比较源码、FT 导入和重复恢复后的结果，不能只比较数量。
- 注解 slots：核对纯注解及与字段重叠的依赖所引用的具体描述符，包括嵌套类型、
  不同完整实例及去重；不能只检查依赖数量或生成 C 能编译。
- 增量授权：在同一 LSP 会话内撤销和恢复授权、修改泛型实参，并验证依赖制品更新
  后旧授权不再生效；通过协议响应确认当前分析已就绪，再断言补全及导航结果。

先执行对应 Symbol、Codegen、CLI/LSP 专项，再在沙箱外执行完整 `make test`。
无限增长及参数包过度传播仍按第 12 节独立跟踪，不纳入本轮补充用例。

新增 LSP 负例检出了导航降级路径的遗漏：撤销源码中的授权后，Semantic 已报告
不可访问，补全也移除了成员，但 Hover 的 AST 名称解析仍返回该成员。现已让
源码与制品的导航解析复用已有成员候选及可见性过滤，不另加 friend 专用许可机制；
授权行为仍引用[阶段一方案 §5.6](./feng-friend-generic-cross-package-dev.md#56-lsp-与诊断)。

依赖制品替换的负例还检出旧成功分析被误用：消费者文本未变，重分析已拒绝访问，
但编辑器仍复用替换前的语义结果。现已在后台记录、比较依赖文件状态，使依赖变化后
失败的分析不再提供旧语义事实；源码编辑失败且依赖未变时保留原有恢复能力。
`didSave` 现在分配新分析代次，允许内容未变但依赖已更新的成功结果替换旧缓存。
查询路径不增加文件系统访问，也不增加 friend 专属的缓存状态。

本轮实际追加／增强的断言如下（位于 `test/`，不增加重复的 FCTS 行为样本）：

| 测试入口 | 补充内容 |
| --- | --- |
| `test_friend_generic_ft` | 新增 8 个消费者样本，两种 profile 共 16 次验证；含字段、方法、静态成员及方法值的匹配／不匹配身份 |
| `test_validation_dependencies_ft` | 源码、两种 profile 及各自重复恢复后逐条核对方法归属、成员指针、`T`／`Box<T>` 绑定及去重 |
| `test_friend_dependency_slots` | 源码与两种 FT profile 的 `i32`、`string` 实例，逐个核对 managed／aggregate 描述符表、嵌套实参、字段重叠和重复注解 |
| `test_lsp_friend_authorization_lifecycle` | 两个连续会话共 12 个状态、32 次补全／Hover／Definition 查询；依赖包原子替换时消费者文本不变，另含依赖未变的错误恢复对照 |

2026-10-08 验证完成：Symbol、Codegen、新增 LSP 矩阵及完整 CLI 专项通过；
Linux ARM64 GNU / Clang 22.1.8 的隔离副本在沙箱外执行完整 `make test`，退出码为 0。
普通构建与 ASan／UBSan 两阶段均通过标准库 607/607、FCTS 1676/1676，包含新增
LSP 矩阵、原有缓存生命周期、构建发布及工具链检查。1996 项输入 SHA-256 校验通过，
确认执行内容与工作区一致；测试后仅更新本节验证记录。macOS 本机本轮未重跑。
本地日志保存在 `temp/friend-coverage-IxnxcC/make-test.log`。

### 9.4 friend 签名具化与指针目标覆盖补充

2026-10-08，基于 `c547d295`，用户批准补齐签名可见性的两个独立覆盖缺口并执行
完整回归。语义继续引用[可见性规范 §11.1](../specifications/feng-visibility.md#111-friend-成员签名可见性)，
不新增或调整可见性规则。

- 签名类型自身为 owner 泛参：分别覆盖参数、返回值、泛型实参和数组元素中的泛参，
  以及闭合指针整体代入泛参；代入可见类型应通过，不可见类型应报告 `AE1338`。
- 指针目标类型：分别覆盖参数和返回值，比较同模块允许、跨模块私有类型拒绝及
  公开类型允许；不能以“指针不能作为 friend 主体”的用例替代签名检查。
- 两类均验证具体 friend、构造泛型 friend 和直接泛参 friend；开放条件经泛型调用
  转传后仍须检查，且无需实际访问 friend 成员。比较源码与销毁 provider AST 后的
  package-public、workspace-cache 两种 FT profile，并断言诊断位置和授权声明关联。
- 新增用例及断言，不改变既有用例预期。先执行相关专项，再在沙箱外执行 `make test`。
  发生失败时，先在本节记录复现与实际结果，再分析和修复；不确定的语义或方案由
  人工决策。

首次专项运行记录：macOS 构建成功，新增矩阵在授权关联位置的路径断言处失败
（`test_friend_signature_visibility.c`，`related->path` 比较）。补充日志后确认，失败
来自 package-public FT：测试错误地要求恢复 provider 的源码路径及注解行列。
[符号表规范 §5.1](../specifications/feng-symbol-table.md#51-公开包表-ft-必须包含的事实)
明确禁止公开 FT 携带源码路径和行列；导入上下文使用制品来源，注解使用成员符号位置。
因此修正本轮新增断言：源码检查精确注解位置；FT 检查所选授权成员的制品来源、符号
位置和关联消息，不降低主诊断码、具化点及错误数量要求。无需修改编译器。
日志保存在 `temp/friend-signatures-oF9jCJ/evidence/`。

随后专项运行记录：源码中直接声明参数 `R*` 时先报告 `AE0333`，因此该样本不能
用于验证 friend 可见性。核对[泛型规范](../specifications/feng-generics-draft.md)的开放
指针限制后，已将样本改为把闭合的
`Hidden*`／`Shared*` 作为完整类型实参代入签名中的 `R`；固定指针签名仍单独保留。
该调整只修正本轮新增测试的构造方式，不改变开放泛参指针的既有规则。

验证完成：新增 [test_friend_signature_visibility.c](../../test/symbol/test_friend_signature_visibility.c)，
并接入 `test_symbol`，保留全部既有测试预期。矩阵包含 12 种签名／实参组合，每种
分别验证 6 个开放 friend 场景和 8 个已知 friend 场景；168 个场景各执行源码及两种
FT profile，共 504 次检查。负例单独断言一个 `AE1338`、不可见类型名、声明或具化点
token／行列，以及具化诊断的原授权关联；未访问成员的调用转传也纳入检查。

2026-10-08，在 macOS ARM64、Clang 22.1.8、UTF-8 locale 的隔离副本中，沙箱外
完整执行 `make test`，退出码为 0。ASan／UBSan 与普通构建均通过新增 504 次检查、
标准库 607/607、FCTS 1676/1676，以及编译器、CLI／LSP、调试器、发布和工具链测试。
测试前后 1997 项输入 SHA-256 校验通过，工作区与被测副本一致；随后仅补记本文结果。
完整日志为 `temp/friend-signatures-oF9jCJ/evidence/make-test.log`。本轮未发现需修复的
编译器实现缺陷，生产代码未改动。

## 10. Review 范围与实施边界

已确认方向见第 2 节，不重新将“是否统一收集”“是否保留零新增 slot”列为待决策。
以下收敛各项的来源与边界，不将全部事项视为新增审批要求。

| 项目 | 方案及 Review 边界 |
| --- | --- |
| R1：fit 访问记录编码 | 第 5、6.2 节说明必要信息及合并方式；建议复用本轮新增的 `0x0013` 布局，重点 Review 是否完整承接访问事实、消除重复记录 |
| R2：旧制品处理 | 沿用[符号表主规范 §6.3.5](../specifications/feng-symbol-table.md#635-atrs-扩展属性节)的统一重建要求，不提供旧制品兼容、迁移或桥接，不再作为待决策项 |
| R3：本轮新增测试调整 | 第 9.2 节三份新增文件随方案同步调整；不是修改历史测试的审批项，不扩大历史测试修改范围 |

若旧制品包含当前未写入普通 reifiable 依赖的类型使用，仅删除属性或静默忽略旧记录会
丢失检查。实施时需落实旧制品识别、拒绝及缓存失效入口，沿主规范要求统一重建，
不能将未恢复的事实视为检查通过，也不另建兼容依赖表。

## 11. 实施顺序与验收条件

- [x] 完成本方案 Review，按第 10 节明确的边界进入实施。
- [x] 更新相关主规范，工程文档只引用其语义和制品契约。
- [x] 接通公共注解参数收集，迁移 `validation_type_deps` 的类型使用来源及检查消费者。
- [x] 增强 `fit_accesses`，接通本地检查、泛型传播、FT 导出和恢复。
- [x] 删除两套独立 deps、`FengValidationDepSet` 及失效接口，确认没有改名后继续双写。
- [x] 同步调整本轮新增测试的断言，补充第 13 节记录的覆盖，完成专项及沙箱外 `make test`。
- [x] 按用户要求将无限增长问题移交独立 bugfix，保留复现和影响范围，本轮不实施。
- [x] 按用户要求将参数包过度传播移交独立 bugfix，本轮沿用既有公共处理，不以其修复作为交付条件。
- [x] 交付当前实际变更、覆盖与回归证据，给出英文 commit message，由开发者提交。

完成标准：注解参数复用既有具化依赖；friend 检查及访问事实完整；源码与制品路径一致；
没有新增注解专属依赖集合；没有未经批准的运行时行为或 ABI 变更。

## 12. 实施验证发现的问题及范围

### 12.1 实参增长与静态闭包

按用户 2026-10-08 的要求，原有 Codegen 无限增长问题移至
[泛型具化依赖无限增长 bugfix](./feng-generic-reification-growth-bugfix.md)，后续单独修复。
该文档统一保存复现、根因、注解接入后扩大的影响范围及待确定的修复契约。
本轮不修改增长检测或诊断规则，也不将该问题的修复作为本轮验收条件；问题仍然存在。

### 12.2 参数包过度传播

按用户 2026-10-08 的要求，本问题移至
[泛型参数包过度传播 bugfix](./feng-generic-arguments-propagation-bugfix.md)，后续单独修复。
该文档统一保存字段与注解的复现、生成 C 对照、既有公共机制的原因及后续验证边界。
本轮沿用与字段相同的公共处理，不将该问题的修复作为 friend 泛型与跨包支持完整交付
的条件；问题仍然存在。

## 13. 实际变更与验证记录

- 已移除两套独立 deps 及 `FengValidationDepSet`；`FengReifiableDepSet` 的字段和类别
  未增加，`fit_accesses` 是 Semantic 的唯一实际访问记录。FT `0x0013` 仅保留其序列化视图。
- 按最小改动要求收回 helper 拆分、批量改名及重复 owner 上下文。相对 HEAD `baf61052`，
  `reifiable_deps.c` 从新增 152 行／删除 78 行收敛到新增 107 行／无删除；
  其中 14 行属于此前已修复的 spec 方法值依赖问题，其余为公共注解入口及类型使用来源补齐。
- 本次继续恢复 Symbol 导入的依赖引用存储及 Codegen 共享原型 helper 的原名；
  收回导出包装层、writer helper 搬动及无关格式差异，不调整参数包机制。
  本次没有修改测试用例，收敛后的全量回归已通过。
- `synthesize_bound_dependency_type` 保留：它调用原 `synthesize_type_ref` 后恢复
  provider 声明身份，并非替换旧 helper 的名称。隔离对照中，provider 的私有模块定义
  `Hidden<T>`，公开泛型函数以 `Hidden<T>` 调用另一泛型函数；consumer 同时具化含该
  私有字段的 friend 宿主。保留绑定时 C 生成成功，仅撤回 callable 实参处的绑定后
  报 `CE0031`（`Hidden<...>` 未注册）。该绑定不扩大 consumer 源码的名称可见性。
- 原六组 Codegen 对照用例迁移为静态依赖与去重断言，另增加非泛型 owner 的闭合注解
  参数注册用例。Symbol 增加同一方法重复访问及不同完整 owner 实例的区分，补齐普通
  类型依赖属性的畸形 owner、参数作用域、保留位和退役 `0x0012` 拒绝验证。
- FCTS 保留原两项跨包行为测试，新增两项独立行为测试：嵌套开放参数和闭合参数的
  注解不会执行参数类型的构造函数；显式构造仍正常执行。总数由 1674 增至 1676。
- 收敛前后两版均在独立 Linux 工作目录、沙箱外完成 `make test`，退出码为 0。
  sanitizer 与普通阶段均通过：smoke 91、标准库 607、FCTS 1676，及编译器、CLI、
  Symbol、Codegen 等完整目标。收敛后快照的 1991 个文件在回归结束后通过 SHA-256
  校验；该记录对应本次继续收回重命名之前的实现。
  Linux 回归证据保存在 `/work/feng-annotation-deps-minimal-20261008-evidence/`。
- 本次继续收敛后，在沙箱外使用 Clang 22.1.8，将 `LANG`、`LC_ALL` 设为 `C.utf8` 执行完整
  `make test`，退出码为 0。Sanitizer 与普通阶段均通过：smoke 91、标准库 607、
  FCTS 1676，及编译器、CLI、LSP、Symbol、发布脚本等完整目标。
  测试前后的 1995 个快照文件均通过 SHA-256 校验，工作区与被测快照一致；随后仅补记
  本文验证结果。日志为 `temp/annotation-name-cleanup-WEvrym/make-test.log`，容器内证据
  保存在 `/work/feng-annotation-name-cleanup-20261008-WEvrym-evidence/`。
- 首次运行因容器使用 POSIX locale，标准库的 12 项字符宽度测试失败；相同已生成程序
  切换至 UTF-8 locale 后 607 项全部通过。修正测试环境后重新执行了上述完整回归，
  没有修改标准库实现或测试来绕过失败。
- 开发者于 2026-10-08 确认本轮优化后的 macOS 本机手动验证已完成，验证来源为
  开发者反馈；未将其表述为本轮代理执行的回归。
- 第 12.1、12.2 节的两项既有公共问题均按用户要求移交后续独立 bugfix，
  不作为本次完整交付的条件；本轮未修复这两项问题。
