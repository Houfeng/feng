# Feng 模块符号冲突规则优化 TODO

目标：将 `import_public_names` 中所有 import 相关的急切报错移除，冲突检测统一推迟到使用点（调用点 / 引用点），使 type / spec / enum / func / let / var 行为统一，符合 `docs/feng-module.md` §7 规范。

## 0. 现状差距（已确认）

- func 调用点已有 AE0513 惰性歧义检查（`current_module_has_local_function` + `find_imported_module_with_public_function`），覆盖了"同模块 vs import"场景。
- 但 `import_public_names` 中仍对 import vs import、import vs 本文件急切报 AE0906，与 `docs/feng-module.md` §7 惰性检查矛盾。
- type / enum / spec / let / var 在使用点没有类似的惰性歧义检查。

设计原则：

- 不新增数据结构，不新增字段。
- func 调用点的 AE0513 检查模式（直接查询模块结构）是参考基准。
- type / enum / spec / let / var 参考 func，在使用点查询模块结构检测冲突。

## 1. 错误码文档更新

- [ ] 1.1 `docs/feng-error-codes-ae.md`：扩展 AE0906 适用范围，涵盖 import 名称二义性场景。
- [ ] 1.2 `docs/feng-error-codes.md`：同步更新 AE0906 / AE0157 描述。

验收：

- 通过 `make test` 全量回归测试。

## 2. Func：移除 `import_public_names` 急切报错

实现职责：

- 移除 `import_public_names` 中 func 的急切报错，让调用点 AE0513 统一处理。

- [ ] 2.1 `import_public_names` → `FENG_DECL_FUNCTION` 分支：删除 import vs import、import vs 本文件的 `append_error` 调用。
- [ ] 2.2 同模块其他文件静默跳过、同一 import 模块去重：保持不变。
- [ ] 2.3 确认 AE0513 调用点检查覆盖 import vs import 场景；若不覆盖，扩展 `find_imported_module_with_public_function` 使其可检测多个 import 模块提供同名 func 的情况。

验收：

- import vs import 同名 func，未使用时不报错，调用时报 AE0513。
- import vs 本文件同名 func，未使用时不报错，调用时报 AE0513。
- import vs 同模块其他文件：行为不变（AE0513 已覆盖）。
- 通过 `make test` 全量回归测试。

## 3. Type / Enum / Spec / Let / Var：移除 `import_public_names` 急切报错

实现职责：

- 移除 `import_public_names` 中 type / enum / let / var 的急切报错。

- [ ] 3.1 `FENG_DECL_TYPE` 分支：删除 import vs import、import vs 本文件的 `append_error` 调用。
- [ ] 3.2 `FENG_DECL_ENUM` 分支：同上。
- [ ] 3.3 `FENG_DECL_GLOBAL_BINDING` 分支：同上。
- [ ] 3.4 同模块其他文件静默跳过、同一 import 模块去重：保持不变。

验收：

- 急切报错移除，使用点尚无歧义检查（后续步骤补齐）。
- 通过 `make test` 全量回归测试。

## 4. Type / Enum / Spec / Let / Var：使用点增加惰性歧义检查

实现职责：

- 参考 func 的 AE0513 模式，在裸名引用点查询模块结构，检测是否存在多来源同名符号。

- [ ] 4.1 新增辅助函数（参考 `current_module_has_local_function` / `find_imported_module_with_public_function` 的模式）：
	- 检查当前模块是否有同名 type / enum / spec / let / var。
	- 检查是否有 import 模块导出同名符号。
- [ ] 4.2 `resolve_expr` → `FENG_EXPR_IDENTIFIER` 分支：查找 visible 条目时，检查是否存在多来源同名。
- [ ] 4.3 `resolve_named_type_ref` → 单段名称查找路径：同上。
- [ ] 4.4 `evaluate_constant_identifier`：同上。
- [ ] 4.5 `extract_match_label_literal` → identifier 分支：同上。
- [ ] 4.6 其他裸名决议位置：补齐检查。

验收：

- import vs import 同名 type / enum / let / var，使用时报歧义错误。
- import vs 本文件同名 type / enum / let / var，使用时报歧义错误。
- import vs 同模块其他文件同名 type / enum / let / var，使用时报歧义错误。
- 未使用的冲突名称不报错。
- 通过 `make test` 全量回归测试。

## 5. 更新测试

实现职责：

- 更新受影响的现有测试，新增惰性行为测试。

- [ ] 5.1 更新现有测试（4 个）：未使用冲突名称的场景改为期望编译成功。
	- `test_imported_type_conflicts_with_local_type`
	- `test_imported_value_conflicts_with_local_value`
	- `test_imported_name_conflicts_between_modules`
	- `test_external_imported_enum_conflicts_with_local_type_name`
- [ ] 5.2 新增测试（6 个）：
	- `test_lazy_ambiguity_import_vs_import`：两个 import 同名 func，使用裸名 → 歧义错误。
	- `test_lazy_ambiguity_import_vs_local_type`：import 同名 type + 本地 type，使用裸名类型标注 → 歧义错误。
	- `test_lazy_ambiguity_import_vs_local_value`：import 同名 func + 本地 func，调用裸名 → 歧义错误。
	- `test_lazy_ambiguity_unused_no_error`：两个 import 同名，未使用 → 编译成功。
	- `test_lazy_ambiguity_resolved_by_qualified_path`：使用完整模块路径 → 编译成功。
	- `test_lazy_ambiguity_resolved_by_alias`：使用 `as` 别名 → 编译成功。

验收：

- 通过 `make test` 全量回归测试。

## 6. 清理

- [ ] 6.1 移除 `import_public_names` 中不再需要的 `is_current_file_conflict` 变量和相关分支。
- [ ] 6.2 确认 `validate_program_alias_conflicts` 和 `find_unshadowed_alias` 无需修改。
- [ ] 6.3 检查并移除不再使用的变量或分支。

验收：

- 通过 `make test` 全量回归测试。

## 7. 依赖关系

```
步骤 1（文档）── 无依赖，可独立执行
步骤 2（func 去急切）── 依赖步骤 1
步骤 3（type/value 去急切）── 依赖步骤 2
步骤 4（type/value 使用点检查）── 依赖步骤 3
步骤 5（测试）── 依赖步骤 2、步骤 4
步骤 6（清理）── 依赖步骤 5
```

---

## 备注：同模块冲突错误码合并（后续单独处理）

当前同模块内的符号冲突使用了 7 个错误码（AE0213/0214/0215/0217/0218/0219/0220），存在不必要的拆分。后续应合并为 2 个：

- **名称重复**（1 个码）：所有非重载的同名冲突统一使用，包括 type vs type、enum vs type、let/var vs func、func vs let/var、let/var vs let/var 等。
- **重载冲突**（1 个码）：所有函数重载冲突统一使用，包括签名完全重复、仅返回值不同、可变参数重载冲突等。

此优化与 `docs/feng-module.md` 中"type / spec / enum / func / let / var 统一适用"的原则对齐，不在本次 import 惰性检查优化范围内。
