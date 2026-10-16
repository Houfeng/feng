# Union 成员引用的静态循环识别修复

> **状态：从 union 布局优化中拆出，修复和回归用例保存为独立 patch，当前未应用。日期：2026-10-16。**
>
> 补丁：[feng-union-cycle-detection-bugfix.patch](feng-union-cycle-detection-bugfix.patch)。
> 应用基线：第一阶段提交 `65672f13` 加本轮纯 union 布局变更。

## 1. 发现过程与根因

补充 union 循环扫描用例时，让两个对象仅通过 `Option<Node>` 字段相互引用，离开
作用域后两个终结器均未执行。`65672f13` 的旧 union 布局也复现，生成描述符的
`is_potentially_cyclic` 为 false。

原静态分析只解析字段直接引用的普通类型，未将 union 的实际成员引用纳入类型图。
这是已有循环识别遗漏，不是静态 slot 表或 tag 选择机制引入的问题。

## 2. 修复边界

`src/semantic/cyclic.c` 收集 union 的具体类型引用和实际参与存储的泛参引用。
引用集合只从未设置变为已设置，求解至不再变化后，将引用边交给现有 SCC 判定。
解析沿用声明及模块身份，支持导入类型、嵌套 union、数组和泛型实参；指针和未使用
的泛参不产生托管边。不将所有含 union 的类型统一标为循环类型。

最初的递归展开实现曾对 `spec Branch: i32 | Branch[]` 不终止，旧编译器没有此
问题。补丁保存的是已改为有限集合求解的版本，并携带递归数组、参数交换、泛参
层层包裹及未使用参数的终止性覆盖。

不修改 runtime 循环收集器、union 布局、借用消除条件、DAP 或部分初始化清理。

## 3. 独立回归覆盖

- 静态分析：直接、嵌套、泛型、数组中的 union 引用；非循环、原始指针和未使用
  泛参对照；递归 union 求解终止。
- CLI：仅经 union 字段形成的环、多个活动字段、组外存活对象、复活和再次回收。
- 源码隐藏的泛型 provider／consumer，四种 Debug／Release 组合。

纯布局用例保留旧分析已经能识别的循环类型，验证 union 活动 slot 的扫描、清理及
复活。仅经 union 成环的缺陷覆盖随本补丁保存，两者的验证目标不同。

## 4. 已知性能影响

正确识别后，相关对象的释放进入已有循环收集路径；现有借用证明也排除潜在循环
类型，恢复 aggregate retain／release 和清理登记。最终 IR 中相关调用仍存在，
并影响一处递归到循环的转换。尚未单独量化这两部分各占多少成本。

拆分前 Linux ARM64 的受控诊断中，旧布局从 0.360 s 变为 1.792 s，新布局从
0.313 s 变为 1.723 s。新布局 RSS 约为 17.27 MiB。旧识别版本含已知缺陷，仅用于
归因；这些数字也不是三项修复全部拆出后的正式评测。

本补丁解决正确性问题，但性能成本仍待人工决策。恢复时需单独评测，不能沿用
纯布局版本的成绩宣称修复后的性能达标。证据索引见
[布局优化记录 §8.5](feng-union-layout-performance-optimize.md#85-第二阶段-linux-性能结果与退化归因2026-10-16)。

## 5. 应用与验证

```sh
git apply --check docs/engineering/feng-union-cycle-detection-bugfix.patch
git apply docs/engineering/feng-union-cycle-detection-bugfix.patch
```

补丁以纯 union 版本为基线，不依赖另外两份 bugfix patch。

- [x] 修改前编译器复现漏判；组合实现通过 Linux sanitizer 和普通全量回归。
- [x] 核验独立补丁应用及反向还原；三份补丁的六种应用顺序均通过，组合后的实现与拆分前备份逐字节一致。
- [x] 在 Linux ARM64 隔离目录仅应用本补丁，静态引用及递归求解用例通过；
      仅经 union 成环、跨包泛型和复活用例在四种构建组合下通过。
- [ ] 人工决定应用后，执行全量回归和独立性能评测。

拆分后专项验证退出码为 0，记录为
`temp/union-layout-split-20261016/cycle-focused.log` 和 `cycle-focused.status`。
本次没有对单独应用本补丁的版本执行全量 `make test` 或性能评测。
