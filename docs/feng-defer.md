# feng defer 语法

这个文档只可用于描述 feng 语言 defer 语法，开发文档请参考 [dev/feng-defer-dev.md](../dev/feng-defer-dev.md)

```feng
func test() {
  let file = open("test.txt");
  defer { file.close(); } 
  ...// do something
}
```

- defer 语句块中不能包含 return 语句
- defer 语句块中不能包含 throw 语句
- defer 语句块中不能直接包含 break/continue 语句, 但在子块 for/while 中可以
- defer 在块作用域结束时执行
