# Getting Started

## Building Thorn

Thorn requires a C99 compiler (`cc`), `make`, and a built checkout of `pith` located adjacent at `../pith`.

To build Thorn:

```bash
# Ensure pith is built beside thorn
cd ../pith && make && cd ../thorn

# Build thorn binary (outputs to out/thorn)
make
```

To run the self-hosting test suite:

```bash
make check
# or directly:
./tests/verify.sh
```

## Creating Your First Project

A Thorn project is configured with a `build.thorn` file at the root of the project directory.

Create `build.thorn`:

```pith
engine.project("myapp")
engine.backend("ninja")

engine.add_target("myapp", engine.exe)
engine.add_source("myapp", "src/main.c")
engine.add_source("myapp", "src/util.c")
engine.add_include("myapp", "src/include")
engine.add_cflag("myapp", "-O2")
engine.add_cflag("myapp", "-Wall")
```

Generate build files:

```bash
thorn
```

This generates `build.ninja` in the current directory. You can then build your application using Ninja or Samurai:

```bash
ninja
# or
samu
```

To generate a Makefile instead, specify `--engine make` on the command line or configure `engine.backend("make")` in `build.thorn`:

```bash
thorn --engine make
make
```
