# Command-Line Interface

Thorn provides a single unified binary that acts as both a meta-build generator and a reverse decompiler.

## Usage Syntax

```text
USAGE:
    thorn [spec]
    thorn build [-f spec] [--out-dir dir] [--engine ninja|make|both]
                [--compiler cc] [--ar ar]
    thorn decompile <build.ninja|Makefile> [-o out.thorn]
    thorn help | version
```

## Commands & Subcommands

### Default / `build` Command

Evaluates the specification file (`build.thorn` or a custom spec) and emits the selected backend files.

```bash
# Evaluate build.thorn in current directory
thorn

# Evaluate custom spec file positionally
thorn myproject.thorn

# Explicit build command with output directory and engine override
thorn build -f custom.thorn --out-dir out/artifacts --engine ninja
```

#### Options:
- `[spec]`, `-f, --file <spec>`  
  Path to the specification file. Defaults to `build.thorn` if omitted.
- `-o, --out-dir <dir>`  
  Destination directory for generated build files (`build.ninja`, `Makefile`) and build artifacts. Defaults to current working directory (`.`).
- `-e, --engine <ninja|make|both>`  
  Override the target build engine(s) to emit:
  - `ninja`: Emit only `build.ninja`.
  - `make`: Emit only `Makefile`.
  - `both`: Emit both backends simultaneously.  
  *Note:* Can also be defined inside the specification via `engine.backend("...")`.
- `--compiler <cc>`  
  Specify the C compiler executable (e.g. `gcc`, `clang`, `cc`, `tcc`). Precedence: `--compiler` flag > `CC` environment variable > system default `cc`. Thorn validates that the compiler exists in `PATH` before writing rules.
- `--ar <ar>`  
  Specify the archiver executable (e.g. `ar`, `llvm-ar`) used for static library archives. Precedence: `--ar` flag > `AR` environment variable > system default `ar`.

### `decompile` Command

Parses an existing Ninja file or Makefile and synthesizes an idiomatic, equivalent `build.thorn` specification.

```bash
# Decompile build.ninja and print to standard output
thorn decompile build.ninja

# Decompile Makefile directly to a specification file
thorn decompile Makefile -o build.thorn
```

#### Options:
- `-o, --out <path>`  
  Output destination file for the recovered specification. If omitted, the recovered spec is written to standard output.

### Informational Commands

- `thorn version` / `-v, --version`: Print version information (`thorn 0.3.0`).
- `thorn help` / `-h, --help`: Display usage summary and help text.
