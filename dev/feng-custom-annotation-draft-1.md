# Feng 自定义注解设计草案

> 本文记录自定义注解系统的设计思路、技术选型与实现方案。  
> **状态**：草案阶段，尚未实现。  
> **目标**：提供编译期元编程能力，支持在 std 中实现 @async/@await 等高级特性。

---

## 0 背景与目标

### 为什么需要自定义注解

Feng 计划引入异步机制（Future/Promise + 调度器），但在 async/await 的实现方式上存在选择：

- **语言级 async/await**：编译器内置支持，需要修改语法和语义分析
- **库级 @async/@await**：通过自定义注解在 std 中实现，编译器保持简单

选择库级方案的优势：
- 编译器复杂度低，不需要理解 async/await 语义
- 更灵活，用户可以自定义其他注解（@serializable、@orm 等）
- 符合 Feng 的"复杂类型在 std 中定义"的设计哲学

### 设计目标

1. **编译器简单**：不暴露复杂 AST 类型，注解系统对编译器透明
2. **灵活强大**：支持表达式级和语句级注解，可在编译期变换代码
3. **类型安全**：提供验证机制，确保注解输入输出符合预期
4. **易于使用**：std 提供解析/生成函数，用户无需直接操作底层结构

---

## 1 注解级别选择

### AST 注解 vs Token 注解

**Token 注解（词法级）**：
- 附加到 token 上，在解析阶段生效
- 可修改解析过程
- 操作低级 token 流

**AST 注解（语义级）**：
- 附加到 AST 节点上，在语义分析阶段生效
- 操作已解析的语法结构
- 可分析控制流、变量作用域

### 选择：AST 注解

对于 @async/@await 的实现，AST 注解更合适：

| 需求 | Token 注解 | AST 注解 |
|------|-----------|----------|
| 控制流分析（if/while/try） | 困难 | 自然 |
| 变量作用域分析 | 困难 | 自然 |
| 生成状态机代码 | 复杂 | 直观 |
| 与后续编译阶段集成 | 困难 | 容易 |

**理由**：@async/@await 的变换是语义级的，需要理解控制流和变量生命周期，在 token 级实现太底层、容易出错。

---

## 2 核心设计：多层数组作为注解接口

### 设计思路

**关键洞察**：编译器不需要"理解"复杂类型，只需要：
1. 将 AST 转换为简单的数据结构（多层数组）
2. 传递给注解函数
3. 接收变换后的数组
4. 转换回 AST 继续编译

这样编译器保持简单，复杂逻辑在 std 中实现。

### 数组表示

AST 节点表示为多层数组（类似 JSON / S-expression）：

```
// 函数定义
["func", "fetchData", [], ["Future", "string"], [
  ["let", "response", ["call", "httpClient.get", ["url"]]],
  ["return", ["field", "response", "body"]]
]]

// 带注解的函数
["@async", ["func", "fetchData", [], ["Future", "string"], [
  ["let", "response", ["@await", ["call", "httpClient.get", ["url"]]]],
  ["return", ["field", "response", "body"]]
]]]
```

### 数组结构设计

```
节点类型：
- ["func", name, params, returnType, body]
- ["let", name, initializer]
- ["var", name, initializer]
- ["return", expression]
- ["if", condition, thenBranch, elseBranch]
- ["while", condition, body]
- ["call", callee, args]
- ["field", object, fieldName]
- ["@annotationName", ...rest]  // 注解节点
- ... 其他节点类型
```

---

## 3 编译器实现

### 编译流程

```
源码
  ↓ 解析
AST（内部表示）
  ↓ 遇到注解
转换为多层数组
  ↓ 调用注解函数
接收变换后的数组
  ↓ 转换回 AST
继续编译（类型检查、优化、代码生成）
```

### 编译器职责

编译器只需要：

1. **AST ↔ 数组转换**
   - `astToArray(node: ASTNode): any[]`
   - `arrayToAst(arr: any[]): ASTNode`

2. **注解调度**
   - 遍历 AST，识别 `["@annotationName", ...]` 节点
   - 调用对应的注解函数，传入数组
   - 用返回值替换原节点

3. **错误处理**
   - 注解函数返回非法数组时，报告错误
   - 注解函数抛出异常时，捕获并报告

**编译器不需要理解注解的语义**，只是传递数据。

### 伪代码示例

```feng
// 编译器内部
func processAnnotations(ast: ASTNode): ASTNode {
  if ast is ArrayNode && ast[0] is String && ast[0].startsWith("@") {
    let annotationName = ast[0].substring(1);
    let annotationFunc = lookupAnnotation(annotationName);
    
    let inputArray = astToArray(ast);
    let outputArray = annotationFunc(inputArray);
    return arrayToAst(outputArray);
  }
  
  // 递归处理子节点
  for child in ast.children {
    child = processAnnotations(child);
  }
  return ast;
}
```

---

## 4 std 库实现

### 提供的解析/生成函数

```feng
// 解析函数
func parseFunction(arr: any[]): FunctionInfo;
func parseLet(arr: any[]): LetInfo;
func parseStatement(arr: any[]): Statement;
func parseExpression(arr: any[]): Expression;

// 生成函数
func generateFunction(info: FunctionInfo): any[];
func generateLet(info: LetInfo): any[];
func generateStateMachine(states: StateInfo[]): any[];

// 验证函数
func validateFunction(arr: any[]): bool;
func validateExpression(arr: any[]): bool;
```

### @async 注解实现示例

```feng
@annotation("async")
func asyncAnnotation(input: any[]): any[] {
  // 解析函数定义
  let funcInfo = parseFunction(input[1]);
  
  // 分析挂起点（@await）
  let suspensionPoints = findSuspensionPoints(funcInfo.body);
  
  // 分析需要捕获的变量
  let capturedVars = analyzeCapturedVars(funcInfo.body, suspensionPoints);
  
  // 生成状态机
  let states = generateStates(funcInfo.body, suspensionPoints, capturedVars);
  let stateMachineCode = generateStateMachine(states);
  
  // 包装为 Future
  let wrappedBody = [
    "return", ["call", "Future", [
      ["lambda", ["resolve", "reject"], stateMachineCode]
    ]]
  ];
  
  // 返回变换后的函数
  return generateFunction({
    name: funcInfo.name,
    params: funcInfo.params,
    returnType: funcInfo.returnType,
    body: [wrappedBody]
  });
}
```

### @await 的实现

@await 不需要单独的注解函数，它在 @async 的处理过程中被识别和变换：

```feng
func findSuspensionPoints(body: any[][]): SuspensionPoint[] {
  let points = [];
  for stmt in body {
    if containsAwait(stmt) {
      points.push({
        location: stmt,
        awaitExpr: extractAwaitExpr(stmt)
      });
    }
  }
  return points;
}

func containsAwait(node: any[]): bool {
  if node[0] == "@await" return true;
  for child in node {
    if child is any[] && containsAwait(child) return true;
  }
  return false;
}
```

---

## 5 @async/@await 变换示例

### 源码

```feng
@async
func fetchData(): Future<string> {
  let response = @await httpClient.get(url);
  return response.body;
}
```

### 输入数组（传给 @async）

```
["@async", ["func", "fetchData", [], ["Future", "string"], [
  ["let", "response", ["@await", ["call", "httpClient.get", ["url"]]]],
  ["return", ["field", "response", "body"]]
]]]
```

### 输出数组（@async 返回）

```
["func", "fetchData", [], ["Future", "string"], [
  ["return", ["call", "Future", [
    ["lambda", ["resolve", "reject"], [
      ["var", "state", 0],
      ["var", "captured_response", ["none"]],
      ["while", ["true"], [
        ["match", "state", [
          [0, [
            ["let", "future", ["call", "httpClient.get", ["url"]]],
            ["set", "state", 1],
            ["call", ["field", "future", "then"], [
              ["lambda", ["response"], [
                ["set", "captured_response", "response"],
                ["call", ["field", "scheduler", "resume"], []]
              ]]
            ]],
            ["return"]
          ]],
          [1, [
            ["call", "resolve", [["field", "captured_response", "body"]]],
            ["return"]
          ]]
        ]]
      ]]
    ]]
  ]]]
]]
```

### 变换要点

1. **识别挂起点**：`@await` 标记的位置
2. **捕获变量**：`response` 跨越挂起点，需要捕获
3. **状态机**：
   - 状态 0：执行到 `@await`，注册回调，挂起
   - 状态 1：回调触发，继续执行
4. **包装为 Future**：返回 Future，内部是状态机

---

## 6 其他注解示例

### @serializable

```feng
@serializable
type User {
  name: string;
  age: int;
}
```

变换：自动生成 `toJson()` 和 `fromJson()` 方法。

### @memoize

```feng
@memoize
func expensiveCalc(x: int): int {
  // ...
}
```

变换：自动添加缓存逻辑。

### @deprecated

```feng
@deprecated("use newFunc instead")
func oldFunc(): void {
  // ...
}
```

变换：编译期警告，不影响代码。

---

## 7 实现计划

### 阶段 1：基础注解系统

- [ ] 设计数组表示格式（完整规范）
- [ ] 实现 AST ↔ 数组转换
- [ ] 实现注解调度机制
- [ ] 支持表达式级和语句级注解

### 阶段 2：std 解析/生成库

- [ ] 实现 `parseFunction`、`parseStatement` 等解析函数
- [ ] 实现 `generateFunction`、`generateLet` 等生成函数
- [ ] 实现验证函数

### 阶段 3：@async/@await 实现

- [ ] 实现挂起点分析
- [ ] 实现变量捕获分析
- [ ] 实现状态机生成
- [ ] 测试各种控制流（if/while/try-catch）

### 阶段 4：其他注解

- [ ] @serializable
- [ ] @memoize
- [ ] @deprecated
- [ ] 用户自定义注解

---

## 8 潜在挑战与解决方案

### 挑战 1：错误信息

**问题**：数组结构错误时，错误信息可能不够直观。

**解决方案**：
- std 提供验证函数，在注解函数内部验证
- 提供友好的错误消息：`"expected function definition, got: ..."`
- 保留源码位置信息，在数组中包含 `["@pos", line, col, ...]`

### 挑战 2：类型安全

**问题**：数组没有静态类型检查，运行时才发现问题。

**解决方案**：
- std 提供类型检查函数：`validateFunction(arr): bool`
- 注解函数内部先验证，再处理
- 未来可考虑添加可选的类型注解

### 挑战 3：性能

**问题**：数组解析/生成可能有序列化开销。

**解决方案**：
- 编译器内部用高效表示，只在注解边界转换为数组
- 注解函数结果可缓存（相同输入返回相同输出）
- 对于性能关键的注解，可用 C 实现核心逻辑

### 挑战 4：调试困难

**问题**：注解变换后的代码难以调试。

**解决方案**：
- 编译器提供 `--dump-annotated` 选项，输出变换后的代码
- 保留源码位置映射，调试器可定位到原始代码
- 提供注解展开的可视化工具

---

## 9 与其他语言的对比

| 语言 | 注解机制 | async/await 实现 | 备注 |
|------|---------|------------------|------|
| Rust | proc macros（token 流） | 语言级 | token 级太复杂，最终选择语言级 |
| Scala | macros（AST） | 可用宏实现 | AST 级更自然 |
| C++20 | 编译器内置 | 语言级 | 编译器直接变换 AST |
| Kotlin | 语言级 suspend | 语言级 | async/await 是库函数 |
| Lisp | 宏（S-expression） | N/A | 最灵活的元编程 |
| **Feng（计划）** | **自定义注解（数组）** | **库级** | **编译器简单，灵活强大** |

---

## 10 与异步机制的关系

### 异步机制路线图

```
1. 基础层：Future/Promise（当前）
   - 与注解系统无关
   - 是所有异步方案的基础

2. 注解系统：自定义注解（中期）
   - 提供编译期元编程能力
   - 不特定于异步

3. 异步语法：@async/@await（后期）
   - 基于注解系统实现
   - 作为库特性，非语言级
```

### 有栈协程 vs 无栈协程

如果选择**有栈协程**：
- 不需要 @async/@await
- 阻塞操作自动挂起协程
- 注解系统可用于其他目的（@serializable 等）

如果选择**无栈协程**：
- 需要 @async/@await
- 注解系统用于实现状态机变换
- 编译器保持简单

**结论**：注解系统无论选择哪种协程方案都有价值。

---

## 11 开放问题

1. **数组格式是否需要版本控制？**
   - 随着语言演进，AST 结构可能变化
   - 可能需要版本号：`["v1", "func", ...]`

2. **注解是否可以链式调用？**
   - `@a @b func foo()` 的执行顺序
   - 是 `a(b(foo))` 还是 `b(a(foo))`

3. **注解是否可以访问类型信息？**
   - 当前设计：注解在类型检查之前执行
   - 如果需要类型信息，需要调整执行时机

4. **注解是否可以生成新文件？**
   - 例如 @serializable 生成独立的序列化代码文件
   - 需要扩展编译器接口

5. **注解的错误如何定位到源码？**
   - 需要在数组中保留位置信息
   - 错误消息需要映射回原始代码

---

## 12 参考资料

- **Lisp 宏**：S-expression 元编程的经典案例
- **Rust proc macros**：token 级宏系统，功能强大但复杂
- **Scala macros**：AST 级宏系统，更接近 Feng 的设计
- **C++20 coroutines**：编译器内置的状态机变换
- **Kotlin coroutines**：suspend 关键字 + 库实现

---

## 13 下一步

1. **完善数组格式规范**：定义完整的 AST 节点类型和数组表示
2. **实现原型**：在编译器中添加基础的注解调度机制
3. **实现简单注解**：先实现 @deprecated 等简单注解验证设计
4. **实现 @async/@await**：作为复杂注解的验证案例
5. **收集反馈**：根据实际使用情况调整设计
