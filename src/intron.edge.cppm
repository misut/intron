export module intron.edge;
import std;
import cppx.env.system;
import intron.domain;

export namespace intron::edge {

auto make_runtime_ports() -> intron::RuntimePorts {
    auto ports = intron::RuntimePorts{};
    ports.filesystem.exists = [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    };
    ports.environment.get = [](std::string_view key) -> std::optional<std::string> {
        auto owned = std::string{key};
        if (auto const* value = std::getenv(owned.c_str()); value) {
            return std::string{value};
        }
        return std::nullopt;
    };
    ports.environment.home_dir = [] {
        return cppx::env::system::home_dir();
    };
    ports.clock.sleep_for = [](std::chrono::milliseconds duration) {
        std::this_thread::sleep_for(duration);
    };
    ports.console.write_stdout = [](std::string_view line) {
        std::println("{}", line);
    };
    ports.console.write_stderr = [](std::string_view line) {
        std::println(std::cerr, "{}", line);
    };
    return ports;
}

auto render(intron::CommandResult const& result,
            intron::RuntimePorts const& ports) -> void
{
    if (!result.diagnostics.empty()) {
        for (auto const& diagnostic : result.diagnostics) {
            if (diagnostic.channel == intron::OutputChannel::Stdout) {
                if (ports.console.write_stdout) {
                    ports.console.write_stdout(diagnostic.message);
                }
            } else if (ports.console.write_stderr) {
                ports.console.write_stderr(diagnostic.message);
            }
        }
        return;
    }

    for (auto const& line : result.stdout_lines) {
        if (ports.console.write_stdout) {
            ports.console.write_stdout(line);
        }
    }
    for (auto const& line : result.stderr_lines) {
        if (ports.console.write_stderr) {
            ports.console.write_stderr(line);
        }
    }
}

auto render(intron::CommandResult const& result) -> void {
    render(result, make_runtime_ports());
}

} // namespace intron::edge
