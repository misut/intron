import std;
import cppx.terminal;
import intron.domain;
import intron.output;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
}

void test_status_and_stage_lines() {
    check(intron::status_line(cppx::terminal::StatusKind::ok, "ready") ==
              "OK      ready",
          "status line uses fixed-width status cell");
    check(intron::stage_line("extract", 3, 4, "ninja 1.13.2") ==
              "[3/4] [ninja 1.13.2] extract",
          "stage line includes index, total, context, and stage");
}

void test_usage_documents_human_color_controls() {
    auto lines = intron::usage_lines("0.21.2");
    auto joined = std::string{};
    for (auto const& line : lines) {
        joined += line;
        joined.push_back('\n');
    }

    check(joined.contains("Commands:"), "usage keeps command section");
    check(joined.contains("Output:"), "usage includes output section");
    check(joined.contains("INTRON_COLOR=auto|always|never"),
          "usage documents INTRON_COLOR");
    check(joined.contains("NO_COLOR=1 disables color in auto mode"),
          "usage documents NO_COLOR");
}

void test_update_rendering() {
    check(intron::render_update_status(intron::make_update_status(
              "ninja", "1.13.1", std::optional<std::string>{"1.13.2"})) ==
              "RUN     ninja 1.13.1 -> 1.13.2 (update available)",
          "update available status is human-readable");
    check(intron::render_upgrade_check(intron::make_update_status(
              "cmake", "4.3.1", std::optional<std::string>{"4.3.1"})) ==
              "OK      cmake 4.3.1 (up to date)",
          "upgrade check reports up-to-date tools");
}

void test_msvc_rendering() {
    auto missing = intron::MsvcUpdateStatus{
        .state = intron::MsvcUpdateState::Missing,
    };
    auto available = intron::MsvcUpdateStatus{
        .current_version = "17.14.9",
        .latest_version = std::optional<std::string>{"17.14.30"},
        .state = intron::MsvcUpdateState::UpdateAvailable,
    };

    check(intron::render_msvc_update_status(missing) == "FAIL    msvc: not installed",
          "msvc missing status is formatted");
    check(intron::render_msvc_upgrade_check(available) ==
              "RUN     msvc 17.14.9 -> 17.14.30...",
          "msvc upgrade transition is formatted");
}

int main() {
    test_status_and_stage_lines();
    test_usage_documents_human_color_controls();
    test_update_rendering();
    test_msvc_rendering();

    if (failures != 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_output: all tests passed");
    return 0;
}
