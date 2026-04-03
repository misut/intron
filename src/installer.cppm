export module installer;
import std;
export import registry;

export namespace installer {

std::filesystem::path intron_home() {
    auto home = std::getenv("HOME");
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

int run(std::string const& cmd) {
    return std::system(cmd.c_str());
}

// 디렉토리 내의 모든 항목을 대상 경로로 이동
void move_contents(std::filesystem::path const& src, std::filesystem::path const& dst) {
    std::filesystem::create_directories(dst);
    for (auto const& entry : std::filesystem::directory_iterator{src}) {
        auto target = dst / entry.path().filename();
        std::filesystem::rename(entry.path(), target);
    }
}

} // namespace detail

bool install(registry::ToolInfo const& info) {
    auto dest = toolchain_path(info.name, info.version);

    // 이미 설치됨
    if (std::filesystem::exists(dest) && !std::filesystem::is_empty(dest)) {
        std::println("{} {} is already installed", info.name, info.version);
        return true;
    }

    auto downloads = intron_home() / "downloads";
    std::filesystem::create_directories(downloads);

    // 아카이브 파일명 추출
    auto url_sv = std::string_view{info.url};
    auto slash = url_sv.rfind('/');
    auto archive_name = std::string{url_sv.substr(slash + 1)};
    auto archive_path = downloads / archive_name;

    // 다운로드
    std::println("Downloading {} {}...", info.name, info.version);
    auto curl_cmd = std::format("curl -fSL -o '{}' '{}'", archive_path.string(), info.url);
    if (detail::run(curl_cmd) != 0) {
        std::println(std::cerr, "error: download failed");
        return false;
    }

    // staging 디렉토리에 압축 해제
    auto staging = intron_home() / "staging";
    std::filesystem::create_directories(staging);

    std::println("Extracting...");
    int extract_status = 0;
    if (info.archive_type == "tar.xz" || info.archive_type == "tar.gz") {
        extract_status = detail::run(
            std::format("tar xf '{}' -C '{}'", archive_path.string(), staging.string()));
    } else if (info.archive_type == "zip") {
        extract_status = detail::run(
            std::format("unzip -qo '{}' -d '{}'", archive_path.string(), staging.string()));
    } else {
        std::println(std::cerr, "error: unknown archive type: {}", info.archive_type);
        return false;
    }

    if (extract_status != 0) {
        std::println(std::cerr, "error: extraction failed");
        std::filesystem::remove_all(staging);
        return false;
    }

    // strip_prefix에 따라 내용물을 최종 경로로 이동
    std::filesystem::create_directories(dest);

    if (info.strip_prefix.empty()) {
        // Ninja처럼 아카이브 루트에 바이너리가 직접 있는 경우
        detail::move_contents(staging, dest);
    } else {
        auto src = staging / info.strip_prefix;
        if (!std::filesystem::exists(src)) {
            std::println(std::cerr, "error: expected directory '{}' not found in archive",
                info.strip_prefix);
            std::filesystem::remove_all(staging);
            std::filesystem::remove_all(dest);
            return false;
        }
        detail::move_contents(src, dest);
    }

    // 정리
    std::filesystem::remove_all(staging);
    std::filesystem::remove(archive_path);

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
    // ninja는 bin/ 서브디렉토리가 없음
    auto path = (tool == "ninja")
        ? base / binary
        : base / "bin" / binary;

    if (std::filesystem::exists(path)) {
        return path;
    }
    return std::nullopt;
}

} // namespace installer
