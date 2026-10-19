# Linux 九配置评测：Feng builtin 分配器

沿用 `2026-10-19T08-05-53-748Z` 的 Linux ARM64 容器、源码、编译器、runtime、std 与其他八种配置制品。
仅重建五个 Feng 可执行文件，在原 Release 构建参数中增加 `--alloc=builtin`。
不修改编译器、runtime、std、用例源码或正式评测脚本。

重建前保存原构建记录与 system 制品；记录 builtin 构建命令、制品及 mimalloc 哈希，检查 malloc/free 的符号定义。
使用现有调度器运行全部五项、九种配置：2 轮预热、7 轮正式采样，运行前及每次采样均校验结果。
归档完整报告、原始样本和本实验操作脚本；报告明确标注 Feng/builtin，并与上一轮 system 数据比较。
仅执行性能评测，不执行 make test。跨轮差异不能单独归因于分配器。
