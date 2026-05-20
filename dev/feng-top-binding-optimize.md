# 顶层绑定 Lazy 初始化方案

## 问题

当前模块级 `let`/`var` 绑定按声明顺序在 startup 时 eager 初始化，存在三类问题：

1. **同模块多文件**：A 文件的 binding 依赖 B 文件的 binding，若 A 先处理，初始化时 B 还是 NULL/0
2. **跨模块引用**：模块 A 的 binding 依赖模块 B 的 binding，顺序同样不保证正确
3. **UserType 零值差距**：BSS 给指针 NULL，但 Feng 规范说"默认值为类型零值"（非 NULL 对象）

三类问题根因相同：初始化顺序依赖注册/文件/模块的处理顺序。

---

## 方案：Per-binding Lazy Ensure-Init

每个顶层绑定编译为一个 storage slot 和一个 ensure_init 函数。任何访问都先触发 ensure_init，再对 storage slot 做读取、写入、取地址或其它基于 slot 的计算。

**核心规则：**

- 每个 binding 有独立的 storage slot、`ensure_init` 函数和 `_inited` flag
- `ensure_init` 内先计算 init expr，完成后再设 `_inited = true`
- 包内和跨包的任何访问，统一先触发 `ensure_init`，再对 storage slot 操作
- `var` 写入不再绕过 lazy init；先 `ensure_init`，再写 storage slot
- public 顶层 binding 在 lib 目标下同时公开 storage slot 和 `ensure_init`
- **循环依赖**：等同普通函数循环调用，自然栈溢出，语言不做特殊处理

---

## 生成代码示例

### Feng 源码

```text
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

// Ensure-init：a1（UserType，无依赖）
static void _feng_ensure_g__A__a1(void) {
    if (_feng_g__A__a1__inited) return;
    _feng_g__A__a1 = Feng__A__Foo__new(42);
    _feng_g__A__a1__inited = true;   // 计算完成后设
}

// Ensure-init：b1（UserType，依赖 a1）
static void _feng_ensure_g__B__b1(void) {
    if (_feng_g__B__b1__inited) return;
    _feng_ensure_g__A__a1(); // 触发 a1 初始化
    _feng_g__B__b1 = Feng__B__Foo__new(_feng_g__A__a1->x * 2);
    _feng_g__B__b1__inited = true;
}

// Ensure-init：a2（标量，依赖 b1）
static void _feng_ensure_g__A__a2(void) {
    if (_feng_g__A__a2__inited) return;
    _feng_ensure_g__B__b1(); // 触发 b1 → a1
    _feng_g__A__a2 = (int32_t)(_feng_g__B__b1->x + 10);
    _feng_g__A__a2__inited = true;
}

// Ensure-init：b2（var，UserType）
static void _feng_ensure_g__B__b2(void) {
    if (_feng_g__B__b2__inited) return;
    _feng_g__B__b2 = Feng__B__Foo__new(0);
    _feng_g__B__b2__inited = true;
}

// 读取：先 ensure_init，再读 slot
_feng_ensure_g__A__a2();
int32_t current = _feng_g__A__a2;

// 取地址：同样先 ensure_init，再对 slot 取址
_feng_ensure_g__A__a2();
int32_t *ptr = &(_feng_g__A__a2);

// main()：不再有 binding 初始化循环
int main(void) {
    // ... ARC init ...
    feng_user_main();
}
```

### var 写入（b2 = newFoo）

```c
// 先 ensure_init，再写 slot
_feng_ensure_g__B__b2();
feng_assign((void **)&_feng_g__B__b2, newFoo);
```

### 无 init expr 的 UserType（`let a3: Foo`）

```c
static void _feng_ensure_g__A__a3(void) {
    if (_feng_g__A__a3__inited) return;
    _feng_g__A__a3 = Feng__A__Foo__default_zero(); // 按需构造，非 startup 全量分配
    _feng_g__A__a3__inited = true;
}
```

---

## 解决的问题

| 场景 | 当前 | Lazy 后 |
| ------ | ------ | --------- |
| 同模块多文件，跨文件 binding 引用 | ❌ 依赖文件处理顺序 | ✅ ensure_init 按需触发 |
| 跨模块 binding 引用（无环） | ❌ 依赖模块注册顺序 | ✅ ensure_init 按需触发 |
| 未使用的 binding（无访问） | ❌ startup 仍全量初始化 | ✅ 从不初始化 |
| UserType 零值差距（BSS NULL） | ❌ 读时可能得 NULL | ✅ 首次读时构造合法对象 |
| 循环 binding 依赖 | ❌ 不确定（错误值或 crash） | ✅ 栈溢出（等同普通函数循环调用，行为一致） |

---

## TODO 拆解

1. 文档对齐
    - 保持本文档作为唯一设计依据
    - 实现过程若发现与本文不一致，先回到文档修正，再继续代码

2. Ensure-init 基础设施
    - 扩展 `ModuleBinding`：增加 `c_ensure_init_name`、`c_inited_name`
    - 在 `cg_register_module_binding` 中统一生成 ensure_init / flag 的 C 名称
    - 在 `cg_pass_register_module_bindings` 中补 `_inited` 静态存储声明

3. 访问路径切换到 ensure_init + slot
    - 新增 `cg_emit_module_binding_ensure_init`
    - 新增 `cg_pass_emit_module_binding_ensure_inits`
    - `cg_emit_identifier` 命中模块 binding 时，先 emit ensure_init，再返回 storage slot 表达式
    - `cg_emit_unary` 命中模块 binding 取地址时，也先 emit ensure_init，再对 storage slot 取址或派生地址
    - 删除 `main()` 启动时的 eager binding 初始化循环

4. 写入路径保持语义一致
    - `var` 模块绑定写入前先触发 ensure_init，再直接写 storage slot
    - 不再保留“写入即跳过 init”特例
    - managed / scalar / compound assignment 三条路径都补齐 ensure_init 前置逻辑

5. 测试用例补齐
    - codegen case：顶层 binding 读取先生成 ensure_init 调用，再直接读 storage slot
    - codegen case：`main()` 中不再内联模块 binding 初始化
    - codegen case：模块级 `var` 写入前会先触发 ensure_init
    - codegen case：无 initializer 的顶层 UserType 走 ensure_init 默认值构造
    - codegen case：顶层 binding 的 `&` 先触发 ensure_init，再对 storage slot 取址

6. 验证顺序
    - 每完成一个实现步骤，先补对应 case，再跑最窄的 `test_codegen`
    - 全部完成后，执行全量回归测试

---

## Codegen 改动范围

仅修改 `src/codegen/codegen.c`：

| 位置 | 改动 |
| ------ | ------ |
| `ModuleBinding` 结构体（L755） | 新增 `c_ensure_init_name`、`c_inited_name` 字段；`c_name` 继续作为 storage slot 名称 |
| `cg_register_module_binding`（L6789） | 计算并存储 ensure_init / `_inited` 名称 |
| `cg_pass_register_module_bindings`（L17438） | 额外 emit `_inited` flag 声明 |
| `cg_emit_identifier`（L8091） | 模块 binding 引用前插入 ensure_init，再返回 storage slot 表达式 |
| `cg_emit_unary`（L7437） | 模块 binding 取地址前插入 ensure_init，再对 storage slot 取址 |
| 新增 `cg_emit_module_binding_ensure_init` | 生成每个 binding 的 ensure_init 函数体（复用现有 init expr 逻辑） |
| 新增 `cg_pass_emit_module_binding_ensure_inits` | 遍历所有 binding，调用上一项 |
| `cg_emit_assign`（var 写入路径） | LHS 为模块 var 时，写入前先插入 ensure_init |
| `cg_emit_main_wrapper`（L17968） | 删除 binding 初始化循环 |

---

## 跨包 Public Binding（基于 Lazy 版本补齐）

跨包访问 public 顶层 binding 时，公开 surface 改为 provider-side storage slot + ensure_init。单独公开 storage slot 不够，因为包内和包外的所有访问都必须先 ensure_init；但 storage slot 本身仍需公开，才能保证读取、写入、取地址和其它基于 slot 的计算都统一指向 canonical slot。

lazy 版本补齐跨包值表面时，导出 ABI 约定如下：

- `pu let name: T` / `pu var name: T`：都导出 public storage slot，命名沿用 binding 自身 mangled symbol
- `pu let name: T` / `pu var name: T`：都导出 public ensure_init，命名为 `feng__<module>__<name>__ensure_init__from__void`
- provider 包内访问与跨包访问统一：先调用 ensure_init，再直接读/写/取址 storage slot
- consumer 侧 imported binding codegen 生成 `extern` storage slot 声明和 `extern ensure_init` prototype；不再单独生成值访问 wrapper

当前以 codegen 回归作为实现标记，验收面收敛为：

1. imported `pu let` 通过 `alias.member` 读取时，codegen 成功，并生成 extern storage slot、extern ensure_init 与 ensure_init 调用点
2. imported `pu var` 通过 `alias.member` 读取和写入时，codegen 成功，并生成 extern storage slot、extern ensure_init 与 slot 读写发码
3. imported public binding 的 `&` 语法可用，并在 ensure_init 之后针对 storage slot 取址或派生地址

### 跨包 TODO 拆解

1. 边界先收敛
    - 本轮跨包支持覆盖 public 顶层 binding 的值读取、`pu var` 写入，以及合法语法里的 `&public_binding`
    - public 顶层 binding 统一公开 storage slot 和 ensure_init，两者缺一不可
    - `&public_binding` 不是可以私自裁掉的语法；若包内 `&top_binding` 成立，跨包 public binding 也必须按同一语言语义补齐

2. provider 侧导出面补齐
    - 在 lazy 版本现有 `storage slot + _inited + internal ensure_init` 基础上，补 public slot / public ensure_init 的导出逻辑
    - `pu let` / `pu var`：lib 目标下都公开非 `static` storage slot
    - `pu let` / `pu var`：lib 目标下都公开非 `static` ensure_init，内部保障 init expr 最多执行一次
    - 包内和跨包都按“先 ensure_init，再对 storage slot 操作”发码，不再引入额外值访问 wrapper 特例

3. imported binding declaration emission 补齐
    - consumer codegen 新增 imported public binding declaration pass，不再只有 imported function prototype
    - imported `pu let` / `pu var` 都生成 storage slot `extern` 声明
    - imported `pu let` / `pu var` 都生成 ensure_init `extern` prototype
    - imported synthetic binding 继续只保留 declaration metadata，不回填 initializer

4. 读取路径补齐到 imported binding
    - `cg_emit_member` 命中 `module_alias.binding` 且 binding 来自 imported package 时，先 lower 为 ensure_init 调用，再返回 extern storage slot 表达式
    - `cg_emit_identifier` 命中 imported visible public binding 时，也先 lower 为 ensure_init 调用，再返回 extern storage slot 表达式
    - 读取路径统一避免把 imported module alias 当运行时 object 去发 member access；所有后续计算都围绕 slot 展开

5. 写入路径补齐到 imported `pu var`
    - identifier assignment 命中 imported public `var` 时，先 lower 为 ensure_init 调用，再直接写 extern storage slot
    - member assignment 命中 `module_alias.binding` 且 binding 为 imported public `var` 时，也先 lower 为 ensure_init 调用，再直接写 extern storage slot
    - compound assignment 不新增额外 ABI，统一拆成“ensure_init -> 读 slot -> 计算 -> 写 slot”

6. 取址路径补齐到 imported public binding
    - `&public_binding` 不能退化为对临时值取址；必须先 ensure_init，再针对 canonical storage slot 取址或从 slot 派生地址
    - scalar public binding：先 ensure_init，再返回 `&storage_slot`
    - string / array / `@abi` object 等类型：也必须先 ensure_init，再严格按包内 `&top_binding` 语义，从 storage slot 的当前值派生 borrowed pointer

7. 回归用例补齐
    - codegen case：imported public `let` 通过 `alias.member` 读取，生成 extern storage slot、extern ensure_init 与 ensure_init 调用点
    - codegen case：imported public `var` 通过 `alias.member` 读取与写入，生成 extern storage slot、extern ensure_init 与 slot 读写发码
    - codegen case：imported public binding 的 `&` 语法可用，并命中“ensure_init 后对 slot 取址/派生地址”的正确发码
    - codegen case：若 direct visible imported binding 语法仍成立，再补 identifier 入口的 ensure_init + slot 覆盖，避免只支持半条路径
    - 当前新增的红灯 case 继续作为实现标记，直到全部转绿

8. 验证顺序
    - 先让现有 imported public `let` / `var` codegen case 通过
    - 再让 imported public binding address-of case 通过，确认不是临时值地址
    - 再补 identifier 入口与 compound assignment 等邻接 case
    - 每一步先跑最窄 `test_codegen`，最后再跑全量 `make test`
