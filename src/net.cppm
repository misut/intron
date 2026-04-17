export module net;
import std;
import cppx.http;
import cppx.http.system;

export namespace net {

enum class Backend { Auto, Cppx, Shell };

auto selected_backend_from_env() -> Backend;
auto should_fallback(cppx::http::http_error error) -> bool;

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

constexpr bool is_windows =
#ifdef _WIN32
    true;
#else
    false;
#endif

struct cppx_failure {
    std::string message;
    std::optional<cppx::http::http_error> error;
    bool fallback_allowed = false;
};

auto ascii_lower(std::string_view text) -> std::string {
    auto lowered = std::string{text};
    for (auto& ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

auto ensure_parent_directory(std::filesystem::path const& path) -> void {
    auto parent = path.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
}

auto cleanup_download_target(std::filesystem::path const& path) -> void {
    auto partial = path;
    partial += ".part";

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(partial, ec);
}

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

auto sh_quote(std::string_view s) -> std::string {
    auto out = std::string{"'"};
    for (auto c : s) {
        if (c == '\'')
            out += "'\"'\"'";
        else
            out += c;
    }
    out += '\'';
    return out;
}

auto check_command(std::string_view name) -> bool {
    if constexpr (is_windows) {
        auto cmd = std::format("where {} >nul 2>nul", name);
        return std::system(cmd.c_str()) == 0;
    } else {
        auto cmd = std::format("command -v {} >/dev/null 2>&1", sh_quote(name));
        return std::system(cmd.c_str()) == 0;
    }
}

auto selected_backend_from_env_impl() -> net::Backend {
    auto const* raw = std::getenv("INTRON_NET_BACKEND");
    if (!raw || !*raw)
        return net::Backend::Auto;

    auto value = ascii_lower(raw);
    if (value == "cppx")
        return net::Backend::Cppx;
    if (value == "shell")
        return net::Backend::Shell;
    return net::Backend::Auto;
}

auto should_fallback_impl(cppx::http::http_error error) -> bool {
    return error == cppx::http::http_error::response_parse_failed ||
        error == cppx::http::http_error::connection_failed ||
        error == cppx::http::http_error::tls_failed ||
        error == cppx::http::http_error::timeout;
}

auto warn_shell_fallback(std::string_view operation,
                         std::string_view cppx_error) -> void {
    std::println(
        std::cerr,
        "warning: cppx.http {} failed ({}); using shell backend",
        operation,
        cppx_error);
}

auto append_curl_headers(std::string& cmd, cppx::http::headers const& extra)
    -> void
{
    for (auto const& [name, value] : extra) {
        cmd += std::format(" -H {}", sh_quote(std::format("{}: {}", name, value)));
    }
}

auto shell_command_failure(std::string_view operation) -> std::string {
    return std::format(
        "{} failed: shell backend unavailable (curl not found)",
        operation);
}

auto powershell_request_to_file(std::string_view url,
                                std::filesystem::path const& path,
                                cppx::http::headers const& extra,
                                std::string_view operation)
    -> std::expected<void, std::string>
{
    if constexpr (!is_windows) {
        return std::unexpected("windows shell backend unavailable");
    } else {
        ensure_parent_directory(path);
        cleanup_download_target(path);

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
        if (std::system(cmd.c_str()) != 0) {
            return std::unexpected(std::format(
                "{} failed: powershell fallback failed",
                operation));
        }
        return {};
    }
}

auto curl_request_to_file(std::string_view url,
                          std::filesystem::path const& path,
                          cppx::http::headers const& extra,
                          std::string_view flags,
                          std::string_view operation)
    -> std::expected<void, std::string>
{
    if constexpr (is_windows) {
        return std::unexpected("curl shell backend unavailable on windows");
    } else {
        if (!check_command("curl"))
            return std::unexpected(shell_command_failure(operation));

        ensure_parent_directory(path);
        cleanup_download_target(path);

        auto cmd = std::format("curl {}", flags);
        append_curl_headers(cmd, extra);
        cmd += std::format(
            " -o {} {}",
            sh_quote(path.string()),
            sh_quote(url));

        if (std::system(cmd.c_str()) != 0) {
            return std::unexpected(std::format(
                "{} failed: curl fallback failed",
                operation));
        }
        return {};
    }
}

auto download_via_shell(std::string_view url,
                        std::filesystem::path const& path,
                        cppx::http::headers const& extra)
    -> std::expected<void, std::string>
{
    if constexpr (is_windows)
        return powershell_request_to_file(url, path, extra, "download");
    return curl_request_to_file(url, path, extra, "-fSL --compressed", "download");
}

auto temp_file_path(std::string_view prefix) -> std::filesystem::path {
    static auto counter = std::atomic<std::uint64_t>{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        std::format(
            "intron-{}-{}-{}.tmp",
            prefix,
            stamp,
            counter.fetch_add(1, std::memory_order_relaxed));
}

auto read_text_file(std::filesystem::path const& path)
    -> std::expected<std::string, std::string>
{
    auto in = std::ifstream{path, std::ios::binary};
    if (!in)
        return std::unexpected("request failed: could not read shell response");

    auto body = std::string{
        std::istreambuf_iterator<char>{in},
        std::istreambuf_iterator<char>{},
    };
    return body;
}

auto get_text_via_shell(std::string_view url, cppx::http::headers const& extra)
    -> std::expected<std::string, std::string>
{
    auto tmp = temp_file_path("get");
    auto cleanup = [&] {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    };

    auto dl = [&]() -> std::expected<void, std::string> {
        if constexpr (is_windows)
            return powershell_request_to_file(url, tmp, extra, "request");
        return curl_request_to_file(url, tmp, extra, "-fsSL", "request");
    }();

    if (!dl) {
        cleanup();
        return std::unexpected(dl.error());
    }

    auto body = read_text_file(tmp);
    cleanup();
    if (!body)
        return std::unexpected(body.error());
    return body;
}

auto get_text_via_cppx(std::string_view url, cppx::http::headers const& extra)
    -> std::expected<std::string, cppx_failure>
{
    auto resp = cppx::http::system::get(url, extra);
    if (!resp) {
        auto err = resp.error();
        return std::unexpected(cppx_failure{
            .message = std::format(
                "request failed: {}", cppx::http::to_string(err)),
            .error = err,
            .fallback_allowed = should_fallback_impl(err),
        });
    }
    if (!resp->stat.ok()) {
        return std::unexpected(cppx_failure{
            .message = std::format("HTTP {}", resp->stat.code),
            .fallback_allowed = false,
        });
    }
    return resp->body_string();
}

auto download_via_cppx(std::string_view url,
                       std::filesystem::path const& path,
                       cppx::http::headers const& extra)
    -> std::expected<void, cppx_failure>
{
    auto last_error = cppx_failure{
        .message = "download failed",
    };

    for (int attempt = 1; attempt <= 3; ++attempt) {
        auto client = cppx::http::system::client{};
        auto resp = client.download_to(url, path, extra);
        if (resp) {
            if (!resp->stat.ok()) {
                return std::unexpected(cppx_failure{
                    .message = std::format("HTTP {}", resp->stat.code),
                    .fallback_allowed = false,
                });
            }
            return {};
        }

        auto err = resp.error();
        last_error = cppx_failure{
            .message = std::format(
                "download failed: {}", cppx::http::to_string(err)),
            .error = err,
            .fallback_allowed = should_fallback_impl(err),
        };
        if (!last_error.fallback_allowed || attempt == 3)
            break;

        cleanup_download_target(path);
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
    }

    return std::unexpected(last_error);
}

} // namespace

export namespace net {

auto selected_backend_from_env() -> Backend {
    return selected_backend_from_env_impl();
}

auto should_fallback(cppx::http::http_error error) -> bool {
    return should_fallback_impl(error);
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
    switch (selected_backend_from_env_impl()) {
    case Backend::Shell:
        return get_text_via_shell(url, extra);
    case Backend::Cppx: {
        auto resp = get_text_via_cppx(url, extra);
        if (!resp)
            return std::unexpected(resp.error().message);
        return *resp;
    }
    case Backend::Auto:
        break;
    }

    auto resp = get_text_via_cppx(url, extra);
    if (resp)
        return *resp;
    if (!resp.error().fallback_allowed)
        return std::unexpected(resp.error().message);

    auto fallback = get_text_via_shell(url, extra);
    if (!fallback) {
        return std::unexpected(
            std::format("{}; {}", resp.error().message, fallback.error()));
    }

    warn_shell_fallback("request", resp.error().message);
    return fallback;
}

auto download_file(std::string_view url, std::filesystem::path const& path,
                   cppx::http::headers extra)
    -> std::expected<void, std::string>
{
    switch (selected_backend_from_env_impl()) {
    case Backend::Shell:
        return download_via_shell(url, path, extra);
    case Backend::Cppx: {
        auto resp = download_via_cppx(url, path, extra);
        if (!resp)
            return std::unexpected(resp.error().message);
        return {};
    }
    case Backend::Auto:
        break;
    }

    auto resp = download_via_cppx(url, path, extra);
    if (resp)
        return {};
    if (!resp.error().fallback_allowed)
        return std::unexpected(resp.error().message);

    auto fallback = download_via_shell(url, path, extra);
    if (!fallback) {
        return std::unexpected(
            std::format("{}; {}", resp.error().message, fallback.error()));
    }

    warn_shell_fallback("download", resp.error().message);
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
