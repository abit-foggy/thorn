# Command-Line Interface

Thorn provides a single binary that acts as both a meta-build generator and a reverse decompiler.

```text
Usage:
  thorn [options] [spec-file]
  thorn decompile <build.ninja|Makefile>
  thorn --help
  thorn --version
```

## Options

### Generation Options

- `[spec-file]`  
  Path to the specification file. Defaults to `build.thorn` if not specified.

- `-e, --engine <ninja|make|both>`  
  Override or specify the target build backend to emit.  
  - `ninja`: Emit only `build.ninja`.
  - `make`: Emit only `Makefile`.
  - `both`: Emit both `build.ninja` and `Makefile`.  
  *Note:* If neither the specification file (`engine.backend(...)`) nor this flag specifies a backend, Thorn exits with an error.

- `-o, --out-dir <path>`  
  Output directory for emitted build files and compiled targets. Defaults to the current directory (`.`). Target binary outputs and object files will be placed inside this directory while source paths remain relative to project root.

- `--compiler <path>`  
  Specify the C compiler binary name or path (e.g. `gcc`, `clang`, `cc`). Thorn validates that the executable exists in `PATH` before baking it into the backend rules. Precedence: `--compiler` flag > `CC` environment variable > system default `cc`.

- `--ar <path>`  
  Specify the archiver executable (e.g. `ar`, `llvm-ar`) used for static libraries. Precedence: `--ar` flag > `AR` environment variable > system default `ar`.

### Informational Options

- `-h, --help`  
  Display usage and options summary.

- `-v, --version`  
  Display Thorn version information.

## Decompile Command

```bash
thorn decompile <build.ninja|Makefile>
```

Parses an existing Ninja file or Makefile and prints the equivalent `build.thorn` specification to standard output. See [Decompilation](decompile.md) for details.
