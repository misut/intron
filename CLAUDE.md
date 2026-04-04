# Intron

C++ toolchain manager. Inspired by Rust's rustup. MIT license (misut).

## Build

Build with intron + exon. intron manages toolchains (LLVM, CMake, Ninja), exon handles the build.

```sh
intron install llvm 22.1.2
intron install cmake 4.3.1
intron install ninja 1.13.2
eval "$(intron env)"
exon build            # debug build → .exon/debug/intron
exon build --release  # release build → .exon/release/intron
```

## Code Style

- Use C++23 `import` instead of `#include`. For modern features and faster builds.
- No external libraries. Implement what's needed from scratch.

## Conventions

- Commits: [Conventional Commits](https://www.conventionalcommits.org/) format (`feat:`, `fix:`, `chore:`, etc.)
- Do not add Co-Authored-By to commits

## Repository

- Remote: `git@github.com:misut/intron.git`
- Default branch: `main`
