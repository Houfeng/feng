# Feng 模块符号冲突规则优化 TODO

目标:完整实现 `docs/specifications/feng-module.md` §7 规范定义的六类符号冲突规则,本次聚焦其中第 1/2/3 类(import 相关)的惰性化,使 `type / enum / spec / func / let / var` 在 import 场景下行为统一。

## 0. 规范六类冲突 vs 当前实现 vs 本次优化范围

`docs/specifications/feng-module.md` §7(L156-163)定义了六类符号冲突规则。**关键边界:涉及 import 的总是惰性(规则 1/2/3),纯本地的非 func 总是急切(规则 4)**。两者不重叠:规则 4 中"同模块(无论是否同文件)"特指**不涉及 import 的纯本地冲突**。

| # | 冲突类别 | 规范要求 | 当前实现 | 本次优化 |
|---|---|---|---|---|
| 1 | import vs import | 惰性报错(使用处) | ❌ `import_public_names` 急切报 AE0906 | ✅ 改为惰性 |
| 2 | import vs 本模块其他文件 | 惰性报错(使用处) | ❌ `import_public_names` 急切报 AE0906(走 `is_current_file_conflict=false` 分支) | ✅ 改为惰性 |
| 3 | import vs 本文件 | 惰性报错(使用处) | ❌ `import_public_names` 急切报 AE0906(走 `is_current_file_conflict=true` 分支) | ✅ 改为惰性 |
| 4 | 非 func,同模块(无论是否同文件,不涉及 import) | 急切报错(定义处) | ✅ `check_symbol_conflicts` 急切报 AE0213-AE0216(但跨种类漏检) | ⚪ 不动(跨种类漏检由 scope 优化处理) |
| 5 | func,同模块(无论是否同文件,不涉及 import) | 允许重载,重载冲突则急切报错 | ✅ `check_symbol_conflicts` 急切报 AE0217-AE0220 | ⚪ 不动 |
| 6 | 别名 | 急切报错(声明处) | ✅ `validate_program_alias_conflicts` 实现 | ⚪ 不动 |

本次优化聚焦于**第 1/2/3 类(import 相关)**的惰性化,不动第 4/5/6 类的现有逻辑。

### 0.1 当前第 1/2/3 类的具体问题

代码位置:`import_public_names`(`analyzer.c:17317`)

当前 TYPE 分支(L17357-17411)逻辑(ENUM/SPEC/VALUE 分支同构):

```c
index = find_visible_type_index(*visible_types, *visible_type_count, name);
if (index < *visible_type_count) {
    if ((*visible_types)[index].provider_module == target_module) {
        break;  // 同一 import 模块去重(合理)
    }
    bool is_current_file_conflict =
        ((*visible_types)[index].provider_module == current_module &&
         (*visible_types)[index].provider_program == program);
    if ((*visible_types)[index].provider_module == current_module &&
        !is_current_file_conflict) {
        break;  // 场景 2: import vs 同模块其他文件
    }
    // 场景 1(import vs import) 或场景 3(import vs 本文件):
    append_error(..., "AE0906", ...);  // 急切报错
    break;
}
```

问题:
- 场景 1/2/3 全部在 import 阶段处理(场景 1/3 急切报错,场景 2 break)
- 完全没有在使用点检测歧义
- 与规范"惰性报错(使用处)"矛盾

func 调用点有部分惰性检查(`current_module_has_local_function` + `find_imported_module_with_public_function`,`analyzer.c:16955-16972`),但仅覆盖场景 2 的 func 部分,且错误码 AE0513 未文档化。

type / enum / spec / let / var 在使用点**完全没有**惰性歧义检查。

### 0.2 第 4/5/6 类的现状(不在本次优化范围)

- **第 4 类**:`check_symbol_conflicts`(`analyzer.c:22063`)处理同模块内重复定义。当前按 `visible_types` / `visible_values` 两张分表分别检查,**跨种类同名漏检**(如 type vs func、type vs let/var)。原因:TYPE/ENUM/SPEC 分支只查 `visible_types`(L22094/22127/22337),FUNCTION/GLOBAL_BINDING 分支只查 `visible_values`(L22161/22207)。此漏检由 scope 优化阶段统一修复(scope 文档 §0.3 第 5 点已识别)。
- **第 5 类**:`check_symbol_conflicts` FUNCTION 分支(L22205)实现重载冲突检测,符合规范。
- **第 6 类**:`validate_program_alias_conflicts`(`analyzer.c:17667`)+ `find_unshadowed_alias`(`analyzer.c:4003`)实现别名急切检查,符合规范。

### 0.3 设计原则(关键决策)

- **简化 `import_public_names` 的责任**:不做任何冲突检查,只负责"把 import 的 public 符号加入 visible_types / visible_values,保持现有 name 唯一约束(已存在同名则不重复添加)"。冲突检测完全交给使用点通用查找。
- **通用查找独立遍历模块结构**:`collect_symbol_candidates` 直接遍历当前模块 + import 模块的 declarations,**不依赖 visible_types / visible_values 内容**,自然发现所有候选(包括 import_public_names 未添加的)。
- **不破坏现有数据结构约束**:visible_types / visible_values 保持 name 唯一,现有 `find_visible_type` 等函数不变。
- **一个错误码(AE0005)** 统一覆盖第 1/2/3 类惰性歧义,不同场景可有不同 message。
- **func 重载不是普通符号冲突**(第 5 类):由专门的重载决议处理;通用查找只负责跨来源歧义检测(第 1/2/3 类)。
- **步骤 2/3/4 必须作为紧密一组提交**,中间状态不允许存在(否则语义不正确)。

### 0.4 设计取舍

为什么不让 import 版本入 visible_types(允许多 entry)?
- 会破坏 visible_types / visible_values 的 name 唯一约束
- 现有 `find_visible_type` / `find_visible_value` / `find_visible_type_decl` 等函数假设 name 唯一,允许多 entry 会导致它们行为不确定
- 需要修改 6+12+ 处调用点,改动大
- scope 重构会重写这部分,本次大改无意义

为什么通用查找独立遍历模块结构,而不复用 visible_types?
- import 版本不入 visible_types,所以基于 visible_types 找不到所有候选
- 独立遍历保证查找到所有候选(包括被 import_public_names 跳过的)
- 查询复杂度 O(M*N),但仅在使用点检查时调用,实际名字碰撞极少(< 1%),性能可接受
- scope 重构时会被 `scope_chain_find` 替代,数据冗余自然消失

## 1. 错误码文档更新

- [x] 1.1 `docs/specifications/feng-error-codes-ae.md` 通用段(AE00)新增 AE0005:
    - 用途:import 引入名称的二义/多义冲突(规范第 1/2/3 类)
    - 触发位置:裸名引用时(惰性)
    - 涵盖场景:import vs import / import vs 本模块其他文件 / import vs 本文件
    - 适用种类:type / enum / spec / func / let / var 统一适用
    - 消息模板(同码不同 message,按场景区分):
        - import vs import(2 个 import):`'<name>' is ambiguous: imported from '<module1>' and '<module2>'; use a fully-qualified path or import alias to disambiguate`
        - import vs 本模块:`'<name>' is ambiguous: defined in current module and also imported from '<module>'; use a fully-qualified path or import alias to disambiguate`
        - 多 import(≥3 个):`'<name>' is ambiguous: imported from multiple modules (<module1>, <module2>, ...); use a fully-qualified path or import alias to disambiguate`
- [x] 1.2 `docs/specifications/feng-error-codes-ae.md` AE0906 标记为废弃(本次优化后不再产生新错误,文档保留向后兼容)。
- [x] 1.3 ~~`docs/specifications/feng-error-codes.md` 同步新增 AE0005 条目。~~ **按人工决策跳过**:`docs/specifications/feng-error-codes.md` 索引文档滞后严重(代码已使用分段方案 AE1302/AE1303 等,索引仍写 AE0005/AE0006 等旧编号且与代码不一致),本次不参考该文档,错误码以 `docs/specifications/feng-error-codes-ae.md` 分段方案为准。

预留编号(本次不引入,scope 阶段使用):
- AE0002:第 4 类合并,同模块符号重复定义(非 func),替代 AE0213 / AE0214 / AE0215 / AE0216。
- AE0004:第 5 类合并,同模块函数重载冲突,替代 AE0217 / AE0218 / AE0219 / AE0220。

验收:

- 错误码文档完整,AE0005 描述清晰,涵盖所有适用场景。
- 通过 `make test` 全量回归测试。

## 2. Func:简化 `import_public_names` 责任 + 通用查找接入调用点

实现职责:

- 简化 `import_public_names` FUNCTION+GLOBAL_BINDING 分支:删除所有冲突检查(急切报错 + `is_current_file_conflict` 判定),只保留"已存在同名则不添加"的简单逻辑。
- 函数调用点用通用查找替代 AE0513 旧检查,覆盖全部三种场景。
- 删除非通用的 `current_module_has_local_function` / `find_imported_module_with_public_function`。

- [x] 2.1 `import_public_names` → `FENG_DECL_FUNCTION` + `FENG_DECL_GLOBAL_BINDING` 合并分支(`analyzer.c:17469`):
    - 删除场景 1/3 的 `append_error` 调用(L17500-17517)。
    - 删除 `is_current_file_conflict` 变量与同模块其他文件特判(L17492-17498)。
    - 简化为:`find_visible_value_index` 发现已存在同名 → `break`(不报错,不重复添加)。
    - 保留同一 import 模块去重(`find_slice_index` 检查 seen_value_names)。
- [x] 2.2 函数调用点 `resolve_expr` → `FENG_EXPR_CALL`(callee 为 identifier,`analyzer.c:16936`):
    - 删除 `current_module_has_local_function` + `find_imported_module_with_public_function` + AE0513 检查(L16955-16972)。
    - 改用通用查找(见步骤 4.1):`collect_symbol_candidates` + `filter_candidates_by_lookup_kind(VALUE)` + `candidates_form_ambiguity`。
    - 通过歧义检测后才进入 `resolve_top_level_function_overload`(func 重载决议保持不变,处理第 5 类)。

验收:

- 场景 1(import vs import 同名 func),未使用不报错,调用时按场景报 AE0005。
- 场景 2(import vs 同模块其他文件同名 func),未使用不报错,调用时按场景报 AE0005。
- 场景 3(import vs 本文件同名 func),未使用不报错,调用时按场景报 AE0005。
- 第 5 类(同模块 func 重载)不受影响。
- 通过 `make test` 全量回归测试。

## 3. Type / Enum / Spec:简化 `import_public_names` 责任

实现职责:

- 简化 `import_public_names` TYPE / ENUM / SPEC 分支:同步骤 2.1。

- [x] 3.1 `FENG_DECL_TYPE` 分支(`analyzer.c:17357`):
    - 删除场景 1/3 的 `append_error` 调用(L17387-17398)。
    - 删除 `is_current_file_conflict` 变量与同模块其他文件特判(L17374-17380)。
    - 简化为:`find_visible_type_index` 发现已存在同名 → `break`(不报错,不重复添加)。
    - 保留同一 import 模块去重(`find_slice_index` 检查 seen_type_names)。
- [x] 3.2 `FENG_DECL_ENUM` 分支(`analyzer.c:17413`):同上(L17433-17454)。
- [x] 3.3 `FENG_DECL_SPEC` 分支(`analyzer.c:17559`):同上(L17576-17600)。
- [x] 3.4 `FENG_DECL_GLOBAL_BINDING` 已在步骤 2.1 处理(与 FUNCTION 合并分支)。
- [x] 3.5 `FENG_DECL_FIT` 分支(`analyzer.c:17615`)是空操作,无需修改。

验收:

- 第 1/2/3 类的急切报错全部移除。
- `import_public_names` 责任单一化:不冲突检查,只添加新名字。
- visible_types / visible_values 保持现有 name 唯一约束,现有 `find_visible_type` 等函数行为不变。
- 通过 `make test` 全量回归测试。

## 4. 通用符号查找基础设施 + 使用点接入

实现职责:

- 设计一个通用的符号查找方法,**独立遍历模块结构**(不依赖 `visible_types` / `visible_values`),服务于所有使用点的惰性歧义检测(场景 1/2/3)。
- func 重载由专门路径处理(第 5 类),通用查找只做跨来源歧义检测。

### 4.1 基础设施

- [x] 4.1.1 定义 `SymbolCandidate` 结构体:

  ```c
  typedef struct {
      const FengSemanticModule *provider_module;
      const FengProgram *provider_program;
      const FengDecl *decl;  /* decl->kind 表达符号种类 */
  } SymbolCandidate;
  ```

- [x] 4.1.2 实现 `collect_symbol_candidates`:

  ```c
  /* 收集当前文件作用域中名为 name 的所有候选符号,不分类别。
   * 独立遍历模块结构,不依赖 visible_types/visible_values(因为
   * import_public_names 跳过了与已有候选同名的 import 版本)。
   * 来源:
   *   1. 当前模块所有 program 的 declarations(全可见性,本模块内部全可见)
   *   2. 当前文件 import 的所有模块所有 program 的 declarations(仅 public)
   * 不去重,同模块多文件同名各自作为候选。
   * fit decl 跳过(不引入命名符号)。 */
  static size_t collect_symbol_candidates(const ResolveContext *context,
                                          FengSlice name,
                                          SymbolCandidate *out_candidates,
                                          size_t capacity);
  ```

- [x] 4.1.3 定义 `SymbolLookupKind` 枚举 + `filter_candidates_by_lookup_kind`:

  ```c
  typedef enum {
      SYMBOL_LOOKUP_TYPE,   /* type/enum/spec */
      SYMBOL_LOOKUP_VALUE,  /* func/let/var */
  } SymbolLookupKind;

  static size_t filter_candidates_by_lookup_kind(const SymbolCandidate *in, size_t count,
                                                 SymbolLookupKind kind,
                                                 SymbolCandidate *out, size_t capacity);
  ```

- [x] 4.1.4 实现 `candidates_form_ambiguity`:

  ```c
  /* 判定候选集是否构成惰性歧义(规范第 1/2/3 类)。
   * 规则:provider_module 不唯一 → 歧义。
   * 第 4 类(同模块非 func 重复定义)已由 check_symbol_conflicts 急切报错;
   * 第 5 类(同模块 func 重载)由重载决议处理,不算歧义(provider_module 相同)。 */
  static bool candidates_form_ambiguity(const SymbolCandidate *candidates, size_t count);
  ```

- [x] 4.1.5 实现 `format_ambiguity_message`:根据 provider_module 数量与来源类型(import vs import / import vs 本模块 / 多 import ≥3)选择对应 message 模板(见步骤 1.1)。

- [x] 4.1.6 候选缓冲区大小:调用方提供栈上数组,默认容量 16(实际名字碰撞通常 < 8);超出时截断并在 debug 构建中断言。

### 4.2 使用点接入

- [x] 4.2.0 **使用点位置审计**(前置任务):grep 所有 `find_visible_type` / `find_visible_value` / `find_visible_type_decl` 调用点(6 + 12 + ?处),逐一确认是否需要接入惰性歧义检测。产出清单应覆盖以下位置:
    - 类型引用位置:`catch` 类型 / `is`-`as` 类型 / lambda 参数类型 / 构造目标类型 / `let`-`var` 类型标注 / 函数参数与返回类型 / 数组元素类型 等
    - 值引用位置:表达式 identifier / 函数调用入口 / 常量求值 / match 标签 / 赋值左侧 / enum 变体引用 等
    - spec 引用位置:`fit` 关键字后的 spec 引用 / spec 继承列表 等
- [x] 4.2.1 `resolve_expr` → `FENG_EXPR_IDENTIFIER` 分支(`analyzer.c:18145` 附近):值引用前先做歧义检测(VALUE 过滤),通过后走原决议。
- [x] 4.2.2 `resolve_expr` → `FENG_EXPR_CALL` callee 为 identifier 分支(`analyzer.c:16936` 附近):函数调用前先做歧义检测(VALUE 过滤),通过后进入重载决议。此步骤与 2.2 合并实现。
- [x] 4.2.3 `resolve_named_type_ref` 单段名称查找路径(`analyzer.c:17756` 附近):类型引用前先做歧义检测(TYPE 过滤)。
- [x] 4.2.4 `evaluate_constant_identifier`(`analyzer.c:13457` 附近):常量上下文值引用前先做歧义检测(VALUE 过滤)。
- [x] 4.2.5 `extract_match_label_literal` identifier 分支(`analyzer.c:6223` 附近):match 标签前先做歧义检测(VALUE 过滤)。
- [x] 4.2.6 步骤 4.2.0 审计清单中的其他位置:逐一接入。

验收:

- 场景 1/2/3 同名 type / enum / spec / let / var / func,使用裸名时按场景报 AE0005。
- 交叉类型歧义(import type + 本地 let 同名,使用裸名时按场景报 AE0005)。
- 未使用的冲突名称不报错。
- 按模块主规范的三种名称访问形式,使用完整模块路径或 import 别名消歧义后编译成功。
- 第 4/5/6 类不受影响。
- 通过 `make test` 全量回归测试。

## 5. 更新测试

实现职责:

- 更新受影响的现有测试(对应场景 1/2/3 的预期改变)。
- 新增覆盖场景 1/2/3 与交叉类型的测试。

- [x] 5.1 更新现有测试(4 个):改为期望惰性报错。
    - 测试用例中加入裸名引用代码,使惰性检查被触发。
    - 错误码期望从 AE0906 改为 AE0005。
    - 涉及测试:
        - `test_imported_type_conflicts_with_local_type`(`test_semantic.c:6488`)— 场景 3
        - `test_imported_value_conflicts_with_local_value`(`test_semantic.c:6514`)— 场景 3
        - `test_imported_name_conflicts_between_modules`(`test_semantic.c:6544`)— 场景 1
        - `test_external_imported_enum_conflicts_with_local_type_name`(`test_semantic.c:7350`)— 场景 3
- [x] 5.2 新增测试(9 个):
    - `test_lazy_ambiguity_import_vs_import`:两个 import 同名 func,使用裸名 → AE0005(场景 1)。
    - `test_lazy_ambiguity_import_vs_local_type`:import 同名 type + 本地 type,使用裸名类型标注 → AE0005(场景 3)。
    - `test_lazy_ambiguity_import_vs_local_value`:import 同名 func + 本地 func,调用裸名 → AE0005(场景 3)。
    - `test_lazy_ambiguity_unused_no_error`:两个 import 同名,未使用 → 编译成功。
    - `test_lazy_ambiguity_resolved_by_qualified_path`:原交付用例实际使用 import alias；G02 已将其修正为真正的完整模块路径并完成回归。
    - `test_lazy_ambiguity_resolved_by_alias`:使用 `as` 别名 → 编译成功。
    - `test_lazy_ambiguity_import_vs_other_file_in_same_module`:import + 同模块其他文件同名,使用裸名 → AE0005(场景 2)。
    - `test_lazy_ambiguity_spec_reference`:spec 引用位置的歧义 → AE0005。
    - `test_lazy_ambiguity_cross_kind_import_func_vs_local_let`:import func + 本地 let 同名,调用裸名 → AE0005(交叉类型)。
- [x] 5.3 拒绝"临时禁用测试"窗口期:步骤 2/3/4 必须作为紧密一组提交,中间状态不允许存在(否则语义不正确)。

验收:

- 通过 `make test` 全量回归测试。

## 6. 清理

- [x] 6.1 删除非通用函数:
    - `current_module_has_local_function`(`analyzer.c:9482`)
    - `find_imported_module_with_public_function`(`analyzer.c:9460`)
- [x] 6.2 确认 `import_public_names` 中已无 `is_current_file_conflict` 变量(步骤 2.1 / 3.1-3.3 已删除)。
- [x] 6.3 确认第 4/5/6 类相关函数无需修改:
    - 第 4 类:`check_symbol_conflicts` 保持现状(跨种类漏检由 scope 优化处理)。
    - 第 5 类:func 重载决议(`resolve_top_level_function_overload` 等)保持现状。
    - 第 6 类:`validate_program_alias_conflicts`(`analyzer.c:17667`)和 `find_unshadowed_alias`(`analyzer.c:4003`)保持现状。
- [x] 6.4 检查并移除不再使用的变量或分支。

验收:

- 通过 `make test` 全量回归测试。

## 7. 与 scope 优化的关系

本方案是过渡实现,在现有 `visible_types` / `visible_values` 双表架构上实现惰性检查。

- `import_public_names` 责任单一化(不冲突检查,只添加新名字),scope 重构时该函数会被简化或合并到作用域链构建逻辑。
- 通用查找 `collect_symbol_candidates` 独立遍历模块结构,scope 重构后会被 `scope_chain_find` 替代,但语义和错误码(AE0005)不变。
- 测试不需要在 scope 阶段重写。
- scope 阶段还会处理:
    - 第 4 类跨种类漏检(`visible_types` / `visible_values` 合并为统一作用域链)。
    - AE0002 / AE0004 编号预留:第 4/5 类合并为统一的"同模块符号重复定义/重载冲突"错误码(对应 scope 文档"备注"提到的合并任务)。

## 8. 依赖关系

```
步骤 1(错误码文档,引入 AE0005)
   ↓
步骤 2(func 去急切 + 调用点接入通用查找) ┐
步骤 3(type/enum/spec 去急切)             ├─ 原子提交(语义正确性)
步骤 4(通用查找基础设施 + 使用点接入)      ┘
   ↓
步骤 5(测试)
   ↓
步骤 6(清理)
```

注意:步骤 2 / 3 / 4 必须同步完成并提交,中间状态(急切报错已移除但惰性检查未接入)语义不正确,不允许存在。

---

## 备注:第 4/5 类同模块冲突错误码合并(scope 阶段处理)

当前同模块内的符号冲突(第 4/5 类)使用了 8 个错误码(AE0213 / AE0214 / AE0215 / AE0216 / AE0217 / AE0218 / AE0219 / AE0220),存在不必要的拆分。scope 阶段应合并为 2 个 AE00 段错误码:

- **AE0002 名称重复**(1 个码):所有非重载的同名冲突统一使用,包括 type vs type、enum vs type、let/var vs func、func vs let/var、let/var vs let/var 等。同时修复第 4 类的跨种类漏检。
- **AE0004 重载冲突**(1 个码):所有函数重载冲突统一使用,包括签名完全重复、仅返回值不同、可变参数重载冲突等。

此优化与 `docs/specifications/feng-module.md` 中"type / spec / enum / func / let / var 统一适用"的原则对齐,属于 scope 阶段工作,不在本次 import 惰性检查优化范围内。
