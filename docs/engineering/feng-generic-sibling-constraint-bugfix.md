# Feng 泛参作用域与约束实例转传修复

> 状态：已完成，完整 `make test` 通过（2026-09-21）。
>
> 语言规则以[泛型主规范 §4 语义](../specifications/feng-generics-draft.md#4-语义)为准；
> 本文只记录问题、实现方案和验证结果。

## 1. 同名顶层类型干扰泛参成员解析

### 1.1 复现与实际结果

```feng
module sibling.shadow;
type U {}
spec Box<V> { func get(): V; }
func read<T, U: Box<T>>(value: U): T { return value.get(); }
```

修复前，`value.get()` 报 `AE0306: type 'U' has no member 'get'`。
仅将顶层 `type U` 改名为 `type Other`，同一泛型函数即可编译。
使用 `type T {}` 与 `func read<T: Box<U>, U>(value: T): U` 也会报同类错误。

### 1.2 原因与修复位置

`resolve_named_type_ref` 已优先识别活动泛参，但 `resolve_type_ref_decl` 在查找表达式
所属类型时直接查询具名类型声明。存在同名顶层类型时，`resolve_expr_owner_type` 得到
该顶层类型；后续仅在没有具体声明时执行的泛参约束成员查询因此被跳过。

在统一的类型声明解析入口识别活动泛参，使其不返回同名顶层声明，继续交由既有泛参
约束路径处理。类型引用保留在原作用域解析得到的具体声明身份，复制和替换过程传递
该身份，避免同文件的其他函数返回具体同名类型时被调用方泛参重新捕获。
该身份通过编译期 `FengTypeRef.resolution_decl` 保留；消费尚未检查的声明签名时，
先在声明作用域绑定具体类型，使结果不受函数声明先后顺序影响。
保留 `resolution_program` 对其他声明文件中具体类型引用的来源保护。
不得针对 `T`、`U`、`get` 或某个 spec 加入名称特判。

## 2. 开放泛参转传时比较了不同声明作用域中的约束

### 2.1 复现与实际结果

provider 定义泛型父子契约和受约束函数：

```feng
open spec Box<V> { func get(): V; }
open spec Child<V>: Box<V> {}
open func first<A, B: Box<A>>(value: B): A { return value.get(); }
```

consumer 使用相反的参数顺序转传：

```feng
func relay<Z: Child<A>, A>(value: Z): A {
    return first<A, Z>(value);
}
```

修复前，Semantic 接受，跨包 FCTS 在 Codegen 报 `CE0293`，提示约束 witness
不满足父契约前缀兼容要求。固定复现位于
`fcts/fcts_bin/src/test_generic_sibling_constraint.ff` 的 `localSiblingRelay`。

### 2.2 原因与修复位置

`cg_selected_call_generic_descriptor` 对直接泛参实参提前复用 callee 的开放约束，
跳过既有调用点约束代入。源 `Child<A>` 的 `A` 属于 caller 的参数槽，目标 `Box<A>`
仍使用 callee 的参数槽；参数改名或重排后，前缀成员类型比较会混用两个作用域。
`cg_instantiated_generic_descriptor` 的直接泛参分支也存在同类提前返回。

先复用现有的调用点或显式实例化约束替换流程，把目标约束放到 caller 作用域，再由
既有 descriptor 构造与 witness 兼容检查处理。已记录的约束投影继续走既有投影入口，
内建 `throw` 与无约束路径保持各自已有语义；不绕过兼容检查或放宽名义满足关系。

## 3. 范围与成本

- 修复限于编译期名称解析和泛参约束代入，不修改 runtime、私有 ABI 或符号表格式。
- 父契约前缀兼容的直接泛参转传继续使用原 descriptor，不增加运行时查找、装箱、
  witness 适配层或每次调用的 descriptor 构造。
- 新增独立测试文件；已有测试只增加入口注册，不修改既有断言。

## 4. 验证

| 层次 | 验证内容 |
| --- | --- |
| Parser | 两种参数顺序的函数、type、spec、type/fit 方法及嵌套约束 AST |
| Semantic | 显式与推导调用、返回目标推导、泛参槽位、两种顺序的负向诊断、同名类型遮蔽、具体返回类型的声明身份、开放父约束转传 |
| Symbol | public/workspace 两种 FT profile 的泛参身份、provider AST 释放后的独立 consumer |
| Codegen | 两种顺序的共享体与 witness 槽位、重排后的父约束实例、生成 C 可编译性 |
| FCTS | 同包与真实跨包调用、标量/引用/值类型/托管聚合、嵌套数组、四种 spec 约束、type/fit 方法、两项缺陷的行为回归 |
| 全量 | 在沙箱外执行 `make test`，包含 UBSan 与普通构建阶段 |

编译器新增用例位于 `test/{parser,semantic,codegen,symbol}/test_generic_sibling_constraint.c`，
共用 `test/generic_sibling_constraint_helpers.h` 的声明夹具。夹具同时声明空的顶层
`T` 和 `U`，因此两种顺序的函数、owner 和方法都持续受到同名声明的回归检查。
负向检查还验证：无约束泛参不能借用同名具体类型的成员，非法泛参应用 `U<i32>`
仍报告 `AE1012`，错误约束实例仍按既有诊断拒绝。

FCTS 新增 16 组行为测试，provider 位于
`fcts/fcts_lib/src/test/lib_generic_sibling_constraint.ff`，consumer 位于
`fcts/fcts_bin/src/test_generic_sibling_constraint.ff`。父约束转传覆盖两个参数顺序、
直接调用、泛型 owner、函数值，以及 string 和托管聚合结果。

最终验证结果（2026-09-21，macOS arm64）：

- 沙箱外执行完整 `make test`，退出码为 **0**。
- `test-sanitize`（UBSan）和 `test-normal`（`-O2 -Werror`）两个阶段全部通过。
- 两阶段的标准库均为 **607/607**，FCTS 均为 **1508/1508**；包含新增的 16 组行为测试。
- Parser、Semantic、Runtime、Codegen、CLI/DAP、Symbol，以及 smoke、性能约束、
  增量构建、发布和工具链脚本检查均通过。
- `git diff --check` 通过。

验证过程中，前一轮普通阶段的 CLI/DAP 测试曾因标准库
`build/macos-arm64/ir/c/feng.c` 的父目录不存在而中断。未修改该测试或构建逻辑，
原样单独重跑 `build/bin/test_cli` 及最终完整 `make test` 均通过。
目录缺失的触发原因尚未确认，保留此记录供后续复现时追踪。
