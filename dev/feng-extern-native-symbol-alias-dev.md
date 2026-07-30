# `extern func` 原生符号别名修复

> 状态：待评审，不实施代码
>
> C ABI 类型与调用规则由
> [Feng ABI 互操作](./feng-interop-delivered.md) 定义；本文只定义 `extern func`
> 的 codegen 修复。

## 1. 问题

多个 Feng `extern func` 可以使用不同签名调用同一个原生符号：

```feng
@cdecl("libc", "write")
extern func writeBytes(fd: i32, data: byte*, count: uint): int;

@cdecl("libc", "write")
extern func writeText(fd: i32, data: string*, count: int): int;
```

当前 codegen 直接使用原生符号名生成 C 声明和调用，并按原生符号名去重：

```c
extern int64_t write(int64_t, uint8_t *, uint64_t);
```

因此先注册的声明决定同名原生符号的唯一 C 原型。其他签名调用该原型时会产生
类型警告，严重时可能按错误 ABI 发码。

当前 extern 注册和调用还按简单 Feng 名查找，无法可靠区分跨模块声明和重载。

## 2. 目标

1. 每个 `extern func` 使用自己的 C ABI 签名，不受其他声明影响。
2. 复用普通顶层函数的完整生成名称规则，支持模块命名空间和重载。
3. 生成代码直接调用原生符号，不增加 wrapper 和运行时调用。
4. `@cdecl`、`@stdcall`、`@fastcall` 的现有调用约定和 ABI 转换保持不变。
5. `@runtime extern func` 保持由 runtime header 声明，不进入本修复路径。
6. C variadic 的固定参数个数完整写入并读回 `.ft`，保证跨包调用不丢失。

## 3. 方案

### 3.1 名称

每个 C ABI `extern func` 保留三个独立名称：

| 名称 | 来源 | 用途 |
|---|---|---|
| Feng 名称 | `extern func` 声明 | 源码名称解析 |
| Feng 完整生成名 | 模块、Feng 名称和参数签名 | 生成 C 声明与调用 |
| 原生符号名 | 调用约定注解的第二个参数；省略时取 Feng 名称 | 链接器符号 |

Feng 完整生成名直接复用普通顶层函数的命名流程：

```text
cg_fn_mangle(module, name) + 参数类型签名后缀
```

其中 `module` 必须取自声明所属 program，不能依赖调用点或遍历过程中的当前模块。

不增加 `__native` 后缀，不建立第二套 mangling 规则。

### 3.2 原生符号映射

生成唯一的 C 标识符，并通过汇编标签映射到原生符号：

```c
#if defined(__APPLE__)
#define FENG_NATIVE_SYMBOL(name) __asm__("_" name)
#else
/* 当前非 Apple 交付目标仅为 Linux ELF。
 * Windows/COFF 尚未支持，启用前必须在此增加独立映射规则。 */
#define FENG_NATIVE_SYMBOL(name) __asm__(name)
#endif

extern int64_t
feng__std_test__helpers__writeText__from__i32__p_string__i64(
    int32_t,
    char *,
    int64_t
) FENG_NATIVE_SYMBOL("write");
```

调用点使用 Feng 完整生成名：

```c
feng__std_test__helpers__writeText__from__i32__p_string__i64(...);
```

目标文件仍直接引用 `write`；Apple Mach-O 目标引用 `_write`。不生成函数体，
不增加中间调用。

调用约定宏继续写入各自声明。原生符号必须按 C 字符串规则转义后输出。

### 3.3 注册、去重和查找

1. `ExternFn` 分别保存 Feng 名称、Feng 完整生成名、原生符号名、声明身份和所属
   program。
2. extern 按 `FengDecl` 身份注册和去重，不按简单 Feng 名或原生符号名去重。
3. 每个不同的 extern 声明都生成自己的 C 原型，即使它们映射到同一原生符号。
4. 调用点使用 semantic 已解析的 `function_decl` 查找 `ExternFn`。
5. 泛型 extern 的具体调用继续复用其声明对应的原生符号和调用约定。

生成名与系统头文件中的 C 标识符不同，因此普通 C ABI extern 不再需要按
系统函数名跳过声明。runtime contract 仍使用 runtime header 作为唯一声明来源。

### 3.4 `@cdecl` 参数

```feng
@cdecl("libc")
extern func write(...);
```

原生符号名默认为 `write`。

```feng
@cdecl("libc", "write")
extern func writeText(...);
```

Feng 名称为 `writeText`，原生符号名为 `write`。

第一个参数 `libc` 只表示链接依赖，不参与生成名，也不构成静态链接符号命名空间。

### 3.5 C variadic 跨包元数据

调用约定注解的第三个参数表示 C 原型中的固定参数个数：

```feng
@cdecl("libc", "snprintf", 3)
extern func snprintf_f64(buf: byte*, size: uint, fmt: byte*, value: f64): i32;
```

对应 C 原型：

```c
extern int32_t feng__std__numeric__snprintf_f64__from__...(
    uint8_t *,
    uint64_t,
    uint8_t *,
    ...
) FENG_NATIVE_SYMBOL("snprintf");
```

当前 `.ft` 只保存调用约定、库名和原生符号名，跨包导入时会丢失固定参数个数。
本方案同时完成以下修复：

1. `FengSymbolDeclView` 保存 C variadic 固定参数个数。
2. symbol export 从调用约定注解的第三个参数读取该值。
3. `.ft` 使用独立、可忽略的 attribute 写入该值；不修改既有记录含义。
4. `.ft` reader 读回该值；旧 `.ft` 缺少该 attribute 时保持当前默认值 `0`。
5. imported module 重建调用约定注解时恢复第三个整数参数。
6. symbol clone、provider 内部视图和相关测试同步覆盖该字段。

新增 attribute 不提升 `.ft` 版本：旧 reader 忽略未知 attribute，新 reader 可读取旧
`.ft`。使用旧编译器生成的 `.ft` 不包含该值，需要重新构建提供方后才能恢复跨包
C variadic 信息。

## 4. 不新增 wrapper

当前 `extern func` 只生成原生函数声明。参数转换、`@abi type` 转换和返回值处理
均在调用点完成。

本修复保持该结构：

```text
Feng 调用点 -> 原生符号
```

不会变为：

```text
Feng 调用点 -> wrapper -> 原生符号
```

普通顶层 `@abi func` 的导出 wrapper 与本方案无关。

## 5. 责任和边界

1. 编译器保证每个 Feng 声明独立生成，不检查不同包中的原生声明是否一致。
2. 声明者负责保证参数、返回值和调用约定与目标原生 ABI 一致。
3. 多个不同 Feng 签名可以映射到同一个最终原生符号。
4. 普通静态链接不能表达“分别调用两个静态库中的同名符号”。需要这种能力时，
   原生库或绑定层必须提供不同的导出符号。
5. 除补齐 C variadic 固定参数个数外，本修复不改变 `.ft` 的其他 C ABI 语义。
6. Windows/COFF 尚未支持；本次只实现并验证 Apple Mach-O 和 Linux ELF。

## 6. 实施 TODO

- [ ] TODO 1：按声明身份重构 extern 注册和查找。
  - [ ] `ExternFn` 分离 Feng 名称、完整生成名、原生符号名、`FengDecl` 和所属
    program。
  - [ ] 复用普通顶层函数的模块和参数签名 mangling；模块取自声明所属 program。
  - [ ] 普通、泛型、同模块和跨模块 extern 调用均按已解析声明定位。
  - [ ] 补齐编译器用例和必要的 fcts 用例。
  - [ ] `make test` 全量回归通过。

- [ ] TODO 2：补齐 C variadic 跨包元数据。
  - [ ] symbol view、export 和 clone 保存固定参数个数。
  - [ ] `.ft` writer/reader 使用独立 attribute 往返固定参数个数。
  - [ ] imported module 重建调用约定注解的第三个整数参数。
  - [ ] 补齐默认值、`.ft` 往返、bundle 导入和跨包 codegen 用例。
  - [ ] 验证本包和跨包生成的 C variadic 原型一致。
  - [ ] `make test` 全量回归通过。

- [ ] TODO 3：生成独立原型并映射原生符号。
  - [ ] 增加统一的平台原生符号映射宏，并注明 Windows/COFF 尚未支持。
  - [ ] 每个普通 C ABI extern 生成独立原型，不再按原生符号名去重或按系统函数
    名跳过。
  - [ ] 调用点改用 Feng 完整生成名，确认目标文件仍直接引用原生符号。
  - [ ] 保持 calling convention、C variadic 和 runtime contract 路径正确。
  - [ ] 验证 Apple 目标引用带下划线的 Mach-O 原生符号。
  - [ ] 验证 ELF 目标引用不带下划线的原生符号。
  - [ ] 验证默认原生名、显式原生名、系统头文件同名符号和跨包 extern。
  - [ ] 验证生成代码没有 extern wrapper 和额外运行时调用。
  - [ ] 补齐同一原生符号对应不同指针、标量和返回值签名的编译器用例及 fcts
    用例。
  - [ ] 验证 `feng build std_test` 不再出现同名 extern 原型导致的
    `-Wpointer-sign` 警告。
  - [ ] `make test` 全量回归通过。
