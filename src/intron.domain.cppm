export module intron.domain;
import std;
import registry;

export namespace intron {

enum class OutputChannel {
    Stdout,
    Stderr,
};

struct Diagnostic {
    OutputChannel channel = OutputChannel::Stdout;
    std::string message;
};

struct CommandResult {
    int exit_code = 0;
    std::vector<std::string> stdout_lines;
    std::vector<std::string> stderr_lines;
    std::vector<Diagnostic> diagnostics;

    auto add_stdout(std::string_view line) -> void {
        stdout_lines.emplace_back(line);
        diagnostics.push_back({OutputChannel::Stdout, std::string{line}});
    }

    auto add_stderr(std::string_view line) -> void {
        stderr_lines.emplace_back(line);
        diagnostics.push_back({OutputChannel::Stderr, std::string{line}});
    }
};

enum class CommandKind {
    Install,
    Remove,
    List,
    Which,
    Default,
    Use,
    Update,
    Upgrade,
    Env,
    Exec,
    SelfUpdate,
    Help,
};

struct CommandRequest {
    CommandKind command;
    std::string raw_command;
    std::vector<std::string> args;
    std::string self_path;
};

struct ConfigDocument {
    std::map<std::string, std::string> common;
    std::map<std::string, std::map<std::string, std::string>> platforms;
};

struct EnvAssignment {
    std::string key;
    std::string value;
    bool append_existing = false;
};

struct EnvPlan {
    std::vector<EnvAssignment> assignments;
};

enum class UpdateState {
    UpToDate,
    UpdateAvailable,
    Unknown,
};

struct UpdateStatus {
    std::string tool;
    std::string current_version;
    std::optional<std::string> latest_version;
    UpdateState state = UpdateState::Unknown;
};

enum class MsvcUpdateState {
    Missing,
    UpToDate,
    UpdateAvailable,
    Unknown,
};

struct MsvcUpdateStatus {
    std::string installation_version;
    std::optional<std::string> latest_installation_version;
    std::string current_version;
    std::optional<std::string> latest_version;
    MsvcUpdateState state = MsvcUpdateState::Missing;
};

struct DownloadPlan {
    std::string url;
    std::filesystem::path archive_path;
    bool use_cached_archive = false;
    bool verify_checksum = false;
    std::string checksum_url;
};

enum class PostInstallActionKind {
    SetupLlvmConfig,
};

struct PostInstallAction {
    PostInstallActionKind kind = PostInstallActionKind::SetupLlvmConfig;
    std::filesystem::path dest;
    std::string version;
};

struct InstallPlan {
    std::filesystem::path home;
    std::filesystem::path downloads_dir;
    std::filesystem::path staging_dir;
    std::filesystem::path dest;
    std::string archive_name;
    std::optional<DownloadPlan> download;
    std::vector<PostInstallAction> post_install_actions;
};

struct FilesystemPort {
    std::function<bool(std::filesystem::path const&)> exists;
};

struct HttpClientPort {
};

struct ProcessRunRequest {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env_overrides;
};

struct ProcessRunnerPort {
    std::function<std::expected<int, std::string>(ProcessRunRequest const&)> run;
};

struct EnvironmentPort {
    std::function<std::optional<std::string>(std::string_view)> get;
    std::function<std::optional<std::filesystem::path>()> home_dir;
};

struct MsvcEnvironment {
    std::filesystem::path bin_dir;
    std::filesystem::path cl;
    std::map<std::string, std::string> variables;
};

struct ToolchainPort {
    std::function<std::optional<MsvcEnvironment>()> msvc_environment;
    std::function<std::optional<std::string>(std::string_view)> latest_version;
    std::function<std::expected<MsvcUpdateStatus, std::string>()> msvc_update_status;
    std::function<std::expected<MsvcUpdateStatus, std::string>()> msvc_upgrade;
    std::function<std::vector<std::filesystem::path>(std::optional<std::string>)>
        windows_sdk_bin_dirs;
};

struct ClockPort {
    std::function<void(std::chrono::milliseconds)> sleep_for;
};

struct ConsolePort {
    std::function<void(std::string_view)> write_stdout;
    std::function<void(std::string_view)> write_stderr;
};

struct RuntimePorts {
    FilesystemPort filesystem;
    HttpClientPort http;
    ProcessRunnerPort process;
    EnvironmentPort environment;
    ToolchainPort toolchain;
    ClockPort clock;
    ConsolePort console;
};

struct ParsedArguments {
    std::vector<std::string> positional;
    std::optional<std::string> platform;
};

constexpr std::array<std::string_view, 3> valid_platforms = {
    "linux",
    "macos",
    "windows",
};

inline auto is_valid_platform(std::string_view name) -> bool {
    return std::ranges::find(valid_platforms, name) != valid_platforms.end();
}

inline auto tool_for_binary(std::string_view binary) -> std::optional<std::string> {
    if (binary.starts_with("clang") || binary.starts_with("llvm-") ||
        binary.starts_with("lldb") || binary.starts_with("lld") ||
        binary == "ld.lld" || binary == "ld64.lld" ||
        binary == "wasm-ld" || binary == "dsymutil") {
        return "llvm";
    }
    if (binary == "cl" || binary == "cl.exe" || binary == "link" || binary == "link.exe") {
        return "msvc";
    }
    if (binary == "cmake" || binary == "ctest" || binary == "cpack" || binary == "ccmake") {
        return "cmake";
    }
    if (binary == "ninja") {
        return "ninja";
    }
    return std::nullopt;
}

inline auto split_platform_args(std::vector<std::string> const& args)
    -> std::expected<ParsedArguments, std::string>
{
    ParsedArguments result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--platform") {
            if (i + 1 >= args.size()) {
                continue;
            }
            auto const& platform = args[i + 1];
            if (!is_valid_platform(platform)) {
                return std::unexpected(std::format(
                    "invalid platform '{}' (expected: linux, macos, windows)",
                    platform));
            }
            if (!result.platform) {
                result.platform = platform;
            }
            ++i;
            continue;
        }
        result.positional.push_back(args[i]);
    }
    return result;
}

inline auto parse_exec_args(std::vector<std::string> const& args)
    -> std::expected<std::vector<std::string>, std::string>
{
    if (args.empty()) {
        return std::unexpected("missing '--' separator before command");
    }
    if (args.front() != "--") {
        return std::unexpected("expected '--' separator before command");
    }
    if (args.size() == 1) {
        return std::unexpected("missing command after '--'");
    }
    return std::vector<std::string>{args.begin() + 1, args.end()};
}

inline auto usage_lines(std::string_view version) -> std::vector<std::string> {
    return {
        std::format("intron {}", version),
        "",
        "Usage: intron <command> [args...]",
        "",
        "Commands:",
        "  install [tool] [version]               Install toolchain(s) (reads .intron.toml if no args)",
        "  remove  <tool> <version>               Remove a toolchain",
        "  list                                   List installed toolchains",
        "  which   <binary>                       Print path to binary",
        "  default <tool> <version> [--platform <name>]  Set global default version",
        "  use     [tool] [version] [--platform <name>]  Set project toolchain in .intron.toml",
        "  update [tool]                          Check for updates",
        "  upgrade [tool]                         Upgrade tools to latest",
        "  env                                    Print environment variables",
        "  exec    -- <command> [args...]         Run a command with intron environment",
        "  self-update                            Update intron itself",
        "  help                                   Show this message",
        "",
        "Tools: android-ndk, cmake, llvm, msvc, ninja, wasi-sdk, wasmtime",
        "",
        "Options:",
        "  --platform <name>  Target a specific platform section (linux, macos, windows)",
    };
}

inline auto merge_config_document(ConfigDocument const& config, std::string_view platform)
    -> std::map<std::string, std::string>
{
    auto merged = config.common;
    auto it = config.platforms.find(std::string{platform});
    if (it == config.platforms.end()) {
        return merged;
    }
    for (auto const& [tool, version] : it->second) {
        merged[tool] = version;
    }
    return merged;
}

inline auto build_tool_map(
    std::vector<std::pair<std::string, std::string>> const& installed,
    std::map<std::string, std::string> const& defaults) -> std::map<std::string, std::string>
{
    std::map<std::string, std::string> current;
    for (auto const& [tool, version] : installed) {
        if (!current.contains(tool)) {
            current[tool] = version;
        }
    }
    for (auto const& [tool, version] : defaults) {
        if (!current.contains(tool)) {
            current[tool] = version;
        }
    }
    return current;
}

inline auto make_update_status(
    std::string tool,
    std::string current_version,
    std::optional<std::string> latest_version) -> UpdateStatus
{
    auto status = UpdateStatus{
        .tool = std::move(tool),
        .current_version = std::move(current_version),
        .latest_version = std::move(latest_version),
    };
    if (!status.latest_version) {
        status.state = UpdateState::Unknown;
    } else if (*status.latest_version == status.current_version) {
        status.state = UpdateState::UpToDate;
    } else {
        status.state = UpdateState::UpdateAvailable;
    }
    return status;
}

inline auto render_update_status(UpdateStatus const& status) -> std::string {
    switch (status.state) {
    case UpdateState::Unknown:
        return std::format(
            "{} {}: could not check latest",
            status.tool,
            status.current_version);
    case UpdateState::UpToDate:
        return std::format(
            "{} {} (up to date)",
            status.tool,
            status.current_version);
    case UpdateState::UpdateAvailable:
        return std::format(
            "{} {} -> {} (update available)",
            status.tool,
            status.current_version,
            *status.latest_version);
    }
    return {};
}

inline auto render_upgrade_check(UpdateStatus const& status) -> std::string {
    switch (status.state) {
    case UpdateState::Unknown:
        return std::format("{}: could not check latest version", status.tool);
    case UpdateState::UpToDate:
        return std::format("{} {} (up to date)", status.tool, status.current_version);
    case UpdateState::UpdateAvailable:
        return std::format(
            "{} {} -> {}...",
            status.tool,
            status.current_version,
            *status.latest_version);
    }
    return {};
}

inline auto render_msvc_update_status(MsvcUpdateStatus const& status) -> std::string {
    switch (status.state) {
    case MsvcUpdateState::Missing:
        return "msvc: not installed";
    case MsvcUpdateState::Unknown:
        if (status.current_version.empty()) {
            return "msvc: could not check latest";
        }
        return std::format("msvc {}: could not check latest", status.current_version);
    case MsvcUpdateState::UpToDate:
        return std::format("msvc {} (up to date)", status.current_version);
    case MsvcUpdateState::UpdateAvailable:
        return std::format(
            "msvc {} -> {} (update available)",
            status.current_version,
            *status.latest_version);
    }
    return {};
}

inline auto render_msvc_upgrade_check(MsvcUpdateStatus const& status) -> std::string {
    switch (status.state) {
    case MsvcUpdateState::Missing:
        return "msvc: not installed";
    case MsvcUpdateState::Unknown:
        return "msvc: could not check latest version";
    case MsvcUpdateState::UpToDate:
        return std::format("msvc {} (up to date)", status.current_version);
    case MsvcUpdateState::UpdateAvailable:
        return std::format(
            "msvc {} -> {}...",
            status.current_version,
            *status.latest_version);
    }
    return {};
}

inline auto build_env_plan(
    std::optional<std::string> path_value,
    std::optional<std::filesystem::path> cc,
    std::optional<std::filesystem::path> cxx,
    std::map<std::string, std::string> const& extra_vars,
    std::optional<std::filesystem::path> wasi_sdk_path) -> EnvPlan
{
    EnvPlan plan;
    if (path_value && !path_value->empty()) {
        plan.assignments.push_back({
            .key = "PATH",
            .value = *path_value,
            .append_existing = true,
        });
    }
    if (cc) {
        plan.assignments.push_back({
            .key = "CC",
            .value = cc->string(),
        });
    }
    if (cxx) {
        plan.assignments.push_back({
            .key = "CXX",
            .value = cxx->string(),
        });
    }
    for (auto const& [key, value] : extra_vars) {
        if (!value.empty()) {
            plan.assignments.push_back({
                .key = key,
                .value = value,
            });
        }
    }
    if (wasi_sdk_path) {
        plan.assignments.push_back({
            .key = "WASI_SDK_PATH",
            .value = wasi_sdk_path->string(),
        });
    }
    return plan;
}

inline auto render_env_lines(EnvPlan const& plan, bool is_windows)
    -> std::vector<std::string>
{
    auto lines = std::vector<std::string>{};
    for (auto const& assignment : plan.assignments) {
        if (is_windows) {
            if (assignment.append_existing) {
                lines.push_back(std::format(
                    "$env:{} = \"{};${{env:{}}}\";",
                    assignment.key,
                    assignment.value,
                    assignment.key));
            } else {
                lines.push_back(std::format(
                    "$env:{} = \"{}\";",
                    assignment.key,
                    assignment.value));
            }
        } else {
            if (assignment.append_existing) {
                lines.push_back(std::format(
                    "export {}=\"{}:{}\";",
                    assignment.key,
                    assignment.value,
                    std::format("${}", assignment.key)));
            } else {
                lines.push_back(std::format(
                    "export {}=\"{}\";",
                    assignment.key,
                    assignment.value));
            }
        }
    }
    return lines;
}

inline auto find_env_value(
    std::map<std::string, std::string> const& env,
    std::string_view key) -> std::optional<std::string>
{
#ifdef _WIN32
    auto equals_key = [key](std::string const& candidate) {
        if (candidate.size() != key.size()) {
            return false;
        }
        for (std::size_t i = 0; i < candidate.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(candidate[i])) !=
                std::tolower(static_cast<unsigned char>(key[i]))) {
                return false;
            }
        }
        return true;
    };
    auto it = std::ranges::find_if(env, [&](auto const& entry) {
        return equals_key(entry.first);
    });
    if (it != env.end()) {
        return it->second;
    }
    return std::nullopt;
#else
    auto it = env.find(std::string{key});
    if (it != env.end()) {
        return it->second;
    }
    return std::nullopt;
#endif
}

inline auto materialize_env_overrides(
    EnvPlan const& plan,
    std::map<std::string, std::string> const& inherited_env) -> std::map<std::string, std::string>
{
    auto overrides = std::map<std::string, std::string>{};
    for (auto const& assignment : plan.assignments) {
        auto value = assignment.value;
        if (assignment.append_existing) {
            if (auto inherited = find_env_value(inherited_env, assignment.key);
                inherited && !inherited->empty()) {
#ifdef _WIN32
                value += std::format(";{}", *inherited);
#else
                value += std::format(":{}", *inherited);
#endif
            }
        }
        overrides[assignment.key] = std::move(value);
    }
    return overrides;
}

inline auto make_install_plan(
    std::filesystem::path const& intron_home,
    registry::ToolInfo const& info,
    bool use_cached_archive = false) -> InstallPlan
{
    auto plan = InstallPlan{
        .home = intron_home,
        .downloads_dir = intron_home / "downloads",
        .staging_dir = intron_home / "staging" / std::format("{}-{}", info.name, info.version),
        .dest = intron_home / "toolchains" / info.name / info.version,
    };

    if (!registry::is_system_tool(info.name) && !info.url.empty()) {
        auto url_sv = std::string_view{info.url};
        auto slash = url_sv.rfind('/');
        plan.archive_name = std::string{url_sv.substr(slash + 1)};
        plan.download = DownloadPlan{
            .url = info.url,
            .archive_path = plan.downloads_dir / plan.archive_name,
            .use_cached_archive = use_cached_archive,
            .verify_checksum = !info.checksum_url.empty(),
            .checksum_url = info.checksum_url,
        };
    }

    if (info.name == "llvm") {
        plan.post_install_actions.push_back({
            .kind = PostInstallActionKind::SetupLlvmConfig,
            .dest = plan.dest,
            .version = info.version,
        });
    }

    return plan;
}

inline auto self_update_archive_extension(bool is_windows) -> std::string_view {
    return is_windows ? ".zip" : ".tar.gz";
}

inline auto self_update_archive_name(bool is_windows) -> std::string {
    return std::format("intron{}", self_update_archive_extension(is_windows));
}

inline auto self_update_download_url(
    std::string_view version,
    std::string_view triple,
    bool is_windows) -> std::string
{
    auto ext = self_update_archive_extension(is_windows);
    return std::format(
        "https://github.com/misut/intron/releases/download/v{}/intron-v{}-{}{}",
        version,
        version,
        triple,
        ext);
}

} // namespace intron
