#include <cstdlib>

import std;
import intron.app;
import intron.domain;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
}

void test_parse_without_command() {
    auto argv0 = std::array{const_cast<char*>("intron")};
    auto parsed = intron::app::parse_command_request(
        static_cast<int>(argv0.size()),
        argv0.data());

    check(!parsed.has_value(), "parse without command returns error result");
    if (!parsed.has_value()) {
        check(parsed.error().exit_code == 1, "usage result exits with code 1");
        check(!parsed.error().stdout_lines.empty(), "usage result contains usage lines");
    }
}

void test_parse_unknown_command() {
    auto argv = std::array{
        const_cast<char*>("intron"),
        const_cast<char*>("mystery"),
    };
    auto parsed = intron::app::parse_command_request(
        static_cast<int>(argv.size()),
        argv.data());

    check(!parsed.has_value(), "unknown command returns error result");
    if (!parsed.has_value()) {
        check(parsed.error().exit_code == 1, "unknown command exits with code 1");
        check(!parsed.error().stderr_lines.empty(), "unknown command reports an error");
    }
}

void test_parse_help_command() {
    auto argv = std::array{
        const_cast<char*>("intron"),
        const_cast<char*>("help"),
    };
    auto parsed = intron::app::parse_command_request(
        static_cast<int>(argv.size()),
        argv.data());

    check(parsed.has_value(), "help command parses successfully");
    if (parsed.has_value()) {
        check(parsed->command == intron::CommandKind::Help, "help command kind");
    }
}

void test_platform_arg_split() {
    auto args = std::vector<std::string>{
        "llvm",
        "22.1.2",
        "--platform",
        "macos",
    };
    auto parsed = intron::split_platform_args(args);
    check(parsed.has_value(), "platform args parse");
    if (parsed.has_value()) {
        check(parsed->positional.size() == 2, "platform args keep positionals");
        check(parsed->platform == std::optional<std::string>{"macos"},
              "platform args capture platform");
    }

    auto dangling = intron::split_platform_args({"llvm", "--platform"});
    check(dangling.has_value(), "dangling platform flag is ignored for compatibility");
    if (dangling.has_value()) {
        check(dangling->platform == std::nullopt, "dangling platform produces no platform");
    }
}

void test_tool_lookup() {
    check(intron::tool_for_binary("clang++") == std::optional<std::string>{"llvm"},
          "clang++ maps to llvm");
    check(intron::tool_for_binary("cmake") == std::optional<std::string>{"cmake"},
          "cmake maps to cmake");
    check(!intron::tool_for_binary("unknown-tool").has_value(),
          "unknown binary is not mapped");
}

void test_build_tool_map() {
    auto current = intron::build_tool_map(
        {{"llvm", "22.1.2"}, {"cmake", "4.3.1"}},
        {{"llvm", "21.0.0"}, {"ninja", "1.13.2"}});

    check(current.at("llvm") == "22.1.2", "installed version wins over defaults");
    check(current.at("ninja") == "1.13.2", "defaults fill missing installed tools");
}

void test_env_rendering() {
    auto plan = intron::build_env_plan(
        std::optional<std::string>{"/tool/bin:/other/bin"},
        std::optional<std::filesystem::path>{"/tool/bin/clang"},
        std::optional<std::filesystem::path>{"/tool/bin/clang++"},
        {},
        std::optional<std::filesystem::path>{"/tool/wasi"});

    auto lines = intron::render_env_lines(plan, false);
    check(lines.size() == 4, "env plan renders expected number of lines");
    if (lines.size() == 4) {
        check(lines[0] == "export PATH=\"/tool/bin:/other/bin:$PATH\";",
              "env rendering formats PATH export");
        check(lines[1] == "export CC=\"/tool/bin/clang\";", "env rendering formats CC export");
        check(lines[2] == "export CXX=\"/tool/bin/clang++\";", "env rendering formats CXX export");
        check(lines[3] == "export WASI_SDK_PATH=\"/tool/wasi\";",
              "env rendering formats WASI export");
    }
}

int main() {
    test_parse_without_command();
    test_parse_unknown_command();
    test_parse_help_command();
    test_platform_arg_split();
    test_tool_lookup();
    test_build_tool_map();
    test_env_rendering();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_app: all tests passed");
    return 0;
}
