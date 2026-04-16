import std;
import installer;
import net;

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

void test_latest_version_from_release_json() {
    auto version = net::latest_version_from_release_json(
        R"({"tag_name":"v0.18.3"})");
    check(version.has_value(), "release json parsed");
    check(*version == "0.18.3", "leading v removed");

    auto dashed = net::latest_version_from_release_json(
        R"({"tag_name":"llvmorg-20.1.0"})");
    check(dashed.has_value(), "dash tag parsed");
    check(*dashed == "20.1.0", "suffix extracted from dashed tag");

    auto missing = net::latest_version_from_release_json(
        R"({"name":"missing"})");
    check(!missing.has_value(), "missing tag_name returns nullopt");
}

void test_github_api_headers() {
    auto hdrs = net::github_api_headers("intron/test");
    check(hdrs.get("user-agent") == "intron/test", "github api user-agent");
    check(hdrs.get("accept") == "application/vnd.github+json",
          "github api accept header");
}

int main() {
    test_toolchain_path();
    test_intron_home();
    test_which_not_installed();
    test_list_installed_empty_version();
    test_latest_version_from_release_json();
    test_github_api_headers();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_installer: all tests passed");
    return 0;
}
