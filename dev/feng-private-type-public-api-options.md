# 公开成员使用私有类型的方案

> 状态：已选择 Go 方案

## 1. 共同底座

无论选择 C# 还是 Go 方案，都必须：

- 允许公开类型的私有成员使用私有类型。
- 将私有依赖注册到编译器内部声明表，但不加入用户名称查找、导入和补全结果。
- 类型引用按符号身份关联声明，不按名称猜测。

本修复采用 Go 方案；类型声明的 `.ft` 收录范围统一见
[修复方案第 2 节](./feng-private-representation-type-bugfix.md#2-语言规则)。

## 2. C# 方案

```csharp
using System.Collections.Generic;

internal class Hidden {}

public static class API
{
    private static Hidden data;                // 合法：私有成员使用私有类型
    public static Hidden Value;                // CS0052：字段类型可见性不足
    public static Hidden Create() => new();    // CS0050：返回类型可见性不足
    public static void Consume(Hidden value) {} // CS0051：参数类型可见性不足

    public static void Process<T>(T value)
        where T : Hidden {}                    // CS0703：约束类型可见性不足

    public static List<Hidden> Values;         // CS0052：泛型实参可见性不足
}
```

规则：

- 公开成员使用可见性更低的类型时，语义阶段报可见性不一致。
- 私有依赖只供对象布局、泛型具化和 codegen 使用。

额外实现：

- 检查全部公开声明的组成类型，包括实例与静态字段、顶层绑定、函数与方法的参数和返回类型、构造函数参数、泛型约束、父 `spec`、`spec` 组成类型和 `fit` 公开签名。
- 递归检查泛型实参、数组元素、指针目标及 callable 的参数和返回类型。
- 检查显式类型和推导类型；按 module、所属 type 和成员共同计算有效可见范围。
- 泛型参数本身不参与检查，其约束类型参与检查。
- 增加诊断码及对应测试。

## 3. Go 方案

提供方：

```go
package provider

type hidden struct {
    Value int
}

func (hidden) privateMarker() {}

func (value hidden) GetValue() int {
    return value.Value
}

type hiddenConstraint interface {
    privateMarker()
}

type Box[T any] struct {
    Value T
}

type API struct {
    privateValue hidden // 合法：未导出字段使用未导出类型
    PublicValue  hidden // 合法：导出字段使用未导出类型
}

var Value hidden
var Values Box[hidden]

func Create() hidden {
    return hidden{Value: 42}
}

func Consume(value hidden) int {
    return value.GetValue()
}

func Process[T hiddenConstraint](value T) {}
```

消费方：

```go
value := provider.Create()                    // 合法：通过推导获得 hidden
result := provider.Consume(value)             // 合法：传递 hidden
provider.Process(value)                       // 合法：推导满足未导出约束
api := provider.API{PublicValue: value}       // 合法：写入导出字段
result = api.PublicValue.GetValue()           // 合法：访问公开方法
result = provider.Value.GetValue()            // 合法：读取导出变量
result = provider.Values.Value.GetValue()     // 合法：使用嵌套泛型实参
var explicit provider.hidden                  // 错误：不能引用未导出类型名
```

规则：

- 公开成员可以使用私有类型。
- 外部代码可以通过类型推导获得、传递该类型，并访问其公开成员。
- 外部代码不能显式引用、导入或构造该私有类型。

额外实现：

- 允许不可命名类型参与类型推导、赋值、参数匹配、泛型推导、重载决议和 codegen。
- 允许通过该类型的值访问其公开成员。
- 导出该私有类型供外部使用所需的公开成员签名。
- 将公开 API 可达的私有类型事实纳入 API/ABI 兼容性判断。
- 用户名称查找、`use`、显式类型引用和模块补全仍不得返回该类型。

仅将私有类型写入 `.ft` 不足以完成本方案；语义分析和 codegen 必须按声明身份解析该类型。

## 4. 对比

| 项目 | C# 方案 | Go 方案 |
|---|---|---|
| 公开成员使用私有类型 | 语义错误 | 允许 |
| 外部显式引用私有类型 | 不允许 | 不允许 |
| 外部通过推导使用私有类型 | 不存在该场景 | 允许 |
| 方案所需的私有依赖导出 | 必须 | 必须 |
| 额外工作 | 可见性一致性检查 | 不可命名类型的外部使用 |

两种方案共用相同的符号表和 codegen 底座。核心选择是：公开签名遇到私有类型时立即报错，还是允许该类型以不可命名形式流动。
