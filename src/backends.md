# Backends

Thorn supports two target build engines: **Ninja** and **Make**. Both backends are designed for minimal overhead, strict dependency tracking, and reproducible output.

## Ninja Backend (`build.ninja`)

The Ninja backend produces clean, minimal Ninja syntax suitable for execution with either `ninja` or `samurai` (`samu`).

### Key Properties

- **Compiler Rules**:
  - `rule cc`: Compiles C files (`.c`) with automatic header dependency extraction (`deps = gcc`, `depfile = $out.d`).
    ```ninja
    rule cc
      depfile = $out.d
      deps = gcc
      command = $cc -MD -MF $out.d $cflags $includes -c $in -o $out
    ```
  - `rule as`: Assembles raw assembly files (`.s`).
    ```ninja
    rule as
      command = $as $asflags -c $in -o $out
    ```
  - `rule as_cpp`: Compiles assembly with C preprocessor directives (`.S`).
    ```ninja
    rule as_cpp
      depfile = $out.d
      deps = gcc
      command = $cc -MD -MF $out.d $asflags $cflags $includes -c $in -o $out
    ```
- **Archiving & Linking**:
  - `rule ar`: Packages static archives (`$ar rcs $out $in`).
  - Executable and dynamic linking rules invoke `$cc` with target `$ldflags`.
- **Automatic Target Extensions**:
  - Executable: `<target>`
  - Static library: `<target>.a`
  - Shared library: `<target>.so`
- **Deterministic Emission**: Rules and build edges are topologically sorted in graph order to guarantee byte-for-byte reproducibility across runs.

## Make Backend (`Makefile`)

The Make backend generates portable, standard POSIX-compatible Makefiles.

### Key Properties

- **Header Dependencies**: Uses `-MMD -MP` flags during compilation and includes generated `.d` files:
  ```makefile
  -include $(OBJS:.o=.d)
  ```
- **Pattern Rules**:
  - `%.o: %.c`: Compiles C files with `$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@`.
  - `%.o: %.s`: Assembles raw assembly with `$(AS) $(ASFLAGS) -c $< -o $@`.
  - `%.o: %.S`: Preprocesses and compiles assembly with `$(CC) $(ASFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@`.
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
