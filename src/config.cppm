export module config;
import std;
import cppx.env.system;
import intron.domain;
import registry;
import toml;

namespace {

auto read_text_file(std::filesystem::path const& path) -> std::string {
    auto in = std::ifstream{path, std::ios::binary};
    if (!in) {
        throw std::runtime_error(std::format("cannot read config file: {}", path.string()));
    }
    return std::string{
        std::istreambuf_iterator<char>{in},
        std::istreambuf_iterator<char>{},
    };
}

auto parse_document_from_table(toml::Table const& table, std::string_view section)
    -> intron::ConfigDocument
{
    auto document = intron::ConfigDocument{};
    auto key = std::string{section};
    if (!table.contains(key)) {
        return document;
    }

    auto const& root = table.at(key).as_table();
    for (auto const& [name, value] : root) {
        if (value.is_string()) {
            document.common[name] = value.as_string();
            continue;
        }
        if (!value.is_table()) {
            continue;
        }
        auto& platform_values = document.platforms[name];
        for (auto const& [tool, tool_value] : value.as_table()) {
            if (tool_value.is_string()) {
                platform_values[tool] = tool_value.as_string();
            }
        }
    }
    return document;
}

auto parse_document_string(std::string_view input, std::string_view section)
    -> intron::ConfigDocument
{
    return parse_document_from_table(toml::parse(input), section);
}

auto serialize_document_string(intron::ConfigDocument const& document,
                               std::string_view section) -> std::string
{
    auto rendered = std::string{};
    rendered += std::format("[{}]\n", section);
    for (auto const& [tool, version] : document.common) {
        rendered += std::format("{} = \"{}\"\n", tool, version);
    }
    for (auto const& [platform, values] : document.platforms) {
        if (values.empty()) {
            continue;
        }
        rendered += std::format("\n[{}.{}]\n", section, platform);
        for (auto const& [tool, version] : values) {
            rendered += std::format("{} = \"{}\"\n", tool, version);
        }
    }
    return rendered;
}

auto load_document(std::filesystem::path const& path, std::string_view section)
    -> intron::ConfigDocument
{
    if (!std::filesystem::exists(path)) {
        return {};
    }
    return parse_document_string(read_text_file(path), section);
}

auto write_document(std::filesystem::path const& path,
                    std::string_view section,
                    intron::ConfigDocument const& document) -> void
{
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    auto out = std::ofstream{path};
    if (!out) {
        throw std::runtime_error(std::format("cannot write config file: {}", path.string()));
    }
    out << serialize_document_string(document, section);
}

} // namespace

export namespace config {

std::filesystem::path config_path() {
    auto home = cppx::env::system::home_dir();
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }
    return *home / ".intron" / "config.toml";
}

using ToolchainConfig = intron::ConfigDocument;
using DefaultsConfig = intron::ConfigDocument;

bool is_valid_platform(std::string_view name) {
    return intron::is_valid_platform(name);
}

auto parse_config_document(std::string_view input, std::string_view section)
    -> intron::ConfigDocument
{
    return parse_document_string(input, section);
}

auto serialize_config_document(intron::ConfigDocument const& document,
                               std::string_view section) -> std::string
{
    return serialize_document_string(document, section);
}

auto merge_document(intron::ConfigDocument const& document, std::string_view platform)
    -> std::map<std::string, std::string>
{
    return intron::merge_config_document(document, platform);
}

std::map<std::string, std::string> load_toml_section(
    std::filesystem::path const& path,
    std::string_view section)
{
    return load_document(path, section).common;
}

std::map<std::string, std::string> load_section_with_platform(
    std::filesystem::path const& path,
    std::string_view section)
{
    return merge_document(load_document(path, section), registry::platform_name());
}

std::map<std::string, std::string> load_defaults() {
    return load_section_with_platform(config_path(), "defaults");
}

std::optional<std::filesystem::path> find_project_config() {
    auto dir = std::filesystem::current_path();
    while (true) {
        auto candidate = dir / ".intron.toml";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        auto parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return std::nullopt;
}

std::map<std::string, std::string> load_project_toolchain() {
    auto path = find_project_config();
    if (!path) {
        return {};
    }
    return load_section_with_platform(*path, "toolchain");
}

std::optional<std::string> get_default(std::string_view tool) {
    auto project = load_project_toolchain();
    if (auto it = project.find(std::string{tool}); it != project.end()) {
        return it->second;
    }

    auto defaults = load_defaults();
    if (auto it = defaults.find(std::string{tool}); it != defaults.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::map<std::string, std::string> load_effective_defaults() {
    auto defaults = load_defaults();
    for (auto const& [tool, version] : load_project_toolchain()) {
        defaults[tool] = version;
    }
    return defaults;
}

ToolchainConfig load_full_project_config() {
    auto path = find_project_config();
    if (!path) {
        return {};
    }
    return load_document(*path, "toolchain");
}

void write_project_config(ToolchainConfig const& config) {
    write_document(".intron.toml", "toolchain", config);
}

DefaultsConfig load_full_defaults() {
    return load_document(config_path(), "defaults");
}

void set_default(std::string_view tool,
                 std::string_view version,
                 std::string_view platform = {})
{
    auto config = load_full_defaults();
    if (platform.empty()) {
        config.common[std::string{tool}] = std::string{version};
    } else {
        config.platforms[std::string{platform}][std::string{tool}] = std::string{version};
    }
    write_document(config_path(), "defaults", config);
}

} // namespace config
