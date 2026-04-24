#include <cstdlib>

import std;
import cppx.terminal;
import intron.app;
import config;
import installer;
import intron.domain;
import registry;

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

auto path_separator() -> std::string_view {
#ifdef _WIN32
    return ";";
#else
    return ":";
#endif
}

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

void write_empty_file(std::filesystem::path const& path) {
    write_text_file(path, "");
}

struct TestProjectLayout {
    std::filesystem::path base;
    std::filesystem::path home;
    std::filesystem::path project;
    std::filesystem::path intron_home;
    std::filesystem::path llvm_bin;
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
    auto llvm_bin = intron_home / "toolchains" / "llvm" / "22.1.2" / "bin";
    std::filesystem::create_directories(project);
    std::filesystem::create_directories(llvm_bin);
    return {
        .base = std::move(base),
        .home = std::move(home),
        .project = std::move(project),
        .intron_home = std::move(intron_home),
        .llvm_bin = std::move(llvm_bin),
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

auto make_msvc_update_status(intron::MsvcUpdateState state,
                             std::string current_version = {},
                             std::optional<std::string> latest_version = std::nullopt,
                             std::string installation_version = {},
                             std::optional<std::string> latest_installation_version =
                                 std::nullopt) -> intron::MsvcUpdateStatus
{
    return {
        .installation_version = installation_version.empty()
            ? current_version
            : std::move(installation_version),
        .latest_installation_version = std::move(latest_installation_version),
        .current_version = std::move(current_version),
        .latest_version = std::move(latest_version),
        .state = state,
    };
}

void test_parse_without_command() {
    auto argv0 = std::array{const_cast<char*>("intron")};
    auto parsed = intron::app::parse_command_request(
        static_cast<int>(argv0.size()),
        argv0.data());

    check(!parsed.has_value(), "parse without command returns error result");
    if (!parsed.has_value()) {
        check(parsed.error().exit_code == 1, "usage result exits with code 1");
        check(!parsed.error().stdout_lines.empty(), "usage result contains usage lines");
    }
}

void test_parse_unknown_command() {
    auto argv = std::array{
        const_cast<char*>("intron"),
        const_cast<char*>("mystery"),
    };
    auto parsed = intron::app::parse_command_request(
        static_cast<int>(argv.size()),
        argv.data());

    check(!parsed.has_value(), "unknown command returns error result");
    if (!parsed.has_value()) {
        check(parsed.error().exit_code == 1, "unknown command exits with code 1");
        check(!parsed.error().stderr_lines.empty(), "unknown command reports an error");
    }
}

void test_parse_help_command() {
    auto argv = std::array{
        const_cast<char*>("intron"),
        const_cast<char*>("help"),
    };
    auto parsed = intron::app::parse_command_request(
        static_cast<int>(argv.size()),
        argv.data());

    check(parsed.has_value(), "help command parses successfully");
    if (parsed.has_value()) {
        check(parsed->command == intron::CommandKind::Help, "help command kind");
    }
}

void test_parse_exec_command() {
    auto argv = std::array{
        const_cast<char*>("intron"),
        const_cast<char*>("exec"),
        const_cast<char*>("--"),
        const_cast<char*>("cmake"),
        const_cast<char*>("--version"),
    };
    auto parsed = intron::app::parse_command_request(
        static_cast<int>(argv.size()),
        argv.data());

    check(parsed.has_value(), "exec command parses successfully");
    if (parsed.has_value()) {
        check(parsed->command == intron::CommandKind::Exec, "exec command kind");
        check(parsed->args ==
                  std::vector<std::string>{"--", "cmake", "--version"},
              "exec command preserves raw args after command");
    }
}

void test_platform_arg_split() {
    auto args = std::vector<std::string>{
        "llvm",
        "22.1.2",
        "--platform",
        "macos",
    };
    auto parsed = intron::split_platform_args(args);
    check(parsed.has_value(), "platform args parse");
    if (parsed.has_value()) {
        check(parsed->positional.size() == 2, "platform args keep positionals");
        check(parsed->platform == std::optional<std::string>{"macos"},
              "platform args capture platform");
    }

    auto dangling = intron::split_platform_args({"llvm", "--platform"});
    check(dangling.has_value(), "dangling platform flag is ignored for compatibility");
    if (dangling.has_value()) {
        check(dangling->platform == std::nullopt, "dangling platform produces no platform");
    }
}

void test_parse_exec_args() {
    auto version = intron::parse_exec_args({"--", "cmake", "--version"});
    check(version.has_value(), "exec args parse cmake invocation");
    if (version.has_value()) {
        check(*version == std::vector<std::string>{"cmake", "--version"},
              "exec args preserve simple command tokens");
    }

    auto exon = intron::parse_exec_args({"--", "exon", "test", "--platform", "windows"});
    check(exon.has_value(), "exec args parse exon invocation");
    if (exon.has_value()) {
        check(*exon == std::vector<std::string>{"exon", "test", "--platform", "windows"},
              "exec args preserve tokens after separator");
    }

    check(!intron::parse_exec_args({}).has_value(), "empty exec args fail");
    check(!intron::parse_exec_args({"--"}).has_value(), "separator-only exec args fail");
    check(!intron::parse_exec_args({"cmake"}).has_value(), "missing separator exec args fail");
}

void test_tool_lookup() {
    check(intron::tool_for_binary("clang++") == std::optional<std::string>{"llvm"},
          "clang++ maps to llvm");
    check(intron::tool_for_binary("cmake") == std::optional<std::string>{"cmake"},
          "cmake maps to cmake");
    check(!intron::tool_for_binary("unknown-tool").has_value(),
          "unknown binary is not mapped");
}

void test_build_tool_map() {
    auto current = intron::build_tool_map(
        {{"llvm", "22.1.2"}, {"cmake", "4.3.1"}},
        {{"llvm", "21.0.0"}, {"ninja", "1.13.2"}});

    check(current.at("llvm") == "22.1.2", "installed version wins over defaults");
    check(current.at("ninja") == "1.13.2", "defaults fill missing installed tools");
}

void test_update_msvc_uses_explicit_status_path() {
    auto status_calls = 0;
    auto upgrade_calls = 0;
    auto ports = intron::RuntimePorts{};
    ports.toolchain.msvc_update_status = [&] {
        ++status_calls;
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(
                intron::MsvcUpdateState::UpdateAvailable,
                "17.14.9",
                "17.14.30",
                "17.14.36310.24",
                "17.14.37203.1")};
    };
    ports.toolchain.msvc_upgrade = [&] {
        ++upgrade_calls;
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(intron::MsvcUpdateState::UpToDate, "17.14.30", "17.14.30")};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Update,
        .raw_command = "update",
        .args = {"msvc"},
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "update msvc exits successfully");
    check(status_calls == 1, "update msvc queries explicit msvc status exactly once");
    check(upgrade_calls == 0, "update msvc does not trigger upgrade");
    check(result.stdout_lines == std::vector<std::string>{
              intron::render_msvc_update_status(make_msvc_update_status(
                  intron::MsvcUpdateState::UpdateAvailable,
                  "17.14.9",
                  "17.14.30",
                  "17.14.36310.24",
                  "17.14.37203.1"))},
          "update msvc reports display versions");
}

void test_update_without_args_does_not_auto_handle_msvc() {
    auto layout = make_test_project_layout("intron-test-app-update-no-msvc");
    auto cleanup = TempDirGuard{layout.base};
    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", layout.home.string());
    set_env("USERPROFILE", layout.home.string());
    std::filesystem::current_path(layout.project);

    auto status_calls = 0;
    auto latest_calls = 0;
    auto ports = intron::RuntimePorts{};
    ports.toolchain.latest_version = [&](std::string_view tool) -> std::optional<std::string> {
        ++latest_calls;
        if (tool == "llvm") return "22.1.2";
        if (tool == "cmake") return "4.3.1";
        if (tool == "ninja") return "1.13.2";
        if (tool == "wasi-sdk") return "32";
        if (tool == "wasmtime") return "43.0.1";
        return std::nullopt;
    };
    ports.toolchain.msvc_update_status = [&] {
        ++status_calls;
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(intron::MsvcUpdateState::UpToDate, "17.14.9")};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Update,
        .raw_command = "update",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "update without args succeeds");
    check(status_calls == 0, "update without args does not query msvc status");
    check(latest_calls > 0, "update without args still checks non-system tools");
}

void test_upgrade_msvc_succeeds() {
    auto status_calls = 0;
    auto upgrade_calls = 0;
    auto ports = intron::RuntimePorts{};
    ports.toolchain.msvc_update_status = [&] {
        ++status_calls;
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(
                intron::MsvcUpdateState::UpdateAvailable,
                "17.14.9",
                "17.14.30",
                "17.14.36310.24",
                "17.14.37203.1")};
    };
    ports.toolchain.msvc_upgrade = [&] {
        ++upgrade_calls;
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(
                intron::MsvcUpdateState::UpToDate,
                "17.14.30",
                "17.14.30",
                "17.14.37203.1",
                "17.14.37203.1")};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Upgrade,
        .raw_command = "upgrade",
        .args = {"msvc"},
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "upgrade msvc exits successfully");
    check(status_calls == 1, "upgrade msvc checks current status once");
    check(upgrade_calls == 1, "upgrade msvc triggers installer upgrade once");
    check(result.stdout_lines == std::vector<std::string>{
              intron::render_msvc_upgrade_check(make_msvc_update_status(
                  intron::MsvcUpdateState::UpdateAvailable,
                  "17.14.9",
                  "17.14.30",
                  "17.14.36310.24",
                  "17.14.37203.1")),
              "",
              intron::status_line(
                  cppx::terminal::StatusKind::ok,
                  "Upgraded msvc to 17.14.30")},
          "upgrade msvc reports transition and completion");
}

void test_upgrade_msvc_handles_unknown_latest_version() {
    auto upgrade_calls = 0;
    auto ports = intron::RuntimePorts{};
    ports.toolchain.msvc_update_status = [] {
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(intron::MsvcUpdateState::Unknown, "17.14.9")};
    };
    ports.toolchain.msvc_upgrade = [&] {
        ++upgrade_calls;
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(intron::MsvcUpdateState::UpToDate, "17.14.30")};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Upgrade,
        .raw_command = "upgrade",
        .args = {"msvc"},
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 1, "upgrade msvc fails when latest version is unknown");
    check(upgrade_calls == 0, "upgrade msvc does not run installer when latest version is unknown");
    check(result.stdout_lines == std::vector<std::string>{
              intron::render_msvc_upgrade_check(
                  make_msvc_update_status(intron::MsvcUpdateState::Unknown, "17.14.9"))},
          "upgrade msvc reports unknown latest version");
}

void test_upgrade_msvc_handles_missing_installation() {
    auto ports = intron::RuntimePorts{};
    ports.toolchain.msvc_update_status = [] {
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(intron::MsvcUpdateState::Missing)};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Upgrade,
        .raw_command = "upgrade",
        .args = {"msvc"},
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 1, "upgrade msvc fails when msvc is missing");
    check(result.stderr_lines == std::vector<std::string>{
              "error: msvc is not installed",
              "hint: run 'intron install msvc 2022'"},
          "upgrade msvc reports missing installation and hint");
}

void test_upgrade_without_args_does_not_auto_handle_msvc() {
    auto layout = make_test_project_layout("intron-test-app-upgrade-no-msvc");
    auto cleanup = TempDirGuard{layout.base};
    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", layout.home.string());
    set_env("USERPROFILE", layout.home.string());
    std::filesystem::current_path(layout.project);

    auto status_calls = 0;
    auto latest_calls = 0;
    auto ports = intron::RuntimePorts{};
    ports.toolchain.latest_version = [&](std::string_view tool) -> std::optional<std::string> {
        ++latest_calls;
        if (tool == "llvm") {
            return std::nullopt;
        }
        return std::nullopt;
    };
    ports.toolchain.msvc_update_status = [&] {
        ++status_calls;
        return std::expected<intron::MsvcUpdateStatus, std::string>{
            make_msvc_update_status(intron::MsvcUpdateState::UpToDate, "17.14.9")};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Upgrade,
        .raw_command = "upgrade",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "upgrade without args succeeds without touching msvc");
    check(status_calls == 0, "upgrade without args does not query msvc status");
    check(latest_calls > 0, "upgrade without args still checks non-system tools");
    check(result.stdout_lines == std::vector<std::string>{
              intron::render_upgrade_check(intron::make_update_status(
                  "llvm",
                  "22.1.2",
                  std::nullopt))},
          "upgrade without args ignores msvc-only state");
}

void test_env_rendering() {
    auto plan = intron::build_env_plan(
        std::optional<std::string>{"/tool/bin:/other/bin"},
        std::optional<std::filesystem::path>{"/tool/bin/clang"},
        std::optional<std::filesystem::path>{"/tool/bin/clang++"},
        {},
        std::optional<std::filesystem::path>{"/tool/wasi"});

    auto lines = intron::render_env_lines(plan, false);
    check(lines.size() == 4, "env plan renders expected number of lines");
    if (lines.size() == 4) {
        check(lines[0] == "export PATH=\"/tool/bin:/other/bin:$PATH\";",
              "env rendering formats PATH export");
        check(lines[1] == "export CC=\"/tool/bin/clang\";", "env rendering formats CC export");
        check(lines[2] == "export CXX=\"/tool/bin/clang++\";", "env rendering formats CXX export");
        check(lines[3] == "export WASI_SDK_PATH=\"/tool/wasi\";",
              "env rendering formats WASI export");
    }
}

void test_env_materialization() {
    auto plan = intron::build_env_plan(
        std::optional<std::string>{"/tool/bin:/other/bin"},
        std::optional<std::filesystem::path>{"/tool/bin/clang"},
        std::optional<std::filesystem::path>{"/tool/bin/clang++"},
        {{"INCLUDE", "/tool/include"}, {"LIB", "/tool/lib"}},
        std::optional<std::filesystem::path>{"/tool/wasi"});
    auto overrides = intron::materialize_env_overrides(plan, {{"PATH", "/usr/bin"}});

    check(overrides.at("PATH") == std::format(
              "/tool/bin:/other/bin{}{}",
              path_separator(),
              "/usr/bin"),
          "env materialization appends inherited PATH");
    check(overrides.at("CC") == "/tool/bin/clang", "env materialization keeps CC");
    check(overrides.at("CXX") == "/tool/bin/clang++", "env materialization keeps CXX");
    check(overrides.at("INCLUDE") == "/tool/include", "env materialization keeps INCLUDE");
    check(overrides.at("LIB") == "/tool/lib", "env materialization keeps LIB");
    check(overrides.at("WASI_SDK_PATH") == "/tool/wasi",
          "env materialization keeps WASI_SDK_PATH");
}

void test_exec_usage_error() {
    auto ports = intron::RuntimePorts{};
    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Exec,
        .raw_command = "exec",
        .args = {"cmake"},
    };

    auto result = intron::app::run_command(request, ports);
    check(result.exit_code == 1, "invalid exec invocation exits with code 1");
    check(result.stderr_lines == std::vector<std::string>{
              "Usage: intron exec -- <command> [args...]"},
          "invalid exec invocation reports usage");
}

void test_exec_run_command_uses_resolved_env() {
    auto base = std::filesystem::temp_directory_path() / std::format(
        "intron-test-app-{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    auto cleanup = TempDirGuard{base};

    auto home = base / "home";
    auto project = base / "project";
    auto intron_home = home / ".intron";
    auto llvm_bin = intron_home / "toolchains" / "llvm" / "22.1.2" / "bin";
    auto cmake_bin = intron_home / "toolchains" / "cmake" / "9.9.9" / "bin";
    auto wasi_root = intron_home / "toolchains" / "wasi-sdk" / "32";

    write_text_file(
        intron_home / "config.toml",
        "[defaults]\n"
        "cmake = \"4.3.1\"\n"
        "llvm = \"22.1.2\"\n"
        "wasi-sdk = \"32\"\n");
    write_text_file(
        project / ".intron.toml",
        "[toolchain]\n"
        "cmake = \"9.9.9\"\n");
#ifdef _WIN32
    auto const llvm_cc = llvm_bin / "clang-cl.exe";
    auto const llvm_cxx = llvm_cc;
#else
    auto const llvm_cc = llvm_bin / "clang";
    auto const llvm_cxx = llvm_bin / "clang++";
#endif
    write_empty_file(llvm_cc);
    write_empty_file(llvm_cxx);
    write_empty_file(cmake_bin / "cmake");
    std::filesystem::create_directories(wasi_root);

    auto home_guard = EnvGuard{"HOME"};
    auto path_guard = EnvGuard{"PATH"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", home.string());
    set_env("PATH", "/usr/bin");
    std::filesystem::current_path(project);

    auto captured = std::optional<intron::ProcessRunRequest>{};
    auto ports = intron::RuntimePorts{};
    ports.filesystem.exists = [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    };
    ports.environment.get = [](std::string_view key) -> std::optional<std::string> {
        auto owned = std::string{key};
        if (auto* value = std::getenv(owned.c_str()); value) {
            return std::string{value};
        }
        return std::nullopt;
    };
    ports.environment.home_dir = [home] {
        return std::optional<std::filesystem::path>{home};
    };
    ports.process.run = [&](intron::ProcessRunRequest const& request)
        -> std::expected<int, std::string>
    {
        captured = request;
        return 23;
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Exec,
        .raw_command = "exec",
        .args = {"--", "cmake", "--version"},
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 23, "exec returns child exit code");
    check(captured.has_value(), "exec forwards request to process runner");
    if (captured.has_value()) {
        check(captured->argv == std::vector<std::string>{"cmake", "--version"},
              "exec forwards child argv without separator");
        check(captured->env_overrides.at("PATH") == std::format(
                  "{}{}{}{}{}",
                  cmake_bin.string(),
                  path_separator(),
                  llvm_bin.string(),
                  path_separator(),
                  "/usr/bin"),
              "exec forwards resolved PATH override");
        check(captured->env_overrides.at("CC") == llvm_cc.string(),
              "exec forwards resolved CC override");
        check(captured->env_overrides.at("CXX") == llvm_cxx.string(),
              "exec forwards resolved CXX override");
        check(captured->env_overrides.at("WASI_SDK_PATH") == wasi_root.string(),
              "exec forwards resolved WASI_SDK_PATH override");
    }
}

void test_windows_env_prefers_msvc_over_llvm_when_both_configured() {
#ifdef _WIN32
    auto layout = make_test_project_layout("intron-test-app-env-msvc-priority");
    auto cleanup = TempDirGuard{layout.base};
    auto expected = make_fake_msvc_environment(layout.base);
    write_text_file(
        layout.home / ".intron" / "config.toml",
        "[defaults.windows]\n"
        "msvc = \"2022\"\n");
    write_text_file(
        layout.project / ".intron.toml",
        "[toolchain]\n"
        "llvm = \"22.1.2\"\n");
    write_empty_file(layout.llvm_bin / "clang-cl.exe");

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
    ports.toolchain.msvc_environment = [expected] {
        return std::optional<intron::MsvcEnvironment>{expected};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Env,
        .raw_command = "env",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "windows env succeeds when llvm and msvc are both configured");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:CC = ") && line.contains(expected.cl.string());
          }),
          "windows env prefers cl.exe when llvm and msvc are both configured");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:CXX = ") && line.contains(expected.cl.string());
          }),
          "windows env prefers cl.exe for CXX when llvm and msvc are both configured");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:PATH = ") && line.contains(layout.llvm_bin.string()) &&
                     line.contains(expected.bin_dir.string());
          }),
          "windows env PATH keeps llvm bin and msvc bin when both are configured");
    check(!std::ranges::any_of(result.stdout_lines, [](std::string const& line) {
              return line.contains("$env:CC = ") && line.contains("clang-cl.exe");
          }),
          "windows env no longer reports clang-cl.exe as CC when msvc is configured");
#endif
}

void test_windows_exec_prefers_msvc_over_llvm_when_both_configured() {
#ifdef _WIN32
    auto layout = make_test_project_layout("intron-test-app-exec-msvc-priority");
    auto cleanup = TempDirGuard{layout.base};
    auto expected = make_fake_msvc_environment(layout.base);
    write_text_file(
        layout.home / ".intron" / "config.toml",
        "[defaults.windows]\n"
        "msvc = \"2022\"\n");
    write_text_file(
        layout.project / ".intron.toml",
        "[toolchain]\n"
        "llvm = \"22.1.2\"\n");
    write_empty_file(layout.llvm_bin / "clang-cl.exe");

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto path_guard = EnvGuard{"PATH"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", layout.home.string());
    set_env("USERPROFILE", layout.home.string());
    set_env("PATH", "C:\\BasePath");
    std::filesystem::current_path(layout.project);

    auto captured = std::optional<intron::ProcessRunRequest>{};
    auto ports = intron::RuntimePorts{};
    ports.filesystem.exists = [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    };
    ports.environment.get = [](std::string_view key) -> std::optional<std::string> {
        auto owned = std::string{key};
        if (auto* value = std::getenv(owned.c_str()); value) {
            return std::string{value};
        }
        return std::nullopt;
    };
    ports.environment.home_dir = [home = layout.home] {
        return std::optional<std::filesystem::path>{home};
    };
    ports.toolchain.msvc_environment = [expected] {
        return std::optional<intron::MsvcEnvironment>{expected};
    };
    ports.process.run = [&](intron::ProcessRunRequest const& request)
        -> std::expected<int, std::string>
    {
        captured = request;
        return 0;
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Exec,
        .raw_command = "exec",
        .args = {"--", "where.exe", "cl.exe"},
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "windows exec succeeds when llvm and msvc are both configured");
    check(captured.has_value(), "windows exec forwards child request when llvm and msvc are both configured");
    if (captured.has_value()) {
        check(captured->env_overrides.at("CC") == expected.cl.string(),
              "windows exec prefers cl.exe for CC when llvm and msvc are both configured");
        check(captured->env_overrides.at("CXX") == expected.cl.string(),
              "windows exec prefers cl.exe for CXX when llvm and msvc are both configured");
        check(captured->env_overrides.at("PATH").starts_with(std::format(
                  "{}{}{}",
                  layout.llvm_bin.string(),
                  path_separator(),
                  expected.bin_dir.string())),
              "windows exec PATH starts with llvm bin then msvc bin");
        check(captured->env_overrides.at("PATH").contains(expected.variables.at("Path")),
              "windows exec PATH keeps captured msvc PATH entries");
        check(captured->env_overrides.at("PATH").ends_with("C:\\BasePath"),
              "windows exec PATH keeps inherited PATH suffix");
    }
#endif
}

void test_windows_env_uses_clang_cl_when_only_llvm_is_configured() {
#ifdef _WIN32
    auto layout = make_test_project_layout("intron-test-app-env-llvm-only");
    auto cleanup = TempDirGuard{layout.base};
    auto clang = layout.llvm_bin / "clang-cl.exe";
    write_text_file(
        layout.project / ".intron.toml",
        "[toolchain]\n"
        "llvm = \"22.1.2\"\n");
    write_empty_file(clang);

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

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Env,
        .raw_command = "env",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "windows env succeeds when only llvm is configured");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:CC = ") && line.contains(clang.string());
          }),
          "windows env uses clang-cl.exe when msvc is not configured");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:CXX = ") && line.contains(clang.string());
          }),
          "windows env uses clang-cl.exe for CXX when msvc is not configured");
    check(!std::ranges::any_of(result.stdout_lines, [](std::string const& line) {
              return line.contains("$env:INCLUDE = ") || line.contains("$env:LIB = ") ||
                     line.contains("$env:LIBPATH = ");
          }),
          "windows env omits msvc include and lib variables when msvc is not configured");
#endif
}

void test_windows_env_errors_when_msvc_is_configured_but_unavailable() {
#ifdef _WIN32
    auto layout = make_test_project_layout("intron-test-app-env-missing-msvc");
    auto cleanup = TempDirGuard{layout.base};
    write_text_file(
        layout.home / ".intron" / "config.toml",
        "[defaults.windows]\n"
        "msvc = \"2022\"\n");

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", layout.home.string());
    set_env("USERPROFILE", layout.home.string());
    std::filesystem::current_path(layout.project);

    auto ports = intron::RuntimePorts{};
    ports.environment.home_dir = [home = layout.home] {
        return std::optional<std::filesystem::path>{home};
    };
    ports.toolchain.msvc_environment = [] {
        return std::optional<intron::MsvcEnvironment>{};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Env,
        .raw_command = "env",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 1, "windows env fails when msvc is configured but unavailable");
    check(result.stderr_lines == std::vector<std::string>{
              "error: msvc is configured as a default toolchain but was not detected",
              "hint: run 'intron install msvc 2022'"},
          "windows env reports the existing msvc missing error");
#endif
}

void test_env_run_command_uses_portable_windows_msvc_defaults() {
#ifdef _WIN32
    auto base = std::filesystem::temp_directory_path() / std::format(
        "intron-test-app-env-msvc-{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    auto cleanup = TempDirGuard{base};

    auto home = base / "home";
    auto project = base / "project";
    std::filesystem::create_directories(home / ".intron");
    std::filesystem::create_directories(project);
    write_text_file(
        project / ".intron.toml",
        "[toolchain.windows]\n"
        "msvc = \"2022\"\n");

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto include_guard = EnvGuard{"INCLUDE"};
    auto lib_guard = EnvGuard{"LIB"};
    auto libpath_guard = EnvGuard{"LIBPATH"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", home.string());
    set_env("USERPROFILE", home.string());
    clear_env("INCLUDE");
    clear_env("LIB");
    clear_env("LIBPATH");
    std::filesystem::current_path(project);

    auto expected = installer::msvc_environment();
    if (!expected.has_value()) {
        std::println("SKIP: msvc environment not available");
        return;
    }

    auto ports = intron::RuntimePorts{};
    ports.environment.home_dir = [home] {
        return std::optional<std::filesystem::path>{home};
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Env,
        .raw_command = "env",
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "env succeeds with portable windows msvc config");
    check(result.stderr_lines.empty(), "env emits no stderr for portable windows msvc config");
    check(std::ranges::any_of(result.stdout_lines, [](std::string const& line) {
              return line.contains("$env:PATH = ");
          }),
          "env renders PATH assignment for portable windows msvc config");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:CC = ") && line.contains(expected->cl.string());
          }),
          "env renders CC from detected msvc environment");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:CXX = ") && line.contains(expected->cl.string());
          }),
          "env renders CXX from detected msvc environment");
    check(std::ranges::any_of(result.stdout_lines, [&](std::string const& line) {
              return line.contains("$env:PATH = ") && line.contains(expected->bin_dir.string());
          }),
          "env PATH includes detected msvc bin directory");
    check(std::ranges::any_of(result.stdout_lines, [](std::string const& line) {
              return line.contains("$env:INCLUDE = ");
          }),
          "env renders INCLUDE from detected msvc environment");
    check(std::ranges::any_of(result.stdout_lines, [](std::string const& line) {
              return line.contains("$env:LIB = ");
          }),
          "env renders LIB from detected msvc environment");
    check(std::ranges::any_of(result.stdout_lines, [](std::string const& line) {
              return line.contains("$env:LIBPATH = ");
          }),
          "env renders LIBPATH from detected msvc environment");
#endif
}

void test_exec_run_command_uses_portable_windows_msvc_defaults() {
#ifdef _WIN32
    auto base = std::filesystem::temp_directory_path() / std::format(
        "intron-test-app-exec-msvc-{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    auto cleanup = TempDirGuard{base};

    auto home = base / "home";
    auto project = base / "project";
    std::filesystem::create_directories(home / ".intron");
    std::filesystem::create_directories(project);
    write_text_file(
        project / ".intron.toml",
        "[toolchain.windows]\n"
        "msvc = \"2022\"\n");

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    auto include_guard = EnvGuard{"INCLUDE"};
    auto lib_guard = EnvGuard{"LIB"};
    auto libpath_guard = EnvGuard{"LIBPATH"};
    auto path_guard = EnvGuard{"PATH"};
    auto cwd_guard = CurrentPathGuard{};
    set_env("HOME", home.string());
    set_env("USERPROFILE", home.string());
    set_env("PATH", "C:\\BasePath");
    clear_env("INCLUDE");
    clear_env("LIB");
    clear_env("LIBPATH");
    std::filesystem::current_path(project);

    auto expected = installer::msvc_environment();
    if (!expected.has_value()) {
        std::println("SKIP: msvc environment not available");
        return;
    }

    auto captured = std::optional<intron::ProcessRunRequest>{};
    auto ports = intron::RuntimePorts{};
    ports.environment.get = [](std::string_view key) -> std::optional<std::string> {
        auto owned = std::string{key};
        if (auto* value = std::getenv(owned.c_str()); value) {
            return std::string{value};
        }
        return std::nullopt;
    };
    ports.environment.home_dir = [home] {
        return std::optional<std::filesystem::path>{home};
    };
    ports.process.run = [&](intron::ProcessRunRequest const& request)
        -> std::expected<int, std::string>
    {
        captured = request;
        return 0;
    };

    auto request = intron::CommandRequest{
        .command = intron::CommandKind::Exec,
        .raw_command = "exec",
        .args = {"--", "where.exe", "cl.exe"},
    };
    auto result = intron::app::run_command(request, ports);

    check(result.exit_code == 0, "exec succeeds with portable windows msvc config");
    check(captured.has_value(), "exec forwards child request for portable windows msvc config");
    if (captured.has_value()) {
        check(captured->argv == std::vector<std::string>{"where.exe", "cl.exe"},
              "exec preserves child argv for portable windows msvc config");
        check(captured->env_overrides.at("CC") == expected->cl.string(),
              "exec forwards CC from detected msvc environment");
        check(captured->env_overrides.at("CXX") == expected->cl.string(),
              "exec forwards CXX from detected msvc environment");
        check(captured->env_overrides.at("INCLUDE") == expected->variables.at("INCLUDE"),
              "exec forwards INCLUDE from detected msvc environment");
        check(captured->env_overrides.at("LIB") == expected->variables.at("LIB"),
              "exec forwards LIB from detected msvc environment");
        check(captured->env_overrides.at("LIBPATH") == expected->variables.at("LIBPATH"),
              "exec forwards LIBPATH from detected msvc environment");
        check(captured->env_overrides.at("PATH").starts_with(
                  std::format("{}{}", expected->bin_dir.string(), path_separator())),
              "exec PATH starts with detected msvc bin directory");
        check(captured->env_overrides.at("PATH").contains(expected->variables.at("Path")),
              "exec PATH includes captured msvc PATH value");
        check(captured->env_overrides.at("PATH").ends_with("C:\\BasePath"),
              "exec PATH keeps inherited PATH suffix");
    }
#endif
}

void test_use_without_args_preserves_platform_specific_defaults() {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "intron_test_use_platform_defaults";
    auto const home = tmp / "home";
    auto const project = tmp / "project";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(home / ".intron");
    std::filesystem::create_directories(project);

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    set_env("HOME", home.string());
    set_env("USERPROFILE", home.string());

    auto const saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(project);

    try {
        auto const platform = std::string{registry::platform_name()};
        auto const platform_tool = platform == "windows" ? "msvc" : "llvm";
        auto const platform_version = platform == "windows" ? "2022" : "22.1.2";

        config::set_default("cmake", "4.3.1");
        config::set_default(platform_tool, platform_version, platform);

        auto request = intron::CommandRequest{
            .command = intron::CommandKind::Use,
            .raw_command = "use",
        };
        auto result = intron::app::run_command(request, {});

        check(result.exit_code == 0, "use without args succeeds");
        auto full = config::load_full_project_config();
        check(full.common.contains("cmake"), "use without args writes common defaults");
        check(full.common.at("cmake") == "4.3.1", "use without args keeps common version");
        check(!full.common.contains(platform_tool),
              "use without args keeps platform-only tool out of common section");
        check(full.platforms.contains(platform), "use without args writes current platform section");
        check(full.platforms.at(platform).contains(platform_tool),
              "use without args writes current platform tool");
        check(full.platforms.at(platform).at(platform_tool) == platform_version,
              "use without args writes current platform version");

        auto project_text = std::string{};
        {
            auto input = std::ifstream{project / ".intron.toml"};
            project_text.assign(
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{});
        }
        check(project_text.contains(std::format("[toolchain.{}]", platform)),
              "use without args renders platform section");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        std::filesystem::remove_all(tmp);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
    std::filesystem::remove_all(tmp);
}

void test_use_without_args_keeps_common_and_platform_override() {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "intron_test_use_common_and_platform_override";
    auto const home = tmp / "home";
    auto const project = tmp / "project";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(home / ".intron");
    std::filesystem::create_directories(project);

    auto home_guard = EnvGuard{"HOME"};
    auto userprofile_guard = EnvGuard{"USERPROFILE"};
    set_env("HOME", home.string());
    set_env("USERPROFILE", home.string());

    auto const saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(project);

    try {
        auto const platform = std::string{registry::platform_name()};

        config::set_default("llvm", "21.0.0");
        config::set_default("llvm", "22.1.2", platform);

        auto request = intron::CommandRequest{
            .command = intron::CommandKind::Use,
            .raw_command = "use",
        };
        auto result = intron::app::run_command(request, {});

        check(result.exit_code == 0, "use without args succeeds for common plus platform override");
        auto full = config::load_full_project_config();
        check(full.common.contains("llvm"), "use without args keeps common tool baseline");
        check(full.common.at("llvm") == "21.0.0", "use without args keeps common tool version");
        check(full.platforms.contains(platform), "use without args keeps current platform override section");
        check(full.platforms.at(platform).contains("llvm"),
              "use without args keeps current platform override tool");
        check(full.platforms.at(platform).at("llvm") == "22.1.2",
              "use without args keeps current platform override version");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        std::filesystem::remove_all(tmp);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
    std::filesystem::remove_all(tmp);
}

int main() {
    test_parse_without_command();
    test_parse_unknown_command();
    test_parse_help_command();
    test_parse_exec_command();
    test_platform_arg_split();
    test_parse_exec_args();
    test_tool_lookup();
    test_build_tool_map();
    test_update_msvc_uses_explicit_status_path();
    test_update_without_args_does_not_auto_handle_msvc();
    test_upgrade_msvc_succeeds();
    test_upgrade_msvc_handles_unknown_latest_version();
    test_upgrade_msvc_handles_missing_installation();
    test_upgrade_without_args_does_not_auto_handle_msvc();
    test_env_rendering();
    test_env_materialization();
    test_exec_usage_error();
    test_exec_run_command_uses_resolved_env();
    test_windows_env_prefers_msvc_over_llvm_when_both_configured();
    test_windows_exec_prefers_msvc_over_llvm_when_both_configured();
    test_windows_env_uses_clang_cl_when_only_llvm_is_configured();
    test_windows_env_errors_when_msvc_is_configured_but_unavailable();
    test_env_run_command_uses_portable_windows_msvc_defaults();
    test_exec_run_command_uses_portable_windows_msvc_defaults();
    test_use_without_args_preserves_platform_specific_defaults();
    test_use_without_args_keeps_common_and_platform_override();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_app: all tests passed");
    return 0;
}
