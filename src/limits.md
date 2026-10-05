# Limits & Constraints

Thorn is intentionally designed to be small, predictable, and deterministic. It enforces explicit design boundaries:

## Identifier & Path Rules

- **No Whitespace**: Thorn strictly rejects whitespace (spaces, tabs, newlines) in target names, source paths, include directories, compiler flags, and custom command outputs. Any whitespace triggers a clear diagnostic error:
  ```text
  thorn: error: target name 'my app' contains whitespace
  ```
- **Portability**: All source paths within `build.thorn` should be relative to the project root directory.

## System & Graph Limits

The graph model allocates fixed limits to ensure fast, deterministic operation without unbounded heap fragmentation:

- Maximum targets per project: 64
- Maximum sources per target: 256
- Maximum includes per target: 64
- Maximum cflags per target: 64
- Maximum ldflags per target: 64
- Maximum custom commands: 128
- Maximum order dependencies: 64

Exceeding these limits produces a descriptive diagnostic error at configuration time.

## Design Philosophy

- **No Implicit Globbing**: Sources must be explicitly enumerated in `build.thorn` to prevent non-deterministic build ordering and missing dependencies.
- **Strict Dependencies**: All order dependencies must be explicitly declared using `engine.add_order_dep()`.
- **Minimal Dependencies**: The Thorn binary relies only on standard POSIX C99 and the embedded Pith frontend/runtime.
