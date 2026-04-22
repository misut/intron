#include <cstdlib>

import std;
import intron.domain;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
}

auto make_plan_with_path_and_extras() -> intron::EnvPlan {
    auto plan = intron::EnvPlan{};
    plan.assignments.push_back({
        .key = "PATH",
        .value = "C:\\a;C:\\b",
        .append_existing = true,
    });
    plan.assignments.push_back({
        .key = "CC",
        .value = "C:\\msvc\\cl.exe",
    });
    plan.assignments.push_back({
        .key = "INCLUDE",
        .value = "C:\\sdk\\include",
    });
    return plan;
}

auto make_plan_with_posix_path() -> intron::EnvPlan {
    auto plan = intron::EnvPlan{};
    plan.assignments.push_back({
        .key = "PATH",
        .value = "/a:/b",
        .append_existing = true,
    });
    plan.assignments.push_back({
        .key = "CC",
        .value = "/usr/bin/clang",
    });
    return plan;
}

void test_render_default_mode_matches_existing_format_windows() {
    auto plan = make_plan_with_path_and_extras();
    auto lines = intron::render_env_lines(plan, /*is_windows=*/true);

    check(lines.size() == 3, "windows default mode emits one line per assignment");
    check(lines.at(0) == R"($env:PATH = "C:\a;C:\b;${env:PATH}";)",
          "windows default mode keeps append-existing PATH form");
    check(lines.at(1) == R"($env:CC = "C:\msvc\cl.exe";)",
          "windows default mode keeps CC assignment form");
    check(lines.at(2) == R"($env:INCLUDE = "C:\sdk\include";)",
          "windows default mode keeps extra var form");
}

void test_render_default_mode_matches_existing_format_posix() {
    auto plan = make_plan_with_posix_path();
    auto lines = intron::render_env_lines(plan, /*is_windows=*/false);

    check(lines.size() == 2, "posix default mode emits one line per assignment");
    check(lines.at(0) == R"(export PATH="/a:/b:$PATH";)",
          "posix default mode keeps append-existing PATH form");
    check(lines.at(1) == R"(export CC="/usr/bin/clang";)",
          "posix default mode keeps CC assignment form");
}

void test_render_path_only_splits_windows_segments() {
    auto plan = make_plan_with_path_and_extras();
    auto lines = intron::render_env_lines(
        plan,
        /*is_windows=*/true,
        intron::EnvOutputMode::PathOnly);

    check(lines.size() == 2, "path-only windows emits one line per PATH segment");
    check(lines.at(0) == "C:\\a", "path-only windows first segment");
    check(lines.at(1) == "C:\\b", "path-only windows second segment");
    check(!std::ranges::any_of(lines, [](auto const& line) {
              return line.contains("CC") || line.contains("INCLUDE") ||
                     line.contains("=");
          }),
          "path-only suppresses non-PATH assignments");
}

void test_render_path_only_splits_posix_segments() {
    auto plan = make_plan_with_posix_path();
    auto lines = intron::render_env_lines(
        plan,
        /*is_windows=*/false,
        intron::EnvOutputMode::PathOnly);

    check(lines.size() == 2, "path-only posix emits one line per PATH segment");
    check(lines.at(0) == "/a", "path-only posix first segment");
    check(lines.at(1) == "/b", "path-only posix second segment");
}

void test_render_path_only_empty_plan() {
    auto plan = intron::EnvPlan{};
    auto lines = intron::render_env_lines(
        plan,
        /*is_windows=*/true,
        intron::EnvOutputMode::PathOnly);
    check(lines.empty(), "path-only empty plan yields empty output");
}

void test_render_path_only_skips_non_appending_path_assignment() {
    auto plan = intron::EnvPlan{};
    plan.assignments.push_back({
        .key = "PATH",
        .value = "C:\\a",
        .append_existing = false,
    });
    auto lines = intron::render_env_lines(
        plan,
        /*is_windows=*/true,
        intron::EnvOutputMode::PathOnly);
    check(lines.empty(),
          "path-only ignores non-append PATH assignments (would overwrite existing PATH)");
}

void test_render_github_prefixes_path_and_env_lines() {
    auto plan = make_plan_with_path_and_extras();
    auto lines = intron::render_env_lines(
        plan,
        /*is_windows=*/true,
        intron::EnvOutputMode::GitHub);

    check(lines.size() == 4, "github emits path lines plus env lines");
    check(lines.at(0) == "path=C:\\a", "github PATH segment 0");
    check(lines.at(1) == "path=C:\\b", "github PATH segment 1");
    check(lines.at(2) == "env=CC=C:\\msvc\\cl.exe", "github CC env line");
    check(lines.at(3) == "env=INCLUDE=C:\\sdk\\include", "github INCLUDE env line");
}

void test_render_github_places_path_block_before_env_block() {
    auto plan = intron::EnvPlan{};
    plan.assignments.push_back({
        .key = "CC",
        .value = "cl.exe",
    });
    plan.assignments.push_back({
        .key = "PATH",
        .value = "/only",
        .append_existing = true,
    });
    auto lines = intron::render_env_lines(
        plan,
        /*is_windows=*/false,
        intron::EnvOutputMode::GitHub);

    check(lines.size() == 2, "github keeps one line per PATH segment plus env");
    check(lines.at(0) == "path=/only",
          "github emits path lines first regardless of plan order");
    check(lines.at(1) == "env=CC=cl.exe",
          "github emits env lines after path block");
}

void test_parse_env_flags_defaults_to_shell_eval() {
    auto result = intron::parse_env_flags({});
    check(result.has_value() && *result == intron::EnvOutputMode::ShellEval,
          "empty args default to ShellEval");
}

void test_parse_env_flags_accepts_path_only_and_alias() {
    auto a = intron::parse_env_flags({"--path-only"});
    auto b = intron::parse_env_flags({"--additive"});
    check(a.has_value() && *a == intron::EnvOutputMode::PathOnly,
          "--path-only maps to PathOnly");
    check(b.has_value() && *b == intron::EnvOutputMode::PathOnly,
          "--additive is an alias for --path-only");
}

void test_parse_env_flags_accepts_github() {
    auto r = intron::parse_env_flags({"--github"});
    check(r.has_value() && *r == intron::EnvOutputMode::GitHub,
          "--github maps to GitHub");
}

void test_parse_env_flags_rejects_conflicting_modes() {
    auto r = intron::parse_env_flags({"--path-only", "--github"});
    check(!r.has_value(), "combining --path-only and --github is rejected");
    if (!r.has_value()) {
        check(r.error().contains("conflicting"),
              "error message mentions conflict");
    }
}

void test_parse_env_flags_rejects_unknown_flag() {
    auto r = intron::parse_env_flags({"--nope"});
    check(!r.has_value(), "unknown flag is rejected");
    if (!r.has_value()) {
        check(r.error().contains("unknown flag") && r.error().contains("--nope"),
              "error message identifies the unknown flag");
    }
}

void test_parse_env_flags_rejects_positional_arg() {
    auto r = intron::parse_env_flags({"foo"});
    check(!r.has_value(), "positional arg to env is rejected");
    if (!r.has_value()) {
        check(r.error().contains("positional"),
              "error message mentions positional argument");
    }
}

void test_parse_env_flags_accepts_duplicate_same_mode() {
    auto r = intron::parse_env_flags({"--path-only", "--additive"});
    check(r.has_value() && *r == intron::EnvOutputMode::PathOnly,
          "duplicate equivalent flags stay at PathOnly");
}

int main() {
    test_render_default_mode_matches_existing_format_windows();
    test_render_default_mode_matches_existing_format_posix();
    test_render_path_only_splits_windows_segments();
    test_render_path_only_splits_posix_segments();
    test_render_path_only_empty_plan();
    test_render_path_only_skips_non_appending_path_assignment();
    test_render_github_prefixes_path_and_env_lines();
    test_render_github_places_path_block_before_env_block();
    test_parse_env_flags_defaults_to_shell_eval();
    test_parse_env_flags_accepts_path_only_and_alias();
    test_parse_env_flags_accepts_github();
    test_parse_env_flags_rejects_conflicting_modes();
    test_parse_env_flags_rejects_unknown_flag();
    test_parse_env_flags_rejects_positional_arg();
    test_parse_env_flags_accepts_duplicate_same_mode();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_env_output_modes: all tests passed");
    return 0;
}
