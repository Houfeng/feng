# 公开成员使用私有类型的方案

> 状态：待决策

## 1. 共同底座

无论选择 C# 还是 Go 方案，都必须：

- 允许公开类型的私有成员使用私有类型。
- 从公开声明递归收集其依赖的私有类型闭包。
- 将闭包写入公开 `.ft`，并保留私有标记。
- 将私有依赖注册到编译器内部声明表，但不加入用户名称查找、导入和补全结果。
- 类型引用按符号身份关联声明，不按名称猜测。
- 递归处理泛型实参和约束、数组元素、指针目标及字段类型。
- 只导出可达闭包，不导出无关私有声明。

私有泛型类型即使只用于私有字段，也必须导出声明骨架、字段布局、约束和 reifiable 依赖，供跨包具化和 codegen 使用。

## 2. C# 方案

```csharp
public class API
{
    private sealed class Hidden {}

    private Hidden data;          // 合法：私有成员使用私有类型
    public Hidden Value;          // CS0052：字段类型可见性不足
    public Hidden Create() => new Hidden(); // CS0050：返回类型可见性不足
}
```

规则：

- 公开成员使用可见性更低的类型时，语义阶段报可见性不一致。
- 私有依赖只供对象布局、泛型具化和 codegen 使用。

额外实现：

- 递归检查字段类型、方法参数与返回类型、构造函数参数和泛型约束的可见性。
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
    privateValue hidden // 合法：未导出字段使用未导出类型
    PublicValue  hidden // 合法：导出字段使用未导出类型
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
value := provider.Create()                    // 合法：通过推导获得 hidden
result := provider.Consume(value)             // 合法：传递 hidden
api := provider.API{PublicValue: value}       // 合法：写入导出字段
result = api.PublicValue.GetValue()           // 合法：访问公开方法
var explicit provider.hidden                  // 错误：不能引用未导出类型名
```

规则：

- 公开成员可以使用私有类型。
- 外部代码可以通过类型推导获得、传递该类型，并访问其公开成员。
- 外部代码不能显式引用、导入或构造该私有类型。

额外实现：

- 支持不可命名类型的推导、传递、公开成员访问和 codegen。
- 导出该私有类型供外部使用所需的公开成员签名。
- 将公开 API 可达的私有类型事实纳入 API/ABI 兼容性判断。

## 4. 对比

| 项目 | C# 方案 | Go 方案 |
|---|---|---|
| 公开成员使用私有类型 | 语义错误 | 允许 |
| 外部显式引用私有类型 | 不允许 | 不允许 |
| 外部通过推导使用私有类型 | 不存在该场景 | 允许 |
| 私有依赖闭包导出 | 必须 | 必须 |
| 额外工作 | 可见性一致性检查 | 不可命名类型的外部使用 |

两种方案共用相同的符号表和 codegen 底座。核心选择是：公开签名遇到私有类型时立即报错，还是允许该类型以不可命名形式流动。
