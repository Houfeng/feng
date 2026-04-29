# runtime 与 codegen 目录与接口草案（Phase 1A）

> 本文档配合 [feng-phase1a-tasks.md](./feng-phase1a-tasks.md)，给出 Phase 1A 实施时 `src/runtime/` 与 `src/codegen/` 的目录布局、对外头文件、关键 ABI 与命名约定。所有不在本草案中的接口在 1A 阶段一律不得对外暴露。

## 1 目录布局

```
src/
  runtime/
    feng_runtime.h         # 唯一对外公共头，被 codegen 与运行时实现共同包含
    feng_object.c          # FengManagedHeader、retain/release、回收 hook
    feng_string.c          # FengString 字面量/拼接/读取
    feng_array.c           # FengArray 元素读写 / 越界 panic
    feng_exception.c       # 全局异常栈、try/throw/finally 接口
    feng_panic.c           # 致命错误、abort、调试日志
  codegen/
    codegen.h              # 对外入口（feng_codegen_emit_program）
    codegen.c              # 入口分派 + 上下文 lifecycle
    emit_module.c          # 翻译单元级骨架：includes / forward decls / main 包装
    emit_decl.c            # 顶层 fn / type / 模块级绑定
    emit_type.c            # type 布局、构造函数、终结器表
    emit_stmt.c            # 语句发码 + cleanup 栈
    emit_expr.c            # 表达式发码 + 临时值生命周期
    mangle.c               # 模块/类型/函数符号 mangling
    naming.c               # 运行时 ABI 函数名常量集中
build/
  gen/                     # 临时生成的 .c（CLI 写入）
  lib/libfeng_runtime.a    # 运行时静态库
test/
  runtime/test_runtime.c   # 运行时单测
  smoke/phase1a/           # 端到端 smoke 套件
```

新增 Make 目标（增量补充，不改现有目标）：

```
make runtime  -> build/lib/libfeng_runtime.a
make smoke    -> 通过 tools/run_smoke.sh 跑 test/smoke/phase1a
make test     -> 既有用例 + test_runtime + smoke
```

## 2 运行时公共头 `feng_runtime.h`

仅列出 1A 必需且对外可见的 ABI；任何未在此列的符号在 1A 不得被 codegen 直接使用。

### 2.1 托管对象头

```c
typedef enum FengTypeTag {
    FENG_TYPE_TAG_STRING = 1,
    FENG_TYPE_TAG_ARRAY  = 2,
    FENG_TYPE_TAG_OBJECT = 3,    /* 普通 type */
    FENG_TYPE_TAG_CLOSURE = 4    /* 闭包环境，1A 仅占位，不要求实现完整闭包 */
} FengTypeTag;

typedef void (*FengFinalizerFn)(void *self);

typedef struct FengTypeDescriptor {
    const char *name;            /* 调试用全限定名 */
    size_t size;                 /* 含头与字段的总字节数；array/string 该字段为 0 */
    FengFinalizerFn finalizer;   /* 可为 NULL */
    /* 1A 不暴露引用枚举元数据；Phase 1B 引入 cycle collector 时再扩展 */
} FengTypeDescriptor;

typedef struct FengManagedHeader {
    const FengTypeDescriptor *desc; /* 指向静态描述符 */
    FengTypeTag tag;
    uint32_t refcount;              /* 原子访问，1A 暂以 stdatomic 实现 */
} FengManagedHeader;
```

### 2.2 retain / release

```c
void *feng_retain(void *obj);                  /* obj 可为 NULL，行为为 no-op；返回 obj */
void  feng_release(void *obj);                 /* obj 可为 NULL；归零时调用 finalizer 后释放 */

/* codegen 在赋值/传参/返回时使用以下封装，集中所有 barrier */
void  feng_assign(void **slot, void *new_value); /* release(*slot); *slot = retain(new_value) */
void *feng_take(void **slot);                    /* 取走所有权，*slot 置 NULL，不调整 refcount */
```

### 2.3 字符串

```c
typedef struct FengString FengString;

const FengString *feng_string_literal(const char *utf8, size_t length);
                                                /* 字面量，refcount = 持久化（运行时常量池） */
FengString *feng_string_concat(const FengString *a, const FengString *b);
size_t      feng_string_length(const FengString *s);
const char *feng_string_data(const FengString *s);
```

### 2.4 数组

```c
typedef struct FengArray FengArray;

FengArray *feng_array_new(const FengTypeDescriptor *element_desc,
                          size_t element_size,
                          bool element_is_managed,
                          size_t length);
size_t     feng_array_length(const FengArray *a);
void      *feng_array_data(FengArray *a);                 /* 返回 items 起始 */
void       feng_array_check_index(const FengArray *a, size_t index); /* 越界 panic */
```

> 1A 不提供 `push`/`pop`/`slice`：固定长度数组语义足以承载 smoke 子集。

### 2.5 普通对象

```c
void *feng_object_new(const FengTypeDescriptor *desc); /* 分配 + 头初始化 + 字段清零 */
```

构造函数发码示例：
```c
static User *User__new__from__name__age(const FengString *name, int32_t age) {
    User *self = (User *)feng_object_new(&FengTypeDesc__User);
    feng_assign((void **)&self->name, (void *)name);
    self->age = age;
    return self; /* +1 ownership 移交调用方 */
}
```

### 2.6 异常

```c
typedef struct FengExceptionFrame {
    struct FengExceptionFrame *prev;
    jmp_buf jb;
    void *value;            /* 当前抛出的值，类型 tag 通过 FengManagedHeader 区分 */
    int    is_managed;
} FengExceptionFrame;

void  feng_exception_push(FengExceptionFrame *frame); /* 由 codegen 在 try 之前调用 */
void  feng_exception_pop(void);                       /* 在 try 正常退出 / catch 完成后调用 */
void  feng_exception_throw(void *value, int is_managed) __attribute__((noreturn));
                                                      /* 触发 longjmp 到最近 frame，沿途由 codegen 已展开局部清理 */
```

> codegen 必须保证：每一段 `try` 的代码生成都成对调用 push/pop；异常展开路径上由 codegen 直接 emit `feng_release` 调用，运行时不维护跨函数自动 cleanup 链。

### 2.7 panic

```c
void feng_panic(const char *fmt, ...) __attribute__((noreturn));
```

用于 `feng_array_check_index` 等不可恢复错误，1A 直接 `abort()`。

## 3 codegen 对外入口

```c
/* codegen.h */
typedef struct FengCodegenOptions {
    const char *entry_module;       /* 1A 仅支持 bin：必须指向 main 所在模块 */
    bool        release;
} FengCodegenOptions;

typedef struct FengCodegenOutput {
    char  *c_source;                /* malloc'd UTF-8，调用方负责 free */
    size_t c_source_length;
} FengCodegenOutput;

bool feng_codegen_emit_program(const FengSemanticAnalysis *analysis,
                               FengCompileTarget target,
                               const FengCodegenOptions *options,
                               FengCodegenOutput *out_output,
                               char **out_error /* malloc'd; NULL on success */);
```

不变量：
- 输入必须是已经过 `feng_semantic_analyze` 校验的分析结果。
- 1A 阶段 `target` 必须为 `FENG_COMPILE_TARGET_BIN`；`LIB` 直接返回 `false` + `out_error = "Phase 1A: lib target not yet supported"`。
- 失败时不写出 `c_source`。

## 4 命名与 mangling

集中在 `mangle.c`：

| 实体 | 命名规则 |
| --- | --- |
| 模块 `feng.examples` | 段以 `__` 连接，整体小写：`feng__examples` |
| 类型 `User` 在 `feng.examples` | `Feng__feng__examples__User` |
| 类型描述符 | `FengTypeDesc__feng__examples__User` |
| 顶层函数 `main` | `feng__examples__main` |
| 类型方法 `User.say(string)` | `Feng__feng__examples__User__say__from__string` |
| 构造函数 | `Feng__feng__examples__User__new__from__<param-types>` |
| 终结器 | `Feng__feng__examples__User__finalize` |
| 闭包环境（1A 占位） | `FengEnv__<owner>__<seq>` |

> 仅以下内建类型在 mangling 里有专用编码：`i8/i16/i32/i64/u8/u16/u32/u64/f32/f64/bool/string`；其余类型在 1A 不参与方法重载（暂未实现重载），mangling 只需保证当前用例不冲突。

## 5 ARC 自动插入约束

- 每一个发码块（block）维护一份按声明顺序排列的 cleanup 列表（仅记录托管引用槽）。
- 块正常退出时，按照逆序 emit `feng_release(slot)`。
- `return expr;` 先把 expr 的 +1 ownership 移交返回值，再按上述顺序释放剩余局部。
- `throw expr;` 先 retain expr 转交异常 frame，再按当前函数所有 active 块的逆序 emit cleanup，再 `feng_exception_throw`。
- `try { A } catch { B } finally { C }`：
  - A 内部的常规 cleanup 与正常路径相同。
  - 异常路径在 longjmp 之前由 try frame 自身记录的 cleanup 完成；catch 进入前再 push 一份新的 cleanup 上下文。
  - finally 的 emit 必须在「正常出口」「catch 出口」「未捕获再抛出」三条路径上各自被复制一次，禁止依赖运行时跨函数遍历。

> 这是 1A 唯一允许的 cleanup 实现策略；后续如改为基于运行时 cleanup 链，需要单独评审并更新本文档。

## 6 1A 不实现项（防止 scope creep）

- 闭包真实捕获、Lambda 表达式发码（仅保留语法解析）。
- 方法重载、`spec`/`fit` 调度。
- `match` 表达式 / `match` 语句的发码。
- `@fixed` 类型与 ABI 桥接。
- 循环检测器、终结器复活、跨对象 finalize 顺序保证。
- `extern fn` 中除 `@cdecl("c")` 调用 `printf`/`puts`/`exit` 外的其他形态。

任何 1A 不实现项在 codegen 入口遇到时必须返回明确的 `out_error`，禁止悄悄跳过。
