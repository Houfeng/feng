# 测试与发行共用的补丁 LLD

固定 LLVM/LLD 23.1.3，维护 Mach-O FDE 重定位、LTO 展开记录归属及空代码段修复，不提交
完整上游源码。测试和发行统一使用 `toolchain/llvm/<host>/bin/lld`；不再维护另一份
测试专用链接器。发行布局以[分发规范](../../docs/engineering/feng-release-and-install.md)为准。

`source.env` 固定源码地址、SHA256 和补丁修订号。源码是 Debian 发布的上游 orig
归档，散列来自同目录的 `llvm-toolchain-23_23.1.3-1.dsc`；不应用 Debian 修改。
`LICENSE.TXT` 为 LLVM 的 Apache-2.0 WITH LLVM-exception 许可证。

`patches/0001-fix-macho-fde-relocation.patch` 保留 Mach-O 配对重定位的完整
“符号＋偏移”语义，修复函数入口前有数据时的 FDE 关联。它不识别 Feng、sanitizer
或固定前缀长度，不向被链接程序增加正常路径指令。

`patches/0002-fix-macho-lto-unwind-owner.patch` 修复
[LLVM #135888](https://github.com/llvm/llvm-project/issues/135888)：LTO 符号保留原始
bitcode 来源，但展开记录必须按函数实际所属的机器码对象登记和查询。保留原始来源
供诊断及链接映射使用；同时处理 DWARF、compact unwind 与 FDE 查询，不放宽重复
或被合并定义的筛选。不修改 Feng personality、异常插件、语言语义或优化级别。

`patches/0003-ignore-empty-unwind-sections.patch` 回补
[LLVM #225055](https://github.com/llvm/llvm-project/pull/225055) 的修复和回归用例：
空代码段不覆盖任何地址范围，其符号不能覆盖下一函数的有效展开记录。
Clang 的 ThinLTO 链接可生成这种空段；原生代码与 bitcode 混合输入也须正确展开。

维护入口：

```sh
bash scripts/build_lld.sh
bash scripts/build_lld.sh --platform=macos-arm64
bash scripts/build_lld.sh --platform=linux-arm64-gnu --llvm-root=/path/to/llvm-23.1.3
bash scripts/build_lld.sh --platform=linux-x64-gnu --source-archive=/path/to/llvm-toolchain-23_23.1.3.orig.tar.xz
```

维护脚本直接在 macOS ARM64、Linux ARM64 GNU 或 Linux x64 GNU host 上执行。
省略 `--platform` 时构建当前 host；显式指定时必须与当前 host 一致。
三个 host 的制品分别在对应环境中构建、验证和安装。
`--llvm-root` 指定当前构建环境中的完整 SDK；省略时查找已安装的同版本 SDK。
维护脚本不安装依赖、不改变系统默认工具；本地目录用途遵循
[工程文档的仓库本地目录说明](../../docs/engineering/README.md#仓库本地目录)。

构建需要 CMake、Ninja、完整 SDK，以及运行上游 lit 所需的 Python 解释器。
SDK 须包含 `FileCheck`、`not`、`count`、`split-file`、`yaml2obj` 和 `obj2yaml`
等测试辅助工具；这些工具只用于维护验证，不加入 Feng 发行包。
macOS 还需要目标 SDK；本目录不引入自行编写的 Python 脚本。
Linux 上游权限用例需要支持 POSIX 目录权限的构建文件系统。

容器是调用方可选的执行环境，脚本不创建、启动或检查容器。以 Apple Container
为例，先准备满足 SDK、依赖及下述 glibc 基线的 Linux 镜像，在仓库根目录执行：

```sh
container run -d --name llvm-build-arm64 --arch arm64 --cpus 4 --memory 6G \
    --volume "$PWD:/work" --tmpfs /work/build "$LLVM_BUILD_IMAGE" sleep infinity
container exec --env CMAKE_BUILD_PARALLEL_LEVEL=4 llvm-build-arm64 \
    bash /work/scripts/build_lld.sh --llvm-root=/opt/llvm
```

示例假定完整 SDK 位于镜像内的 `/opt/llvm`。源码、`local/` 缓存和 `toolchain/`
安装输出通过共享仓库访问；`build/` 单独挂为 Linux 原生 `tmpfs`，避免共享文件系统
在 `chmod 100` 目录中启动程序时的权限错误。需保留的构建记录应在停止容器前
复制到共享目录。Linux x64 使用对应架构的镜像和 `--arch amd64`；Apple Silicon 上
的转译执行不能代替原生 x64 验收。容器的创建、启停和目录挂载由调用方负责。

采用 CMake 是因为 **LLD 上游本身使用 CMake**。直接复用上游的生成文件、LLVM
库依赖及测试配置，比另写 Makefile 并重复维护这些规则更简单。Ninja 执行 CMake
生成的构建任务；两者仅用于维护者手工预构建 LLD，不改变 Feng 的 Makefile 或
llvm-c-eh 插件的 Makefile。`cmake/LLVMConfig.cmake` 只为独立构建选择 SDK 的
静态库依赖，不修改已安装 SDK。

既有 `scripts/fetch_llvm.sh` 获取的是 LLVM 预编译 SDK，不是构建补丁需要的源码。
本入口复用已安装 SDK，只下载 `source.env` 固定的源码归档；也可用
`--source-archive` 复用本地归档，两种路径均校验同一 SHA256。
默认源码缓存为 `local/lld/llvm-<版本>.tar.xz`；已有归档校验通过后直接复用，
仅在归档不存在时下载。每次维护构建仍重新解压并应用补丁，清理旧 CMake 构建目录，
避免更换 `--llvm-root` 后复用上一 SDK 的头文件、库或工具路径。

脚本只解压 `lld/`、公共 CMake 文件、同版本 lit 及 ELF 测试所需的上游
DTLTO 夹具，使用 SDK 已构建的 LLVM 库。
静态链接 LLVM 及可用的静态依赖，验证运行依赖闭包后生成可搬移的预构建工具。
通过上游 Mach-O／ELF 测试、缺陷回归、依赖和搬移检查后，原位更新
`toolchain/llvm/<host>/bin/lld`，维护 `ld.lld` 和 `ld64.lld` 两个相对别名。
其他 LLVM 工具不变。仅安装已经验证的制品；失败保留已有工具。
macOS 最低版本保持 26.0；Linux 保持发行规范的 glibc 2.34 下限，非系统依赖
必须由现有随包运行库闭合，不能隐式依赖构建机器的库。构建 SDK 的静态库也须满足
此基线；只设置链接目标版本不能消除静态库已有的新系统依赖。
macOS 链接时优先搜索系统 SDK 的库目录，避免 LLVM SDK 自带的 `libc++` 覆盖
系统运行库；这只影响补丁 LLD 的维护构建。
所需许可证随 LLVM 工具保留；构建记录和散列只保留在 `build/lld/<host>/`。

维护顺序为获取／裁剪 LLVM，再分别在三个 host 执行本入口构建补丁 LLD。重新裁剪
会恢复官方 LLD，必须再次执行本入口后才能验证和发布。普通构建、测试、CI 和发行
组装只读取预构建，不下载源码、不现场构建 LLD。

新增验证覆盖 LTO／非 LTO、ARM64／x64 Mach-O 的展开记录、重复弱定义与原生／
bitcode 混合输入；macOS 执行真实抛出、捕获、重抛和清理对照，Linux 执行 ELF
异常行为并检查交叉生成的 Mach-O。Linux 生成的 macOS 程序须在 macOS 上执行，
不能将静态检查记为运行通过。最终执行仓库完整 `make test`，失败如实记录。

源码、补丁或配套 LLVM 版本变化时重新预构建。校验属于构建过程，不增加编译器
代理。三份 host 制品沿用 `toolchain/**` 的 Git LFS 和预构建 Release 流程，
不按输出目标重复分发链接器。
