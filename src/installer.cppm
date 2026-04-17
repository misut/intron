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
import intron.domain;
import net;
export import registry;

export namespace installer {

std::filesystem::path intron_home_path(std::filesystem::path const& home_dir) {
    return home_dir / ".intron";
}

std::filesystem::path intron_home() {
    auto home = cppx::env::system::home_dir();
    if (!home)
        throw std::runtime_error("HOME environment variable not set");
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

std::string user_agent() {
    return std::format("intron/{}", EXON_PKG_VERSION);
}

auto trim_line_endings(std::string text) -> std::string {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    return text;
}

auto capture_stdout(cppx::process::ProcessSpec spec) -> std::string {
    auto result = cppx::process::system::capture(std::move(spec));
    if (!result || result->timed_out || result->exit_code != 0)
        return {};
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

int strip_depth(std::string_view prefix) {
    if (prefix.empty()) return 0;
    int depth = 1;
    for (auto c : prefix)
        if (c == '/') ++depth;
    return depth;
}

auto extract_archive(std::filesystem::path const& archive,
                     std::filesystem::path const& staging,
                     registry::ToolInfo const& info)
    -> std::expected<void, std::string> {
    auto format = cppx::archive::archive_format_from_string(info.archive_type);
    if (!format) {
        return std::unexpected(std::format(
            "unknown archive type: {}",
            info.archive_type));
    }

    auto extracted = cppx::archive::system::extract({
        .archive_path = archive,
        .destination_dir = staging,
        .format = *format,
        .strip_components = strip_depth(info.strip_prefix),
    });
    if (!extracted)
        return std::unexpected(extracted.error().message);
    return {};
}

bool verify_checksum(std::filesystem::path const& archive,
                     std::string_view archive_name,
                     std::string const& checksum_url) {
    auto manifest = net::get_text(
        checksum_url, net::user_agent_headers(user_agent()));
    if (!manifest) {
        std::println(
            "warning: could not download checksum file ({}), skipping verification",
            manifest.error());
        return true;
    }

    auto expected_hash = cppx::checksum::find_sha256_for_filename(
        *manifest, archive_name);
    if (!expected_hash) {
        std::println("warning: checksum entry not found, skipping verification");
        return true;
    }

    auto actual_hash = cppx::checksum::system::sha256_file(archive);
    if (!actual_hash) {
        std::println(
            std::cerr,
            "error: could not calculate checksum: {}",
            actual_hash.error().message);
        return false;
    }

    if (*actual_hash != *expected_hash) {
        std::println(std::cerr,
            "error: checksum mismatch\n  expected: {}\n  actual:   {}",
            *expected_hash, *actual_hash);
        return false;
    }
    std::println("Checksum OK");
    return true;
}

void write_clang_wrapper(std::filesystem::path const& bin_dir,
                          std::string const& cfg_flag) {
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
            std::filesystem::permissions(orig,
                std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec,
                std::filesystem::perm_options::add);
        }
    }
}

void setup_llvm_config(std::filesystem::path const& dest, std::string_view /*version*/) {
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
            auto darwin_pos = cfg_target.find("darwin");
            if (darwin_pos != std::string::npos) {
                auto ver_start = darwin_pos + 6;
                auto dot = cfg_target.find('.', ver_start);
                if (dot != std::string::npos)
                    cfg_target = cfg_target.substr(0, dot);
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

            write_clang_wrapper(dest / "bin",
                std::format("--config-system-dir={}", cfg_dir.string()));
            std::println("Generated clang config: {}", cfg_file.string());
        }
    } else if (plat.os == registry::OS::Linux && !target.empty()) {
        auto lib_dir = dest / "lib" / target;
        if (!std::filesystem::exists(lib_dir / "libc++.so") &&
            !std::filesystem::exists(lib_dir / "libc++.a"))
            lib_dir = dest / "lib";

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

        write_clang_wrapper(dest / "bin",
            std::format("--config-system-dir={}", cfg_dir.string()));
        std::println("Generated clang config: {}", cfg_file.string());
    }
}

bool verify_installed_binary(std::filesystem::path const& dest, registry::ToolInfo const& info) {
    std::filesystem::path verify_bin;
    if (info.name == "llvm")
        verify_bin = dest / "bin" / "clang++";
    else if (info.name == "ninja")
        verify_bin = dest / "ninja";
    else if (info.name == "wasi-sdk")
        verify_bin = dest / "bin" / "clang++";
    else
        verify_bin = dest / "bin" / info.name;

    if (!std::filesystem::exists(verify_bin))
        return true;

    auto result = cppx::process::system::capture({
        .program = verify_bin.string(),
        .args = {"--version"},
    });
    if (!result || result->timed_out || result->exit_code != 0) {
        std::println(std::cerr,
            "error: {} binary is not executable on this platform\n"
            "hint: the downloaded binary may not match your architecture",
            info.name);
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> find_latest_msvc_toolset(
    std::filesystem::path const& installs_root) {
    if (!std::filesystem::exists(installs_root))
        return std::nullopt;

    std::filesystem::path best_path;
    std::string best_version;

    for (auto const& year_entry : std::filesystem::directory_iterator{installs_root}) {
        if (!year_entry.is_directory())
            continue;
        for (auto const& edition_entry : std::filesystem::directory_iterator{year_entry.path()}) {
            if (!edition_entry.is_directory())
                continue;

            auto vc_tools = edition_entry.path() / "VC" / "Tools" / "MSVC";
            if (!std::filesystem::exists(vc_tools))
                continue;

            for (auto const& version_entry : std::filesystem::directory_iterator{vc_tools}) {
                if (!version_entry.is_directory())
                    continue;

                auto version = version_entry.path().filename().string();
                auto cl = version_entry.path() / "bin" / "Hostx64" / "x64" / "cl.exe";
                if (!std::filesystem::exists(cl))
                    continue;

                if (version > best_version) {
                    best_version = version;
                    best_path = version_entry.path();
                }
            }
        }
    }

    if (best_version.empty())
        return std::nullopt;
    return best_path;
}

// Detect MSVC installation via vswhere (Windows only)
std::optional<std::filesystem::path> detect_msvc_path() {
    if constexpr (!is_windows) return std::nullopt;

    // vswhere ships with VS Build Tools / VS Installer
    std::string vswhere_cmd;
    auto const* pf86 = std::getenv("ProgramFiles(x86)");
    if (pf86) {
        auto vswhere = std::filesystem::path{pf86}
            / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
        if (std::filesystem::exists(vswhere))
            vswhere_cmd = vswhere.string();
    }
    if (vswhere_cmd.empty()) {
        // Try PATH
        if (auto found = cppx::env::system::find_in_path("vswhere"))
            vswhere_cmd = found->string();
        else
            return std::nullopt;
    }

    auto install_path = capture_stdout({
        .program = vswhere_cmd,
        .args = {
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        },
    });
    if (!install_path.empty()) {
        // Find the latest MSVC toolset version
        auto vc_tools = std::filesystem::path{install_path} / "VC" / "Tools" / "MSVC";
        if (std::filesystem::exists(vc_tools)) {
            std::string latest_ver;
            for (auto const& entry : std::filesystem::directory_iterator{vc_tools}) {
                if (!entry.is_directory()) continue;
                auto ver = entry.path().filename().string();
                if (ver > latest_ver) latest_ver = ver;
            }
            if (!latest_ver.empty())
                return vc_tools / latest_ver;
        }
    }

    std::vector<std::filesystem::path> install_roots;
    for (auto var : {"ProgramFiles", "ProgramFiles(x86)"}) {
        auto root = std::getenv(var);
        if (!root || !*root)
            continue;
        install_roots.push_back(std::filesystem::path{root} / "Microsoft Visual Studio");
    }
    install_roots.push_back("C:/Program Files/Microsoft Visual Studio");
    install_roots.push_back("C:/Program Files (x86)/Microsoft Visual Studio");

    std::ranges::sort(install_roots);
    install_roots.erase(std::unique(install_roots.begin(), install_roots.end()), install_roots.end());

    for (auto const& installs_root : install_roots)
        if (auto detected = find_latest_msvc_toolset(installs_root))
            return detected;

    return std::nullopt;
}

std::filesystem::path msvc_bin_path(std::filesystem::path const& root) {
    return root / "bin" / "Hostx64" / "x64";
}

std::filesystem::path msvc_vcvars64_path(std::filesystem::path const& root) {
    return root.parent_path().parent_path().parent_path() / "Auxiliary" / "Build" / "vcvars64.bat";
}

std::filesystem::path msvc_asan_runtime_path(std::filesystem::path const& root) {
    return msvc_bin_path(root) / "clang_rt.asan_dynamic-x86_64.dll";
}

std::map<std::string, std::string> parse_environment_block(std::string_view text) {
    std::map<std::string, std::string> env;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        auto line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.remove_suffix(1);
        auto eq = line.find('=');
        if (eq != std::string_view::npos && eq != 0) {
            env.emplace(std::string{line.substr(0, eq)}, std::string{line.substr(eq + 1)});
        }
        start = end + 1;
    }
    return env;
}

std::optional<std::map<std::string, std::string>> capture_msvc_environment(
    std::filesystem::path const& root) {
    if constexpr (!is_windows)
        return std::nullopt;

    auto vcvars = msvc_vcvars64_path(root);
    if (!std::filesystem::exists(vcvars))
        return std::nullopt;

    auto captured = cppx::process::system::capture({
        .program = "cmd",
        .args = {
            "/d",
            "/c",
            std::format("call \"{}\" >nul && set", vcvars.string()),
        },
    });
    if (!captured || captured->timed_out || captured->exit_code != 0)
        return std::nullopt;

    auto env_text = trim_line_endings(std::move(captured->stdout_text));
    if (env_text.empty())
        return std::nullopt;
    return parse_environment_block(env_text);
}

std::optional<std::map<std::string, std::string>> current_msvc_environment(
    std::filesystem::path const& root) {
    if constexpr (!is_windows)
        return std::nullopt;

    auto include = std::getenv("INCLUDE");
    auto lib = std::getenv("LIB");
    auto libpath = std::getenv("LIBPATH");
    if (!(include && *include) || !(lib && *lib) || !(libpath && *libpath))
        return std::nullopt;

    std::map<std::string, std::string> env;
    env["INCLUDE"] = include;
    env["LIB"] = lib;
    env["LIBPATH"] = libpath;
    if (auto path_env = std::getenv("Path"); path_env && *path_env)
        env["Path"] = path_env;
    else if (auto path_upper = std::getenv("PATH"); path_upper && *path_upper)
        env["Path"] = path_upper;

    return env;
}

} // namespace detail

struct MsvcEnvironment {
    std::filesystem::path tool_root;
    std::filesystem::path bin_dir;
    std::filesystem::path cl;
    std::filesystem::path asan_runtime;
    std::map<std::string, std::string> variables;
};

// Install a system tool (e.g. msvc) — detect and verify, no download
bool install_system_tool(registry::ToolInfo const& info) {
    if (info.name == "msvc") {
        auto path = detail::detect_msvc_path();
        if (!path) {
            std::println(std::cerr,
                "error: MSVC is not installed\n"
                "hint: install Visual Studio or Build Tools with C++ workload");
            return false;
        }
        std::println("msvc detected at {}", path->string());
        return true;
    }
    std::println(std::cerr, "error: unknown system tool: {}", info.name);
    return false;
}

bool install(registry::ToolInfo const& info) {
    // System tools (e.g. msvc) are detected, not downloaded
    if (registry::is_system_tool(info.name))
        return install_system_tool(info);

    auto home = intron_home();
    auto cached_archive = intron::make_install_plan(home, info).download;
    auto use_cached_archive = cached_archive && std::filesystem::exists(cached_archive->archive_path);
    auto plan = intron::make_install_plan(home, info, use_cached_archive);
    auto dest = plan.dest;

    // Already installed
    if (std::filesystem::exists(dest) && !std::filesystem::is_empty(dest)) {
        std::println("{} {} is already installed", info.name, info.version);
        return true;
    }

    if (!plan.download) {
        std::println(std::cerr, "error: missing download plan for {}", info.name);
        return false;
    }

    auto downloads = plan.downloads_dir;
    auto archive_name = plan.archive_name;
    auto archive_path = plan.download->archive_path;

    std::filesystem::create_directories(downloads);

    // Cleanup guard on failure
    bool success = false;
    auto cleanup = [&] {
        if (!success) {
            std::filesystem::remove_all(dest);
        }
    };

    // Download (reuse cached archive if available)
    if (plan.download->use_cached_archive) {
        std::println("Using cached archive for {} {}...", info.name, info.version);
    } else {
        std::println("Downloading {} {}...", info.name, info.version);
        auto dl = net::download_file(
            plan.download->url, archive_path, net::user_agent_headers(detail::user_agent()));
        if (!dl) {
            std::println(std::cerr, "error: {}", dl.error());
            std::filesystem::remove(archive_path);
            cleanup();
            return false;
        }
    }

    // Checksum verification
    if (plan.download->verify_checksum) {
        std::println("Verifying checksum...");
        if (!detail::verify_checksum(
                archive_path,
                archive_name,
                plan.download->checksum_url)) {
            cleanup();
            return false;
        }
    }

    // Extract to staging directory (atomic install: staging → rename)
    auto staging = plan.staging_dir;
    std::filesystem::create_directories(staging);

    std::println("Extracting...");
    auto extracted = detail::extract_archive(archive_path, staging, info);
    if (!extracted) {
        std::println(std::cerr, "error: extraction failed: {}", extracted.error());
        std::filesystem::remove_all(staging);
        cleanup();
        return false;
    }

    // Atomically place at final destination
    std::filesystem::create_directories(dest.parent_path());
    std::filesystem::rename(staging, dest);

    // Generate clang config + wrapper scripts after LLVM install
    for (auto const& action : plan.post_install_actions) {
        switch (action.kind) {
        case intron::PostInstallActionKind::SetupLlvmConfig:
            detail::setup_llvm_config(action.dest, action.version);
            break;
        }
    }

    // Verify the installed binary is executable on this platform
    if (!detail::verify_installed_binary(dest, info)) {
        std::filesystem::remove_all(dest);
        cleanup();
        return false;
    }

    success = true;
    std::println("Installed {} {} to {}", info.name, info.version, dest.string());
    return true;
}

bool remove(std::string_view tool, std::string_view version) {
    auto path = toolchain_path(tool, version);
    if (!std::filesystem::exists(path)) {
        std::println(std::cerr, "error: {} {} is not installed", tool, version);
        return false;
    }
    std::filesystem::remove_all(path);
    std::println("Removed {} {}", tool, version);
    return true;
}

std::vector<std::pair<std::string, std::string>> list_installed() {
    std::vector<std::pair<std::string, std::string>> result;
    auto toolchains = intron_home() / "toolchains";
    if (!std::filesystem::exists(toolchains)) {
        return result;
    }
    for (auto const& tool_entry : std::filesystem::directory_iterator{toolchains}) {
        if (!tool_entry.is_directory()) continue;
        auto tool_name = tool_entry.path().filename().string();
        for (auto const& ver_entry : std::filesystem::directory_iterator{tool_entry.path()}) {
            if (!ver_entry.is_directory()) continue;
            result.emplace_back(tool_name, ver_entry.path().filename().string());
        }
    }
    std::ranges::sort(result);
    return result;
}

std::optional<std::filesystem::path> which(
    std::string_view binary, std::string_view tool, std::string_view version)
{
    auto base = toolchain_path(tool, version);
    auto path = (tool == "ninja")
        ? base / binary
        : base / "bin" / binary;

    if (std::filesystem::exists(path)) {
        return path;
    }
#ifdef _WIN32
    // Windows: try with .exe extension
    auto exe_path = path;
    exe_path += ".exe";
    if (std::filesystem::exists(exe_path)) {
        return exe_path;
    }
#endif
    return std::nullopt;
}

std::optional<std::filesystem::path> msvc_path() {
    return detail::detect_msvc_path();
}

std::filesystem::path msvc_bin_path(std::filesystem::path const& root) {
    return detail::msvc_bin_path(root);
}

std::filesystem::path msvc_asan_runtime_path(std::filesystem::path const& root) {
    return detail::msvc_asan_runtime_path(root);
}

std::optional<MsvcEnvironment> msvc_environment() {
    auto root = detail::detect_msvc_path();
    if (!root)
        return std::nullopt;

    auto bin_dir = detail::msvc_bin_path(*root);
    auto cl = bin_dir / "cl.exe";
    if (!std::filesystem::exists(cl))
        return std::nullopt;

    auto variables = detail::capture_msvc_environment(*root);
    if (!variables)
        variables = detail::current_msvc_environment(*root);
    if (!variables)
        return std::nullopt;

    MsvcEnvironment env{
        .tool_root = *root,
        .bin_dir = bin_dir,
        .cl = cl,
        .asan_runtime = detail::msvc_asan_runtime_path(*root),
        .variables = std::move(*variables),
    };
    return env;
}

std::optional<std::string> latest_version(std::string_view tool) {
    auto api_url = registry::latest_release_api(tool);
    auto json = net::get_text(
        api_url, net::github_api_headers(detail::user_agent()));
    if (!json) return std::nullopt;
    return net::latest_version_from_release_json(*json);
}

} // namespace installer
