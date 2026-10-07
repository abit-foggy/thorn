# Specification Reference

Thorn specifications are written in Pith script (`build.thorn` or `.thorn`). The Thorn host engine registers the `engine` namespace with typed functions and constants.

## Target Types

Target types configured via `engine.add_target`:

- `engine.exe` — Executable binary.
- `engine.static_lib` — Static library archive (`.a`).
- `engine.shared_lib` — Dynamic/shared library (`.so`).

## Project & Backend Configuration

### `engine.project(name: str)`
Defines the name of the project.
```pith
engine.project("myproject")
```

### `engine.backend(backend: str)`
Selects the build file backend(s) to emit. Accepted values:
- `"ninja"`: Emit `build.ninja`.
- `"make"`: Emit `Makefile`.
- `"both"`: Emit both `build.ninja` and `Makefile`.

```pith
engine.backend("ninja")
```
*Note: A backend must be selected either via `engine.backend()` in the spec or via `--engine` on the command line.*

## Target Definition

### `engine.add_target(name: str, target_type: int)`
Declares a new target in the build graph. Target type is one of `engine.exe`, `engine.static_lib`, or `engine.shared_lib`.
```pith
engine.add_target("myapp", engine.exe)
engine.add_target("mylib", engine.static_lib)
```

### `engine.add_source(target: str, source_path: str)`
Associates a source file with a target. Thorn automatically classifies the source kind based on file extension:
- `.c`: C source compiled with the C compiler rule (`cc`).
- `.s`: Raw assembly assembled via the assembly rule (`as`).
- `.S`: Assembly with C preprocessor directives, compiled via the preprocessed assembly rule (`as_cpp`).

```pith
engine.add_source("myapp", "src/main.c")
engine.add_source("myapp", "src/arch/bootstub.S")
engine.add_source("myapp", "src/arch/trampoline.s")
```

### `engine.add_include(target: str, include_path: str)`
Appends an include directory (`-I<path>`) for compiling sources belonging to this target.
```pith
engine.add_include("myapp", "src/include")
```

### `engine.add_cflag(target: str, flag: str)`
Appends a C compiler flag used when compiling C sources (`.c`) of this target.
```pith
engine.add_cflag("myapp", "-Wall")
engine.add_cflag("myapp", "-O2")
```

### `engine.add_asflag(target: str, flag: str)`
Appends an assembler/preprocessor flag used when compiling assembly sources (`.s`, `.S`) of this target.
```pith
engine.add_asflag("myapp", "-Wa,--noexecstack")
engine.add_asflag("myapp", "-D__ASSEMBLY__")
```

### `engine.add_ldflag(target: str, flag: str)`
Appends a linker flag or linked library when creating the target artifact.
```pith
engine.add_ldflag("myapp", "-lpthread")
engine.add_ldflag("myapp", "-lm")
```

### `engine.add_order_dep(target: str, dependency_path: str)`
Adds an order-only prerequisite on the target (e.g. generated headers, assets, or pre-requisite build steps).
```pith
engine.add_order_dep("myapp", "generated/config.h")
```

## Custom Commands

### `engine.add_command(output: str, command: str, input: str)`
Defines an explicit build edge where running `command` generates `output` from `input`.

Variables supported inside the command string:
- `$in` / `$<`: The input prerequisite.
- `$out` / `$@`: The output file.
- `$$`: An escaped dollar sign literal (`$`).

```pith
engine.add_command("generated/version.h", "sh scripts/gen_version.sh $in $out", "VERSION")
```

When emitting Ninja, the rule is emitted as `cmd = <command>` with `$in` and `$out`. When emitting Makefile, `$out` and `$in` are translated to `$@` and `$<` respectively.

## Package Configuration

### `engine.pkg_config(target: str, package_name: str)`
Queries system `pkg-config` at build-generation time and automatically appends `--cflags` to compiler flags and `--libs` to linker flags for the target.
```pith
engine.pkg_config("myapp", "libssl")
```

## Emission

### `engine.emit()`
Dispatches graph emission to the chosen backend(s). When running `thorn`, this call is automatically appended as an epilogue to `build.thorn` if not explicitly called.
```pith
engine.emit()
```
