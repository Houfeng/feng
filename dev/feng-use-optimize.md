# Feng use / 类型引用优化计划

## 1. 文档定位

- 行为规则以 docs/feng-module.md 为准；本文只记录当前实现现状、差异判断与后续执行计划。
- 本文聚焦 use 导入后的类型引用，不讨论普通表达式成员访问、lambda 语法或 LSP 补全行为。

## 2. 当前规则基线

按当前确认的目标规则，类型位置只支持以下两种写法：

- use module.path; 之后，使用短名类型：Type
- use module.path as alias; 之后，使用别名限定类型：alias.Type

不作为目标能力的写法：

- 直接在类型位置写完整模块路径：module.path.Type

示例：

```feng
use my.app.user;
use my.app.user as user;

let a: User;
let b: user.User;
let c: user.User[];
```

以下写法按当前目标规则应视为不合法：

```feng
use my.app.user;

let a: my.app.user.User;
```

## 3. 当前实际代码支持情况

### 3.1 parser

- src/parser/parser.c 的 parse_type_ref() 通过 parse_path(...) 解析命名类型。
- 这意味着 parser 本身可以接受多段类型名；不会在语法层拒绝 alias.Type 或 module.path.Type。

结论：parser 支持多段类型引用语法。

### 3.2 semantic

src/semantic/analyzer.c 的 find_named_type_decl(...) 当前行为如下：

- 单段名：按当前可见类型查找
- 两段名：若首段命中 use ... as alias 的别名，则解析为 alias.Type
- 多段名：会把前 n-1 段当作模块路径查找，再解析末段类型名

因此，主语义解析当前不仅支持 alias.Type，还比目标规则更宽，实际允许 module.path.Type 进入解析流程。

现有证据：

- test/semantic/test_semantic.c 已有正例：use vendor.api as api; fn project(user: api.User): api.Named { ... }
- 这说明 alias-qualified type 至少在 parser + semantic 路径上已经被实际支持。

结论：主语义解析已支持 alias.Type，并且当前还放宽支持了 module.path.Type。

### 3.3 codegen

- src/codegen/codegen.c 的 cg_resolve_type() 在处理 FENG_TYPE_REF_NAMED 时，遇到 segment_count != 1 会直接失败。
- 报错文案为：codegen: qualified type names not supported in Phase 1A
- 这意味着 codegen 当前仍然只接受单段类型名；alias.Type 与 alias.Type[] 都没有端到端打通。

结论：codegen 目前不支持任何多段类型引用。

### 3.4 端到端支持结论

按当前代码实际状态，可归纳为：

- Type：已支持 end-to-end
- alias.Type：parser 支持，semantic 支持，codegen 不支持
- alias.Type[]：parser 支持，semantic 预期支持，codegen 不支持
- module.path.Type：parser 支持，semantic 当前支持，但按目标规则本应不支持，codegen 不支持

## 4. 判断

这个问题不是单纯“注释或文档没有更新”，也不是“语义和 codegen 都没做”。

当前真实状态是：

1. 模块规则文档已经把“别名.成员名访问公开 type”定义为目标行为。
2. parser 与主语义解析已经具备 alias-qualified type 的基础支持。
3. codegen 仍保留单段类型名假设，导致 alias-qualified type 不能端到端工作。
4. 主语义解析当前还额外放宽了 module.path.Type，这与当前确认的目标规则不一致，需要通过测试锁定后决定是否收紧。

## 5. 测试优先的执行计划

### 5.1 总原则

- 先补 cases，锁定真实规则与真实行为。
- 如果 cases 不通过，按最小实现面补齐。
- 如果 cases 全部通过，只更新说明文字，不额外改实现。
- 不修改已有测试用例，只新增覆盖该问题的 cases。

### 5.2 第一阶段：补 semantic cases

在 test/semantic/test_semantic.c 新增最小覆盖集合：

1. 短名正例
- use vendor.api;
- 在类型位置使用 User

2. 别名正例
- use vendor.api as api;
- 在参数、返回值或绑定类型标注位置使用 api.User

3. 别名数组正例
- use vendor.api as api;
- 在类型位置使用 api.User[]

4. 完整路径负例
- use vendor.api;
- 在类型位置直接写 vendor.api.User
- 该 case 应报错，用于锁定“不支持完整模块路径类型引用”的规则

验证命令：

1. make build/bin/test_semantic
2. ./build/bin/test_semantic

判定规则：

- 如果别名正例失败，说明 semantic 仍有遗漏，先补 semantic
- 如果完整路径负例意外通过，说明 semantic 当前比目标规则更宽，需要在 semantic 层收紧

### 5.3 第二阶段：补 codegen cases

在 test/codegen/test_codegen.c 基于现有 imported source fixture 模式新增：

1. alias.Type 的 codegen 正例
- 例如参数、返回值或局部类型标注使用 api.User

2. alias.Type[] 的 codegen 正例
- 例如数组绑定、函数参数或返回值类型使用 api.User[]

验证命令：

1. make build/bin/test_codegen
2. ./build/bin/test_codegen

判定规则：

- 如果这些 case 失败，并命中 qualified type names not supported in Phase 1A，则确认为 codegen 实现缺口

### 5.4 第三阶段：按最小实现面修复

若 semantic 或 codegen cases 失败，则按以下最小范围修复：

1. semantic 收口
- 若 module.path.Type 负例当前通过，则在 src/semantic/analyzer.c 收紧规则
- 保留 Type 与 alias.Type
- 拒绝直接使用 module.path.Type 作为类型引用

2. codegen 打通 alias-qualified type
- 在 src/codegen/codegen.c 只补 alias.Type 的解析与发射路径
- 不顺手放开任意多段路径类型名
- alias.Type[] 应跟随内层 alias.Type 一并打通

3. 辅助路径评估
- src/semantic/cyclic.c 等辅助路径目前仍保留多段类型名的旧假设
- 只有在 alias-qualified type 确认进入正式 end-to-end 支持面后，再决定是否同步补齐

### 5.5 第四阶段：回归与说明更新

若实现有修改，至少执行：

1. make build/bin/test_semantic
2. ./build/bin/test_semantic
3. make build/bin/test_codegen
4. ./build/bin/test_codegen
5. make test

若新增 cases 全部通过且无需改实现，则只更新说明性文字：

- 把过宽的“限定名类型引用未支持”改为更准确的表述
- 明确写成：
  - 支持短名类型
  - 支持别名限定类型 alias.Type
  - 不支持完整模块路径类型引用 module.path.Type

## 6. 推荐测试落点

- test/semantic/test_semantic.c
  - 负责锁定规则边界
  - 负责确认 parser + semantic 的实际支持面
- test/codegen/test_codegen.c
  - 负责确认 alias-qualified type 是否已经 end-to-end 可用

## 7. 现阶段工作结论

当前最接近事实的结论应为：

- 规则目标：支持 Type 与 alias.Type，不支持 module.path.Type
- 当前实现：
  - parser 已支持多段类型名
  - 主语义已支持 alias.Type，并且当前还放宽支持了 module.path.Type
  - codegen 尚未支持多段类型名，因此 alias.Type 还未端到端完成

因此，下一步最合理的工作方式不是先猜结论，而是先补覆盖 Type / alias.Type / alias.Type[] / module.path.Type 的 cases，再依据结果决定：

- 收紧 semantic
- 打通 codegen
- 或仅更新说明文字
