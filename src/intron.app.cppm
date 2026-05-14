export module intron.app;
import std;
import cppx.cli;
import cppx.terminal;
import cppx.terminal.system;
import intron.domain;
import intron.output;
import config;
import installer;
import net;
import registry;

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif

namespace {

constexpr auto intron_version = EXON_PKG_VERSION;

auto stdout_color_enabled() -> bool {
    return cppx::terminal::system::stdout_color_enabled(intron::terminal_options());
}

auto stderr_color_enabled() -> bool {
    return cppx::terminal::system::stderr_color_enabled(intron::terminal_options());
}

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
    auto color = stderr_color_enabled();
    result.add_stderr(intron::error_line("msvc is not installed", color));
    result.add_stderr(intron::hint_line("run 'intron install msvc 2022'", color));
}

enum class StatusOutputMode {
  Human,
  Json,
};

struct EffectiveToolchainEntry {
  std::string tool;
  std::string version;
  std::string source;
};

struct ToolStatus {
  std::string tool;
  std::string version;
  bool installed = false;
  bool system = false;
  std::string path;
  std::map<std::string, std::string> binaries;
};

struct EnvironmentStatus {
  bool available = false;
  std::string error;
  std::map<std::string, std::string> assignments;
  std::vector<std::string> path_entries;
};

struct NetworkStatus {
  std::string env_value;
  std::string selected_backend;
};

struct MsvcStatus {
  bool configured = false;
  bool available = false;
  std::string status;
  std::string bin_dir;
  std::string cl;
  std::vector<std::string> windows_sdk_bin_dirs;
};

struct StatusDiagnostic {
  cppx::terminal::DiagnosticSeverity severity =
      cppx::terminal::DiagnosticSeverity::info;
  std::string context;
  std::string message;
  std::vector<std::string> hints;
};

struct StatusReport {
  std::string version;
  std::string platform;
  std::string triple;
  std::optional<std::filesystem::path> project_config;
  std::vector<EffectiveToolchainEntry> effective_toolchain;
  std::vector<ToolStatus> tools;
  EnvironmentStatus environment;
  NetworkStatus network;
  MsvcStatus msvc;
  std::vector<StatusDiagnostic> diagnostics;
};

auto status_command_spec(std::string_view command) -> cppx::cli::CommandSpec {
  return cppx::cli::CommandSpec{
      .name = std::string{command},
      .summary = "Show toolchain diagnostics",
      .options =
          {
              cppx::cli::OptionSpec{
                  .name = "output",
                  .arity = cppx::cli::OptionArity::one,
                  .value_name = "mode",
                  .description = "Output mode",
                  .value_hints = {"human", "json"},
              },
          },
      .allow_positionals = false,
  };
}

auto parse_status_output_mode(std::vector<std::string> const &args,
                              std::string_view command)
    -> std::expected<StatusOutputMode, std::string> {
  auto views = std::vector<std::string_view>{};
  views.reserve(args.size());
  for (auto const &arg : args) {
    views.push_back(arg);
  }

  auto parsed = cppx::cli::parse(status_command_spec(command), views);
  if (!parsed) {
    return std::unexpected(parsed.error().message);
  }

  auto output = parsed->value("output").value_or("human");
  if (output == "human") {
    return StatusOutputMode::Human;
  }
  if (output == "json") {
    return StatusOutputMode::Json;
  }
  return std::unexpected(
      std::format("invalid output mode '{}' (expected: human, json)", output));
}

auto diagnostic_severity_string(cppx::terminal::DiagnosticSeverity severity)
    -> std::string_view {
  switch (severity) {
  case cppx::terminal::DiagnosticSeverity::info:
    return "info";
  case cppx::terminal::DiagnosticSeverity::warning:
    return "warning";
  case cppx::terminal::DiagnosticSeverity::error:
    return "error";
  }
  return "info";
}

auto add_status_diagnostic(StatusReport &report,
                           cppx::terminal::DiagnosticSeverity severity,
                           std::string context, std::string message,
                           std::vector<std::string> hints = {}) -> void {
  report.diagnostics.push_back({
      .severity = severity,
      .context = std::move(context),
      .message = std::move(message),
      .hints = std::move(hints),
  });
}

auto backend_string(net::Backend backend) -> std::string_view {
  switch (backend) {
  case net::Backend::Auto:
    return "auto";
  case net::Backend::Cppx:
    return "cppx";
  case net::Backend::Shell:
    return "shell";
  }
  return "auto";
}

auto get_env_value(intron::RuntimePorts const &ports, std::string_view key)
    -> std::optional<std::string> {
  if (ports.environment.get) {
    if (auto value = ports.environment.get(key)) {
      return value;
    }
  }
  auto owned = std::string{key};
  if (auto const *value = std::getenv(owned.c_str()); value && *value) {
    return std::string{value};
  }
  return std::nullopt;
}

auto platform_values(intron::ConfigDocument const &document,
                     std::string_view platform)
    -> std::map<std::string, std::string>;

auto resolve_env_plan(intron::RuntimePorts const &ports)
    -> std::expected<intron::EnvPlan, intron::CommandResult>;

auto add_effective_entries_from(
    std::map<std::string, EffectiveToolchainEntry> &entries,
    std::map<std::string, std::string> const &values, std::string_view source)
    -> void {
  for (auto const &[tool, version] : values) {
    entries[tool] = EffectiveToolchainEntry{
        .tool = tool,
        .version = version,
        .source = std::string{source},
    };
  }
}

auto normalize_effective_versions(StatusReport &report) -> void {
  for (auto &entry : report.effective_toolchain) {
    try {
      entry.version =
          registry::normalize_requested_version(entry.tool, entry.version);
    } catch (std::exception const &e) {
      add_status_diagnostic(
          report, cppx::terminal::DiagnosticSeverity::error, "config", e.what(),
          {"fix the version in .intron.toml or ~/.intron/config.toml"});
    }
  }
}

auto collect_effective_toolchain(StatusReport &report,
                                 intron::ConfigDocument const &defaults,
                                 intron::ConfigDocument const &project)
    -> void {
  auto const platform = std::string{registry::platform_name()};
  auto entries = std::map<std::string, EffectiveToolchainEntry>{};
  add_effective_entries_from(entries, defaults.common, "defaults");
  add_effective_entries_from(entries, platform_values(defaults, platform),
                             std::format("defaults.{}", platform));
  add_effective_entries_from(entries, project.common, "project");
  add_effective_entries_from(entries, platform_values(project, platform),
                             std::format("project.{}", platform));

  for (auto &[_, entry] : entries) {
    report.effective_toolchain.push_back(std::move(entry));
  }
  normalize_effective_versions(report);

  if (report.effective_toolchain.empty()) {
    add_status_diagnostic(report, cppx::terminal::DiagnosticSeverity::error,
                          "config", "no effective toolchain entries",
                          {"run 'intron default <tool> <version>' or create "
                           ".intron.toml with 'intron use'"});
  }
}

auto exists_for_status(intron::RuntimePorts const &ports,
                       std::filesystem::path const &path) -> bool {
  return exists_with_ports(ports, path);
}

auto resolve_binary_path(intron::RuntimePorts const &ports,
                         std::filesystem::path const &intron_home,
                         std::string_view tool, std::string_view version,
                         std::string_view binary)
    -> std::optional<std::filesystem::path> {
  auto base = installer::toolchain_path(intron_home, tool, version);
  auto path = (tool == "ninja" || tool == "wasmtime" || tool == "android-ndk")
                  ? base / binary
                  : base / "bin" / binary;
  if (exists_for_status(ports, path)) {
    return path;
  }
#ifdef _WIN32
  auto exe_path = path;
  exe_path += ".exe";
  if (exists_for_status(ports, exe_path)) {
    return exe_path;
  }
#endif
  return std::nullopt;
}

auto binary_candidates(std::string_view tool)
    -> std::vector<std::pair<std::string, std::string>> {
  if (tool == "cmake") {
    return {{"cmake", "cmake"}};
  }
  if (tool == "ninja") {
    return {{"ninja", "ninja"}};
  }
  if (tool == "llvm") {
#ifdef _WIN32
    return {{"compiler", "clang-cl"}};
#else
    return {{"cc", "clang"}, {"cxx", "clang++"}};
#endif
  }
  if (tool == "wasi-sdk") {
    return {{"wasi-cxx", "clang++"}};
  }
  if (tool == "wasmtime") {
    return {{"wasmtime", "wasmtime"}};
  }
  if (tool == "android-ndk") {
    return {{"ndk-build", "ndk-build"}};
  }
  return {};
}

auto collect_tool_statuses(
    StatusReport &report, intron::RuntimePorts const &ports,
    std::optional<std::filesystem::path> const &intron_home,
    std::optional<intron::MsvcEnvironment> const &msvc_env) -> void {
  for (auto const &entry : report.effective_toolchain) {
    auto status = ToolStatus{
        .tool = entry.tool,
        .version = entry.version,
        .system = registry::is_system_tool(entry.tool),
    };

    if (entry.tool == "sdk") {
      status.installed = !entry.version.empty();
      report.tools.push_back(std::move(status));
      continue;
    }

    if (entry.tool == "msvc") {
      status.installed = msvc_env.has_value();
      if (msvc_env) {
        status.path = msvc_env->bin_dir.string();
        status.binaries["cl"] = msvc_env->cl.string();
      } else {
        add_status_diagnostic(report, cppx::terminal::DiagnosticSeverity::error,
                              "msvc", "msvc is configured but was not detected",
                              {"run 'intron install msvc 2022'"});
      }
      report.tools.push_back(std::move(status));
      continue;
    }

    if (!intron_home) {
      add_status_diagnostic(report, cppx::terminal::DiagnosticSeverity::error,
                            "environment", "HOME environment variable not set",
                            {"set HOME before running intron status"});
      report.tools.push_back(std::move(status));
      continue;
    }

    auto base =
        installer::toolchain_path(*intron_home, entry.tool, entry.version);
    status.path = base.string();
    status.installed = exists_for_status(ports, base);
    if (!status.installed) {
      add_status_diagnostic(
          report, cppx::terminal::DiagnosticSeverity::warning, "tools",
          std::format("{} {} is not installed", entry.tool, entry.version),
          {std::format("run 'intron install {} {}'", entry.tool,
                       entry.version)});
    }

    for (auto const &[label, binary] : binary_candidates(entry.tool)) {
      if (auto path = resolve_binary_path(ports, *intron_home, entry.tool,
                                          entry.version, binary)) {
        status.binaries[label] = path->string();
      }
    }
    report.tools.push_back(std::move(status));
  }
}

auto collect_environment_status(StatusReport &report,
                                intron::RuntimePorts const &ports) -> void {
  auto resolved = resolve_env_plan(ports);
  if (!resolved) {
    report.environment.available = false;
    auto message = std::string{};
    for (auto const &line : resolved.error().stderr_lines) {
      if (!message.empty()) {
        message += "; ";
      }
      message += line;
    }
    report.environment.error =
        message.empty() ? "could not resolve environment" : std::move(message);
    add_status_diagnostic(
        report, cppx::terminal::DiagnosticSeverity::error, "environment",
        report.environment.error,
        {"fix the reported toolchain configuration before using 'intron env'"});
    return;
  }

  report.environment.available = true;
#ifdef _WIN32
  constexpr auto is_windows = true;
#else
  constexpr auto is_windows = false;
#endif
  for (auto const &assignment : resolved->assignments) {
    report.environment.assignments[assignment.key] = assignment.value;
    if (assignment.key == "PATH" && assignment.append_existing) {
      report.environment.path_entries =
          intron::split_path_value(assignment.value, is_windows);
    }
  }
}

auto collect_network_status(StatusReport &report,
                            intron::RuntimePorts const &ports) -> void {
  auto raw = get_env_value(ports, "INTRON_NET_BACKEND");
  report.network.env_value = raw.value_or("");
  auto selected = net::selected_backend_from_string(
      raw ? std::optional<std::string_view>{std::string_view{*raw}}
          : std::nullopt);
  report.network.selected_backend = std::string{backend_string(selected)};
  if (raw && !raw->empty()) {
    auto lowered = std::string{*raw};
    for (auto &ch : lowered) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (selected == net::Backend::Auto && lowered != "auto") {
      add_status_diagnostic(
          report, cppx::terminal::DiagnosticSeverity::warning, "network",
          std::format("unknown INTRON_NET_BACKEND '{}'", *raw),
          {"use INTRON_NET_BACKEND=auto, cppx, or shell"});
    }
  }
}

auto collect_msvc_status(StatusReport &report,
                         intron::RuntimePorts const &ports,
                         std::optional<intron::MsvcEnvironment> const &msvc_env)
    -> void {
  report.msvc.configured =
      std::ranges::any_of(report.effective_toolchain, [](auto const &entry) {
        return entry.tool == "msvc";
      });

#ifdef _WIN32
  report.msvc.available = msvc_env.has_value();
  if (msvc_env) {
    report.msvc.status = report.msvc.configured ? "configured" : "available";
    report.msvc.bin_dir = msvc_env->bin_dir.string();
    report.msvc.cl = msvc_env->cl.string();
  } else {
    report.msvc.status = report.msvc.configured ? "missing" : "not_configured";
  }
  if (ports.toolchain.windows_sdk_bin_dirs) {
    for (auto const &path :
         ports.toolchain.windows_sdk_bin_dirs(config::get_windows_sdk_pin())) {
      report.msvc.windows_sdk_bin_dirs.push_back(path.string());
    }
  }
#else
  report.msvc.available = false;
  report.msvc.status = "not_applicable";
#endif
}

auto build_status_report(intron::RuntimePorts const &ports) -> StatusReport {
  auto report = StatusReport{
      .version = std::string{intron_version},
      .platform = std::string{registry::platform_name()},
      .triple = registry::platform_triple(),
  };

  auto defaults = intron::ConfigDocument{};
  auto project = intron::ConfigDocument{};
  try {
    defaults = config::load_full_defaults();
  } catch (std::exception const &e) {
    add_status_diagnostic(report, cppx::terminal::DiagnosticSeverity::warning,
                          "config", e.what());
  }

  try {
    report.project_config = config::find_project_config();
    project = config::load_full_project_config();
  } catch (std::exception const &e) {
    add_status_diagnostic(report, cppx::terminal::DiagnosticSeverity::error,
                          "config", e.what());
  }

  if (!report.project_config) {
    add_status_diagnostic(
        report, cppx::terminal::DiagnosticSeverity::warning, "config",
        "project config .intron.toml was not found",
        {"run 'intron use' from a project after setting defaults"});
  }

  collect_effective_toolchain(report, defaults, project);

  auto home = std::optional<std::filesystem::path>{};
  try {
    home = resolved_home_dir(ports);
  } catch (std::exception const &e) {
    add_status_diagnostic(report, cppx::terminal::DiagnosticSeverity::error,
                          "environment", e.what());
  }
  auto intron_home =
      home ? std::optional<std::filesystem::path>{installer::intron_home_path(
                 *home)}
           : std::nullopt;

  auto msvc_env = resolved_msvc_environment(ports);
  collect_tool_statuses(report, ports, intron_home, msvc_env);
  collect_environment_status(report, ports);
  collect_network_status(report, ports);
  collect_msvc_status(report, ports, msvc_env);

  return report;
}

auto json_escape(std::string_view text) -> std::string {
  auto out = std::string{};
  out.reserve(text.size() + 2);
  for (auto ch : text) {
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        out += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return out;
}

auto json_string(std::string_view text) -> std::string {
  return std::format("\"{}\"", json_escape(text));
}

auto json_bool(bool value) -> std::string_view {
  return value ? "true" : "false";
}

auto append_json_string_map(std::string &out,
                            std::map<std::string, std::string> const &values)
    -> void {
  out.push_back('{');
  auto first = true;
  for (auto const &[key, value] : values) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out += std::format("{}:{}", json_string(key), json_string(value));
  }
  out.push_back('}');
}

auto append_json_string_array(std::string &out,
                              std::vector<std::string> const &values) -> void {
  out.push_back('[');
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += json_string(values[i]);
  }
  out.push_back(']');
}

auto render_status_json(StatusReport const &report) -> std::string {
  auto out = std::string{};
  out += "{";
  out += std::format("\"version\":{},", json_string(report.version));
  out += std::format("\"platform\":{{\"name\":{},\"triple\":{}}},",
                     json_string(report.platform), json_string(report.triple));
  out += "\"project_config\":{";
  out += std::format("\"found\":{},",
                     json_bool(report.project_config.has_value()));
  out += "\"path\":";
  out += report.project_config ? json_string(report.project_config->string())
                               : "null";
  out += "},";

  out += "\"effective_toolchain\":{";
  for (std::size_t i = 0; i < report.effective_toolchain.size(); ++i) {
    auto const &entry = report.effective_toolchain[i];
    if (i != 0) {
      out.push_back(',');
    }
    out += std::format("{}:{{\"version\":{},\"source\":{}}}",
                       json_string(entry.tool), json_string(entry.version),
                       json_string(entry.source));
  }
  out += "},";

  out += "\"tools\":{";
  for (std::size_t i = 0; i < report.tools.size(); ++i) {
    auto const &tool = report.tools[i];
    if (i != 0) {
      out.push_back(',');
    }
    out += std::format("{}:{{\"version\":{},\"installed\":{},\"system\":{},"
                       "\"path\":{},\"binaries\":",
                       json_string(tool.tool), json_string(tool.version),
                       json_bool(tool.installed), json_bool(tool.system),
                       tool.path.empty() ? std::string{"null"}
                                         : json_string(tool.path));
    append_json_string_map(out, tool.binaries);
    out += "}";
  }
  out += "},";

  out += "\"environment\":{";
  out += std::format("\"available\":{},\"error\":{},\"assignments\":",
                     json_bool(report.environment.available),
                     report.environment.error.empty()
                         ? std::string{"null"}
                         : json_string(report.environment.error));
  append_json_string_map(out, report.environment.assignments);
  out += ",\"path_entries\":";
  append_json_string_array(out, report.environment.path_entries);
  out += "},";

  out += std::format("\"network\":{{\"env\":{},\"selected_backend\":{}}},",
                     report.network.env_value.empty()
                         ? std::string{"null"}
                         : json_string(report.network.env_value),
                     json_string(report.network.selected_backend));

  out += "\"msvc\":{";
  out += std::format(
      "\"configured\":{},\"available\":{},\"status\":{},\"bin_dir\":{},\"cl\":{},"
      "\"windows_sdk_bin_dirs\":",
      json_bool(report.msvc.configured), json_bool(report.msvc.available),
      json_string(report.msvc.status),
      report.msvc.bin_dir.empty() ? std::string{"null"}
                                  : json_string(report.msvc.bin_dir),
      report.msvc.cl.empty() ? std::string{"null"}
                             : json_string(report.msvc.cl));
  append_json_string_array(out, report.msvc.windows_sdk_bin_dirs);
  out += "},";

  out += "\"diagnostics\":[";
  for (std::size_t i = 0; i < report.diagnostics.size(); ++i) {
    auto const &diagnostic = report.diagnostics[i];
    if (i != 0) {
      out.push_back(',');
    }
    out += std::format(
        "{{\"severity\":{},\"context\":{},\"message\":{},\"hints\":",
        json_string(diagnostic_severity_string(diagnostic.severity)),
        json_string(diagnostic.context), json_string(diagnostic.message));
    append_json_string_array(out, diagnostic.hints);
    out += "}";
  }
  out += "]}";
  return out;
}

auto find_tool_binary(StatusReport const &report, std::string_view tool,
                      std::string_view binary) -> std::optional<std::string> {
  for (auto const &status : report.tools) {
    if (status.tool != tool) {
      continue;
    }
    auto it = status.binaries.find(std::string{binary});
    if (it != status.binaries.end()) {
      return it->second;
    }
  }
  return std::nullopt;
}

auto render_status_human(StatusReport const &report)
    -> std::vector<std::string> {
  auto color = stdout_color_enabled();
  auto lines = std::vector<std::string>{};
  lines.push_back(intron::section_line("intron", color));
  lines.push_back(intron::key_value_line("version", report.version));
  lines.push_back(intron::key_value_line("platform", report.platform));
  lines.push_back(intron::key_value_line("triple", report.triple));
  lines.push_back("");

  lines.push_back(intron::section_line("project", color));
  lines.push_back(intron::key_value_line(
      "config",
      report.project_config ? report.project_config->string() : "(not found)"));
  lines.push_back("");

  lines.push_back(intron::section_line("effective toolchain", color));
  if (report.effective_toolchain.empty()) {
    lines.push_back(intron::status_line(cppx::terminal::StatusKind::fail,
                                        "no entries", color));
  } else {
    for (auto const &entry : report.effective_toolchain) {
      lines.push_back(intron::key_value_line(
          entry.tool, std::format("{} ({})", entry.version, entry.source)));
    }
  }
  lines.push_back("");

  lines.push_back(intron::section_line("tools", color));
  if (report.tools.empty()) {
    lines.push_back(intron::status_line(cppx::terminal::StatusKind::skip,
                                        "no configured tools", color));
  } else {
    for (auto const &tool : report.tools) {
      lines.push_back(intron::status_line(
          tool.installed ? cppx::terminal::StatusKind::ok
                         : cppx::terminal::StatusKind::fail,
          std::format("{} {} {}", tool.tool, tool.version,
                      tool.installed ? "installed" : "missing"),
          color));
      for (auto const &[name, path] : tool.binaries) {
        lines.push_back(intron::key_value_line(name, path));
      }
    }
  }
  lines.push_back("");

  lines.push_back(intron::section_line("resolved binaries", color));
  auto compiler = std::optional<std::string>{};
  if (auto cxx = report.environment.assignments.find("CXX");
      cxx != report.environment.assignments.end()) {
    compiler = cxx->second;
  } else if (auto cc = report.environment.assignments.find("CC");
             cc != report.environment.assignments.end()) {
    compiler = cc->second;
  } else if (auto cl = find_tool_binary(report, "msvc", "cl")) {
    compiler = cl;
  }
  lines.push_back(
      intron::key_value_line("compiler", compiler.value_or("(missing)")));
  lines.push_back(intron::key_value_line(
      "cmake",
      find_tool_binary(report, "cmake", "cmake").value_or("(missing)")));
  lines.push_back(intron::key_value_line(
      "ninja",
      find_tool_binary(report, "ninja", "ninja").value_or("(missing)")));
  lines.push_back("");

  lines.push_back(intron::section_line("environment", color));
  if (report.environment.available) {
    lines.push_back(intron::status_line(cppx::terminal::StatusKind::ok,
                                        "environment plan resolved", color));
    lines.push_back(intron::key_value_line(
        "PATH",
        report.environment.path_entries.empty()
            ? "(unchanged)"
            : std::format(
                  "{} entr{}", report.environment.path_entries.size(),
                  report.environment.path_entries.size() == 1 ? "y" : "ies")));
  } else {
    lines.push_back(intron::status_line(cppx::terminal::StatusKind::fail,
                                        report.environment.error, color));
  }
  lines.push_back("");

  lines.push_back(intron::section_line("network", color));
  lines.push_back(
      intron::key_value_line("backend", report.network.selected_backend));
  lines.push_back(intron::key_value_line(
      "env",
      report.network.env_value.empty()
          ? "INTRON_NET_BACKEND unset"
          : std::format("INTRON_NET_BACKEND={}", report.network.env_value)));
  lines.push_back("");

  lines.push_back(intron::section_line("msvc", color));
  lines.push_back(intron::key_value_line("status", report.msvc.status));
  if (!report.msvc.cl.empty()) {
    lines.push_back(intron::key_value_line("cl", report.msvc.cl));
  }
  if (!report.msvc.windows_sdk_bin_dirs.empty()) {
    lines.push_back(intron::key_value_line(
        "sdk bins",
        std::format("{} entr{}", report.msvc.windows_sdk_bin_dirs.size(),
                    report.msvc.windows_sdk_bin_dirs.size() == 1 ? "y"
                                                                 : "ies")));
  }
  lines.push_back("");

  lines.push_back(intron::section_line("diagnostics", color));
  if (report.diagnostics.empty()) {
    lines.push_back(intron::status_line(cppx::terminal::StatusKind::ok,
                                        "no issues detected", color));
  } else {
    for (auto const &diagnostic : report.diagnostics) {
      lines.push_back(cppx::terminal::format_diagnostic(
          cppx::terminal::DiagnosticMessage{
              .severity = diagnostic.severity,
              .message = diagnostic.message,
              .context = diagnostic.context,
              .hints = diagnostic.hints,
          },
          color));
    }
  }
  return lines;
}

auto cmd_status(intron::CommandRequest const &request,
                intron::RuntimePorts const &ports) -> intron::CommandResult {
  auto mode = parse_status_output_mode(request.args, request.raw_command);
  if (!mode) {
    auto result = intron::CommandResult{.exit_code = 2};
    auto color = stderr_color_enabled();
    result.add_stderr(intron::error_line(mode.error(), color));
    result.add_stderr(intron::hint_line(
        std::format("run 'intron {} --output human' or '--output json'",
                    request.raw_command),
        color));
    return result;
  }

  auto report = build_status_report(ports);
  auto result = intron::CommandResult{};
  if (*mode == StatusOutputMode::Json) {
    result.add_stdout(render_status_json(report));
    return result;
  }

  for (auto const &line : render_status_human(report)) {
    result.add_stdout(line);
  }
  return result;
}

auto usage_result(int exit_code) -> intron::CommandResult {
    auto result = intron::CommandResult{
        .exit_code = exit_code,
    };
    auto color = stdout_color_enabled();
    for (auto const& line : intron::usage_lines(intron_version, color)) {
        result.add_stdout(line);
    }
    return result;
}

auto unknown_command_result(std::string_view command) -> intron::CommandResult {
    auto result = intron::CommandResult{
        .exit_code = 1,
    };
    result.add_stderr(intron::error_line(
        std::format("unknown command '{}'", command),
        stderr_color_enabled()));
    auto color = stdout_color_enabled();
    for (auto const& line : intron::usage_lines(intron_version, color)) {
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
    if (command == "status") return intron::CommandKind::Status;
    if (command == "doctor") return intron::CommandKind::Doctor;
    if (command == "env") return intron::CommandKind::Env;
    if (command == "exec") return intron::CommandKind::Exec;
    if (command == "self-update") return intron::CommandKind::SelfUpdate;
    if (command == "help" || command == "--help" || command == "-h") {
        return intron::CommandKind::Help;
    }
    return std::nullopt;
}

auto add_config_write_notice(intron::CommandResult& result) -> void {
    result.add_stdout(intron::status_line(
        cppx::terminal::StatusKind::ok,
        "wrote .intron.toml",
        stdout_color_enabled()));
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
            auto color = stderr_color_enabled();
            result.add_stderr(intron::error_line(
                "usage: intron install <tool> <version>",
                color));
            result.add_stderr(intron::hint_line(
                "run 'intron install' inside a project with .intron.toml",
                color));
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
        result.add_stderr(intron::error_line(
            "usage: intron remove <tool> <version>",
            stderr_color_enabled()));
        return result;
    }
    result.exit_code = installer::remove(request.args[0], request.args[1]) ? 0 : 1;
    return result;
}

auto cmd_list() -> intron::CommandResult {
    auto result = intron::CommandResult{};
    auto installed = installer::list_installed();
    if (installed.empty()) {
        result.add_stdout(intron::status_line(
            cppx::terminal::StatusKind::skip,
            "no toolchains installed",
            stdout_color_enabled()));
        return result;
    }

    auto color = stdout_color_enabled();
    auto defaults = config::load_effective_defaults();
    result.add_stdout(intron::section_line("installed toolchains", color));
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
            result.add_stdout(intron::key_value_line(
                tool,
                std::format("{} (default)", display_version)));
        } else {
            result.add_stdout(intron::key_value_line(tool, display_version));
        }
    }
    return result;
}

auto cmd_which(intron::CommandRequest const& request) -> intron::CommandResult {
    auto result = intron::CommandResult{};
    if (request.args.size() != 1) {
        result.exit_code = 1;
        result.add_stderr(intron::error_line(
            "usage: intron which <binary>",
            stderr_color_enabled()));
        return result;
    }

    auto binary = std::string_view{request.args[0]};
    auto tool = intron::tool_for_binary(binary);
    if (!tool) {
        result.exit_code = 1;
        result.add_stderr(intron::error_line(
            std::format("unknown binary '{}'", binary),
            stderr_color_enabled()));
        return result;
    }

    auto version = config::get_default(*tool);
    if (!version) {
        result.exit_code = 1;
        auto color = stderr_color_enabled();
        result.add_stderr(intron::error_line(
            std::format("no default version set for {}", *tool),
            color));
        result.add_stderr(intron::hint_line(
            std::format("run 'intron default {} <version>'", *tool),
            color));
        return result;
    }

    auto path = installer::which(binary, *tool, *version);
    if (!path) {
        result.exit_code = 1;
        result.add_stderr(intron::error_line(std::format(
            "'{}' not found in {} {}",
            binary,
            *tool,
            *version), stderr_color_enabled()));
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
        result.add_stderr(intron::error_line(
            "usage: intron default <tool> <version> [--platform <name>]",
            stderr_color_enabled()));
        return result;
    }

    auto tool = std::string_view{parsed->positional[0]};
    auto version = registry::normalize_requested_version(tool, parsed->positional[1]);
    if (!registry::is_system_tool(tool)) {
        auto home = resolved_intron_home(ports);
        auto path = installer::toolchain_path(home, tool, version);
        if (!exists_with_ports(ports, path)) {
            result.exit_code = 1;
            auto color = stderr_color_enabled();
            result.add_stderr(intron::error_line(
                std::format("{} {} is not installed", tool, version),
                color));
            result.add_stderr(intron::hint_line(
                std::format("run 'intron install {} {}'", tool, version),
                color));
            return result;
        }
    }

    config::set_default(tool, version, parsed->platform.value_or(""));
    auto color = stdout_color_enabled();
    if (parsed->platform) {
        result.add_stdout(intron::status_line(cppx::terminal::StatusKind::ok, std::format(
            "Set {} default to {} (platform: {})",
            tool,
            version,
            *parsed->platform), color));
    } else {
        result.add_stdout(intron::status_line(
            cppx::terminal::StatusKind::ok,
            std::format("Set {} default to {}", tool, version),
            color));
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
            auto color = stderr_color_enabled();
            result.add_stderr(intron::error_line("no default versions set", color));
            result.add_stderr(intron::hint_line(
                "run 'intron default <tool> <version>' first",
                color));
            return result;
        }
        auto color = stdout_color_enabled();
        for (auto const& [tool, version] : common) {
            result.add_stdout(intron::status_line(
                cppx::terminal::StatusKind::ok,
                std::format("set {} {}", tool, version),
                color));
        }
        auto current_platform = std::string{registry::platform_name()};
        for (auto const& [tool, version] : current_platform_values) {
            result.add_stdout(intron::status_line(cppx::terminal::StatusKind::ok, std::format(
                "set {} {} (platform: {})",
                tool,
                version,
                current_platform), color));
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
                    result.add_stdout(intron::warning_line(
                        std::format("{} {} is not installed", tool, version),
                        stdout_color_enabled()));
                }
            }
        } else {
            auto def = config::get_default(tool);
            if (!def) {
                result.exit_code = 1;
                result.add_stderr(intron::error_line(
                    std::format("no default version for {}", tool),
                    stderr_color_enabled()));
                return result;
            }
            version = *def;
        }

        if (parsed->platform) {
            document.platforms[*parsed->platform][tool] = version;
            result.add_stdout(intron::status_line(cppx::terminal::StatusKind::ok, std::format(
                "set {} {} (platform: {})",
                tool,
                version,
                *parsed->platform), stdout_color_enabled()));
        } else {
            document.common[tool] = version;
            result.add_stdout(intron::status_line(
                cppx::terminal::StatusKind::ok,
                std::format("set {} {}", tool, version),
                stdout_color_enabled()));
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
        result.add_stderr(intron::error_line(
            "usage: intron update [tool]",
            stderr_color_enabled()));
        return result;
    }

    if (request.args.size() == 1 && request.args.front() == "msvc") {
        auto status = resolved_msvc_update_status(ports);
        if (!status) {
            result.exit_code = 1;
            result.add_stderr(intron::error_line(status.error(), stderr_color_enabled()));
            return result;
        }
        if (status->state == intron::MsvcUpdateState::Missing) {
            add_missing_msvc_result(result);
            return result;
        }
        result.add_stdout(intron::render_msvc_update_status(*status, stdout_color_enabled()));
        return result;
    }

    auto current = intron::build_tool_map(
        installer::list_installed(),
        config::load_effective_defaults());

    if (!request.args.empty()) {
        auto tool = request.args.front();
        if (!current.contains(tool) || registry::is_system_tool(tool)) {
            result.exit_code = 1;
            result.add_stderr(intron::error_line(
                std::format("{} is not installed", tool),
                stderr_color_enabled()));
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
        result.add_stdout(intron::section_line("latest versions", stdout_color_enabled()));
        for (auto tool : registry::supported_tools) {
            if (registry::is_system_tool(tool)) {
                continue;
            }
            if (auto latest = resolved_latest_version(ports, tool)) {
                result.add_stdout(intron::key_value_line(tool, std::format("latest {}", *latest)));
            }
        }
        return result;
    }

    auto color = stdout_color_enabled();
    bool has_update = false;
    for (auto const& [tool, version] : current) {
        if (registry::is_system_tool(tool)) {
            continue;
        }
        auto status = intron::make_update_status(tool, version, resolved_latest_version(ports, tool));
        if (status.state == intron::UpdateState::UpdateAvailable) {
            has_update = true;
        }
        result.add_stdout(intron::render_update_status(status, color));
    }

    if (has_update) {
        result.add_stdout("");
        result.add_stdout(intron::hint_line(
            "run 'intron install <tool> <version>' to update",
            color));
    }
    return result;
}

auto cmd_upgrade(intron::CommandRequest const& request,
                 intron::RuntimePorts const& ports) -> intron::CommandResult
{
    auto result = intron::CommandResult{};
    if (request.args.size() > 1) {
        result.exit_code = 1;
        result.add_stderr(intron::error_line(
            "usage: intron upgrade [tool]",
            stderr_color_enabled()));
        return result;
    }

    if (request.args.size() == 1 && request.args.front() == "msvc") {
        auto status = resolved_msvc_update_status(ports);
        if (!status) {
            result.exit_code = 1;
            result.add_stderr(intron::error_line(status.error(), stderr_color_enabled()));
            return result;
        }
        if (status->state == intron::MsvcUpdateState::Missing) {
            add_missing_msvc_result(result);
            return result;
        }
        if (status->state == intron::MsvcUpdateState::Unknown) {
            result.exit_code = 1;
            result.add_stdout(intron::render_msvc_upgrade_check(*status, stdout_color_enabled()));
            return result;
        }
        if (status->state == intron::MsvcUpdateState::UpToDate) {
            result.add_stdout(intron::render_msvc_upgrade_check(*status, stdout_color_enabled()));
            return result;
        }

        auto color = stdout_color_enabled();
        result.add_stdout(intron::render_msvc_upgrade_check(*status, color));
        auto upgraded = resolved_msvc_upgrade(ports);
        if (!upgraded) {
            result.exit_code = 1;
            result.add_stderr(intron::error_line(upgraded.error(), stderr_color_enabled()));
            return result;
        }
        if (upgraded->state != intron::MsvcUpdateState::UpToDate) {
            result.exit_code = 1;
            result.add_stderr(intron::error_line(
                "msvc upgrade did not reach the latest servicing version",
                stderr_color_enabled()));
            return result;
        }
        result.add_stdout("");
        result.add_stdout(intron::status_line(
            cppx::terminal::StatusKind::ok,
            std::format("Upgraded msvc to {}", upgraded->current_version),
            color));
        return result;
    }

    auto current = intron::build_tool_map(
        installer::list_installed(),
        config::load_effective_defaults());

    if (!request.args.empty()) {
        auto tool = request.args.front();
        if (!current.contains(tool)) {
            result.exit_code = 1;
            result.add_stderr(intron::error_line(
                std::format("{} is not installed", tool),
                stderr_color_enabled()));
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
        result.add_stdout(intron::status_line(
            cppx::terminal::StatusKind::skip,
            "no toolchains installed",
            stdout_color_enabled()));
        return result;
    }

    auto color = stdout_color_enabled();
    int upgraded = 0;
    for (auto const& [tool, version] : current) {
        if (registry::is_system_tool(tool)) {
            continue;
        }
        auto status = intron::make_update_status(tool, version, resolved_latest_version(ports, tool));
        if (status.state == intron::UpdateState::Unknown) {
            result.add_stdout(intron::render_upgrade_check(status, color));
            continue;
        }
        if (status.state == intron::UpdateState::UpToDate) {
            result.add_stdout(intron::render_upgrade_check(status, color));
            continue;
        }

        result.add_stdout(intron::render_upgrade_check(status, color));
        auto info = registry::resolve(tool, *status.latest_version);
        if (!installer::install(info)) {
            result.add_stderr(intron::error_line(
                std::format("failed to upgrade {}", tool),
                stderr_color_enabled()));
            continue;
        }
        config::set_default(tool, *status.latest_version);
        ++upgraded;
    }

    if (upgraded > 0) {
        result.add_stdout("");
        result.add_stdout(intron::status_line(
            cppx::terminal::StatusKind::ok,
            std::format(
            "Upgraded {} tool{}",
            upgraded,
            upgraded == 1 ? "" : "s"),
            color));
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
        auto color = stderr_color_enabled();
        result.add_stderr(intron::error_line("no default versions set", color));
        result.add_stderr(intron::hint_line("run 'intron default <tool> <version>'", color));
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
            auto color = stderr_color_enabled();
            result.add_stderr(intron::error_line(
                "msvc is configured as a default toolchain but was not detected",
                color));
            result.add_stderr(intron::hint_line("run 'intron install msvc 2022'", color));
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

auto cmd_env(intron::CommandRequest const& request,
             intron::RuntimePorts const& ports) -> intron::CommandResult
{
    auto mode = intron::parse_env_flags(request.args);
    if (!mode) {
        auto result = intron::CommandResult{
            .exit_code = 2,
        };
        result.add_stderr(intron::error_line(mode.error(), stderr_color_enabled()));
        auto color = stdout_color_enabled();
        for (auto const& line : intron::usage_lines(intron_version, color)) {
            result.add_stdout(line);
        }
        return result;
    }

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
    for (auto const& line :
         intron::render_env_lines(*resolved, is_windows, *mode)) {
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
        result.add_stderr(intron::error_line(
            "process runner is not configured",
            stderr_color_enabled()));
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
        result.add_stderr(intron::error_line(run_result.error(), stderr_color_enabled()));
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
    case intron::CommandKind::Status:
    case intron::CommandKind::Doctor:
        return cmd_status(request, ports);
    case intron::CommandKind::Env:
        return cmd_env(request, ports);
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
