# Backends

Thorn supports two target build engines: **Ninja** and **Make**. Both backends are designed for minimal overhead, strict dependency tracking, and reproducible output.

## Ninja Backend (`build.ninja`)

The Ninja backend produces clean, minimal Ninja syntax suitable for execution with either `ninja` or `samurai` (`samu`).

### Key Properties

- **Compiler Rule**: Configured with `deps = gcc` and `depfile = $out.d`.
  ```ninja
  rule cc
    depfile = $out.d
    deps = gcc
    command = $cc -MD -MF $out.d $cflags $includes -c $in -o $out
  ```
- **Automatic Target Names**:
  - Executable: `<target>`
  - Static library: `<target>.a` (built via `rule ar` running `$ar rcs $out $in`)
  - Shared library: `<target>.so` (built with `$cc -shared $in $ldflags -o $out`)
- **Deterministic Emission**: Rules and build edges are sorted in graph order to ensure byte-for-byte reproducibility across runs.

## Make Backend (`Makefile`)

The Make backend generates portable, standard POSIX-compatible Makefiles.

### Key Properties

- **Header Dependencies**: Uses `-MMD -MP` flags during compilation and includes generated `.d` files:
  ```makefile
  -include $(OBJS:.o=.d)
  ```
- **Rule Ordering**: Emits an `all:` rule as the very first default target before any explicit targets or custom commands, ensuring that running `make` with no arguments builds all primary targets.
- **Token Translation**: In custom commands (`engine.add_command`), `$in` is mapped to Make's automatic variable `$<` and `$out` is mapped to `$@`.
- **Phony Targets**: Declares `.PHONY: all clean` and provides an automatic `clean` target removing all build products.

## Engine Selection

You can select the engine in `build.thorn`:

```pith
engine.backend("ninja") # or "make", or "both"
```

Or override it from the CLI:

```bash
thorn --engine both
```
