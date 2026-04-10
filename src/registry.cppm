export module registry;
import std;

export namespace registry {

struct ToolInfo {
    std::string name;
    std::string version;
    std::string url;
    std::string archive_type;  // "tar.xz", "tar.gz", "zip"
    std::string strip_prefix;  // top-level directory inside archive
    std::string checksum_url;  // SHA-256 checksum file URL (empty to skip verification)
};

enum class OS { macOS, Linux, Windows };
enum class Arch { ARM64, X64 };

struct Platform {
    OS os;
    Arch arch;
};

Platform detect_platform() {
#if defined(__APPLE__)
    #if defined(__aarch64__) || defined(__arm64__)
    return {OS::macOS, Arch::ARM64};
    #else
    return {OS::macOS, Arch::X64};
    #endif
#elif defined(__linux__)
    #if defined(__aarch64__)
    return {OS::Linux, Arch::ARM64};
    #else
    return {OS::Linux, Arch::X64};
    #endif
#elif defined(_WIN32)
    #if defined(_M_ARM64)
    return {OS::Windows, Arch::ARM64};
    #else
    return {OS::Windows, Arch::X64};
    #endif
#else
    #error "Unsupported platform"
#endif
}

ToolInfo resolve(std::string_view tool, std::string_view version) {
    auto plat = detect_platform();

    if (tool == "llvm") {
        if (plat.os == OS::Windows) {
            // clang+llvm-{ver}-x86_64-pc-windows-msvc.tar.xz
            auto arch_str = plat.arch == Arch::ARM64
                ? "aarch64-pc-windows-msvc" : "x86_64-pc-windows-msvc";
            auto filename = std::format("clang+llvm-{}-{}.tar.xz", version, arch_str);
            return {
                .name = std::string{tool},
                .version = std::string{version},
                .url = std::format(
                    "https://github.com/llvm/llvm-project/releases/download/llvmorg-{}/{}",
                    version, filename),
                .archive_type = "tar.xz",
                .strip_prefix = std::format("clang+llvm-{}-{}", version, arch_str),
            };
        }
        // macOS / Linux: LLVM-{ver}-{macOS-ARM64|Linux-X64|...}.tar.xz
        auto plat_str = std::format("{}-{}",
            plat.os == OS::macOS ? "macOS" : "Linux",
            plat.arch == Arch::ARM64 ? "ARM64" : "X64");
        auto filename = std::format("LLVM-{}-{}.tar.xz", version, plat_str);
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = std::format(
                "https://github.com/llvm/llvm-project/releases/download/llvmorg-{}/{}",
                version, filename),
            .archive_type = "tar.xz",
            .strip_prefix = std::format("LLVM-{}-{}", version, plat_str),
        };
    }

    if (tool == "cmake") {
        std::string filename;
        std::string prefix;
        if (plat.os == OS::macOS) {
            filename = std::format("cmake-{}-macos-universal.tar.gz", version);
            prefix = std::format("cmake-{}-macos-universal/CMake.app/Contents", version);
        } else if (plat.os == OS::Windows) {
            filename = std::format("cmake-{}-windows-x86_64.zip", version);
            prefix = std::format("cmake-{}-windows-x86_64", version);
        } else if (plat.arch == Arch::ARM64) {
            filename = std::format("cmake-{}-linux-aarch64.tar.gz", version);
            prefix = std::format("cmake-{}-linux-aarch64", version);
        } else {
            filename = std::format("cmake-{}-linux-x86_64.tar.gz", version);
            prefix = std::format("cmake-{}-linux-x86_64", version);
        }
        auto archive = (plat.os == OS::Windows) ? "zip" : "tar.gz";
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = std::format(
                "https://github.com/Kitware/CMake/releases/download/v{}/{}",
                version, filename),
            .archive_type = std::string{archive},
            .strip_prefix = prefix,
            .checksum_url = std::format(
                "https://github.com/Kitware/CMake/releases/download/v{}/cmake-{}-SHA-256.txt",
                version, version),
        };
    }

    if (tool == "wasi-sdk") {
        auto arch_str = plat.arch == Arch::ARM64 ? "arm64" : "x86_64";
        std::string_view os_str;
        switch (plat.os) {
            case OS::macOS:   os_str = "macos"; break;
            case OS::Linux:   os_str = "linux"; break;
            case OS::Windows: os_str = "windows"; break;
        }
        auto dirname = std::format("wasi-sdk-{}.0-{}-{}", version, arch_str, os_str);
        auto filename = std::format("{}.tar.gz", dirname);
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = std::format(
                "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-{}/{}",
                version, filename),
            .archive_type = "tar.gz",
            .strip_prefix = dirname,
        };
    }

    if (tool == "wasmtime") {
        std::string arch_str, os_str, archive;
        if (plat.os == OS::macOS) {
            arch_str = plat.arch == Arch::ARM64 ? "aarch64" : "x86_64";
            os_str = "macos"; archive = "tar.xz";
        } else if (plat.os == OS::Windows) {
            arch_str = "x86_64"; os_str = "windows"; archive = "zip";
        } else {
            arch_str = plat.arch == Arch::ARM64 ? "aarch64" : "x86_64";
            os_str = "linux"; archive = "tar.xz";
        }
        auto dirname = std::format("wasmtime-v{}-{}-{}", version, arch_str, os_str);
        auto filename = std::format("{}.{}", dirname, archive);
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = std::format(
                "https://github.com/bytecodealliance/wasmtime/releases/download/v{}/{}",
                version, filename),
            .archive_type = std::string{archive},
            .strip_prefix = dirname,
        };
    }

    if (tool == "ninja") {
        std::string filename;
        if (plat.os == OS::macOS) {
            filename = "ninja-mac.zip";
        } else if (plat.os == OS::Windows) {
            filename = "ninja-win.zip";
        } else if (plat.arch == Arch::ARM64) {
            throw std::runtime_error(
                "ninja does not provide ARM64 Linux binaries.\n"
                "hint: install via system package manager (e.g. apt install ninja-build)");
        } else {
            filename = "ninja-linux.zip";
        }
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = std::format(
                "https://github.com/ninja-build/ninja/releases/download/v{}/{}",
                version, filename),
            .archive_type = "zip",
            .strip_prefix = {},
        };
    }

    throw std::runtime_error(std::format("unknown tool: {}", tool));
}

// GitHub API URL for latest release tag
std::string latest_release_api(std::string_view tool) {
    if (tool == "llvm") {
        return "https://api.github.com/repos/llvm/llvm-project/releases/latest";
    }
    if (tool == "cmake") {
        return "https://api.github.com/repos/Kitware/CMake/releases/latest";
    }
    if (tool == "ninja") {
        return "https://api.github.com/repos/ninja-build/ninja/releases/latest";
    }
    if (tool == "wasi-sdk") {
        return "https://api.github.com/repos/WebAssembly/wasi-sdk/releases/latest";
    }
    if (tool == "wasmtime") {
        return "https://api.github.com/repos/bytecodealliance/wasmtime/releases/latest";
    }
    if (tool == "intron") {
        return "https://api.github.com/repos/misut/intron/releases/latest";
    }
    throw std::runtime_error(std::format("unknown tool: {}", tool));
}

constexpr std::array<std::string_view, 5> supported_tools = {"cmake", "llvm", "ninja", "wasi-sdk", "wasmtime"};

// Platform triple for release binaries
std::string platform_triple() {
    auto plat = detect_platform();
    if (plat.os == OS::macOS) {
        return plat.arch == Arch::ARM64 ? "aarch64-apple-darwin" : "x86_64-apple-darwin";
    }
    if (plat.os == OS::Windows) {
        return "x86_64-pc-windows-msvc";
    }
    return plat.arch == Arch::ARM64 ? "aarch64-linux-gnu" : "x86_64-linux-gnu";
}

} // namespace registry
