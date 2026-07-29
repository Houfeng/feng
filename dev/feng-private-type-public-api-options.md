# 公开成员使用私有类型的方案

> 状态：已选择 C# 方案

## 1. 共同底座

无论选择 C# 还是 Go 方案，都必须：

- 允许公开类型的私有成员使用私有类型。
- 将布局、泛型具化和 codegen 所需的私有依赖注册到编译器内部声明表。
- 私有依赖不进入用户名称查找、导入和补全结果。
- 类型引用按声明身份关联声明，不按名称猜测。

本修复采用 C# 方案；语言规则和 `.ft` 收录范围统一见
[修复方案第 2 节](./feng-private-representation-type-bugfix.md#2-语言规则)。

## 2. C# 方案

C# 示例：

```csharp
using System.Collections.Generic;

internal class Hidden {}

public static class API
{
    private static Hidden data;                 // 合法
    public static Hidden Value;                 // CS0052
    public static Hidden Create() => new();     // CS0050
    public static void Consume(Hidden value) {} // CS0051

    public static void Process<T>(T value)
        where T : Hidden {}                     // CS0703

    public static List<Hidden> Values;          // CS0052
}
```

Feng 采用该方案后的效果：

```feng
type Hidden {}

open let value: Hidden;                 // 错误
open func create() -> Hidden;           // 错误
open func consume(value: Hidden);       // 错误
open func process<T: Hidden>(value: T); // 错误：约束类型不可见
open let values: List<Hidden>;          // 错误：递归检查泛型实参

open type Public {
  seal let hidden: Hidden;              // 合法：私有成员
  let exposed: Hidden;                  // 错误：实例成员默认公开
  static let shared: Hidden;            // 错误：静态成员默认公开
}
```

规则：

- 声明签名的组成类型不得比声明本身更不可见。
- 私有依赖只供对象布局、泛型具化和 codegen 使用。
- 可见性不一致在提供方语义阶段报错。

额外实现：

- 按 module、所属类型和成员共同计算有效可见范围。
- 检查顶层绑定和函数，以及类型、`spec`、`fit` 的实例与静态成员。
- 检查字段、参数、返回类型、构造函数参数、泛型约束、父 `spec`、
  `spec` 组成类型和 `fit` 公开签名。
- 递归检查泛型实参、数组元素、指针目标及 callable 的参数和返回类型。
- 检查显式类型和推导类型；泛型参数本身不参与检查，其约束类型参与检查。
- 增加诊断码及对应测试。

## 3. Go 方案

提供方：

```go
package provider

type hidden struct {
    Value int
}

func (value hidden) GetValue() int {
    return value.Value
}

type API struct {
    privateValue hidden
    PublicValue  hidden
}

func Create() hidden {
    return hidden{Value: 42}
}

func Consume(value hidden) int {
    return value.GetValue()
}
```

消费方：

```go
value := provider.Create()
result := provider.Consume(value)
result = value.GetValue()
var explicit provider.hidden // 错误：不能显式引用未导出类型
```

该方案需要：

- 将公开 API 可达的私有类型及其公开成员签名纳入 `.ft` 和 API/ABI 事实。
- 允许不可命名类型参与推导、赋值、参数匹配、泛型推导、重载决议和 codegen。
- 允许通过推导值访问其公开成员。
- 保持显式类型引用、导入、构造和补全不可见。

## 4. 对比

| 项目 | C# 方案 | Go 方案 |
|---|---|---|
| 公开成员使用私有类型 | 提供方语义错误 | 允许 |
| 外部显式引用私有类型 | 不允许 | 不允许 |
| 外部通过推导使用私有类型 | 不存在该场景 | 允许 |
| 私有表示依赖的内部注册 | 必须 | 必须 |
| 公开 API 可达私有类型的导出 | 不需要，签名已被拒绝 | 必须 |
| 额外工作 | 集中的递归可见性检查 | 分散的不可命名类型外部使用 |

## 5. 选择

Feng 采用 C# 方案，原因如下：

- 与 `open` 表示外部可访问、三级可见性共同决定成员可达性的现有规则一致。
- 在提供方报告完整、稳定的诊断，不把错误延迟到消费方或 codegen。
- 公开 `.ft` 不需要承载不可命名类型的外部使用语义。
- 私有表示类型仍可支持公开泛型类型的布局和具化。
- 若未来需要不可命名但可用的类型，应单独设计显式 opaque type。
