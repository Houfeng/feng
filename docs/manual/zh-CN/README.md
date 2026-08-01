# Feng 用户手册（简体中文）

本目录保存面向 Feng 开发者的使用文档，重点说明如何安装、学习和使用 Feng，而不是解释编译器内部如何实现。

## 内容边界

本目录用于编写：

- 安装、环境配置和快速入门。
- Feng 语言功能的使用说明与完整示例。
- 项目创建、构建、运行、测试、调试和依赖管理。
- 标准库、互操作及常见开发任务的使用说明。
- 推荐的代码风格和实践建议。

使用手册侧重任务、示例和使用方式，不解释编译器内部实现。手册中的导航与延伸阅读只引用本语言目录内的内容，确保简体中文手册可以独立发布。

## 手册目录

### 入门

- [安装 Feng](./getting-started/installation.md)
- [快速开始](./getting-started/quick-start.md)
- [第一个项目](./getting-started/first-project.md)

### 语言指南

- [值与绑定](./language/values-and-bindings.md)
- [类型](./language/types.md)
- [表达式](./language/expressions.md)
- [函数](./language/functions.md)
- [流程控制](./language/control-flow.md)
- [模式匹配](./language/pattern-matching.md)
- [自定义类型](./language/user-defined-types.md)
- [契约与 fit](./language/contracts-and-fit.md)
- [泛型](./language/generics.md)
- [异常处理](./language/error-handling.md)
- [模块与可见性](./language/modules-and-visibility.md)

### 项目开发

- [项目结构](./projects/project-structure.md)
- [构建与运行](./projects/build-and-run.md)
- [依赖管理](./projects/dependencies.md)
- [检查、测试与调试](./projects/testing-and-debugging.md)

### 标准库、工具与互操作

- [标准库指南](./standard-library/README.md)
- [CLI](./tooling/cli.md)
- [编辑器](./tooling/editor.md)
- [代码格式化](./tooling/formatter.md)
- [C 互操作](./interop/c-interop.md)
- [Feng 代码风格建议](./feng-style.md)

## 目录设计

```text
zh-CN/
├── README.md
├── getting-started/
│   ├── installation.md
│   ├── quick-start.md
│   └── first-project.md
├── language/
│   ├── values-and-bindings.md
│   ├── types.md
│   ├── expressions.md
│   ├── functions.md
│   ├── control-flow.md
│   ├── pattern-matching.md
│   ├── user-defined-types.md
│   ├── contracts-and-fit.md
│   ├── generics.md
│   ├── error-handling.md
│   └── modules-and-visibility.md
├── projects/
│   ├── project-structure.md
│   ├── build-and-run.md
│   ├── dependencies.md
│   └── testing-and-debugging.md
├── standard-library/
│   ├── README.md
│   ├── collections.md
│   ├── text.md
│   ├── filesystem-and-io.md
│   ├── time-and-math.md
│   └── platform-and-process.md
├── tooling/
│   ├── cli.md
│   ├── editor.md
│   └── formatter.md
├── interop/
│   └── c-interop.md
└── feng-style.md
```

各目录职责如下：

- `getting-started/`：帮助开发者完成安装、首次运行和第一个完整项目。
- `language/`：按学习顺序讲解 Feng 的主要语言能力及其使用方式。
- `projects/`：说明项目组织、构建、运行、依赖、测试和调试。
- `standard-library/`：提供标准库的使用指南和典型任务示例，不重复定义标准库规范。
- `tooling/`：说明命令行、编辑器支持和格式化工具的使用方式。
- `interop/`：说明 Feng 与其他语言及原生接口的互操作方式。
- `feng-style.md`：提供 Feng 代码风格建议。

## 推荐阅读顺序

新用户应依次阅读安装、快速开始和第一个项目，再进入语言指南。掌握语言基础后，可根据任务查阅项目开发、标准库、工具链和互操作文档。

后续手册内容应按用户完成任务所需的阅读顺序逐步补充，并保持示例完整、可理解、可执行。目录中的章节按实际编写进度创建，不预先建立空文件。
