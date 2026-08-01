# 代码格式化

Feng CLI 当前不提供 `feng fmt`。生产可用的格式化入口由 Feng VS Code 插件提供。

## 使用方式

安装 Feng Language 插件后，在 Feng 源文件或 `feng.fm` 中执行 VS Code 的 **Format Document**。也可以启用保存时格式化：

```json
{
  "[feng]": {
    "editor.formatOnSave": true
  },
  "[feng-manifest]": {
    "editor.formatOnSave": true
  }
}
```

## 当前格式化范围

Feng 源文件格式化器处理：

- 花括号、圆括号和方括号周围的缩进。
- 行尾空白和换行符。
- 二元、复合赋值和移位运算符的空格。
- 参数、实参、类型标注、逗号和对象字面量的常用间距。

清单格式化器处理：

- `[section]` 节标题。
- `#` 注释空格。
- 同一节内 `key: "value"` 的值列对齐。

格式化器不会自动重排复杂表达式，也不替代语义检查。格式化后仍应运行：

```bash
feng check
```

团队项目应统一使用同一插件版本，并配合[Feng 代码风格建议](../feng-style.md)约定格式化器未覆盖的命名和结构选择。
