# Feng User Manual

This directory contains user documentation for Feng developers. It focuses on installing, learning, and using Feng rather than explaining the compiler's internal implementation.

## Scope

This directory covers:

- Installation, environment setup, and getting started.
- Guides and complete examples for Feng language features.
- Project creation, building, running, testing, debugging, and dependency management.
- The standard library, interoperability, and common development tasks.
- Recommended coding style and practices.

The user manual focuses on tasks, examples, and usage rather than compiler internals. Navigation and further reading link only to content within this language directory so that the English manual can be published independently.

## Contents

### Getting Started

- [Install Feng](./getting-started/installation.md)
- [Quick Start](./getting-started/quick-start.md)
- [Your First Project](./getting-started/first-project.md)

### Language Guide

- [Values and Bindings](./language/values-and-bindings.md)
- [Types](./language/types.md)
- [Expressions](./language/expressions.md)
- [Functions](./language/functions.md)
- [Control Flow](./language/control-flow.md)
- [Pattern Matching](./language/pattern-matching.md)
- [User-Defined Types](./language/user-defined-types.md)
- [Contracts and `fit`](./language/contracts-and-fit.md)
- [Generics](./language/generics.md)
- [Error Handling](./language/error-handling.md)
- [Modules and Visibility](./language/modules-and-visibility.md)

### Project Development

- [Project Structure](./projects/project-structure.md)
- [Build and Run](./projects/build-and-run.md)
- [Dependency Management](./projects/dependencies.md)
- [Checking, Testing, and Debugging](./projects/testing-and-debugging.md)

### Standard Library, Tooling, and Interoperability

- [Standard Library Guide](./standard-library/README.md)
- [CLI](./tooling/cli.md)
- [Editor Support](./tooling/editor.md)
- [Code Formatting](./tooling/formatter.md)
- [C Interoperability](./interop/c-interop.md)
- [Feng Style Guide](./feng-style.md)

## Directory Structure

```text
en/
├── README.md
├── getting-started/
│   ├── installation.md
│   ├── quick-start.md
│   └── first-project.md
├── language/
│   ├── values-and-bindings.md
│   ├── types.md
│   ├── expressions.md
│   ├── functions.md
│   ├── control-flow.md
│   ├── pattern-matching.md
│   ├── user-defined-types.md
│   ├── contracts-and-fit.md
│   ├── generics.md
│   ├── error-handling.md
│   └── modules-and-visibility.md
├── projects/
│   ├── project-structure.md
│   ├── build-and-run.md
│   ├── dependencies.md
│   └── testing-and-debugging.md
├── standard-library/
│   ├── README.md
│   ├── collections.md
│   ├── text.md
│   ├── filesystem-and-io.md
│   ├── time-and-math.md
│   └── platform-and-process.md
├── tooling/
│   ├── cli.md
│   ├── editor.md
│   └── formatter.md
├── interop/
│   └── c-interop.md
└── feng-style.md
```

Each directory has a distinct purpose:

- `getting-started/` helps developers install Feng, run it for the first time, and create their first complete project.
- `language/` introduces Feng's core language features and how to use them in a recommended learning order.
- `projects/` covers project organization, building, running, dependencies, testing, and debugging.
- `standard-library/` provides usage guides and examples for common tasks without redefining the standard library.
- `tooling/` explains the command-line interface, editor support, and formatting tools.
- `interop/` explains interoperability between Feng and other languages or native interfaces.
- `feng-style.md` provides recommended Feng coding conventions.

## Recommended Reading Order

New users should read Install Feng, Quick Start, and Your First Project in order before moving on to the language guide. After learning the language basics, use the project development, standard library, tooling, and interoperability sections as task-oriented references.

Add future content in the order users need it to complete tasks. Keep examples complete, understandable, and executable. Create chapters only when their content is ready; do not add empty placeholders.
