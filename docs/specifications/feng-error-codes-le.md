# LE - 词法错误

词法错误 token 的行列指向该非法词法结构的起始字符：字符串指向起始引号，块注释指向
起始 `/`，标识符和数字指向其首字符，注解指向 `@`，其他非法字符指向该字符本身。错误
token 的源码片段从上述起点延伸到 Lexer 已取得足够信息判定错误的位置；双引号字符串因
换行而未终止时，换行符不属于该错误 token。编译管线在每个源码文件的首个错误 token 处
停止该文件的词法处理，因此一个只包含一个非法词法结构的最小输入只报告一条词法诊断。

| 错误码 | 用途 | 错误文案 |
|--------|------|----------|
| **LE0001** | 无效的标识符 | reserved word cannot be used as an identifier in the current language version |
| **LE0002** | 无效的数字字面量 | invalid numeric literal |
| **LE0002** | 无效的数字字面量 | integer literal overflows u64 |
| **LE0003** | 字符串字面量未终止 | unterminated string literal |
| **LE0003** | 字符串字面量未终止 | unterminated raw string literal |
| **LE0004** | 无效的转义序列 | invalid \\x escape: expected 2 hex digits |
| **LE0004** | 无效的转义序列 | invalid \\x escape: expected hex digit |
| **LE0004** | 无效的转义序列 | invalid string escape |
| **LE0005** | 词法不完整 | expected annotation name after '@' |
| **LE0006** | 块注释未终止 | unterminated block comment |
| **LE0007** | 无效字符 | unexpected character |
