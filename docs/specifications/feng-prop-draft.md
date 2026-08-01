# Feng 语言属性规范

> **状态**: 草案。
> 本文档定义属性的语法、语义与编译期约束; 属性是方法调用的语法糖,不引入新的存储或语义层级。

本文档补充 [feng-language.md](./feng-language.md) 中的类型系统概要,聚焦属性的声明、访问语法与编译期规则。

## 1 职责

- 为类型提供属性语法,将方法调用包装为字段式的访问与赋值形式,提升可读性与表达力。
- 提供索引属性,允许通过下标语法 `obj[index]` 访问与修改类型实例中的元素。
- 属性是纯语法糖,与显式方法调用在语义上完全等价,不引入隐式存储、额外开销或新的 ABI surface。

## 2 术语

- 属性: 通过 `prop` 声明的计算成员,由 `get` / `set` 函数体组成,对外以字段式语法访问。
- 普通属性: 以 `prop name { ... }` 形式声明的具名属性,访问语法为 `obj.name`,赋值语法为 `obj.name = value`。
- 索引属性: 以 `prop [param: Type] { ... }` 形式声明的下标属性,访问语法为 `obj[key]`,赋值语法为 `obj[key] = value`。
- 索引参数: 索引属性头部 `[param: Type]` 中声明的参数; 该参数在 `get` / `set` 函数体内隐式可用,无需重复声明。
- getter: 属性中的 `get` 函数体; 普通属性无参数,索引属性隐式接收索引参数; 返回属性值。
- setter: 属性中的 `set` 函数体; 普通属性接收一个显式声明的值参数,索引属性隐式接收索引参数并显式声明值参数; 返回类型为 `void`,可显式声明或省略(省略即 `void`)。
- 只读属性: 仅声明 `get` 的属性,外部只能读取,不能赋值。
- 只写属性: 仅声明 `set` 的属性,外部只能赋值,不能读取。
- 读写属性: 同时声明 `get` 与 `set` 的属性,外部可读取也可赋值。

## 3 语法

正确语法一,普通属性(读写、只读、只写):

```feng
type User {
  var _name: string;
  var _age: int;

  // 读写属性
  prop name {
    func get(): string {
      return self._name;
    }
    func set(value: string): void {
      self._name = value;
    }
  }

  // 只读属性
  prop age {
    func get(): int {
      return self._age;
    }
  }

  // 只写属性
  prop password {
    func set(value: string): void {
      // 仅写入,不可读取
    }
  }
}

let user = User { _name: "guest", _age: 18 };
let n = user.name;        // 编译器降为内部 getter 调用,无显式可调方法
user.name = "admin";      // 编译器降为内部 setter 调用,无显式可调方法
let a = user.age;         // 只读,合法
// user.age = 20;         // 错误: age 是只读属性,不可赋值
// let p = user.password; // 错误: password 是只写属性,不可读取
user.password = "secret"; // 只写,合法
```

正确语法二,索引属性:

```feng
type Grid {
  var _data: int[!];

  prop [index: int] {
    func get(): int {
      return self._data[index];
    }
    func set(value: int): void {
      self._data[index] = value;
    }
  }
}

let grid = Grid { _data: [0, 0, 0] };
let v = grid[1];      // 编译器降为内部 getter 调用,无显式可调方法
grid[1] = 42;         // 编译器降为内部 setter 调用,无显式可调方法
```

索引参数 `index` 在 `prop [index: int]` 头部声明,`get` 与 `set` 函数体内直接通过名称 `index` 引用,无需在参数列表中重复声明。

正确语法三,索引属性重载:

```feng
type DynamicMap {
  prop [key: int] {
    func get(): string { return ""; }
    func set(value: string): void {}
  }

  prop [key: string] {
    func get(): string { return ""; }
    func set(value: string): void {}
  }
}

let map = DynamicMap {};
let a = map[1];       // 匹配 int 索引属性
let b = map["hello"]; // 匹配 string 索引属性
```

正确语法四,泛型类型中的索引属性与索引属性泛型重载:

```feng
type Container<T> {
  var _items: T[!];

  prop [index: int] {
    func get(): T {
      return self._items[index];
    }
    func set(value: T): void {
      self._items[index] = value;
    }
  }
}

type MultiKey<V> {
  prop [key: int] {
    func get(): V { return V {}; }
    func set(value: V): void {}
  }

  prop [key: string] {
    func get(): V { return V {}; }
    func set(value: V): void {}
  }
}
```

正确语法五,复合赋值:

```feng
type Counter {
  var _count: int;

  prop count {
    func get(): int {
      return self._count;
    }
    func set(value: int): void {
      self._count = value;
    }
  }
}

let c = Counter { _count: 0 };
c.count += 1;  // 等价于 c.count = c.count + 1
c.count *= 2;  // 等价于 c.count = c.count * 2
```

索引属性同样支持复合赋值:

```feng
let grid = Grid { _data: [0, 0, 0] };
grid[0] += 10; // 等价于 grid[0] = grid[0] + 10
```

正确语法六,可见性修饰:

```feng
type Config {
  var _debug: bool;
  var _token: string;

  // seal prop: get/set 无论是否声明都是 seal
  seal prop debug {
    func get(): bool {
      return self._debug;
    }
  }

  // open prop: get/set 默认 open,可单独声明为 seal
  prop token {
    func get(): string {
      return self._token;
    }
    seal func set(value: string): void {
      self._token = value;
    }
  }

  // open prop(默认): 外部可读,不可写
  prop verbose {
    func get(): bool {
      return true;
    }
  }
}
```

属性默认可见性为 `open`,可显式声明 `seal` 或 `open`; 当 `prop` 自身为 `seal` 时,其 `get` / `set` 无论是否显式声明均为 `seal`; 当 `prop` 为 `open` 时,`get` / `set` 可各自独立声明为 `seal` 或 `open`(省略即 `open`)。

正确语法七,spec 中声明属性签名:

```feng
spec Named {
  // 只读属性
  prop name {
    func get(): string;
  }
}

spec Writable {
  // 读写属性
  prop value {
    func get(): int;
    func set(value: int): void;
  }

  // 只写属性
  prop output {
    func set(value: string): void;
  }
}

spec Indexed<T> {
  // 只读索引属性
  prop [key: int] {
    func get(): T;
  }

  // 读写索引属性
  prop [key: string] {
    func get(): T;
    func set(value: T): void;
  }
}
```

`spec` 中属性使用与 `type` 一致的块语法,但 `get` / `set` 只有签名、不含实现体; 只读属性仅声明 `get`,只写属性仅声明 `set`,读写属性同时声明 `get` 与 `set`。

正确语法八,fit 中实现或补充属性:

```feng
// fit 实现 spec 中的属性契约
fit User: Named {
  prop name {
    func get(): string {
      return self._name;
    }
    func set(value: string): void {
      self._name = value;
    }
  }
}

// fit 自扩展补充属性
fit User {
  prop display_name {
    func get(): string {
      return "User(" + self._name + ")";
    }
  }
}
```

正确语法九,泛型类型的属性使用外层泛型参数:

```feng
type Wrapper<T> {
  var _inner: T;

  prop inner {
    func get(): T {
      return self._inner;
    }
    func set(value: T): void {
      self._inner = value;
    }
  }
}

let w = Wrapper<int> { _inner: 42 };
let v = w.inner; // v: int
w.inner = 100;
```

错语法一,属性与字段或方法同名:

```feng
type Bad {
  var name: string;

  // 错误: name 与字段 name 同属同一命名空间,构成冲突
  prop name {
    func get(): string { return self.name; }
  }
}
```

错语法二,set 声明非 void 返回类型:

```feng
type Bad {
  var _v: int;

  prop value {
    // 错误: set 返回类型必须是 void,不得声明为非 void 类型
    func set(value: int): int {
      self._v = value;
      return self._v;
    }
  }
}
```

错语法三,索引属性使用多参数:

```feng
type Bad {
  // 错误: 索引属性仅支持单参数
  prop [x: int, y: int] {
    func get(): int { return 0; }
  }
}
```

错语法四,索引属性索引参数类型冲突:

```feng
type Bad {
  prop [key: int] {
    func get(): string { return ""; }
  }

  // 错误: 与上一个索引属性索引参数类型相同,构成签名冲突
  prop [idx: int] {
    func get(): string { return ""; }
  }
}
```

错语法五,属性声明为 static:

```feng
type Bad {
  // 错误: 属性不支持 static 声明
  static prop count {
    func get(): int { return 0; }
  }
}
```

错语法六,属性自身声明泛型参数:

```feng
type Bad {
  // 错误: 属性自身不支持泛型参数,仅能使用外层类型的泛型参数
  prop value<T> {
    func get(): T { return T {}; }
  }
}
```

错语法七,只读属性赋值:

```feng
type Config {
  prop debug {
    func get(): bool { return false; }
  }
}

let cfg = Config {};
cfg.debug = true; // 错误: debug 是只读属性,不可赋值
```

错语法八,只写属性读取:

```feng
type Sink {
  prop output {
    func set(value: string): void {}
  }
}

let s = Sink {};
let v = s.output; // 错误: output 是只写属性,不可读取
```

错语法九,对象字面量中使用属性名:

```feng
type User {
  var _name: string;

  prop name {
    func get(): string { return self._name; }
    func set(value: string): void { self._name = value; }
  }
}

// 错误: 属性不是字段,不可在对象字面量中初始化
let user = User { name: "guest" };
```

错语法十,spec 属性以字段或方法满足:

```feng
spec Named {
  prop name {
    func get(): string;
  }
}

// 错误: spec 声明了 prop name,实现方必须以 prop 满足,不得以字段替代
type Bad1: Named {
  var name: string;
}

// 错误: 不得以方法替代属性满足 spec 中的 prop
type Bad2: Named {
  func name(): string { return ""; }
}

// 正确: 以 prop 满足 spec 中的 prop
type Good: Named {
  var _name: string;

  prop name {
    func get(): string {
      return self._name;
    }
  }
}
```

错语法十一,普通属性重复声明:

```feng
type Bad {
  var _v: int;

  prop value {
    func get(): int { return self._v; }
  }

  // 错误: 普通属性不支持重载,同一 type 中不得声明多个同名普通属性
  prop value {
    func get(): string { return ""; }
  }
}
```

## 4 语义

- 属性是纯语法糖,与显式方法调用在语义上完全等价; `prop` 声明不分配任何隐式存储空间。属性的 `get` / `set` 在编译器内部应作为普通成员方法处理,尽可能复用现有方法的解析、类型检查、重载决议与代码生成链路,避免为属性引入独立的处理路径。
- 普通属性 `prop name` 中的 `get` 与 `set` 是普通成员函数的语法糖; 读取 `obj.name` 与赋值 `obj.name = value` 由编译器降为内部 getter / setter 调用,不生成用户可调用的显式方法; 用户只能通过属性语法 `obj.name` 访问。
- 索引属性 `prop [key: T]` 中的 `get` 与 `set` 同样是普通成员函数的语法糖; `obj[key]` 与 `obj[key] = value` 由编译器降为内部 getter / setter 调用,不生成用户可调用的显式方法; 用户只能通过下标语法 `obj[key]` 访问。
- 索引属性的索引参数在属性头部 `[param: Type]` 中声明,`get` 与 `set` 函数体内通过参数名直接引用,无需在各自的参数列表中重复声明; 索引参数由编译器隐式传入,语义类似于成员方法中的 `self`。
- 索引属性的 `get` 无显式参数,`set` 仅声明值参数; 编译器按属性头部的索引参数类型自动将索引值传入对应函数。
- 属性的 getter 返回类型可省略,省略时按方法返回类型推导规则处理。
- setter 返回类型为 `void`,可显式声明或省略(省略即 `void`); 不得声明为非 `void` 类型。
- 普通属性的 setter 参数列表只包含一个显式声明的值参数。
- 属性与方法、字段共享同一命名空间; 同一 type 中不得存在与属性同名的字段或方法。
- 普通属性不支持重载; 同一 type 中不得声明多个同名普通属性。
- 索引属性按"索引参数类型"参与重载决议; 同一 type 中可声明多个索引属性,但其索引参数类型不得相同,否则构成签名冲突。
- 属性自身不支持泛型参数声明,但可使用外层类型的泛型参数。
- 属性不支持 `static` 声明; 需要类型级的静态访问,应使用普通静态方法。
- 属性支持 `open` / `seal` 可见性修饰,默认 `open`; `get` / `set` 可各自独立声明可见性; 当 `prop` 自身为 `seal` 时,其 `get` / `set` 无论是否声明均为 `seal`; 当 `prop` 为 `open` 时,`get` / `set` 省略可见性修饰则默认为 `open`,显式声明 `seal` 可收窄为包内可见。
- 复合赋值 `obj.prop op= expr` 等价于 `obj.prop = obj.prop op expr`,但左侧只求值一次(对象表达式只求值一次); 复合赋值要求属性同时具有 `get` 和 `set`。
- 属性访问不复制对象状态,语义等同于方法调用; `self` 绑定规则与普通成员方法一致。
- `spec` 中的属性签名是对属性形状(getter / setter 签名)的契约声明,使用与 `type` 一致的块语法但不含实现体; 只读属性仅声明 `get`,只写属性仅声明 `set`,读写属性同时声明 `get` 与 `set`; `spec` 中所有成员默认 `open`,属性不得声明可见性修饰符。
- `fit` 可实现 `spec` 中声明的属性契约,也可通过自扩展为类型补充新属性; `fit` 块中的属性规则与 `type` 中一致。
- 类型声明满足 `spec` 时,属性、字段与方法各自独立满足,不得互相替代: spec 中声明为属性(`prop`)的成员,实现方必须也是属性; spec 中声明为字段(`let` / `var`)的成员,实现方必须也是字段; spec 中声明为方法(`func`)的成员,实现方必须也是方法。
- 属性满足 spec 时,实现方必须至少提供 spec 所声明的 `get` / `set`: spec 仅声明 `get` 时,实现方可提供 `get` 或 `get` + `set`; spec 同时声明 `get` + `set` 时,实现方必须同时提供 `get` + `set`; spec 仅声明 `set` 时,实现方可提供 `set` 或 `get` + `set`。
- 属性不是字段,不参与对象字面量初始化; `Type { propName: value }` 中 `propName` 不是合法字段名,编译器报错。
- 属性不引入新的 ABI surface; `@abi` 标注不影响属性的语法糖性质,属性不占用 ABI payload 空间,属性访问仍由编译器降为内部方法调用。
- 终结器中可正常访问属性,属性访问由编译器降为内部方法调用,规则与其他成员方法一致。

## 5 规则

分为「必须、禁止、建议」。

- [必须] 属性使用 `prop` 关键字声明,内部包含 `get` / `set` 函数体; 每个属性至少声明 `get` 或 `set` 其一。
- [必须] 普通属性的 getter 无参数; 索引属性的 getter 无显式参数,索引参数由属性头部隐式提供。
- [必须] 普通属性的 setter 必须显式声明一个值参数及其类型; 索引属性的 setter 仅声明值参数,索引参数由属性头部隐式提供。
- [必须] setter 返回类型为 `void`,可显式声明或省略(省略即 `void`); 不得声明为非 `void` 类型。
- [必须] 属性与方法、字段共享同一命名空间; 同一 type 中属性名不得与字段名或方法名冲突。
- [必须] 普通属性不支持重载; 同一 type 中不得声明多个同名普通属性。
- [必须] 索引属性仅支持单个索引参数。
- [必须] 同一 type 中多个索引属性按索引参数类型参与重载决议; 索引参数类型相同的多个索引属性构成签名冲突,编译期报错。
- [必须] 属性自身不支持泛型参数声明; 属性可使用外层类型的泛型参数。
- [必须] 属性不支持 `static` 声明。
- [必须] 属性支持 `open` / `seal` 可见性修饰,默认可见性与 type 成员一致(默认 `open`)。
- [必须] `get` / `set` 可各自独立声明可见性(`open` / `seal`),省略时默认为 `open`; 当 `prop` 自身为 `seal` 时,其 `get` / `set` 无论是否声明均强制为 `seal`。
- [必须] 复合赋值 `obj.prop op= expr` 要求属性同时具有 `get` 和 `set`; 只有 `get` 或只有 `set` 的属性不支持复合赋值。
- [必须] 对象字面量中不得使用属性名进行初始化; 属性不是字段,不参与构造阶段。
- [必须] `spec` 中声明属性签名时,使用与 `type` 一致的块语法,`get` / `set` 只有签名、不含实现体; 只读属性仅声明 `get`,只写属性仅声明 `set`,读写属性同时声明 `get` 与 `set`。
- [必须] `fit` 中实现或补充属性时,属性规则与 `type` 中一致。
- [必须] 类型满足 `spec` 时,属性、字段与方法不得互相替代: spec 中声明为 `prop` 的成员必须以 `prop` 实现,不得以 `let` / `var` 字段或 `func` 方法满足; spec 中声明为 `let` / `var` 字段的成员必须以字段满足; spec 中声明为 `func` 方法的成员必须以方法满足。
- [必须] 属性满足 spec 时,实现方必须至少提供 spec 所声明的 `get` / `set`: spec 仅声明 `get` 时,实现方可提供 `get` 或 `get` + `set`; spec 同时声明 `get` + `set` 时,实现方必须同时提供两者; spec 仅声明 `set` 时,实现方可提供 `set` 或 `get` + `set`。
- [必须] `spec` 中属性不支持可见性修饰; spec 是契约,所有成员默认 `open`,不得声明 `seal`。
- [禁止] 在只读属性上使用赋值或复合赋值。
- [禁止] 在只写属性上进行读取。
- [禁止] 在索引属性中使用多参数索引。
- [禁止] 将属性声明为 `static`。
- [禁止] 在属性上声明自身泛型参数。
- [禁止] 在对象字面量中使用属性名。
- [建议] 为属性提供 `get` 与 `set` 的完整实现,以保持读写一致性; 若需只读语义,仅声明 `get` 即可。

## 6 编译期

- 属性的 `get` / `set` 在编译器内部应作为普通成员方法进入现有处理链路; 语法解析、符号表注册、类型检查、重载决议与代码生成应尽可能复用现有方法的处理逻辑,编译器仅在语法糖展开阶段进行属性访问到内部方法调用的映射,避免为属性引入独立的语义或代码生成路径。

- 编译器必须检查属性声明的合法性: 属性名不得与同 type 中的字段或方法同名。
- 编译器必须检查普通属性是否存在重复声明; 同一 type 中出现多个同名普通属性时报错。
- 编译器必须检查每个属性是否至少包含 `get` 或 `set`。
- 编译器必须检查 setter 返回类型: 显式声明时必须为 `void`,声明为非 `void` 类型时报错; 省略返回类型时推导为 `void`。
- 编译器必须检查索引属性的 `get` 是否无显式参数、`set` 是否仅声明值参数; 若 `get` 或 `set` 中重复声明了索引参数,报错。
- 编译器必须检查索引属性是否仅使用单参数索引; 多参数索引报错。
- 编译器必须检查同一 type 中的多个索引属性是否构成签名冲突(索引参数类型相同)。
- 编译器必须检查属性读取操作: 目标属性必须具有 `get`; 仅 `set` 的属性不可读取。
- 编译器必须检查属性赋值操作: 目标属性必须具有 `set`; 仅 `get` 的属性不可赋值。
- 编译器必须检查复合赋值目标属性是否同时具有 `get` 和 `set`。
- 编译器必须检查属性是否被声明为 `static`; 若是则报错。
- 编译器必须检查属性自身是否声明了泛型参数; 若是则报错。
- 编译器必须检查对象字面量中是否使用了属性名; 若是则报错。
- 编译器必须检查 `spec` 中的属性签名是否使用块语法且不含实现体; `spec` 中属性不得声明可见性修饰符(`open` / `seal`),若是则报错。
- 编译器必须检查类型满足 `spec` 时,属性、字段与方法是否各自对应满足: spec 中声明为 `prop` 的成员必须以 `prop` 实现,以字段或方法满足时报错; spec 中声明为字段的成员必须以字段满足; spec 中声明为方法的成员必须以方法满足。
- 编译器必须检查属性满足 spec 时是否至少提供了 spec 所声明的 `get` / `set`: spec 声明了 `get` 而实现方未提供时报错; spec 声明了 `set` 而实现方未提供时报错; 实现方可提供多于 spec 要求的 `get` / `set`。
- 编译器必须检查 `fit` 中的属性实现是否符合 spec 契约(若存在)。
- 编译器必须检查泛型类型中属性的类型参数引用是否合法(仅引用外层类型泛型参数)。
- 编译器必须检查属性的可见性修饰符(`open` / `seal`)是否合法,并按可见性规则验证外部访问; 当 `prop` 为 `seal` 时,其 `get` / `set` 上显式声明的 `open` 修饰无效,编译器应忽略该修饰并视为 `seal`。
- 编译器必须检查 `get` / `set` 上的独立可见性声明是否合法,并按各自可见性验证外部读取与赋值访问。

## 7 运行时

- 属性访问与赋值在编译期已降为内部方法调用,运行时无额外开销; 属性不引入额外的运行时元数据或调度层。
- 属性不占用对象实例的存储空间; 只有显式声明的 `let` / `var` 字段参与对象内存布局。
- 复合赋值 `obj.prop op= expr` 在运行时先执行 getter 获取当前值,计算 `当前值 op expr`,再调用 setter 写入结果; 对象表达式只求值一次。
- 索引属性的运行时行为与普通属性一致: `obj[key]` 与 `obj[key] = value` 由编译器降为内部 getter / setter 调用; 索引值作为隐式参数传入。
- `@abi` 类型的属性不进入 ABI payload; 属性访问仍由编译器降为内部方法调用,不影响对象在 C ABI 中的传递方式。

## 8 关联

- [Feng 语言核心规范](./feng-language.md): 类型系统总览与保留字说明(`prop` 已从保留字转为正式关键字)。
- [Feng 语言类型规范](./feng-type.md): type 成员模型、`self`、构造与终结器规则。
- [Feng 语言函数规范](./feng-function.md): 成员方法语法与重载决议规则; 属性 getter/setter 遵循相同规则。
- [Feng 语言变量绑定与作用域规范](./feng-binding.md): `let` / `var` 绑定语义; 属性不参与绑定。
- [Feng 语言表达式与运算规范](./feng-expression.md): 成员访问、下标访问与赋值语义; 属性访问由编译器降为内部方法调用。
- [Feng 语言 `spec` 规范](./feng-spec.md): object-form spec 中的属性签名声明。
- [Feng 语言 `fit` 规范](./feng-fit.md): 契约适配与自扩展中的属性实现与补充。
- [Feng 语言泛型规范](./feng-generics-draft.md): 泛型类型中属性使用外层类型参数。
- [Feng 语言可见性规范](./feng-visibility.md): 三级可见性模型与属性的 `open` / `seal` 修饰。
