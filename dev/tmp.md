
**明确遗留点**
- try/catch/finally 内的 return 未支持，codegen.c。
- 跨 try/catch/finally 的 break/continue 未支持，codegen.c。
- throw 非 managed 值未支持；当前只支持 managed payload，codegen.c。
- aggregate 类型作为泛型实参未支持，代码里明确写了缺 flatten rule，codegen.c 和 codegen.c。
- extern 模块级 binding 未支持，codegen.c。
