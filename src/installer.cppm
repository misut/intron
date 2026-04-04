export module installer;
import std;
export import registry;

export namespace installer {

std::filesystem::path intron_home() {
    auto home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }
    auto path = std::filesystem::path{home} / ".intron";
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path toolchain_path(std::string_view tool, std::string_view version) {
    return intron_home() / "toolchains" / tool / version;
}

namespace detail {

constexpr bool is_windows =
#ifdef _WIN32
    true;
#else
    false;
#endif

int run(std::string const& cmd) {
    return std::system(cmd.c_str());
}

std::string capture(std::string const& cmd) {
    auto tmp = std::filesystem::temp_directory_path() / "intron_capture.tmp";
    std::string full_cmd;
    if constexpr (is_windows) {
        full_cmd = std::format("{} > \"{}\" 2>nul", cmd, tmp.string());
    } else {
        full_cmd = std::format("{} > '{}' 2>/dev/null", cmd, tmp.string());
    }
    if (std::system(full_cmd.c_str()) != 0) return {};
    auto in = std::ifstream{tmp};
    auto result = std::string{
        std::istreambuf_iterator<char>{in},
        std::istreambuf_iterator<char>{}};
    std::filesystem::remove(tmp);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool check_command(std::string_view name) {
    if constexpr (is_windows) {
        return run(std::format("where {} >nul 2>nul", name)) == 0;
    } else {
        return run(std::format("command -v '{}' >/dev/null 2>&1", name)) == 0;
    }
}

std::string sha256_command(std::filesystem::path const& file) {
    if constexpr (is_windows) {
        return std::format("certutil -hashfile \"{}\" SHA256", file.string());
    } else {
        return std::format("shasum -a 256 '{}'", file.string());
    }
}

std::string parse_sha256(std::string const& output) {
    if constexpr (is_windows) {
        // certutil output: 1st line header, 2nd line hash, 3rd line footer
        auto first_nl = output.find('\n');
        if (first_nl == std::string::npos) return {};
        auto start = first_nl + 1;
        auto end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        auto hash = output.substr(start, end - start);
        // Remove spaces
        std::erase(hash, ' ');
        std::erase(hash, '\r');
        // Convert to lowercase
        for (auto& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return hash;
    } else {
        return output.substr(0, output.find(' '));
    }
}

} // namespace detail

bool install(registry::ToolInfo const& info) {
    auto dest = toolchain_path(info.name, info.version);

    // Already installed
    if (std::filesystem::exists(dest) && !std::filesystem::is_empty(dest)) {
        std::println("{} {} is already installed", info.name, info.version);
        return true;
    }

    // Preflight check for system tools
    if (!detail::check_command("curl")) {
        std::println(std::cerr, "error: curl is required but not found");
        return false;
    }
    if ((info.archive_type == "tar.xz" || info.archive_type == "tar.gz")
        && !detail::check_command("tar")) {
        std::println(std::cerr, "error: tar is required but not found");
        return false;
    }
    if constexpr (!detail::is_windows) {
        if (info.archive_type == "zip" && !detail::check_command("unzip")) {
            std::println(std::cerr, "error: unzip is required but not found");
            return false;
        }
    }

    auto downloads = intron_home() / "downloads";
    std::filesystem::create_directories(downloads);

    // Extract archive filename from URL
    auto url_sv = std::string_view{info.url};
    auto slash = url_sv.rfind('/');
    auto archive_name = std::string{url_sv.substr(slash + 1)};
    auto archive_path = downloads / archive_name;

    // Cleanup guard on failure
    bool success = false;
    auto cleanup = [&] {
        if (!success) {
            std::filesystem::remove_all(dest);
        }
    };

    // Download (reuse cached archive if available)
    if (std::filesystem::exists(archive_path)) {
        std::println("Using cached archive for {} {}...", info.name, info.version);
    } else {
        std::println("Downloading {} {}...", info.name, info.version);
        std::string curl_cmd;
        if constexpr (detail::is_windows) {
            curl_cmd = std::format("curl -fSL --compressed -o \"{}\" \"{}\"",
                archive_path.string(), info.url);
        } else {
            curl_cmd = std::format("curl -fSL --compressed -o '{}' '{}'",
                archive_path.string(), info.url);
        }
        if (detail::run(curl_cmd) != 0) {
            std::println(std::cerr, "error: download failed");
            std::filesystem::remove(archive_path);
            cleanup();
            return false;
        }
    }

    // Checksum verification
    if (!info.checksum_url.empty()) {
        std::println("Verifying checksum...");
        auto checksum_file = downloads / "checksums.txt";
        std::string dl_cmd;
        if constexpr (detail::is_windows) {
            dl_cmd = std::format("curl -fsSL --compressed -o \"{}\" \"{}\"",
                checksum_file.string(), info.checksum_url);
        } else {
            dl_cmd = std::format("curl -fsSL --compressed -o '{}' '{}'",
                checksum_file.string(), info.checksum_url);
        }
        if (detail::run(dl_cmd) == 0) {
            auto raw = detail::capture(detail::sha256_command(archive_path));
            auto actual_hash = detail::parse_sha256(raw);

            auto in = std::ifstream{checksum_file};
            std::string line;
            bool verified = false;
            while (std::getline(in, line)) {
                if (line.contains(archive_name)) {
                    auto expected_hash = line.substr(0, line.find(' '));
                    if (actual_hash == expected_hash) {
                        std::println("Checksum OK");
                        verified = true;
                    } else {
                        std::println(std::cerr,
                            "error: checksum mismatch\n  expected: {}\n  actual:   {}",
                            expected_hash, actual_hash);
                        std::filesystem::remove(checksum_file);
                        cleanup();
                        return false;
                    }
                    break;
                }
            }
            std::filesystem::remove(checksum_file);
            if (!verified) {
                std::println("warning: checksum entry not found, skipping verification");
            }
        } else {
            std::println("warning: could not download checksum file, skipping verification");
        }
    }

    // Extract to staging directory (atomic install: staging → rename)
    auto staging = intron_home() / "staging" / std::format("{}-{}", info.name, info.version);
    std::filesystem::create_directories(staging);

    std::println("Extracting...");
    int extract_status = 0;
    if (info.archive_type == "tar.xz" || info.archive_type == "tar.gz") {
        int depth = 0;
        if (!info.strip_prefix.empty()) {
            depth = 1;
            for (auto c : info.strip_prefix) {
                if (c == '/') ++depth;
            }
        }
        if constexpr (detail::is_windows) {
            extract_status = detail::run(
                std::format("tar xf \"{}\" --strip-components={} -C \"{}\"",
                    archive_path.string(), depth, staging.string()));
        } else {
            extract_status = detail::run(
                std::format("tar xf '{}' --strip-components={} -C '{}'",
                    archive_path.string(), depth, staging.string()));
        }
    } else if (info.archive_type == "zip") {
        if constexpr (detail::is_windows) {
            // Windows: tar can handle zip files too
            extract_status = detail::run(
                std::format("tar xf \"{}\" -C \"{}\"",
                    archive_path.string(), staging.string()));
        } else {
            extract_status = detail::run(
                std::format("unzip -qo '{}' -d '{}'",
                    archive_path.string(), staging.string()));
        }
    } else {
        std::println(std::cerr, "error: unknown archive type: {}", info.archive_type);
        std::filesystem::remove_all(staging);
        cleanup();
        return false;
    }

    if (extract_status != 0) {
        std::println(std::cerr, "error: extraction failed");
        std::filesystem::remove_all(staging);
        cleanup();
        return false;
    }

    // Atomically place at final destination
    std::filesystem::create_directories(dest.parent_path());
    std::filesystem::rename(staging, dest);

    // Generate clang config + wrapper scripts after LLVM install (macOS only)
    if (info.name == "llvm" && registry::detect_platform().os == registry::OS::macOS) {
        auto clang = dest / "bin" / "clang";
        auto target = detail::capture(std::format("'{}' -dumpmachine", clang.string()));
        auto sdk_path = detail::capture("xcrun --show-sdk-path");
        if (!target.empty() && !sdk_path.empty()) {
            auto cfg_target = target;
            auto darwin_pos = cfg_target.find("darwin");
            if (darwin_pos != std::string::npos) {
                auto ver_start = darwin_pos + 6;
                auto dot = cfg_target.find('.', ver_start);
                if (dot != std::string::npos) {
                    cfg_target = cfg_target.substr(0, dot);
                }
            }

            auto cfg_dir = dest / "etc" / "clang";
            std::filesystem::create_directories(cfg_dir);
            auto cfg_file = cfg_dir / std::format("{}.cfg", cfg_target);
            {
                auto out = std::ofstream{cfg_file};
                if (out) {
                    out << std::format("-isysroot {}\n", sdk_path);
                    out << "-stdlib=libc++\n";
                    out << "-lc++\n";
                }
            }

            auto bin_dir = dest / "bin";
            auto cfg_flag = std::format("--config-system-dir={}", cfg_dir.string());
            for (auto name : {"clang", "clang++"}) {
                auto orig = bin_dir / name;
                auto backup = bin_dir / std::format("{}.orig", name);
                if (std::filesystem::exists(orig) && !std::filesystem::exists(backup)) {
                    std::filesystem::rename(orig, backup);
                    auto out = std::ofstream{orig};
                    if (out) {
                        out << "#!/bin/sh\n";
                        out << std::format("exec \"{}\" {} \"$@\"\n",
                            backup.string(), cfg_flag);
                    }
                    std::filesystem::permissions(orig,
                        std::filesystem::perms::owner_exec |
                        std::filesystem::perms::group_exec |
                        std::filesystem::perms::others_exec,
                        std::filesystem::perm_options::add);
                }
            }

            std::println("Generated clang config: {}", cfg_file.string());
        }
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

std::optional<std::string> latest_version(std::string_view tool) {
    auto api_url = registry::latest_release_api(tool);
    std::string curl_cmd;
    if constexpr (detail::is_windows) {
        curl_cmd = std::format("curl -fsSL \"{}\"", api_url);
    } else {
        curl_cmd = std::format("curl -fsSL '{}'", api_url);
    }
    auto json = detail::capture(curl_cmd);
    if (json.empty()) return std::nullopt;

    auto pos = json.find("\"tag_name\"");
    if (pos == std::string::npos) return std::nullopt;
    auto colon = json.find(':', pos);
    auto quote1 = json.find('"', colon + 1);
    auto quote2 = json.find('"', quote1 + 1);
    if (quote1 == std::string::npos || quote2 == std::string::npos) return std::nullopt;

    auto tag = json.substr(quote1 + 1, quote2 - quote1 - 1);

    if (tag.starts_with("v")) {
        return tag.substr(1);
    }
    if (auto dash = tag.rfind('-'); dash != std::string::npos) {
        return tag.substr(dash + 1);
    }
    return tag;
}

} // namespace installer
