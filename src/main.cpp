import std;
import installer;
import config;

namespace {

// binary 이름으로 어떤 tool에 속하는지 판별
std::optional<std::string> tool_for_binary(std::string_view binary) {
    // LLVM 바이너리
    if (binary.starts_with("clang") || binary.starts_with("llvm-") ||
        binary.starts_with("lldb") || binary.starts_with("lld") ||
        binary == "ld.lld" || binary == "ld64.lld" ||
        binary == "wasm-ld" || binary == "dsymutil") {
        return "llvm";
    }
    // CMake 바이너리
    if (binary == "cmake" || binary == "ctest" || binary == "cpack" || binary == "ccmake") {
        return "cmake";
    }
    // Ninja
    if (binary == "ninja") {
        return "ninja";
    }
    return std::nullopt;
}

void print_usage() {
    std::println("Usage: intron <command> [args...]");
    std::println("");
    std::println("Commands:");
    std::println("  install <tool> <version>   Install a toolchain");
    std::println("  remove  <tool> <version>   Remove a toolchain");
    std::println("  list                       List installed toolchains");
    std::println("  which   <binary>           Print path to binary");
    std::println("  default <tool> <version>   Set default version");
    std::println("  update                     Check for updates");
    std::println("  env                        Print environment variables");
    std::println("  help                       Show this message");
    std::println("");
    std::println("Tools: llvm, cmake, ninja");
}

int cmd_install(int argc, char* argv[]) {
    if (argc != 4) {
        std::println(std::cerr, "Usage: intron install <tool> <version>");
        return 1;
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
    auto defaults = config::load_defaults();
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

    // 설치 여부 확인
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

int cmd_update() {
    auto installed = installer::list_installed();
    auto defaults = config::load_defaults();

    // 설치된 도구 + default 도구의 합집합
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

    if (current.empty()) {
        // 설치된 게 없으면 모든 도구의 최신 버전 표시
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

int cmd_env() {
    auto defaults = config::load_defaults();
    if (defaults.empty()) {
        std::println(std::cerr, "error: no default versions set");
        std::println(std::cerr, "hint: run 'intron default <tool> <version>'");
        return 1;
    }

    // PATH에 추가할 bin 디렉토리 수집
    std::vector<std::string> paths;
    for (auto const& [tool, version] : defaults) {
        auto base = installer::toolchain_path(tool, version);
        if (!std::filesystem::exists(base)) continue;
        if (tool == "ninja") {
            paths.push_back(base.string());
        } else {
            paths.push_back((base / "bin").string());
        }
    }

    if (paths.empty()) return 0;

    // PATH
    std::string path_val;
    for (auto const& p : paths) {
        if (!path_val.empty()) path_val += ':';
        path_val += p;
    }
    std::println("export PATH=\"{}:$PATH\";", path_val);

    // CC/CXX (LLVM이 default에 있을 때)
    if (defaults.contains("llvm")) {
        auto llvm_bin = installer::toolchain_path("llvm", defaults.at("llvm")) / "bin";
        if (std::filesystem::exists(llvm_bin / "clang")) {
            std::println("export CC=\"{}\";", (llvm_bin / "clang").string());
            std::println("export CXX=\"{}\";", (llvm_bin / "clang++").string());
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
        if (command == "update")  return cmd_update();
        if (command == "env")     return cmd_env();
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
