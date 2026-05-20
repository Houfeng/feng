# 顶层绑定 Lazy 初始化方案

## 问题

当前模块级 `let`/`var` 绑定按声明顺序在 startup 时 eager 初始化，存在三类问题：

1. **同模块多文件**：A 文件的 binding 依赖 B 文件的 binding，若 A 先处理，初始化时 B 还是 NULL/0
2. **跨模块引用**：模块 A 的 binding 依赖模块 B 的 binding，顺序同样不保证正确
3. **UserType 零值差距**：BSS 给指针 NULL，但 Feng 规范说"默认值为类型零值"（非 NULL 对象）

三类问题根因相同：初始化顺序依赖注册/文件/模块的处理顺序。

---

## 方案：Per-binding Lazy Getter

每个顶层绑定编译为一个 getter 函数，第一次访问时执行初始化表达式。

**核心规则：**
- 每个 binding 有独立的存储变量和 `_inited` flag
- Getter 内先计算 init expr，完成后再设 `_inited = true`
- 所有对顶层 binding 的读取引用改为调用 getter
- `var` 写入：直接写存储变量，同时标记 `_inited = true`（跳过 lazy init）
- **循环依赖**：等同普通函数循环调用，自然栈溢出，语言不做特殊处理

---

## 生成代码示例

### Feng 源码

```
// Module A
type Foo { x: i32 }
let a1: Foo = Foo { x: 42 }        // UserType，无依赖
let a2: i32 = b1.x + 10            // 依赖 B.b1

// Module B
let b1: Foo = Foo { x: a1.x * 2 }  // 依赖 A.a1
var b2: Foo = Foo { x: 0 }          // var，UserType
```

### 当前生成（Eager，有 Bug）

```c
static struct Feng__A__Foo *_feng_g__A__a1 = NULL;  // BSS
static int32_t              _feng_g__A__a2 = 0;
static struct Feng__B__Foo *_feng_g__B__b1 = NULL;  // BSS
static struct Feng__B__Foo *_feng_g__B__b2 = NULL;

// main() 按声明顺序初始化
_feng_g__A__a1 = Feng__A__Foo__new(42);
_feng_g__A__a2 = (int32_t)(_feng_g__B__b1->x + 10); // b1 是 NULL → crash
_feng_g__B__b1 = Feng__B__Foo__new(_feng_g__A__a1->x * 2);
_feng_g__B__b2 = Feng__B__Foo__new(0);
```

注：这一步生成的仍然是包内 `static` 存储槽。即使顶层 binding 是 `pu`，pre-lazy codegen 也没有为它单独生成跨包值导出表面；`pu` 只影响符号/可见性层，不改变这里的包内 eager 初始化形态。

### Lazy 后生成

```c
// 存储 + inited flag
static struct Feng__A__Foo *_feng_g__A__a1 = NULL;
static bool                 _feng_g__A__a1__inited = false;
static int32_t              _feng_g__A__a2 = 0;
static bool                 _feng_g__A__a2__inited = false;
static struct Feng__B__Foo *_feng_g__B__b1 = NULL;
static bool                 _feng_g__B__b1__inited = false;
static struct Feng__B__Foo *_feng_g__B__b2 = NULL;
static bool                 _feng_g__B__b2__inited = false;

// Getter：a1（UserType，无依赖）
static struct Feng__A__Foo *_feng_get_g__A__a1(void) {
    if (_feng_g__A__a1__inited) return _feng_g__A__a1;
    _feng_g__A__a1 = Feng__A__Foo__new(42);
    _feng_g__A__a1__inited = true;   // 计算完成后设
    return _feng_g__A__a1;
}

// Getter：b1（UserType，依赖 a1）
static struct Feng__B__Foo *_feng_get_g__B__b1(void) {
    if (_feng_g__B__b1__inited) return _feng_g__B__b1;
    struct Feng__A__Foo *_a1 = _feng_get_g__A__a1(); // 触发 a1 初始化
    _feng_g__B__b1 = Feng__B__Foo__new(_a1->x * 2);
    _feng_g__B__b1__inited = true;
    return _feng_g__B__b1;
}

// Getter：a2（标量，依赖 b1）
static int32_t _feng_get_g__A__a2(void) {
    if (_feng_g__A__a2__inited) return _feng_g__A__a2;
    struct Feng__B__Foo *_b1 = _feng_get_g__B__b1(); // 触发 b1 → a1
    _feng_g__A__a2 = (int32_t)(_b1->x + 10);
    _feng_g__A__a2__inited = true;
    return _feng_g__A__a2;
}

// Getter：b2（var，UserType）
static struct Feng__B__Foo *_feng_get_g__B__b2(void) {
    if (_feng_g__B__b2__inited) return _feng_g__B__b2;
    _feng_g__B__b2 = Feng__B__Foo__new(0);
    _feng_g__B__b2__inited = true;
    return _feng_g__B__b2;
}

// main()：不再有 binding 初始化循环
int main(void) {
    // ... ARC init ...
    feng_user_main();
}
```

### var 写入（b2 = newFoo）

```c
// 直接写 + 标记 inited，跳过 lazy init，不触发 init expr
_feng_g__B__b2__inited = true;
feng_assign((void **)&_feng_g__B__b2, newFoo);
```

### 无 init expr 的 UserType（`let a3: Foo`）

```c
static struct Feng__A__Foo *_feng_get_g__A__a3(void) {
    if (_feng_g__A__a3__inited) return _feng_g__A__a3;
    _feng_g__A__a3 = Feng__A__Foo__default_zero(); // 按需构造，非 startup 全量分配
    _feng_g__A__a3__inited = true;
    return _feng_g__A__a3;
}
```

---

## 解决的问题

| 场景 | 当前 | Lazy 后 |
|------|------|---------|
| 同模块多文件，跨文件 binding 引用 | ❌ 依赖文件处理顺序 | ✅ getter 按需触发 |
| 跨模块 binding 引用（无环） | ❌ 依赖模块注册顺序 | ✅ getter 按需触发 |
| 未使用的 binding（无访问） | ❌ startup 仍全量初始化 | ✅ 从不初始化 |
| UserType 零值差距（BSS NULL） | ❌ 读时可能得 NULL | ✅ 首次读时构造合法对象 |
| 循环 binding 依赖 | ❌ 不确定（错误值或 crash）| ✅ 栈溢出（等同普通函数循环调用，行为一致）|

---

## Codegen 改动范围

仅修改 `src/codegen/codegen.c`：

| 位置 | 改动 |
|------|------|
| `ModuleBinding` 结构体（L755） | 新增 `c_getter_name`、`c_inited_name` 两个字段 |
| `cg_register_module_binding`（L6789） | 计算并存储新字段 |
| `cg_pass_register_module_bindings`（L17438） | 额外 emit `_inited` flag 声明 |
| `cg_emit_identifier`（L8091） | 模块 binding 引用改为返回 getter 调用表达式 |
| 新增 `cg_emit_module_binding_getter` | 生成每个 binding 的 getter 函数体（复用现有 init expr 逻辑）|
| 新增 `cg_pass_emit_module_binding_getters` | 遍历所有 binding，调用上一项 |
| `cg_emit_assign`（var 写入路径） | LHS 为模块 var 时，插入 `_inited = true` |
| `cg_emit_main_wrapper`（L17968） | 删除 binding 初始化循环 |

---

## 跨包 Public Binding（基于 Lazy 版本补齐）

跨包访问 public 顶层 binding 时，不能直接导出 provider 包内的 storage 槽；否则会绕过 provider 侧 lazy 初始化与 `var` 写入时的统一存储更新逻辑。

lazy 版本补齐跨包值表面时，导出 ABI 约定如下：

- `pu let name: T`：导出 public getter，命名为 `feng__<module>__<name>__get__from__void`
- `pu var name: T`：在 public getter 之外，再导出 public setter，命名为 `feng__<module>__<name>__set__from__<T>`
- provider 包内仍保留 `static storage + _inited + internal getter`；public getter / setter 只是包边界 wrapper
- consumer 侧 imported binding codegen 仅生成 `extern` prototype，并把读取/写入 lower 为 getter / setter 调用

当前以 codegen 回归作为实现标记，验收面收敛为：

1. imported `pu let` 通过 `alias.member` 读取时，codegen 成功，并生成 getter prototype 与调用点
2. imported `pu var` 通过 `alias.member` 读取和写入时，codegen 成功，并生成 getter / setter prototype 与调用点

