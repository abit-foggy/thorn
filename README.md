# thorn

> **thorn** /θɔːrn/ *noun*
>
> The sharp spine on the stem of a plant. A thorn grows on the pith.

A lean, native meta-build generator that dogfoods
[pith](https://github.com/abit-foggy/pith): the project specification
is a **Pith script compiled to machine code** and linked directly
against a C99 engine. No VM, no interpreter, no runtime config
parsing — in the QBE spirit: minimal, zero-bloat, deterministic.

```
[ build.thorn ]  (a pith script)
      |
      |  pith build --plugin        (compiles AOT to an object file)
      v
[ build_thorn.o ] --exports c_thorn_<author>_<name>_thorn_configure()
      |
      |  Direct C ABI link ($CC)
      v
[ thorn_engine.o ] + [ thorn_decompile.o ] + pith's libruntime.a
      |
      v
    thorn  ----->  build.ninja  (samurai / ninja)
              --->  Makefile    (portable make)
              --->  thorn decompile build.ninja  --->  build.thorn
```

## Forward generation

```
thorn                       emit build.ninja and Makefile in cwd
thorn build [--ninja|--make]
thorn decompile <file> [-o out.thorn]
thorn help | version
```

`thorn` invokes the compiled-in Pith entry point (`thorn_configure()`),
which configures the build graph by calling the engine's C ABI directly
through pith's FFI. The engine then emits deterministic backends:
identical graph and environment produce identical bytes. Header
dependencies are handled by the backends themselves (`deps = gcc` with
`-MD -MF`, `-MMD -MP` in make) — no custom dependency parsing.

`CC`/`AR` environment variables override the baked toolchain.

## The specification

```pith
import "thorn_engine.c"

fn thorn_configure()
    thorn_engine.project("demo")

    thorn_engine.add_target("demo_app", thorn_engine.exe)
    thorn_engine.add_source("demo_app", "main.c")
    thorn_engine.add_include("demo_app", ".")
    thorn_engine.add_cflag("demo_app", "-O2")
    thorn_engine.add_ldflag("demo_app", "-lm")
end
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

Graph state lives entirely in the C engine: pith v0.1 has no
collections, and keeping the spec declarative is the lean design
anyway. The decompiler is likewise C99 — pith has no string-splitting
primitives yet.

## Bootstrap recipe (per project)

thorn is a **generated per-project binary** — the spec is compiled in
at link time (there is no runtime interpreter). Each project needs a
`pith.toml` fixing the entry symbol:

```toml
[project]
author = "thorn"
name = "demo"        # the binary will call c_thorn_demo_thorn_configure()
```

```sh
pith build build.thorn --plugin -o build_thorn
tar -xOf build_thorn.ppkg plugin.o > build_thorn.o
cc -D<stem>=c_thorn_engine_<stem> ... thorn_engine.c ...      # see the Makefile
cc thorn_engine.o thorn_decompile.o build_thorn.o \
   ~/pith/runtime/libruntime.a -o my-thorn
./my-thorn && samu
```

The `-D<stem>=c_thorn_engine_<stem>` renames are the author-aware
symbol mangling pith applies to every imported C unit; the engine is
compiled with the identical defines. The bootstrap `Makefile` in this
repo encodes the full recipe, `verify.sh` runs the acceptance suite,
and thorn's own `build.thorn` carries the renames as plain cflags so
the stage 2 self-build works.

## Self-hosting

thorn builds itself: `make` (stage 1), then `./thorn --ninja && samu`
regenerates and rebuilds thorn from its own emitted ninja. The only
input stage 2 takes from stage 1 is `build_thorn.o`, the pith-compiled
configure object (pith compilation itself stays in the bootstrap).

## Reverse decompilation

`thorn decompile` reads a `build.ninja` or a `Makefile` (detected by
content), reconstructs the graph (targets, sources, includes, cflags,
ldflags, order deps), and writes an idiomatic `build.thorn` — with
honest `# note:` comments for everything outside the model (pools,
custom codegen rules, conditionals, ...). Verified end-to-end: the
decompiled spec regenerates a byte-identical `build.ninja`.

## Prerequisites & pith integration notes

- Build the pith toolchain first: `cd ~/pith && make`.
- **glibc >= 2.43 / GCC 16**: pith's strict `-D_POSIX_C_SOURCE` build
  fails because `realpath`/`mkdtemp` left the strict POSIX namespace
  (removed in POSIX.1-2024). Build pith with
  `make CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -D_DEFAULT_SOURCE"`.
  (Suggested pith fix: add `-D_DEFAULT_SOURCE` to `POSIXDEF`.)
- If the `patch(1)` utility is missing, apply the vendored patches
  manually: `cd vendor/qbe && git apply ../../patches/qbe-embed.patch`
  (and likewise for tcc), then run `make`.
- Suggested pith additions that would sharpen thorn's workflow
  (none are required): a direct object output (`pith build --object`)
  to skip the tar extraction; plugin functions with parameters and
  inter-fn calls; and a minimal string toolkit (`str.length` etc.).
- On SELinux-hardened kernels, pith's in-memory tcc JIT may log an
  mprotect fallback; pith falls back to its temp-executable path and
  everything works.

## Layout

```
build.thorn        thorn's own spec (dogfood + stage 2 self-build)
thorn_engine.c     graph, pith C ABI, ninja/make emitters, CLI
thorn_engine.h     shared internal model
thorn_decompile.c   ninja/Makefile ingestion -> build.thorn
Makefile           stage 1 bootstrap
verify.sh          acceptance suite (self-build, demo, roundtrip)
demo/              a small multi-file C project + its build.thorn
```

## License

BSD 2-Clause, matching pith.
