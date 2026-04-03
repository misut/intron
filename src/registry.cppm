export module registry;
import std;

export namespace registry {

struct ToolInfo {
    std::string name;
    std::string version;
    std::string url;
    std::string archive_type;  // "tar.xz", "tar.gz", "zip"
    std::string strip_prefix;  // 아카이브 내부 최상위 디렉토리
    std::string checksum_url;  // SHA-256 체크섬 파일 URL (비어있으면 검증 생략)
};

enum class OS { macOS, Linux };
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
#else
    #error "Unsupported platform"
#endif
}

ToolInfo resolve(std::string_view tool, std::string_view version) {
    auto plat = detect_platform();

    if (tool == "llvm") {
        // LLVM GitHub releases: LLVM-{ver}-{macOS-ARM64|macOS-X64|...}.tar.xz
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
        // CMake GitHub releases
        std::string filename;
        std::string prefix;
        if (plat.os == OS::macOS) {
            filename = std::format("cmake-{}-macos-universal.tar.gz", version);
            prefix = std::format("cmake-{}-macos-universal/CMake.app/Contents", version);
        } else if (plat.arch == Arch::ARM64) {
            filename = std::format("cmake-{}-linux-aarch64.tar.gz", version);
            prefix = std::format("cmake-{}-linux-aarch64", version);
        } else {
            filename = std::format("cmake-{}-linux-x86_64.tar.gz", version);
            prefix = std::format("cmake-{}-linux-x86_64", version);
        }
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = std::format(
                "https://github.com/Kitware/CMake/releases/download/v{}/{}",
                version, filename),
            .archive_type = "tar.gz",
            .strip_prefix = prefix,
            .checksum_url = std::format(
                "https://github.com/Kitware/CMake/releases/download/v{}/cmake-{}-SHA-256.txt",
                version, version),
        };
    }

    if (tool == "ninja") {
        // Ninja GitHub releases: ninja-mac.zip / ninja-linux.zip
        auto filename = plat.os == OS::macOS ? "ninja-mac.zip" : "ninja-linux.zip";
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

// GitHub API에서 최신 릴리즈 태그를 가져오기 위한 URL
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
    throw std::runtime_error(std::format("unknown tool: {}", tool));
}

constexpr std::array<std::string_view, 3> supported_tools = {"cmake", "llvm", "ninja"};

} // namespace registry
