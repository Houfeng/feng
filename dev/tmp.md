
**明确遗留点**
- 类型字段默认初始化器未支持，codegen.c。
- 裸 lambda 作为普通表达式值仍未支持。现在只有 callable coercion 路径会专门处理 lambda，codegen.c；通用表达式分派缺少对应分支，会落到兜底报错，codegen.c。
- 空数组字面量仍未支持，codegen.c。
- try/catch/finally 内的 return 未支持，codegen.c。
- 跨 try/catch/finally 的 break/continue 未支持，codegen.c。
- throw 非 managed 值未支持；当前只支持 managed payload，codegen.c。
- match 语句的 codegen 未实现。AST 里有 match 语句种类，parser.h；但语句发射 switch 没有对应分支，最终会走 not yet supported，codegen.c。
- aggregate 类型作为泛型实参未支持，代码里明确写了缺 flatten rule，codegen.c 和 codegen.c。
- extern 模块级 binding 未支持，codegen.c。

**疑似残留**
- options.c 的注释说直编模式接受 lib 只是为了后续给出“尚不支持”的诊断，但我没找到实际拒绝路径；相反，直编流程看起来已经会生成 lib 输出目录和库名，direct.c 和 direct.c。这更像注释残留，不像真实缺口。
- codegen.c 只处理 spec 的 field/method 成员；如果规范允许 spec 出现 constructor/finalizer，那这里也是缺口。语法层的成员种类确实包含它们，parser.h，但我这次没有继续追到“规范是否允许 spec 使用这些成员”。
