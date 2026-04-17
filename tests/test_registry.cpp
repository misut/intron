import std;
import registry;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
}

void test_resolve_llvm() {
    auto info = registry::resolve("llvm", "22.1.2");
    check(info.name == "llvm", "llvm name");
    check(info.version == "22.1.2", "llvm version");
    check(info.archive_type == "tar.xz", "llvm archive_type");
    check(info.url.contains("llvmorg-22.1.2"), "llvm url contains tag");
#if defined(_WIN32)
    check(info.url.contains("clang+llvm-22.1.2-"), "llvm url contains filename");
#else
    check(info.url.contains("LLVM-22.1.2-"), "llvm url contains filename");
#endif
    check(!info.strip_prefix.empty(), "llvm strip_prefix not empty");
    check(info.checksum_url.empty(), "llvm has no checksum url");
}

void test_resolve_cmake() {
    auto info = registry::resolve("cmake", "4.0.3");
    check(info.name == "cmake", "cmake name");
    check(info.version == "4.0.3", "cmake version");
#if defined(_WIN32)
    check(info.archive_type == "zip", "cmake archive_type");
#else
    check(info.archive_type == "tar.gz", "cmake archive_type");
#endif
    check(info.url.contains("/v4.0.3/"), "cmake url contains version");
    check(!info.strip_prefix.empty(), "cmake strip_prefix not empty");
    check(!info.checksum_url.empty(), "cmake has checksum url");
    check(info.checksum_url.contains("SHA-256"), "cmake checksum url contains SHA-256");
}

void test_resolve_ninja() {
    auto plat = registry::detect_platform();
    if (plat.os == registry::OS::Linux && plat.arch == registry::Arch::ARM64) {
        // ARM64 Linux: should throw (no official binaries)
        bool threw = false;
        try {
            registry::resolve("ninja", "1.12.1");
        } catch (std::runtime_error const&) {
            threw = true;
        }
        check(threw, "ninja ARM64 Linux throws");
        return;
    }
    auto info = registry::resolve("ninja", "1.12.1");
    check(info.name == "ninja", "ninja name");
    check(info.version == "1.12.1", "ninja version");
    check(info.archive_type == "zip", "ninja archive_type");
    check(info.url.contains("/v1.12.1/"), "ninja url contains version");
    check(info.strip_prefix.empty(), "ninja strip_prefix empty");
}


void test_resolve_wasi_sdk() {
    auto info = registry::resolve("wasi-sdk", "32");
    check(info.name == "wasi-sdk", "wasi-sdk name");
    check(info.version == "32", "wasi-sdk version");
    check(info.archive_type == "tar.gz", "wasi-sdk archive_type");
    check(info.url.contains("/wasi-sdk-32/"), "wasi-sdk url contains tag");
    check(info.url.contains("WebAssembly/wasi-sdk"), "wasi-sdk url repo");
    check(info.url.ends_with(".tar.gz"), "wasi-sdk url ends with .tar.gz");
    check(info.strip_prefix.starts_with("wasi-sdk-32.0-"), "wasi-sdk strip_prefix format");
    check(info.checksum_url.empty(), "wasi-sdk has no checksum url");
}

void test_latest_release_api() {
    auto llvm = registry::latest_release_api("llvm");
    check(llvm.contains("llvm/llvm-project"), "llvm api url");

    auto cmake = registry::latest_release_api("cmake");
    check(cmake.contains("Kitware/CMake"), "cmake api url");

    auto ninja = registry::latest_release_api("ninja");
    check(ninja.contains("ninja-build/ninja"), "ninja api url");

    auto wasi_sdk = registry::latest_release_api("wasi-sdk");
    check(wasi_sdk.contains("WebAssembly/wasi-sdk"), "wasi-sdk api url");

    auto intron = registry::latest_release_api("intron");
    check(intron.contains("misut/intron"), "intron api url");
}

void test_resolve_msvc() {
    auto info = registry::resolve("msvc", "latest");
    check(info.name == "msvc", "msvc name");
    check(info.version == "2022", "msvc latest normalizes to 2022");
    check(info.url.empty(), "msvc url empty (system tool)");
    check(info.archive_type.empty(), "msvc archive_type empty");
    check(info.visual_studio.has_value(), "msvc has visual studio installer metadata");
    if (info.visual_studio) {
        check(info.visual_studio->bootstrapper_url.contains("vs_BuildTools.exe"),
              "msvc bootstrapper url points to build tools");
        check(info.visual_studio->product_id == "Microsoft.VisualStudio.Product.BuildTools",
              "msvc product id");
        check(info.visual_studio->channel_id == "VisualStudio.17.Release",
              "msvc channel id");
        check(info.visual_studio->workload_id == "Microsoft.VisualStudio.Workload.VCTools",
              "msvc workload id");
        check(info.visual_studio->install_path.string().contains("BuildTools"),
              "msvc install path contains BuildTools");
    }
    check(registry::is_system_tool("msvc"), "msvc is system tool");
    check(!registry::is_system_tool("llvm"), "llvm is not system tool");
}

void test_normalize_requested_version() {
    check(registry::normalize_requested_version("llvm", "22.1.2") == "22.1.2",
          "non-system tool version is unchanged");
    check(registry::normalize_requested_version("msvc", "latest") == "2022",
          "msvc latest aliases to 2022");
    check(registry::normalize_requested_version("msvc", "2022") == "2022",
          "msvc 2022 remains 2022");

    bool threw = false;
    try {
        (void)registry::normalize_requested_version("msvc", "2019");
    } catch (std::runtime_error const&) {
        threw = true;
    }
    check(threw, "unsupported msvc version throws");
}

void test_platform_name() {
    auto name = registry::platform_name();
#if defined(__APPLE__)
    check(name == "macos", "platform_name is macos");
#elif defined(__linux__)
    check(name == "linux", "platform_name is linux");
#elif defined(_WIN32)
    check(name == "windows", "platform_name is windows");
#endif
}

void test_detect_platform() {
    auto plat = registry::detect_platform();
#if defined(__APPLE__)
    check(plat.os == registry::OS::macOS, "platform is macOS");
#elif defined(__linux__)
    check(plat.os == registry::OS::Linux, "platform is Linux");
#elif defined(_WIN32)
    check(plat.os == registry::OS::Windows, "platform is Windows");
#endif
#if defined(__aarch64__) || defined(__arm64__)
    check(plat.arch == registry::Arch::ARM64, "arch is ARM64");
#elif defined(__x86_64__) || defined(_M_X64)
    check(plat.arch == registry::Arch::X64, "arch is X64");
#endif
}

int main() {
    test_resolve_llvm();
    test_resolve_cmake();
    test_resolve_ninja();
    test_resolve_wasi_sdk();
    test_resolve_msvc();
    test_normalize_requested_version();
    test_platform_name();
    test_latest_release_api();
    test_detect_platform();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_registry: all tests passed");
    return 0;
}
