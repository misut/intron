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
    check(info.url.contains("LLVM-22.1.2-"), "llvm url contains filename");
    check(!info.strip_prefix.empty(), "llvm strip_prefix not empty");
    check(info.checksum_url.empty(), "llvm has no checksum url");
}

void test_resolve_cmake() {
    auto info = registry::resolve("cmake", "4.0.3");
    check(info.name == "cmake", "cmake name");
    check(info.version == "4.0.3", "cmake version");
    check(info.archive_type == "tar.gz", "cmake archive_type");
    check(info.url.contains("/v4.0.3/"), "cmake url contains version");
    check(!info.strip_prefix.empty(), "cmake strip_prefix not empty");
    check(!info.checksum_url.empty(), "cmake has checksum url");
    check(info.checksum_url.contains("SHA-256"), "cmake checksum url contains SHA-256");
}

void test_resolve_ninja() {
    auto info = registry::resolve("ninja", "1.12.1");
    check(info.name == "ninja", "ninja name");
    check(info.version == "1.12.1", "ninja version");
    check(info.archive_type == "zip", "ninja archive_type");
    check(info.url.contains("/v1.12.1/"), "ninja url contains version");
    check(info.strip_prefix.empty(), "ninja strip_prefix empty");
}


void test_latest_release_api() {
    auto llvm = registry::latest_release_api("llvm");
    check(llvm.contains("llvm/llvm-project"), "llvm api url");

    auto cmake = registry::latest_release_api("cmake");
    check(cmake.contains("Kitware/CMake"), "cmake api url");

    auto ninja = registry::latest_release_api("ninja");
    check(ninja.contains("ninja-build/ninja"), "ninja api url");

    auto intron = registry::latest_release_api("intron");
    check(intron.contains("misut/intron"), "intron api url");
}

void test_detect_platform() {
    auto plat = registry::detect_platform();
#if defined(__APPLE__)
    check(plat.os == registry::OS::macOS, "platform is macOS");
#elif defined(__linux__)
    check(plat.os == registry::OS::Linux, "platform is Linux");
#endif
#if defined(__aarch64__) || defined(__arm64__)
    check(plat.arch == registry::Arch::ARM64, "arch is ARM64");
#elif defined(__x86_64__)
    check(plat.arch == registry::Arch::X64, "arch is X64");
#endif
}

int main() {
    test_resolve_llvm();
    test_resolve_cmake();
    test_resolve_ninja();
    test_latest_release_api();
    test_detect_platform();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_registry: all tests passed");
    return 0;
}
