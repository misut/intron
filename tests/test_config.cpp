import std;
import config;

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
    // 임시 환경에서 set_default → load_defaults round-trip 테스트
    // 기존 config를 백업
    auto path = config::config_path();
    auto backup = path;
    backup += ".test_bak";
    bool had_config = std::filesystem::exists(path);
    if (had_config) {
        std::filesystem::copy_file(path, backup);
    }

    // 테스트 값 설정
    config::set_default("test-tool", "1.2.3");
    config::set_default("another", "4.5.6");

    // 읽기 검증
    auto defaults = config::load_defaults();
    check(defaults.contains("test-tool"), "defaults contains test-tool");
    check(defaults.at("test-tool") == "1.2.3", "test-tool version");
    check(defaults.contains("another"), "defaults contains another");
    check(defaults.at("another") == "4.5.6", "another version");

    // get_default 검증
    auto ver = config::get_default("test-tool");
    check(ver.has_value(), "get_default returns value");
    check(*ver == "1.2.3", "get_default correct version");

    auto missing = config::get_default("nonexistent");
    check(!missing.has_value(), "get_default returns nullopt for missing");

    // 원복
    if (had_config) {
        std::filesystem::rename(backup, path);
    } else {
        std::filesystem::remove(path);
    }
}

void test_project_config() {
    // 임시 .intron.toml 생성하여 프로젝트 설정 테스트
    auto project_cfg = std::filesystem::current_path() / ".intron.toml";
    {
        auto out = std::ofstream{project_cfg};
        out << "[toolchain]\n";
        out << "llvm = \"19.0.0\"\n";
    }

    auto result = config::load_project_toolchain();
    check(result.contains("llvm"), "project config contains llvm");
    check(result.at("llvm") == "19.0.0", "project config llvm version");

    std::filesystem::remove(project_cfg);
}

int main() {
    test_config_path();
    test_set_and_load_defaults();
    test_project_config();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_config: all tests passed");
    return 0;
}
