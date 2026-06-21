# 后续待处理任务

- [x] C ABI，库查找规则优化
- [X] C ABI，支持声明函数名称
- [ ] ARC 循环引用检查器优化，实现真正的无 STW
- [X] 部分关键字优化
- [X] callable-form spec，未绑定到 spec 时保持结构匹配，绑定到 spec 时优化为名义匹配
- [X] 支持联元组
- [x] 支持联合类型
- [x] 支持 enum
- [x] 修复 test() 参数直接传函数，报错
- [x] import 应该将导入符号到当前文件，而不是当前模块
- [ ] func 未声明返回值或返回值为 void，时必须无 `return` 语句或只能使用空 `return`，func 声明返回值，每个分支必须返回相同类型
- [ ] runtime api: feng_pointer_get_string(char * ptr, len) -1 读取到 \0
- [x] if-match ，类型匹配优化，比 if v { x:string {} else {} }
- [ ] if-match, 支持匹配 enum
- [ ] if 表达式、match 表达式、try 表达式，最后一句支持 throw
