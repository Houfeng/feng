# 匿名 catch 原样重抛实现

语言规则以 [异常规范 §2](../specifications/feng-exception.md#2-throw-语句)为唯一来源。
本次按人工决策取消 `unknown` 关键字和专用 catch 绑定，具名 catch 与带值 throw 的行为保持不变。

## 实现范围

- C 编译器和标准库编译器词法、语法同步移除 `unknown` 关键字，恢复普通标识符解析。
- C 编译器的 `FENG_STMT_THROW` 节点允许空操作数，AST 文本输出保留 `throw;`；语义层按最近 catch 和 callable 边界检查作用域。标准库 Parser 尚未实现 throw 语句解析，本次仅同步其现有类型名称解析。
- 新增 `AE1407`，停止产生 `AE1403`、`AE1404`；诊断归属见[语义错误码规范](../specifications/feng-error-codes-ae.md#14-异常处理段)。
- Codegen 将合法的空操作数直接生成既有 `feng_rethrow()` 调用，移除 unknown 绑定及其识别分支。
- LSP、编辑器词法高亮与关键字集合保持一致。
- 内部未确定类型状态、分支结果推导、运行时私有 ABI 和异常生命周期不在本次变更范围内。

## 既有测试迁移清单

以下既有测试迁移已获人工批准，保留原有行为及资源检查：

- `test/lexer/test_lexer.c`、`std/std_test/src/test_lexer.ff`：unknown 改为普通标识符断言，关键字数量减一。
- `test/parser/test_parser.c`：unknown 兜底子句改为匿名子句，原有具体类型 catch 断言保留；原空 throw 的语法错误用例迁移为不完整带值 throw 的操作数错误，空 throw 的非法作用域由 Semantic 用例检查。
- `test/semantic/test_semantic.c`：旧 unknown 专用正反向规则迁移为匿名空 throw 正向、非法空 throw 作用域反向和未声明 unknown 类型的名称解析检查；保留兜底顺序和具体类型 throw 检查。
- `test/codegen/test_codegen.c`：两层 unknown 绑定重抛改为空 throw，保留原异常只分配一次的断言，并检查没有异常绑定读取。
- `test/smoke/phase1a/exception_try_expr.ff`：旧 unknown 重抛改为匿名空 throw，预期输出不变。
- `fcts/fcts_bin/src/test_exception.ff`、`test_g17_exception_semantics.ff`、`test_exception_payload.ff`：迁移原样重抛与兜底语法，保留原有类型、值、跨包及资源释放行为断言。

## 验证

- 新增编译器用例检查空 throw 的 AST、诊断及生成代码，覆盖普通子块、具名 catch 屏蔽和 Lambda 边界。
- 新增 FCTS 行为用例覆盖匿名重抛的传播、语句/表达式上下文和清理。
- 2026-09-12：实现与获批测试迁移完成后，在沙箱外执行完整 `make test`，退出码为 0；UBSan 与普通构建两个阶段均通过。
- 两个阶段各通过标准库 604 项、FCTS 1426 项，失败和跳过均为 0；编译器、CLI、符号表、smoke、项目构建、性能约束及 Makefile 指定的发布与工具链检查全部通过。
