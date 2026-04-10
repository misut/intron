export module config;
import std;
import toml;

export namespace config {

std::filesystem::path config_path() {
    auto home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }
    return std::filesystem::path{home} / ".intron" / "config.toml";
}

std::map<std::string, std::string> load_toml_section(
    std::filesystem::path const& path, std::string_view section) {
    std::map<std::string, std::string> result;
    if (!std::filesystem::exists(path)) return result;
    auto table = toml::parse_file(path.string());
    auto sec_key = std::string{section};
    if (table.contains(sec_key)) {
        auto const& sec = table.at(sec_key).as_table();
        for (auto const& [key, value] : sec)
            if (value.is_string()) result[key] = value.as_string();
    }
    return result;
}

std::map<std::string, std::string> load_defaults() {
    return load_toml_section(config_path(), "defaults");
}

// Search for .intron.toml from current directory upward
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

std::map<std::string, std::string> load_project_toolchain() {
    auto path = find_project_config();
    if (!path) return {};
    return load_toml_section(*path, "toolchain");
}

// Look up: project config > global defaults
std::optional<std::string> get_default(std::string_view tool) {
    // Project config takes priority
    auto project = load_project_toolchain();
    auto it = project.find(std::string{tool});
    if (it != project.end()) {
        return it->second;
    }
    // Global defaults
    auto defaults = load_defaults();
    auto it2 = defaults.find(std::string{tool});
    if (it2 != defaults.end()) {
        return it2->second;
    }
    return std::nullopt;
}

// Merge project + global defaults (project takes priority)
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

void write_project_config(std::map<std::string, std::string> const& toolchain) {
    auto out = std::ofstream{".intron.toml"};
    if (!out)
        throw std::runtime_error("cannot write .intron.toml");
    out << "[toolchain]\n";
    for (auto const& [k, v] : toolchain)
        out << std::format("{} = \"{}\"\n", k, v);
}

} // namespace config
