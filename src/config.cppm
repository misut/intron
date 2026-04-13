export module config;
import std;
import cppx.env.system;
import registry;
import toml;

export namespace config {

std::filesystem::path config_path() {
    auto home = cppx::env::system::home_dir();
    if (!home)
        throw std::runtime_error("HOME environment variable not set");
    return *home / ".intron" / "config.toml";
}

constexpr std::array<std::string_view, 3> valid_platforms = {"linux", "macos", "windows"};

bool is_valid_platform(std::string_view name) {
    return std::ranges::find(valid_platforms, name) != valid_platforms.end();
}

// Load a TOML section as flat key-value pairs (ignores sub-tables)
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

// Load a TOML section with platform-specific merge:
// common entries + current platform's sub-table entries (platform overrides common)
std::map<std::string, std::string> load_section_with_platform(
    std::filesystem::path const& path, std::string_view section) {
    std::map<std::string, std::string> result;
    if (!std::filesystem::exists(path)) return result;
    auto table = toml::parse_file(path.string());
    auto sec_key = std::string{section};
    if (!table.contains(sec_key)) return result;

    auto const& sec = table.at(sec_key).as_table();

    // Common entries (string values only, sub-tables are skipped)
    for (auto const& [key, value] : sec)
        if (value.is_string()) result[key] = value.as_string();

    // Platform-specific entries (merge over common)
    auto plat = std::string{registry::platform_name()};
    if (sec.contains(plat)) {
        auto const& plat_table = sec.at(plat).as_table();
        for (auto const& [key, value] : plat_table)
            if (value.is_string()) result[key] = value.as_string();
    }

    return result;
}

std::map<std::string, std::string> load_defaults() {
    return load_section_with_platform(config_path(), "defaults");
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
    return load_section_with_platform(*path, "toolchain");
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

// Full project config with platform sections (not filtered by current platform)
struct ToolchainConfig {
    std::map<std::string, std::string> common;
    std::map<std::string, std::map<std::string, std::string>> platforms;
};

ToolchainConfig load_full_project_config() {
    ToolchainConfig config;
    auto path = find_project_config();
    if (!path) return config;
    if (!std::filesystem::exists(*path)) return config;

    auto table = toml::parse_file(path->string());
    if (!table.contains("toolchain")) return config;

    auto const& sec = table.at("toolchain").as_table();
    for (auto const& [key, value] : sec) {
        if (value.is_string()) {
            config.common[key] = value.as_string();
        } else if (value.is_table()) {
            auto& plat_map = config.platforms[key];
            for (auto const& [k, v] : value.as_table())
                if (v.is_string()) plat_map[k] = v.as_string();
        }
    }
    return config;
}

void write_project_config(ToolchainConfig const& config) {
    auto out = std::ofstream{".intron.toml"};
    if (!out)
        throw std::runtime_error("cannot write .intron.toml");

    out << "[toolchain]\n";
    for (auto const& [k, v] : config.common)
        out << std::format("{} = \"{}\"\n", k, v);

    for (auto const& [plat, tools] : config.platforms) {
        if (tools.empty()) continue;
        out << std::format("\n[toolchain.{}]\n", plat);
        for (auto const& [k, v] : tools)
            out << std::format("{} = \"{}\"\n", k, v);
    }
}

// Full defaults config with platform sections
struct DefaultsConfig {
    std::map<std::string, std::string> common;
    std::map<std::string, std::map<std::string, std::string>> platforms;
};

DefaultsConfig load_full_defaults() {
    DefaultsConfig config;
    auto path = config_path();
    if (!std::filesystem::exists(path)) return config;

    auto table = toml::parse_file(path.string());
    if (!table.contains("defaults")) return config;

    auto const& sec = table.at("defaults").as_table();
    for (auto const& [key, value] : sec) {
        if (value.is_string()) {
            config.common[key] = value.as_string();
        } else if (value.is_table()) {
            auto& plat_map = config.platforms[key];
            for (auto const& [k, v] : value.as_table())
                if (v.is_string()) plat_map[k] = v.as_string();
        }
    }
    return config;
}

void set_default(std::string_view tool, std::string_view version,
                 std::string_view platform = {}) {
    auto config = load_full_defaults();

    if (platform.empty()) {
        config.common[std::string{tool}] = std::string{version};
    } else {
        config.platforms[std::string{platform}][std::string{tool}] = std::string{version};
    }

    auto path = config_path();
    std::filesystem::create_directories(path.parent_path());
    auto out = std::ofstream{path};
    if (!out) {
        throw std::runtime_error(
            std::format("cannot write config file: {}", path.string()));
    }

    out << "[defaults]\n";
    for (auto const& [k, v] : config.common)
        out << std::format("{} = \"{}\"\n", k, v);

    for (auto const& [plat, tools] : config.platforms) {
        if (tools.empty()) continue;
        out << std::format("\n[defaults.{}]\n", plat);
        for (auto const& [k, v] : tools)
            out << std::format("{} = \"{}\"\n", k, v);
    }
}

} // namespace config
