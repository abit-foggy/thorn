# Architecture

Thorn is structured around a single-binary embedding model: one tool binary serves every project without per-project compilation or AOT plugin extraction.

## Component Overview

Thorn consists of three core components in C99:

- **`src/main.c`**: The command-line interface entry point. Responsible for option parsing, path validation, initializing the Pith runtime context, binding host functions, reading `build.thorn`, appending the evaluation epilogue, and driving the decompiler.
- **`src/engine.c`**: The build graph model and backend emitter. Maintains target structures, sources, flags, custom commands, and generates `build.ninja` and `Makefile`. All symbols in this file are mangled under `c_engine_*` for direct ABI interop.
- **`src/decompile.c`**: The reverse parser. Parses Ninja and Make syntax, extracts rules, edges, and dependencies, and synthesizes `build.thorn` code.

## Runtime Embedding of Pith

Thorn embeds the Pith compiler frontend directly:

1. **Host Function Registration**: During startup, `main.c` registers the engine functions using `pith_register_ns_fn()` under the `engine` namespace. Each function specifies its return and parameter classes according to Pith's C ABI (e.g. `v` for void, `w` for 32-bit int, `p` for ARC string pointer).
2. **Epilogue Evaluation**: When evaluating `build.thorn`, Thorn wraps or appends a call to `engine.emit()` after executing the project statements.
3. **Execution Fallback & Link Objects**:
   - In environments permitting JIT (libtcc in-memory execution), Pith executes the byte instructions directly against the loaded host symbols in Thorn's process memory.
   - On SELinux-hardened or `W^X` systems where runtime memory execution is restricted, Pith falls back to compiling a temporary child executable via QBE/TCC.
   - To support this fallback child, Thorn registers `out/artifacts/engine.o` using `pith_register_link_object()`. The child links against this object and builds the identical graph, emitting the backend files seamlessly.

## Self-Hosting

Thorn builds itself using its own generated backend. During `tests/verify.sh`:

1. `out/thorn` is built from C source using the bootstrapping `Makefile`.
2. `out/thorn` runs on its own `build.thorn`, emitting `build.ninja`.
3. `samu` builds `out/thorn` from `build.ninja`.
4. The resulting Stage 2 binary runs, builds projects, and decompiles backends deterministically.
