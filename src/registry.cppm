export module registry;
import std;

export namespace registry {

struct ToolInfo {
    std::string name;
    std::string version;
    std::string url;           // 다운로드 URL
    std::string archive_type;  // "tar.xz", "tar.gz", "zip"
};

std::string detect_platform() {
    // macOS arm64 / x86_64
    #if defined(__aarch64__) || defined(__arm64__)
    return "aarch64-apple-darwin";
    #elif defined(__x86_64__)
    return "x86_64-apple-darwin";
    #elif defined(__linux__) && defined(__aarch64__)
    return "aarch64-linux-gnu";
    #elif defined(__linux__) && defined(__x86_64__)
    return "x86_64-linux-gnu";
    #else
    return "unknown";
    #endif
}

ToolInfo resolve(std::string_view tool, std::string_view version) {
    auto platform = detect_platform();

    if (tool == "llvm") {
        // LLVM GitHub releases: llvm/llvm-project
        // 패턴: LLVM-{version}-{platform}.tar.xz
        auto url = std::format(
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-{}/LLVM-{}-{}.tar.xz",
            version, version, platform);
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = url,
            .archive_type = "tar.xz",
        };
    }

    if (tool == "cmake") {
        // CMake GitHub releases
        // macOS: cmake-{version}-macos-universal.tar.gz
        // Linux: cmake-{version}-linux-{arch}.tar.gz
        std::string filename;
        if (platform.contains("apple")) {
            filename = std::format("cmake-{}-macos-universal.tar.gz", version);
        } else if (platform.contains("linux") && platform.contains("aarch64")) {
            filename = std::format("cmake-{}-linux-aarch64.tar.gz", version);
        } else {
            filename = std::format("cmake-{}-linux-x86_64.tar.gz", version);
        }
        auto url = std::format(
            "https://github.com/Kitware/CMake/releases/download/v{}/{}", version, filename);
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = url,
            .archive_type = "tar.gz",
        };
    }

    if (tool == "ninja") {
        // Ninja GitHub releases
        std::string filename;
        if (platform.contains("apple")) {
            filename = "ninja-mac.zip";
        } else {
            filename = "ninja-linux.zip";
        }
        auto url = std::format(
            "https://github.com/ninja-build/ninja/releases/download/v{}/{}", version, filename);
        return {
            .name = std::string{tool},
            .version = std::string{version},
            .url = url,
            .archive_type = "zip",
        };
    }

    throw std::runtime_error(std::format("unknown tool: {}", tool));
}

} // namespace registry
