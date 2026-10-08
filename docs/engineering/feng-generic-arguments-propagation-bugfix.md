# 泛型参数包过度传播 bugfix 记录

日期：2026-10-08。状态：待后续单独设计和修复。

按用户要求，本问题从[注解参数依赖优化](./feng-annotation-dependency-optimize-dev.md)
中移出，作为独立 bugfix 跟踪。泛型字段原有路径存在同样问题，本次 friend 注解接入
公共依赖后也受到影响。当前沿用既有公共机制，不将本问题的修复作为本次 friend
泛型与跨包支持完整交付的条件；这不表示问题已经修复或额外传参没有成本。

## 1. 问题与复现

以下例子在注解依赖优化前后都能生成 C：

```feng
open module annotation.arguments;
open type Box<T> {}
open func make<T>(): int { return 3; }
open type Reader<T> { let value: int = make<Box<T>>(); }
open type Vault<T> {
    @friend(Reader<T>)
    seal let value: int = 0;
}
open func create(): Vault<int> { return Vault<int>(); }
```

`Reader<T>` 的初始化需要既有 `FengGenericArguments` 参数包。注解接入普通类型依赖
后，`Vault<T>` 也获得该需求，其共享构造入口和调用由两个参数变为三个：

```c
/* 优化前（省略符号的模块限定部分） */
Vault_ctor(self, type_desc);
/* 优化后 */
Vault_ctor(self, type_desc, &static_generic_arguments);
```

该例 `Vault<T>` 的共享初始化／构造函数仅以 `(void)_generic_args` 忽略此参数；
默认构造包装函数则继续转传。宿主并未执行 `Reader<T>` 的构造，不实际消费其
初始化参数包。

字段也有相同的过度传播情况。保留其余声明，将 `Vault<T>` 改为：

```feng
open type Vault<T> {
    let reader: Reader<T>;
}
```

宿主默认初始化转传子参数包，但 `Reader<T>` 的默认零初始化不执行字段初始化
表达式，最终忽略该参数。若字段改为显式初始化
`let reader: Reader<T> = Reader<T>();`，则 Reader 构造过程实际消费该参数包。

## 2. 已观察结果与影响

2026-10-08 的生成 C 对照结果如下：

| 宿主使用方式 | 现有处理 |
| --- | --- |
| `let value: T;` | 从宿主描述符取得泛参，不增加参数包 |
| `let reader: Reader<T>;`，Reader 只有简单初始化 | 从普通类型依赖 slot 取得描述符，不增加参数包 |
| `let reader: Reader<T> = Reader<T>();`，Reader 如第 1 节调用 `make<Box<T>>()` | 宿主转传子参数包，Reader 构造过程实际消费其中的泛参记录 |
| `let reader: Reader<T>;`，Reader 如第 1 节定义 | 宿主默认初始化转传子参数包；Reader 默认零初始化最终忽略该参数 |
| 仅 `@friend(Reader<T>)`，Reader 如第 1 节定义 | 宿主构造入口也接收参数包，但不消费它 |

参数包及其中的指针是静态数据，没有因注解执行 Reader 构造或动态分配参数包。
不过，生成 C 的共享入口确实增加了隐藏指针参数，不能概括为“只有静态 slots 增加”。
本轮没有修改 runtime 结构或参数包协议，也未测量机器码指令数和实际耗时。

## 3. 已确认原因与来源

[generic_arguments.c](../../src/codegen/detail/generic_arguments.c) 中的
`cg_generic_arguments_needed_inner` 沿普通类型依赖传播参数包需求，没有精确区分
宿主实际执行的初始化入口。因此，子类型初始化需要参数包，不代表每个依赖该类型的
宿主都会执行该初始化或使用该参数包。

注解与字段已经共用需求分析、静态参数包生成和传递路径，没有注解专属的参数包逻辑。
该文件相对本次实施前的 `baf61052` 没有变更，字段原有路径也存在上述无实际消费的
传递。问题来源是既有公共需求分析偏宽，本次注解接入扩大了影响范围。

注解参数进入公共依赖的规则引用
[泛型规范 §6.1](../specifications/feng-generics-draft.md#61-开放检查条件)，
本 bugfix 不另设注解依赖类别。

## 4. 后续修复与验证边界

后续应单独设计公共参数包需求分析，使需求与实际执行操作对应；具体方案尚未确定。
需同时覆盖字段与注解、显式构造与默认零初始化，以及跨包共享实现，保留实际执行
操作需要的参数包。不能通过 friend 名称特判或重新建立注解专属 deps 规避问题。

编译器验证应覆盖第 2 节的对照场景，检查生成 C 的入口签名、传参及实际使用，并验证
源码与无 provider 源码的制品消费路径一致。FCTS 验证初始化和显式构造的可观察行为。
实现变更完成后，按仓库要求在沙箱外执行完整 `make test`。

本次仅移交问题记录，未修改参数包实现或测试；既有全量回归通过不表示本问题已修复。
