# Union-form Spec Throw 代码生成 Bug

> **状态**：已修复，并通过 Semantic 定向测试与完整回归
> **日期**：2026-07-07
> **严重程度**：中（会导致编译失败，但不影响正确程序的运行）

---

## 问题描述

Union-form spec 值在 throw 时代码生成错误，会生成无效的 C 代码。

## 根因

### 值结构差异

- **Object-form spec**：`{ subject: void*, witness: Witness* }`
- **Union-form spec**：`{ tag: uint32_t, _fwd: FengForward*, payload: union }`

### 代码生成逻辑

`cg_emit_throw`（codegen.c:24867-24896）对所有 `CG_TYPE_SPEC` 类型统一处理：

```c
if (r.type->kind == CG_TYPE_SPEC) {
    // 提取 .subject 字段
    buf_append_fmt(cg->cur_body,
        "    struct %s %s = %s;\n"
        "    void *%s = %s.subject;\n"  // 访问 .subject
        "    feng_retain(%s);\n"
        "    feng_throw(%s, ((const FengManagedHeader *)%s)->desc);\n",
        ...);
}
```

Union-form spec 在 codegen 类型系统中也是 `CG_TYPE_SPEC`（通过 `user_spec->form == FENG_SPEC_FORM_UNION` 区分），但其值结构没有 `.subject` 字段。

### 语义验证缺失

`type_ref_is_throwable`（analyzer.c:11660-11666）只拒绝 callable-spec：

```c
if (decl->kind == FENG_DECL_SPEC &&
    decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE) {
    *out_reason = "callable-spec values are function types and cannot be thrown";
    return false;
}
```

Union-form spec 通过了语义验证，但在代码生成阶段会生成无效 C 代码。

## 复现

```feng
spec IntOrString: int | string;

func test() {
    let v: IntOrString = 42;
    throw v;  // 语义验证通过，但代码生成失败
}
```

生成的 C 代码会尝试访问 `v.subject`，但 `IntOrString` 的值结构没有 `subject` 字段，导致 C 编译失败。

## 已实施修复

按 [Feng 异常模型规范](../specifications/feng-exception.md#33-catch)，`throw` 与具体类型 `catch` 共用同一个
有限类型分类，object-form、intersection-form、union-form 与 callable-form 在内的所有 `spec` 均在
Semantic 阶段拒绝。实现使用统一分类入口，不再为 union-form 增加独立特判，也不为 spec 增加箱、异常
descriptor 或运行时匹配逻辑。

Codegen 仅保留防御性拒绝：通过 Semantic 的合法程序不可能进入 `CG_TYPE_SPEC` 的 throw 路径。该方案不
增加分配、运行时分支、间接访问、descriptor 操作或 ARC/CC 操作，也不执行运行时查找。

## 关联

- 主规则：[Feng 异常模型规范](../specifications/feng-exception.md#33-catch)
- 实施与测试门禁：[统一 ValueBox 与 throw/catch 对齐方案](./feng-unified-value-box-and-exception-dev.md)

## 测试用例

```feng
// 应该在语义验证阶段报错
spec IntOrString: int | string;

func test_throw_union_spec() {
    let v: IntOrString = 42;
    throw v;  // 期望：稳定的 Semantic “不可作为异常载荷”诊断
}
```
