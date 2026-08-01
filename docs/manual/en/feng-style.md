# Feng Style Guide

This document records recommended conventions for Feng code. Its purpose is to help projects, teams, and individuals maintain a consistent style without changing language semantics.

Everything in this document is advisory rather than mandatory:

- It does not define syntax.
- It does not change semantic analysis, compiler behavior, or runtime behavior.
- The compiler and runtime must not change a program's meaning based on whether it follows these recommendations.
- Teams may refine these recommendations to suit their collaboration practices or deviate from them for a well-founded reason.

## 1 General Principles

- Style recommendations should improve readability, consistency, and maintainability rather than introduce additional semantic burden.
- Naming should make a symbol's broad category immediately recognizable, such as a contract, type, member, local binding, or constant-like global binding.
- Keep the style consistent within a file, module, and project. Avoid mixing multiple conventions for the same kind of symbol.

## 2 Naming

### 2.1 Naming `spec` Contracts

- Use PascalCase for contracts declared with `spec`, whether they are object contracts or callable contracts.
- Rationale: at the point of use, a contract normally acts as an abstract type or callable shape. PascalCase distinguishes it clearly from ordinary values and members.

Recommended:

```feng
spec CommitOptions {
  var message: i32;
}

spec ValueFormatter(value: i32): string;
```

### 2.2 Naming `type` Declarations and Members

- Use PascalCase for types declared with `type`.
- Use camelCase for fields, methods, constructor parameters, and ordinary member names.
- This naturally separates type names from member names.

Recommended:

```feng
type UserProfile {
  let displayName: string;
  var loginCount: i32;

  func incrementLoginCount(): void {
    self.loginCount = self.loginCount + 1;
  }
}
```

### 2.3 Naming Local `let` and `var` Bindings

- Use camelCase for local `let` and `var` bindings inside functions, methods, and blocks.
- Keep names readable even when a variable is used only in a small local scope; avoid abbreviations with no clear meaning.

Recommended:

```feng
func commit(message: string): void {
  let trimmedMessage = message;
  var retryCount = 0;
}
```

### 2.4 Naming Immutable Global Bindings

- When a top-level global `let` binding has constant-like semantics, use UPPER_SNAKE_CASE.
- The key distinction is not that every `let` should be uppercase, but that a global, immutable, semantically constant value benefits from this convention.

Recommended:

```feng
let DEFAULT_PORT = 8080;
let MAX_RETRY_COUNT = 3;
```

Do not apply the same style to an ordinary local binding:

```feng
func run(): void {
  let TEMP_VALUE = 1;
}
```

## 3 Blank Lines and Layout

### 3.1 Blank Lines Between Functions and Methods

- Separate functions at the same level with one blank line.
- Separate methods in the same `type` with one blank line.
- This makes declaration boundaries easier to scan.

Recommended:

```feng
func load(): void {
  sync();
}

func save(): void {
  flush();
}
```

```feng
type User {
  func open(): void {
    sync();
  }

  func close(): void {
    flush();
  }
}
```

### 3.2 Blank Lines Inside Functions and Methods

- Keep function and method bodies compact; normally, do not add blank lines unnecessarily.
- Use a small number of blank lines only when they clearly separate logical phases.
- Prefer uninterrupted reading for short functions.

Recommended:

```feng
func commit(message: string): void {
  let normalizedMessage = message;
  writeLog(normalizedMessage);
  flush();
}
```

## 4 Indentation and Braces

### 4.1 Indentation

- Use two spaces consistently for indentation.
- Do not mix indentation widths within a file.
- If a project uses an automatic formatter, configure its default output to follow this guide.

Recommended:

```feng
func main(args: string[]) {
  if true {
    print(args);
  }
}
```

### 4.2 Brace Placement

- Put an opening brace on the same line as the construct that introduces the block.
- Apply this convention to `func`, `type`, `if`, `else`, `for`, `while`, `try`, `catch`, `defer`, and other block constructs.

Recommended:

```feng
type User {
  func commit(message: i32): i32 {
    return message;
  }
}
```

```feng
if ready {
  run();
} else {
  stop();
}
```

## 5 Consistency

- Consistency matters more than any individual style recommendation.
- When a project already has a stable style, new code should follow the existing conventions.
- If a team later provides an automatic formatter, treat its output as the automated application of these recommendations. This document remains a style guide, not a language definition.

### 5.1 Keep Type Suffixes Adjacent

- Write the array type suffix `[]` immediately after the preceding type, without a space between the type and the brackets.
- Apply the same rule after a generic type instance. Prefer `string[]` and `Map<K, V>[]` over `string []` and `Map<K, V> []`.

### 5.2 Keep Generic Angle Brackets Adjacent

- Write generic parameter lists and explicit type-argument lists immediately after the preceding type, function, or method name. Do not insert a space before `<` or before `>`.
- Apply the same convention to constrained type parameters and nested generics. Prefer `Map<K: Hashable<K>, V>` and `Box<Map<string, int>>` over `Map < K: Hashable<K>, V >` and `Box<Map<string, int> >`.

## 6 Documentation Comments

### 6.1 Public API Documentation Comments

- Always place a `/** */` documentation comment immediately before a public API, especially for public declarations in the `std` standard library and official packages.
- Provide a concise and accurate summary for top-level `open type`, `open spec`, `open fit`, and `open func` declarations, as well as public member methods.
- Add `@param`, `@return`, or implementation notes when parameters, return values, complexity, zero-value behavior, or visible side effects affect how callers use the API.
- When a public API explicitly `throw`s a catchable Feng exception, use `@throws` to document its type or value and the condition that triggers it.
- When a public API invokes a runtime panic because the caller violated a precondition, use `@panic` to document that condition. Do not write it as `@throws`, because a runtime panic is not a Feng exception that `catch` can handle. There is no need to repeat non-caller preconditions such as allocation failure or corrupted runtime state on every API.
- Document stable semantics that callers need to know. Avoid turning internal implementation details that may change into API contracts.

## 7 Relationship to Other Documentation

- This document describes recommended style only; it does not define syntax or semantics.
- This document provides coding-style guidance only; it does not define syntax, semantics, the type system, function rules, lifetimes, or interoperability behavior.
