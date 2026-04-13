import std;
import config;
import registry;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
}

void test_config_path() {
    auto path = config::config_path();
    check(path.string().contains(".intron/config.toml"), "config path");
}

void test_set_and_load_defaults() {
    auto path = config::config_path();
    auto backup = path;
    backup += ".test_bak";
    bool had_config = std::filesystem::exists(path);
    if (had_config) {
        std::filesystem::copy_file(path, backup);
    }

    config::set_default("test-tool", "1.2.3");
    config::set_default("another", "4.5.6");

    auto defaults = config::load_defaults();
    check(defaults.contains("test-tool"), "defaults contains test-tool");
    check(defaults.at("test-tool") == "1.2.3", "test-tool version");
    check(defaults.contains("another"), "defaults contains another");
    check(defaults.at("another") == "4.5.6", "another version");

    auto ver = config::get_default("test-tool");
    check(ver.has_value(), "get_default returns value");
    check(*ver == "1.2.3", "get_default correct version");

    auto missing = config::get_default("nonexistent");
    check(!missing.has_value(), "get_default returns nullopt for missing");

    if (had_config) {
        std::filesystem::rename(backup, path);
    } else {
        std::filesystem::remove(path);
    }
}

void test_project_config() {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "intron_test_project_config";
    std::filesystem::create_directories(tmp);
    auto const saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);

    try {
        {
            auto out = std::ofstream{".intron.toml"};
            out << "[toolchain]\n";
            out << "llvm = \"19.0.0\"\n";
        }

        auto result = config::load_project_toolchain();
        check(result.contains("llvm"), "project config contains llvm");
        check(result.at("llvm") == "19.0.0", "project config llvm version");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        std::filesystem::remove_all(tmp);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
    std::filesystem::remove_all(tmp);
}

void test_platform_config() {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "intron_test_platform_config";
    std::filesystem::create_directories(tmp);
    auto const saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);

    try {
        // Write a config with platform-specific sections
        {
            auto out = std::ofstream{".intron.toml"};
            out << "[toolchain]\n";
            out << "cmake = \"4.3.1\"\n";
            out << "ninja = \"1.13.2\"\n";
            out << "\n";
            out << "[toolchain.macos]\n";
            out << "llvm = \"22.1.2\"\n";
            out << "\n";
            out << "[toolchain.linux]\n";
            out << "llvm = \"22.1.2\"\n";
            out << "\n";
            out << "[toolchain.windows]\n";
            out << "msvc = \"latest\"\n";
        }

        // load_project_toolchain should return common + current platform tools
        auto result = config::load_project_toolchain();
        check(result.contains("cmake"), "platform config contains cmake");
        check(result.at("cmake") == "4.3.1", "cmake version");
        check(result.contains("ninja"), "platform config contains ninja");

        // Current platform should have its specific tools merged in
        auto plat = std::string{registry::platform_name()};
        if (plat == "macos" || plat == "linux") {
            check(result.contains("llvm"), "platform config contains llvm on unix");
            check(result.at("llvm") == "22.1.2", "llvm version on unix");
            check(!result.contains("msvc"), "no msvc on unix");
        } else if (plat == "windows") {
            check(result.contains("msvc"), "platform config contains msvc on windows");
            check(!result.contains("llvm"), "no llvm on windows");
        }

        // load_full_project_config should return everything
        auto full = config::load_full_project_config();
        check(full.common.contains("cmake"), "full config common has cmake");
        check(full.common.contains("ninja"), "full config common has ninja");
        check(!full.common.contains("llvm"), "full config common has no llvm");
        check(full.platforms.contains("macos"), "full config has macos platform");
        check(full.platforms.at("macos").contains("llvm"), "macos platform has llvm");
        check(full.platforms.contains("windows"), "full config has windows platform");
        check(full.platforms.at("windows").contains("msvc"), "windows platform has msvc");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        std::filesystem::remove_all(tmp);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
    std::filesystem::remove_all(tmp);
}

void test_platform_override() {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "intron_test_platform_override";
    std::filesystem::create_directories(tmp);
    auto const saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);

    try {
        // Platform-specific version should override common version
        auto plat = std::string{registry::platform_name()};
        {
            auto out = std::ofstream{".intron.toml"};
            out << "[toolchain]\n";
            out << "llvm = \"21.0.0\"\n";
            out << "\n";
            out << std::format("[toolchain.{}]\n", plat);
            out << "llvm = \"22.1.2\"\n";
        }

        auto result = config::load_project_toolchain();
        check(result.contains("llvm"), "override: contains llvm");
        check(result.at("llvm") == "22.1.2", "override: platform version wins");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        std::filesystem::remove_all(tmp);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
    std::filesystem::remove_all(tmp);
}

void test_write_project_config_roundtrip() {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "intron_test_write_roundtrip";
    std::filesystem::create_directories(tmp);
    auto const saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);

    try {
        config::ToolchainConfig cfg;
        cfg.common["cmake"] = "4.3.1";
        cfg.common["ninja"] = "1.13.2";
        cfg.platforms["macos"]["llvm"] = "22.1.2";
        cfg.platforms["linux"]["llvm"] = "22.1.2";
        cfg.platforms["windows"]["msvc"] = "latest";

        config::write_project_config(cfg);

        // Read back and verify
        auto full = config::load_full_project_config();
        check(full.common.at("cmake") == "4.3.1", "roundtrip: cmake");
        check(full.common.at("ninja") == "1.13.2", "roundtrip: ninja");
        check(full.platforms.at("macos").at("llvm") == "22.1.2", "roundtrip: macos llvm");
        check(full.platforms.at("linux").at("llvm") == "22.1.2", "roundtrip: linux llvm");
        check(full.platforms.at("windows").at("msvc") == "latest", "roundtrip: windows msvc");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        std::filesystem::remove_all(tmp);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
    std::filesystem::remove_all(tmp);
}

void test_backward_compatibility() {
    auto const tmp = std::filesystem::temp_directory_path() /
                     "intron_test_backward_compat";
    std::filesystem::create_directories(tmp);
    auto const saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(tmp);

    try {
        // Old-style config without platform sections should still work
        {
            auto out = std::ofstream{".intron.toml"};
            out << "[toolchain]\n";
            out << "llvm = \"22.1.2\"\n";
            out << "cmake = \"4.3.1\"\n";
            out << "ninja = \"1.13.2\"\n";
        }

        auto result = config::load_project_toolchain();
        check(result.size() == 3, "backward compat: 3 tools");
        check(result.at("llvm") == "22.1.2", "backward compat: llvm");
        check(result.at("cmake") == "4.3.1", "backward compat: cmake");
        check(result.at("ninja") == "1.13.2", "backward compat: ninja");

        auto full = config::load_full_project_config();
        check(full.common.size() == 3, "backward compat full: 3 common tools");
        check(full.platforms.empty(), "backward compat full: no platforms");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        std::filesystem::remove_all(tmp);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
    std::filesystem::remove_all(tmp);
}

void test_platform_name() {
    auto plat = registry::platform_name();
    // Should be one of the valid platforms
    check(config::is_valid_platform(plat), "platform_name returns valid platform");
}

int main() {
    test_config_path();
    test_set_and_load_defaults();
    test_project_config();
    test_platform_config();
    test_platform_override();
    test_write_project_config_roundtrip();
    test_backward_compatibility();
    test_platform_name();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_config: all tests passed");
    return 0;
}
