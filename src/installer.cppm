export module installer;
import std;
import cppx.archive;
import cppx.archive.system;
import cppx.checksum;
import cppx.checksum.system;
import cppx.env.system;
import cppx.fs;
import cppx.fs.system;
import cppx.process;
import cppx.process.system;
import cppx.terminal;
import cppx.terminal.system;
import intron.domain;
import intron.output;
import net;
export import registry;

export namespace installer {

struct VisualStudioInstance {
    std::filesystem::path installation_path;
    std::string product_id;
    std::string channel_id;
    std::string channel_uri;
    std::string installed_channel_uri;
    std::string installation_version;
    std::string product_display_version;
    std::optional<std::filesystem::path> toolset_root;
    std::optional<std::filesystem::path> vcvars64_path;
    std::optional<std::filesystem::path> cl_path;

    auto has_msvc() const -> bool {
        return toolset_root.has_value() && vcvars64_path.has_value() && cl_path.has_value();
    }

    auto is_build_tools() const -> bool {
        return product_id == "Microsoft.VisualStudio.Product.BuildTools";
    }
};

struct VisualStudioChannelVersion {
    std::string installation_version;
    std::string display_version;
};

struct VisualStudioCommand {
    std::filesystem::path program;
    std::vector<std::string> args;
};

enum class VisualStudioInstallerExitKind {
    Success,
    SuccessRebootRequired,
    ElevationRequired,
    Failure,
};

struct VisualStudioInstallerExit {
    int exit_code = 1;
    VisualStudioInstallerExitKind kind = VisualStudioInstallerExitKind::Failure;
    std::string message;

    auto succeeded() const -> bool {
        return kind == VisualStudioInstallerExitKind::Success ||
               kind == VisualStudioInstallerExitKind::SuccessRebootRequired;
    }
};

auto classify_msvc_installer_exit(int exit_code) -> VisualStudioInstallerExit;
auto parse_visual_studio_channel_version(std::string_view json, std::string_view product_id)
    -> std::optional<VisualStudioChannelVersion>;

struct MsvcEnvironment {
    std::filesystem::path installation_path;
    std::filesystem::path tool_root;
    std::filesystem::path bin_dir;
    std::filesystem::path cl;
    std::filesystem::path asan_runtime;
    std::map<std::string, std::string> variables;
};

auto build_msvc_update_command(VisualStudioInstance const& instance,
                               std::filesystem::path const& setup_path)
    -> VisualStudioCommand;
auto msvc_update_status() -> std::expected<intron::MsvcUpdateStatus, std::string>;
auto upgrade_msvc() -> std::expected<intron::MsvcUpdateStatus, std::string>;
auto discover_windows_sdk_bin_dirs(std::optional<std::string> pinned_version)
    -> std::vector<std::filesystem::path>;

std::filesystem::path intron_home_path(std::filesystem::path const& home_dir) {
    return home_dir / ".intron";
}

std::filesystem::path intron_home() {
    auto home = cppx::env::system::home_dir();
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }
    auto path = intron_home_path(*home);
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path toolchain_path(
    std::filesystem::path const& intron_home,
    std::string_view tool,
    std::string_view version)
{
    return intron_home / "toolchains" / tool / version;
}

std::filesystem::path toolchain_path(std::string_view tool, std::string_view version) {
    return toolchain_path(intron_home(), tool, version);
}

namespace detail {

constexpr bool is_windows =
#ifdef _WIN32
    true;
#else
    false;
#endif

struct VisualStudioCommandResult {
    VisualStudioInstallerExit status;
    std::string stdout_text;
    std::string stderr_text;
};

auto user_agent() -> std::string {
    return std::format("intron/{}", EXON_PKG_VERSION);
}

auto stdout_color_enabled() -> bool {
    return cppx::terminal::system::stdout_color_enabled(intron::terminal_options());
}

auto stderr_color_enabled() -> bool {
    return cppx::terminal::system::stderr_color_enabled(intron::terminal_options());
}

auto print_status(cppx::terminal::StatusKind status,
                  std::string_view message) -> void
{
    std::println("{}", intron::status_line(status, message, stdout_color_enabled()));
}

auto print_stage(std::string_view name,
                 int index,
                 int total,
                 std::string_view context) -> void
{
    std::println("{}", intron::stage_line(
        name,
        index,
        total,
        context,
        stdout_color_enabled()));
}

auto print_warning(std::string_view message) -> void {
    std::println("{}", intron::warning_line(message, stdout_color_enabled()));
}

auto print_error(std::string_view message) -> void {
    std::println(std::cerr, "{}", intron::error_line(message, stderr_color_enabled()));
}

auto print_hint(std::string_view message) -> void {
    std::println(std::cerr, "{}", intron::hint_line(message, stderr_color_enabled()));
}

auto trim_line_endings(std::string text) -> std::string {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

auto capture_stdout(cppx::process::ProcessSpec spec) -> std::string {
    auto result = cppx::process::system::capture(std::move(spec));
    if (!result || result->timed_out || result->exit_code != 0) {
        return {};
    }
    return trim_line_endings(std::move(result->stdout_text));
}

auto write_text_file(std::filesystem::path const& path, std::string content) -> void {
    auto written = cppx::fs::system::write_if_changed({
        .path = path,
        .content = std::move(content),
    });
    if (!written) {
        throw std::runtime_error(std::format(
            "cannot write file: {} ({})",
            path.string(),
            cppx::fs::to_string(written.error())));
    }
}

auto try_write_text_file(std::filesystem::path const& path, std::string content) -> bool {
    auto written = cppx::fs::system::write_if_changed({
        .path = path,
        .content = std::move(content),
    });
    return static_cast<bool>(written);
}

auto command_processor_path() -> std::filesystem::path {
    if constexpr (!is_windows) {
        return {};
    }
    if (auto const* comspec = std::getenv("ComSpec"); comspec && *comspec) {
        auto path = std::filesystem::path{comspec};
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return "cmd.exe";
}

auto temp_batch_script_path(std::string_view stem) -> std::optional<std::filesystem::path> {
    if constexpr (!is_windows) {
        return std::nullopt;
    }

    std::error_code ec;
    auto temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::nullopt;
    }
    auto unique_id = std::format(
        "{}-{}",
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return temp_dir / std::format("{}-{}.cmd", stem, unique_id);
}

auto write_capture_msvc_environment_script(std::filesystem::path const& script_path,
                                           std::filesystem::path const& vcvars_path) -> bool
{
    auto content = std::format(
        "@echo off\r\n"
        "call \"{}\" >nul\r\n"
        "if errorlevel 1 exit /b %errorlevel%\r\n"
        "set\r\n",
        vcvars_path.string());
    return try_write_text_file(script_path, std::move(content));
}

auto strip_depth(std::string_view prefix) -> int {
    if (prefix.empty()) {
        return 0;
    }
    auto depth = 1;
    for (auto c : prefix) {
        if (c == '/') {
            ++depth;
        }
    }
    return depth;
}

auto extract_archive(std::filesystem::path const& archive,
                     std::filesystem::path const& staging,
                     registry::ToolInfo const& info)
    -> std::expected<void, std::string>
{
    auto format = cppx::archive::archive_format_from_string(info.archive_type);
    if (!format) {
        return std::unexpected(std::format("unknown archive type: {}", info.archive_type));
    }

    auto extracted = cppx::archive::system::extract({
        .archive_path = archive,
        .destination_dir = staging,
        .format = *format,
        .strip_components = strip_depth(info.strip_prefix),
    });
    if (!extracted) {
        return std::unexpected(extracted.error().message);
    }
    return {};
}

auto verify_checksum(std::filesystem::path const& archive,
                     std::string_view archive_name,
                     std::string const& checksum_url) -> bool
{
    auto manifest = net::get_text(checksum_url, net::user_agent_headers(user_agent()));
    if (!manifest) {
        print_warning(std::format(
            "could not download checksum file ({}), skipping verification",
            manifest.error()));
        return true;
    }

    auto expected_hash = cppx::checksum::find_sha256_for_filename(*manifest, archive_name);
    if (!expected_hash) {
        print_warning("checksum entry not found, skipping verification");
        return true;
    }

    auto actual_hash = cppx::checksum::system::sha256_file(archive);
    if (!actual_hash) {
        print_error(std::format(
            "could not calculate checksum: {}",
            actual_hash.error().message));
        return false;
    }

    if (*actual_hash != *expected_hash) {
        print_error(std::format(
            "checksum mismatch\n  expected: {}\n  actual:   {}",
            *expected_hash,
            *actual_hash));
        return false;
    }

    print_status(cppx::terminal::StatusKind::ok, "checksum OK");
    return true;
}

auto write_clang_wrapper(std::filesystem::path const& bin_dir, std::string const& cfg_flag)
    -> void
{
    for (auto name : {"clang", "clang++"}) {
        auto orig = bin_dir / name;
        auto backup = bin_dir / std::format("{}.orig", name);
        if (std::filesystem::exists(orig) && !std::filesystem::exists(backup)) {
            std::filesystem::rename(orig, backup);
            write_text_file(
                orig,
                std::format(
                    "#!/bin/sh\nexec \"{}\" {} \"$@\"\n",
                    backup.string(),
                    cfg_flag));
            std::filesystem::permissions(
                orig,
                std::filesystem::perms::owner_exec |
                    std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_exec,
                std::filesystem::perm_options::add);
        }
    }
}

auto setup_llvm_config(std::filesystem::path const& dest, std::string_view /*version*/) -> void {
    auto plat = registry::detect_platform();
    auto clang = dest / "bin" / "clang";
    auto target = capture_stdout({
        .program = clang.string(),
        .args = {"-dumpmachine"},
    });

    if (plat.os == registry::OS::macOS) {
        auto sdk_path = capture_stdout({
            .program = "xcrun",
            .args = {"--show-sdk-path"},
        });
        if (!target.empty() && !sdk_path.empty()) {
            auto cfg_target = target;
            if (auto darwin_pos = cfg_target.find("darwin");
                darwin_pos != std::string::npos) {
                auto ver_start = darwin_pos + 6;
                if (auto dot = cfg_target.find('.', ver_start); dot != std::string::npos) {
                    cfg_target = cfg_target.substr(0, dot);
                }
            }

            auto cfg_dir = dest / "etc" / "clang";
            std::filesystem::create_directories(cfg_dir);
            auto cfg_file = cfg_dir / std::format("{}.cfg", cfg_target);
            write_text_file(
                cfg_file,
                std::format(
                    "-isysroot {}\n"
                    "-stdlib=libc++\n"
                    "-lc++\n",
                    sdk_path));

            write_clang_wrapper(
                dest / "bin",
                std::format("--config-system-dir={}", cfg_dir.string()));
            print_status(cppx::terminal::StatusKind::ok,
                         std::format("generated clang config: {}", cfg_file.string()));
        }
    } else if (plat.os == registry::OS::Linux && !target.empty()) {
        auto lib_dir = dest / "lib" / target;
        if (!std::filesystem::exists(lib_dir / "libc++.so") &&
            !std::filesystem::exists(lib_dir / "libc++.a")) {
            lib_dir = dest / "lib";
        }

        auto cfg_dir = dest / "etc" / "clang";
        std::filesystem::create_directories(cfg_dir);
        auto cfg_file = cfg_dir / std::format("{}.cfg", target);
        write_text_file(
            cfg_file,
            std::format(
                "-stdlib=libc++\n"
                "-lc++\n"
                "-lc++abi\n"
                "-L{}\n"
                "-Wl,-rpath,{}\n",
                lib_dir.string(),
                lib_dir.string()));

        write_clang_wrapper(
            dest / "bin",
            std::format("--config-system-dir={}", cfg_dir.string()));
        print_status(cppx::terminal::StatusKind::ok,
                     std::format("generated clang config: {}", cfg_file.string()));
    }
}

auto verify_installed_binary(std::filesystem::path const& dest,
                             registry::ToolInfo const& info) -> bool
{
    std::filesystem::path verify_bin;
    if (info.name == "llvm") {
        verify_bin = dest / "bin" / "clang++";
    } else if (info.name == "ninja") {
        verify_bin = dest / "ninja";
    } else if (info.name == "wasi-sdk") {
        verify_bin = dest / "bin" / "clang++";
    } else {
        verify_bin = dest / "bin" / info.name;
    }

    if (!std::filesystem::exists(verify_bin)) {
        return true;
    }

    auto result = cppx::process::system::capture({
        .program = verify_bin.string(),
        .args = {"--version"},
    });
    if (!result || result->timed_out || result->exit_code != 0) {
        print_error(std::format("{} binary is not executable on this platform",
                                info.name));
        print_hint("the downloaded binary may not match your architecture");
        return false;
    }
    return true;
}

auto compare_dotted_versions(std::string_view lhs, std::string_view rhs) -> int {
    auto next_part = [](std::string_view text, std::size_t& pos) -> int {
        if (pos >= text.size()) {
            return 0;
        }
        auto end = text.find('.', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto part = text.substr(pos, end - pos);
        pos = end == text.size() ? end : end + 1;
        auto value = 0;
        std::from_chars(part.data(), part.data() + part.size(), value);
        return value;
    };

    auto lhs_pos = std::size_t{0};
    auto rhs_pos = std::size_t{0};
    while (lhs_pos < lhs.size() || rhs_pos < rhs.size()) {
        auto lhs_part = next_part(lhs, lhs_pos);
        auto rhs_part = next_part(rhs, rhs_pos);
        if (lhs_part < rhs_part) {
            return -1;
        }
        if (lhs_part > rhs_part) {
            return 1;
        }
    }
    return 0;
}

auto instance_newer(VisualStudioInstance const& lhs, VisualStudioInstance const& rhs) -> bool {
    if (auto cmp = compare_dotted_versions(lhs.installation_version, rhs.installation_version);
        cmp != 0) {
        return cmp > 0;
    }
    if (lhs.is_build_tools() != rhs.is_build_tools()) {
        return lhs.is_build_tools();
    }
    return lhs.installation_path.string() < rhs.installation_path.string();
}

auto normalized_display_version(std::string_view version) -> std::string {
    auto end = std::size_t{0};
    while (end < version.size()) {
        auto c = version[end];
        if ((c >= '0' && c <= '9') || c == '.') {
            ++end;
            continue;
        }
        break;
    }
    if (end == 0) {
        return std::string{version};
    }
    return std::string{version.substr(0, end)};
}

auto current_display_version(VisualStudioInstance const& instance) -> std::string {
    if (!instance.product_display_version.empty()) {
        return normalized_display_version(instance.product_display_version);
    }
    return instance.installation_version;
}

auto find_latest_toolset_in_installation(std::filesystem::path const& installation_path)
    -> std::optional<std::filesystem::path>
{
    auto vc_tools = installation_path / "VC" / "Tools" / "MSVC";
    if (!std::filesystem::exists(vc_tools)) {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> best_path;
    std::string best_version;
    for (auto const& entry : std::filesystem::directory_iterator{vc_tools}) {
        if (!entry.is_directory()) {
            continue;
        }
        auto version = entry.path().filename().string();
        auto cl = entry.path() / "bin" / "Hostx64" / "x64" / "cl.exe";
        if (!std::filesystem::exists(cl)) {
            continue;
        }
        if (!best_path || compare_dotted_versions(version, best_version) > 0) {
            best_version = version;
            best_path = entry.path();
        }
    }
    return best_path;
}

auto make_visual_studio_instance(std::filesystem::path installation_path,
                                 std::string product_id,
                                 std::string channel_id,
                                 std::string channel_uri,
                                 std::string installed_channel_uri,
                                 std::string installation_version,
                                 std::string product_display_version)
    -> VisualStudioInstance
{
    auto instance = VisualStudioInstance{
        .installation_path = std::move(installation_path),
        .product_id = std::move(product_id),
        .channel_id = std::move(channel_id),
        .channel_uri = std::move(channel_uri),
        .installed_channel_uri = std::move(installed_channel_uri),
        .installation_version = std::move(installation_version),
        .product_display_version = std::move(product_display_version),
    };

    if (auto toolset = find_latest_toolset_in_installation(instance.installation_path)) {
        auto vcvars = instance.installation_path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
        auto cl = *toolset / "bin" / "Hostx64" / "x64" / "cl.exe";
        if (std::filesystem::exists(vcvars) && std::filesystem::exists(cl)) {
            instance.toolset_root = *toolset;
            instance.vcvars64_path = vcvars;
            instance.cl_path = cl;
        }
    }

    return instance;
}

auto scan_visual_studio_install_roots() -> std::vector<std::filesystem::path> {
    auto roots = std::vector<std::filesystem::path>{};
    for (auto var : {"ProgramFiles", "ProgramFiles(x86)"}) {
        if (auto value = std::getenv(var); value && *value) {
            roots.push_back(std::filesystem::path{value} / "Microsoft Visual Studio");
        }
    }
    roots.push_back("C:/Program Files/Microsoft Visual Studio");
    roots.push_back("C:/Program Files (x86)/Microsoft Visual Studio");
    std::ranges::sort(roots);
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

auto fallback_detect_msvc_instance() -> std::optional<VisualStudioInstance> {
    std::optional<std::filesystem::path> best_toolset;
    std::string best_toolset_version;

    for (auto const& installs_root : scan_visual_studio_install_roots()) {
        if (!std::filesystem::exists(installs_root)) {
            continue;
        }
        for (auto const& year_entry : std::filesystem::directory_iterator{installs_root}) {
            if (!year_entry.is_directory()) {
                continue;
            }
            for (auto const& edition_entry : std::filesystem::directory_iterator{year_entry.path()}) {
                if (!edition_entry.is_directory()) {
                    continue;
                }
                if (auto toolset = find_latest_toolset_in_installation(edition_entry.path())) {
                    auto version = toolset->filename().string();
                    if (!best_toolset ||
                        compare_dotted_versions(version, best_toolset_version) > 0) {
                        best_toolset = *toolset;
                        best_toolset_version = version;
                    }
                }
            }
        }
    }

    if (!best_toolset) {
        return std::nullopt;
    }

    auto installation_path = best_toolset->parent_path().parent_path().parent_path().parent_path();
    auto instance = make_visual_studio_instance(
        installation_path,
        {},
        {},
        {},
        {},
        "17.0",
        {});
    if (!instance.has_msvc()) {
        return std::nullopt;
    }
    return instance;
}

auto find_vswhere_path() -> std::optional<std::filesystem::path> {
    if constexpr (!is_windows) {
        return std::nullopt;
    }

    if (auto const* pf86 = std::getenv("ProgramFiles(x86)"); pf86 && *pf86) {
        auto path = std::filesystem::path{pf86}
            / "Microsoft Visual Studio"
            / "Installer"
            / "vswhere.exe";
        if (std::filesystem::exists(path)) {
            return path;
        }
    }

    if (auto found = cppx::env::system::find_in_path("vswhere")) {
        return *found;
    }
    return std::nullopt;
}

auto find_visual_studio_setup_path(std::optional<std::filesystem::path> const& vswhere_path)
    -> std::optional<std::filesystem::path>
{
    if constexpr (!is_windows) {
        return std::nullopt;
    }

    if (vswhere_path) {
        auto setup = vswhere_path->parent_path() / "setup.exe";
        if (std::filesystem::exists(setup)) {
            return setup;
        }
    }

    if (auto const* pf86 = std::getenv("ProgramFiles(x86)"); pf86 && *pf86) {
        auto setup = std::filesystem::path{pf86}
            / "Microsoft Visual Studio"
            / "Installer"
            / "setup.exe";
        if (std::filesystem::exists(setup)) {
            return setup;
        }
    }

    return std::nullopt;
}

auto parse_json_string(std::string_view text, std::size_t& pos) -> std::optional<std::string> {
    if (pos >= text.size() || text[pos] != '"') {
        return std::nullopt;
    }

    ++pos;
    auto out = std::string{};
    while (pos < text.size()) {
        auto c = text[pos++];
        if (c == '"') {
            return out;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }

        if (pos >= text.size()) {
            return std::nullopt;
        }

        auto escaped = text[pos++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            out.push_back(escaped);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u':
            if (pos + 4 <= text.size()) {
                pos += 4;
            } else {
                return std::nullopt;
            }
            out.push_back('?');
            break;
        default:
            out.push_back(escaped);
            break;
        }
    }

    return std::nullopt;
}

auto split_top_level_json_objects(std::string_view json) -> std::vector<std::string_view> {
    auto objects = std::vector<std::string_view>{};
    auto depth = 0;
    auto in_string = false;
    auto escape = false;
    auto object_start = std::size_t{0};

    for (auto i = std::size_t{0}; i < json.size(); ++i) {
        auto c = json[i];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') {
            if (depth == 0) {
                object_start = i;
            }
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 0) {
                objects.push_back(json.substr(object_start, i - object_start + 1));
            }
        }
    }

    return objects;
}

auto extract_json_string_field(std::string_view object, std::string_view key)
    -> std::optional<std::string>
{
    auto needle = std::format("\"{}\"", key);
    auto key_pos = object.find(needle);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }

    auto colon = object.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }

    auto pos = colon + 1;
    while (pos < object.size() &&
           std::isspace(static_cast<unsigned char>(object[pos])) != 0) {
        ++pos;
    }

    return parse_json_string(object, pos);
}

auto extract_json_array_field(std::string_view object, std::string_view key)
    -> std::optional<std::string_view>
{
    auto needle = std::format("\"{}\"", key);
    auto key_pos = object.find(needle);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }

    auto colon = object.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }

    auto pos = colon + 1;
    while (pos < object.size() &&
           std::isspace(static_cast<unsigned char>(object[pos])) != 0) {
        ++pos;
    }
    if (pos >= object.size() || object[pos] != '[') {
        return std::nullopt;
    }

    auto in_string = false;
    auto escape = false;
    auto depth = 1;
    auto start = pos + 1;
    for (auto i = start; i < object.size(); ++i) {
        auto c = object[i];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '[') {
            ++depth;
            continue;
        }
        if (c == ']') {
            --depth;
            if (depth == 0) {
                return object.substr(start, i - start);
            }
        }
    }

    return std::nullopt;
}

auto effective_channel_uri(VisualStudioInstance const& instance) -> std::string {
    if (!instance.installed_channel_uri.empty()) {
        return instance.installed_channel_uri;
    }
    return instance.channel_uri;
}

auto capture_vswhere_instances_json(std::filesystem::path const& vswhere_path) -> std::string {
    return capture_stdout({
        .program = vswhere_path.string(),
        .args = {
            "-products",
            "*",
            "-version",
            "[17.0,18.0)",
            "-format",
            "json",
            "-utf8",
        },
    });
}

auto cached_msvc_bootstrapper_path(registry::ToolInfo const& info) -> std::filesystem::path {
    auto downloads = intron_home() / "downloads";
    std::filesystem::create_directories(downloads);
    auto filename = std::string{"vs_BuildTools.exe"};
    if (info.visual_studio) {
        if (auto bootstrapper = std::filesystem::path{info.visual_studio->bootstrapper_url};
            !bootstrapper.filename().empty()) {
            filename = bootstrapper.filename().string();
        }
    }
    return downloads / filename;
}

auto visual_studio_log_hint() -> std::string {
    return "see %TEMP%\\dd_bootstrapper*.log and %TEMP%\\dd_setup*.log for details";
}

auto run_visual_studio_command(VisualStudioCommand const& command)
    -> VisualStudioCommandResult
{
    auto captured = cppx::process::system::capture({
        .program = command.program.string(),
        .args = command.args,
    });

    if (!captured) {
        return {
            .status =
                {
                    .exit_code = 1,
                    .kind = VisualStudioInstallerExitKind::Failure,
                    .message = "failed to start Visual Studio installer",
                },
        };
    }

    if (captured->timed_out) {
        return {
            .status =
                {
                    .exit_code = captured->exit_code,
                    .kind = VisualStudioInstallerExitKind::Failure,
                    .message = "Visual Studio installer timed out",
                },
            .stdout_text = std::move(captured->stdout_text),
            .stderr_text = std::move(captured->stderr_text),
        };
    }

    return {
        .status = classify_msvc_installer_exit(captured->exit_code),
        .stdout_text = std::move(captured->stdout_text),
        .stderr_text = std::move(captured->stderr_text),
    };
}

auto print_visual_studio_failure(VisualStudioCommandResult const& result) -> void {
    print_error(result.status.message);
    if (!result.stderr_text.empty()) {
        std::println(std::cerr, "stderr:");
        std::println(std::cerr, "{}", trim_line_endings(result.stderr_text));
    }
    if (!result.stdout_text.empty()) {
        std::println(std::cerr, "stdout:");
        std::println(std::cerr, "{}", trim_line_endings(result.stdout_text));
    }
    print_hint(visual_studio_log_hint());
}

} // namespace detail

auto parse_vswhere_instances(std::string_view json) -> std::vector<VisualStudioInstance> {
    auto instances = std::vector<VisualStudioInstance>{};
    for (auto object : detail::split_top_level_json_objects(json)) {
        auto installation_path = detail::extract_json_string_field(object, "installationPath");
        if (!installation_path || installation_path->empty()) {
            continue;
        }

        auto product_id = detail::extract_json_string_field(object, "productId");
        auto channel_id = detail::extract_json_string_field(object, "channelId");
        auto channel_uri = detail::extract_json_string_field(object, "channelUri");
        auto installed_channel_uri =
            detail::extract_json_string_field(object, "installedChannelUri");
        auto installation_version =
            detail::extract_json_string_field(object, "installationVersion");
        auto product_display_version =
            detail::extract_json_string_field(object, "productDisplayVersion");

        instances.push_back(detail::make_visual_studio_instance(
            std::filesystem::path{*installation_path},
            product_id.value_or(""),
            channel_id.value_or(""),
            channel_uri.value_or(""),
            installed_channel_uri.value_or(""),
            installation_version.value_or(""),
            product_display_version.value_or("")));
    }
    return instances;
}

auto parse_visual_studio_channel_version(std::string_view json, std::string_view product_id)
    -> std::optional<VisualStudioChannelVersion>
{
    auto display_version = detail::extract_json_string_field(json, "productDisplayVersion");
    auto channel_items = detail::extract_json_array_field(json, "channelItems");
    if (!channel_items) {
        return std::nullopt;
    }

    for (auto object : detail::split_top_level_json_objects(*channel_items)) {
        auto id = detail::extract_json_string_field(object, "id");
        if (!id || *id != product_id) {
            continue;
        }

        auto type = detail::extract_json_string_field(object, "type");
        if (type && *type != "ChannelProduct") {
            continue;
        }

        auto version = detail::extract_json_string_field(object, "version");
        if (!version || version->empty()) {
            continue;
        }

        return VisualStudioChannelVersion{
            .installation_version = *version,
            .display_version = detail::normalized_display_version(
                display_version.value_or(*version)),
        };
    }

    return std::nullopt;
}

auto discover_visual_studio_instances() -> std::vector<VisualStudioInstance> {
    if constexpr (!detail::is_windows) {
        return {};
    }

    auto vswhere_path = detail::find_vswhere_path();
    if (!vswhere_path) {
        return {};
    }

    auto json = detail::capture_vswhere_instances_json(*vswhere_path);
    if (json.empty()) {
        return {};
    }
    return parse_vswhere_instances(json);
}

auto select_ready_msvc_instance(std::vector<VisualStudioInstance> const& instances)
    -> std::optional<VisualStudioInstance>
{
    auto best = std::optional<VisualStudioInstance>{};

    auto consider = [&](auto const& instance) {
        if (!best || detail::instance_newer(instance, *best)) {
            best = instance;
        }
    };

    for (auto const& instance : instances) {
        if (instance.is_build_tools() && instance.has_msvc()) {
            consider(instance);
        }
    }
    if (best) {
        return best;
    }

    for (auto const& instance : instances) {
        if (instance.has_msvc()) {
            consider(instance);
        }
    }
    return best;
}

auto select_msvc_modify_target(std::vector<VisualStudioInstance> const& instances)
    -> std::optional<VisualStudioInstance>
{
    auto best = std::optional<VisualStudioInstance>{};

    auto consider = [&](auto const& instance) {
        if (!best || detail::instance_newer(instance, *best)) {
            best = instance;
        }
    };

    for (auto const& instance : instances) {
        if (instance.is_build_tools()) {
            consider(instance);
        }
    }
    if (best) {
        return best;
    }

    for (auto const& instance : instances) {
        consider(instance);
    }
    return best;
}

namespace detail {

auto fetch_visual_studio_channel_version(VisualStudioInstance const& instance)
    -> std::optional<VisualStudioChannelVersion>
{
    auto channel_uri = effective_channel_uri(instance);
    if (channel_uri.empty() || instance.product_id.empty()) {
        return std::nullopt;
    }

    auto json = net::get_text(channel_uri, net::user_agent_headers(user_agent()));
    if (!json) {
        return std::nullopt;
    }

    return parse_visual_studio_channel_version(*json, instance.product_id);
}

auto make_msvc_update_status(VisualStudioInstance const& instance,
                             std::optional<VisualStudioChannelVersion> const& latest)
    -> intron::MsvcUpdateStatus
{
    auto status = intron::MsvcUpdateStatus{
        .installation_version = instance.installation_version,
        .current_version = current_display_version(instance),
    };
    if (!latest) {
        status.state = intron::MsvcUpdateState::Unknown;
        return status;
    }

    status.latest_installation_version = latest->installation_version;
    status.latest_version = latest->display_version;
    if (status.installation_version.empty()) {
        status.state = intron::MsvcUpdateState::Unknown;
        return status;
    }

    status.state =
        compare_dotted_versions(status.installation_version, latest->installation_version) >= 0
        ? intron::MsvcUpdateState::UpToDate
        : intron::MsvcUpdateState::UpdateAvailable;
    return status;
}

auto find_instance_by_path(std::vector<VisualStudioInstance> const& instances,
                           std::filesystem::path const& installation_path)
    -> std::optional<VisualStudioInstance>
{
    auto target = installation_path.lexically_normal().generic_string();
    for (auto const& instance : instances) {
        if (instance.installation_path.lexically_normal().generic_string() == target) {
            return instance;
        }
    }
    return std::nullopt;
}

} // namespace detail

auto build_msvc_install_command(registry::ToolInfo const& info,
                                std::filesystem::path const& bootstrapper)
    -> VisualStudioCommand
{
    if (!info.visual_studio) {
        throw std::runtime_error("msvc installer metadata is missing");
    }

    return {
        .program = bootstrapper,
        .args = {
            "--productId",
            info.visual_studio->product_id,
            "--channelId",
            info.visual_studio->channel_id,
            "--installPath",
            info.visual_studio->install_path.string(),
            "--add",
            info.visual_studio->workload_id,
            "--includeRecommended",
            "--passive",
            "--wait",
            "--norestart",
        },
    };
}

auto build_msvc_update_command(VisualStudioInstance const& instance,
                               std::filesystem::path const& setup_path)
    -> VisualStudioCommand
{
    return {
        .program = setup_path,
        .args = {
            "update",
            "--installPath",
            instance.installation_path.string(),
            "--passive",
            "--wait",
            "--norestart",
        },
    };
}

auto build_msvc_modify_command(registry::ToolInfo const& info,
                               VisualStudioInstance const& instance,
                               std::filesystem::path const& setup_path)
    -> VisualStudioCommand
{
    if (!info.visual_studio) {
        throw std::runtime_error("msvc installer metadata is missing");
    }

    auto product_id = instance.product_id.empty() ? info.visual_studio->product_id
                                                  : instance.product_id;
    auto channel_id = instance.channel_id.empty() ? info.visual_studio->channel_id
                                                  : instance.channel_id;

    return {
        .program = setup_path,
        .args = {
            "modify",
            "--installPath",
            instance.installation_path.string(),
            "--productId",
            product_id,
            "--channelId",
            channel_id,
            "--add",
            info.visual_studio->workload_id,
            "--includeRecommended",
            "--passive",
            "--wait",
            "--norestart",
        },
    };
}

auto classify_msvc_installer_exit(int exit_code) -> VisualStudioInstallerExit {
    if (exit_code == 0) {
        return {
            .exit_code = exit_code,
            .kind = VisualStudioInstallerExitKind::Success,
            .message = "Visual Studio Build Tools installation succeeded",
        };
    }
    if (exit_code == 3010) {
        return {
            .exit_code = exit_code,
            .kind = VisualStudioInstallerExitKind::SuccessRebootRequired,
            .message = "Visual Studio Build Tools installation succeeded; reboot required",
        };
    }
    if (exit_code == 740) {
        return {
            .exit_code = exit_code,
            .kind = VisualStudioInstallerExitKind::ElevationRequired,
            .message =
                "Visual Studio Build Tools installation requires administrator privileges",
        };
    }
    return {
        .exit_code = exit_code,
        .kind = VisualStudioInstallerExitKind::Failure,
        .message = std::format(
            "Visual Studio installer failed with exit code {}",
            exit_code),
    };
}

auto msvc_binary_path(VisualStudioInstance const& instance, std::string_view binary)
    -> std::optional<std::filesystem::path>
{
    if (!instance.has_msvc() || !instance.cl_path) {
        return std::nullopt;
    }

    auto bin_dir = instance.cl_path->parent_path();
    auto name = std::string{binary};
    if (name == "cl" || name == "cl.exe") {
        return *instance.cl_path;
    }
    if (name == "link" || name == "link.exe") {
        auto link = bin_dir / "link.exe";
        if (std::filesystem::exists(link)) {
            return link;
        }
        return std::nullopt;
    }

    auto path = bin_dir / name;
    if (std::filesystem::exists(path)) {
        return path;
    }
#ifdef _WIN32
    if (!path.has_extension()) {
        auto exe = path;
        exe += ".exe";
        if (std::filesystem::exists(exe)) {
            return exe;
        }
    }
#endif
    return std::nullopt;
}

auto detect_ready_msvc_instance() -> std::optional<VisualStudioInstance> {
    auto instances = discover_visual_studio_instances();
    if (auto selected = select_ready_msvc_instance(instances)) {
        return selected;
    }
    return detail::fallback_detect_msvc_instance();
}

auto msvc_bin_path(std::filesystem::path const& root) -> std::filesystem::path {
    return root / "bin" / "Hostx64" / "x64";
}

auto msvc_asan_runtime_path(std::filesystem::path const& root) -> std::filesystem::path {
    return msvc_bin_path(root) / "clang_rt.asan_dynamic-x86_64.dll";
}

namespace detail {

auto parse_environment_block(std::string_view text) -> std::map<std::string, std::string> {
    auto env = std::map<std::string, std::string>{};
    auto start = std::size_t{0};
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (auto eq = line.find('='); eq != std::string_view::npos && eq != 0) {
            env.emplace(
                std::string{line.substr(0, eq)},
                std::string{line.substr(eq + 1)});
        }
        start = end + 1;
    }
    return env;
}

auto capture_msvc_environment(VisualStudioInstance const& instance)
    -> std::optional<std::map<std::string, std::string>>
{
    if constexpr (!is_windows) {
        return std::nullopt;
    }
    if (!instance.vcvars64_path || !std::filesystem::exists(*instance.vcvars64_path)) {
        return std::nullopt;
    }

    auto script_path = temp_batch_script_path("intron-msvc-env");
    if (!script_path) {
        return std::nullopt;
    }
    if (!write_capture_msvc_environment_script(*script_path, *instance.vcvars64_path)) {
        return std::nullopt;
    }

    auto captured = cppx::process::system::capture({
        .program = command_processor_path().string(),
        .args = {
            "/d",
            "/c",
            script_path->string(),
        },
    });
    {
        std::error_code ec;
        std::filesystem::remove(*script_path, ec);
    }
    if (!captured || captured->timed_out || captured->exit_code != 0) {
        return std::nullopt;
    }

    auto env_text = trim_line_endings(std::move(captured->stdout_text));
    if (env_text.empty()) {
        return std::nullopt;
    }
    return parse_environment_block(env_text);
}

auto current_msvc_environment() -> std::optional<std::map<std::string, std::string>> {
    if constexpr (!is_windows) {
        return std::nullopt;
    }

    auto include = std::getenv("INCLUDE");
    auto lib = std::getenv("LIB");
    auto libpath = std::getenv("LIBPATH");
    if (!(include && *include) || !(lib && *lib) || !(libpath && *libpath)) {
        return std::nullopt;
    }

    auto env = std::map<std::string, std::string>{};
    env["INCLUDE"] = include;
    env["LIB"] = lib;
    env["LIBPATH"] = libpath;
    if (auto path_env = std::getenv("Path"); path_env && *path_env) {
        env["Path"] = path_env;
    } else if (auto path_upper = std::getenv("PATH"); path_upper && *path_upper) {
        env["Path"] = path_upper;
    }
    return env;
}

auto ensure_msvc_available(registry::ToolInfo const& info) -> bool {
    if constexpr (!is_windows) {
        detail::print_error("msvc provisioning is only supported on Windows");
        return false;
    }

    if (!info.visual_studio) {
        detail::print_error("msvc installer metadata is missing");
        return false;
    }

    auto context = std::format("{} {}", info.name, info.version);
    if (auto ready = detect_ready_msvc_instance()) {
        detail::print_status(cppx::terminal::StatusKind::ok,
                             std::format("msvc {} ready at {}",
                                         info.version,
                                         ready->toolset_root->string()));
        return true;
    }

    auto vswhere_path = find_vswhere_path();
    auto instances = discover_visual_studio_instances();
    auto restart_required = false;
    auto modified_existing = false;

    if (auto modify_target = select_msvc_modify_target(instances)) {
        modified_existing = true;
        auto setup_path = find_visual_studio_setup_path(vswhere_path);
        if (!setup_path) {
            detail::print_error("Visual Studio Installer setup.exe was not found");
            detail::print_hint("repair the Visual Studio Installer and retry");
            return false;
        }

        detail::print_stage("configure", 1, 2, context);
        detail::print_status(cppx::terminal::StatusKind::run,
                             std::format("configuring Visual Studio instance at {}",
                                         modify_target->installation_path.string()));
        auto command = build_msvc_modify_command(info, *modify_target, *setup_path);
        auto result = run_visual_studio_command(command);
        if (!result.status.succeeded()) {
            print_visual_studio_failure(result);
            return false;
        }
        restart_required =
            result.status.kind == VisualStudioInstallerExitKind::SuccessRebootRequired;
    } else {
        auto bootstrapper = cached_msvc_bootstrapper_path(info);
        detail::print_stage("bootstrap", 1, 3, context);
        if (std::filesystem::exists(bootstrapper)) {
            detail::print_status(cppx::terminal::StatusKind::ok,
                                 "using cached Visual Studio Build Tools bootstrapper");
        } else {
            detail::print_status(cppx::terminal::StatusKind::run,
                                 "downloading Visual Studio Build Tools bootstrapper");
            auto downloaded = net::download_file(
                info.visual_studio->bootstrapper_url,
                bootstrapper,
                net::user_agent_headers(user_agent()));
            if (!downloaded) {
                detail::print_error(downloaded.error());
                return false;
            }
        }

        detail::print_stage("install", 2, 3, context);
        detail::print_status(cppx::terminal::StatusKind::run,
                             "installing Visual Studio Build Tools 2022");
        auto command = build_msvc_install_command(info, bootstrapper);
        auto result = run_visual_studio_command(command);
        if (!result.status.succeeded()) {
            print_visual_studio_failure(result);
            return false;
        }
        restart_required =
            result.status.kind == VisualStudioInstallerExitKind::SuccessRebootRequired;
    }

    auto ready = detect_ready_msvc_instance();
    if (!ready) {
        detail::print_error("Visual Studio installer completed but MSVC was not detected");
        detail::print_hint(visual_studio_log_hint());
        return false;
    }

    if (restart_required) {
        detail::print_warning(
            "Visual Studio Build Tools requested a reboot before the toolchain is fully ready");
    }
    auto finish_stage = modified_existing ? 2 : 3;
    detail::print_stage("finish", finish_stage, finish_stage, context);
    detail::print_status(cppx::terminal::StatusKind::ok,
                         std::format("msvc {} ready at {}",
                                     info.version,
                                     ready->toolset_root->string()));
    return true;
}

} // namespace detail

auto install_system_tool(registry::ToolInfo const& info) -> bool {
    if (info.name == "msvc") {
        return detail::ensure_msvc_available(info);
    }
    detail::print_error(std::format("unknown system tool: {}", info.name));
    return false;
}

auto install(registry::ToolInfo const& info) -> bool {
    if (registry::is_system_tool(info.name)) {
        return install_system_tool(info);
    }

    auto home = intron_home();
    auto cached_archive = intron::make_install_plan(home, info).download;
    auto use_cached_archive =
        cached_archive && std::filesystem::exists(cached_archive->archive_path);
    auto plan = intron::make_install_plan(home, info, use_cached_archive);
    auto dest = plan.dest;
    auto context = std::format("{} {}", info.name, info.version);

    if (std::filesystem::exists(dest) && !std::filesystem::is_empty(dest)) {
        detail::print_status(cppx::terminal::StatusKind::ok,
                             std::format("{} {} is already installed",
                                         info.name,
                                         info.version));
        return true;
    }

    if (!plan.download) {
        detail::print_error(std::format("missing download plan for {}", info.name));
        return false;
    }

    std::filesystem::create_directories(plan.downloads_dir);

    auto stage_total = 3;
    if (plan.download->verify_checksum) {
        ++stage_total;
    }
    if (!plan.post_install_actions.empty()) {
        ++stage_total;
    }
    auto stage_index = 1;

    auto success = false;
    auto cleanup = [&] {
        if (!success) {
            std::filesystem::remove_all(dest);
        }
    };

    detail::print_stage("archive", stage_index++, stage_total, context);
    if (plan.download->use_cached_archive) {
        detail::print_status(cppx::terminal::StatusKind::ok,
                             std::format("using cached archive for {} {}",
                                         info.name,
                                         info.version));
    } else {
        detail::print_status(cppx::terminal::StatusKind::run,
                             std::format("downloading {} {}",
                                         info.name,
                                         info.version));
        auto downloaded = net::download_file(
            plan.download->url,
            plan.download->archive_path,
            net::user_agent_headers(detail::user_agent()));
        if (!downloaded) {
            detail::print_error(downloaded.error());
            std::filesystem::remove(plan.download->archive_path);
            cleanup();
            return false;
        }
    }

    if (plan.download->verify_checksum) {
        detail::print_stage("verify", stage_index++, stage_total, context);
        detail::print_status(cppx::terminal::StatusKind::run, "verifying checksum");
        if (!detail::verify_checksum(
                plan.download->archive_path,
                plan.archive_name,
                plan.download->checksum_url)) {
            cleanup();
            return false;
        }
    }

    auto staging = plan.staging_dir;
    std::filesystem::create_directories(staging);

    detail::print_stage("extract", stage_index++, stage_total, context);
    detail::print_status(cppx::terminal::StatusKind::run, "extracting");
    auto extracted = detail::extract_archive(plan.download->archive_path, staging, info);
    if (!extracted) {
        detail::print_error(std::format("extraction failed: {}", extracted.error()));
        std::filesystem::remove_all(staging);
        cleanup();
        return false;
    }

    std::filesystem::create_directories(dest.parent_path());
    std::filesystem::rename(staging, dest);

    if (!plan.post_install_actions.empty()) {
        detail::print_stage("configure", stage_index++, stage_total, context);
    }
    for (auto const& action : plan.post_install_actions) {
        switch (action.kind) {
        case intron::PostInstallActionKind::SetupLlvmConfig:
            detail::setup_llvm_config(action.dest, action.version);
            break;
        }
    }

    if (!detail::verify_installed_binary(dest, info)) {
        std::filesystem::remove_all(dest);
        cleanup();
        return false;
    }

    success = true;
    detail::print_stage("finish", stage_index++, stage_total, context);
    detail::print_status(cppx::terminal::StatusKind::ok,
                         std::format("installed {} {} to {}",
                                     info.name,
                                     info.version,
                                     dest.string()));
    return true;
}

auto remove(std::string_view tool, std::string_view version) -> bool {
    auto path = toolchain_path(tool, version);
    if (!std::filesystem::exists(path)) {
        detail::print_error(std::format("{} {} is not installed", tool, version));
        return false;
    }
    std::filesystem::remove_all(path);
    detail::print_status(cppx::terminal::StatusKind::ok,
                         std::format("removed {} {}", tool, version));
    return true;
}

auto list_installed() -> std::vector<std::pair<std::string, std::string>> {
    auto result = std::vector<std::pair<std::string, std::string>>{};
    auto toolchains = intron_home() / "toolchains";
    if (std::filesystem::exists(toolchains)) {
        for (auto const& tool_entry : std::filesystem::directory_iterator{toolchains}) {
            if (!tool_entry.is_directory()) {
                continue;
            }
            auto tool_name = tool_entry.path().filename().string();
            for (auto const& ver_entry : std::filesystem::directory_iterator{tool_entry.path()}) {
                if (!ver_entry.is_directory()) {
                    continue;
                }
                result.emplace_back(tool_name, ver_entry.path().filename().string());
            }
        }
    }

    if (detect_ready_msvc_instance()) {
        result.emplace_back("msvc", "2022");
    }

    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

auto which(std::string_view binary, std::string_view tool, std::string_view version)
    -> std::optional<std::filesystem::path>
{
    if (tool == "msvc") {
        auto instance = detect_ready_msvc_instance();
        if (!instance) {
            return std::nullopt;
        }
        return msvc_binary_path(*instance, binary);
    }

    auto base = toolchain_path(tool, version);
    auto path = (tool == "ninja") ? base / binary : base / "bin" / binary;

    if (std::filesystem::exists(path)) {
        return path;
    }
#ifdef _WIN32
    auto exe_path = path;
    exe_path += ".exe";
    if (std::filesystem::exists(exe_path)) {
        return exe_path;
    }
#endif
    return std::nullopt;
}

auto msvc_instance() -> std::optional<VisualStudioInstance> {
    return detect_ready_msvc_instance();
}

auto msvc_path() -> std::optional<std::filesystem::path> {
    auto instance = detect_ready_msvc_instance();
    if (!instance || !instance->toolset_root) {
        return std::nullopt;
    }
    return instance->toolset_root;
}

auto msvc_environment() -> std::optional<MsvcEnvironment> {
    auto instance = detect_ready_msvc_instance();
    if (!instance || !instance->toolset_root || !instance->cl_path) {
        return std::nullopt;
    }

    auto variables = detail::capture_msvc_environment(*instance);
    if (!variables) {
        variables = detail::current_msvc_environment();
    }
    if (!variables) {
        return std::nullopt;
    }

    auto env = MsvcEnvironment{
        .installation_path = instance->installation_path,
        .tool_root = *instance->toolset_root,
        .bin_dir = instance->cl_path->parent_path(),
        .cl = *instance->cl_path,
        .asan_runtime = msvc_asan_runtime_path(*instance->toolset_root),
        .variables = std::move(*variables),
    };
    return env;
}

auto discover_windows_sdk_bin_dirs(std::optional<std::string> pinned_version)
    -> std::vector<std::filesystem::path>
{
    if constexpr (!detail::is_windows) {
        return {};
    }

    auto candidate_roots = std::vector<std::filesystem::path>{};
    auto push_root = [&](std::filesystem::path const& root) {
        std::error_code ec;
        if (!root.empty() && std::filesystem::is_directory(root, ec)) {
            if (std::ranges::find(candidate_roots, root) == candidate_roots.end()) {
                candidate_roots.push_back(root);
            }
        }
    };
    if (auto const* pf_x86 = std::getenv("ProgramFiles(x86)"); pf_x86 && *pf_x86) {
        push_root(std::filesystem::path{pf_x86} / "Windows Kits" / "10" / "bin");
    }
    if (auto const* pf = std::getenv("ProgramFiles"); pf && *pf) {
        push_root(std::filesystem::path{pf} / "Windows Kits" / "10" / "bin");
    }
    push_root(std::filesystem::path{R"(C:\Program Files (x86)\Windows Kits\10\bin)"});
    push_root(std::filesystem::path{R"(C:\Program Files\Windows Kits\10\bin)"});

    auto is_sdk_version_name = [](std::string const& name) {
        if (!name.starts_with("10.")) {
            return false;
        }
        auto dots = 0;
        for (auto c : name) {
            if (c == '.') {
                ++dots;
            } else if (c < '0' || c > '9') {
                return false;
            }
        }
        return dots == 3;
    };

    auto arch_subdir = std::string{"x64"};
    if (auto const* proc_arch = std::getenv("PROCESSOR_ARCHITECTURE");
        proc_arch && std::string_view{proc_arch} == "ARM64") {
        arch_subdir = "arm64";
    }

    for (auto const& root : candidate_roots) {
        auto chosen = std::optional<std::string>{};
        std::error_code iter_ec;
        for (auto const& entry : std::filesystem::directory_iterator{root, iter_ec}) {
            if (iter_ec) {
                break;
            }
            if (!entry.is_directory()) {
                continue;
            }
            auto name = entry.path().filename().string();
            if (!is_sdk_version_name(name)) {
                continue;
            }
            if (pinned_version) {
                if (name == *pinned_version) {
                    chosen = name;
                    break;
                }
                continue;
            }
            if (!chosen || detail::compare_dotted_versions(name, *chosen) > 0) {
                chosen = name;
            }
        }
        if (!chosen) {
            continue;
        }

        auto version_dir = root / *chosen;
        auto arch_dir = version_dir / arch_subdir;
        std::error_code ec;
        if (std::filesystem::is_directory(arch_dir, ec)) {
            return {arch_dir};
        }
        if (std::filesystem::is_directory(version_dir, ec)) {
            return {version_dir};
        }
    }

    return {};
}

auto msvc_update_status() -> std::expected<intron::MsvcUpdateStatus, std::string> {
    if constexpr (!detail::is_windows) {
        return std::unexpected("msvc updates are only supported on Windows");
    }

    auto instance = detect_ready_msvc_instance();
    if (!instance) {
        return intron::MsvcUpdateStatus{
            .state = intron::MsvcUpdateState::Missing,
        };
    }

    return detail::make_msvc_update_status(
        *instance,
        detail::fetch_visual_studio_channel_version(*instance));
}

auto upgrade_msvc() -> std::expected<intron::MsvcUpdateStatus, std::string> {
    if constexpr (!detail::is_windows) {
        return std::unexpected("msvc updates are only supported on Windows");
    }

    auto current = msvc_update_status();
    if (!current) {
        return std::unexpected(current.error());
    }
    if (current->state == intron::MsvcUpdateState::Missing) {
        return std::unexpected("msvc is not installed");
    }
    if (current->state == intron::MsvcUpdateState::Unknown) {
        return std::unexpected("could not check latest version for msvc");
    }
    if (current->state == intron::MsvcUpdateState::UpToDate) {
        return *current;
    }

    auto instance = detect_ready_msvc_instance();
    if (!instance) {
        return std::unexpected("msvc is not installed");
    }

    auto vswhere_path = detail::find_vswhere_path();
    auto setup_path = detail::find_visual_studio_setup_path(vswhere_path);
    if (!setup_path) {
        return std::unexpected(
            "Visual Studio Installer setup.exe was not found");
    }

    auto command = build_msvc_update_command(*instance, *setup_path);
    auto result = detail::run_visual_studio_command(command);
    if (!result.status.succeeded()) {
        return std::unexpected(result.status.message);
    }

    auto refreshed = detail::find_instance_by_path(
        discover_visual_studio_instances(),
        instance->installation_path);
    if (!refreshed) {
        return std::unexpected(
            "Visual Studio installer completed but the MSVC instance was not detected");
    }

    auto latest_target = current->latest_installation_version;
    auto refreshed_status = detail::make_msvc_update_status(
        *refreshed,
        detail::fetch_visual_studio_channel_version(*refreshed));
    if (!latest_target || refreshed_status.installation_version.empty() ||
        detail::compare_dotted_versions(
            refreshed_status.installation_version,
            *latest_target) < 0) {
        return std::unexpected(std::format(
            "msvc update completed but the installed version is still {}",
            refreshed_status.current_version.empty()
                ? refreshed_status.installation_version
                : refreshed_status.current_version));
    }

    refreshed_status.state = intron::MsvcUpdateState::UpToDate;
    refreshed_status.installation_version = *latest_target;
    refreshed_status.latest_installation_version = latest_target;
    if (!refreshed_status.latest_version && current->latest_version) {
        refreshed_status.latest_version = current->latest_version;
    }
    if (refreshed_status.current_version.empty()) {
        refreshed_status.current_version =
            refreshed_status.latest_version.value_or(*latest_target);
    } else if (refreshed_status.latest_version) {
        refreshed_status.current_version = *refreshed_status.latest_version;
    }
    return refreshed_status;
}

auto latest_version(std::string_view tool) -> std::optional<std::string> {
    auto api_url = registry::latest_release_api(tool);
    auto json = net::get_text(api_url, net::github_api_headers(detail::user_agent()));
    if (!json) {
        return std::nullopt;
    }
    return net::latest_version_from_release_json(*json);
}

} // namespace installer
