module;
#if defined(_WIN32)
#define NOMINMAX
#include <process.h>
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>

extern char** environ;
#endif

export module intron.edge;
import std;
import cppx.env;
import cppx.env.system;
import intron.domain;

namespace {

using EnvMap = std::map<std::string, std::string>;

auto env_key_equals(std::string_view lhs, std::string_view rhs) -> bool {
#ifdef _WIN32
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
#else
    return lhs == rhs;
#endif
}

auto find_env_entry(EnvMap& env, std::string_view key) -> EnvMap::iterator {
    return std::ranges::find_if(env, [&](auto const& entry) {
        return env_key_equals(entry.first, key);
    });
}

auto find_env_entry(EnvMap const& env, std::string_view key) -> EnvMap::const_iterator {
    return std::ranges::find_if(env, [&](auto const& entry) {
        return env_key_equals(entry.first, key);
    });
}

auto current_environment() -> EnvMap {
    auto env = EnvMap{};
#ifdef _WIN32
    auto* block = GetEnvironmentStringsW();
    if (!block) {
        return env;
    }
    auto* cursor = block;
    while (*cursor != L'\0') {
        auto entry_view = std::wstring_view{cursor};
        auto split = entry_view.find(L'=');
        if (split == 0) {
            split = entry_view.find(L'=', 1);
        }
        if (split != std::wstring_view::npos) {
            auto narrow = [](std::wstring_view value) -> std::string {
                if (value.empty()) {
                    return {};
                }
                auto size = WideCharToMultiByte(
                    CP_ACP,
                    0,
                    value.data(),
                    static_cast<int>(value.size()),
                    nullptr,
                    0,
                    nullptr,
                    nullptr);
                if (size <= 0) {
                    throw std::runtime_error("failed to convert wide environment string");
                }
                auto out = std::string(static_cast<std::size_t>(size), '\0');
                WideCharToMultiByte(
                    CP_ACP,
                    0,
                    value.data(),
                    static_cast<int>(value.size()),
                    out.data(),
                    size,
                    nullptr,
                    nullptr);
                return out;
            };
            env.emplace(
                narrow(entry_view.substr(0, split)),
                narrow(entry_view.substr(split + 1)));
        }
        cursor += entry_view.size() + 1;
    }
    FreeEnvironmentStringsW(block);
#else
    for (auto** entry = environ; entry && *entry; ++entry) {
        auto text = std::string_view{*entry};
        auto split = text.find('=');
        if (split == std::string_view::npos) {
            continue;
        }
        env.emplace(
            std::string{text.substr(0, split)},
            std::string{text.substr(split + 1)});
    }
#endif
    return env;
}

auto apply_overrides(EnvMap env, std::map<std::string, std::string> const& overrides) -> EnvMap {
    for (auto const& [key, value] : overrides) {
        if (auto it = find_env_entry(env, key); it != env.end()) {
            it->second = value;
        } else {
            env.emplace(key, value);
        }
    }
    return env;
}

struct MergedEnvSource {
    EnvMap const& env;

    auto get(std::string_view key) const -> std::optional<std::string> {
        if (auto it = find_env_entry(env, key); it != env.end() && !it->second.empty()) {
            return it->second;
        }
        return std::nullopt;
    }
};

struct FilesystemSource {
    auto is_regular_file(std::filesystem::path const& path) const -> bool {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    }
};

auto resolve_executable(
    std::string_view argv0,
    EnvMap const& env) -> std::expected<std::filesystem::path, std::string>
{
    if (argv0.empty()) {
        return std::unexpected("missing executable name");
    }

    auto candidate = std::filesystem::path{argv0};
    if (candidate.has_parent_path() || candidate.is_absolute()) {
        return candidate;
    }

    auto found = cppx::env::find_in_path(MergedEnvSource{env}, FilesystemSource{}, argv0);
    if (!found) {
        return std::unexpected(std::format("executable not found: {}", argv0));
    }
    return *found;
}

#ifdef _WIN32
auto to_wide(std::string_view value) -> std::wstring {
    if (value.empty()) {
        return {};
    }
    auto size = MultiByteToWideChar(
        CP_ACP,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (size <= 0) {
        throw std::runtime_error("failed to convert string to UTF-16");
    }
    auto out = std::wstring(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_ACP,
        0,
        value.data(),
        static_cast<int>(value.size()),
        out.data(),
        size);
    return out;
}

auto quote_windows_arg(std::wstring_view value) -> std::wstring {
    if (value.empty()) {
        return L"\"\"";
    }

    auto needs_quotes = false;
    for (auto ch : value) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return std::wstring{value};
    }

    auto out = std::wstring{};
    out.push_back(L'"');

    auto backslashes = std::size_t{0};
    for (auto ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }

    if (backslashes > 0) {
        out.append(backslashes * 2, L'\\');
    }
    out.push_back(L'"');
    return out;
}

auto build_windows_command_line(std::filesystem::path const& executable,
                                std::vector<std::string> const& argv) -> std::wstring
{
    auto command_line = quote_windows_arg(executable.wstring());
    for (std::size_t i = 1; i < argv.size(); ++i) {
        command_line.push_back(L' ');
        command_line += quote_windows_arg(to_wide(argv[i]));
    }
    return command_line;
}

auto build_windows_environment_block(EnvMap const& env) -> std::vector<wchar_t> {
    auto block = std::vector<wchar_t>{};
    for (auto const& [key, value] : env) {
        auto entry = to_wide(std::format("{}={}", key, value));
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
#endif

#ifndef _WIN32
auto normalize_unix_exit_status(int status) -> int {
    if (status == -1) {
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}
#endif

auto run_process(intron::ProcessRunRequest const& request) -> std::expected<int, std::string> {
    if (request.argv.empty()) {
        return std::unexpected("missing executable name");
    }
    auto env = apply_overrides(current_environment(), request.env_overrides);
    auto executable = resolve_executable(request.argv.front(), env);
    if (!executable) {
        return std::unexpected(executable.error());
    }

#ifdef _WIN32
    auto command_line = build_windows_command_line(*executable, request.argv);
    auto mutable_command_line = std::vector<wchar_t>(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');
    auto environment = build_windows_environment_block(env);

    auto startup_info = STARTUPINFOW{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags |= STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup_info.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    auto process_info = PROCESS_INFORMATION{};
    auto created = CreateProcessW(
        executable->c_str(),
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT,
        environment.data(),
        nullptr,
        &startup_info,
        &process_info);
    if (!created) {
        auto error = GetLastError();
        return std::unexpected(std::format(
            "failed to spawn '{}': {}",
            executable->string(),
            std::system_category().message(static_cast<int>(error))));
    }

    auto wait_result = WaitForSingleObject(process_info.hProcess, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        auto error = GetLastError();
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return std::unexpected(std::format(
            "failed to wait for '{}': {}",
            executable->string(),
            std::system_category().message(static_cast<int>(error))));
    }

    DWORD exit_code = 1;
    auto got_exit_code = GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    if (!got_exit_code) {
        auto error = GetLastError();
        return std::unexpected(std::format(
            "failed to read exit status for '{}': {}",
            executable->string(),
            std::system_category().message(static_cast<int>(error))));
    }
    return static_cast<int>(exit_code);
#else
    auto argv_storage = request.argv;
    auto argv = std::vector<char*>{};
    argv.reserve(argv_storage.size() + 1);
    for (auto& arg : argv_storage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    auto env_storage = std::vector<std::string>{};
    env_storage.reserve(env.size());
    for (auto const& [key, value] : env) {
        env_storage.push_back(std::format("{}={}", key, value));
    }
    auto envp = std::vector<char*>{};
    envp.reserve(env_storage.size() + 1);
    for (auto& entry : env_storage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    pid_t pid = 0;
    auto rc = posix_spawn(
        &pid,
        executable->c_str(),
        nullptr,
        nullptr,
        argv.data(),
        envp.data());
    if (rc != 0) {
        return std::unexpected(std::format(
            "failed to spawn '{}': {}",
            executable->string(),
            std::generic_category().message(rc)));
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        return std::unexpected(std::format(
            "failed to wait for '{}': {}",
            executable->string(),
            std::generic_category().message(errno)));
    }
    return normalize_unix_exit_status(status);
#endif
}

} // namespace

export namespace intron::edge {

auto make_runtime_ports() -> intron::RuntimePorts {
    auto ports = intron::RuntimePorts{};
    ports.filesystem.exists = [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    };
    ports.process.run = [](intron::ProcessRunRequest const& request)
        -> std::expected<int, std::string>
    {
        return run_process(request);
    };
    ports.environment.get = [](std::string_view key) -> std::optional<std::string> {
        auto owned = std::string{key};
        if (auto const* value = std::getenv(owned.c_str()); value) {
            return std::string{value};
        }
        return std::nullopt;
    };
    ports.environment.home_dir = [] {
        return cppx::env::system::home_dir();
    };
    ports.clock.sleep_for = [](std::chrono::milliseconds duration) {
        std::this_thread::sleep_for(duration);
    };
    ports.console.write_stdout = [](std::string_view line) {
        std::println("{}", line);
    };
    ports.console.write_stderr = [](std::string_view line) {
        std::println(std::cerr, "{}", line);
    };
    return ports;
}

auto render(intron::CommandResult const& result,
            intron::RuntimePorts const& ports) -> void
{
    if (!result.diagnostics.empty()) {
        for (auto const& diagnostic : result.diagnostics) {
            if (diagnostic.channel == intron::OutputChannel::Stdout) {
                if (ports.console.write_stdout) {
                    ports.console.write_stdout(diagnostic.message);
                }
            } else if (ports.console.write_stderr) {
                ports.console.write_stderr(diagnostic.message);
            }
        }
        return;
    }

    for (auto const& line : result.stdout_lines) {
        if (ports.console.write_stdout) {
            ports.console.write_stdout(line);
        }
    }
    for (auto const& line : result.stderr_lines) {
        if (ports.console.write_stderr) {
            ports.console.write_stderr(line);
        }
    }
}

auto render(intron::CommandResult const& result) -> void {
    render(result, make_runtime_ports());
}

} // namespace intron::edge
