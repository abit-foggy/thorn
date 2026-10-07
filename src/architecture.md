# Architecture

Thorn is structured around a single-binary embedding model: one tool binary serves every project without per-project compilation or AOT plugin extraction.

## Component Overview

Thorn's core is implemented in clean, modular C99 with separated compilation units and public headers in `src/include/`:

### 1. CLI Driver & Embedding (`src/main.c`, `src/engine.c`)
- **`src/main.c`**: The command-line entry point. Handles option parsing (supporting subcommands `build`, `decompile`, `version`, `help`, and positional spec paths), toolchain binary verification in `PATH`, Pith runtime context initialization, host function registration under the `engine` namespace, and execution orchestration.
- **`src/engine.c`**: The host runtime API bridge. Implements the C host functions (`c_engine_*`) exposed to the embedded Pith evaluation environment, providing typed wrappers between Pith's ARC/value system and Thorn's internal build graph.

### 2. Build Graph & Invariants (`src/graph.c`, `src/include/graph.h`)
- **`src/graph.c`**: In-memory Directed Acyclic Graph (DAG) state manager. Manages projects, targets (`Target`), custom commands (`Command`), flags, includes, and order dependencies.
- Enforces strict deterministic invariants:
  - **Whitespace Rejection**: Rejects spaces or newlines in target names, source paths, flags, and include directories.
  - **Source Classification**: Classifies sources into C (`SRC_C`), raw assembly (`SRC_ASM_RAW`), and preprocessed assembly (`SRC_ASM_CPP`).
  - **Duplicate Elimination**: Detects and eliminates duplicate sources, includes, and flags.
  - **Dynamic Package Queries**: Resolves system `pkg-config` flags at generation time without deferred shell subshells.

### 3. Backend Emitters (`src/emit.c`, `src/ninja.c`, `src/make.c`)
- **`src/emit.c`**: Generation coordinator and filesystem driver. Validates the graph, ensures destination directories exist, and dispatches generation to the active backend(s).
- **`src/ninja.c`**: High-performance Ninja backend generator (`build.ninja`). Emits canonical rule definitions (`cc`, `as`, `as_cpp`, `ar`, custom commands), dependency tracking with `deps = gcc` and `depfile = $out.d`, and topologically ordered build edges for byte-for-byte reproducibility.
- **`src/make.c`**: Portable POSIX / GNU Makefile generator (`Makefile`). Emits pattern rules (`%.o: %.c`, `%.o: %.s`, `%.o: %.S`), header dependency tracking (`-MMD -MP`), automatic default `all:` rules, token translation (`$in` -> `$<`, `$out` -> `$@`), and automatic `clean` targets.

### 4. Reverse Decompiler Engine (`src/decompile.c`, `src/parser.c`, `src/lex.c`, `src/spec.c`)
- **`src/decompile.c`**: Reverse engineering orchestrator. Drives parsing of existing Ninja or Make syntax and exposes a rich, hookable FFI API (`decompile.*`) usable by standalone scripts and external tools (such as `thornk` for Linux Kbuild conversion).
- **`src/lex.c`**: Lexical scanner and tokenizer for Ninja and Make syntax.
- **`src/parser.c`**: AST parser extracting variables, rule definitions, and build edges from backend files.
- **`src/spec.c`**: Specification synthesizer; translates extracted build graphs into idiomatic, formatted `build.thorn` Pith specifications.

### 5. Modular Header Architecture (`src/include/`)
Internal subsystem boundaries are strictly enforced via modular C headers:
- `engine.h`: Target model, source kinds, and host function prototypes.
- `graph.h`: Graph allocation, source classification, and validation.
- `emit.h`: Backend generation entry points.
- `ninja.h`: Ninja backend emitter declarations.
- `make.h`: Makefile backend emitter declarations.
- `parser.h`: Ninja and Makefile AST structures and parse APIs.
- `lex.h`: Token definitions and lexical scanner state.
- `spec.h`: Spec synthesis and formatting declarations.

## Runtime Embedding of Pith

Thorn embeds the Pith compiler frontend directly:

1. **Host Function Registration**: During startup, `main.c` registers the engine functions using `pith_register_ns_fn()` under the `engine` namespace. Each function specifies its return and parameter classes according to Pith's C ABI (e.g. `v` for void, `w` for 32-bit int, `p` for ARC string pointer).
2. **Epilogue Evaluation**: When evaluating `build.thorn`, Thorn wraps or appends a call to `engine.emit()` after executing the project statements.
3. **Execution Fallback & Link Objects**:
   - In environments permitting JIT (libtcc in-memory execution), Pith executes the byte instructions directly against the loaded host symbols in Thorn's process memory.
   - On SELinux-hardened or `W^X` systems where runtime memory execution is restricted, Pith falls back to compiling a temporary child executable via QBE/TCC.
   - To support this fallback child, Thorn registers `out/artifacts/engine.o` (or `libthorn_core.a`) using `pith_register_link_object()`. The child links against this object and builds the identical graph, emitting the backend files seamlessly.

## Self-Hosting

Thorn builds itself using its own generated backend. During `tests/verify.sh`:

1. `out/thorn` is built from C source using the bootstrapping `Makefile`.
2. `out/thorn` runs on its own `build.thorn`, emitting `out/artifacts/build.ninja`.
3. `samu` builds `out/artifacts/thorn` from `build.ninja`.
4. The resulting Stage 2 binary runs, builds projects, and decompiles backends deterministically.
