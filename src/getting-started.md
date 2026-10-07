# Getting Started

## Installation

### Pre-built Nightly Binaries

Pre-compiled nightly binaries for Linux (`x86_64`) are automatically published via GitHub Actions. You can download the latest tarball directly from the [Releases](https://github.com/abit-foggy/thorn/releases/tag/nightly) page.

Each release includes:
- `thorn`: The standalone executable binary.
- `libthorn_core.a`: The pre-built core static library.
- `include/`: Public C header interfaces.

To install manually:

```bash
tar -xzf thorn-x86_64-linux-nightly.tar.gz
sudo install -m 755 thorn /usr/local/bin/thorn
```

### Building From Source

Thorn requires a C99 compiler (`cc`), GNU `make`, and vendored `pith` submodules.

```bash
# Clone the repository with submodules
git clone --recurse-submodules https://github.com/abit-foggy/thorn.git
cd thorn

# Build Thorn (outputs to out/thorn)
make

# Run the acceptance and verification suite
make check
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
engine.add_source("myapp", "src/arch/boot.S")
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
samu
# or
ninja
```

To generate a Makefile instead, specify `--engine make` on the command line or configure `engine.backend("make")` in `build.thorn`:

```bash
thorn --engine make
make
```
