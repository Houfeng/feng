
**明确遗留点**
- try/catch/finally 内的 return 未支持，codegen.c。
- 跨 try/catch/finally 的 break/continue 未支持，codegen.c。
- throw 非 managed 值未支持；当前只支持 managed payload，codegen.c。