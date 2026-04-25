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
# macOS and Linux — self-host with intron:
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh
git clone git@github.com:misut/intron.git && cd intron
intron install
eval "$(intron env)"
exon build --release
```

The Linux self-host flow relies on the `share/libc++/v1/std.cppm` module
that ships in the official `LLVM-<version>-Linux-X64.tar.xz` tarball,
together with the `-stdlib=libc++` / rpath flags that
`intron install llvm` writes into `etc/clang/<target>.cfg`. No
distribution-level libc++ package is required.

The binary will be at `.exon/release/intron`.

#### Windows (MSVC bootstrap)

From a regular PowerShell session:

```powershell
iwr -useb https://raw.githubusercontent.com/misut/intron/main/install.ps1 | iex
git clone https://github.com/misut/intron
cd intron
intron install
```

With the repo's `.intron.toml`, `intron install` will:

- reuse a compatible Visual Studio 2022 / Build Tools 2022 instance when one already exists
- modify an existing Visual Studio 2022 instance to add the C++ workload when needed
- install a dedicated Build Tools 2022 instance when no compatible instance exists

Use `intron env` to export the resolved environment into the current shell, or
`intron exec -- <command> [args...]` to run a one-off command with the same
resolved variables:

```powershell
Invoke-Expression ((intron env) -join "`n")
```

Once that environment is active, you can bootstrap intron itself from a normal shell:

```powershell
git clone https://github.com/misut/tomlcpp --branch v0.3.0
git clone https://github.com/misut/cppx --branch v1.4.0
cmake -G Ninja -S .github/cmake -B build `
  -DCMAKE_BUILD_TYPE=Release `
  -DTOMLCPP_DIR=..\tomlcpp `
  -DCPPX_DIR=..\cppx
cmake --build build
.\build\intron.exe help
```

Or keep the current shell untouched and run the same flow through `intron exec`:

```powershell
git clone https://github.com/misut/tomlcpp --branch v0.3.0
git clone https://github.com/misut/cppx --branch v1.4.0
intron exec -- cmake -G Ninja -S .github/cmake -B build `
  -DCMAKE_BUILD_TYPE=Release `
  -DTOMLCPP_DIR=..\tomlcpp `
  -DCPPX_DIR=..\cppx
intron exec -- cmake --build build
intron exec -- .\build\intron.exe help
```

Requirements: Windows x64, Visual Studio 2022 channel support, CMake 3.30+, Ninja.

### Windows developer environment

When `msvc` is selected as the default toolchain on Windows, `intron env` prints a full developer environment instead of just `CC` and `CXX`:

```powershell
intron install msvc 2022
intron default msvc 2022 --platform windows
intron env
```

The output includes:

- `$env:PATH` with the MSVC `Hostx64/x64` bin directory
- `$env:PATH` also gets the installed Windows 10/11 SDK `bin/<version>/<arch>` directory so `rc.exe`, `mt.exe`, and `signtool.exe` resolve without a separately-opened developer shell
- `$env:CC` / `$env:CXX` pointing to `cl.exe`
- `$env:INCLUDE`, `$env:LIB`, and `$env:LIBPATH` from `vcvars64.bat`

The highest installed Windows SDK is picked by default. To pin a specific version, add it under `[toolchain.windows]` in `.intron.toml`:

```toml
[toolchain.windows]
msvc = "2022"
sdk = "10.0.26100.0"
```

This is intended to make direct execution of MSVC-built tools and Windows ASan binaries work from a regular PowerShell session, not just from a pre-opened Visual Studio developer shell.

`intron exec -- <command> [args...]` uses the same resolved MSVC-related variables and injects them directly into the child process environment, so commands such as `cl.exe` and `exon test` can run without manually `eval`-ing the shell output first.

On Windows, compiler selection now follows the effective defaults after project and
global config are merged:

- If `msvc` is configured, `intron env` / `intron exec` use `cl.exe` for `CC` and `CXX`.
- If `msvc` is not configured and `llvm` is configured, they use `clang-cl.exe`.
- Installed LLVM tools still stay on `PATH`, even when `cl.exe` is selected.

Before this change, a mixed configuration such as project `[toolchain] llvm = "22.1.2"`
plus global `[defaults.windows] msvc = "2022"` still selected `clang-cl.exe` for
`CC` / `CXX`. That older hybrid mode no longer happens implicitly. If you still want
it, override `CC` and `CXX` manually after `intron env` or before `intron exec`.

This is why a repository such as `phenotype` could differ between local Windows and
GitHub Actions Windows CI:

- Local Windows: the repo's common `.intron.toml` kept `llvm`, and the user's
  `~/.intron/config.toml` added `[defaults.windows] msvc = "2022"`, so `intron env`
  merged both sources into one effective Windows toolchain set.
- GitHub Actions Windows CI: the workflow used `ilammy/msvc-dev-cmd@v1` and then set
  `CC=cl` / `CXX=cl` directly, which bypassed intron's compiler selection logic.

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

On Windows PowerShell, use `Invoke-Expression ((intron env) -join "`n")`
instead of `eval "$(intron env)"`.

### Output modes

Human-facing commands use compact stage and status lines. Long-running
installation work is grouped by tool and phase, while script-oriented commands
such as `intron env`, `intron which`, and `intron exec -- ...` keep their
machine-readable output unchanged.

Set `INTRON_COLOR=auto|always|never` to control ANSI color in human output.
`NO_COLOR=1` disables color in auto mode.

`intron env` supports two additive output modes for cases where the default
shell-eval form is unwanted (for example, GitHub Actions, where reassigning
`PATH` via `Invoke-Expression` would freeze it at eval time and mask later
`$GITHUB_PATH` updates).

- `intron env` (default) — shell-eval form (`export KEY="..."` or
  `$env:KEY = "..."`). Intended for `eval "$(intron env)"` /
  `Invoke-Expression`. Unchanged from previous releases.
- `intron env --path-only` (alias `intron env --additive`) — emits every
  segment intron would prepend to `PATH`, one directory per line, with no
  quoting and no variable references. Non-`PATH` variables are suppressed.
  Pipe directly into `$GITHUB_PATH` on GitHub Actions:

  ```powershell
  intron env --path-only | ForEach-Object { $_ >> $env:GITHUB_PATH }
  ```

- `intron env --github` — emits `path=<dir>` for every `PATH` segment and
  `env=<KEY>=<value>` for every other variable. Route the two streams with a
  single-pass switch:

  ```powershell
  intron env --github | ForEach-Object {
    if ($_ -match '^path=(.+)$')    { $matches[1] >> $env:GITHUB_PATH }
    elseif ($_ -match '^env=(.+)$') { $matches[1] >> $env:GITHUB_ENV }
  }
  ```

`intron exec -- <command>` is unaffected; it always injects the resolved
variables directly into the child process environment.

## Commands

| Command | Description |
|---------|-------------|
| `intron install [tool] [version]` | Install toolchain(s) (reads `.intron.toml` if no args) |
| `intron remove <tool> <version>` | Remove an installed toolchain |
| `intron list` | List installed toolchains |
| `intron which <binary>` | Print absolute path to a binary |
| `intron default <tool> <version> [--platform <name>]` | Set global default version |
| `intron use [tool] [version] [--platform <name>]` | Set project toolchain in `.intron.toml` |
| `intron env [--path-only\|--github]` | Print environment variables (`eval "$(intron env)"` or PowerShell `Invoke-Expression ((intron env) -join "`n")`) |
| `intron exec -- <command> [args...]` | Run a command with the resolved intron environment |
| `intron update [tool]` | Check for newer versions |
| `intron upgrade [tool]` | Upgrade tools to latest |
| `intron self-update` | Update intron itself |
| `intron help` | Show usage information |

For MSVC on Windows, `intron install msvc 2022` keeps its provisioning role: it
ensures that a compatible Visual Studio 2022 / Build Tools 2022 instance has the
C++ workload and can be used by `intron env` / `intron exec`.

Use `intron update msvc` to compare the single MSVC instance currently selected by
intron against that instance's configured update channel, and `intron upgrade msvc`
to apply the available servicing update to that same selected instance.

## Supported Tools

| Tool | Source | Binaries |
|------|--------|----------|
| LLVM | [llvm/llvm-project](https://github.com/llvm/llvm-project/releases) | clang, clang++, lld, lldb, ... |
| CMake | [Kitware/CMake](https://github.com/Kitware/CMake/releases) | cmake, ctest, cpack |
| MSVC | Visual Studio 2022 / Build Tools 2022 | cl.exe, link.exe, vcvars64.bat (system tool) |
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
intron use msvc 2022 --platform windows
intron install                # install all from .intron.toml
```

For shared repositories, keep portable tools in `[toolchain]` and put platform-only
tools under `[toolchain.<platform>]`. `intron use` without arguments preserves that
layout when your defaults already use platform sections.

This creates `.intron.toml` in the current directory:

```toml
[toolchain]
cmake = "4.3.1"
ninja = "1.13.2"

[toolchain.macos]
llvm = "22.1.2"

[toolchain.linux]
llvm = "22.1.2"

[toolchain.windows]
msvc = "2022"
```

Project config is used by `install` (when called without args), `which`, `env`,
`exec`, `list`, and `update`.

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
