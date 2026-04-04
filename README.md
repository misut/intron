# Intron

A toolchain manager for C++. Inspired by rustup.

## Supported Platforms

| Platform | Status |
|----------|--------|
| macOS ARM64 (Apple Silicon) | Supported |
| Linux x86_64 | Supported |
| Linux ARM64 | Supported |
| Windows x86_64 | Experimental |

## Installation

### Script

```sh
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh
```

### mise

```sh
mise plugin add intron https://github.com/misut/mise-intron.git
mise install intron@latest
mise use intron@latest
```

### Build from source

Requires [intron](https://github.com/misut/intron) (for LLVM/CMake/Ninja) and [exon](https://github.com/misut/exon).

```sh
# macOS (self-hosting)
git clone git@github.com:misut/intron.git && cd intron
intron install llvm 22.1.2
intron install cmake 4.3.1
intron install ninja 1.13.2
eval "$(intron env)"
exon build --release

# Linux (Ubuntu 24.04)
# Install LLVM 20 from apt.llvm.org, then:
export PATH="/usr/lib/llvm-20/bin:$PATH"
exon build --release
```

The binary will be at `.exon/release/intron`.

## Quick Start

```sh
intron install llvm 22.1.2
intron install cmake 4.3.1
intron install ninja 1.12.1
intron default llvm 22.1.2
intron default cmake 4.3.1
intron default ninja 1.12.1
eval "$(intron env)"
clang++ --version
```

## Commands

| Command | Description |
|---------|-------------|
| `intron install <tool> <version>` | Download and install a toolchain |
| `intron remove <tool> <version>` | Remove an installed toolchain |
| `intron list` | List installed toolchains |
| `intron which <binary>` | Print absolute path to a binary |
| `intron default <tool> <version>` | Set default version |
| `intron env` | Print environment variables (`eval "$(intron env)"`) |
| `intron update` | Check for newer versions |
| `intron self-update` | Update intron itself |
| `intron help` | Show usage information |

## Supported Tools

| Tool | Source | Binaries |
|------|--------|----------|
| LLVM | [llvm/llvm-project](https://github.com/llvm/llvm-project/releases) | clang, clang++, lld, lldb, ... |
| CMake | [Kitware/CMake](https://github.com/Kitware/CMake/releases) | cmake, ctest, cpack |
| Ninja | [ninja-build/ninja](https://github.com/ninja-build/ninja/releases) | ninja |

## Features

- **Download caching** — archives are cached in `~/.intron/downloads/` and reused on reinstall
- **Checksum verification** — SHA-256 verification for tools that provide checksums (CMake)
- **Atomic installs** — extracts to staging directory, then atomically renames to final path
- **Project config** — pin toolchain versions per project with `.intron.toml`
- **Auto clang config** — generates clang configuration files on macOS for SDK and libc++ setup

## Project Configuration

Create `.intron.toml` in your project root to pin toolchain versions:

```toml
[toolchain]
llvm = "22.1.2"
cmake = "4.3.1"
ninja = "1.12.1"
```

Project config overrides global defaults for `which`, `env`, `list`, and `update`.

## Directory Layout

```
~/.intron/
├── config.toml                     # default versions
├── downloads/                      # cached archives
└── toolchains/
    ├── llvm/22.1.2/bin/clang++
    ├── cmake/4.3.1/bin/cmake
    └── ninja/1.12.1/ninja
```

## License

MIT
