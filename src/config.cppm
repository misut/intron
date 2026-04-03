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

std::optional<std::string> get_default(std::string_view tool) {
    auto defaults = load_defaults();
    auto it = defaults.find(std::string{tool});
    if (it != defaults.end()) {
        return it->second;
    }
    return std::nullopt;
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
