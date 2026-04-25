export module intron.output;
import std;
import cppx.terminal;
import intron.domain;

export namespace intron {

inline auto terminal_options() -> cppx::terminal::TerminalOptions {
    return cppx::terminal::TerminalOptions{
        .color_env = "INTRON_COLOR",
    };
}

inline auto status_line(cppx::terminal::StatusKind status,
                        std::string_view message,
                        bool color_enabled = false) -> std::string
{
    return std::format(
        "  {} {}",
        cppx::terminal::status_cell(status, color_enabled, 4),
        message);
}

inline auto stage_line(std::string_view name,
                       int index,
                       int total,
                       std::string_view context = {},
                       bool color_enabled = false) -> std::string
{
    return cppx::terminal::stage(name, index, total, context, color_enabled);
}

inline auto section_line(std::string_view title,
                         bool color_enabled = false) -> std::string
{
    return cppx::terminal::section(title, color_enabled);
}

inline auto key_value_line(std::string_view key,
                           std::string_view value,
                           std::size_t width = 12) -> std::string
{
    return cppx::terminal::key_value(key, value, width);
}

inline auto error_line(std::string_view message,
                       bool color_enabled = false) -> std::string
{
    return std::format(
        "{} {}",
        cppx::terminal::style("error:", cppx::terminal::StyleRole::error, color_enabled),
        message);
}

inline auto warning_line(std::string_view message,
                         bool color_enabled = false) -> std::string
{
    return std::format(
        "{} {}",
        cppx::terminal::style("warning:", cppx::terminal::StyleRole::warning, color_enabled),
        message);
}

inline auto hint_line(std::string_view message,
                      bool color_enabled = false) -> std::string
{
    return std::format(
        "{} {}",
        cppx::terminal::style("hint:", cppx::terminal::StyleRole::dim, color_enabled),
        message);
}

inline auto usage_lines(std::string_view version,
                        bool color_enabled = false) -> std::vector<std::string>
{
    return {
        cppx::terminal::style(std::format("intron {}", version),
                              cppx::terminal::StyleRole::bold,
                              color_enabled),
        "",
        "Usage: intron <command> [args...]",
        "",
        section_line("Commands:", color_enabled),
        "  install [tool] [version]                     Install toolchain(s) (reads .intron.toml if no args)",
        "  remove  <tool> <version>                     Remove a toolchain",
        "  list                                         List installed toolchains",
        "  which   <binary>                             Print path to binary",
        "  default <tool> <version> [--platform <name>]  Set global default version",
        "  use     [tool] [version] [--platform <name>]  Set project toolchain in .intron.toml",
        "  update  [tool]                                Check for updates",
        "  upgrade [tool]                                Upgrade tools to latest",
        "  env     [--path-only|--github]                Print environment variables",
        "  exec    -- <command> [args...]                Run a command with intron environment",
        "  self-update                                  Update intron itself",
        "  help                                         Show this message",
        "",
        section_line("Tools:", color_enabled),
        "  android-ndk, cmake, llvm, msvc, ninja, wasi-sdk, wasmtime",
        "",
        section_line("Options:", color_enabled),
        "  --platform <name>  Target a specific platform section (linux, macos, windows)",
        "",
        section_line("Output:", color_enabled),
        "  INTRON_COLOR=auto|always|never controls ANSI color in human output",
        "  NO_COLOR=1 disables color in auto mode",
    };
}

inline auto render_update_status(UpdateStatus const& status,
                                 bool color_enabled = false) -> std::string
{
    switch (status.state) {
    case UpdateState::Unknown:
        return status_line(cppx::terminal::StatusKind::fail, std::format(
            "{} {}: could not check latest",
            status.tool,
            status.current_version), color_enabled);
    case UpdateState::UpToDate:
        return status_line(cppx::terminal::StatusKind::ok, std::format(
            "{} {} (up to date)",
            status.tool,
            status.current_version), color_enabled);
    case UpdateState::UpdateAvailable:
        return status_line(cppx::terminal::StatusKind::run, std::format(
            "{} {} -> {} (update available)",
            status.tool,
            status.current_version,
            *status.latest_version), color_enabled);
    }
    return {};
}

inline auto render_upgrade_check(UpdateStatus const& status,
                                 bool color_enabled = false) -> std::string
{
    switch (status.state) {
    case UpdateState::Unknown:
        return status_line(
            cppx::terminal::StatusKind::fail,
            std::format("{}: could not check latest version", status.tool),
            color_enabled);
    case UpdateState::UpToDate:
        return status_line(
            cppx::terminal::StatusKind::ok,
            std::format("{} {} (up to date)", status.tool, status.current_version),
            color_enabled);
    case UpdateState::UpdateAvailable:
        return status_line(cppx::terminal::StatusKind::run, std::format(
            "{} {} -> {}...",
            status.tool,
            status.current_version,
            *status.latest_version), color_enabled);
    }
    return {};
}

inline auto render_msvc_update_status(MsvcUpdateStatus const& status,
                                      bool color_enabled = false) -> std::string
{
    switch (status.state) {
    case MsvcUpdateState::Missing:
        return status_line(cppx::terminal::StatusKind::fail,
                           "msvc: not installed",
                           color_enabled);
    case MsvcUpdateState::Unknown:
        if (status.current_version.empty()) {
            return status_line(cppx::terminal::StatusKind::fail,
                               "msvc: could not check latest",
                               color_enabled);
        }
        return status_line(cppx::terminal::StatusKind::fail,
                           std::format("msvc {}: could not check latest",
                                       status.current_version),
                           color_enabled);
    case MsvcUpdateState::UpToDate:
        return status_line(cppx::terminal::StatusKind::ok,
                           std::format("msvc {} (up to date)", status.current_version),
                           color_enabled);
    case MsvcUpdateState::UpdateAvailable:
        return status_line(cppx::terminal::StatusKind::run, std::format(
            "msvc {} -> {} (update available)",
            status.current_version,
            *status.latest_version), color_enabled);
    }
    return {};
}

inline auto render_msvc_upgrade_check(MsvcUpdateStatus const& status,
                                      bool color_enabled = false) -> std::string
{
    switch (status.state) {
    case MsvcUpdateState::Missing:
        return status_line(cppx::terminal::StatusKind::fail,
                           "msvc: not installed",
                           color_enabled);
    case MsvcUpdateState::Unknown:
        return status_line(cppx::terminal::StatusKind::fail,
                           "msvc: could not check latest version",
                           color_enabled);
    case MsvcUpdateState::UpToDate:
        return status_line(cppx::terminal::StatusKind::ok,
                           std::format("msvc {} (up to date)", status.current_version),
                           color_enabled);
    case MsvcUpdateState::UpdateAvailable:
        return status_line(cppx::terminal::StatusKind::run, std::format(
            "msvc {} -> {}...",
            status.current_version,
            *status.latest_version), color_enabled);
    }
    return {};
}

} // namespace intron
