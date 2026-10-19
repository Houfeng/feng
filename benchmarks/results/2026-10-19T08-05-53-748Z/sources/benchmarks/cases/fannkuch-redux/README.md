# fannkuch-redux

公共评测方法见 [主规范](../../README.md)。任务参考
[Benchmarks Game fannkuch-redux](https://benchmarksgame-team.pages.debian.net/benchmarksgame/description/fannkuchredux.html)。

输入为唯一命令行参数 n，范围 1～11。依次遍历 0～n−1 的全部排列；每次取首元素 k，
反转前 k+1 个元素，直到首元素为 0，统计反转次数。必须实际生成排列并执行反转。
排列顺序从恒等排列开始，以前缀循环左移生成：递归枚举前 r−1 项，再循环左移前 r 项，
共重复 r 次；被测实现可用计数数组迭代实现同一顺序。

输出两个十进制整数：按排列序号偶数加、奇数减得到的反转次数校验和，以及最大反转次数。
校验和必须精确累计有符号整数，不允许溢出或丢失精度，禁止按输入预存输出。
JavaScript 可使用 Number：新增校验独立枚举全部支持输入 1～11，检查 `n! × 最大翻转次数`
不超过 `Number.MAX_SAFE_INTEGER`；这个界限同时覆盖任何中间校验和的绝对值。
计时外的参考实现用递归排列枚举，
并以公开 n=7 的校验和 228、最大值 16 约束参考结果。

`quick` 为 n=7，`standard` 为 n=11；正确性检查覆盖合法输入 1、2、3、7。
吞吐量单位为排列/秒，工作量为 n!。每个程序使用普通数组，复用反转工作区。
