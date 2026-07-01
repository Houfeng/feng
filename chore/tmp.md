
**明确遗留点**
- LSDA/libunwind 后端已替换 try/catch 的 setjmp/longjmp 路径；剩余工作是继续收敛为最终零开销形态（减少正常路径 try registration/marker 开销）、补齐 Windows/SEH 后端设计，以及完成全量回归验证。