# 属性

```feng
type UserType {

  //普通属性
  prop name {
    fn get():string {}
    fn set(value:string):void {}
  }

  //索引属性
  prop [key:int] {
    fn get(): string {}
    fn set(value:string):void {}
  }

}
```
