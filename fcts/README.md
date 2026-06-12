# Feng Conformance Test Suite

Feng 语言一致性测试集，用于验证 Feng 语言的实现是否与标准一致。

- 测试集由 fcts_bin 和 fcts_lib 两个项目组成，用于保障跨包正确性
- 测试用例通常写在 fcts_bin 项目中，包括 feng 的所有语法特性
- 需要跨包验证时，部分定义可写在 fcts_lib 项目中，然后由 fcts_bin 项目引用进行验证
- 测试用例由 feng 语言自身编写，通过 feng run 执行
- 泛型一致性测试应覆盖泛型函数、泛型类型、泛型方法、泛型约束、泛型 spec、泛型 fit、泛型 union、泛型数组创建、嵌套泛型默认初始化、泛型对象初始化、spec 作为泛型实参、泛型聚合返回、父级约束转发、具体化父泛型 spec，以及跨包泛型使用
