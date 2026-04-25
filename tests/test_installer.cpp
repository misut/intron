#include <cstdlib>

import std;
import intron.domain;
import installer;
import net;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "FAIL: {}", msg);
        ++failures;
    }
}

void test_toolchain_path() {
    auto path = installer::toolchain_path("llvm", "22.1.2");
    check(path.generic_string().contains(".intron/toolchains/llvm/22.1.2"),
          "llvm toolchain path");

    auto path2 = installer::toolchain_path("ninja", "1.12.1");
    check(path2.generic_string().contains(".intron/toolchains/ninja/1.12.1"),
          "ninja toolchain path");

    auto pure_home = installer::intron_home_path("/tmp/home");
    auto pure_path = installer::toolchain_path(pure_home, "llvm", "22.1.2");
    check(pure_home.generic_string() == "/tmp/home/.intron", "pure intron home path");
    check(pure_path.generic_string() == "/tmp/home/.intron/toolchains/llvm/22.1.2",
          "pure toolchain path");
}

void test_intron_home() {
    auto home = installer::intron_home();
    check(home.string().contains(".intron"), "intron home contains .intron");
    check(std::filesystem::exists(home), "intron home exists");
}

void test_which_not_installed() {
    auto result = installer::which("clang++", "llvm", "99.99.99");
    check(!result.has_value(), "which returns nullopt for missing tool");
}

void test_latest_version_from_release_json() {
    auto version = net::latest_version_from_release_json(R"({"tag_name":"v0.18.3"})");
    check(version.has_value(), "release json parsed");
    check(*version == "0.18.3", "leading v removed");

    auto dashed = net::latest_version_from_release_json(R"({"tag_name":"llvmorg-20.1.0"})");
    check(dashed.has_value(), "dash tag parsed");
    check(*dashed == "20.1.0", "suffix extracted from dashed tag");

    auto missing = net::latest_version_from_release_json(R"({"name":"missing"})");
    check(!missing.has_value(), "missing tag_name returns nullopt");
}

void test_github_api_headers() {
    auto hdrs = net::github_api_headers("intron/test");
    check(hdrs.get("user-agent") == "intron/test", "github api user-agent");
    check(hdrs.get("accept") == "application/vnd.github+json",
          "github api accept header");
}

void set_env(std::string_view name, std::string_view value) {
    auto env_name = std::string{name};
    auto env_value = std::string{value};
#ifdef _WIN32
    ::_putenv_s(env_name.c_str(), env_value.c_str());
#else
    ::setenv(env_name.c_str(), env_value.c_str(), 1);
#endif
}

void clear_env(std::string_view name) {
    auto env_name = std::string{name};
#ifdef _WIN32
    ::_putenv_s(env_name.c_str(), "");
#else
    ::unsetenv(env_name.c_str());
#endif
}

struct EnvGuard {
    explicit EnvGuard(std::string_view key)
        : key(key)
    {
        if (auto* value = std::getenv(this->key.c_str()); value) {
            original = value;
        }
    }

    ~EnvGuard() {
        if (original.has_value()) {
            set_env(key, *original);
        } else {
            clear_env(key);
        }
    }

    std::string key;
    std::optional<std::string> original;
};

struct TempDirGuard {
    explicit TempDirGuard(std::filesystem::path path)
        : path(std::move(path))
    {
    }

    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
};

void write_text_file(std::filesystem::path const& path, std::string_view text) {
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    auto out = std::ofstream{path};
    out << text;
}

void test_selected_backend_from_env() {
    auto guard = EnvGuard{"INTRON_NET_BACKEND"};

    clear_env("INTRON_NET_BACKEND");
    check(net::selected_backend_from_env() == net::Backend::Auto,
          "net backend defaults to auto");

    set_env("INTRON_NET_BACKEND", "cppx");
    check(net::selected_backend_from_env() == net::Backend::Cppx,
          "cppx backend can be forced");

    set_env("INTRON_NET_BACKEND", "SHELL");
    check(net::selected_backend_from_env() == net::Backend::Shell,
          "shell backend parsing is case insensitive");

    set_env("INTRON_NET_BACKEND", "bogus");
    check(net::selected_backend_from_env() == net::Backend::Auto,
          "invalid backend falls back to auto");
}

void test_selected_backend_from_string() {
    check(net::selected_backend_from_string(std::nullopt) == net::Backend::Auto,
          "missing backend string defaults to auto");
    check(net::selected_backend_from_string("cppx") == net::Backend::Cppx,
          "pure backend parser handles cppx");
    check(net::selected_backend_from_string("SHELL") == net::Backend::Shell,
          "pure backend parser is case insensitive");
}

void test_parse_vswhere_instances() {
    auto json = R"([
      {
        "installationPath": "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools",
        "productId": "Microsoft.VisualStudio.Product.BuildTools",
        "channelId": "VisualStudio.17.Release",
        "channelUri": "https://aka.ms/vs/17/release/channel",
        "installedChannelUri": "https://aka.ms/vs/17/release/channel",
        "installationVersion": "17.14.36015.10",
        "catalog": {
          "productDisplayVersion": "17.14.9"
        }
      },
      {
        "installationPath": "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community",
        "productId": "Microsoft.VisualStudio.Product.Community",
        "channelId": "VisualStudio.17.Release",
        "installationVersion": "17.13.35500.0"
      }
    ])";

    auto instances = installer::parse_vswhere_instances(json);
    check(instances.size() == 2, "vswhere parser keeps both instances");
    if (instances.size() == 2) {
        check(instances[0].installation_path.string().contains("BuildTools"),
              "vswhere parser reads installation path");
        check(instances[0].product_id == "Microsoft.VisualStudio.Product.BuildTools",
              "vswhere parser reads product id");
        check(instances[0].channel_id == "VisualStudio.17.Release",
              "vswhere parser reads channel id");
        check(instances[0].channel_uri == "https://aka.ms/vs/17/release/channel",
              "vswhere parser reads channel uri");
        check(instances[0].installed_channel_uri == "https://aka.ms/vs/17/release/channel",
              "vswhere parser reads installed channel uri");
        check(instances[0].installation_version == "17.14.36015.10",
              "vswhere parser reads installation version");
        check(instances[0].product_display_version == "17.14.9",
              "vswhere parser reads display version");
    }
}

auto fake_instance(std::string install_path,
                   std::string product_id,
                   std::string installation_version,
                   bool ready) -> installer::VisualStudioInstance
{
    auto instance = installer::VisualStudioInstance{
        .installation_path = std::filesystem::path{install_path},
        .product_id = std::move(product_id),
        .channel_id = "VisualStudio.17.Release",
        .channel_uri = "https://aka.ms/vs/17/release/channel",
        .installed_channel_uri = "https://aka.ms/vs/17/release/channel",
        .installation_version = std::move(installation_version),
        .product_display_version = "17.14.9",
    };
    if (ready) {
        instance.toolset_root = instance.installation_path / "VC" / "Tools" / "MSVC" / "14.44.35207";
        instance.vcvars64_path = instance.installation_path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
        instance.cl_path = *instance.toolset_root / "bin" / "Hostx64" / "x64" / "cl.exe";
    }
    return instance;
}

void test_select_ready_msvc_instance() {
    auto instances = std::vector<installer::VisualStudioInstance>{
        fake_instance("C:/VS/Community", "Microsoft.VisualStudio.Product.Community", "17.14.1", true),
        fake_instance("C:/VS/BuildTools", "Microsoft.VisualStudio.Product.BuildTools", "17.13.9", true),
    };

    auto selected = installer::select_ready_msvc_instance(instances);
    check(selected.has_value(), "ready instance is selected");
    if (selected) {
        check(selected->is_build_tools(), "build tools instance wins over ready IDE instance");
    }
}

void test_parse_visual_studio_channel_version() {
    auto json = R"({
      "info": {
        "productDisplayVersion": "17.14.30"
      },
      "channelItems": [
        {
          "id": "Microsoft.VisualStudio.Product.BuildTools",
          "version": "17.14.37203.1",
          "type": "ChannelProduct"
        },
        {
          "id": "Microsoft.VisualStudio.Product.Community",
          "version": "17.14.37203.1",
          "type": "ChannelProduct"
        }
      ]
    })";

    auto build_tools = installer::parse_visual_studio_channel_version(
        json,
        "Microsoft.VisualStudio.Product.BuildTools");
    check(build_tools.has_value(), "channel parser finds BuildTools product");
    if (build_tools) {
        check(build_tools->installation_version == "17.14.37203.1",
              "channel parser reads product installation version");
        check(build_tools->display_version == "17.14.30",
              "channel parser reads display version");
    }

    auto community = installer::parse_visual_studio_channel_version(
        json,
        "Microsoft.VisualStudio.Product.Community");
    check(community.has_value(), "channel parser finds Community product");

    auto missing = installer::parse_visual_studio_channel_version(
        json,
        "Microsoft.VisualStudio.Product.Enterprise");
    check(!missing.has_value(), "channel parser returns nullopt for missing product");

    auto malformed = installer::parse_visual_studio_channel_version(
        R"({"info":{"productDisplayVersion":"17.14.30"}})",
        "Microsoft.VisualStudio.Product.BuildTools");
    check(!malformed.has_value(), "channel parser returns nullopt when channelItems is missing");
}

void test_make_msvc_update_status() {
    auto current = fake_instance(
        "C:/VS/BuildTools",
        "Microsoft.VisualStudio.Product.BuildTools",
        "17.14.36310.24",
        true);
    current.product_display_version = "17.14.9";

    auto latest = installer::VisualStudioChannelVersion{
        .installation_version = "17.14.37203.1",
        .display_version = "17.14.30",
    };

    auto status = installer::detail::make_msvc_update_status(current, latest);
    check(status.state == intron::MsvcUpdateState::UpdateAvailable,
          "msvc status reports update when channel version is newer");
    check(status.current_version == "17.14.9", "msvc status keeps current display version");
    check(status.latest_version == std::optional<std::string>{"17.14.30"},
          "msvc status exposes latest display version");

    current.installation_version = "17.14.37203.1";
    current.product_display_version = "17.14.30";
    auto up_to_date = installer::detail::make_msvc_update_status(current, latest);
    check(up_to_date.state == intron::MsvcUpdateState::UpToDate,
          "msvc status reports up-to-date when versions match");

    auto unknown = installer::detail::make_msvc_update_status(current, std::nullopt);
    check(unknown.state == intron::MsvcUpdateState::Unknown,
          "msvc status reports unknown when channel data is unavailable");
}

void test_select_msvc_modify_target() {
    auto instances = std::vector<installer::VisualStudioInstance>{
        fake_instance("C:/VS/Community", "Microsoft.VisualStudio.Product.Community", "17.14.1", false),
        fake_instance("C:/VS/BuildTools", "Microsoft.VisualStudio.Product.BuildTools", "17.13.9", false),
    };

    auto selected = installer::select_msvc_modify_target(instances);
    check(selected.has_value(), "modify target is selected");
    if (selected) {
        check(selected->is_build_tools(), "build tools instance wins as modify target");
    }
}

void test_build_msvc_install_command() {
    auto info = registry::resolve("msvc", "2022");
    auto command = installer::build_msvc_install_command(
        info,
        "C:/Users/test/.intron/downloads/vs_BuildTools.exe");

    check(command.program.generic_string().ends_with("vs_BuildTools.exe"),
          "install command uses bootstrapper");
    auto joined = std::string{};
    for (auto const& arg : command.args) {
        joined += arg;
        joined += '\n';
    }
    check(joined.contains("--productId"), "install command includes product id");
    check(joined.contains("Microsoft.VisualStudio.Product.BuildTools"),
          "install command targets build tools");
    check(joined.contains("--channelId"), "install command includes channel id");
    check(joined.contains("VisualStudio.17.Release"), "install command targets release channel");
    check(joined.contains("--installPath"), "install command includes install path");
    check(joined.contains("BuildTools"), "install command targets BuildTools path");
    check(joined.contains("Microsoft.VisualStudio.Workload.VCTools"),
          "install command adds VCTools workload");
    check(joined.contains("--includeRecommended"), "install command includes recommended components");
    check(joined.contains("--passive"), "install command uses passive mode");
    check(joined.contains("--wait"), "install command waits for completion");
    check(joined.contains("--norestart"), "install command disables auto restart");
}

void test_build_msvc_modify_command() {
    auto info = registry::resolve("msvc", "2022");
    auto instance = fake_instance(
        "C:/VS/Community",
        "Microsoft.VisualStudio.Product.Community",
        "17.14.1",
        false);
    auto command = installer::build_msvc_modify_command(
        info,
        instance,
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/setup.exe");

    check(command.program.generic_string().ends_with("setup.exe"),
          "modify command uses setup.exe");
    auto joined = std::string{};
    for (auto const& arg : command.args) {
        joined += arg;
        joined += '\n';
    }
    check(joined.contains("modify"), "modify command uses modify verb");
    check(joined.contains("Microsoft.VisualStudio.Product.Community"),
          "modify command preserves instance product id");
    check(joined.contains("C:/VS/Community"), "modify command targets instance install path");
    check(joined.contains("Microsoft.VisualStudio.Workload.VCTools"),
          "modify command adds VCTools workload");
}

void test_build_msvc_update_command() {
    auto instance = fake_instance(
        "C:/VS/BuildTools",
        "Microsoft.VisualStudio.Product.BuildTools",
        "17.14.1",
        true);
    auto command = installer::build_msvc_update_command(
        instance,
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/setup.exe");

    check(command.program.generic_string().ends_with("setup.exe"),
          "update command uses setup.exe");
    auto joined = std::string{};
    for (auto const& arg : command.args) {
        joined += arg;
        joined += '\n';
    }
    check(joined.contains("update"), "update command uses update verb");
    check(joined.contains("--installPath"), "update command includes install path");
    check(joined.contains("C:/VS/BuildTools"), "update command targets selected instance");
    check(joined.contains("--passive"), "update command uses passive mode");
    check(joined.contains("--wait"), "update command waits for completion");
    check(joined.contains("--norestart"), "update command disables auto restart");
}

void test_classify_msvc_installer_exit() {
    auto ok = installer::classify_msvc_installer_exit(0);
    check(ok.kind == installer::VisualStudioInstallerExitKind::Success,
          "exit code 0 is success");

    auto reboot = installer::classify_msvc_installer_exit(3010);
    check(reboot.kind == installer::VisualStudioInstallerExitKind::SuccessRebootRequired,
          "exit code 3010 requires reboot");

    auto elevation = installer::classify_msvc_installer_exit(740);
    check(elevation.kind == installer::VisualStudioInstallerExitKind::ElevationRequired,
          "exit code 740 requires elevation");

    auto failure = installer::classify_msvc_installer_exit(1234);
    check(failure.kind == installer::VisualStudioInstallerExitKind::Failure,
          "unknown exit code is failure");
}

void test_msvc_helper_paths() {
    auto root = std::filesystem::path{"C:/VS/VC/Tools/MSVC/14.40.33807"};
    auto bin = installer::msvc_bin_path(root);
    auto asan = installer::msvc_asan_runtime_path(root);

    check(bin.generic_string().ends_with("VC/Tools/MSVC/14.40.33807/bin/Hostx64/x64"),
          "msvc bin path uses Hostx64/x64");
    check(asan.generic_string().ends_with(
              "VC/Tools/MSVC/14.40.33807/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll"),
          "msvc asan runtime path points at clang_rt dll");
}

void test_msvc_binary_path() {
    auto temp = std::filesystem::temp_directory_path() / "intron-msvc-binary-path-test";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    auto bin_dir = temp / "VC" / "Tools" / "MSVC" / "14.44.35207" / "bin" / "Hostx64" / "x64";
    std::filesystem::create_directories(bin_dir);
    std::ofstream{bin_dir / "cl.exe"}.put('\n');
    std::ofstream{bin_dir / "link.exe"}.put('\n');

    auto instance = fake_instance(
        temp.string(),
        "Microsoft.VisualStudio.Product.BuildTools",
        "17.14.1",
        true);

    auto cl = installer::msvc_binary_path(instance, "cl.exe");
    check(cl.has_value(), "cl.exe path is produced for ready instance");
    if (cl) {
        check(cl->generic_string().ends_with("/cl.exe"), "cl.exe path ends with cl.exe");
    }

    auto link = installer::msvc_binary_path(instance, "link");
    check(link.has_value(), "link path is produced for ready instance");
    if (link) {
        check(link->generic_string().ends_with("/link.exe"), "link path ends with link.exe");
    }

    std::filesystem::remove_all(temp, ec);
}

void test_capture_msvc_environment_with_wrapper_script() {
#ifdef _WIN32
    auto base = std::filesystem::temp_directory_path() / std::format(
        "intron msvc env test {}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    auto cleanup = TempDirGuard{base};

    auto instance = fake_instance(
        base.string(),
        "Microsoft.VisualStudio.Product.Community",
        "17.14.1",
        true);
    write_text_file(
        *instance.vcvars64_path,
        "@echo off\r\n"
        "set INCLUDE=C:\\Fake Include\r\n"
        "set LIB=C:\\Fake Lib\r\n"
        "set LIBPATH=C:\\Fake LibPath\r\n"
        "set Path=C:\\Fake Bin;%Path%\r\n");

    auto env = installer::detail::capture_msvc_environment(instance);
    check(env.has_value(), "msvc environment capture works with wrapper script");
    if (!env.has_value()) {
        return;
    }
    check(env->contains("INCLUDE"), "captured environment includes INCLUDE");
    check(env->contains("LIB"), "captured environment includes LIB");
    check(env->contains("LIBPATH"), "captured environment includes LIBPATH");
    check(env->contains("Path"), "captured environment includes Path");
    if (env->contains("INCLUDE")) {
        check(env->at("INCLUDE") == "C:\\Fake Include", "captured INCLUDE keeps fake value");
    }
    if (env->contains("LIB")) {
        check(env->at("LIB") == "C:\\Fake Lib", "captured LIB keeps fake value");
    }
    if (env->contains("LIBPATH")) {
        check(env->at("LIBPATH") == "C:\\Fake LibPath", "captured LIBPATH keeps fake value");
    }
    if (env->contains("Path")) {
        check(env->at("Path").starts_with("C:\\Fake Bin;"), "captured Path keeps fake prefix");
    }
#endif
}

void test_install_plan() {
    auto info = registry::resolve("cmake", "4.3.1");
    auto plan = intron::make_install_plan("/tmp/intron-home", info, true);

    check(plan.home.generic_string() == "/tmp/intron-home", "install plan keeps home path");
    check(plan.dest.generic_string().ends_with("/toolchains/cmake/4.3.1"),
          "install plan computes destination");
    check(plan.download.has_value(), "install plan includes download");
    check(plan.download->use_cached_archive, "install plan carries cache flag");
    check(plan.download->verify_checksum, "install plan tracks checksum requirement");
    check(plan.archive_name.contains("cmake-4.3.1"), "install plan records archive name");

    auto llvm_plan =
        intron::make_install_plan("/tmp/intron-home", registry::resolve("llvm", "22.1.2"));
    check(llvm_plan.post_install_actions.size() == 1,
          "llvm install plan schedules post-install action");

    auto msvc_plan =
        intron::make_install_plan("/tmp/intron-home", registry::resolve("msvc", "2022"));
    check(!msvc_plan.download.has_value(), "msvc install plan has no archive download");
}

void test_prepare_clean_staging_dir_removes_stale_content() {
    auto root = std::filesystem::temp_directory_path() /
        std::format(
            "intron-staging-test-{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
    auto guard = TempDirGuard{root};
    auto staging = root / "staging" / "android-ndk-r30-beta1";

    write_text_file(staging / "toolchains" / "stale.txt", "stale");

    auto prepared = installer::detail::prepare_clean_staging_dir(staging);
    check(prepared.has_value(), "clean staging dir succeeds");
    check(std::filesystem::exists(staging), "clean staging dir exists");
    check(std::filesystem::is_empty(staging), "clean staging dir removes stale contents");
}

void test_msvc_environment_smoke() {
#ifdef _WIN32
    auto root = installer::msvc_path();
    if (!root.has_value()) {
        std::println("SKIP: msvc installation not available");
        return;
    }
    auto env = installer::msvc_environment();
    check(env.has_value(), "msvc environment is available when msvc is installed");
    if (!env.has_value()) {
        return;
    }
    check(std::filesystem::exists(env->cl), "msvc environment exposes cl.exe");
    check(env->variables.contains("INCLUDE"), "msvc environment includes INCLUDE");
    check(env->variables.contains("LIB"), "msvc environment includes LIB");
    check(env->variables.contains("LIBPATH"), "msvc environment includes LIBPATH");
#endif
}

int main() {
    test_toolchain_path();
    test_intron_home();
    test_which_not_installed();
    test_latest_version_from_release_json();
    test_github_api_headers();
    test_selected_backend_from_env();
    test_selected_backend_from_string();
    test_parse_vswhere_instances();
    test_parse_visual_studio_channel_version();
    test_make_msvc_update_status();
    test_select_ready_msvc_instance();
    test_select_msvc_modify_target();
    test_build_msvc_install_command();
    test_build_msvc_modify_command();
    test_build_msvc_update_command();
    test_classify_msvc_installer_exit();
    test_msvc_helper_paths();
    test_msvc_binary_path();
    test_capture_msvc_environment_with_wrapper_script();
    test_install_plan();
    test_prepare_clean_staging_dir_removes_stale_content();
    test_msvc_environment_smoke();

    if (failures > 0) {
        std::println(std::cerr, "{} test(s) failed", failures);
        return 1;
    }
    std::println("test_installer: all tests passed");
    return 0;
}
