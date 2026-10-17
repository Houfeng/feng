# spectral-norm

公共评测方法见 [主规范](../../README.md)。任务参考
[Benchmarks Game spectral-norm](https://benchmarksgame-team.pages.debian.net/benchmarksgame/description/spectralnorm.html)。

输入为唯一命令行参数 n，范围 1～10000。使用双精度浮点，矩阵元素（从 0 编号）为
`A(i,j) = 1 / ((i+j)*(i+j+1)/2 + i + 1)`。u 初始全为 1，重复十次：
`v = Aᵀ(Au)`，`u = Aᵀ(Av)`，最后输出 `sqrt(dot(u,v) / dot(v,v))`。

矩阵元素在乘法循环中按公式计算；分别实现元素计算、A 乘向量、Aᵀ 乘向量、
AᵀA 乘向量四个函数，使用三个长度为 n 的普通数组并复用工作区。
输出一个十进制数，绝对误差不超过 1e-9。不得使用外部线性代数库或预存答案。
计时外参考实现显式构造矩阵，与被测的即时元素计算区分；n=100 另以公开结果
1.274219991 校验，公开值只保留九位小数。

`quick` 为 n=100，`standard` 为 n=2000；正确性检查覆盖合法输入 1、2、10、100。
吞吐量单位为矩阵元素求值/秒，工作量为 40×n²。
