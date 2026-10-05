# Decompilation

Thorn includes a built-in reverse decompiler capable of reading an existing `build.ninja` or `Makefile` and synthesizing an equivalent `build.thorn` specification script.

## Usage

```bash
thorn decompile <build.ninja|Makefile>
```

The reconstructed specification is output directly to standard output.

## Reverse Engineering Capabilities

The decompiler performs heuristic analysis on backend rules and targets:

- **Target Recovery**: Detects targets, outputs, and classifies whether they are executables (`engine.exe`), static archives (`engine.static_lib`), or shared objects (`engine.shared_lib`).
- **Source Mapping**: Maps compiled object files back to their `.c` source files.
- **Flags & Includes**: Extracts `-I` include flags into `engine.add_include()` and compiler options into `engine.add_cflag()`.
- **Linker Flags**: Extracts library links (`-l`, `-L`) into `engine.add_ldflag()`.
- **Custom Commands**: Recovers custom build edges into `engine.add_command()`, transforming Make automatic variables (`$@`, `$<`) back into Thorn's portable tokens (`$out`, `$in`).
- **Backend Recognition**: Automatically emits `engine.backend("ninja")` or `engine.backend("make")` based on the ingested file format.

## Byte-Identical Roundtripping

Thorn's forward emitter and reverse decompiler are verified against each other:

1. A project emits `build.ninja` via Thorn.
2. `thorn decompile build.ninja > build_recovered.thorn` generates a recovered spec.
3. Running `thorn build_recovered.thorn` regenerates a byte-identical `build.ninja`.

This provides safe migration from existing Ninja or Make setups into Thorn.
