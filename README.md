# Intron

A toolchain manager for C++. Inspired by rustup.

## Supported Platforms

| Platform | Status |
|----------|--------|
| macOS ARM64 (Apple Silicon) | Supported |
| Linux x86_64 | Supported |
| Linux ARM64 | Supported |
| Windows x86_64 | Supported |

## Installation

### Script (macOS/Linux)

```sh
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh
```

### Script (Windows PowerShell)

```powershell
irm https://raw.githubusercontent.com/misut/intron/main/install.ps1 | iex
```

### mise

```sh
mise plugin add intron https://github.com/misut/mise-intron.git
mise install intron@latest
mise use intron@latest
```

### Build from source

Requires [Homebrew LLVM](https://formulae.brew.sh/formula/llvm) (macOS) or LLVM 20+ (Linux) and [exon](https://github.com/misut/exon).

```sh
# macOS
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
