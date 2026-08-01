# Feng use / 类型引用优化计划

## 1. 文档定位

- 行为规则以 docs/specifications/feng-module.md 为准；本文只记录当前实现现状、差异判断与后续执行计划。
- 本文聚焦 use 导入后的类型引用，不讨论普通表达式成员访问、lambda 语法或 LSP 补全行为。

## 2. 当前规则基线

按当前确认的目标规则，类型位置支持以下三种写法：

- import module.path; 之后，使用短名类型：Type
- import module.path as alias; 之后，使用别名限定类型：alias.Type
- 完整模块路径类型引用不要求先 use，直接使用：module.path.Type

不作为目标能力的写法：

- 未经 use 导入，使用短名或别名访问其他模块类型

示例：

```feng
import my.app.user;
import my.app.user as user;

let a: User;
let b: user.User;
let c: user.User[];
let d: my.app.user.User;
let e: my.app.user.User[];
```

以下写法按当前目标规则应视为不合法：

```feng
let a: User;
let b: user.User;
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

因此，主语义解析当前支持 alias.Type，也支持 module.path.Type，但 module.path.Type 仍错误地维持了“必须先 use 才可见”的旧约束。

现有证据：

- test/semantic/test_semantic.c 已有正例：import vendor.api as api; func project(user: api.User): api.Named { ... }
- 这说明 alias-qualified type 至少在 parser + semantic 路径上已经被实际支持。

结论：主语义解析已支持 alias.Type；module.path.Type 的路径解析已有基础，但可见性规则需要改成不依赖 use。

### 3.3 codegen

- src/codegen/codegen.c 的 cg_resolve_type() 在处理 FENG_TYPE_REF_NAMED 时，遇到 segment_count != 1 会直接失败。
- 报错文案为：codegen: qualified type names not supported in Phase 1A
- 这意味着 codegen 当前仍然只接受单段类型名；alias.Type、alias.Type[]、module.path.Type 与 module.path.Type[] 都没有端到端打通。

结论：codegen 目前不支持任何多段类型引用。

### 3.4 端到端支持结论

按当前代码实际状态，可归纳为：

- Type：已支持 end-to-end
- alias.Type：parser 支持，semantic 支持，codegen 不支持
- alias.Type[]：parser 支持，semantic 预期支持，codegen 不支持
- module.path.Type：parser 支持，semantic 路径解析已有基础但仍错误要求 use，codegen 不支持
- module.path.Type[]：parser 支持，semantic 路径解析已有基础但仍错误要求 use，codegen 不支持

## 4. 判断

这个问题不是单纯“注释或文档没有更新”，也不是“语义和 codegen 都没做”。

当前真实状态是：

1. 模块规则文档已经把“别名.成员名访问公开 type”定义为目标行为，本轮再进一步扩展为完整路径类型引用不需要先 use。
2. parser 与主语义解析已经具备 alias-qualified type 与 module.path.Type 的基础支持。
3. semantic 当前对 module.path.Type 仍复用了 use 可见性约束，需要调整为按模块完整路径直接解析公开类型。
4. codegen 仍保留单段类型名假设，导致多段类型引用不能端到端工作。
5. 部分辅助路径仍保留“多段命名类型跳过或单段限定”的旧假设，需要在端到端支持完成后一起补齐。

## 5. 测试优先的执行计划

### 5.1 总原则

- 先补 cases，锁定真实规则与真实行为。
- 如果 cases 不通过，按最小实现面补齐。
- 如果 cases 全部通过，只更新说明文字，不额外改实现。
- 不修改已有测试用例，只新增覆盖该问题的 cases。

### 5.2 第一阶段：补 semantic cases

在 test/semantic/test_semantic.c 新增最小覆盖集合：

1. 短名正例
- import vendor.api;
- 在类型位置使用 User

2. 别名正例
- import vendor.api as api;
- 在参数、返回值或绑定类型标注位置使用 api.User

3. 别名数组正例
- import vendor.api as api;
- 在类型位置使用 api.User[]

4. 完整路径正例
- 不写 import vendor.api;
- 在类型位置直接写 vendor.api.User

5. 完整路径数组正例
- 不写 import vendor.api;
- 在类型位置直接写 vendor.api.User[]

6. 未导入短名 / 别名负例
- 不写 import vendor.api;
- 在类型位置直接写 User 或 api.User
- 该 case 应报错，用于锁定“短名与别名仍必须先 use”的规则

验证命令：

1. make build/bin/test_semantic
2. ./build/bin/test_semantic

判定规则：

- 如果完整路径正例失败，说明 semantic 或辅助路径仍有遗漏，先补解析链路
- 如果未导入短名 / 别名负例意外通过，说明 use 规则被放宽过头，需要收口

### 5.3 第二阶段：补 codegen cases

在 test/codegen/test_codegen.c 基于现有 imported source fixture 模式新增：

1. alias.Type 的 codegen 正例
- 例如参数、返回值或局部类型标注使用 api.User

2. alias.Type[] 的 codegen 正例
- 例如数组绑定、函数参数或返回值类型使用 api.User[]

3. module.path.Type 的 codegen 正例
- 例如参数、返回值或局部类型标注使用 vendor.api.User
- 不额外编写 import vendor.api;

4. module.path.Type[] 的 codegen 正例
- 例如数组绑定、函数参数或返回值类型使用 vendor.api.User[]
- 不额外编写 import vendor.api;

验证命令：

1. make build/bin/test_codegen
2. ./build/bin/test_codegen

判定规则：

- 如果这些 case 失败，并命中 qualified type names not supported in Phase 1A，则确认为 codegen 实现缺口

### 5.4 第三阶段：按最小实现面修复

若 semantic 或 codegen cases 失败，则按以下最小范围修复：

1. semantic / 辅助路径补齐
- 若完整路径正例在部分语境失败，则补齐 semantic / 辅助路径对已 use 模块全路径类型引用的支持
- 若完整路径引用指向外部包模块，则在 semantic 预注入阶段按 type ref 的完整路径加载目标模块
- 完整路径类型引用不要求 use；短名与 alias.Type 继续保留 use 约束
- 保留 Type、alias.Type 与 module.path.Type 三种合法形式

2. codegen 打通多段类型引用
- 在 src/codegen/codegen.c 打通 alias.Type 与 module.path.Type 的解析与发射路径
- alias.Type[] 与 module.path.Type[] 应跟随内层元素类型一并打通
- 不顺手放开“未 use 也能通过短名或别名访问其他模块类型”的能力

3. 泛型链路评估
- 检查 generic type/spec 的多段 decl 查找是否仍保留单段假设
- 若存在，则与普通多段命名类型一起打通，避免出现“非泛型可用、泛型全路径仍坏”的半支持状态

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
  - 支持不依赖 use 的完整路径类型引用 module.path.Type

## 6. 推荐测试落点

- test/semantic/test_semantic.c
  - 负责锁定规则边界
  - 负责确认 parser + semantic 的实际支持面
- test/codegen/test_codegen.c
  - 负责确认 alias-qualified type 与完整路径类型引用是否已经 end-to-end 可用

## 7. 现阶段工作结论

当前最接近事实的结论应为：

- 规则目标：支持 Type、alias.Type 与不依赖 use 的 module.path.Type
- 当前实现：
  - parser 已支持多段类型名
  - 主语义已支持 alias.Type；module.path.Type 的路径解析已有基础，但仍需去掉 use 约束并补外部包预注入
  - codegen 尚未支持多段类型名，因此 alias.Type 与 module.path.Type 还未端到端完成

因此，下一步最合理的工作方式不是先猜结论，而是先补覆盖 Type / alias.Type / alias.Type[] / module.path.Type / module.path.Type[] 的 cases，再依据结果决定：

- 补齐 semantic / 辅助路径
- 打通 codegen
- 或仅更新说明文字
