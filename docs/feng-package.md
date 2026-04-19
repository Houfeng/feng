# FC语言包分发与二进制复用规范

本文档用于补充 [feng-language.md](./feng-language.md) 中的包分发与二进制复用概要说明,聚焦 FC 自有二进制包与 C ABI 兼容包的分发模式、包格式、编译流程和使用规则。

## 1 包系统概览

- FC 支持两种标准化包分发模式: FC 自有二进制包 `.fp` 和 C ABI 兼容二进制包 `.fcp`。
- `.fp` 适用于 FC 内部公有成员的二进制复用。
- `.fcp` 适用于 `extern` 接口的跨语言复用。
- 两种模式均支持闭源分发、压缩归档与编译加速,且不破坏 FC → C 编译架构。

## 2 分发模式总览

FC 支持两种标准化包分发模式,适配不同复用场景,均支持闭源、压缩、编译加速,不破坏 FC → C 编译架构:

1. FC 自有二进制包(`.fp`): 适用于非 `extern`、FC 内部 `pu` 公有成员复用,仅 FC 间调用
2. C ABI 兼容二进制包(`.fcp`): 适用于 `extern` 修饰、跨语言复用成员,兼容 C/C++/Go/Python 等语言调用

## 3 FC自有二进制包(.fp格式)

### 3.1 适用场景

- 非 `extern` 修饰的 `pu type` / `pu fn` 公有成员
- 仅 FC 项目间复用,不跨语言
- 需求: 闭源分发、隐藏源码、加速编译、不依赖 C ABI

### 3.2 包格式定义

- 扩展名: `.fp` (`FPackage`)
- 底层: ZIP 兼容压缩归档格式,支持标准解压工具
- 核心: 不含 FC 实现源码,仅含接口描述和多平台预编译静态库

### 3.3 包固定结构

```text
库名-版本.fp
├── feng.fm             // 包元信息清单(文本格式,不可修改)
├── api.fi              // 公有接口描述文件(无实现、无源码)
└── lib/
    ├── linux-x64/      // Linux x64 静态库
    │   └── libxxx.a
    ├── windows-x64/    // Windows x64 静态库
    │   └── xxx.lib
    └── macos-arm64/    // macOS arm64 静态库
        └── libxxx.a
```

### 3.4 清单文件(feng.fm)

包 manifest 文件扩展名为 `.fm`,文件名固定为 `feng.fm`,记录包名、版本、模块名、支持平台和接口路径,编译器自动识别。

```text
name:mylib
version:1.0.0
module:mylib
arch:linux-x64,windows-x64,macos-arm64
api_path:api.fi
```

### 3.5 接口文件(api.fi)

公开接口描述文件使用 `.fi` 扩展名,由 FC 编译器自动生成,仅包含公有签名,无任何实现逻辑:

```fc
pu mod mylib;

pu type User {
    pu var name: string;
    pu var age: int;
    pu fn get_info(): string;
}

pu fn add_user(u: User): bool;
fn init();
```

### 3.6 编译与使用流程

1. 发布方: FC 源码 → 编译为 C → 生成 C 静态库 → 自动生成 `api.fi` / `feng.fm` → 打包为 `.fp`
2. 使用方: `use` 模块名导入包 → 编译器读取 `api.fi` 完成类型检查 → 自动链接对应平台静态库
3. 特性: 全程无源码暴露,跳过库源码编译,直接链接加速

### 3.7 约束规则

- 仅支持 FC 间复用,不兼容 C 语言调用
- 接口文件不可手动修改,否则链接失败
- 不同 FC 编译器版本编译的包不兼容
- 私有成员(`pr`)不写入接口文件,完全隐藏

## 4 C ABI兼容二进制包(.fcp格式)

### 4.1 适用场景

- `extern type` / `extern fn` 修饰的 C 兼容成员
- 需跨语言(C/C++/Go/Python)复用
- 需生成 C 可调用的动态库或静态库

### 4.2 包格式定义

- 扩展名: `.fcp` (`FCP-ABIPackage`)
- 底层: ZIP 兼容压缩归档格式
- 核心: C 标准 ABI 接口 + 多平台二进制库 + C 兼容头文件(可选)

### 4.3 包固定结构

```text
库名-版本.fcp
├── feng.fm             // 包元信息清单
├── api.fi              // FC 侧 C 兼容接口描述
├── include/            // C 语言头文件(可选,供外部调用)
│   └── xxx.h
└── lib/
    ├── linux-x64/
    │   ├── libxxx.a    // 静态库
    │   └── libxxx.so   // 动态库
    ├── windows-x64/
    │   ├── xxx.lib
    │   └── xxx.dll
    └── macos-arm64/
        ├── libxxx.a
        └── libxxx.dylib
```

### 4.4 编译与使用流程

1. 发布方: FC `extern` 接口 → 编译为 C ABI 兼容代码 → 生成 C 静态库/动态库 → 打包为 `.fcp`
2. FC 使用方: `use` 模块名后即可直接调用 `extern` 接口,无需额外声明 `link`,因为包内已包含对应链接声明
3. 外部语言使用方: 链接库文件 + 引入头文件,按 C 标准方式调用

### 4.5 约束规则

- 仅支持 C 兼容类型,禁止使用 GC 托管类型,如 `string` 和动态数组
- `extern` 函数禁止捕获变量、使用闭包
- 遵循 C 语言内存布局与调用约定
- 支持动态库热更新、静态库全量嵌入

## 5 包导入语法

```fc
// 导入 FC 自有二进制包(.fp)
use mylib;

// 导入 C ABI 兼容包(.fcp),无需额外声明 link
use c_mylib;
```

## 6 与主规范的关系

- [feng-language.md](./feng-language.md): 语言总体规范、包系统概要、模块、类型、函数、C 互操作、流程控制、GC 与完整示例。
- 本文档: `.fp` / `.fcp` 包格式、编译流程、使用规则和导入语法的独立补充文档。