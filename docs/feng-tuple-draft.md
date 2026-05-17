# 元组

```feng
spec MyTuple(int, char*);
spec OrderPair(int, char*);

// 此时 (1, "str") 是匿名的。只要结构对齐，它既能去 MyTuple，也能去 OrderPair
let a : MyTuple   = (1, "str"); // ✅ 结构匹配，成功绑定
let b : OrderPair = (1, "str"); // ✅ 结构匹配，成功绑定

let a: MyTuple   =(MyTuple)b;   // ✅ 具名元组，需要显式转换

let (x, y) = a; //支持解构

let item1 = a.item1; //支持成员访问
```
