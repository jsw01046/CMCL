#include <windows.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>

#include "core/encoding.h"
#include "core/log.h"
#include "mc/download.h"
#include "mc/version.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "[ok] " << message << '\n';
    } else {
        std::cerr << "[FAIL] " << message << '\n';
        ++failures;
    }
}

void print_progress(const cl::mc::DownloadProgress& progress) {
    std::cout << '\r' << '[' << progress.stage << "] "
              << progress.completed_files << '/' << progress.total_files;
    if (progress.total_bytes > 0) {
        std::cout << "  " << progress.downloaded_bytes / (1024 * 1024) << " / "
                  << progress.total_bytes / (1024 * 1024) << " MB";
    }
    if (!progress.file.empty()) {
        std::cout << "  " << progress.file.substr(0, 70);
    }
    std::cout << "      " << std::flush;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    const std::string version =
        argc > 1 ? cl::to_utf8(argv[1]) : std::string("1.5.2");
    const std::string name =
        argc > 2 ? cl::to_utf8(argv[2]) : std::string("m3probe");
    const bool keep = argc > 3 && cl::to_utf8(argv[3]) == "--keep";

    const fs::path root =
        fs::temp_directory_path() /
        ("craftlauncher_m3_probe_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) {
        std::cerr << "无法创建临时目录: " << root << '\n';
        return 1;
    }

    cl::Log::init(root / L"logs" / L"probe.log", true);
    cl::mc::DownloadRequest request;
    request.mc_dir = root;
    request.version_id = version;
    request.instance_name = name;
    request.source = cl::mc::DownloadSource::Auto;

    cl::mc::DownloadCancelToken cancel{false};
    std::cout << "安装 " << version << " 到 " << cl::path_to_utf8(root) << '\n';
    auto installed = cl::mc::install_version(request, print_progress, cancel);
    std::cout << '\n';
    if (!installed) {
        std::cerr << "安装失败: " << installed.error().to_string() << '\n';
        return 1;
    }

    const fs::path version_dir = root / L"versions" / cl::path_from_utf8(name);
    const fs::path json_path = version_dir / (cl::path_from_utf8(name).wstring() + L".json");
    const fs::path client_jar = version_dir / (cl::path_from_utf8(name).wstring() + L".jar");
    check(fs::is_regular_file(json_path), "版本 JSON 存在");
    check(fs::is_regular_file(client_jar), "客户端 jar 存在");
    auto json = cl::mc::load_json(json_path);
    check(json.ok(), "版本 JSON 可解析");
    check(fs::is_directory(root / L"assets" / L"objects"), "assets/objects 存在");
    check(fs::is_directory(root / L"libraries"), "libraries 存在");
    if (json.ok()) {
        std::string index = "legacy";
        if (json.value().contains("assetIndex") &&
            json.value()["assetIndex"].is_object()) {
            index = json.value()["assetIndex"].value("id", "legacy");
        }
        check(fs::is_regular_file(root / L"assets" / L"indexes" /
                                  cl::path_from_utf8(index + ".json")),
              "资源索引存在 (" + index + ")");
        const std::string assets =
            json.value().value("assets", std::string{});
        if (index == "legacy" || assets == "pre-1.6") {
            check(fs::is_directory(root / L"assets" / L"virtual" / L"legacy"),
                  "legacy 虚拟资源目录存在");
            bool has_file = false;
            std::error_code virtual_ec;
            for (const auto& entry : fs::recursive_directory_iterator(
                     root / L"assets" / L"virtual" / L"legacy", virtual_ec)) {
                if (entry.is_regular_file()) {
                    has_file = true;
                    break;
                }
            }
            check(has_file, "legacy 虚拟资源包含文件");
        }
    }

    // 修复：删掉客户端 jar 和一个库文件，再让修复流程补回来。
    fs::remove(client_jar, ec);
    check(!fs::exists(client_jar), "已删除客户端 jar（模拟损坏）");

    fs::path removed_library;
    if (fs::is_directory(root / L"libraries", ec)) {
        for (const auto& entry :
             fs::recursive_directory_iterator(root / L"libraries", ec)) {
            if (entry.is_regular_file() && entry.path().extension() == L".jar") {
                removed_library = entry.path();
                break;
            }
        }
    }
    if (!removed_library.empty()) {
        fs::remove(removed_library, ec);
    }

    std::cout << "开始修复…\n";
    // repair 的 version_id 是 versions 下的实例文件夹名，而不是原版版本号。
    cl::mc::DownloadRequest repair_request = request;
    repair_request.version_id = name;
    auto repaired = cl::mc::repair_version(repair_request, print_progress, cancel);
    std::cout << '\n';
    if (!repaired) {
        std::cerr << "修复失败: " << repaired.error().to_string() << '\n';
        return 1;
    }
    check(fs::is_regular_file(client_jar), "客户端 jar 已修复");
    if (!removed_library.empty()) {
        check(fs::is_regular_file(removed_library), "库文件已修复");
    }

    if (!keep) {
        fs::remove_all(root, ec);
    } else {
        std::cout << "保留测试目录: " << cl::path_to_utf8(root) << '\n';
    }

    cl::Log::shutdown();
    std::cout << (failures == 0 ? "net_probe: all checks passed\n"
                                : "net_probe: FAILED\n");
    return failures == 0 ? 0 : 1;
}
