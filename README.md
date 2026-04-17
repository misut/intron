# Intron

A toolchain manager for C++. Inspired by rustup.

## Supported Platforms

| Platform | Status |
|----------|--------|
| macOS ARM64 (Apple Silicon) | Supported |
| Linux x86_64 | Supported |
| Linux ARM64 | Supported |
| Windows x86_64 (MSVC) | Supported |

## Installation

### Script

macOS / Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh
```

Windows (PowerShell):

```powershell
iwr -useb https://raw.githubusercontent.com/misut/intron/main/install.ps1 | iex
```

### mise

```sh
mise install "vfox:misut/mise-intron@latest"
mise use "vfox:misut/mise-intron@latest"
```

### Build from source

Requires [exon](https://github.com/misut/exon) and a C++23 toolchain with
`import std;` support (clang with libc++ modules).

```sh
# macOS — self-host with intron:
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh
git clone git@github.com:misut/intron.git && cd intron
intron install llvm 22.1.2
intron install cmake 4.3.1
intron install ninja 1.13.2
eval "$(intron env)"
exon build --release

# Linux (Ubuntu 24.04) — use apt LLVM 20 (modules are not available in
# LLVM's pre-built Linux tarballs yet):
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 20
sudo apt-get install -y libc++-20-dev libc++abi-20-dev ninja-build
pip install cmake --break-system-packages
export PATH="/usr/lib/llvm-20/bin:$PATH"
exon build --release
```

The binary will be at `.exon/release/intron`.

#### Windows (MSVC bootstrap)

From a regular PowerShell session:

```powershell
iwr -useb https://raw.githubusercontent.com/misut/intron/main/install.ps1 | iex
intron install msvc 2022
intron default msvc 2022
intron env
```

`intron install msvc 2022` will:

- reuse a compatible Visual Studio 2022 / Build Tools 2022 instance when one already exists
- modify an existing Visual Studio 2022 instance to add the C++ workload when needed
- install a dedicated Build Tools 2022 instance when no compatible instance exists

Once `intron env` is applied, you can bootstrap intron itself from a normal shell:

```powershell
git clone https://github.com/misut/intron
git clone https://github.com/misut/tomlcpp --branch v0.3.0
git clone https://github.com/misut/cppx --branch v1.1.0
cd intron
cmake -G Ninja -S .github/cmake -B build `
  -DCMAKE_BUILD_TYPE=Release `
  -DTOMLCPP_DIR=..\tomlcpp `
  -DCPPX_DIR=..\cppx
cmake --build build
.\build\intron.exe help
```

Requirements: Windows x64, Visual Studio 2022 channel support, CMake 3.30+, Ninja.

### Windows developer environment

When `msvc` is selected as the default toolchain on Windows, `intron env` prints a full developer environment instead of just `CC` and `CXX`:

```powershell
intron install msvc 2022
intron default msvc 2022
intron env
```

The output includes:

- `$env:PATH` with the MSVC `Hostx64/x64` bin directory
- `$env:CC` / `$env:CXX` pointing to `cl.exe`
- `$env:INCLUDE`, `$env:LIB`, and `$env:LIBPATH` from `vcvars64.bat`

This is intended to make direct execution of MSVC-built tools and Windows ASan binaries work from a regular PowerShell session, not just from a pre-opened Visual Studio developer shell.

`intron exec -- <command> [args...]` uses the same resolved MSVC-related variables and injects them directly into the child process environment, so commands such as `cl.exe` and `exon test` can run without manually `eval`-ing the shell output first.

## Quick Start

```sh
intron install llvm 22.1.2
intron install cmake 4.3.1
intron install ninja 1.13.2
intron default llvm 22.1.2
intron default cmake 4.3.1
intron default ninja 1.13.2
eval "$(intron env)"
clang++ --version
intron exec -- cmake --version
intron exec -- exon test
```

## Commands

| Command | Description |
|---------|-------------|
| `intron install [tool] [version]` | Install toolchain(s) (reads `.intron.toml` if no args) |
| `intron remove <tool> <version>` | Remove an installed toolchain |
| `intron list` | List installed toolchains |
| `intron which <binary>` | Print absolute path to a binary |
| `intron default <tool> <version>` | Set global default version |
| `intron use [tool] [version]` | Set project toolchain in `.intron.toml` |
| `intron env` | Print environment variables (`eval "$(intron env)"`) |
| `intron exec -- <command> [args...]` | Run a command with the resolved intron environment |
| `intron update` | Check for newer versions |
| `intron upgrade [tool]` | Upgrade tools to latest |
| `intron self-update` | Update intron itself |
| `intron help` | Show usage information |

## Supported Tools

| Tool | Source | Binaries |
|------|--------|----------|
| LLVM | [llvm/llvm-project](https://github.com/llvm/llvm-project/releases) | clang, clang++, lld, lldb, ... |
| CMake | [Kitware/CMake](https://github.com/Kitware/CMake/releases) | cmake, ctest, cpack |
| Ninja | [ninja-build/ninja](https://github.com/ninja-build/ninja/releases) | ninja |
| wasi-sdk | [WebAssembly/wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases) | WASI cross-compiler (via `$WASI_SDK_PATH`) |
| wasmtime | [bytecodealliance/wasmtime](https://github.com/bytecodealliance/wasmtime/releases) | wasmtime |

### WebAssembly (WASI) compilation

wasi-sdk is exposed via the `WASI_SDK_PATH` environment variable (not `PATH`) to avoid
conflicts with the native LLVM clang.

```sh
intron install wasi-sdk 32
intron default wasi-sdk 32
eval "$(intron env)"
echo $WASI_SDK_PATH
$WASI_SDK_PATH/bin/clang --sysroot=$WASI_SDK_PATH/share/wasi-sysroot hello.c -o hello.wasm
# Or via the bundled CMake toolchain file:
cmake -DCMAKE_TOOLCHAIN_FILE=$WASI_SDK_PATH/share/cmake/wasi-sdk.cmake ...
```

## Features

- **Download caching** — archives are cached in `~/.intron/downloads/` and reused on reinstall
- **Checksum verification** — SHA-256 verification for tools that provide checksums (CMake)
- **Atomic installs** — extracts to staging directory, then atomically renames to final path
- **Project config** — pin toolchain versions per project with `.intron.toml`
- **Auto clang config** — generates clang configuration files on macOS for SDK and libc++ setup

## Project Configuration

Use `intron use` to generate `.intron.toml` from current defaults:

```sh
intron use                    # pin all defaults
intron use llvm 22.1.2        # pin specific tool
intron install                # install all from .intron.toml
```

This creates `.intron.toml` in the current directory:

```toml
[toolchain]
llvm = "22.1.2"
cmake = "4.3.1"
ninja = "1.13.2"
wasi-sdk = "32"
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
    ├── ninja/1.13.2/ninja
    └── wasi-sdk/32/bin/clang
```

## License

MIT
