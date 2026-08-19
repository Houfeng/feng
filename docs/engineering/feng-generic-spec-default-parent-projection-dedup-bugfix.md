# Feng generic spec 默认 parent projection 去重修复开发文档

> 状态：已实施并通过全量回归（2026-08-19）
>
> 本文只修复同一个 canonical generic spec 默认 parent projection 被重复发码的问题，不改变
> spec 默认值、继承、witness、descriptor 或运行时语义。

## 1. 实际失败

跨包 generic child spec 同时具化为 `Surface<int>` 与 `Surface<string>` 时，生成 C 对两个
语义不同的默认 parent projection 给出了同一个静态符号名：

```text
FengSpecDefaultProjection__PackageExplicitOpenSurface__GenericABI
  __as__PackageExplicitOpenBase__GenericABI
```

C 编译器因此报告符号重定义。两个 projection 的 witness 结构 ABI 相同，但其初始化内容
分别引用 `int` 与 `string` 闭合实例的 default thunks，不能选择其中一个简单去重。

## 2. 根因

`cg_ensure_default_parent_witness()` 生成的静态符号只由 root/target 的 canonical
`c_witness_struct_name` 决定，丢失了闭合泛参；缓存又只以两个 `UserSpec *` 地址为键。
generic spec 的不同闭合实例共享 GenericABI 结构，但拥有不同的 exact default witness；
因此当前“符号身份”过粗，而“缓存身份”又可能比同一个 exact 实例的重复注册更细。

这不是跨包、具体 spec 名或具化次数特判，而是缓存键与被缓存产物身份不一致。

## 3. 修复方案

1. projection 静态符号使用 root 与 target 的 exact `c_default_witness_name` 构造，确保
   `Surface<int>` 与 `Surface<string>` 不同名；函数指针结构类型仍使用既有 canonical
   `c_witness_struct_name`，不改变 witness ABI；
2. 缓存按同一 exact default-witness 身份匹配，使相同闭合实例即使由不同 `UserSpec *`
   注册也只发码一次；不同闭合实例绝不合并。

修复只影响编译期去重，不增加运行时分支、查找、缓存、内存或调用层级。

## 4. 测试与 TODO

- [x] **[分析]** 由跨包 generic type/fit 使 `int` / `string` child spec 同时具化，复现同名 C 定义；
- [x] **[分析]** 确认两个 projection 内容引用不同闭合 default thunks，不能简单合并；
- [x] **[分析]** 确认符号名丢失 exact 闭合身份，缓存又只比较对象地址；
- [x] **[实际变更]** 以 exact default-witness 身份生成 projection 符号并缓存；
- [x] **[测试变更]** 增加同一 generic child/parent ABI 经多个上下文具化仍只发码一次的 Codegen 回归；
- [x] **[验证]** 验证不同闭合实例不合并、同一闭合实例不重复发码；
- [x] **[全量回归]** 在非沙箱环境执行 `make test`。

## 5. 实施结果

- projection 符号和缓存都使用 exact default-witness 身份；canonical witness struct ABI
  保持不变；
- 跨包 generic child spec 同时闭合为 `int` 与 `string` 的 FCTS，以及生成 C 编译共同验证
  不同实例不误合并、同一实例不重复发码；
- Codegen 专项测试通过，FCTS `814/814` 通过；非沙箱全量 `make test` 通过。
