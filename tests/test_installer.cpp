#include <cstdlib>

import std;
import cppx.http;
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

void set_env(std::string_view name, std::string_view value) {
    auto env_name = std::string{name};
    auto env_value = std::string{value};
#ifdef _WIN32
    ::_putenv_s(env_name.c_str(), env_value.c_str());
#else
    ::setenv(env_name.c_str(), env_value.c_str(), 1);
#endif
}

void clear_env(std::string_view name) {
    auto env_name = std::string{name};
#ifdef _WIN32
    ::_putenv_s(env_name.c_str(), "");
#else
    ::unsetenv(env_name.c_str());
#endif
}

struct EnvGuard {
    explicit EnvGuard(std::string_view key)
        : key(key)
    {
        if (auto* value = std::getenv(this->key.c_str()); value) {
            original = value;
        }
    }

    ~EnvGuard() {
        if (original.has_value())
            set_env(key, *original);
        else
            clear_env(key);
    }

    std::string key;
    std::optional<std::string> original;
};

void test_selected_backend_from_env() {
    auto guard = EnvGuard{"INTRON_NET_BACKEND"};

    clear_env("INTRON_NET_BACKEND");
    check(net::selected_backend_from_env() == net::Backend::Auto,
          "net backend defaults to auto");

    set_env("INTRON_NET_BACKEND", "cppx");
    check(net::selected_backend_from_env() == net::Backend::Cppx,
          "cppx backend can be forced");

    set_env("INTRON_NET_BACKEND", "SHELL");
    check(net::selected_backend_from_env() == net::Backend::Shell,
          "shell backend parsing is case insensitive");

    set_env("INTRON_NET_BACKEND", "bogus");
    check(net::selected_backend_from_env() == net::Backend::Auto,
          "invalid backend falls back to auto");
}

void test_should_fallback() {
    check(net::should_fallback(cppx::http::http_error::response_parse_failed),
          "response parse failures are retryable");
    check(net::should_fallback(cppx::http::http_error::connection_failed),
          "connection failures are retryable");
    check(net::should_fallback(cppx::http::http_error::tls_failed),
          "tls failures are retryable");
    check(net::should_fallback(cppx::http::http_error::timeout),
          "timeouts are retryable");
    check(!net::should_fallback(cppx::http::http_error::send_failed),
          "send failures do not trigger shell fallback");
    check(!net::should_fallback(cppx::http::http_error::redirect_limit),
          "redirect limit errors do not trigger shell fallback");
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
    auto root = installer::msvc_path();
    if (!root.has_value()) {
        std::println("SKIP: msvc installation not available");
        return;
    }
    auto env = installer::msvc_environment();
    check(env.has_value(), "msvc environment is available when msvc is installed");
    if (!env.has_value()) return;
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
    test_latest_version_from_release_json();
    test_github_api_headers();
    test_selected_backend_from_env();
    test_should_fallback();
    test_msvc_helper_paths();
    test_msvc_environment_smoke();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_installer: all tests passed");
    return 0;
}
