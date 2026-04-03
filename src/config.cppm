export module config;
import std;
import toml;

export namespace config {

std::filesystem::path config_path() {
    auto home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }
    return std::filesystem::path{home} / ".intron" / "config.toml";
}

std::map<std::string, std::string> load_defaults() {
    std::map<std::string, std::string> defaults;
    auto path = config_path();
    if (!std::filesystem::exists(path)) {
        return defaults;
    }

    auto table = toml::parse_file(path.string());
    if (table.contains("defaults")) {
        auto const& defs = table.at("defaults").as_table();
        for (auto const& [key, value] : defs) {
            if (value.is_string()) {
                defaults[key] = value.as_string();
            }
        }
    }
    return defaults;
}

// 현재 디렉토리부터 상위로 .intron.toml 탐색
std::optional<std::filesystem::path> find_project_config() {
    auto dir = std::filesystem::current_path();
    while (true) {
        auto candidate = dir / ".intron.toml";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return std::nullopt;
}

// 프로젝트 설정의 [toolchain] 섹션 로드
std::map<std::string, std::string> load_project_toolchain() {
    std::map<std::string, std::string> result;
    auto path = find_project_config();
    if (!path) return result;

    auto table = toml::parse_file(path->string());
    if (table.contains("toolchain")) {
        auto const& tc = table.at("toolchain").as_table();
        for (auto const& [key, value] : tc) {
            if (value.is_string()) {
                result[key] = value.as_string();
            }
        }
    }
    return result;
}

// 프로젝트 설정 > 글로벌 defaults 순으로 조회
std::optional<std::string> get_default(std::string_view tool) {
    // 프로젝트 설정 우선
    auto project = load_project_toolchain();
    auto it = project.find(std::string{tool});
    if (it != project.end()) {
        return it->second;
    }
    // 글로벌 defaults
    auto defaults = load_defaults();
    auto it2 = defaults.find(std::string{tool});
    if (it2 != defaults.end()) {
        return it2->second;
    }
    return std::nullopt;
}

// 프로젝트 + 글로벌 병합 (프로젝트 우선)
std::map<std::string, std::string> load_effective_defaults() {
    auto defaults = load_defaults();
    auto project = load_project_toolchain();
    for (auto const& [k, v] : project) {
        defaults[k] = v;
    }
    return defaults;
}

void set_default(std::string_view tool, std::string_view version) {
    auto defaults = load_defaults();
    defaults[std::string{tool}] = std::string{version};

    auto path = config_path();
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream{path};
    if (!out) {
        throw std::runtime_error(
            std::format("cannot write config file: {}", path.string()));
    }
    out << "[defaults]\n";
    for (auto const& [k, v] : defaults) {
        out << std::format("{} = \"{}\"\n", k, v);
    }
}

} // namespace config
