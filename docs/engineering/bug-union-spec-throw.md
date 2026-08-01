# Union-form Spec Throw 代码生成 Bug

> **状态**：已知 Bug（未修复）
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

## 修复方案（待定）

### 方案 1：语义验证拒绝

在 `type_ref_is_throwable` 中增加对 union-form spec 的拒绝：

```c
if (decl->kind == FENG_DECL_SPEC &&
    (decl->as.spec_decl.form == FENG_SPEC_FORM_CALLABLE ||
     decl->as.spec_decl.form == FENG_SPEC_FORM_UNION)) {
    *out_reason = "spec values cannot be thrown as exceptions";
    return false;
}
```

**优点**：简单，与 catch 拒绝所有 spec 保持一致
**缺点**：限制了 union-form spec 的使用场景

### 方案 2：Codegen 特殊处理

为 union-form spec 实现独立的 throw 逻辑：

```c
if (r.type->kind == CG_TYPE_SPEC) {
    if (r.type->user_spec->form == FENG_SPEC_FORM_UNION) {
        // Union-form spec: 需要特殊的 throw 逻辑
        // 可能需要 box 或提取具体成员
    } else {
        // Object-form spec: 提取 subject
    }
}
```

**优点**：允许 throw union-form spec
**缺点**：需要设计 union-form spec 的异常匹配语义（如何 match？按 tag？按具体成员类型？）

### 方案 3：Box 包装

将 union-form spec 值 box 为具体类型，类似 value-semantics 类型的处理方式。

**优点**：统一处理方式
**缺点**：增加运行时开销，需要设计 box 结构

## 建议

考虑到：
1. Catch 已经拒绝所有 spec 类型（包括 union-form）
2. Throw union-form spec 的语义不明确（如何匹配？）
3. 目前没有明确的用例需要 throw union-form spec

**建议采用方案 1**：在语义验证中拒绝 union-form spec throw，与 catch 的限制保持一致。

## 关联

- Intersection-form spec 也会有同样的问题（值结构是 `{ subject, merged_witness }`，与 object-form 一致，理论上可以 throw，但需要确认）
- Catch 拒绝所有 spec 类型（analyzer.c:11725-11729）

## 测试用例

```feng
// 应该在语义验证阶段报错
spec IntOrString: int | string;

func test_throw_union_spec() {
    let v: IntOrString = 42;
    throw v;  // 期望：AE0076 错误
}
```
