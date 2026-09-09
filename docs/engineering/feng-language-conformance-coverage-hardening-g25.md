# G25：泛型诊断用例补齐实施文档

> 状态：待 Review，尚未实施
>
> 所属实施文档：[Feng 语言正确性用例补齐实施文档](./feng-language-conformance-coverage-hardening-pending.md)
>
> 交付组：G25

本文独立维护 G25 的测试重点、用例 TODO、验收与交付记录。通用授权边界、测试层级、分组原则与
TODO 状态规则沿用[总实施文档第 1～4 节](./feng-language-conformance-coverage-hardening-pending.md#1-文档定位)，
不因文档拆分改变本组范围或批准状态。

## 1 测试重点

验证泛型声明、实例化、约束和推断产生的稳定 Semantic 诊断。

union／intersection 特有的约束准入、收窄、合并成员及约束转传由
[G24 COMPOSITE23～COMPOSITE28](./feng-language-conformance-coverage-hardening-pending.md#28-g24复合类型诊断)
交付；本组映射其证据，负责通用泛型机制与其他约束形式，不重复新增同构复合约束用例。

## 2 用例 TODO

- [ ] GENERIC01：泛型参数重复或声明不合法；
- [ ] GENERIC02：泛型实参数量不匹配；
- [ ] GENERIC03：泛型实参不满足约束；
- [ ] GENERIC04：泛型参数无法完成推断；
- [ ] GENERIC05：显式实例化目标不合法；
- [ ] GENERIC06：对应泛型约束和推断的最小合法邻界程序。

## 3 独立验收与交付 TODO

- [ ] 建立泛型稳定诊断码到现有测试的映射，区分声明、实例化、约束和推断阶段；
- [ ] 独立运行 G25，核对诊断码、位置、数量、阶段和实例化上下文；
- [ ] 在 Codex 沙箱外为 G25 独立执行 `make test`；
- [ ] 执行 `git diff --check`，关闭或决策 G25 问题；
- [ ] 填写“不新增”依据或实际新增用例、专项结果和全量结果。

## 4 独立交付记录

- 状态：待实施
- 稳定码映射与新增用例：—
- 本组专项结果：—
- 本组沙箱外 `make test`：—
- 问题：—
- 建议 commit message：`test: audit generic diagnostics`

## 5 实施问题记录

详细问题只在 [G25 问题记录](./feng-language-conformance-coverage-hardening-issues/g25.md)中维护，
本文在相关 TODO 或交付记录中引用问题编号。编号、状态、模板与处理规则沿用
[总实施文档第 30 节](./feng-language-conformance-coverage-hardening-pending.md#30-实施问题记录)。
