# Feng 迭代器开发文档

> 规范来源：[docs/feng-iterator.md](../docs/feng-iterator.md)  
> 本文记录核心实现原理与分步开发任务。

---

## 0 核心实现原理

### 0.1 协议识别机制

编译器通过两个内建注解识别迭代协议，不感知任何类型名称或模块路径：

- **`@iterable`**：标注在容器类型的某个成员方法上，表示"产生游标"入口。编译器通过注解名称定位该方法，调用后得到游标对象。
- **`@iterator`**：标注在游标类型的某个成员方法上，表示"推进迭代"入口。编译器通过注解名称定位该方法，结合返回类型结构约定（具名元组，第一元素为 `bool`）驱动循环。

两个注解均存储在方法的注解列表中，语义阶段遍历类型成员时一次性完成收集与验证。

### 0.2 `for/in` 展开模型

```
for let it in expr { body }
```

展开为：

```
// 有 @iterable 的情况
let __cursor = expr.<@iterable方法>();     // 游标创建，恰好一次
loop {
  let __r = __cursor.<@iterator方法>();   // 内联调用
  if !__r.item1 { break; }
  let it = __r.item2;                     // 直接绑定，无中间拷贝
  body;
}
```

若 `expr` 的类型自身带 `@iterator`（无 `@iterable`），则跳过游标创建步骤，直接在 `expr` 上循环调用 `@iterator` 方法。

### 0.3 发码性能目标（强制）

| 目标 | 说明 |
| --- | --- |
| `@iterable` 恰好调用一次 | 不得重复求值游标 |
| `@iterator` 静态内联 | 游标类型编译期已知时，消除间接调用 |
| 不引入额外堆分配 | `for/in` 展开本身不分配堆内存 |
| 循环变量无额外拷贝 | 直接绑定 `@iterator` 返回值第二元素 |

不达到以上目标视为编译器缺陷。

### 0.4 返回类型结构检查

`@iterator` 方法的返回类型须是具名元组类型，结构为 `(bool, E)`。编译器在语义阶段按以下步骤校验：

1. 解析返回类型，确认其为具名元组（`type` 以圆括号声明）。
2. 确认元组元素数量恰好为 2。
3. 确认第一元素类型为 `bool`。

不关心类型名称，仅验证结构。第二元素类型在 `for/in` 节点解析时从方法返回类型按需读取。

---

## 1 解析阶段

- [ ] **注解存储**：确认 `@iterable` / `@iterator` 能被解析器正确解析并附加到方法节点的注解列表。（若当前解析器对任意标识符注解已通用支持，此步可能只需注册名称白名单。）
- [ ] 补齐本阶段测试用例并执行全量回归测试。

---

## 2 语义分析阶段

### 2.1 注解约束检查

`@iterable`/`@iterator` 注解已在解析阶段挂到方法节点上，方法本身也已在类型成员集内，无需预缓存到额外描述符字段。语义阶段的任务是对各约束做一次全面校验，校验结果（方法引用）在 §2.4 解析 `for/in` 时按需扫描取用。

**符号表导出**：导出方法符号时，必须将 `@iterable`/`@iterator` 注解信息一并写入导出元数据，否则其他包 `import` 后扫描可见面时无法识别该注解，跨包 `for/in` 将失败。

- [ ] `@iterable` / `@iterator` 出现在非方法位置（字段、`type` 声明体顶层等）时报语义错误。（语法上合法，位置合法性属于语义约束，在此阶段检查。）
- [ ] 遍历类型可见面，若找到多于一个 `@iterable` 方法则报错。
- [ ] 遍历类型可见面，若找到多于一个 `@iterator` 方法则报错。
- [ ] 同一类型可见面内同时存在 `@iterable` 和 `@iterator` 时报错。
- [ ] 确认方法符号导出时携带 `@iterable`/`@iterator` 注解信息（跨包 `for/in` 依赖此元数据）。

### 2.2 `@iterable` 约束检查

- [ ] 被标注方法必须无参数（除隐式 `self`），否则报错。
- [ ] 被标注方法的返回值类型上必须存在恰好一个 `@iterator` 方法，否则报错。

### 2.3 `@iterator` 约束检查

- [ ] 被标注方法必须无参数（除隐式 `self`），否则报错。
- [ ] 返回值类型必须是具名元组类型，元素数量恰好为 2，第一元素类型为 `bool`，否则报错。（第二元素类型通过方法返回类型按需推导，无需单独记录。）

### 2.4 `for/in` 节点语义解析

- [ ] 对 `for/in` 的迭代表达式类型 `S`：
  - 若为 `T[]` / `T[!]`，走现有数组路径，跳过迭代器协议。
  - 否则查找 `S` 上的 `@iterable` 方法：找到则继续；未找到则查找 `@iterator` 方法（`S` 自身为游标）；均未找到则报错"类型不可迭代"。
- [ ] 确定循环变量类型（来自 `@iterator` 方法返回具名元组的第二元素）。
- [ ] 将展开信息（游标类型、`@iterable` 方法引用、`@iterator` 方法引用、循环变量类型）绑定到 `for/in` 节点，供 codegen 使用。
- [ ] 补齐本阶段测试用例并执行全量回归测试。

---

## 3 代码生成阶段

- [ ] **游标创建**：生成对 `@iterable` 方法的调用，结果绑定到栈上临时变量 `__cursor`，每个 `for/in` 节点恰好一次。若 `S` 自身为游标（无 `@iterable`），此步跳过，直接在 `expr` 上操作。
- [ ] **循环主体**：生成对 `__cursor`（或 `expr`）的 `@iterator` 方法调用，结果绑定到栈上临时变量 `__r`。
- [ ] **终止判断**：生成 `if !__r.item1 { break; }`。
- [ ] **循环变量绑定**：生成 `let it = __r.item2`，直接引用，不引入额外拷贝。
- [ ] **`@iterator` 内联**：游标类型静态已知时，将 `@iterator` 方法体直接内联到循环，不生成间接调用。
- [ ] 补齐本阶段测试用例并执行全量回归测试。

---

## 4 错误诊断

- [ ] `@iterable` 超过一个：`type X has multiple @iterable methods`。
- [ ] `@iterator` 超过一个：`type X has multiple @iterator methods`。
- [ ] 同时拥有两个注解：`type X cannot have both @iterable and @iterator`。
- [ ] `@iterable` 方法有参数：`@iterable method must take no parameters`。
- [ ] `@iterable` 返回类型无 `@iterator`：`return type of @iterable method has no @iterator method`。
- [ ] `@iterator` 方法有参数：`@iterator method must take no parameters`。
- [ ] `@iterator` 返回类型结构非法：`@iterator method must return a named tuple type of the form (bool, E)`。
- [ ] `for/in` 目标不可迭代：`type X is not iterable (no @iterable or @iterator method found)`。
- [ ] 补齐本阶段测试用例并执行全量回归测试。

---

## 5 测试

- [ ] `@iterable` / `@iterator` 基本功能：自定义 `Range` 容器 + `RangeCursor`，验证 `for/in` 输出正确。
- [ ] 自身即游标：`Counter` 直接带 `@iterator`，验证无 `@iterable` 时 `for/in` 正确工作。
- [ ] 嵌套迭代：两层 `for/in` 使用同一容器，验证游标独立，外层不影响内层。
- [ ] 错误诊断：对各约束违反情况分别写负向测试，验证报错信息。
- [ ] 与数组 `for/in` 共存：同一函数内既有数组 `for/in` 又有迭代器 `for/in`，验证两条路径不互扰。
- [ ] 泛型容器（基础）：泛型 `type` 上的 `@iterable` / `@iterator`，验证类型参数正确传播到循环变量。
- [ ] 跨包迭代：将带 `@iterable`/`@iterator` 的类型定义在独立包中，在另一个包内 `import` 后使用 `for/in`，验证注解元数据正确导出与识别。
- [ ] 执行全量回归测试，确认所有测试通过。

---

## 6 后续（暂不实现）

- 生成器 / `yield` 语义。
- 标准库 `Iterator<T>` 的 `filter`、`map`、`take` 等组合子实现（属于标准库任务，不在编译器本轮交付范围）。
