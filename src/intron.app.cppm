export module intron.app;
import std;
import intron.domain;
import config;
import installer;
import net;
import registry;

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif

namespace {

constexpr auto intron_version = EXON_PKG_VERSION;

auto exists_with_ports(intron::RuntimePorts const& ports,
                       std::filesystem::path const& path) -> bool
{
    if (ports.filesystem.exists) {
        return ports.filesystem.exists(path);
    }
    return std::filesystem::exists(path);
}

auto resolved_home_dir(intron::RuntimePorts const& ports) -> std::filesystem::path {
    if (ports.environment.home_dir) {
        if (auto home = ports.environment.home_dir()) {
            return *home;
        }
    }
    throw std::runtime_error("HOME environment variable not set");
}

auto resolved_intron_home(intron::RuntimePorts const& ports) -> std::filesystem::path {
    return installer::intron_home_path(resolved_home_dir(ports));
}

auto resolved_msvc_environment(intron::RuntimePorts const& ports)
    -> std::optional<intron::MsvcEnvironment>
{
    if (ports.toolchain.msvc_environment) {
        return ports.toolchain.msvc_environment();
    }
    auto env = installer::msvc_environment();
    if (!env.has_value()) {
        return std::nullopt;
    }
    return intron::MsvcEnvironment{
        .bin_dir = env->bin_dir,
        .cl = env->cl,
        .variables = env->variables,
    };
}

auto resolved_msvc_update_status(intron::RuntimePorts const& ports)
    -> std::expected<intron::MsvcUpdateStatus, std::string>
{
    if (ports.toolchain.msvc_update_status) {
        return ports.toolchain.msvc_update_status();
    }
    return installer::msvc_update_status();
}

auto resolved_msvc_upgrade(intron::RuntimePorts const& ports)
    -> std::expected<intron::MsvcUpdateStatus, std::string>
{
    if (ports.toolchain.msvc_upgrade) {
        return ports.toolchain.msvc_upgrade();
    }
    return installer::upgrade_msvc();
}

auto resolved_latest_version(intron::RuntimePorts const& ports, std::string_view tool)
    -> std::optional<std::string>
{
    if (ports.toolchain.latest_version) {
        return ports.toolchain.latest_version(tool);
    }
    return installer::latest_version(tool);
}

auto add_missing_msvc_result(intron::CommandResult& result) -> void {
    result.exit_code = 1;
    result.add_stderr("error: msvc is not installed");
    result.add_stderr("hint: run 'intron install msvc 2022'");
}

auto usage_result(int exit_code) -> intron::CommandResult {
    auto result = intron::CommandResult{
        .exit_code = exit_code,
    };
    for (auto const& line : intron::usage_lines(intron_version)) {
        result.add_stdout(line);
    }
    return result;
}

auto unknown_command_result(std::string_view command) -> intron::CommandResult {
    auto result = intron::CommandResult{
        .exit_code = 1,
    };
    result.add_stderr(std::format("error: unknown command '{}'", command));
    for (auto const& line : intron::usage_lines(intron_version)) {
        result.add_stdout(line);
    }
    return result;
}

auto command_from_string(std::string_view command) -> std::optional<intron::CommandKind> {
    if (command == "install") return intron::CommandKind::Install;
    if (command == "remove") return intron::CommandKind::Remove;
    if (command == "list") return intron::CommandKind::List;
    if (command == "which") return intron::CommandKind::Which;
    if (command == "default") return intron::CommandKind::Default;
    if (command == "use") return intron::CommandKind::Use;
    if (command == "update") return intron::CommandKind::Update;
    if (command == "upgrade") return intron::CommandKind::Upgrade;
    if (command == "env") return intron::CommandKind::Env;
    if (command == "exec") return intron::CommandKind::Exec;
    if (command == "self-update") return intron::CommandKind::SelfUpdate;
    if (command == "help" || command == "--help" || command == "-h") {
        return intron::CommandKind::Help;
    }
    return std::nullopt;
}

auto add_config_write_notice(intron::CommandResult& result) -> void {
    result.add_stdout("wrote .intron.toml");
}

auto platform_values(intron::ConfigDocument const& document, std::string_view platform)
    -> std::map<std::string, std::string>
{
    if (auto it = document.platforms.find(std::string{platform}); it != document.platforms.end()) {
        return it->second;
    }
    return {};
}

auto merged_values(std::map<std::string, std::string> const& base,
                   std::map<std::string, std::string> const& override)
    -> std::map<std::string, std::string>
{
    auto merged = base;
    for (auto const& [tool, version] : override) {
        merged[tool] = version;
    }
    return merged;
}

auto normalize_versions(std::map<std::string, std::string>& values) -> void {
    for (auto& [tool, version] : values) {
        version = registry::normalize_requested_version(tool, version);
    }
}

auto set_platform_values(intron::ConfigDocument& document,
                         std::string_view platform,
                         std::map<std::string, std::string> values) -> void
{
    auto key = std::string{platform};
    if (values.empty()) {
        document.platforms.erase(key);
        return;
    }
    document.platforms[key] = std::move(values);
}

auto apply_current_defaults(intron::ConfigDocument& document)
    -> std::pair<std::map<std::string, std::string>, std::map<std::string, std::string>>
{
    auto const platform = std::string{registry::platform_name()};
    auto defaults = config::load_full_defaults();

    auto common = merged_values(defaults.common, document.common);
    auto current_platform_values = merged_values(
        platform_values(defaults, platform),
        platform_values(document, platform));

    normalize_versions(common);
    normalize_versions(current_platform_values);

    for (auto it = common.begin(); it != common.end();) {
        if (registry::is_system_tool(it->first)) {
            current_platform_values.try_emplace(it->first, it->second);
            it = common.erase(it);
            continue;
        }
        ++it;
    }

    document.common = common;
    set_platform_values(document, platform, current_platform_values);

    return {std::move(common), std::move(current_platform_values)};
}

auto cmd_install(intron::CommandRequest const& request) -> intron::CommandResult {
    auto result = intron::CommandResult{};
    if (request.args.size() < 2) {
        auto toolchain = config::load_project_toolchain();
        if (toolchain.empty()) {
            result.exit_code = 1;
            result.add_stderr("Usage: intron install <tool> <version>");
            result.add_stderr("       intron install  (reads .intron.toml)");
            return result;
        }
        int failed = 0;
        for (auto const& [tool, version] : toolchain) {
            auto info = registry::resolve(tool, version);
            if (!installer::install(info)) {
                ++failed;
            }
        }
        result.exit_code = failed > 0 ? 1 : 0;
        return result;
    }

    auto info = registry::resolve(request.args[0], request.args[1]);
    result.exit_code = installer::install(info) ? 0 : 1;
    return result;
}

auto cmd_remove(intron::CommandRequest const& request) -> intron::CommandResult {
    auto result = intron::CommandResult{};
    if (request.args.size() != 2) {
        result.exit_code = 1;
        result.add_stderr("Usage: intron remove <tool> <version>");
        return result;
    }
    result.exit_code = installer::remove(request.args[0], request.args[1]) ? 0 : 1;
    return result;
}

auto cmd_list() -> intron::CommandResult {
    auto result = intron::CommandResult{};
    auto installed = installer::list_installed();
    if (installed.empty()) {
        result.add_stdout("No toolchains installed");
        return result;
    }

    auto defaults = config::load_effective_defaults();
    for (auto const& [tool, version] : installed) {
        auto it = defaults.find(tool);
        auto display_version = version;
        auto default_version = std::optional<std::string>{};
        if (registry::is_system_tool(tool)) {
            display_version = registry::normalize_requested_version(tool, version);
            if (it != defaults.end()) {
                default_version = registry::normalize_requested_version(tool, it->second);
            }
        } else if (it != defaults.end()) {
            default_version = it->second;
        }

        if (default_version && *default_version == display_version) {
            result.add_stdout(std::format("{} {} (default)", tool, display_version));
        } else {
            result.add_stdout(std::format("{} {}", tool, display_version));
        }
    }
    return result;
}

auto cmd_which(intron::CommandRequest const& request) -> intron::CommandResult {
    auto result = intron::CommandResult{};
    if (request.args.size() != 1) {
        result.exit_code = 1;
        result.add_stderr("Usage: intron which <binary>");
        return result;
    }

    auto binary = std::string_view{request.args[0]};
    auto tool = intron::tool_for_binary(binary);
    if (!tool) {
        result.exit_code = 1;
        result.add_stderr(std::format("error: unknown binary '{}'", binary));
        return result;
    }

    auto version = config::get_default(*tool);
    if (!version) {
        result.exit_code = 1;
        result.add_stderr(std::format("error: no default version set for {}", *tool));
        result.add_stderr(std::format("hint: run 'intron default {} <version>'", *tool));
        return result;
    }

    auto path = installer::which(binary, *tool, *version);
    if (!path) {
        result.exit_code = 1;
        result.add_stderr(std::format(
            "error: '{}' not found in {} {}",
            binary,
            *tool,
            *version));
        return result;
    }

    result.add_stdout(path->string());
    return result;
}

auto cmd_default(intron::CommandRequest const& request,
                 intron::RuntimePorts const& ports) -> intron::CommandResult
{
    auto result = intron::CommandResult{};
    auto parsed = intron::split_platform_args(request.args);
    if (!parsed) {
        throw std::runtime_error(parsed.error());
    }
    if (parsed->positional.size() != 2) {
        result.exit_code = 1;
        result.add_stderr("Usage: intron default <tool> <version> [--platform <name>]");
        return result;
    }

    auto tool = std::string_view{parsed->positional[0]};
    auto version = registry::normalize_requested_version(tool, parsed->positional[1]);
    if (!registry::is_system_tool(tool)) {
        auto home = resolved_intron_home(ports);
        auto path = installer::toolchain_path(home, tool, version);
        if (!exists_with_ports(ports, path)) {
            result.exit_code = 1;
            result.add_stderr(std::format("error: {} {} is not installed", tool, version));
            result.add_stderr(std::format("hint: run 'intron install {} {}'", tool, version));
            return result;
        }
    }

    config::set_default(tool, version, parsed->platform.value_or(""));
    if (parsed->platform) {
        result.add_stdout(std::format(
            "Set {} default to {} (platform: {})",
            tool,
            version,
            *parsed->platform));
    } else {
        result.add_stdout(std::format("Set {} default to {}", tool, version));
    }
    return result;
}

auto cmd_use(intron::CommandRequest const& request,
             intron::RuntimePorts const& ports) -> intron::CommandResult
{
    auto result = intron::CommandResult{};
    auto parsed = intron::split_platform_args(request.args);
    if (!parsed) {
        throw std::runtime_error(parsed.error());
    }

    auto document = config::load_full_project_config();
    if (parsed->positional.empty()) {
        auto [common, current_platform_values] = apply_current_defaults(document);
        if (common.empty() && current_platform_values.empty()) {
            result.exit_code = 1;
            result.add_stderr("error: no default versions set");
            result.add_stderr("hint: run 'intron default <tool> <version>' first");
            return result;
        }
        for (auto const& [tool, version] : common) {
            result.add_stdout(std::format("set {} {}", tool, version));
        }
        auto current_platform = std::string{registry::platform_name()};
        for (auto const& [tool, version] : current_platform_values) {
            result.add_stdout(std::format(
                "set {} {} (platform: {})",
                tool,
                version,
                current_platform));
        }
    } else {
        auto tool = parsed->positional[0];
        auto version = std::string{};
        if (parsed->positional.size() >= 2) {
            version = registry::normalize_requested_version(tool, parsed->positional[1]);
            if (!registry::is_system_tool(tool)) {
                auto home = resolved_intron_home(ports);
                auto dest = installer::toolchain_path(home, tool, version);
                if (!exists_with_ports(ports, dest)) {
                    result.add_stdout(std::format("warning: {} {} is not installed", tool, version));
                }
            }
        } else {
            auto def = config::get_default(tool);
            if (!def) {
                result.exit_code = 1;
                result.add_stderr(std::format("error: no default version for {}", tool));
                return result;
            }
            version = *def;
        }

        if (parsed->platform) {
            document.platforms[*parsed->platform][tool] = version;
            result.add_stdout(std::format(
                "set {} {} (platform: {})",
                tool,
                version,
                *parsed->platform));
        } else {
            document.common[tool] = version;
            result.add_stdout(std::format("set {} {}", tool, version));
        }
    }

    config::write_project_config(document);
    add_config_write_notice(result);
    return result;
}

auto cmd_update(intron::CommandRequest const& request,
                intron::RuntimePorts const& ports) -> intron::CommandResult
{
    auto result = intron::CommandResult{};
    if (request.args.size() > 1) {
        result.exit_code = 1;
        result.add_stderr("Usage: intron update [tool]");
        return result;
    }

    if (request.args.size() == 1 && request.args.front() == "msvc") {
        auto status = resolved_msvc_update_status(ports);
        if (!status) {
            result.exit_code = 1;
            result.add_stderr(std::format("error: {}", status.error()));
            return result;
        }
        if (status->state == intron::MsvcUpdateState::Missing) {
            add_missing_msvc_result(result);
            return result;
        }
        result.add_stdout(intron::render_msvc_update_status(*status));
        return result;
    }

    auto current = intron::build_tool_map(
        installer::list_installed(),
        config::load_effective_defaults());

    if (!request.args.empty()) {
        auto tool = request.args.front();
        if (!current.contains(tool) || registry::is_system_tool(tool)) {
            result.exit_code = 1;
            result.add_stderr(std::format("error: {} is not installed", tool));
            return result;
        }
        auto version = current[tool];
        current.clear();
        current[tool] = version;
    }

    auto has_non_system_tools = std::ranges::any_of(current, [](auto const& entry) {
        return !registry::is_system_tool(entry.first);
    });

    if (!has_non_system_tools) {
        for (auto tool : registry::supported_tools) {
            if (registry::is_system_tool(tool)) {
                continue;
            }
            if (auto latest = resolved_latest_version(ports, tool)) {
                result.add_stdout(std::format("{}: latest {}", tool, *latest));
            }
        }
        return result;
    }

    bool has_update = false;
    for (auto const& [tool, version] : current) {
        if (registry::is_system_tool(tool)) {
            continue;
        }
        auto status = intron::make_update_status(tool, version, resolved_latest_version(ports, tool));
        if (status.state == intron::UpdateState::UpdateAvailable) {
            has_update = true;
        }
        result.add_stdout(intron::render_update_status(status));
    }

    if (has_update) {
        result.add_stdout("");
        result.add_stdout("Run 'intron install <tool> <version>' to update");
    }
    return result;
}

auto cmd_upgrade(intron::CommandRequest const& request,
                 intron::RuntimePorts const& ports) -> intron::CommandResult
{
    auto result = intron::CommandResult{};
    if (request.args.size() > 1) {
        result.exit_code = 1;
        result.add_stderr("Usage: intron upgrade [tool]");
        return result;
    }

    if (request.args.size() == 1 && request.args.front() == "msvc") {
        auto status = resolved_msvc_update_status(ports);
        if (!status) {
            result.exit_code = 1;
            result.add_stderr(std::format("error: {}", status.error()));
            return result;
        }
        if (status->state == intron::MsvcUpdateState::Missing) {
            add_missing_msvc_result(result);
            return result;
        }
        if (status->state == intron::MsvcUpdateState::Unknown) {
            result.exit_code = 1;
            result.add_stdout(intron::render_msvc_upgrade_check(*status));
            return result;
        }
        if (status->state == intron::MsvcUpdateState::UpToDate) {
            result.add_stdout(intron::render_msvc_upgrade_check(*status));
            return result;
        }

        result.add_stdout(intron::render_msvc_upgrade_check(*status));
        auto upgraded = resolved_msvc_upgrade(ports);
        if (!upgraded) {
            result.exit_code = 1;
            result.add_stderr(std::format("error: {}", upgraded.error()));
            return result;
        }
        if (upgraded->state != intron::MsvcUpdateState::UpToDate) {
            result.exit_code = 1;
            result.add_stderr("error: msvc upgrade did not reach the latest servicing version");
            return result;
        }
        result.add_stdout("");
        result.add_stdout(std::format("Upgraded msvc to {}", upgraded->current_version));
        return result;
    }

    auto current = intron::build_tool_map(
        installer::list_installed(),
        config::load_effective_defaults());

    if (!request.args.empty()) {
        auto tool = request.args.front();
        if (!current.contains(tool)) {
            result.exit_code = 1;
            result.add_stderr(std::format("error: {} is not installed", tool));
            return result;
        }
        auto version = current[tool];
        current.clear();
        current[tool] = version;
    }

    auto has_non_system_tools = std::ranges::any_of(current, [](auto const& entry) {
        return !registry::is_system_tool(entry.first);
    });

    if (!has_non_system_tools) {
        result.add_stdout("No toolchains installed");
        return result;
    }

    int upgraded = 0;
    for (auto const& [tool, version] : current) {
        if (registry::is_system_tool(tool)) {
            continue;
        }
        auto status = intron::make_update_status(tool, version, resolved_latest_version(ports, tool));
        if (status.state == intron::UpdateState::Unknown) {
            result.add_stdout(intron::render_upgrade_check(status));
            continue;
        }
        if (status.state == intron::UpdateState::UpToDate) {
            result.add_stdout(intron::render_upgrade_check(status));
            continue;
        }

        result.add_stdout(intron::render_upgrade_check(status));
        auto info = registry::resolve(tool, *status.latest_version);
        if (!installer::install(info)) {
            result.add_stderr(std::format("error: failed to upgrade {}", tool));
            continue;
        }
        config::set_default(tool, *status.latest_version);
        ++upgraded;
    }

    if (upgraded > 0) {
        result.add_stdout("");
        result.add_stdout(std::format(
            "Upgraded {} tool{}",
            upgraded,
            upgraded == 1 ? "" : "s"));
    }
    return result;
}

auto exec_usage_result() -> intron::CommandResult {
    auto result = intron::CommandResult{
        .exit_code = 1,
    };
    result.add_stderr("Usage: intron exec -- <command> [args...]");
    return result;
}

auto snapshot_inherited_environment(intron::RuntimePorts const& ports)
    -> std::map<std::string, std::string>
{
    auto inherited = std::map<std::string, std::string>{};
    if (!ports.environment.get) {
        return inherited;
    }
    for (auto const* key : {"PATH"
#ifdef _WIN32
        , "Path"
#endif
    }) {
        if (auto value = ports.environment.get(key)) {
            inherited[std::string{key}] = *value;
        }
    }
    return inherited;
}

auto resolve_env_plan(intron::RuntimePorts const& ports)
    -> std::expected<intron::EnvPlan, intron::CommandResult>
{
    auto defaults = config::load_effective_defaults();
    if (defaults.empty()) {
        auto result = intron::CommandResult{
            .exit_code = 1,
        };
        result.add_stderr("error: no default versions set");
        result.add_stderr("hint: run 'intron default <tool> <version>'");
        return std::unexpected(std::move(result));
    }

    auto intron_home = resolved_intron_home(ports);

    std::optional<intron::MsvcEnvironment> msvc_env;
    if (defaults.contains("msvc")) {
        msvc_env = resolved_msvc_environment(ports);
        if (!msvc_env) {
            auto result = intron::CommandResult{
                .exit_code = 1,
            };
            result.add_stderr("error: msvc is configured as a default toolchain but was not detected");
            result.add_stderr("hint: run 'intron install msvc 2022'");
            return std::unexpected(std::move(result));
        }
    }

    std::vector<std::string> path_entries;
    for (auto const& [tool, version] : defaults) {
        if (tool == "wasi-sdk" || tool == "msvc" || tool == "android-ndk") {
            continue;
        }
        if (registry::is_system_tool(tool)) {
            continue;
        }

        auto base = installer::toolchain_path(intron_home, tool, version);
        if (!exists_with_ports(ports, base)) {
            continue;
        }
        if (tool == "ninja" || tool == "wasmtime") {
            path_entries.push_back(base.string());
        } else {
            path_entries.push_back((base / "bin").string());
        }
    }
    if (msvc_env) {
        path_entries.push_back(msvc_env->bin_dir.string());
        if (ports.toolchain.windows_sdk_bin_dirs) {
            auto pinned = config::get_windows_sdk_pin();
            for (auto const& dir :
                 ports.toolchain.windows_sdk_bin_dirs(std::move(pinned))) {
                path_entries.push_back(dir.string());
            }
        }
    }

#ifdef _WIN32
    constexpr auto path_sep = ';';
#else
    constexpr auto path_sep = ':';
#endif

    auto path_value = std::optional<std::string>{};
    if (!path_entries.empty()) {
        auto combined = std::string{};
        for (auto const& path : path_entries) {
            if (!combined.empty()) {
                combined += path_sep;
            }
            combined += path;
        }
        if (msvc_env) {
            if (auto it = msvc_env->variables.find("Path");
                it != msvc_env->variables.end() && !it->second.empty()) {
                combined += std::format("{}{}", path_sep, it->second);
            }
        }
        path_value = combined;
    }

    auto cc = std::optional<std::filesystem::path>{};
    auto cxx = std::optional<std::filesystem::path>{};

#ifdef _WIN32
    if (defaults.contains("msvc") && msvc_env) {
        cc = msvc_env->cl;
        cxx = msvc_env->cl;
    } else if (defaults.contains("llvm")) {
        auto llvm_bin = installer::toolchain_path(intron_home, "llvm", defaults.at("llvm")) / "bin";
        auto clang = llvm_bin / "clang-cl.exe";
        if (exists_with_ports(ports, clang)) {
            cc = clang;
            cxx = clang;
        }
    }
#else
    if (defaults.contains("llvm")) {
        auto llvm_bin = installer::toolchain_path(intron_home, "llvm", defaults.at("llvm")) / "bin";
        auto clang = llvm_bin / "clang";
        auto clangxx = llvm_bin / "clang++";
        if (exists_with_ports(ports, clang)) {
            cc = clang;
            cxx = clangxx;
        }
    }
#endif

    auto extra_vars = std::map<std::string, std::string>{};
#ifdef _WIN32
    if (msvc_env) {
        for (auto const& key : {"INCLUDE", "LIB", "LIBPATH"}) {
            if (auto it = msvc_env->variables.find(key);
                it != msvc_env->variables.end() && !it->second.empty()) {
                extra_vars[key] = it->second;
            }
        }
    }
#endif

    auto wasi_sdk_path = std::optional<std::filesystem::path>{};
    if (defaults.contains("wasi-sdk")) {
        auto wasi = installer::toolchain_path(intron_home, "wasi-sdk", defaults.at("wasi-sdk"));
        if (exists_with_ports(ports, wasi)) {
            wasi_sdk_path = wasi;
        }
    }

    auto plan = intron::build_env_plan(
        path_value,
        cc,
        cxx,
        extra_vars,
        wasi_sdk_path);

    return plan;
}

auto cmd_env(intron::RuntimePorts const& ports) -> intron::CommandResult {
    auto resolved = resolve_env_plan(ports);
    if (!resolved) {
        return resolved.error();
    }

#ifdef _WIN32
    constexpr auto is_windows = true;
#else
    constexpr auto is_windows = false;
#endif

    auto result = intron::CommandResult{};
    for (auto const& line : intron::render_env_lines(*resolved, is_windows)) {
        result.add_stdout(line);
    }
    return result;
}

auto cmd_exec(intron::CommandRequest const& request,
              intron::RuntimePorts const& ports) -> intron::CommandResult
{
    auto child_argv = intron::parse_exec_args(request.args);
    if (!child_argv) {
        return exec_usage_result();
    }

    auto resolved = resolve_env_plan(ports);
    if (!resolved) {
        return resolved.error();
    }

    auto result = intron::CommandResult{};
    if (!ports.process.run) {
        result.exit_code = 1;
        result.add_stderr("error: process runner is not configured");
        return result;
    }

    auto request_env = intron::materialize_env_overrides(
        *resolved,
        snapshot_inherited_environment(ports));
    auto run_result = ports.process.run({
        .argv = *child_argv,
        .env_overrides = std::move(request_env),
    });
    if (!run_result) {
        result.exit_code = 1;
        result.add_stderr(std::format("error: {}", run_result.error()));
        return result;
    }

    result.exit_code = *run_result;
    return result;
}

} // namespace

export namespace intron::app {

auto package_version() -> std::string_view {
    return intron_version;
}

auto parse_command_request(int argc, char* argv[])
    -> std::expected<intron::CommandRequest, intron::CommandResult>
{
    if (argc < 2) {
        return std::unexpected(usage_result(1));
    }

    auto command = std::string_view{argv[1]};
    auto kind = command_from_string(command);
    if (!kind) {
        return std::unexpected(unknown_command_result(command));
    }

    auto request = intron::CommandRequest{
        .command = *kind,
        .raw_command = std::string{command},
        .self_path = argc > 0 ? std::string{argv[0]} : std::string{},
    };
    request.args.reserve(std::max(argc - 2, 0));
    for (int i = 2; i < argc; ++i) {
        request.args.emplace_back(argv[i]);
    }
    return request;
}

auto run_command(intron::CommandRequest const& request,
                 intron::RuntimePorts const& ports) -> intron::CommandResult
{
    switch (request.command) {
    case intron::CommandKind::Install:
        return cmd_install(request);
    case intron::CommandKind::Remove:
        return cmd_remove(request);
    case intron::CommandKind::List:
        return cmd_list();
    case intron::CommandKind::Which:
        return cmd_which(request);
    case intron::CommandKind::Default:
        return cmd_default(request, ports);
    case intron::CommandKind::Use:
        return cmd_use(request, ports);
    case intron::CommandKind::Update:
        return cmd_update(request, ports);
    case intron::CommandKind::Upgrade:
        return cmd_upgrade(request, ports);
    case intron::CommandKind::Env:
        return cmd_env(ports);
    case intron::CommandKind::Exec:
        return cmd_exec(request, ports);
    case intron::CommandKind::Help:
        return usage_result(0);
    case intron::CommandKind::SelfUpdate:
        break;
    }
    throw std::runtime_error("self-update is handled at the edge");
}

} // namespace intron::app
