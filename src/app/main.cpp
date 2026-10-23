#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "core/encoding.h"
#include "core/log.h"
#include "core/paths.h"
#include "mc/diagnostics.h"
#include "mc/download.h"
#include "mc/instance.h"
#include "mc/java.h"
#include "mc/jdk.h"
#include "mc/launch.h"
#include "mc/loader.h"

namespace {

using cl::path_from_utf8;
using cl::path_to_utf8;
using cl::to_utf8;

bool has_arg(const std::vector<std::string>& args, const std::string& name) {
    for (const auto& arg : args) {
        if (arg == name) return true;
    }
    return false;
}

std::string arg_value(const std::vector<std::string>& args, const std::string& name,
                      const std::string& fallback = {}) {
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == name) return args[i + 1];
    }
    return fallback;
}

int arg_int(const std::vector<std::string>& args, const std::string& name, int fallback) {
    const std::string text = arg_value(args, name);
    if (text.empty()) return fallback;
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

std::filesystem::path exe_dir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD size = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (size == 0) return std::filesystem::current_path();
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path default_mc_dir() {
    wchar_t buffer[32768]{};
    const DWORD size = ::GetEnvironmentVariableW(L"APPDATA", buffer, 32768);
    if (size > 0 && size < 32768) {
        return std::filesystem::path(buffer) / L".minecraft";
    }
    return std::filesystem::current_path() / L".minecraft";
}

void print_error(const cl::Error& error) {
    std::cerr << "错误：" << error.message;
    if (!error.context.empty()) std::cerr << "\n  " << error.context;
    std::cerr << '\n';
}

int command_list(const std::vector<std::string>& args) {
    const std::filesystem::path mc_dir =
        path_from_utf8(arg_value(args, "--mc-dir", path_to_utf8(default_mc_dir())));

    const int removed_logs = cl::cleanup_game_logs(mc_dir);
    if (removed_logs > 0) {
        std::cout << "已清理 " << removed_logs << " 个超过 7 天的游戏日志\n";
    }

    auto instances = cl::mc::scan_instances(mc_dir);
    if (!instances) {
        print_error(instances.error());
        return 1;
    }

    if (instances.value().empty()) {
        std::cout << "没有找到任何版本（" << path_to_utf8(mc_dir / L"versions") << "）\n";
        return 0;
    }

    for (const auto& instance : instances.value()) {
        std::cout << instance.id;
        if (!instance.type.empty()) std::cout << "  [" << instance.type << "]";
        if (instance.inherits_from) std::cout << "  inherits=" << *instance.inherits_from;
        if (!instance.main_class.empty()) std::cout << "  " << instance.main_class;
        if (!instance.valid) std::cout << "  <无效: " << instance.error << ">";
        std::cout << '\n';
    }
    return 0;
}

int command_java(const std::vector<std::string>& args) {
    const std::filesystem::path mc_dir =
        path_from_utf8(arg_value(args, "--mc-dir", path_to_utf8(default_mc_dir())));
    const auto javas = cl::mc::detect_java(mc_dir);
    if (javas.empty()) {
        std::cout << "没有检测到 Java\n";
        return 1;
    }
    for (const auto& java : javas) {
        std::cout << "Java " << java.version << " (" << java.major << ", "
                  << java.bits << " 位)  " << path_to_utf8(java.exe) << '\n';
    }
    return 0;
}

cl::mc::LaunchOptions make_launch_options(const std::vector<std::string>& args) {
    cl::mc::LaunchOptions options;
    options.mc_dir =
        path_from_utf8(arg_value(args, "--mc-dir", path_to_utf8(default_mc_dir())));
    options.version_id = arg_value(args, "--version");
    options.player_name = arg_value(args, "--username", "Player");
    options.uuid = arg_value(args, "--uuid");
    options.access_token = arg_value(args, "--access-token", "0");
    options.user_type = arg_value(args, "--user-type", "legacy");
    options.min_memory_mb = arg_int(args, "--min-mem", 512);
    options.max_memory_mb = arg_int(args, "--max-mem", 4096);
    options.width = arg_int(args, "--width", 854);
    options.height = arg_int(args, "--height", 480);
    options.demo = has_arg(args, "--demo");
    options.fullscreen = has_arg(args, "--fullscreen");
    options.server = arg_value(args, "--server");
    if (const std::string java = arg_value(args, "--java"); !java.empty()) {
        options.java_exe = path_from_utf8(java);
    }
    return options;
}

int command_inspect(const std::vector<std::string>& args) {
    auto options = make_launch_options(args);
    if (options.version_id.empty()) {
        std::cerr << "需要 --version <版本名>\n";
        return 2;
    }

    auto prepared = cl::mc::prepare_launch(options, false);
    if (!prepared) {
        print_error(prepared.error());
        return 1;
    }
    const auto& launch = prepared.value();

    std::cout << "实例:        " << launch.version.id << '\n';
    std::cout << "原版版本:    " << launch.version.vanilla_id << '\n';
    std::cout << "主 jar:      " << launch.version.jar_id << '\n';
    std::cout << "主类:        " << launch.main_class << '\n';
    std::cout << "Java:        " << path_to_utf8(launch.java_exe) << '\n';
    std::cout << "游戏目录:    " << path_to_utf8(launch.game_dir) << '\n';
    std::cout << "Natives:     " << path_to_utf8(launch.natives_dir) << '\n';
    std::cout << "命令行长度:  " << launch.command_line.size() << '\n';

    std::cout << "\nClasspath:\n";
    size_t start = 0;
    while (start <= launch.classpath.size()) {
        const size_t pos = launch.classpath.find(';', start);
        std::cout << "  " << launch.classpath.substr(
            start, pos == std::string::npos ? std::string::npos : pos - start) << '\n';
        if (pos == std::string::npos) break;
        start = pos + 1;
    }

    std::cout << "\nJVM 参数 (" << launch.jvm_args.size() << "):\n";
    for (const auto& arg : launch.jvm_args) std::cout << "  " << arg << '\n';

    std::cout << "\n游戏参数 (" << launch.game_args.size() << "):\n";
    for (const auto& arg : launch.game_args) std::cout << "  " << arg << '\n';
    return 0;
}

int command_launch(const std::vector<std::string>& args) {
    auto options = make_launch_options(args);
    if (options.version_id.empty()) {
        std::cerr << "需要 --version <版本名>\n";
        return 2;
    }

    auto prepared = cl::mc::prepare_launch(options, true);
    if (!prepared) {
        print_error(prepared.error());
        return 1;
    }

    std::cout << "正在启动 " << prepared.value().version.id << " ...\n";
    std::vector<std::string> game_lines;
    auto started = cl::mc::launch_prepared(
        prepared.value(), [&game_lines](std::string_view line) {
            game_lines.emplace_back(line);
            std::cout << line << '\n';
            std::cout.flush();
            cl::Log::info(line);  // 游戏输出同时写入启动器日志，方便崩溃分析
        });
    if (!started) {
        print_error(started.error());
        return 1;
    }

    auto process = std::move(started).value();
    const unsigned long code = process->wait();

    const auto log_tail = cl::mc::read_log_tail(options.mc_dir / L"logs" /
                                                L"latest.log");
    game_lines.insert(game_lines.end(), log_tail.begin(), log_tail.end());

    const auto summary = cl::mc::analyze_run(static_cast<int>(code), game_lines);
    std::cout << "\n游戏已退出，退出码 " << code << "\n\n"
              << cl::mc::format_run_summary(summary) << '\n';
    cl::Log::info("运行总结：" + summary.title + " — " + summary.detail);
    return static_cast<int>(code);
}

std::string format_mb(unsigned long long bytes) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

void print_progress(const cl::mc::DownloadProgress& progress) {
    std::cout << '\r' << '[' << progress.stage << "] "
              << progress.completed_files << '/' << progress.total_files;
    if (progress.total_bytes > 0) {
        std::cout << "  " << format_mb(progress.downloaded_bytes) << " / "
                  << format_mb(progress.total_bytes);
    }
    if (!progress.file.empty()) {
        std::cout << "  " << progress.file.substr(0, 80);
    }
    std::cout << "    " << std::flush;
}

int command_versions(const std::vector<std::string>& args) {
    const auto source =
        cl::mc::parse_download_source(arg_value(args, "--source", "auto"));
    auto manifest = cl::mc::fetch_version_manifest(source, true);
    if (!manifest) {
        print_error(manifest.error());
        return 1;
    }
    std::cout << "最新正式版: " << manifest.value().latest_release << '\n';
    std::cout << "最新快照:   " << manifest.value().latest_snapshot << '\n';
    const std::string type = arg_value(args, "--type", "release");
    for (const auto& version : manifest.value().versions) {
        if (!type.empty() && type != "all" && version.type != type) continue;
        std::cout << "  " << version.id << "  [" << version.type << "]  "
                  << version.release_time << '\n';
    }
    return 0;
}

int command_mirrors() {
    const auto mirrors = cl::mc::available_mirrors();
    std::cout << "可用镜像源（按优先级）:\n";
    for (const auto& mirror : mirrors) {
        std::cout << "  " << mirror.id << "  " << mirror.name << "  "
                  << mirror.manifest_url << '\n';
    }
    return 0;
}

int command_download(const std::vector<std::string>& args) {
    cl::mc::DownloadRequest request;
    request.mc_dir =
        path_from_utf8(arg_value(args, "--mc-dir", path_to_utf8(default_mc_dir())));
    request.version_id = arg_value(args, "--version");
    request.instance_name = arg_value(args, "--name");
    request.source =
        cl::mc::parse_download_source(arg_value(args, "--source", "auto"));
    if (request.version_id.empty()) {
        std::cerr << "需要 --version <Minecraft 版本>\n";
        return 2;
    }

    cl::mc::DownloadCancelToken cancel{false};
    auto result = cl::mc::install_version(
        request, [](const cl::mc::DownloadProgress& progress) {
            print_progress(progress);
        }, cancel);
    std::cout << '\n';
    if (!result) {
        print_error(result.error());
        return 1;
    }
    std::cout << "下载完成\n";
    return 0;
}

int command_repair(const std::vector<std::string>& args) {
    cl::mc::DownloadRequest request;
    request.mc_dir =
        path_from_utf8(arg_value(args, "--mc-dir", path_to_utf8(default_mc_dir())));
    request.version_id = arg_value(args, "--version");
    request.source =
        cl::mc::parse_download_source(arg_value(args, "--source", "auto"));
    if (request.version_id.empty()) {
        std::cerr << "需要 --version <版本名>\n";
        return 2;
    }

    cl::mc::DownloadCancelToken cancel{false};
    auto result = cl::mc::repair_version(
        request, [](const cl::mc::DownloadProgress& progress) {
            print_progress(progress);
        }, cancel);
    std::cout << '\n';
    if (!result) {
        print_error(result.error());
        return 1;
    }
    std::cout << "修复完成\n";
    return 0;
}

int command_jdk(const std::vector<std::string>& args) {
    const int install = arg_int(args, "--install", 0);
    if (install > 0) {
        cl::mc::DownloadCancelToken cancel{false};
        auto result = cl::mc::install_openjdk(
            install, has_arg(args, "--reinstall"),
            [](const cl::mc::DownloadProgress& progress) {
                print_progress(progress);
            }, cancel);
        std::cout << '\n';
        if (!result) {
            print_error(result.error());
            return 1;
        }
        const auto& java = result.value();
        std::cout << "OpenJDK 安装完成: Java " << java.version << "（"
                  << java.major << "，managed）\n  "
                  << path_to_utf8(java.exe) << '\n';
        return 0;
    }

    auto releases = cl::mc::fetch_openjdk_releases(has_arg(args, "--force"));
    if (!releases) {
        print_error(releases.error());
        return 1;
    }
    std::cout << "可下载的 OpenJDK（Adoptium Temurin）:\n";
    for (const auto& release : releases.value()) {
        std::cout << "  Java " << release.feature << "  " << release.version
                  << "  " << format_mb(release.size) << '\n';
    }
    std::cout << "安装: cmcl jdk --install <主版本>\n";
    return 0;
}

int command_loader(const std::vector<std::string>& args) {
    const std::string loader = arg_value(args, "--loader", "fabric");
    auto kind = cl::mc::parse_loader_kind(loader);
    if (!kind) {
        std::cerr << "未知的加载器：" << loader << '\n';
        return 2;
    }
    const std::string game = arg_value(args, "--game", "");
    if (game.empty()) {
        std::cerr << "需要 --game <Minecraft 版本>\n";
        return 2;
    }

    const std::string version = arg_value(args, "--version", "");
    if (version.empty()) {
        auto versions = cl::mc::fetch_loader_versions(kind.value(), game);
        if (!versions) {
            print_error(versions.error());
            return 1;
        }
        std::cout << cl::mc::loader_display_name(kind.value()) << " for "
                  << game << ":\n";
        int shown = 0;
        for (const auto& item : versions.value()) {
            std::cout << "  " << item.version
                      << (item.stable ? "  (稳定)" : "") << '\n';
            if (++shown >= 30) break;
        }
        std::cout << "安装: cmcl loader --loader " << loader << " --game "
                  << game << " --version <版本>\n";
        return 0;
    }

    cl::mc::LoaderInstallRequest request;
    request.mc_dir =
        path_from_utf8(arg_value(args, "--mc-dir", path_to_utf8(default_mc_dir())));
    request.game_version = game;
    request.loader_version = version;
    request.instance_name = arg_value(args, "--name",
                                      game + "-" +
                                          cl::mc::loader_display_name(kind.value()));
    request.kind = kind.value();
    request.source =
        cl::mc::parse_download_source(arg_value(args, "--source", "auto"));

    cl::mc::DownloadCancelToken cancel{false};
    auto result = cl::mc::install_loader(
        request, [](const cl::mc::DownloadProgress& progress) {
            print_progress(progress);
        }, cancel);
    std::cout << '\n';
    if (!result) {
        print_error(result.error());
        return 1;
    }
    std::cout << cl::mc::loader_display_name(kind.value())
              << " 安装完成：" << request.instance_name << '\n';
    return 0;
}

void print_help() {
    std::cout <<
        "CMCL CLI 0.5.1（C++ Minecraft Launcher）\n"
        "\n"
        "用法:\n"
        "  cmcl list    [--mc-dir <目录>]\n"
        "  cmcl java    [--mc-dir <目录>]\n"
        "  cmcl inspect --version <版本名> [--mc-dir <目录>] [--java <java.exe>]\n"
        "  cmcl launch  --version <版本名> [--mc-dir <目录>] [选项]\n"
        "  cmcl versions [--source auto|official|mirror] [--type release|snapshot|all]\n"
        "  cmcl mirrors\n"
        "  cmcl download --version <Minecraft 版本> [--name <实例名>] [--source auto|official|mirror]\n"
        "  cmcl repair  --version <实例名> [--source auto|official|mirror]\n"
        "  cmcl jdk     [--force]\n"
        "  cmcl jdk     --install <8|11|17|21|25> [--reinstall]\n"
        "  cmcl loader  --loader <forge|neoforge|fabric|quilt> --game <MC 版本> [--version <加载器版本>]\n"
        "\n"
        "launch / inspect 选项:\n"
        "  --username <名字>      玩家名（默认 Player）\n"
        "  --uuid <UUID>          指定 UUID（默认按离线 UUID 生成）\n"
        "  --access-token <令牌>  访问令牌（默认 0）\n"
        "  --user-type <类型>     legacy / msa（默认 legacy）\n"
        "  --java <路径>          指定 java.exe\n"
        "  --min-mem <MB>         -Xms（默认 512）\n"
        "  --max-mem <MB>         -Xmx（默认 4096）\n"
        "  --width <像素>         窗口宽度（默认 854）\n"
        "  --height <像素>        窗口高度（默认 480）\n"
        "  --demo                 以试玩模式启动\n"
        "  --fullscreen           全屏启动\n"
        "  --server <host[:port]> 启动后直接连接服务器\n"
        "  --debug                输出调试日志\n"
        "\n"
        "示例:\n"
        "  cmcl list --mc-dir \"C:\\Users\\me\\.minecraft\"\n"
        "  cmcl inspect --version 1.20.1 --mc-dir \"C:\\Users\\me\\.minecraft\"\n"
        "  cmcl launch --version 1.20.1 --username Steve\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(to_utf8(argv[i]));

    cl::Log::init(exe_dir() / L"logs" / L"cmcl.log", has_arg(args, "--debug"));
    if (auto dirs = cl::ensure_app_dirs(); !dirs) {
        cl::Log::warn("初始化启动器目录失败：" + dirs.error().to_string());
    }

    int result = 0;
    try {
        const std::string command = argc > 1 ? args[1] : "";
        if (command.empty() || command == "--help" || command == "-h" ||
            command == "help") {
            print_help();
        } else if (command == "list") {
            result = command_list(args);
        } else if (command == "java") {
            result = command_java(args);
        } else if (command == "inspect") {
            result = command_inspect(args);
        } else if (command == "launch") {
            result = command_launch(args);
        } else if (command == "versions") {
            result = command_versions(args);
        } else if (command == "mirrors") {
            result = command_mirrors();
        } else if (command == "download") {
            result = command_download(args);
        } else if (command == "repair") {
            result = command_repair(args);
        } else if (command == "jdk") {
            result = command_jdk(args);
        } else if (command == "loader") {
            result = command_loader(args);
        } else {
            std::cerr << "未知命令：" << command << "\n\n";
            print_help();
            result = 2;
        }
    } catch (const std::exception& ex) {
        std::cerr << "未处理的异常：" << ex.what() << '\n';
        result = 3;
    }

    cl::Log::shutdown();
    return result;
}
