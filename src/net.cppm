export module net;
import std;
import cppx.http;
import cppx.http.client;
import cppx.http.system;

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
        auto transient =
            err == cppx::http::http_error::response_parse_failed ||
            err == cppx::http::http_error::connection_failed ||
            err == cppx::http::http_error::tls_failed ||
            err == cppx::http::http_error::timeout;
        if (!transient || attempt == 3)
            break;

        auto partial = path;
        partial += ".part";
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(partial, ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
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
