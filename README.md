# thorn

> **thorn** /θɔːrn/ *noun*
>
> The sharp spine on the stem of a plant. A thorn grows on the pith.

A lean, native meta-build generator that **embeds**
[pith](https://github.com/abit-foggy/pith) the way a game engine embeds
a scripting language: thorn carries pith's frontend, reads
`build.thorn` at runtime, and evaluates it with the build API
registered as a host namespace. No VM, no interpreter semantics, no
per-project binaries — in the QBE spirit: minimal, zero-bloat,
deterministic.

```
                build.thorn (a pith script, read at runtime)
                      |
                      |  thorn embeds pith (pith_embed.h)
                      |  host API = the thorn_engine.* namespace
                      v
    [ out/thorn ]  ---- thorn_engine.add_target / add_source / ...
              |           (direct C ABI calls, typed by pith's FFI)
              |
              |  thorn_engine.emit()  (appended by the CLI)
              v
    out/artifacts/build.ninja   (samurai / ninja, deps = gcc depfiles)
    out/artifacts/Makefile      (portable make, -MMD -MP)
              |
              v
    thorn decompile <backend>  ->  idiomatic build.thorn
```

## Usage

```
thorn                              evaluate build.thorn, emit backends
thorn build [-f spec] [--out-dir dir] [--ninja|--make]
thorn decompile <build.ninja|Makefile> [-o out.thorn]
thorn help | version
```

Backends are written to `--out-dir` (default: the project root) and
built from the root:

```
thorn --out-dir out/artifacts
samu -f out/artifacts/build.ninja
make -f out/artifacts/Makefile
```

Outputs land in `out/artifacts`; source paths stay project-root
relative. `CC`/`AR` override the baked toolchain. `THORN_ENGINE_OBJ`
overrides the fallback link object (default: resolved next to the
binary).

## The specification

```pith
thorn_engine.project("demo")

thorn_engine.add_target("demo_app", thorn_engine.exe)
thorn_engine.add_source("demo_app", "main.c")
thorn_engine.add_include("demo_app", ".")
thorn_engine.add_cflag("demo_app", "-O2")
thorn_engine.add_ldflag("demo_app", "-lm")
```

| Call | Meaning |
|---|---|
| `thorn_engine.project(name)` | project name (headers, aggregates) |
| `thorn_engine.exe` / `.static_lib` / `.shared_lib` | target-type pseudo-constants |
| `thorn_engine.add_target(name, type)` | declare a target |
| `thorn_engine.add_source(target, src)` | `.c` is compiled per-target; anything else links raw |
| `thorn_engine.add_cflag(target, flag)` | compile flag |
| `thorn_engine.add_ldflag(target, flag)` | link flag (also libraries, archives) |
| `thorn_engine.add_include(target, dir)` | `-I` directory |
| `thorn_engine.add_order_dep(target, prereq)` | regeneration-order prerequisite (order-only `\|\|` on compile edges, implicit `\|` on link edges) |
| `thorn_engine.pkg_config(target, pkg)` | folds `pkg-config --cflags --libs` into the target |
| `thorn_engine.emit()` | write the backends (the CLI appends this automatically) |

Do not call `proc.exit()` in a spec before it finishes: emission runs
after the last statement.

## How the embedding works

thorn uses pith's embed C ABI (`pith_embed.h`):

- `pith_register_ns_fn` registers every API function under the
  `thorn_engine` namespace with its FFI signature, so the compiler
  resolves `thorn_engine.add_target(...)` exactly like a native C
  import: typed calls straight through the C ABI (System V AMD64 /
  AAPCS64), zero-arg members readable as pseudo-constants, string
  parameters borrowed, `int` returns usable as statements or values.
- `pith_eval_string` compiles and runs the spec. On hardened kernels
  where in-memory JIT is blocked (SELinux `mprotect`), pith falls back
  to a temporary executable; `pith_register_link_object` supplies
  `out/artifacts/thorn_engine.o` — compiled under the
  `-D<stem>=c_thorn_engine_<stem>` renames that mirror pith's import
  mangling — so host functions resolve in the child process. The graph
  and `emit()` live in the engine, so evaluation behaves identically
  on both backends.

`src/thorn_engine.c` therefore has no `main()`; the CLI is
`src/thorn_main.c`.

## Self-hosting

`make` builds `out/thorn` (stage 1). Then:

```
make selfhost          # or: ./out/thorn --out-dir out/artifacts
                       #      samu -f out/artifacts/build.ninja
```

thorn evaluates its own `build.thorn`, emits its backend, and samu
rebuilds thorn — engine, CLI, decompiler, and the embedded pith
frontend — from it. `build.thorn` carries the symbol renames as plain
cflags, so the self-built `out/artifacts/thorn_core.a` doubles as its
own fallback link object.

## Reverse decompilation

`thorn decompile` reads a `build.ninja` or a `Makefile` (detected by
content), reconstructs the graph, and writes an idiomatic top-level
`build.thorn` — with honest `# note:` comments for everything outside
the model (pools, custom codegen rules, conditionals, ...). Verified
end-to-end: the decompiled spec regenerates a byte-identical
`build.ninja`.

## Prerequisites & pith integration notes

- Build the pith toolchain first: `cd ~/pith && make`.
- **glibc >= 2.43 / GCC 16**: pith's strict `-D_POSIX_C_SOURCE` build
  fails because `realpath`/`mkdtemp` left the strict POSIX namespace
  (removed in POSIX.1-2024). Build pith with
  `make CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -D_DEFAULT_SOURCE"`.
- If the `patch(1)` utility is missing, apply the vendored patches
  manually: `cd vendor/qbe && git apply ../../patches/qbe-embed.patch`
  (and likewise for tcc), then run `make`.
- On SELinux-hardened kernels the in-memory JIT is blocked; pith's
  temp-executable fallback handles it (this repo is developed and
  verified on such a host).

## Layout

```
build.thorn           thorn's own spec (self-hosting)
src/thorn_main.c      the CLI: spec loading, embed context, epilogue
src/thorn_engine.c    graph, thorn_engine.* host API, emitters
src/thorn_decompile.c ninja/Makefile ingestion -> build.thorn
src/include/          internal headers
out/                  the binary (out/thorn)
out/artifacts/        objects, generated build.ninja and Makefile
demo/                 a small multi-file C project + its build.thorn
Makefile              stage 1 bootstrap; verify.sh acceptance suite
```

## License

BSD 2-Clause, matching pith.
