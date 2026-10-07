# Decompilation

Thorn includes a built-in reverse decompiler capable of reading an existing `build.ninja` or `Makefile` and synthesizing an equivalent, idiomatic `build.thorn` specification script.

## CLI Usage

```bash
# Decompile build.ninja and print specification to stdout
thorn decompile build.ninja

# Decompile Makefile directly to a specification file
thorn decompile Makefile -o build.thorn
```

## Reverse Engineering Capabilities

The decompiler performs heuristic AST analysis on backend rules and targets:

- **Target Recovery**: Detects targets, outputs, and classifies whether they are executables (`engine.exe`), static archives (`engine.static_lib`), or shared objects (`engine.shared_lib`).
- **Source Mapping**: Maps compiled object files back to their `.c`, `.s`, and `.S` source files.
- **Flags & Includes**: Extracts `-I` include flags into `engine.add_include()`, compiler options into `engine.add_cflag()`, and assembly flags into `engine.add_asflag()`.
- **Linker Flags**: Extracts library links (`-l`, `-L`) into `engine.add_ldflag()`.
- **Custom Commands**: Recovers custom build edges into `engine.add_command()`, transforming Make automatic variables (`$@`, `$<`) back into Thorn's portable tokens (`$out`, `$in`).
- **Backend Recognition**: Automatically emits `engine.backend("ninja")` or `engine.backend("make")` based on the ingested file format.

## Hookable Decompiler API (Pith & C FFI)

In addition to CLI usage, Thorn exposes a standalone, hookable decompiler API (`decompile.*`) designed for advanced build-system transformations and automated migrations (such as [thornk](https://github.com/abit-foggy/thornk) for Linux Kbuild translation):

### Configuration & Filtering
- `decompile.reset()`: Clear the current decompiler graph state.
- `decompile.set_project(name: str)`: Set the project name for the synthesized spec.
- `decompile.set_compiler(cc: str)`: Override the target compiler.
- `decompile.set_ar(ar: str)`: Override the target archiver.
- `decompile.ignore_target(pattern: str)`: Filter out unwanted targets (e.g. tests, firmware blobs).
- `decompile.keep_target(pattern: str)`: Whitelist specific targets.

### Flag Manipulation
- `decompile.strip_cflag(pattern: str)`: Remove specific compiler flags (e.g. unsupported GCC internal flags).
- `decompile.inject_cflag(target_pattern: str, flag: str)`: Ingest custom compiler flags (e.g. `-nostdinc`, `-D__KERNEL__`).
- `decompile.inject_include(target_pattern: str, include_dir: str)`: Inject include paths into matching targets.
- `decompile.remap_target(old_name: str, new_name: str)`: Rename targets in the synthesized graph.

### Extraction & Synthesis
- `decompile.parse_file(path: str)`: Ingest a Ninja or Makefile from disk.
- `decompile.target_count() -> int`: Returns the number of recovered targets.
- `decompile.to_thorn() -> str`: Returns the synthesized `build.thorn` specification as a string.
- `decompile.emit_thorn(path: str)`: Writes the synthesized `build.thorn` directly to disk.

## Byte-Identical Roundtripping

Thorn's forward emitter and reverse decompiler are verified against each other:

1. A project emits `build.ninja` via Thorn.
2. `thorn decompile build.ninja -o build_recovered.thorn` generates a recovered spec.
3. Running `thorn build_recovered.thorn` regenerates a byte-identical `build.ninja`.

This ensures deterministic, lossless migration from legacy Ninja or Make setups into Thorn.
