# Feng void 类型开发方案

> **状态**：待 Review，未实施，不覆盖正式规范。§2 记录已确认的目标语义；方案最终采用及实施安排独立 Review。
>
> **更新**：2026-10-22。本文集中定义 void 类型增量，可独立实施、验收和交付，不依赖 async/await。

## 1 目标与边界

将 void 从仅表示无返回值的类型扩展为可按普通类型规则使用的内建单值类型，不要求调用方构造 Unit 或零元素 tuple。

交付范围为类型语义、泛型、代码生成、FT 和语言服务，不包含 Future、状态机、调度器或 async 私有 runtime API。[async/await 草案](feng-async-future-draft-4.md)以本文为前置依赖，不能反向成为本文的验收条件。

## 2 目标语义

`void` 不是 null，唯一的值写作关键字 `void`。它可作为声明类型、泛型实参，按普通类型规则绑定、持有和传递；允许运算 `==`、`!=`，禁止其他运算及成员访问，也没有成员。

```feng
let a: void;
let b = void;
let c: void = void;
let d: void = c;
let equal = a == b;   // bool，true。
let different = a != b; // bool，false。

func pass<T>(value: T): T { return value; }
let e = pass<void>(d);

func test() {}
let x = test();       // test 返回 void，x 推导为 void。
let y: void = test();

func done(): void { return; }
func explicitDone(): void { return void; }
func forward(): void { return test(); }
```

- 省略 void 绑定的初始化表达式时，其值仍为唯一的 `void`；传递不会产生其他 void 值。
- 两个 void 类型的操作数可进行相等比较，结果类型为 bool；正常求值完成后，`==` 得到 true，`!=` 得到 false。按既有求值顺序执行操作数，保留副作用和异常传播，不得仅因比较结果恒定而删除求值。本条不新增 void 与其他类型之间的比较规则。
- 返回类型为 void 时接受空 `return;`、`return void;` 和返回 void 类型表达式；正常落尾沿用现有 void 函数规则。表达式的副作用与异常必须保留。
- 未声明返回类型且无 return 或仅空 return 的函数仍推导为 void；调用结果可用于绑定，绑定类型推导为 void。
- void 不可添加 fit 扩展、适配 spec 或作为交叉类型成员；可以作为联合类型成员。联合成员资格不等于对 spec 的适配。
- 泛参替换为 void 不改变源码参数个数：上例 `pass<void>` 仍接受一个参数，不能写成 `pass<void>()`。底层载荷表示不参与源码重载决议。
- 普通声明、成员、容器与泛型组合沿用对应类型规则及约束；不能仅因类型为 void 而拒绝，也不能绕过上述禁止项。

## 3 实施范围

| 层次 | 工作及边界 |
| --- | --- |
| Parser／Semantic | 在表达式位置识别 void 值；统一显式类型、默认初始化、推导、赋值、传参及返回检查；支持 void 的相等比较；移除旧的普通值禁令，保留 §2 禁止项 |
| 泛型／组合类型 | 支持 void 实参替换、参数与返回类型传播、成员及容器实例化；联合成员按现有成员身份及分支规则处理；禁止 fit、spec 适配和交叉成员 |
| Codegen／生命周期 | 统一处理无载荷值的局部、字段、参数及返回路径；保留求值顺序、副作用、异常和联合判别信息；不得用“无载荷”代替联合分支身份 |
| ABI／描述符 | 核对普通调用、泛型共享体、间接调用及外部 ABI 的无载荷表示；沿用既有互操作限制，不因源码参数存在而假定 C ABI 接受 void 形参 |
| FT／语言服务 | 保存并恢复 void 表达式和完整类型信息；源码、纯 FT、workspace cache 一致；Hover／SignatureHelp 展示 void，格式化支持新表达式位置 |

性能要求：void 本身不引入结果盒、堆分配或 ARC；容器、联合判别与外围对象的必要成本按既有规则处理。具体布局及调用约定须在实现前核定；需要新增运行时开销或私有 runtime ABI 时，另行提交人工 Review，不借本方案默认授权。

## 4 实施与规范收敛

1. Review 本文；将语义增量归入[类型规范](../specifications/feng-type.md)、[绑定规范](../specifications/feng-binding.md)、[函数规范](../specifications/feng-function.md)及[泛型规范](../specifications/feng-generics-draft.md)对应章节；本文改为引用，避免重复定义。
2. 按 §3 完成实现。通用类型机制不得依赖 Future 名称或 async 标记。
3. 新增编译器测试与 FCTS，完成 §5 验收后单独交付。

正式规范更新及实现完成前，既有禁止 void 用作普通值的条款仍有效；本文示例表示目标行为，不代表当前已支持。既有测试如需修改，另行取得批准。

## 5 验收与独立交付

| 层次 | 必须验证 |
| --- | --- |
| 编译器 | §2 合法声明、表达式与类型推导；空 return、return void、返回 void 表达式及正常落尾；参数个数不变；相等比较合法且结果为 bool；其他运算、成员访问、fit、spec 适配、交叉成员诊断 |
| 泛型／组合 | 泛型参数与返回值传递；void 字段、容器元素及联合成员；无载荷成员与其他联合分支仍可区分；约束检查不被绕过 |
| FCTS | `let x = test()` 正常执行；赋值、传参与返回；void 的 `==`／`!=` 结果；比较操作数按顺序求值且副作用只执行一次，异常正常传播；普通函数、方法与 Lambda 的规则一致 |
| ABI／工具链 | 直接／间接调用及泛型 provider/consumer 一致；既有 extern 调用不回归；源码、纯 FT、workspace cache、Hover 与格式化一致 |
| 性能 | 生成结果不为 void 单独装箱、分配或添加 ARC；无载荷处理不丢失求值或异常路径 |

`test/` 检查诊断、AST 与生成代码，`fcts/` 检查语言行为。实现交付前在沙箱外执行 `make test`，二进制仅在 build/ 或 temp/ 中运行；记录实际验证平台及未运行项。本文自身的文档变更不要求运行回归。

本文可先于 async/await 独立交付。若最终采用本方案，async/await 在此前置能力交付后按自己的两阶段计划实施；若未采用，须重新 Review 其无结果协议，不能把替代方案默认为已批准。
