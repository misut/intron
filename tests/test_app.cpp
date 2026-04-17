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

void write_empty_file(std::filesystem::path const& path) {
    write_text_file(path, "");
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

    check(overrides.at("PATH") == "/tool/bin:/other/bin:/usr/bin",
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
    write_empty_file(llvm_bin / "clang");
    write_empty_file(llvm_bin / "clang++");
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
                  "{}:{}:/usr/bin",
                  cmake_bin.string(),
                  llvm_bin.string()),
              "exec forwards resolved PATH override");
        check(captured->env_overrides.at("CC") == (llvm_bin / "clang").string(),
              "exec forwards resolved CC override");
        check(captured->env_overrides.at("CXX") == (llvm_bin / "clang++").string(),
              "exec forwards resolved CXX override");
        check(captured->env_overrides.at("WASI_SDK_PATH") == wasi_root.string(),
              "exec forwards resolved WASI_SDK_PATH override");
    }
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
    test_env_rendering();
    test_env_materialization();
    test_exec_usage_error();
    test_exec_run_command_uses_resolved_env();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_app: all tests passed");
    return 0;
}
