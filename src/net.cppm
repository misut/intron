export module net;
import std;
import cppx.http;
import cppx.http.client;
import cppx.http.system;

namespace {

constexpr bool is_windows =
#ifdef _WIN32
    true;
#else
    false;
#endif

auto powershell_quote(std::string_view s) -> std::string {
    auto out = std::string{"'"};
    for (auto c : s) {
        if (c == '\'')
            out += "''";
        else
            out += c;
    }
    out += '\'';
    return out;
}

auto download_file_windows(std::string_view url, std::filesystem::path const& path,
                           cppx::http::headers const& extra)
    -> std::expected<void, std::string>
{
    if constexpr (!is_windows) {
        return std::unexpected("windows fallback unavailable");
    } else {
        std::filesystem::create_directories(path.parent_path());

        auto partial = path;
        partial += ".part";
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(partial, ec);

        auto script = std::string{
            "$ErrorActionPreference='Stop';"
            "$ProgressPreference='SilentlyContinue';"
            "$headers=@{};"
        };
        for (auto const& [name, value] : extra) {
            script += std::format(
                "$headers[{}]={};",
                powershell_quote(name),
                powershell_quote(value));
        }
        script += std::format(
            "Invoke-WebRequest -UseBasicParsing -Uri {} -Headers $headers -OutFile {} "
            "-MaximumRedirection 10;",
            powershell_quote(url),
            powershell_quote(path.string()));

        auto cmd = std::format(
            "powershell -NoLogo -NoProfile -NonInteractive "
            "-ExecutionPolicy Bypass -Command \"{}\"",
            script);
        if (std::system(cmd.c_str()) != 0)
            return std::unexpected("download failed: powershell fallback failed");
        return {};
    }
}

} // namespace

export namespace net {

auto user_agent_headers(std::string_view user_agent) -> cppx::http::headers {
    cppx::http::headers hdrs;
    hdrs.set("user-agent", user_agent);
    return hdrs;
}

auto github_api_headers(std::string_view user_agent) -> cppx::http::headers {
    auto hdrs = user_agent_headers(user_agent);
    hdrs.set("accept", "application/vnd.github+json");
    return hdrs;
}

auto get_text(std::string_view url, cppx::http::headers extra = {})
    -> std::expected<std::string, std::string>
{
    auto resp = cppx::http::system::get(url, std::move(extra));
    if (!resp)
        return std::unexpected(std::format(
            "request failed: {}", cppx::http::to_string(resp.error())));
    if (!resp->stat.ok())
        return std::unexpected(std::format("HTTP {}", resp->stat.code));
    return resp->body_string();
}

auto download_file(std::string_view url, std::filesystem::path const& path,
                   cppx::http::headers extra = {})
    -> std::expected<void, std::string>
{
    auto last_error = std::string{};
    auto retryable = false;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        auto client = cppx::http::client<
            cppx::http::system::stream, cppx::http::system::tls>{};
        auto resp = client.download_to(url, path, extra);
        if (resp) {
            if (!resp->stat.ok())
                return std::unexpected(std::format("HTTP {}", resp->stat.code));
            return {};
        }

        auto err = resp.error();
        last_error = std::format(
            "download failed: {}", cppx::http::to_string(err));

        // GitHub-hosted archives can fail transiently on Windows runners.
        retryable =
            err == cppx::http::http_error::response_parse_failed ||
            err == cppx::http::http_error::connection_failed ||
            err == cppx::http::http_error::tls_failed ||
            err == cppx::http::http_error::timeout;
        if (!retryable || attempt == 3)
            break;

        auto partial = path;
        partial += ".part";
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(partial, ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
    }

    if constexpr (is_windows) {
        if (retryable) {
            auto fallback = download_file_windows(url, path, extra);
            if (fallback) return {};
            last_error = std::format("{}; {}", last_error, fallback.error());
        }
    }

    return std::unexpected(last_error);
}

auto latest_version_from_release_json(std::string_view json)
    -> std::optional<std::string>
{
    auto pos = json.find("\"tag_name\"");
    if (pos == std::string_view::npos) return std::nullopt;

    auto colon = json.find(':', pos);
    if (colon == std::string_view::npos) return std::nullopt;

    auto quote1 = json.find('"', colon + 1);
    auto quote2 = quote1 == std::string_view::npos
        ? std::string_view::npos
        : json.find('"', quote1 + 1);
    if (quote1 == std::string_view::npos || quote2 == std::string_view::npos)
        return std::nullopt;

    auto tag = std::string{json.substr(quote1 + 1, quote2 - quote1 - 1)};
    if (tag.starts_with("v"))
        return tag.substr(1);
    if (auto dash = tag.rfind('-'); dash != std::string::npos)
        return tag.substr(dash + 1);
    return tag;
}

} // namespace net
