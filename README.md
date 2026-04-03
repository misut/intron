# Intron

A toolchain manager for C++. Inspired by rustup.

## Installation

### Script

```sh
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh
```

### mise

```sh
mise plugin add intron https://github.com/misut/mise-intron.git
mise install intron@0.0.0
mise use intron@0.0.0
```

### Build from source

Requires [Homebrew LLVM](https://formulae.brew.sh/formula/llvm) and [exon](https://github.com/misut/exon).

```sh
brew install llvm
git clone git@github.com:misut/intron.git
cd intron
export PATH="$(brew --prefix llvm)/bin:$PATH"
exon build --release
```

The binary will be at `.exon/release/intron`.

## Quick Start

```sh
intron install llvm 22.1.2
intron install cmake 4.3.1
intron install ninja 1.12.1
intron default llvm 22.1.2
intron which clang++
```

```
Installed llvm 22.1.2 to ~/.intron/toolchains/llvm/22.1.2
Installed cmake 4.3.1 to ~/.intron/toolchains/cmake/4.3.1
Installed ninja 1.12.1 to ~/.intron/toolchains/ninja/1.12.1
Set llvm default to 22.1.2
/Users/you/.intron/toolchains/llvm/22.1.2/bin/clang++
```

## Commands

| Command | Description |
|---------|-------------|
| `intron install <tool> <version>` | Download and install a toolchain |
| `intron remove <tool> <version>` | Remove an installed toolchain |
| `intron list` | List installed toolchains |
| `intron which <binary>` | Print absolute path to a binary |
| `intron default <tool> <version>` | Set default version |
| `intron help` | Show usage information |

## Supported Tools

| Tool | Source | Binaries |
|------|--------|----------|
| LLVM | [llvm/llvm-project](https://github.com/llvm/llvm-project/releases) | clang, clang++, lld, lldb, ... |
| CMake | [Kitware/CMake](https://github.com/Kitware/CMake/releases) | cmake, ctest, cpack |
| Ninja | [ninja-build/ninja](https://github.com/ninja-build/ninja/releases) | ninja |

## Directory Layout

```
~/.intron/
├── config.toml                     # default versions
└── toolchains/
    ├── llvm/22.1.2/bin/clang++
    ├── cmake/4.3.1/bin/cmake
    └── ninja/1.12.1/ninja
```

## License

MIT
