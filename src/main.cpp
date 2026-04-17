import std;
import intron.app;
import intron.domain;
import intron.edge;
import installer;
import net;
import registry;

namespace {

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif

constexpr auto intron_version = EXON_PKG_VERSION;

auto user_agent() -> std::string {
    return std::format("intron/{}", intron_version);
}

auto error_result(std::string_view message) -> intron::CommandResult {
    auto result = intron::CommandResult{
        .exit_code = 1,
    };
    result.add_stderr(std::format("error: {}", message));
    return result;
}

auto self_update_result(std::string_view self_path) -> intron::CommandResult {
    auto result = intron::CommandResult{};

    result.add_stdout("Checking for updates...");
    auto latest = installer::latest_version("intron");
    if (!latest) {
        result.exit_code = 1;
        result.add_stderr("error: could not check latest version");
        return result;
    }
    if (*latest == intron_version) {
        result.add_stdout(std::format("intron {} is already up to date", intron_version));
        return result;
    }

    result.add_stdout(std::format(
        "Updating intron {} -> {}...",
        intron_version,
        *latest));

    auto tmp = std::filesystem::temp_directory_path() / "intron-update";
    std::filesystem::create_directories(tmp);
    auto triple = registry::platform_triple();

#ifdef _WIN32
    constexpr auto is_windows = true;
#else
    constexpr auto is_windows = false;
#endif

    auto archive = tmp / intron::self_update_archive_name(is_windows);
    auto url = intron::self_update_download_url(*latest, triple, is_windows);
    auto dl = net::download_file(url, archive, net::user_agent_headers(user_agent()));
    if (!dl) {
        result.exit_code = 1;
        result.add_stderr(std::format("error: {}", dl.error()));
        std::filesystem::remove_all(tmp);
        return result;
    }

#ifdef _WIN32
    std::string tar = "tar";
    if (auto const* sr = std::getenv("SystemRoot")) {
        auto sys_tar = std::filesystem::path{sr} / "System32" / "tar.exe";
        if (std::filesystem::exists(sys_tar)) {
            tar = std::format("\"{}\"", sys_tar.string());
        }
    }
    auto extract_cmd = std::format(
        "\"{} xf \"{}\" -C \"{}\"\"",
        tar,
        archive.string(),
        tmp.string());
#else
    auto extract_cmd = std::format("tar xzf '{}' -C '{}'", archive.string(), tmp.string());
#endif

    if (std::system(extract_cmd.c_str()) != 0) {
        result.exit_code = 1;
        result.add_stderr("error: extraction failed");
        std::filesystem::remove_all(tmp);
        return result;
    }

    auto target = std::filesystem::canonical(self_path);
#ifdef _WIN32
    auto new_binary = tmp / "intron.exe";
    auto old_binary = target;
    old_binary += ".old";
    std::error_code ec;
    std::filesystem::remove(old_binary, ec);
    std::filesystem::rename(target, old_binary);
    std::filesystem::rename(new_binary, target);
    std::filesystem::remove(old_binary, ec);
#else
    auto new_binary = tmp / "intron";
    std::filesystem::rename(new_binary, target);
#endif

    std::error_code tmp_ec;
    std::filesystem::remove_all(tmp, tmp_ec);

    result.add_stdout(std::format("Updated intron to {}", *latest));
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    auto ports = intron::edge::make_runtime_ports();

    try {
        auto request = intron::app::parse_command_request(argc, argv);
        if (!request) {
            intron::edge::render(request.error(), ports);
            return request.error().exit_code;
        }

        auto result = intron::CommandResult{};
        if (request->command == intron::CommandKind::SelfUpdate) {
            result = self_update_result(request->self_path);
        } else {
            result = intron::app::run_command(*request, ports);
        }

        intron::edge::render(result, ports);
        return result.exit_code;
    } catch (std::exception const& e) {
        auto result = error_result(e.what());
        intron::edge::render(result, ports);
        return result.exit_code;
    }
}
