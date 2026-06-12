---
name: stdio_overload_preserve_existing
description: 修改 stdio 或格式化 API 时必须保留已有重载，只能按用户要求在尾部追加新重载
type: feedback
createdAt: 2026-06-12T15:14:27
---
修改 `stdio`、`print`、`format` 等重载时，不要替换、移动或改写已有函数；需要新增能力时，只能按用户明确要求追加新重载，并放在相关文件尾部。不得因为实现方便自行新增未要求的 API 形态，例如 `print(string[])` 或 `first: Display`。

Why: 用户明确纠正“不要改原来的 args: string[] 和 args: string... 两个函数，增加面向 Display 即可”，随后再次纠正“为什么需要 first: Display？没有我的允许，禁止自作主张；在 stdio 尾部增加，不要插在 print string 函数中间”，并指出删除 `format` 不应引入额外 `print(string[])` 变更。
How to apply: 未来涉及标准库格式化/输出 API 调整时，先保持现有重载的签名、顺序和语义不变；新增 Display 或其他类型重载必须放在文件尾部；任何额外 API 形态都必须先征得用户确认。