export module net;
import std;
import cppx.http;
import cppx.http.transfer;
import cppx.http.transfer.system;

export namespace net {

enum class Backend { Auto, Cppx, Shell };

auto selected_backend_from_string(std::optional<std::string_view> value) -> Backend;
auto selected_backend_from_env() -> Backend;

auto user_agent_headers(std::string_view user_agent) -> cppx::http::headers;
auto github_api_headers(std::string_view user_agent) -> cppx::http::headers;
auto get_text(std::string_view url, cppx::http::headers extra = {})
    -> std::expected<std::string, std::string>;
auto download_file(std::string_view url, std::filesystem::path const& path,
                   cppx::http::headers extra = {})
    -> std::expected<void, std::string>;
auto latest_version_from_release_json(std::string_view json)
    -> std::optional<std::string>;

} // namespace net

namespace {

auto ascii_lower(std::string_view text) -> std::string {
    auto lowered = std::string{text};
    for (auto& ch : lowered)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lowered;
}

auto selected_backend_from_string_impl(std::optional<std::string_view> raw) -> net::Backend {
    if (!raw || raw->empty())
        return net::Backend::Auto;

    auto value = ascii_lower(*raw);
    if (value == "cppx")
        return net::Backend::Cppx;
    if (value == "shell")
        return net::Backend::Shell;
    return net::Backend::Auto;
}

auto selected_backend_from_env_impl() -> net::Backend {
    auto const* raw = std::getenv("INTRON_NET_BACKEND");
    if (!raw || !*raw)
        return net::Backend::Auto;
    return selected_backend_from_string_impl(raw);
}

auto transfer_backend(net::Backend backend) -> cppx::http::transfer::TransferBackend {
    switch (backend) {
    case net::Backend::Cppx:
        return cppx::http::transfer::TransferBackend::CppxHttp;
    case net::Backend::Shell:
        return cppx::http::transfer::TransferBackend::Shell;
    case net::Backend::Auto:
        return cppx::http::transfer::TransferBackend::Auto;
    }
    return cppx::http::transfer::TransferBackend::Auto;
}

auto emit_warning(std::optional<std::string> const& warning) -> void {
    if (warning && !warning->empty())
        std::println(std::cerr, "{}", *warning);
}

} // namespace

export namespace net {

auto selected_backend_from_string(std::optional<std::string_view> value) -> Backend {
    return selected_backend_from_string_impl(value);
}

auto selected_backend_from_env() -> Backend {
    return selected_backend_from_env_impl();
}

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

auto get_text(std::string_view url, cppx::http::headers extra)
    -> std::expected<std::string, std::string>
{
    auto result = cppx::http::transfer::system::get_text(
        url,
        {
            .backend = transfer_backend(selected_backend_from_env_impl()),
            .headers = std::move(extra),
        });
    if (!result)
        return std::unexpected(result.error().message);

    emit_warning(result->warning);
    return std::move(result->text);
}

auto download_file(std::string_view url,
                   std::filesystem::path const& path,
                   cppx::http::headers extra)
    -> std::expected<void, std::string>
{
    auto result = cppx::http::transfer::system::download_file(
        url,
        path,
        {
            .backend = transfer_backend(selected_backend_from_env_impl()),
            .headers = std::move(extra),
        });
    if (!result)
        return std::unexpected(result.error().message);

    emit_warning(result->warning);
    return {};
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
