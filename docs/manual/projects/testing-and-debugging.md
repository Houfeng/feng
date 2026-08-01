# 检查、测试与调试

Feng 当前把语义检查、用户测试和调试分成独立工作流。

## 项目检查

```bash
feng check
feng check --format json
```

`check` 总是检查解析出的整个项目，而不是只检查传入的单个 `.ff` 文件。参数可以是项目目录、`feng.fm`，或项目内任意文件；工具会向上查找最近的清单。

文本输出适合终端，JSON 输出适合编辑器和 CI。

## 编写测试项目

CLI 当前没有 `feng test` 命令。测试可以写成普通的 `target: "bin"` 项目，使用标准库 `std.test`：

```feng
module app_test;

import std.test;

func main(args: string[]) {
  describe("math", () {
    test("addition", () {
      assert(1 + 1 == 2, "addition should produce two");
    });
  });
}
```

通过普通运行命令执行：

```bash
feng run
```

`std.test` 还提供跳过标记和 `assertEquals`。测试项目可以把被测库声明为本地路径依赖。

## VS Code 调试

安装 Feng VS Code 插件后，在“运行和调试”中创建 Feng 启动配置。插件会调用 `feng dap --stdio`，并尝试从最近的 `target: "bin"` 清单推导程序路径和构建任务。

当前调试目标是 macOS 和 Linux 上的本地非 release 可执行项目。支持断点、栈帧和变量查看；watch/evaluate 支持标识符、成员访问、常量整数索引以及简单算术和比较表达式，不支持带副作用的调用或赋值。

## 诊断问题

遇到构建问题时建议按以下顺序定位：

1. 运行 `feng check` 获取语法和语义诊断。
2. 运行 `feng deps install --force` 排除依赖缓存问题。
3. 运行 `feng clean` 后重新构建。
4. 使用 `feng --help` 或对应命令的 `--help` 核对参数。

`feng tool` 下的词法、语法和语义子命令面向高级诊断，不是普通项目工作流的必需步骤。
