# Collections

Import `std.collections` to use array extensions and collection types:

```feng
import std.collections;
```

## Array Extensions

```feng
let values = [10, 20, 30];
let count = values.length();
let index = values.indexOf(20);
let copied = values.clone();
let range = values.clone(1, 3);
```

`clone` creates an independent array. `slice` creates a read-only `Span<T>` that shares the underlying array:

```feng
let view = values.slice(1, 3);
let first = view.get(0);
let dense = view.toArray();
```

All ranges use `[start, end)`. An out-of-bounds index raises an exception.

## List

`List<T>` is a growable ordered collection:

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

`removeAt(index)` removes by position; `remove(item)` removes the first matching element and reports whether it was found. `entries()` returns a read-only array copy. `List<T>` supports `for/in`:

```feng
for let name in names {
  println(name);
}
```

## Map

The key type must satisfy `Hashable<K>`. The standard string implementation is provided by `std.text`, so string keys normally require both modules:

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

`entries()` returns `MapEntity<K, V>[]`; the key and value are stored in `item1` and `item2`, respectively.

## Set

```feng
let tags = Set<string>();
tags.add("feng");
tags.add("language");

let contains = tags.has("feng");
tags.remove("language");
let all = tags.entries();
```

As with `Map`, the element type must satisfy `Hashable<K>`.
