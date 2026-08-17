# `spec` 实现成员可见性问题备忘

> **状态**：已合并，不再独立实施。
> **记录日期**：2026-08-14。
> **合并日期**：2026-08-17。
> **性质**：engineering 历史备忘，不是语言权威规范。

本文原用于记录“公开 object-form `spec` requirement 可以由同名同结构的
`type seal` 成员自动满足，并通过公开 spec 视角访问”的现有问题。

该问题已经合并到
[`feng-spec-seal-member-draft.md`](./feng-spec-seal-member-draft.md) 的契约
满足规则和实施 TODO 中，作为 `spec seal` 成员能力的一项同步修复。具体
规则、实施范围和完成状态只以该草案为准；本文不再重复定义或维护独立方案。

原始问题背景、候选方向和讨论过程由 Git 历史保留。
