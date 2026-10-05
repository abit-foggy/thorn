# Thorn

**Thorn** is a fast, deterministic meta-build generator written in C99 that embeds the [Pith](https://github.com/abit-foggy/pith) scripting language as its specification frontend.

Thorn reads a `build.thorn` file describing your project's targets, sources, flags, and dependencies, and emits:
- **Ninja** files (`build.ninja`), optimized for tools like [ninja](https://ninja-build.org) or [samurai](https://github.com/michaelfournier/samurai) (`samu`) with automatic dependency generation (`deps = gcc`).
- **Makefiles** (`Makefile`), strictly POSIX-compatible pattern rules with `-MMD -MP` automatic header dependency tracking.

In addition to forward generation, Thorn features a **reverse decompiler** capable of reading existing `build.ninja` files or Makefiles and reconstructing idiomatic, clean `build.thorn` specifications that regenerate byte-identical outputs.

## Key Features

- **Embedded Pith Runtime**: Specification files are written in Pith, evaluated directly via Pith's C embed API with zero AOT compile step or intermediate plugins.
- **Dual Backends**: Emit fast, minimal Ninja files, portable Makefiles, or both simultaneously.
- **Custom Build Edges**: Declare custom build commands with `$in` / `$out` token substitutions.
- **Roundtrip Decompilation**: Ingest legacy or hand-written build files back into `build.thorn`.
- **Strict & Deterministic**: Whitespace rejection for flags and identifiers, ordered dependency edges, and byte-for-byte reproducible build artifacts.
