# Text

Import `std.text` to use string extensions, formatting, Unicode views, and regular expressions:

```feng
import std.text;
```

## String Operations

```feng
let text = "  Feng Language  ";
let trimmed = text.trim();
let lower = trimmed.toLowerCase();
let upper = trimmed.toUpperCase();
let found = trimmed.contains("Language");
let index = trimmed.indexOf("Lang");
let parts = trimmed.split(" ");
```

Other commonly used methods include `startsWith`, `endsWith`, `replace`, `repeat`, `padStart`, `padEnd`, `clone`, and `toBytes`.

`string.length()` returns the number of UTF-8 bytes. `at(index)` and `getByte(index)` also operate on byte positions. Do not use them to count user-visible characters.

## Joining and Formatting

```feng
let joined = string.join(", ", ["Feng", "C", "LLVM"]);
let message = string.format("{0}: {1}", "score", "100");
```

Placeholders use zero-based indexes such as `{0}` and `{1}`. An invalid or out-of-range placeholder is preserved as written.

Arguments of the same type that satisfy `Display` can also be formatted directly:

```feng
import std.numeric;

let message = string.format<int>("{0} + {1} = {2}", 20, 22, 42);
```

## Unicode Views

Strings are stored as UTF-8. Use a rune view to iterate over Unicode code points and a grapheme view to iterate over user-visible grapheme clusters. Both avoid treating the bytes of a multibyte character as separate text characters.

## Regular Expressions

```feng
let pattern = RegExp("[0-9]+");
defer {
  pattern.destroy();
}

let matched = pattern.test("Feng 42");
let matches = pattern.findAll("1, 2, 3");
let replaced = pattern.replaceAll("v1 v2", "version");
```

Use `RegExpFlag` to configure case sensitivity, multiline mode, dot-all behavior, and related options. `RegExp` owns native resources, so call `destroy()` after use and prefer `defer` to guarantee cleanup.

Strings also provide convenience methods such as `matches`, `replacePattern`, and `splitPattern`.
