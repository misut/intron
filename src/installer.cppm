export module installer;
import std;
import cppx.env.system;
export import registry;

export namespace installer {

std::filesystem::path intron_home() {
    auto home = cppx::env::system::home_dir();
    if (!home)
        throw std::runtime_error("HOME environment variable not set");
    auto path = *home / ".intron";
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path toolchain_path(std::string_view tool, std::string_view version) {
    return intron_home() / "toolchains" / tool / version;
}

namespace detail {

constexpr bool is_windows =
#ifdef _WIN32
    true;
#else
    false;
#endif

int run(std::string const& cmd) {
    return std::system(cmd.c_str());
}

std::string capture(std::string const& cmd) {
    auto tmp = std::filesystem::temp_directory_path() / "intron_capture.tmp";
    std::string full_cmd;
    if constexpr (is_windows) {
        full_cmd = std::format("{} > \"{}\" 2>nul", cmd, tmp.string());
    } else {
        full_cmd = std::format("{} > '{}' 2>/dev/null", cmd, tmp.string());
    }
    if (std::system(full_cmd.c_str()) != 0) return {};
    std::string result;
    {
        auto in = std::ifstream{tmp};
        result.assign(
            std::istreambuf_iterator<char>{in},
            std::istreambuf_iterator<char>{});
    } // close stream before remove (Windows blocks removing open files)
    std::filesystem::remove(tmp);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool check_command(std::string_view name) {
    if constexpr (is_windows) {
        return run(std::format("where {} >nul 2>nul", name)) == 0;
    } else {
        return run(std::format("command -v '{}' >/dev/null 2>&1", name)) == 0;
    }
}

std::string tar_command() {
    // On Windows, use the system bsdtar directly (C:\Windows\System32\tar.exe)
    // to avoid accidentally invoking MSYS/GNU tar from PATH, which treats
    // Windows absolute paths like "C:\..." as remote hosts.
    if constexpr (is_windows) {
        auto const* sysroot = std::getenv("SystemRoot");
        if (sysroot) {
            auto p = std::filesystem::path{sysroot} / "System32" / "tar.exe";
            if (std::filesystem::exists(p)) {
                return std::format("\"{}\"", p.string());
            }
        }
    }
    return "tar";
}

std::string sha256_command(std::filesystem::path const& file) {
    if constexpr (is_windows) {
        return std::format("certutil -hashfile \"{}\" SHA256", file.string());
    } else {
        return std::format("shasum -a 256 '{}'", file.string());
    }
}

std::string parse_sha256(std::string const& output) {
    if constexpr (is_windows) {
        // certutil output: 1st line header, 2nd line hash, 3rd line footer
        auto first_nl = output.find('\n');
        if (first_nl == std::string::npos) return {};
        auto start = first_nl + 1;
        auto end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        auto hash = output.substr(start, end - start);
        // Remove spaces
        std::erase(hash, ' ');
        std::erase(hash, '\r');
        // Convert to lowercase
        for (auto& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return hash;
    } else {
        return output.substr(0, output.find(' '));
    }
}

std::string shell_quote(std::filesystem::path const& p) {
    if constexpr (is_windows)
        return std::format("\"{}\"", p.string());
    else
        return std::format("'{}'", p.string());
}

int strip_depth(std::string_view prefix) {
    if (prefix.empty()) return 0;
    int depth = 1;
    for (auto c : prefix)
        if (c == '/') ++depth;
    return depth;
}

int extract_archive(std::filesystem::path const& archive,
                    std::filesystem::path const& staging,
                    registry::ToolInfo const& info) {
    int depth = strip_depth(info.strip_prefix);

    if (info.archive_type == "tar.xz" || info.archive_type == "tar.gz") {
        if constexpr (is_windows) {
            return run(std::format("\"{} xf \"{}\" --strip-components={} -C \"{}\"\"",
                tar_command(), archive.string(), depth, staging.string()));
        } else {
            return run(std::format("tar xf '{}' --strip-components={} -C '{}'",
                archive.string(), depth, staging.string()));
        }
    } else if (info.archive_type == "zip") {
        if constexpr (is_windows) {
            return run(std::format("\"{} xf \"{}\" --strip-components={} -C \"{}\"\"",
                tar_command(), archive.string(), depth, staging.string()));
        } else {
            if (depth > 0) {
                int rc = run(std::format("unzip -qo '{}' -d '{}'",
                    archive.string(), staging.string()));
                if (rc == 0) {
                    auto inner = staging / info.strip_prefix;
                    auto tmp = staging.parent_path() / (staging.filename().string() + ".tmp");
                    std::filesystem::rename(inner, tmp);
                    std::filesystem::remove_all(staging);
                    std::filesystem::rename(tmp, staging);
                }
                return rc;
            } else {
                return run(std::format("unzip -qo '{}' -d '{}'",
                    archive.string(), staging.string()));
            }
        }
    }
    return 1; // unknown archive type
}

bool verify_checksum(std::filesystem::path const& archive,
                     std::string_view archive_name,
                     std::filesystem::path const& downloads,
                     std::string const& checksum_url) {
    auto checksum_file = downloads / "checksums.txt";
    std::string dl_cmd;
    if constexpr (is_windows) {
        dl_cmd = std::format("curl -fsSL --compressed -o \"{}\" \"{}\"",
            checksum_file.string(), checksum_url);
    } else {
        dl_cmd = std::format("curl -fsSL --compressed -o '{}' '{}'",
            checksum_file.string(), checksum_url);
    }
    if (run(dl_cmd) != 0) {
        std::println("warning: could not download checksum file, skipping verification");
        return true;
    }

    auto raw = capture(sha256_command(archive));
    auto actual_hash = parse_sha256(raw);

    bool verified = false;
    bool mismatch = false;
    std::string expected_hash;
    {
        auto in = std::ifstream{checksum_file};
        std::string line;
        while (std::getline(in, line)) {
            if (line.contains(archive_name)) {
                expected_hash = line.substr(0, line.find(' '));
                if (actual_hash == expected_hash)
                    verified = true;
                else
                    mismatch = true;
                break;
            }
        }
    }
    std::filesystem::remove(checksum_file);

    if (mismatch) {
        std::println(std::cerr,
            "error: checksum mismatch\n  expected: {}\n  actual:   {}",
            expected_hash, actual_hash);
        return false;
    }
    if (verified)
        std::println("Checksum OK");
    else
        std::println("warning: checksum entry not found, skipping verification");
    return true;
}

void write_clang_wrapper(std::filesystem::path const& bin_dir,
                          std::string const& cfg_flag) {
    for (auto name : {"clang", "clang++"}) {
        auto orig = bin_dir / name;
        auto backup = bin_dir / std::format("{}.orig", name);
        if (std::filesystem::exists(orig) && !std::filesystem::exists(backup)) {
            std::filesystem::rename(orig, backup);
            auto out = std::ofstream{orig};
            if (out) {
                out << "#!/bin/sh\n";
                out << std::format("exec \"{}\" {} \"$@\"\n",
                    backup.string(), cfg_flag);
            }
            std::filesystem::permissions(orig,
                std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec,
                std::filesystem::perm_options::add);
        }
    }
}

void setup_llvm_config(std::filesystem::path const& dest, std::string_view version) {
    auto plat = registry::detect_platform();
    auto clang = dest / "bin" / "clang";
    auto target = capture(std::format("'{}' -dumpmachine", clang.string()));

    if (plat.os == registry::OS::macOS) {
        auto sdk_path = capture("xcrun --show-sdk-path");
        if (!target.empty() && !sdk_path.empty()) {
            auto cfg_target = target;
            auto darwin_pos = cfg_target.find("darwin");
            if (darwin_pos != std::string::npos) {
                auto ver_start = darwin_pos + 6;
                auto dot = cfg_target.find('.', ver_start);
                if (dot != std::string::npos)
                    cfg_target = cfg_target.substr(0, dot);
            }

            auto cfg_dir = dest / "etc" / "clang";
            std::filesystem::create_directories(cfg_dir);
            auto cfg_file = cfg_dir / std::format("{}.cfg", cfg_target);
            {
                auto lib_dir = dest / "lib";
                auto out = std::ofstream{cfg_file};
                if (out) {
                    out << std::format("-isysroot {}\n", sdk_path);
                    out << "-stdlib=libc++\n";
                    out << "-lc++\n";
                    out << std::format("-L{}\n", lib_dir.string());
                    out << std::format("-Wl,-rpath,{}\n", lib_dir.string());
                }
            }

            write_clang_wrapper(dest / "bin",
                std::format("--config-system-dir={}", cfg_dir.string()));
            std::println("Generated clang config: {}", cfg_file.string());
        }
    } else if (plat.os == registry::OS::Linux && !target.empty()) {
        auto lib_dir = dest / "lib" / target;
        if (!std::filesystem::exists(lib_dir / "libc++.so") &&
            !std::filesystem::exists(lib_dir / "libc++.a"))
            lib_dir = dest / "lib";

        auto cfg_dir = dest / "etc" / "clang";
        std::filesystem::create_directories(cfg_dir);
        auto cfg_file = cfg_dir / std::format("{}.cfg", target);
        {
            auto out = std::ofstream{cfg_file};
            if (out) {
                out << "-stdlib=libc++\n";
                out << "-lc++\n";
                out << "-lc++abi\n";
                out << std::format("-L{}\n", lib_dir.string());
                out << std::format("-Wl,-rpath,{}\n", lib_dir.string());
            }
        }

        write_clang_wrapper(dest / "bin",
            std::format("--config-system-dir={}", cfg_dir.string()));
        std::println("Generated clang config: {}", cfg_file.string());
    }
}

// Rebuild libc++ and libc++abi with hermetic static library flags so
// the toolchain's C++ standard library can coexist with the macOS
// system libc++ (loaded by Metal/Foundation frameworks). Uses a custom
// ABI namespace (__intron) and hidden symbols to avoid ODR violations.
void rebuild_hermetic_libcxx(std::filesystem::path const& dest, std::string_view version) {
    std::println("Rebuilding libc++ with hermetic ABI namespace...");

    auto tmp = std::filesystem::temp_directory_path() / "intron_libcxx_build";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    // Download llvm-project source (just runtimes needed)
    auto src_name = std::format("llvm-project-{}.src", version);
    auto tarball = std::format("{}.tar.xz", src_name);
    auto tarball_url = std::format(
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-{}/{}",
        version, tarball);
    auto tarball_path = tmp / tarball;

    std::println("  Downloading libc++ source...");
    auto dl_cmd = std::format("curl -fSL --compressed -o '{}' '{}'",
        tarball_path.string(), tarball_url);
    if (run(dl_cmd) != 0) {
        std::println(std::cerr, "warning: hermetic libc++ rebuild skipped (download failed)");
        std::filesystem::remove_all(tmp);
        return;
    }

    // Extract just the runtimes directory
    std::println("  Extracting...");
    auto extract_cmd = std::format(
        "tar -xf '{}' -C '{}' '{}/runtimes' '{}/libcxx' '{}/libcxxabi' '{}/cmake' '{}/llvm' '{}/libc'",
        tarball_path.string(), tmp.string(),
        src_name, src_name, src_name, src_name, src_name, src_name);
    if (run(extract_cmd) != 0) {
        std::println(std::cerr, "warning: hermetic libc++ rebuild skipped (extraction failed)");
        std::filesystem::remove_all(tmp);
        return;
    }

    auto src_dir = tmp / src_name;
    auto build_dir = tmp / "build";
    std::filesystem::create_directories(build_dir);

    // Find cmake and ninja from the toolchain
    auto clang = dest / "bin" / "clang";
    auto clangxx = dest / "bin" / "clang++";
    // Use the real clang, not the wrapper (which may reference the config
    // we just wrote — and that config may reference libc++ flags that
    // won't work for building libc++ itself).
    auto clang_real = dest / "bin" / "clang.orig";
    if (!std::filesystem::exists(clang_real))
        clang_real = clang;
    auto clangxx_real = dest / "bin" / "clang++.orig";
    if (!std::filesystem::exists(clangxx_real))
        clangxx_real = clangxx;

    // Detect sysroot on macOS
    auto plat = registry::detect_platform();
    std::string sysroot_flag;
    if (plat.os == registry::OS::macOS) {
        auto sdk = capture("xcrun --show-sdk-path");
        if (!sdk.empty())
            sysroot_flag = std::format(" -DCMAKE_OSX_SYSROOT={}", sdk);
    }

    // Find cmake and ninja from intron's toolchains (fall back to system PATH)
    auto home = intron_home();
    std::string cmake_bin = "cmake";
    std::string ninja_bin = "ninja";
    if (auto cmake_dir = home / "toolchains" / "cmake"; std::filesystem::is_directory(cmake_dir)) {
        for (auto const& entry : std::filesystem::directory_iterator(cmake_dir)) {
            auto candidate = entry.path() / "bin" / "cmake";
            if (std::filesystem::exists(candidate)) { cmake_bin = candidate.string(); break; }
        }
    }
    if (auto ninja_dir = home / "toolchains" / "ninja"; std::filesystem::is_directory(ninja_dir)) {
        for (auto const& entry : std::filesystem::directory_iterator(ninja_dir)) {
            auto candidate = entry.path() / "ninja";
            if (std::filesystem::exists(candidate)) { ninja_bin = candidate.string(); break; }
        }
    }

    // Configure — prefer Ninja, fall back to Unix Makefiles (make is
    // always available on macOS via Xcode CLT)
    std::println("  Configuring hermetic libc++...");
    bool has_ninja = (ninja_bin != "ninja") || check_command("ninja");
    std::string cmake_cmd;
    if (has_ninja) {
        cmake_cmd = std::format(
            "'{}' -G Ninja -DCMAKE_MAKE_PROGRAM='{}' -S '{}' -B '{}'",
            cmake_bin, ninja_bin,
            (src_dir / "runtimes").string(), build_dir.string());
    } else {
        cmake_cmd = std::format(
            "'{}' -G 'Unix Makefiles' -S '{}' -B '{}'",
            cmake_bin,
            (src_dir / "runtimes").string(), build_dir.string());
    }
    cmake_cmd += std::format(
        " -DCMAKE_C_COMPILER='{}'"
        " -DCMAKE_CXX_COMPILER='{}'"
        " -DLLVM_PATH='{}'"
        " -DLLVM_ENABLE_RUNTIMES='libcxx;libcxxabi'",
        clang_real.string(), clangxx_real.string(),
        src_dir.string());
    cmake_cmd += " -DLIBCXXABI_USE_LLVM_UNWINDER=OFF"
        " -DLIBCXX_INCLUDE_TESTS=OFF"
        " -DLIBCXX_INCLUDE_BENCHMARKS=OFF"
        " -DLIBCXXABI_INCLUDE_TESTS=OFF"
        " -DLIBCXX_ABI_NAMESPACE=__intron"
        " -DLIBCXX_ENABLE_SHARED=ON"
        " -DLIBCXX_ENABLE_STATIC=ON"
        " -DCMAKE_BUILD_TYPE=Release";
    cmake_cmd += sysroot_flag;
    if (run(cmake_cmd) != 0) {
        std::println(std::cerr, "warning: hermetic libc++ rebuild skipped (cmake configure failed)");
        std::filesystem::remove_all(tmp);
        return;
    }

    // Build
    std::println("  Building...");
    auto build_cmd = std::format(
        "'{}' --build '{}' --target cxx_static cxxabi_static cxx_shared cxxabi_shared",
        cmake_bin, build_dir.string());
    if (run(build_cmd) != 0) {
        std::println(std::cerr, "warning: hermetic libc++ rebuild skipped (build failed)");
        std::filesystem::remove_all(tmp);
        return;
    }

    // Copy results
    std::println("  Installing hermetic libc++ to toolchain...");
    auto lib_dir = dest / "lib";
    auto include_dir = dest / "include" / "c++" / "v1";

    auto built_lib = build_dir / "lib";
    for (auto const& name : {"libc++.a", "libc++abi.a"}) {
        auto src = built_lib / name;
        if (std::filesystem::exists(src))
            std::filesystem::copy_file(src, lib_dir / name,
                std::filesystem::copy_options::overwrite_existing);
    }
    // Also copy dynamic libs + versioned symlinks
    for (auto const& name : {
        "libc++.dylib", "libc++.1.dylib", "libc++.1.0.dylib",
        "libc++abi.dylib", "libc++abi.1.dylib", "libc++abi.1.0.dylib",
        "libc++.so", "libc++.so.1", "libc++.so.1.0",
        "libc++abi.so", "libc++abi.so.1", "libc++abi.so.1.0"
    }) {
        auto src = built_lib / name;
        if (std::filesystem::exists(src)) {
            auto dst = lib_dir / name;
            std::filesystem::remove(dst);
            std::filesystem::copy(src, dst,
                std::filesystem::copy_options::overwrite_existing |
                std::filesystem::copy_options::copy_symlinks);
        }
    }

    // Copy __config_site header (defines _LIBCPP_ABI_NAMESPACE=__intron)
    auto config_site = build_dir / "include" / "c++" / "v1" / "__config_site";
    if (std::filesystem::exists(config_site))
        std::filesystem::copy_file(config_site, include_dir / "__config_site",
            std::filesystem::copy_options::overwrite_existing);

    // Cleanup
    std::filesystem::remove_all(tmp);
    std::println("  Hermetic libc++ installed (ABI namespace: __intron)");
}

bool verify_installed_binary(std::filesystem::path const& dest, registry::ToolInfo const& info) {
    std::filesystem::path verify_bin;
    if (info.name == "llvm")
        verify_bin = dest / "bin" / "clang++";
    else if (info.name == "ninja")
        verify_bin = dest / "ninja";
    else if (info.name == "wasi-sdk")
        verify_bin = dest / "bin" / "clang++";
    else
        verify_bin = dest / "bin" / info.name;

    if (!std::filesystem::exists(verify_bin))
        return true;

    auto cmd = std::format("{} --version", shell_quote(verify_bin));
    std::string redirect;
    if constexpr (is_windows)
        redirect = " > NUL 2>&1";
    else
        redirect = " > /dev/null 2>&1";
    if (run(cmd + redirect) != 0) {
        std::println(std::cerr,
            "error: {} binary is not executable on this platform\n"
            "hint: the downloaded binary may not match your architecture",
            info.name);
        return false;
    }
    return true;
}

// Detect MSVC installation via vswhere (Windows only)
std::optional<std::filesystem::path> detect_msvc_path() {
    if constexpr (!is_windows) return std::nullopt;

    // vswhere ships with VS Build Tools / VS Installer
    std::string vswhere_cmd;
    auto const* pf86 = std::getenv("ProgramFiles(x86)");
    if (pf86) {
        auto vswhere = std::filesystem::path{pf86}
            / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
        if (std::filesystem::exists(vswhere))
            vswhere_cmd = std::format("\"{}\"", vswhere.string());
    }
    if (vswhere_cmd.empty()) {
        // Try PATH
        if (check_command("vswhere"))
            vswhere_cmd = "vswhere";
        else
            return std::nullopt;
    }

    auto install_path = capture(
        std::format("{} -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath",
            vswhere_cmd));
    if (install_path.empty()) return std::nullopt;

    // Find the latest MSVC toolset version
    auto vc_tools = std::filesystem::path{install_path} / "VC" / "Tools" / "MSVC";
    if (!std::filesystem::exists(vc_tools)) return std::nullopt;

    std::string latest_ver;
    for (auto const& entry : std::filesystem::directory_iterator{vc_tools}) {
        if (!entry.is_directory()) continue;
        auto ver = entry.path().filename().string();
        if (ver > latest_ver) latest_ver = ver;
    }
    if (latest_ver.empty()) return std::nullopt;

    return vc_tools / latest_ver;
}

} // namespace detail

// Install a system tool (e.g. msvc) — detect and verify, no download
bool install_system_tool(registry::ToolInfo const& info) {
    if (info.name == "msvc") {
        auto path = detail::detect_msvc_path();
        if (!path) {
            std::println(std::cerr,
                "error: MSVC is not installed\n"
                "hint: install Visual Studio or Build Tools with C++ workload");
            return false;
        }
        std::println("msvc detected at {}", path->string());
        return true;
    }
    std::println(std::cerr, "error: unknown system tool: {}", info.name);
    return false;
}

bool install(registry::ToolInfo const& info) {
    // System tools (e.g. msvc) are detected, not downloaded
    if (registry::is_system_tool(info.name))
        return install_system_tool(info);

    auto dest = toolchain_path(info.name, info.version);

    // Already installed
    if (std::filesystem::exists(dest) && !std::filesystem::is_empty(dest)) {
        std::println("{} {} is already installed", info.name, info.version);
        return true;
    }

    // Preflight check
    if (!detail::check_command("curl")) {
        std::println(std::cerr, "error: curl is required but not found");
        return false;
    }
    if ((info.archive_type == "tar.xz" || info.archive_type == "tar.gz")
        && !detail::check_command("tar")) {
        std::println(std::cerr, "error: tar is required but not found");
        return false;
    }
    if constexpr (!detail::is_windows) {
        if (info.archive_type == "zip" && !detail::check_command("unzip")) {
            std::println(std::cerr, "error: unzip is required but not found");
            return false;
        }
    }

    auto downloads = intron_home() / "downloads";
    std::filesystem::create_directories(downloads);

    // Extract archive filename from URL
    auto url_sv = std::string_view{info.url};
    auto slash = url_sv.rfind('/');
    auto archive_name = std::string{url_sv.substr(slash + 1)};
    auto archive_path = downloads / archive_name;

    // Cleanup guard on failure
    bool success = false;
    auto cleanup = [&] {
        if (!success) {
            std::filesystem::remove_all(dest);
        }
    };

    // Download (reuse cached archive if available)
    if (std::filesystem::exists(archive_path)) {
        std::println("Using cached archive for {} {}...", info.name, info.version);
    } else {
        std::println("Downloading {} {}...", info.name, info.version);
        std::string curl_cmd;
        if constexpr (detail::is_windows) {
            curl_cmd = std::format("curl -fSL --compressed -o \"{}\" \"{}\"",
                archive_path.string(), info.url);
        } else {
            curl_cmd = std::format("curl -fSL --compressed -o '{}' '{}'",
                archive_path.string(), info.url);
        }
        if (detail::run(curl_cmd) != 0) {
            std::println(std::cerr, "error: download failed");
            std::filesystem::remove(archive_path);
            cleanup();
            return false;
        }
    }

    // Checksum verification
    if (!info.checksum_url.empty()) {
        std::println("Verifying checksum...");
        if (!detail::verify_checksum(archive_path, archive_name, downloads, info.checksum_url)) {
            cleanup();
            return false;
        }
    }

    // Extract to staging directory (atomic install: staging → rename)
    auto staging = intron_home() / "staging" / std::format("{}-{}", info.name, info.version);
    std::filesystem::create_directories(staging);

    std::println("Extracting...");
    if (info.archive_type != "tar.xz" && info.archive_type != "tar.gz" &&
        info.archive_type != "zip") {
        std::println(std::cerr, "error: unknown archive type: {}", info.archive_type);
        std::filesystem::remove_all(staging);
        cleanup();
        return false;
    }
    if (detail::extract_archive(archive_path, staging, info) != 0) {
        std::println(std::cerr, "error: extraction failed");
        std::filesystem::remove_all(staging);
        cleanup();
        return false;
    }

    // Atomically place at final destination
    std::filesystem::create_directories(dest.parent_path());
    std::filesystem::rename(staging, dest);

    // Generate clang config + wrapper scripts after LLVM install
    if (info.name == "llvm") {
        detail::setup_llvm_config(dest, info.version);
        if (registry::detect_platform().os == registry::OS::macOS)
            detail::rebuild_hermetic_libcxx(dest, info.version);
    }

    // Verify the installed binary is executable on this platform
    if (!detail::verify_installed_binary(dest, info)) {
        std::filesystem::remove_all(dest);
        cleanup();
        return false;
    }

    success = true;
    std::println("Installed {} {} to {}", info.name, info.version, dest.string());
    return true;
}

bool remove(std::string_view tool, std::string_view version) {
    auto path = toolchain_path(tool, version);
    if (!std::filesystem::exists(path)) {
        std::println(std::cerr, "error: {} {} is not installed", tool, version);
        return false;
    }
    std::filesystem::remove_all(path);
    std::println("Removed {} {}", tool, version);
    return true;
}

std::vector<std::pair<std::string, std::string>> list_installed() {
    std::vector<std::pair<std::string, std::string>> result;
    auto toolchains = intron_home() / "toolchains";
    if (!std::filesystem::exists(toolchains)) {
        return result;
    }
    for (auto const& tool_entry : std::filesystem::directory_iterator{toolchains}) {
        if (!tool_entry.is_directory()) continue;
        auto tool_name = tool_entry.path().filename().string();
        for (auto const& ver_entry : std::filesystem::directory_iterator{tool_entry.path()}) {
            if (!ver_entry.is_directory()) continue;
            result.emplace_back(tool_name, ver_entry.path().filename().string());
        }
    }
    std::ranges::sort(result);
    return result;
}

std::optional<std::filesystem::path> which(
    std::string_view binary, std::string_view tool, std::string_view version)
{
    auto base = toolchain_path(tool, version);
    auto path = (tool == "ninja")
        ? base / binary
        : base / "bin" / binary;

    if (std::filesystem::exists(path)) {
        return path;
    }
#ifdef _WIN32
    // Windows: try with .exe extension
    auto exe_path = path;
    exe_path += ".exe";
    if (std::filesystem::exists(exe_path)) {
        return exe_path;
    }
#endif
    return std::nullopt;
}

std::optional<std::filesystem::path> msvc_path() {
    return detail::detect_msvc_path();
}

std::optional<std::string> latest_version(std::string_view tool) {
    auto api_url = registry::latest_release_api(tool);
    std::string curl_cmd;
    if constexpr (detail::is_windows) {
        curl_cmd = std::format("curl -fsSL \"{}\"", api_url);
    } else {
        curl_cmd = std::format("curl -fsSL '{}'", api_url);
    }
    auto json = detail::capture(curl_cmd);
    if (json.empty()) return std::nullopt;

    auto pos = json.find("\"tag_name\"");
    if (pos == std::string::npos) return std::nullopt;
    auto colon = json.find(':', pos);
    auto quote1 = json.find('"', colon + 1);
    auto quote2 = json.find('"', quote1 + 1);
    if (quote1 == std::string::npos || quote2 == std::string::npos) return std::nullopt;

    auto tag = json.substr(quote1 + 1, quote2 - quote1 - 1);

    if (tag.starts_with("v")) {
        return tag.substr(1);
    }
    if (auto dash = tag.rfind('-'); dash != std::string::npos) {
        return tag.substr(dash + 1);
    }
    return tag;
}

} // namespace installer
