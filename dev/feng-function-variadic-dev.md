# Feng 变长参数实现开发文档

> 规范来源：[docs/feng-function-variadic.md](../docs/feng-function-variadic.md)  
> 本文记录实现原理、关键代码位置与待办任务。

---

## 0 实现原理

### 核心思路：规范化为只读数组 + 调用侧打包

变长参数不引入新的运行时机制。整体思路是：

1. **Parser 侧**：`T...` 是语法糖。解析后立即将参数类型规范化为 `T[]`（`FengTypeRef kind=ARRAY, element_writable=false, inner=T`），并在 `FengParameter` 上记录 `is_variadic=true` 标记。此后所有阶段看到的参数类型均为普通只读数组，无需理解 `T...` 语法。
2. **Semantic 侧**：函数体内 `args` 类型即为 `T[]`，所有数组操作原生合法，无需特判。调用侧匹配时，检查 `is_variadic` 标记，放宽参数计数下界，并逐一对变参位实参匹配元素类型 `T`（从 `T[]` 取 `inner`）。
3. **Codegen 侧**：在 `cg_emit_call()` 中，若被调用方最后一个参数 `is_variadic`，则将变参位所有实参收集起来，复用现有 `cg_emit_array_literal_typed()` 生成 `T[]`，再作为最后一个参数传入。零实参时生成空数组（`feng_array_new(... count=0)`）。函数体 codegen 不需要任何改动。

### 关键数据结构

```c
// src/parser/parser.h  line ~65-70
typedef struct FengParameter {
    FengToken       token;
    FengMutability  mutability;    // LET | VAR | DEFAULT
    FengSlice       name;
    FengTypeRef    *type;          // 变参时存储的是 T[]（已规范化）
    bool            is_variadic;   // 新增：是否是变参参数
} FengParameter;
```

`FengCallableSignature`（line ~351-358）中已有 `FengParameter *params` 与 `size_t param_count`，无需新增字段；变参信息通过 `params[param_count-1].is_variadic` 读取。

### 调用侧打包伪代码（Codegen）

```c
// cg_emit_call() 中变参处理（新增逻辑）

bool has_variadic = (sig->param_count > 0) &&
                    sig->params[sig->param_count - 1].is_variadic;
size_t fixed_count = has_variadic ? sig->param_count - 1 : sig->param_count;

// 生成固定参数（不变）
for (size_t i = 0; i < fixed_count; i++) {
    emit_arg(args[i]);
}

// 生成变参打包数组
if (has_variadic) {
    size_t variadic_count = arg_count - fixed_count;  // 可为 0
    FengTypeRef *elem_type = sig->params[sig->param_count - 1].type->as.inner; // T[] → T
    // 复用 cg_emit_array_literal_typed(elem_type, &args[fixed_count], variadic_count)
    // 生成 FengArray* 并作为最后一个参数传递
}
```

### ARC 生命周期

打包产生的临时 `FengArray*` 在调用点前 retain，调用返回后 release。现有 ARC 对函数调用参数的托管机制应能自然覆盖此场景；**在 codegen 实现时需确认**：若现有机制不覆盖临时数组的 release，则需在调用点后显式插入 release 代码。

### 重载冲突与消歧

按规范，声明侧必须先严格检测非变参重载与变参重载之间是否存在参数类型重叠。若存在某个实参序列能同时匹配两者，则在声明处直接报冲突错误，不依赖“普通重载优先于变参重载”的调用侧优先级规则消歧。仅当声明侧冲突检测通过后，调用侧才按唯一匹配继续解析。

---

## 1 Todo List

### Phase 1：文档

- [x] **V1**：新建 `docs/feng-function-variadic.md`，完整描述变参规范（语法、语义、规则、编译期、运行时）
- [x] **V2**：在 `docs/feng-function.md` § 8 关联章节增加对 `feng-function-variadic.md` 的引用链接

### Phase 2：Parser

- [x] **P1**：`src/parser/parser.h`（line ~65-70）  
  在 `FengParameter` 结构体中添加 `bool is_variadic` 字段。

- [x] **P2**：`src/parser/parser.c`，`parse_parameters()`（line ~616-680）  
  **目标**：识别 `name: T...` 语法。  
  实现要点：
  - 解析完类型 `T` 后，检查下一个 token 是否为 `...`（三点号）；若是，消耗该 token，设 `param.is_variadic = true`，并将参数类型从 `T` 替换为 `T[]`（构造 `FengTypeRef{kind=ARRAY, element_writable=false, inner=T}`）。
  - 解析完一个 `is_variadic=true` 的参数后，若下一个 token 是逗号（而非 `)`），立即报编译期错误："变长参数必须是最后一个参数"，阻止通过。
  - 不允许多个变参；结合“变参必须位于最后一个参数位置”的规则，多个变参声明同样必须在编译期被拒绝。

- [x] **P3**：`src/parser/parser.c`，`parse_callable_signature()`（line ~744-760）  
  **目标**：将 `is_variadic` 信息透传，使 `spec` 声明同样支持变参。  
  `spec` 声明与 `fn` 走同一参数解析路径（`parse_parameters()`），P2 完成后此处应自动获得支持；确认 `FengCallableSignature` 的构建路径正确保留 `is_variadic` 即可。  
  同时在此处对 `extern fn` 检查：若 `extern fn` 的参数含 `is_variadic=true`，报错并阻止通过。

### Phase 3：Semantic

- [x] **S1**：`src/semantic/analyzer.c`，`function_type_parameters_match_args()`（line ~7890-7960）  
  **目标**：调用侧参数匹配，支持变参计数松弛与元素类型逐一检查。  
  实现要点：
  - 检测最后一个参数是否 `is_variadic`；若是，放宽计数检查为 `arg_count >= param_count - 1`（原来是精确相等）。
  - 对变参位的每个实参（索引 `fixed_count` 到 `arg_count-1`），单独调用 `expr_matches_expected_type_ref()` 检查其是否匹配变参元素类型（即 `params[last].type->as.inner`，即 `T[]` 的 inner `T`）。
  - 零实参时（`arg_count == param_count - 1`）：变参位合法，无需额外检查。
  - 检测"已有 `T[]` 直接传入变参位"：若 `arg_count == param_count` 且第 `last` 个实参的类型是 `T[]`（数组类型），而参数要求的是变参（`is_variadic`），报错："变参位不接受已有数组，请逐个传入元素"，阻止通过。

- [x] **S2**：`src/semantic/analyzer.c`，`callable_parameters_match_args()`（line ~7000-7215）  
  **目标**：通过 callable-form spec 值调用时同样支持变参匹配。  
  此函数最终调用 S1 中的 `function_type_parameters_match_args()`，确认调用链路完整即可；若有独立计数检查需同步修改。

- [x] **S3**：`src/semantic/analyzer.c`，spec-fn 结构匹配路径  
  **目标**：未绑定函数或 lambda 进入变参 callable-form spec 位置时，变参标志必须一致。  
  实现要点：在结构匹配阶段，逐个参数对比 `is_variadic` 标记；`fn(args: T[])` 不匹配 `spec S(args: T...)`，反之亦然；两侧变参标志不一致时报错。

- [x] **S4**：`src/semantic/analyzer.c`，重载冲突检测路径  
  **目标**：声明侧检测含变参重载之间的参数类型重叠。  
  实现要点：在同名重载注册阶段，对新增重载与已有同名重载逐对执行以下算法：  
  令 `nf` = 新重载固定参数数，`ng` = 已有重载固定参数数。  
  - `nf < ng` 且新重载为变参：用元素类型 `T` 将新重载固定列表填充至 `ng`，逐位比较；一致则冲突。  
  - `nf > ng` 且新重载为变参：长度不等，跳过（不冲突）。  
  - `nf == ng`：逐位比较固定参数列表；一致则冲突。  
  对已有重载为变参的情形，对称执行上述逻辑。冲突时报声明冲突错误，阻止注册。

### Phase 4：Codegen

- [x] **C1**：`src/codegen/codegen.c`，`cg_emit_call()`（line ~10730-10850）  
  **目标**：调用侧变参自动打包。  
  实现要点：
  - 获取被调用方签名，检查最后参数是否 `is_variadic`。
  - 分割实参：`args[0..fixed_count-1]` 正常生成，`args[fixed_count..arg_count-1]` 收集为变参实参列表。
  - 调用 `cg_emit_array_literal_typed(elem_type, variadic_args, variadic_count)` 生成临时 `FengArray*`（其中 `elem_type` 为变参元素类型 `T`）；零实参时传入 `variadic_count=0`。
  - 将生成的 `FengArray*` 作为最后一个参数传递。
  - 确认 ARC 处理：临时数组的 retain/release 是否由现有机制覆盖；若不覆盖，在调用返回后插入显式 release。

- [x] **C2**：`src/codegen/codegen.c`，通过 spec 值调用路径  
  **目标**：通过 callable-form spec 值（如 `let s: Logger = ...`）发起变参调用时，同样执行打包。  
  spec 值调用走 indirect call 路径；检查该路径中是否读取 spec 的 `is_variadic`，并在变参位执行与 C1 相同的打包逻辑。

### Phase 5：测试

- [x] **T1**：仅含变参参数的函数，以 0/1/N 实参调用
  ```feng
  fn sum(args: int...): int { ... }
  sum()          // 合法，args=[]
  sum(1)         // 合法，args=[1]
  sum(1, 2, 3)   // 合法，args=[1,2,3]
  ```

- [x] **T2**：固定参数 + 变参参数
  ```feng
  fn log(level: int, args: string...): void { ... }
  log(0)             // 合法，args=[]
  log(1, "a", "b")   // 合法，args=["a","b"]
  ```

- [x] **T3**：变参位类型不匹配 → 编译期报错
  ```feng
  fn f(args: int...): void { }
  f("bad")   // 错误：string 不匹配 int
  ```

- [x] **T4**：变参不在最后 → 编译期报错
  ```feng
  fn bad(args: int..., x: int): void { }   // 错误
  ```

- [x] **T5**：将已有数组传入变参位 → 编译期报错
  ```feng
  let arr: int[] = [1, 2];
  fn f(args: int...): void { }
  f(arr)   // 错误：变参位不接受已有数组
  ```

- [x] **T6**：callable-form spec 变参声明，lambda 实现，通过 spec 值调用
  ```feng
  spec Printer(args: string...): void;
  let p: Printer = (args: string...) { ... };
  p("hello", "world")   // 合法，自动打包
  ```

- [x] **T7**：重载冲突验证
  ```feng
  fn f(x: int): void { }
  fn f(args: int...): void { }
  // 错误：声明冲突；`(int)` 落入变参的等效展开范围
  ```

- [x] **T8**：全量回归（`make test` 或对应回归命令），确保无现有用例破坏。

---

## 2 关键文件速查

| 阶段 | 文件 | 位置 | 改动摘要 |
|------|------|------|---------|
| Parser 结构 | `src/parser/parser.h` | `FengParameter`（line ~65） | 新增 `bool is_variadic` |
| Parser 解析 | `src/parser/parser.c` | `parse_parameters()`（line ~616） | 识别 `T...`，规范化为 `T[]`，验证位置 |
| Parser 传播 | `src/parser/parser.c` | `parse_callable_signature()`（line ~744） | 确认 spec/extern 路径，extern fn 拒绝 |
| Semantic 匹配 | `src/semantic/analyzer.c` | `function_type_parameters_match_args()`（line ~7890） | 计数松弛 + 元素类型逐一检查 |
| Semantic 间接 | `src/semantic/analyzer.c` | `callable_parameters_match_args()`（line ~7000） | spec 值调用路径同步 |
| Semantic 结构 | `src/semantic/analyzer.c` | 结构匹配路径 | 变参标志一致性检查 |
| Semantic 重载 | `src/semantic/analyzer.c` | 重载冲突检测路径 | 声明侧检测变参与非变参参数重叠并拒绝冲突 |
| Codegen 调用 | `src/codegen/codegen.c` | `cg_emit_call()`（line ~10730） | 变参实参打包为 `T[]` |
| Codegen 参考 | `src/codegen/codegen.c` | `cg_emit_array_literal_typed()`（line ~11934） | 复用现有数组字面量生成 |
| Codegen spec | `src/codegen/codegen.c` | spec 值 indirect call 路径 | 同样执行打包 |
