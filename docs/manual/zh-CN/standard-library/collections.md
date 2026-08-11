# 集合

导入 `std.collections` 使用数组扩展和集合类型：

```feng
import std.collections;
```

## 数组扩展

```feng
let values = [10, 20, 30];
let count = values.length();
let index = values.indexOf(20);
let copied = values.clone();
let range = values.clone(1, 3);
```

`clone` 创建独立数组；`slice` 创建共享底层数组的只读 `Span<T>`：

```feng
let view = values.slice(1, 3);
let first = view.get(0);
let dense = view.toArray();
```

区间都采用 `[start, end)`。越界会抛出异常。

## List

`List<T>` 是可增长的有序集合：

```feng
let names = List<string>();
names.add("Alice");
names.add("Bob");
names.insert(1, "Carol");

let first = names.get(0);
names.set(0, "Alicia");
names.removeAt(1);
let removed = names.remove("Bob");
let count = names.size();
let items = names.entries();
```

`removeAt(index)` 按位置删除；`remove(item)` 删除第一个匹配元素并返回是否找到。`entries()` 返回只读数组副本。`List<T>` 支持 `for/in`：

```feng
for let name in names {
  println(name);
}
```

## Map

键类型必须满足 `Hashable<K>`。标准字符串实现由 `std.text` 提供，因此字符串键通常同时导入两个模块：

```feng
import std.collections;
import std.text;

let scores = Map<string, int>();
scores.set("Alice", 90);
scores.set("Bob", 85);

if scores.has("Alice") {
  println("{0}", scores.get("Alice"));
}

scores.remove("Bob");
let count = scores.count();
```

`entries()` 返回 `MapEntity<K, V>[]`，键和值分别位于 `item1` 和 `item2`。

## Set

```feng
let tags = Set<string>();
tags.add("feng");
tags.add("language");

let contains = tags.has("feng");
tags.remove("language");
let all = tags.entries();
```

与 `Map` 一样，元素类型必须满足 `Hashable<K>`。
