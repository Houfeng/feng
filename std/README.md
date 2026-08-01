# 标准库

本目录聚合标准库及其测试项目，自身不是 Feng 项目，不包含 `feng.fm`。

```text
std/
├── std/          # 标准库项目
└── std_test/     # 标准库测试项目
```

标准库源码位于仓库根目录下的 `std/std/src/`：

```text
src/
├── basic/        # 基础共用类型与错误处理
├── collections/  # 集合及容器
├── fs/           # 文件系统与路径处理
├── io/           # 核心 I/O 契约与标准输入输出
├── net/          # 网络与套接字
├── numeric/      # 标量扩展与数值计算
├── platform/     # 宿主操作系统与体系结构信息
├── process/      # 进程、环境变量与信号
├── text/         # 字符串、Unicode 与文本处理
├── thread/       # 多线程、锁与状态通知
└── time/         # 时钟、日期与时间
```

标准库行为测试统一放在 `std/std_test/src/`，由仓库根目录的 `make std-tests` 执行。
