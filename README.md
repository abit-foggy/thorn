# thorn

A C and subsets meta-generator for Ninja and POSIX Make; written in C99

[![CI](https://github.com/abit-foggy/thorn/actions/workflows/ci.yml/badge.svg)](https://github.com/abit-foggy/thorn/actions)
[![Documentation](https://img.shields.io/badge/docs-mdBook-emerald.svg)](https://abit-foggy.github.io/thorn/)
[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)

## Installation

```bash
# Change TAG=v0.2.0 to TAG=nightly for the nightly build
curl -fsSL https://raw.githubusercontent.com/abit-foggy/thorn/main/install.sh | TAG=v0.2.0 bash
```

## Extensions

- [**thornk**](https://github.com/abit-foggy/thornk) — An extension of Thorn that converts Linux Kbuild trees to Ninja and `thorn.build` specifications.
- [**thorn-rs**](https://github.com/abit-foggy/thorn-rs) — Zero-cargo `#![no_std]` Rust static libraries, as a Thorn plugin.

## Plugins

Extensions ship as `.tpkg` bundles and load from a `thorn.toml` (or
`pith.toml`) manifest:

```toml
[plugins]
thorn-rs = "abit-foggy/thorn-rs"
```

```sh
thorn pkg sync     # installs from the github release, or from source
thorn              # plugins load; their namespaces work in build.thorn
```

`thorn pkg install <user/repo|git-url>` prefers a `.tpkg` release asset
and falls back to cloning and building the source. The installed
plugin's namespace (e.g. `rust.*`) is available inside `build.thorn`.

## Links

- **Documentation**: [https://abit-foggy.github.io/thorn/](https://abit-foggy.github.io/thorn/)
- **Repository**: [https://github.com/abit-foggy/thorn](https://github.com/abit-foggy/thorn)
- **Issue Tracker**: [https://github.com/abit-foggy/thorn/issues](https://github.com/abit-foggy/thorn/issues)

## Authorship

The majority of the code in this repository was written by an AI. All
architectural design was made by a human, and every change was
reviewed by both a human and an AI for flaws before it landed.

## License

BSD 2-Clause Simplified License. See [LICENSE](LICENSE) for details.
