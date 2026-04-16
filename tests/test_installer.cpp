import std;
import installer;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
}

void test_toolchain_path() {
    auto path = installer::toolchain_path("llvm", "22.1.2");
    auto str = path.string();
    check(path.generic_string().contains(".intron/toolchains/llvm/22.1.2"), "llvm toolchain path");

    auto path2 = installer::toolchain_path("ninja", "1.12.1");
    check(path2.generic_string().contains(".intron/toolchains/ninja/1.12.1"), "ninja toolchain path");
}

void test_intron_home() {
    auto home = installer::intron_home();
    check(home.string().contains(".intron"), "intron home contains .intron");
    check(std::filesystem::exists(home), "intron home exists");
}

void test_which_not_installed() {
    // 설치되지 않은 도구는 nullopt 반환
    auto result = installer::which("clang++", "llvm", "99.99.99");
    check(!result.has_value(), "which returns nullopt for missing tool");
}

void test_list_installed_empty_version() {
    // 존재하지 않는 버전 디렉토리
    auto result = installer::which("ninja", "ninja", "99.99.99");
    check(!result.has_value(), "which returns nullopt for missing version");
}

void test_msvc_helper_paths() {
    auto root = std::filesystem::path{"C:/VS/VC/Tools/MSVC/14.40.33807"};
    auto bin = installer::msvc_bin_path(root);
    auto asan = installer::msvc_asan_runtime_path(root);

    check(bin.generic_string().ends_with("VC/Tools/MSVC/14.40.33807/bin/Hostx64/x64"),
          "msvc bin path uses Hostx64/x64");
    check(asan.generic_string().ends_with(
              "VC/Tools/MSVC/14.40.33807/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll"),
          "msvc asan runtime path points at clang_rt dll");
}

void test_msvc_environment_smoke() {
#ifdef _WIN32
    auto env = installer::msvc_environment();
    if (!env.has_value()) {
        std::println("SKIP: msvc environment not available");
        return;
    }
    check(std::filesystem::exists(env->cl), "msvc environment exposes cl.exe");
    check(env->variables.contains("INCLUDE"), "msvc environment includes INCLUDE");
    check(env->variables.contains("LIB"), "msvc environment includes LIB");
    check(env->variables.contains("LIBPATH"), "msvc environment includes LIBPATH");
#endif
}

int main() {
    test_toolchain_path();
    test_intron_home();
    test_which_not_installed();
    test_list_installed_empty_version();
    test_msvc_helper_paths();
    test_msvc_environment_smoke();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_installer: all tests passed");
    return 0;
}
