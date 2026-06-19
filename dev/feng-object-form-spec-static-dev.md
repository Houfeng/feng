# object-form spec 静态成员声明与泛型分派

## 功能概述

允许 object-form `spec` 声明静态成员（`static let`、`static var`、`static func`），使 spec 能完整描述 type 的类型级能力约束；支持泛型约束中通过类型参数访问静态成员（`T.make()`、`T.field`）。

**核心原则：object-form spec 本质是对 type 的约束描述；type 有什么能力，spec 就能描述什么能力。**

**核心规则：**

- `spec` 中允许声明 `static let`、`static var`、`static func`（均无实现体/初始值）
- 满足类型（type 或 fit）必须提供匹配的静态成员实现
- 静态成员通过类型名访问（`Widget.make()`），不通过实例访问（与 type 规则一致）
- 泛型约束中支持 `T.make()` 和 `T.field`（`T` 为类型参数，`T: Spec`）
- 非 spec 视角（直接 `Widget.make()`）调用开销不变，仍为直接 C 函数调用
- spec 视角（泛型 `T.make()`）通过 witness 表间接分派

**静态方法的语义边界（关键决策）：**

- **spec 没有 constructor 概念**（静态和实例都不检查方法名 == spec 名）：
  - 用户决策：spec 中"方法名 == spec 名视为构造器并禁止"的规则没有意义，因为具体 type 名不会和 spec 名相同（如 `type Widget: Factory<Widget>` 中 Widget != Factory）
  - **parser 层移除** spec 路径中 `slice_equals(name, spec_name)` 的 constructor 判定（`parser.c:1673`），方法名 == spec 名直接视为 `FENG_TYPE_MEMBER_METHOD`
  - **analyzer 层移除** spec CONSTRUCTOR 检查（`analyzer.c:19875-86`），保留 FINALIZER 检查
  - 适用范围：spec **静态**方法 和 **实例**方法 都放宽（一步到位）
- **spec 一律不允许 `~` 前缀**（不论静态或实例）：终结器只允许用于 `type`。此约束**已在语义层落地**（`analyzer.c:19892`，错误码 AE0620），并已有测试 `test_object_form_spec_rejects_finalizer_member`。解析器**不需要新增特判**——spec 方法 `~` 前缀会解析为 FINALIZER 成员，自动触发 AE0620 拒绝。

> **关联变更**：本次会一并放宽 spec **实例**方法名 == spec 名（移除 constructor 概念），并清理相关测试 `test_object_form_spec_rejects_constructor_member`。

---

## 设计决策

### 1. 静态成员进入 witness 表

静态成员与实例成员统一进入 witness 表，按源码声明顺序混合排列。

**witness slot 区别：**

| 成员类型 | witness slot 签名 |
|---------|-------------------|
| 实例方法 | `RetType (*method)(void *_subject, params...)` |
| 静态方法 | `RetType (*static_method)(params...)` — 无 `_subject` |
| 实例字段 get | `T (*get_field)(void *_subject)` |
| 静态字段 get | `T (*get_static_field)(void)` — 无 `_subject` |
| 实例字段 set（var） | `void (*set_field)(void *_subject, T value)` |
| 静态字段 set（static var） | `void (*set_static_field)(T value)` — 无 `_subject` |

**slot 签名由 `spec_member->is_static` 决定**，与 witness source kind 无关。

**生成的 C 代码示例：**

```c
// spec Factory<T> { static func make(): T; let name: string; }
struct FengSpecWitness__M__Factory {
    // 静态方法 slot（无 _subject）
    void *(*make)(void);
    // 实例字段 slot（有 _subject）
    struct Feng__M__string (*get_name)(void *_subject);
};
```

### 2. 复用 FengGenericParamDescriptor.witness

当前泛型函数已通过 `FengGenericParamDescriptor` 传递 witness 表：

```c
typedef struct FengGenericParamDescriptor {
    FengValueKind   kind;
    const void     *descriptor;
    const void     *witness;   // ← 已承载 spec witness 表
} FengGenericParamDescriptor;
```

泛型函数签名已为每个类型参数传递 descriptor：

```c
void Feng__create(
    const FengFunctionDescriptor *_desc,
    const FengGenericParamDescriptor *T,  // T.witness 指向 witness 表
    void *_out
);
```

泛型约束中 `T.make()` 的发码方式与实例方法一致，仅去掉 `_subject`：

```c
// 实例方法（现有）: ((const struct W *)T->witness)->greet(subject, args)
// 静态方法（新增）: ((const struct W *)T->witness)->make(args)
// 静态字段读（新增）: ((const struct W *)T->witness)->get_zero()
// 静态字段写（新增）: ((const struct W *)T->witness)->set_count(value)
```

**不需要新增运行时结构、不需要改变泛型函数调用约定。**

### 3. UserSpecMember 新增 `is_static` bool

`UserSpecMember.kind` 保持 `USM_KIND_FIELD` / `USM_KIND_METHOD` 不变。新增 `bool is_static` 字段区分静态/实例。

**理由：**

- 不新增枚举值 = 现有 `sm->kind` 分支结构不变
- witness struct 生成、thunk 生成中加 `if (sm->is_static)` 分支处理即可
- 改动最小

### 4. 复用现有 witness source kind（不新增）

`FengSpecWitnessSourceKind` **保持不变**，仍为：

```c
FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_FIELD,
FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD,
FENG_SPEC_WITNESS_SOURCE_FIT_METHOD
```

**slot 签名与 source kind 解耦**：

- `source_kind` 仅记录"满足来源"（type 自身 vs fit）
- slot 签名（是否有 `_subject`）由 `spec_member->is_static` 决定
- thunk 转发目标查找时，根据 `spec_member->is_static` 选择 type 实例/静态成员表

**理由：**

- 复用现有架构（用户决策原则）
- 静态字段只能由 type 自身满足（fit 不能声明 static let/var），source kind 不会出现"静态字段 + FIT"组合，无需新增枚举
- 静态方法 source kind 复用 `TYPE_OWN_METHOD`/`FIT_METHOD`，thunk 生成时根据 `is_static` 切换实例/静态方法查找路径

### 5. 完全复用现有静态成员查找函数（不新增）

`analyzer.c` 已存在的静态成员查找函数全部可直接复用，**本次变更不新增任何查找函数**：

| 现有函数 | 位置 | 用途 |
|---------|------|------|
| `find_type_static_field_member` | 7853 | type 自身静态字段（按 name，过滤 is_static） |
| `find_type_static_method_member` | 10297 | type 自身静态方法（按 name） |
| `find_fit_static_method_member_for_type` | 10503 | 可见 fit 中的静态方法（target 视角） |
| `find_fit_static_method_member_for_owner_type` | 10532 | 同上（owner 视角） |
| `resolve_accessible_static_method_overload` | 10953 | 可访问静态方法重载解析 |
| `type_find_matching_method_in_spec_ref` | 20030 | 泛型 spec 类型参数绑定后的方法匹配 |

spec 闭包中静态成员的查找（泛型 `T.make()` 路径）通过**扩展 `find_spec_object_member` 的入参**实现，不新增对偶函数（见 Phase 3.2）。

---

## 语法

### 正确语法

```feng
spec Factory<T> {
  static func make(): T;
}

spec Configurable {
  static let defaults: string;
  static var current: int;
}

spec Registry<T> {
  static func all(): T[];
  static func byId(id: int): T;
  static let empty: T;
}

spec GenericFactory<T> {
  static func create<U>(): T;
}

// 静态方法名与 spec 名相同，视为普通静态方法（无构造器歧义）
spec Resource {
  static func Resource(): Resource;
}
```

> **泛型模式说明**：当 spec 静态方法的返回值或参数需要使用"实现类型自身"时，使用泛型参数 `T` 描述，由满足方在 `type Widget: Factory<Widget>` 中绑定。这与 spec 实例方法的模式一致，利用现有泛型能力实现精确匹配，不引入协变或 Self 类型。

### 错误语法

```feng
spec Bad {
  static let zero: int = 0;          // 错误 SE0603：spec 字段不能有初始化器
  static func make(): Bad { ... }    // 错误 SE0605：spec 方法签名不能有函数体
  static func ~Bad();                 // 错误 AE0620：spec 一律不允许 ~ 前缀（语义层统一拒绝）
  func ~Bad();                        // 错误 AE0620：spec 一律不允许 ~ 前缀（已有，自动覆盖实例方法）
}
```

### 满足示例

```feng
spec Factory<T> {
  static func make(): T;
  static let tag: string;
}

type Widget: Factory<Widget> {
  let name: string;

  open static func make(): Widget {
    return Widget { name: "default" };
  }
  open static let tag: string = "widget";
}

// 或通过 fit 满足静态方法（fit 不能提供静态字段）
type Gadget {
  let id: int;
  open static let tag: string = "gadget";
}

fit Gadget: Factory<Gadget> {
  open static func make(): Gadget {
    return Gadget { id: 0 };
  }
}
```

### 泛型约束中使用

```feng
spec Factory<T> {
  static func make(): T;
}

func create<T: Factory<T>>(): T {
  return T.make();
}

func createAll<T: Factory<T>>(n: int): T[] {
  let result: T[] = [];
  for i in 0..n {
    result = result + [T.make()];
  }
  return result;
}

// 调用
let w = create<Widget>();       // Widget.make()
let gs = createAll<Gadget>(3);  // 3 个 Gadget.make()
```

---

## 语义

### spec 静态成员声明规则

- `spec` 中允许声明 `static let`、`static var`、`static func`
- spec 静态成员不允许有初始值或函数体（spec 是契约声明，不提供实现）
- spec 静态字段声明必须以 `;` 结束
- spec 静态方法签名必须以 `;` 结束
- spec 静态方法参数不能使用 `let` / `var` 修饰符
- spec 静态方法必须显式声明返回类型
- **spec 一律不允许 `~` 前缀**（不论静态或实例）：终结器只允许用于 type，已在语义层（AE0620）统一拒绝，解析器无需新增特判
- **spec 静态方法名可以与 spec 名相同**（视为普通静态方法，无构造器/终结器概念）
- spec 静态方法支持泛型，规则与 spec 实例方法一致
- spec 静态方法支持重载，规则与 type 中静态方法一致
- spec 静态成员不允许显式 `open` / `seal` 可见性修饰（与 spec 所有成员一致）

### 满足性规则

- 静态字段匹配：名称 + 绑定方式（`let` / `var`） + 类型完全一致
- 静态方法匹配：名称 + 参数个数 + 参数类型 + 参数顺序 + 返回值类型完全一致
- 泛型 spec 的类型参数在满足检查时按 `type Widget: Factory<Widget>` 中的实参替换后精确匹配（复用现有 `type_find_matching_method_in_spec_ref` 机制）
- 满足来源：
  - 静态字段：只能由 `type` 自身提供（`fit` 不能声明 `static let` / `static var`）
  - 静态方法：可由 `type` 自身或可见 `fit` 提供
- spec 父 spec 中的静态成员通过现有 `spec_collect_closure` 闭包自然继承
- 同名同签名的静态方法多源歧义按现有规则诊断

### 访问规则

- 通过具体类型名访问：`Widget.make()`、`Widget.tag` — 直接调用/访问，零开销
- 通过泛型类型参数访问：`T.make()`、`T.tag`（`T: Spec`） — 通过 witness 表分派
- 不支持通过实例访问静态成员（与 type 规则一致）
- 不支持通过 spec 值访问静态成员（spec 值是实例级概念）

---

## 各层变更明细

### Phase 1 — 文档（先于所有代码变更）

- `docs/feng-spec.md`：
  - **移除 §3 "错语法三"**（当前禁止 spec 静态成员的示例）
  - 在 §4（成员声明）新增 spec 静态成员语法、语义节
  - **移除 spec 不允许 constructor 的相关条款**（spec 不再有 constructor 概念，方法名 == spec 名视为普通方法）
  - 保留 spec 不允许 finalizer 的条款（`~` 前缀仍禁止）
- `docs/feng-fit.md`：补充 fit 静态方法可满足 spec 静态方法约束（fit 仍不得声明 static let/var）
- `docs/feng-error-codes-se.md`：
  - **SE0602 标记为失效**（原"spec 成员不能声明为 static"已不适用；错误码空闲，未来根据情况决定是否用作他处理）
  - **AE0620 描述更新**：仅保留 finalizer 部分（spec 不再有 constructor 概念）
- `dev/feng-static-member-dev.md`：移除「spec 中暂不支持声明静态成员」说明

### Phase 2 — 解析器

**`src/parser/parser.c` — `parse_spec_member`（第 1573 行）**

当前第 1585–1590 行完全拒绝 `static`。改为：

```c
bool is_static = false;

if parser_check(STATIC):
    parser_advance()  // consume 'static'
    is_static = true

if is_static && (check(LET) || check(VAR)):
    // 静态字段路径：复用现有 spec let/var 解析
    // - 拒绝 '=' 初始值（现有 SE0603 自动覆盖）
    // - 设置 member->is_static = true
elif is_static && check(FUNC):
    // 静态方法路径：
    // - 不进行 "方法名 == spec 名" 构造器判定（视为普通 METHOD）
    // - 复用 parse_callable_signature（自动应用 SE0604/SE0605/SE0606）
    // - 设置 member->is_static = true
    // 注意：~ 前缀不特判，由语义层 AE0620 统一拒绝
elif !is_static:
    // 走现有 let/var/func 路径（不变）
else:
    报错 SE0003
```

**关键改动点：**

1. 第 1585 行 `parser_check(FENG_TOKEN_KW_STATIC)`：从拒绝改为接受并消费，设置 `is_static = true`
2. 静态字段解析：复用现有 spec `let`/`var` 字段解析（第 1592–1627 行），在 `new_type_member` 后设置 `member->is_static = true`
3. 静态方法解析：复用现有 spec `func` 方法签名解析（第 1629–1694 行），**不需要任何特判**：
   - 因为方案 B 顺带移除了 spec 的 constructor 判定（见下条 4），spec 方法名 == spec 名已经自动视为 METHOD（静态和实例一致）
4. 设置 `member->is_static = true`

**关联变更（方案 B：spec 不再有 constructor 概念）：**

5. 第 1671–1675 行：移除 `else if (slice_equals(name, spec_name)) { member_kind = FENG_TYPE_MEMBER_CONSTRUCTOR; }` 判定
   - 保留 `if (is_finalizer) { member_kind = FINALIZER; }`（spec `~` 前缀仍解析为 FINALIZER，由 AE0620 拒绝）

**不需要特判的场景（按用户决策自动覆盖）：**

- spec 方法 `~` 前缀（静态和实例）：解析器仍解析为 FINALIZER，语义层 AE0620 统一拒绝（已有逻辑与测试）
- spec 静态方法名 == spec 名：constructor 判定已移除，自动视为 METHOD
- spec 静态方法的其他语法约束（初始值、函数体、参数修饰符、返回类型）：复用现有 spec 实例方法的 SE0603/SE0604/SE0605/SE0606 检查

**测试 `test/parser/test_parser.c`：**

- 修改第 352–358 行 SE0602 测试：原"static 禁止"用例移除或改标注（SE0602 失效，空闲）
- 新增 spec `static let`/`static var`/`static func` 解析成功测试
- 新增 spec 静态方法名 == spec 名的合法测试（解析为 METHOD）
- 新增 spec 静态字段初始值禁止测试（SE0603）
- 新增 spec 静态方法函数体禁止测试（SE0605）
- spec 静态方法 `~` 前缀的拒绝由语义层测试覆盖（已有 AE0620 测试）

### Phase 3 — 语义分析

**核心原则：完全复用现有静态成员查找函数，不新增任何查找函数。**

现有可用函数（位置见 §设计决策 5）：

| 现有函数 | 用途 |
|---------|------|
| `find_type_static_field_member` | type 静态字段（按 name） |
| `find_type_static_method_member` | type 静态方法（按 name） |
| `find_fit_static_method_member_for_type` | fit 静态方法（target 视角） |
| `find_fit_static_method_member_for_owner_type` | fit 静态方法（owner 视角） |
| `resolve_accessible_static_method_overload` | 静态方法重载解析 |
| `type_find_matching_method_in_spec_ref` | 泛型 spec 类型参数绑定后匹配 |

**关联变更（方案 B：spec 不再有 constructor 概念）：**

- `analyzer.c:19875-86`：移除 spec CONSTRUCTOR 检查分支（`member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR`）
- `analyzer.c:19887-98`：保留 spec FINALIZER 检查（`~` 前缀仍禁止）
- AE0620 错误消息：仅保留 "cannot declare a finalizer"

#### 3.1 修改 `find_spec_object_member`（第 7920 行）

扩展入参为 `bool include_static`，控制是否查找静态成员：

```c
static const FengTypeMember *find_spec_object_member(
    const ResolveContext *context,
    const FengDecl *spec_decl,
    FengSlice name,
    bool include_static);  // 新增参数
```

**调用点适配：**

- 现有所有调用点（实例成员查找）传 `false`，自动排除静态成员
- 泛型 `T.static_member` 解析路径（§3.3）传 `true`

**实现：** 在循环内增加 `if (member->is_static && !include_static) continue;`。

> **必要性**：type 允许同名静态/实例方法（`analyzer.c:15987` 中 `mi->is_static != mj->is_static) continue;` 证实），所以 spec 成员查找必须区分静态/实例，否则实例查找会误命中同名的静态成员。这是必须新增的参数，不是可选优化。

#### 3.2 修改 `compute_spec_witness_if_absent`（第 20621 行）

在现有 `sm->kind` 分支前增加 `sm->is_static` 分支，**调用现有静态查找函数**：

```c
if (sm->is_static) {
    if (sm->kind == FENG_TYPE_MEMBER_FIELD) {
        // 静态字段：只能由 type 自身提供（fit 不能声明 static let/var）
        const FengTypeMember *t_field = find_type_static_field_member(
            type_decl, sm->name);  // 复用现有
        if (t_field == NULL) {
            report_unsatisfied_static_field(...);
        } else {
            // 复用 TYPE_OWN_FIELD source kind
            append witness member (TYPE_OWN_FIELD, t_field);
        }
    } else if (sm->kind == FENG_TYPE_MEMBER_METHOD) {
        // 静态方法：type 自身或可见 fit 提供
        // 1. find_type_static_method_member（type 自身）
        // 2. find_fit_static_method_member_for_type（可见 fit）
        // 3. 多源歧义按现有规则处理
        // source_kind 复用 TYPE_OWN_METHOD 或 FIT_METHOD
    }
    continue;
}
// ... 现有实例成员逻辑不变
```

> mutability 匹配（let/var）由调用方在 `t_field` 返回后判定（与实例字段路径一致），不改变 `find_type_static_field_member` 签名。

#### 3.3 泛型类型参数静态成员访问

`resolve_type_target_expr`（第 12839 行）：当 `FENG_EXPR_IDENTIFIER` 未命中 `visible_types` 时，增加对当前泛型函数类型参数的检查。

**`ResolvedTypeTarget`（analyzer.c:332，非 semantic.h，是 analyzer 内部 typedef）新增字段：**

```c
typedef struct ResolvedTypeTarget {
    const FengDecl *type_decl;
    const FengSemanticModule *provider_module;
    const FengTypeRef *type_ref;
    bool is_builtin_type_name;
    FengSlice builtin_name;
    // 新增：
    bool is_generic_type_param;
    size_t generic_param_index;
    const FengDecl *constraint_spec_decl;
} ResolvedTypeTarget;
```

**静态成员调用解析路径**（`validate_function_call_expr`、`infer_member_expr_type`）：

当 `resolve_type_target_expr` 返回 `is_generic_type_param == true` 时：

- 从 `constraint_spec_decl` 闭包中查找静态成员（`find_spec_object_member(..., include_static=true)`）
- 记录 `SpecCoercionSite`，标记为泛型参数静态成员访问
- 类型推断返回静态成员的类型

#### 3.4 重载检查

spec 内静态方法与实例方法形成独立重载集（与 type 规则一致），复用现有 `resolve_accessible_static_method_overload` 处理静态方法重载解析。

**测试 `test/semantic/test_semantic.c`：**

- type 提供静态成员满足 spec
- fit 提供静态方法满足 spec
- 泛型约束中 `T.make()` / `T.field` 类型推断和合法性
- spec 父 spec 静态成员约束传递（通过 spec_collect_closure 自然继承）
- 不满足场景（缺失、签名不匹配、泛型实参不一致）
- 通过实例访问 spec 静态成员 → 报错
- spec 静态方法名 == spec 名 视为普通方法（合法）
- spec 实例方法名 == spec 名 视为普通方法（合法，方案 B 一并放宽）
- spec 静态方法 `~` 前缀 → AE0620（已有测试，可补静态用例）

### Phase 4 — 符号表

**`src/symbol/export.c`（第 2732 行）**

spec 成员导出循环遍历 `as.object.members`，静态成员自然被导出。需验证：

- `build_member_decl` 正确传递 `is_static` 标志
- 消费侧 `imported_module.c` 正确还原 `is_static`（第 853 行已有 `member->is_static = member_decl->is_static`）

### Phase 5 — 代码生成

**`src/codegen/codegen.c`**

#### 5.1 `UserSpecMember` 结构（第 661 行）新增 `is_static`

```c
typedef struct UserSpecMember {
    char   *feng_name;
    char   *c_field_name;
    enum { USM_KIND_FIELD = 0, USM_KIND_METHOD } kind;
    CGType  *type;
    bool     is_var;
    bool     is_static;      // 新增
    CGType **param_types;
    char   **param_names;
    size_t   param_count;
    const FengTypeMember *member;
} UserSpecMember;
```

#### 5.2 spec 成员注册（`cg_ensure_user_spec_members_registered`，第 8448 行）

设置 `sm->is_static`：

```c
sm->is_static = (member != NULL && member->is_static);
```

`cg_user_spec_clone_inherited_member`（第 7437 行）中同步复制：

```c
dst->is_static = src->is_static;
```

> 此为**必须修复**：当前 `cg_user_spec_clone_inherited_member` 未复制 `is_static`，会导致父 spec 静态成员被 clone 到子 spec 后丢失静态属性，slot 签名生成错误。

#### 5.3 witness struct 生成（`cg_emit_witness_struct_body`，第 9171 行）

为静态成员生成无 `_subject` 的 slot：

```c
for (size_t i = 0; i < s->member_count; i++) {
    const UserSpecMember *sm = &s->members[i];
    if (sm->kind == USM_KIND_METHOD) {
        if (sm->is_static) {
            // 静态方法：无 _subject
            emit: RetType (*name)(params...);
        } else {
            // 现有实例方法：有 _subject
            emit: RetType (*name)(void *_subject, params...);
        }
    } else if (sm->kind == USM_KIND_FIELD) {
        if (sm->is_static) {
            // 静态字段 get：无 _subject
            emit: T (*get_name)(void);
            if (sm->is_var) {
                // 静态字段 set：无 _subject
                emit: void (*set_name)(T value);
            }
        } else {
            // 现有实例字段 get/set：有 _subject
        }
    }
}
```

#### 5.4 witness thunk 生成（`cg_ensure_witness_instance_for_type`，第 26548 行）

为静态成员生成无 `_subject` cast 的 thunk：

```c
if (sm->is_static) {
    if (sm->kind == USM_KIND_METHOD) {
        // 静态方法 thunk：直接转发，无 subject cast
        // binding 来源：
        //   - source_kind == TYPE_OWN_METHOD：从 type 的静态方法表查找（复用现有 type 静态方法命名）
        //   - source_kind == FIT_METHOD：从 fit 静态方法查找
        emit:
            static RetType Thunk__T__as__S__method(params...) {
                return Feng__mod__T__static__method(params...);
            }
    } else if (sm->kind == USM_KIND_FIELD) {
        // 静态字段 getter thunk
        emit:
            static T Thunk__T__as__S__get_field(void) {
                ensure_init();
                return Feng__mod__T__static__field;
            }
        // 静态字段 setter thunk（var）
        emit:
            static void Thunk__T__as__S__set_field(T value) {
                ensure_init();
                Feng__mod__T__static__field = value;
            }
    }
    continue;
}
// ... 现有实例成员 thunk 逻辑不变
```

**binding 查找（复用现有 `cg_user_type_static_method_by_member` 等命名 helper）：**

```c
if (sm->is_static && sm->kind == USM_KIND_METHOD) {
    if (binding.source_kind == FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD) {
        // 复用现有 type 静态方法符号查找
        binding.method = cg_user_type_static_method_by_member(t, wm->impl_member);
    } else if (binding.source_kind == FENG_SPEC_WITNESS_SOURCE_FIT_METHOD) {
        // 复用现有 fit 静态方法符号查找
    }
}
```

> 字段符号命名规则与 type 静态字段保持一致（复用现有 `cg_user_type_*` helper），不引入新命名约定。

#### 5.5 默认 witness 生成

为静态成员生成返回默认零值的 thunk：

```c
// 静态方法默认 thunk
static RetType DefaultThunk__S__method(params...) {
    return default_zero_value;  // 或 void
}

// 静态字段默认 getter thunk
static T DefaultThunk__S__get_field(void) {
    return default_zero_value;
}
```

涉及位置：约第 9686–9711 行的默认 witness 生成区域。

#### 5.6 泛型约束中 `T.make()` / `T.field` 发码

复用现有泛型实例成员分派模式（`desc_name->witness`），去掉 `_subject`：

**静态方法调用**（参考现有实例方法分派 `codegen.c:14795`）：

```c
// 现有实例方法:
//   ((const struct W *)T->witness)->method(subject, args)
// 新增静态方法:
//   ((const struct W *)T->witness)->static_method(args)
buf_append_fmt(&b,
    "((const struct %s *)%s->witness)->%s(%s)",
    us->c_witness_struct_name,
    desc_name,            // T_desc
    sm->c_field_name,     // witness slot 名
    args_buf.data);       // 无 subject，仅 args
```

**静态字段读取**（参考现有实例字段分派 `codegen.c:15646`）：

```c
buf_append_fmt(&b,
    "((const struct %s *)%s->witness)->get_%s()",
    us->c_witness_struct_name,
    desc_name,
    sm->c_field_name);
```

**静态字段写入**（参考现有实例字段写入 `codegen.c:20020`）：

```c
buf_append_fmt(cg->cur_body,
    "((const struct %s *)%s->witness)->set_%s(%s);\n",
    us->c_witness_struct_name,
    desc_name,
    sm->c_field_name,
    value_expr);
```

**发码入口判断条件：**

```c
if (resolved.is_generic_type_param) {
    const UserSpec *us = constraint_spec;
    const char *desc_name = desc_names[generic_param_index];
    // find_spec_object_member 已扩展 include_static 参数（见 Phase 3.1）
    const FengTypeMember *spec_member = find_spec_object_member(
        context, constraint_spec_decl, member_name, /*include_static=*/true);
    // 由语义侧解析时已转换为对应的 UserSpecMember，直接走 sm->is_static 分支
    if (spec_member != NULL) {
        const UserSpecMember *sm = cg_user_spec_member(us, spec_member);
        if (sm->is_static) {
            // 生成 witness->static_slot 访问（无 subject）
        }
    }
}
```

#### 5.7 spec slot witness（`cg_ensure_spec_slot_witness`，第 25277 行）

spec-to-spec 适配（子 spec → 父 spec）中，静态成员 slot 也需要适配转发：

```c
if (dst_member->is_static) {
    if (dst_member->kind == USM_KIND_METHOD) {
        // 静态方法 slot 适配：直接转发到 src 的同名 slot
        emit: static RetType slot_thunk(params...) {
            return src_witness->static_method(params...);
        }
    } else if (dst_member->kind == USM_KIND_FIELD) {
        // 静态字段 slot 适配
        emit: static T slot_getter(void) { return src_witness->get_field(); }
        emit: static void slot_setter(T v) { src_witness->set_field(v); }
    }
    continue;
}
```

### Phase 6 — 测试

#### 解析器测试

成功场景：

```feng
spec F { static func make(): F; }
spec C { static let x: int; static var y: int; }
spec O { static func all(): O[]; static let empty: O; }
spec R { static func R(): R; }   // 静态方法名 == spec 名，合法（spec 无 constructor 概念）
spec R { func R(): R; }           // 实例方法名 == spec 名，合法（方案 B 一并放宽）
```

失败场景：

```feng
spec B { static let x: int = 0; }   // SE0603 初始值禁止
spec B { static func B() {} }        // SE0605 函数体禁止
```

> spec 方法 `~` 前缀的拒绝由语义层 AE0620 覆盖（已有测试 `test_object_form_spec_rejects_finalizer_member`，可补充静态用例），解析层无需新增 `~` 拒绝测试。

#### 语义测试

- type 静态成员满足 spec（含泛型 spec 如 `Factory<T>`）
- fit 静态方法满足泛型 spec
- 泛型约束 `T.make()` 类型推断（`T: Factory<T>`）
- 泛型约束 `T.field` 读写类型推断
- 不满足场景（缺失、签名不匹配、泛型实参不一致）
- spec 父 spec 静态成员约束传递
- 实例访问静态成员 → 报错
- spec 方法名 == spec 名（静态和实例）视为普通方法（合法）
- spec 方法 `~` 前缀 → AE0620（扩展现有 `test_object_form_spec_rejects_finalizer_member` 增加静态用例）
- **移除** `test_object_form_spec_rejects_constructor_member`（test_semantic.c:9155）—— spec 不再有 constructor 概念

#### 代码生成测试

- witness struct 含静态成员 slot（验证无 `_subject`）
- thunk 生成（验证无 subject cast）
- 默认 witness 静态成员 slot
- 泛型 `T.make()` 发码（验证 witness 分派）
- 泛型 `T.field` 发码（验证 witness getter/setter）
- spec-to-spec slot witness 静态成员适配转发

#### Smoke 测试

```feng
spec Factory<T> {
    static func make(): T;
}

type Widget: Factory<Widget> {
    let name: string;
    open static func make(): Widget {
        return Widget { name: "widget" };
    }
}

func create<T: Factory<T>>(): string {
    let item = T.make();
    return item.name;
}

func main(args: string[]) {
    let direct = Widget.make();
    print(direct.name);
    print(create<Widget>());
}
```

#### 全量回归

所有现有测试通过，无破坏性变更。

---

## 关键文件

| 文件 | 变更内容 |
|------|---------|
| `docs/feng-spec.md` | 移除 §3 "错语法三"；新增 spec 静态成员合法语法节；移除 spec constructor 相关条款 |
| `docs/feng-fit.md` | 补充 fit 静态方法满足 spec 说明 |
| `docs/feng-error-codes-se.md` | SE0602 标记为失效（空闲）；AE0620 描述更新（仅保留 finalizer） |
| `dev/feng-static-member-dev.md` | 移除「暂不支持」说明 |
| `src/parser/parser.c` | `parse_spec_member`：接受 static；移除 spec constructor 判定（方法名 == spec 名视为 METHOD） |
| `test/parser/test_parser.c` | 修改 SE0602 测试；新增静态成员测试 |
| `src/semantic/analyzer.c` | 完全复用现有静态查找函数；`find_spec_object_member` 扩展 `include_static` 参数；扩展 `ResolvedTypeTarget`；修改 `compute_spec_witness_if_absent`、`validate_function_call_expr`、`infer_member_expr_type`；移除 spec CONSTRUCTOR 检查分支（保留 FINALIZER） |
| `test/semantic/test_semantic.c` | 新增静态成员测试；移除 `test_object_form_spec_rejects_constructor_member`；扩展 AE0620 finalizer 测试 |
| `src/symbol/export.c` | 验证 is_static 传递 |
| `src/symbol/imported_module.c` | 验证 is_static 还原 |
| `src/codegen/codegen.c` | UserSpecMember 新增 is_static；witness struct/thunk/默认 witness/泛型分派/slot witness；修复 `cg_user_spec_clone_inherited_member` 复制 is_static |
| `test/codegen/test_codegen.c` | 新增测试 |

---

## 运行时

- `FengGenericParamDescriptor` 已有 `witness` 字段，无需修改运行时结构
- witness 表布局对 spec 而言按编译单元生成；spec 增加成员属于编译期 ABI 演进（spec 仍在演进期，不构成稳定的跨版本 ABI 承诺）。同一编译单元内 witness 表布局由 spec 声明顺序静态确定，不存在运行时布局漂移。
- 静态方法 thunk 无 `_subject` cast，转发到 type 的静态方法（直接 C 函数调用），开销为一次函数指针间接调用
- 静态字段 getter/setter thunk 无 `_subject`，转发到 type 的静态字段（ensure_init + 存储槽访问）

---

## 实施步骤（每步可独立交付，独立通过全量回归测试）

> 拆分原则：每个 Step 是一次"垂直切片"，覆盖 parser → semantic → symbol → codegen → 测试。每完成一个 Step，编译器即可交付一个完整且自洽的能力，并保持现有测试全部通过。

### Step 1 — 文档先行（无代码改动）

- [ ] `docs/feng-spec.md`：
  - 移除 §3 "错语法三"（spec 静态成员禁止示例）
  - 在 §4 新增 spec 静态成员（`static let` / `static var` / `static func`）合法语法、语义节
  - 移除 spec constructor 相关条款（spec 不再有 constructor 概念）
  - 明确"spec 方法名可以与 spec 名相同（视为普通方法）"
- [ ] `docs/feng-fit.md`：补充 fit 静态方法可满足 spec 静态方法约束（fit 仍不得声明 static let/var）
- [ ] `docs/feng-error-codes-se.md`：
  - SE0602 标记为失效（空闲，未来根据情况决定是否用作他处理）
  - AE0620 描述更新：仅保留 "cannot declare a finalizer"（移除 constructor 部分）
- [ ] `dev/feng-static-member-dev.md`：移除「spec 中暂不支持声明静态成员」说明
- **交付**：文档定稿，作为后续代码改动的契约
- **回归**：纯文档变更，无代码影响 → 全量回归通过

### Step 2 — 方案 B：移除 spec constructor 概念（关联变更）

- [ ] `src/parser/parser.c`（`parse_spec_member`，第 1671–1675 行）：移除 `else if (slice_equals(name, spec_name)) { member_kind = FENG_TYPE_MEMBER_CONSTRUCTOR; }` 判定
  - 保留 `if (is_finalizer) { member_kind = FINALIZER; }`（`~` 前缀仍解析为 FINALIZER）
- [ ] `src/semantic/analyzer.c`（约第 19875–86 行）：移除 spec CONSTRUCTOR 检查分支（`member->kind == FENG_TYPE_MEMBER_CONSTRUCTOR`），保留 FINALIZER 检查
- [ ] `test/semantic/test_semantic.c`：
  - **移除** `test_object_form_spec_rejects_constructor_member`（第 9155 行）及其在 `main` 中的注册
  - **新增** `test_object_form_spec_allows_method_same_name_as_spec`：验证 spec 实例方法名 == spec 名合法
  - **扩展** `test_object_form_spec_rejects_finalizer_member`：增加 spec 静态方法 `~` 前缀用例（验证 AE0620 自动覆盖静态路径）
- **交付**：spec 方法（静态和实例）名 == spec 名视为普通方法；spec `~` 前缀仍由 AE0620 拒绝（语义一致）
- **回归**：全量回归通过。**唯一行为变化**：spec 实例方法名 == spec 名从被拒绝变为合法；不涉及 spec 静态成员声明（仍由 parser 拒绝）

### Step 3 — spec 静态成员声明 + type 静态成员满足 + 直接调用

- [ ] `src/parser/parser.c`（`parse_spec_member`）：
  - 第 1585 行：从拒绝 `static` 改为接受并消费，设置 `is_static = true`
  - 静态字段路径（let/var）：复用现有 spec 字段解析，设置 `member->is_static = true`
  - 静态方法路径（func）：复用现有 spec 方法签名解析，设置 `member->is_static = true`
- [ ] `src/semantic/analyzer.c`：
  - `find_spec_object_member`（第 7920 行）：扩展入参 `bool include_static`，循环内增加 `if (member->is_static && !include_static) continue;`
  - 全部现有调用点适配：传 `false`（实例查找不命中静态）
  - `compute_spec_witness_if_absent`（第 20621 行）：在 `sm->kind` 分支前增加 `if (sm->is_static)` 分支，调用现有 `find_type_static_field_member` / `find_type_static_method_member` / `find_fit_static_method_member_for_*` 函数；source kind 复用 `TYPE_OWN_FIELD` / `TYPE_OWN_METHOD` / `FIT_METHOD`
- [ ] `src/symbol/export.c` + `src/symbol/imported_module.c`：验证 `is_static` 在 spec 成员导出/还原时正确传递（预期无代码改动）
- [ ] `src/codegen/codegen.c`：
  - `UserSpecMember`（第 661 行）：新增 `bool is_static` 字段
  - `cg_ensure_user_spec_members_registered`（第 8448 行）：设置 `sm->is_static`
  - `cg_user_spec_clone_inherited_member`（第 7437 行）：**修复** 添加 `dst->is_static = src->is_static`
  - `cg_emit_witness_struct_body`（第 9171 行）：静态成员 slot 签名无 `_subject`
  - `cg_ensure_witness_instance_for_type`（第 26548 行）：静态成员 thunk 转发无 `_subject` cast
  - 默认 witness 生成（约第 9686–9711 行）：静态成员默认 thunk
- [ ] 测试：
  - `test/parser/test_parser.c`：移除 SE0602 "static 禁止"测试；新增 spec `static let` / `static var` / `static func` 解析测试；新增 spec 静态字段初始值禁止（SE0603）、静态方法函数体禁止（SE0605）
  - `test/semantic/test_semantic.c`：type 静态成员满足 spec（含泛型 spec `Factory<T>`）；fit 静态方法满足 spec；不满足场景（缺失、签名不匹配）；通过实例访问 spec 静态成员 → 报错
  - `test/codegen/test_codegen.c`：witness struct 含静态成员 slot（无 `_subject`）；thunk 生成（无 subject cast）；默认 witness 静态成员 slot
- **交付**：spec 声明静态成员；type/fit 提供静态成员实现；直接调用 `Widget.make()` / `Widget.tag`（零开销）
- **回归**：全量回归通过。**新增能力**：spec 可声明静态成员，非破坏性

### Step 4 — 泛型约束 `T.make()` / `T.field` 通过 witness 分派

- [ ] `src/semantic/analyzer.c`：
  - `ResolvedTypeTarget`（第 332 行）：新增 `bool is_generic_type_param` / `size_t generic_param_index` / `const FengDecl *constraint_spec_decl`
  - `resolve_type_target_expr`（第 12839 行）：当 identifier 未命中 visible_types 时，检查当前泛型函数类型参数；命中则填充上述字段
  - `validate_function_call_expr` / `infer_member_expr_type`：当 `is_generic_type_param == true` 时，从 `constraint_spec_decl` 闭包通过 `find_spec_object_member(..., include_static=true)` 查找静态成员；记录 SpecCoercionSite；返回静态成员类型
- [ ] `src/codegen/codegen.c`：
  - 泛型 `T.make()` 发码：`((const struct W *)T->witness)->static_method(args)`（无 subject）
  - 泛型 `T.field` 读：`((const struct W *)T->witness)->get_field()`（无 subject）
  - 泛型 `T.field` 写：`((const struct W *)T->witness)->set_field(value)`（无 subject）
  - `cg_ensure_spec_slot_witness`（第 25277 行）：spec-to-spec 适配中处理静态成员 slot（无 subject 转发）
- [ ] 测试：
  - `test/semantic/test_semantic.c`：泛型约束 `T.make()` 类型推断（`T: Factory<T>`）；泛型 `T.field` 读写类型推断；spec 父 spec 静态成员约束传递；不满足场景
  - `test/codegen/test_codegen.c`：泛型 `T.make()` 发码（验证 witness 分派）；泛型 `T.field` 发码（验证 witness getter/setter）；spec-to-spec slot witness 静态成员适配转发
- [ ] Smoke 测试：完整 `spec Factory<T>` / `type Widget: Factory<Widget>` / `func create<T: Factory<T>>()` 调用链
- **交付**：泛型约束中 `T.make()` / `T.field` 通过 witness 表间接分派；spec-to-spec 闭包传递静态成员
- **回归**：全量回归通过。**新增能力**：泛型分派，非破坏性

### 验收 Checklist（每 Step 完成后）

- [ ] 该 Step 列出的所有文件改动完成
- [ ] 该 Step 新增/修改的测试全部通过
- [ ] **全量回归测试通过**（`ctest` / 项目测试入口无破坏）
- [ ] commit message 给出建议（英文，由开发者自行提交）
- [ ] 进入下一 Step 前等待开发者确认

### 跨 Step 依赖关系

```
Step 1（文档） → 不阻塞任何 Step，但应先于代码改动
Step 2（移除 constructor） → 不阻塞 Step 3，但建议先做（清理干净）
Step 3（spec 静态成员 + 直接调用） → 阻塞 Step 4（泛型分派依赖 Step 3 的 witness 表基础）
Step 4（泛型分派） → 完整能力
```

> **建议执行顺序**：Step 1 → Step 2 → Step 3 → Step 4。每完成一步，向开发者汇报并等待确认后再进入下一步。
