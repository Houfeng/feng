# nbody

公共评测方法见 [主规范](../../README.md)。任务参考
[Benchmarks Game n-body](https://benchmarksgame-team.pages.debian.net/benchmarksgame/description/nbody.html)。

输入为唯一命令行参数：步数，范围 0～100000000。使用太阳、木星、土星、天王星、
海王星的固定初始数据，先抵消总动量，再以步长 0.01 更新速度、位置。
数值均为双精度浮点。输出初始能量和最终能量两个十进制数，绝对误差不超过 1e-8。
循环必须计算实际状态，不能输出预先保存的结果。

`quick` 为 10000 步，`standard` 为 20000000 步，吞吐量单位为步/秒。
正确性检查包含 0、1、1000 步；1000 步另与公开参考结果核对。
不要求各语言采用相同内部存储布局。
