# Security Policy

## Supported Versions

Security updates are applied to the active release stream.

| Version | Supported |
| ------- | --------- |
| 0.2.x   | Yes       |
| nightly | Yes       |
| < 0.2.0 | No        |

## Reporting a Vulnerability

The thorn project takes security vulnerabilities seriously. If you discover a vulnerability or security issue, please do not file a public issue on GitHub.

Please report security vulnerabilities through GitHub Private Vulnerability Reporting:
Navigate to the Security tab of the repository on GitHub, select "Report a vulnerability", and provide the details.

### What to Include in Your Report

To help us triage and resolve the issue quickly, please include:
- A clear description of the vulnerability and its potential impact.
- Affected component: the build graph and host API (`src/thorn_engine.c`), the backend emitters (`build.ninja` / `Makefile` writers), the reverse decompiler (`src/thorn_decompile.c`), or the CLI and spec evaluation (`src/thorn_main.c`).
- Step-by-step reproduction instructions, including a sample `build.thorn` or the build file that triggers the issue.
- Target platform and architecture (e.g. Linux x86_64, macOS aarch64).
- Any proposed mitigations or proof-of-concept files.

## Response and Disclosure Process

1. **Acknowledgment**: We aim to acknowledge receipt of security reports within 48 hours.
2. **Investigation & Triage**: We will confirm the vulnerability, determine its severity, and provide regular progress updates.
3. **Patch Development**: Fixes are developed in private branches and tested against the thorn acceptance suite.
4. **Coordinated Disclosure**: Once a fix is verified and ready for release, we will coordinate public disclosure and publish a security advisory with credit to the reporter.

## Security Architecture & Invariants

thorn is designed with several defensive security principles in mind:
- **Emitters write files only**: backend generation never executes commands. All command execution happens later, in samu/ninja/make, from the generated files.
- **Decompiler ingestion**: `build.ninja` and `Makefile` parsing uses bounded, fixed-size buffers; hostile input fails with a diagnostic, never with memory unsafety.
- **Deterministic output**: identical graph and environment produce byte-identical backends. No timestamps, no hidden environment probing beyond the declared toolchain selection.
- **Trust model**: `build.thorn` is project code evaluated by the embedded pith runtime, exactly like a `Makefile` or `CMakeLists.txt`. Project files are trusted inputs; thorn never evaluates files from outside the project tree.
- **Subprocess handling**: `pkg_config` runs `pkg-config` with the package name shell-quoted; nothing else spawns processes at configure time.
- **Fallback link objects**: only objects registered by the thorn binary itself are linked into fallback executables built by the embedded pith runtime.

## Authorship & Review

The majority of the code in this repository was written by an AI. All
architectural design was made by a human, and every change was
reviewed by both a human and an AI for flaws before it landed.
Security-sensitive components (backend ingestion, file emission,
process handling, and the embed boundary) receive additional review
scrutiny.
