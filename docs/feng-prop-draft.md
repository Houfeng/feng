# 属性

```feng
type UserType {

  //普通属性
  prop name {
    func get():string {}
    func set(value:string):void {}
  }

  //索引属性
  prop [key:int] {
    func get(): string {}
    func set(value:string):void {}
  }

}
```
