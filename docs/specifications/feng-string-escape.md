# Feng 字符串转义序列规范

## 概述

Feng 字符串字面量支持以下转义序列。所有转义在编译期由 codegen 解码为字节，运行时 `FengString` 存储的是已解析的 byte 数组。

## 支持的转义序列

| 转义 | 含义 | 说明 |
|------|------|------|
| `\\` | 反斜杠 `\` | 字面量反斜杠 |
| `\"` | 双引号 `"` | 字符串内嵌入双引号 |
| `\n` | 换行符 (0x0A) | LF |
| `\r` | 回车符 (0x0D) | CR |
| `\t` | 制表符 (0x09) | TAB |
| `\0` | 空字节 (0x00) | NUL |
| `\xNN` | 十六进制字节 | 恰好 2 位十六进制数字，表示一个字节 (0x00–0xFF) |

## `\xNN` 详细规则

### 语法

`\x` 后必须紧跟**恰好 2 位**十六进制数字（`0-9`, `a-f`, `A-F`）。

### 合法示例

```feng
"\x1b"            // ESC 字符 (0x1B)
"\x1b[32m"        // ESC + "[32m"，用于 ANSI 颜色
"\xe4\xb8\xad"    // UTF-8 编码的 "中"
"\xf0\x9f\x98\x80" // UTF-8 编码的 "😀"
"\x1b1"           // ESC (0x1B) + 普通字符 '1' (0x31)
```

### 非法示例与错误

| 写法 | 错误原因 |
|------|---------|
| `\x1` | 不足 2 位十六进制数字 |
| `\x` | 缺少十六进制数字 |
| `\xGG` | 包含非法十六进制字符 |
| `\x1b3` | 不报错；`\x1b` 合法，`3` 是普通字符 |

### 设计决策

- **限定恰好 2 位**：避免 C 语言贪婪消费多位十六进制的歧义，与 Rust、Go、Python、Swift 行为一致。
- **支持连续书写**：多个 `\xNN` 可无缝连接，每个独立解析为一个字节。
- **可表达所有 Unicode**：UTF-8 编码的任意 Unicode 字符均可通过多个 `\xNN` 组合表示。
- **编译期解码**：codegen 阶段将 `\xNN` 转换为对应字节值，运行时零开销。

## 不支持的转义序列

| 转义 | 状态 | 备注 |
|------|------|------|
| `\uXXXX` | 未支持 | 未来可扩展，需在 Lexer 和 Codegen 中增加 UTF-8 编码逻辑 |
| `\UXXXXXXXX` | 未支持 | 同上 |
| `\a`, `\b`, `\f`, `\v` | 未支持 | 如有需求可按相同模式扩展 |

## 实现位置

| 阶段 | 文件 | 职责 |
|------|------|------|
| Lexer | `src/lexer/lexer.c` | 验证转义语法合法性，拒绝非法格式 |
| Codegen | `src/codegen/codegen.c` | 将转义序列解码为字节，生成 C 字符串字面量 |
| Runtime | `src/runtime/feng_string.c` | `feng_string_literal()` 仅做 memcpy，不做任何转换 |

## Codegen 解码实现方案

### 当前流程

Codegen 在处理 `FENG_EXPR_STRING` 时（`src/codegen/codegen.c:10824-10862`），从 AST 节点获取原始 lexeme（含引号和未处理转义），通过一个解码循环将其转换为字节数组，再传给 `cg_string_literal_var()`。

当前解码循环的 switch 仅处理 `\\ \" \n \r \t \0`，遇到其他字符走 default 报错。

### `\xNN` 解码逻辑

在解码循环的 switch 中增加 `case 'x'` 分支：

```c
case 'x':
    /* \xNN: exactly 2 hex digits → one byte */
    if (i + 2 >= blen ||
        !is_hex_digit(body[i + 1]) ||
        !is_hex_digit(body[i + 2])) {
        free(decoded);
        return cg_fail(cg, e->token,
            "codegen: invalid \\x escape, expected 2 hex digits");
    }
    {
        unsigned int high = hex_value(body[i + 1]);
        unsigned int low  = hex_value(body[i + 2]);
        decoded[di++] = (char)((high << 4) | low);
        i += 2; /* skip the 2 hex digits (loop will ++i again) */
    }
    break;
```

### 辅助函数

需要两个静态辅助函数（放在解码循环之前）：

```c
/* 判断字符是否为十六进制数字 */
static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* 将十六进制字符转为数值 0-15 */
static unsigned int hex_value(char c) {
    if (c >= '0' && c <= '9') return (unsigned int)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (unsigned int)(c - 'A' + 10);
    return 0; /* unreachable if guarded by is_hex_digit */
}
```

### 关键点

- **Lexer 已验证语法**：codegen 中的检查是防御性编程，正常情况下不会触发。
- **编译期完成**：解码后的字节直接写入 `decoded` buffer，运行时零开销。
- **与现有转义一致**：遵循相同的 switch-case 模式，不引入额外抽象。
- **生成的 C 代码**：`cg_string_literal_var()` 将解码后的字节存入静态变量，`feng_string_literal()` 调用时传入的是已解析的 byte 数组。
DOCEOF; __aone_exit=$?; pwd -P > '/var/folders/_0/s4cd_7ln2lz9gtn1lyh5l8j40000gp/T/aone-copilot-cwd-1781272999498-ujjszhtlq4d.txt' 2>/dev/null; exit $__aone_exit