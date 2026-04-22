#include <cstdlib>

import std;
import intron.app;
import intron.domain;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
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
        if (original.has_value()) {
            set_env(key, *original);
        } else {
            clear_env(key);
        }
    }

    std::string key;
    std::optional<std::string> original;
};

struct CurrentPathGuard {
    CurrentPathGuard()
        : original(std::filesystem::current_path())
    {
    }

    ~CurrentPathGuard() {
        std::filesystem::current_path(original);
    }

    std::filesystem::path original;
};

struct TempDirGuard {
    explicit TempDirGuard(std::filesystem::path path)
        : path(std::move(path))
    {
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

void write_text_file(std::filesystem::path const& path, std::string_view text) {
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    auto out = std::ofstream{path};
    out << text;
}

struct TestProjectLayout {
    std::filesystem::path base;
    std::filesystem::path home;
    std::filesystem::path project;
    std::filesystem::path intron_home;
};

auto make_test_project_layout(std::string_view name) -> TestProjectLayout {
    auto base = std::filesystem::temp_directory_path() / std::format(
        "{}-{}",
        name,
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    auto home = base / "home";
    auto project = base / "project";
    auto intron_home = home / ".intron";
    std::filesystem::create_directories(project);
    std::filesystem::create_directories(intron_home);
    return {
        .base = std::move(base),
        .home = std::move(home),
        .project = std::move(project),
        .intron_home = std::move(intron_home),
    };
}

auto make_fake_msvc_environment(std::filesystem::path const& base) -> intron::MsvcEnvironment {
    auto bin_dir = base / "fake-msvc" / "bin";
    return {
        .bin_dir = bin_dir,
        .cl = bin_dir / "cl.exe",
        .variables = {
            {"Path", (base / "fake-msvc" / "path").string()},
            {"INCLUDE", (base / "fake-msvc" / "include").string()},
            {"LIB", (base / "fake-msvc" / "lib").string()},
            {"LIBPATH", (base / "fake-msvc" / "libpath").string()},
        },
    };
}

void test_windows_env_appends_discovered_sdk_bin_dir() {
#ifdef _WIN32
    auto layout = make_test_project_layout("intron-test-env-sdk-discover");
    auto cleanup = TempDirGuard{layout.base};
    auto expected_msvc = make_fake_msvc_environment(layout.base);
    auto fake_sdk_bin = layout.base / "fake-sdk" / "10.0.FAKE.0" / "x64";
    write_text_file(
        layout.home / ".intron" / "config.toml",
        "[defaults.windows]\n"
        "msvc = \"2022\"\n");
    write_text_file(layout.project / ".intron.toml", "[toolchain]\n");

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", layout.home.string());
    set_env("USERPROFILE", layout.home.string());
    std::filesystem::current_path(layout.project);

    auto captured_version = std::make_shared<std::optional<std::string>>();
    auto invocations = std::make_shared<int>(0);

    auto ports = intron::RuntimePorts{};
    ports.filesystem.exists = [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    };
    ports.environment.home_dir = [home = layout.home] {
        return std::optional<std::filesystem::path>{home};
    };
    ports.toolchain.msvc_environment = [expected_msvc] {
        return std::optional<intron::MsvcEnvironment>{expected_msvc};
    };
    ports.toolchain.windows_sdk_bin_dirs =
        [captured_version, invocations, fake_sdk_bin]
        (std::optional<std::string> version) -> std::vector<std::filesystem::path>
        {
            *captured_version = std::move(version);
            ++*invocations;
            return {fake_sdk_bin};
        };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Env,
        .raw_command = "env",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "env with discovered SDK succeeds");
    check(*invocations == 1, "windows_sdk_bin_dirs port is invoked once");
    check(!captured_version->has_value(),
          "windows_sdk_bin_dirs receives nullopt when no sdk pin is set");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:PATH = ") && line.contains(fake_sdk_bin.string());
          }),
          "discovered SDK bin dir appears in emitted PATH");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:PATH = ") &&
                     line.contains(expected_msvc.bin_dir.string());
          }),
          "MSVC bin dir still appears in emitted PATH");
#endif
}

void test_windows_env_passes_sdk_pin_from_config() {
#ifdef _WIN32
    auto layout = make_test_project_layout("intron-test-env-sdk-pin");
    auto cleanup = TempDirGuard{layout.base};
    auto expected_msvc = make_fake_msvc_environment(layout.base);
    auto fake_sdk_bin = layout.base / "fake-sdk" / "10.0.26100.0" / "x64";
    write_text_file(
        layout.home / ".intron" / "config.toml",
        "[defaults.windows]\n"
        "msvc = \"2022\"\n");
    write_text_file(
        layout.project / ".intron.toml",
        "[toolchain.windows]\n"
        "sdk = \"10.0.26100.0\"\n");

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", layout.home.string());
    set_env("USERPROFILE", layout.home.string());
    std::filesystem::current_path(layout.project);

    auto captured_version = std::make_shared<std::optional<std::string>>();

    auto ports = intron::RuntimePorts{};
    ports.filesystem.exists = [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    };
    ports.environment.home_dir = [home = layout.home] {
        return std::optional<std::filesystem::path>{home};
    };
    ports.toolchain.msvc_environment = [expected_msvc] {
        return std::optional<intron::MsvcEnvironment>{expected_msvc};
    };
    ports.toolchain.windows_sdk_bin_dirs =
        [captured_version, fake_sdk_bin]
        (std::optional<std::string> version) -> std::vector<std::filesystem::path>
        {
            *captured_version = std::move(version);
            return {fake_sdk_bin};
        };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Env,
        .raw_command = "env",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "env with pinned SDK succeeds");
    check(captured_version->has_value() &&
              **captured_version == "10.0.26100.0",
          "windows_sdk_bin_dirs receives pinned sdk version from .intron.toml");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:PATH = ") && line.contains(fake_sdk_bin.string());
          }),
          "pinned SDK bin dir appears in emitted PATH");
#endif
}

void test_windows_env_tolerates_empty_sdk_result() {
#ifdef _WIN32
    auto layout = make_test_project_layout("intron-test-env-sdk-empty");
    auto cleanup = TempDirGuard{layout.base};
    auto expected_msvc = make_fake_msvc_environment(layout.base);
    write_text_file(
        layout.home / ".intron" / "config.toml",
        "[defaults.windows]\n"
        "msvc = \"2022\"\n");
    write_text_file(layout.project / ".intron.toml", "[toolchain]\n");

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", layout.home.string());
    set_env("USERPROFILE", layout.home.string());
    std::filesystem::current_path(layout.project);

    auto ports = intron::RuntimePorts{};
    ports.filesystem.exists = [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    };
    ports.environment.home_dir = [home = layout.home] {
        return std::optional<std::filesystem::path>{home};
    };
    ports.toolchain.msvc_environment = [expected_msvc] {
        return std::optional<intron::MsvcEnvironment>{expected_msvc};
    };
    ports.toolchain.windows_sdk_bin_dirs =
        [](std::optional<std::string>) -> std::vector<std::filesystem::path> {
            return {};
        };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Env,
        .raw_command = "env",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "env succeeds when SDK discovery returns empty");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:PATH = ") &&
                     line.contains(expected_msvc.bin_dir.string());
          }),
          "MSVC bin dir still appears when SDK discovery yields no results");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:PATH = ") &&
                     line.contains(expected_msvc.variables.at("Path"));
          }),
          "vcvars Path suffix is preserved when SDK discovery yields no results");
    check(!std::ranges::any_of(result.stdout_lines, [](std::string const& line) {
              return line.contains(";;");
          }),
          "no empty PATH segment is introduced when SDK discovery yields no results");
#endif
}

int main() {
    test_windows_env_appends_discovered_sdk_bin_dir();
    test_windows_env_passes_sdk_pin_from_config();
    test_windows_env_tolerates_empty_sdk_result();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_env_windows_sdk: all tests passed");
    return 0;
}
