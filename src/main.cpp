import std;
import installer;
import config;

namespace {

// Map binary name to its parent tool
std::optional<std::string> tool_for_binary(std::string_view binary) {
    // LLVM binaries
    if (binary.starts_with("clang") || binary.starts_with("llvm-") ||
        binary.starts_with("lldb") || binary.starts_with("lld") ||
        binary == "ld.lld" || binary == "ld64.lld" ||
        binary == "wasm-ld" || binary == "dsymutil") {
        return "llvm";
    }
    // CMake binaries
    if (binary == "cmake" || binary == "ctest" || binary == "cpack" || binary == "ccmake") {
        return "cmake";
    }
    // Ninja
    if (binary == "ninja") {
        return "ninja";
    }
    return std::nullopt;
}

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif
constexpr auto intron_version = EXON_PKG_VERSION;

void print_usage() {
    std::println("intron {}", intron_version);
    std::println("");
    std::println("Usage: intron <command> [args...]");
    std::println("");
    std::println("Commands:");
    std::println("  install [tool] [version]   Install toolchain(s) (reads .intron.toml if no args)");
    std::println("  remove  <tool> <version>   Remove a toolchain");
    std::println("  list                       List installed toolchains");
    std::println("  which   <binary>           Print path to binary");
    std::println("  default <tool> <version>   Set global default version");
    std::println("  use     [tool] [version]   Set project toolchain in .intron.toml");
    std::println("  update                     Check for updates");
    std::println("  upgrade [tool]             Upgrade tools to latest");
    std::println("  env                        Print environment variables");
    std::println("  self-update                Update intron itself");
    std::println("  help                       Show this message");
    std::println("");
    std::println("Tools: llvm, cmake, ninja, wasi-sdk, wasmtime");
}

int cmd_install(int argc, char* argv[]) {
    if (argc < 4) {
        // No args: install all from .intron.toml
        auto toolchain = config::load_project_toolchain();
        if (toolchain.empty()) {
            std::println(std::cerr, "Usage: intron install <tool> <version>");
            std::println(std::cerr, "       intron install  (reads .intron.toml)");
            return 1;
        }
        int failed = 0;
        for (auto const& [tool, version] : toolchain) {
            auto info = registry::resolve(tool, version);
            if (!installer::install(info)) ++failed;
        }
        return failed > 0 ? 1 : 0;
    }
    auto info = registry::resolve(argv[2], argv[3]);
    return installer::install(info) ? 0 : 1;
}

int cmd_remove(int argc, char* argv[]) {
    if (argc != 4) {
        std::println(std::cerr, "Usage: intron remove <tool> <version>");
        return 1;
    }
    return installer::remove(argv[2], argv[3]) ? 0 : 1;
}

int cmd_list() {
    auto installed = installer::list_installed();
    if (installed.empty()) {
        std::println("No toolchains installed");
        return 0;
    }
    auto defaults = config::load_effective_defaults();
    for (auto const& [tool, version] : installed) {
        auto it = defaults.find(tool);
        if (it != defaults.end() && it->second == version) {
            std::println("{} {} (default)", tool, version);
        } else {
            std::println("{} {}", tool, version);
        }
    }
    return 0;
}

int cmd_which(int argc, char* argv[]) {
    if (argc != 3) {
        std::println(std::cerr, "Usage: intron which <binary>");
        return 1;
    }
    auto binary = std::string_view{argv[2]};
    auto tool = tool_for_binary(binary);
    if (!tool) {
        std::println(std::cerr, "error: unknown binary '{}'", binary);
        return 1;
    }
    auto version = config::get_default(*tool);
    if (!version) {
        std::println(std::cerr, "error: no default version set for {}", *tool);
        std::println(std::cerr, "hint: run 'intron default {} <version>'", *tool);
        return 1;
    }
    auto path = installer::which(binary, *tool, *version);
    if (!path) {
        std::println(std::cerr, "error: '{}' not found in {} {}", binary, *tool, *version);
        return 1;
    }
    std::println("{}", path->string());
    return 0;
}

int cmd_default(int argc, char* argv[]) {
    if (argc != 4) {
        std::println(std::cerr, "Usage: intron default <tool> <version>");
        return 1;
    }
    auto tool = std::string_view{argv[2]};
    auto version = std::string_view{argv[3]};

    // Check if installed
    auto path = installer::toolchain_path(tool, version);
    if (!std::filesystem::exists(path)) {
        std::println(std::cerr, "error: {} {} is not installed", tool, version);
        std::println(std::cerr, "hint: run 'intron install {} {}'", tool, version);
        return 1;
    }

    config::set_default(tool, version);
    std::println("Set {} default to {}", tool, version);
    return 0;
}

int cmd_use(int argc, char* argv[]) {
    auto existing = config::load_project_toolchain();

    if (argc < 3) {
        // No args: write all effective defaults
        auto defaults = config::load_effective_defaults();
        if (defaults.empty()) {
            std::println(std::cerr, "error: no default versions set");
            std::println(std::cerr, "hint: run 'intron default <tool> <version>' first");
            return 1;
        }
        for (auto const& [tool, version] : defaults) {
            existing[tool] = version;
            std::println("set {} {}", tool, version);
        }
    } else {
        auto tool = std::string{argv[2]};
        std::string version;
        if (argc >= 4) {
            version = argv[3];
            auto dest = installer::toolchain_path(tool, version);
            if (!std::filesystem::exists(dest))
                std::println("warning: {} {} is not installed", tool, version);
        } else {
            auto def = config::get_default(tool);
            if (!def) {
                std::println(std::cerr, "error: no default version for {}", tool);
                return 1;
            }
            version = *def;
        }
        existing[tool] = version;
        std::println("set {} {}", tool, version);
    }

    config::write_project_config(existing);
    std::println("wrote .intron.toml");
    return 0;
}

std::map<std::string, std::string> build_tool_map() {
    auto installed = installer::list_installed();
    auto defaults = config::load_effective_defaults();
    std::map<std::string, std::string> current;
    for (auto const& [tool, version] : installed)
        if (!current.contains(tool)) current[tool] = version;
    for (auto const& [tool, version] : defaults)
        if (!current.contains(tool)) current[tool] = version;
    return current;
}

int cmd_update() {
    auto current = build_tool_map();

    if (current.empty()) {
        // Show latest versions for all tools if none installed
        for (auto tool : registry::supported_tools) {
            auto latest = installer::latest_version(tool);
            if (latest) {
                std::println("{}: latest {}", tool, *latest);
            }
        }
        return 0;
    }

    bool has_update = false;
    for (auto const& [tool, version] : current) {
        auto latest = installer::latest_version(tool);
        if (!latest) {
            std::println("{} {}: could not check latest", tool, version);
            continue;
        }
        if (*latest != version) {
            std::println("{} {} -> {} (update available)", tool, version, *latest);
            has_update = true;
        } else {
            std::println("{} {} (up to date)", tool, version);
        }
    }

    if (has_update) {
        std::println("");
        std::println("Run 'intron install <tool> <version>' to update");
    }
    return 0;
}

int cmd_upgrade(int argc, char* argv[]) {
    auto current = build_tool_map();

    // Filter to specific tool if requested
    if (argc >= 3) {
        auto tool = std::string{argv[2]};
        if (!current.contains(tool)) {
            std::println(std::cerr, "error: {} is not installed", tool);
            return 1;
        }
        auto version = current[tool];
        current.clear();
        current[tool] = version;
    }

    if (current.empty()) {
        std::println("No toolchains installed");
        return 0;
    }

    int upgraded = 0;
    for (auto const& [tool, version] : current) {
        auto latest = installer::latest_version(tool);
        if (!latest) {
            std::println("{}: could not check latest version", tool);
            continue;
        }
        if (*latest == version) {
            std::println("{} {} (up to date)", tool, version);
            continue;
        }
        std::println("{} {} -> {}...", tool, version, *latest);
        auto info = registry::resolve(tool, *latest);
        if (!installer::install(info)) {
            std::println(std::cerr, "error: failed to upgrade {}", tool);
            continue;
        }
        config::set_default(tool, *latest);
        ++upgraded;
    }

    if (upgraded > 0) {
        std::println("");
        std::println("Upgraded {} tool{}", upgraded, upgraded == 1 ? "" : "s");
    }
    return 0;
}

int cmd_self_update(std::string_view self_path) {
    auto current_version = intron_version;

    std::println("Checking for updates...");
    auto latest = installer::latest_version("intron");
    if (!latest) {
        std::println(std::cerr, "error: could not check latest version");
        return 1;
    }
    if (*latest == current_version) {
        std::println("intron {} is already up to date", current_version);
        return 0;
    }
    std::println("Updating intron {} -> {}...", current_version, *latest);

    auto tmp = std::filesystem::temp_directory_path() / "intron-update";
    std::filesystem::create_directories(tmp);
    auto triple = registry::platform_triple();

#ifdef _WIN32
    auto ext = ".zip";
#else
    auto ext = ".tar.gz";
#endif
    auto archive = tmp / std::format("intron{}", ext);
    auto url = std::format(
        "https://github.com/misut/intron/releases/download/v{}/intron-v{}-{}{}",
        *latest, *latest, triple, ext);

#ifdef _WIN32
    auto dl_cmd = std::format("curl -fsSL -o \"{}\" \"{}\"", archive.string(), url);
#else
    auto dl_cmd = std::format("curl -fsSL -o '{}' '{}'", archive.string(), url);
#endif
    if (std::system(dl_cmd.c_str()) != 0) {
        std::println(std::cerr, "error: download failed");
        std::filesystem::remove_all(tmp);
        return 1;
    }

#ifdef _WIN32
    // Use %SystemRoot%\System32\tar.exe (bsdtar) directly: GNU tar from
    // Git Bash / MSYS treats "C:\..." as a remote host. The extra outer
    // quotes are needed because cmd /c strips first/last quotes when the
    // command starts with a quoted path.
    std::string tar = "tar";
    if (auto const* sr = std::getenv("SystemRoot")) {
        auto sys_tar = std::filesystem::path{sr} / "System32" / "tar.exe";
        if (std::filesystem::exists(sys_tar))
            tar = std::format("\"{}\"", sys_tar.string());
    }
    auto extract_cmd = std::format("\"{} xf \"{}\" -C \"{}\"\"",
        tar, archive.string(), tmp.string());
#else
    auto extract_cmd = std::format("tar xzf '{}' -C '{}'", archive.string(), tmp.string());
#endif
    if (std::system(extract_cmd.c_str()) != 0) {
        std::println(std::cerr, "error: extraction failed");
        std::filesystem::remove_all(tmp);
        return 1;
    }

    auto target = std::filesystem::canonical(self_path);
#ifdef _WIN32
    // Windows: rename the running exe out of the way, then place the new
    // one. Windows lets us rename a running exe but not delete it while
    // the process is alive, so we mark the stale .old for best-effort
    // removal and move on if it sticks around.
    auto new_binary = tmp / "intron.exe";
    auto old_binary = target;
    old_binary += ".old";
    std::error_code ec;
    std::filesystem::remove(old_binary, ec); // clean leftover from prior update
    std::filesystem::rename(target, old_binary);
    std::filesystem::rename(new_binary, target);
    std::filesystem::remove(old_binary, ec);
#else
    auto new_binary = tmp / "intron";
    std::filesystem::rename(new_binary, target);
#endif
    std::error_code tmp_ec;
    std::filesystem::remove_all(tmp, tmp_ec);

    std::println("Updated intron to {}", *latest);
    return 0;
}

int cmd_env() {
    auto defaults = config::load_effective_defaults();
    if (defaults.empty()) {
        std::println(std::cerr, "error: no default versions set");
        std::println(std::cerr, "hint: run 'intron default <tool> <version>'");
        return 1;
    }

    // Collect bin directories for PATH (wasi-sdk is excluded to avoid
    // clang binary conflicts with llvm; it is exposed via WASI_SDK_PATH instead)
    std::vector<std::string> paths;
    for (auto const& [tool, version] : defaults) {
        if (tool == "wasi-sdk") continue;
        auto base = installer::toolchain_path(tool, version);
        if (!std::filesystem::exists(base)) continue;
        if (tool == "ninja" || tool == "wasmtime") {
            paths.push_back(base.string());
        } else {
            paths.push_back((base / "bin").string());
        }
    }

    // PATH
#ifdef _WIN32
    constexpr auto path_sep = ';';
#else
    constexpr auto path_sep = ':';
#endif
    if (!paths.empty()) {
        std::string path_val;
        for (auto const& p : paths) {
            if (!path_val.empty()) path_val += path_sep;
            path_val += p;
        }
#ifdef _WIN32
        // PowerShell syntax
        std::println("$env:PATH = \"{}{}{}\";", path_val, path_sep, "$env:PATH");
#else
        std::println("export PATH=\"{}{}$PATH\";", path_val, path_sep);
#endif
    }

    // CC/CXX (when LLVM is set as default)
    if (defaults.contains("llvm")) {
        auto llvm_bin = installer::toolchain_path("llvm", defaults.at("llvm")) / "bin";
#ifdef _WIN32
        if (std::filesystem::exists(llvm_bin / "clang-cl.exe")) {
            std::println("$env:CC = \"{}\";", (llvm_bin / "clang-cl.exe").string());
            std::println("$env:CXX = \"{}\";", (llvm_bin / "clang-cl.exe").string());
        }
#else
        if (std::filesystem::exists(llvm_bin / "clang")) {
            std::println("export CC=\"{}\";", (llvm_bin / "clang").string());
            std::println("export CXX=\"{}\";", (llvm_bin / "clang++").string());
        }
#endif
    }

    // WASI_SDK_PATH (when wasi-sdk is set as default)
    if (defaults.contains("wasi-sdk")) {
        auto wasi_path = installer::toolchain_path("wasi-sdk", defaults.at("wasi-sdk"));
        if (std::filesystem::exists(wasi_path)) {
#ifdef _WIN32
            std::println("$env:WASI_SDK_PATH = \"{}\";", wasi_path.string());
#else
            std::println("export WASI_SDK_PATH=\"{}\";", wasi_path.string());
#endif
        }
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    auto command = std::string_view{argv[1]};

    try {
        if (command == "install") return cmd_install(argc, argv);
        if (command == "remove")  return cmd_remove(argc, argv);
        if (command == "list")    return cmd_list();
        if (command == "which")   return cmd_which(argc, argv);
        if (command == "default") return cmd_default(argc, argv);
        if (command == "use")     return cmd_use(argc, argv);
        if (command == "update")  return cmd_update();
        if (command == "upgrade") return cmd_upgrade(argc, argv);
        if (command == "env")     return cmd_env();
        if (command == "self-update") return cmd_self_update(argv[0]);
        if (command == "help" || command == "--help" || command == "-h") {
            print_usage();
            return 0;
        }

        std::println(std::cerr, "error: unknown command '{}'", command);
        print_usage();
        return 1;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}
