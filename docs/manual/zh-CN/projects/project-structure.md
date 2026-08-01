# 项目结构

Feng 项目由一个 `feng.fm` 清单、源文件目录和构建输出目录组成。清单格式以[包分发规范](../../../specifications/feng-package.md)为准。

## 典型布局

```text
myapp/
├── feng.fm
├── src/
│   ├── main.ff
│   ├── model.ff
│   └── service.ff
└── build/
```

`src/` 和 `build/` 是默认值，可以在清单中修改。源文件路径不决定模块名；模块由每个文件开头的 `module` 声明决定。

## feng.fm

最小可执行项目：

```text
[package]
name: "myapp"
version: "0.1.0"
target: "bin"
src: "src/"
out: "build/"
```

最小库项目把 `target` 改为 `"lib"`。开发清单可包含四个节：

- `[package]`：名称、版本、目标、源码目录、输出目录和目标平台。
- `[dependencies]`：精确版本或本地路径依赖。
- `[assets]`：构建时复制的资源目录。
- `[registry]`：当前项目使用的包源。

清单使用自身的节式文本语法，不应当按 TOML、YAML 或 INI 解析。

## 模块与文件

```feng
open module myapp.user;

open type User {
  let name: string;
}
```

一个文件只能属于一个模块，同一模块可以分布在多个文件中。每个文件独立声明需要的 `import`。

建议按职责拆分文件和模块，但不要依赖目录层级提供可见性。真正的包外边界由 `open module`、顶层 `open` 和成员可见性共同决定。

## 输出目录

项目构建会为每个目标平台创建独立目录：

```text
build/<platform>/
├── bin/    # 可执行目标
├── lib/    # 库目标
├── mod/    # 符号表
├── obj/    # 中间对象
└── ...
```

库打包产物位于 `build/pkg/<name>-<version>.fb`。构建目录是生成内容，不应提交或手动维护。
