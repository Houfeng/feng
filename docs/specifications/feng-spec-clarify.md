# Feng 语言 Spec 澄清说明

## 在实现制层面

- **object-form spec** 本质上是接口，类似其他语言的 interface/trait
- **callable-form spec** 本质上是指针，类似 C# 中的委托 delegate
- **union-form spec** 本质上是 tagged+union，带 tag 的安全联合体

虽然在 Feng 中都用了 spec 这个关键字，但本质上是互不相同的概念和用途。

## 在面向用户层面

之所以都用 spec 关键字同，是为了表达在用户视角这些不是实体类型（相交于 type/enum），而是「契约」， 分别是：

- **ObjectSpec**, 对象契约（或称为类型契约），名义匹配，必须显式声明，内存布局相关，不能转换
- **CallableSpec**, 函数契约（或称为调用契约），未绑定时结构匹配，已绑定后相同时可显式转换
- **UnionSpec**, 联合契约（多个契约的组合），非名义匹配，面向用户表现为成员集合检查和匹配
