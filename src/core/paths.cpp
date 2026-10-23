#include "core/paths.h"

#include <windows.h>

#include "core/encoding.h"

namespace cl {

namespace {

std::filesystem::path env_path(const wchar_t* name) {
    wchar_t buffer[32768]{};
    const DWORD size = ::GetEnvironmentVariableW(name, buffer, 32768);
    if (size == 0 || size >= 32768) return {};
    return std::filesystem::path(buffer);
}

} // namespace

std::filesystem::path exe_path() {
    wchar_t buffer[32768]{};
    const DWORD size = ::GetModuleFileNameW(nullptr, buffer, 32768);
    if (size == 0 || size >= 32768) return std::filesystem::current_path();
    return std::filesystem::path(buffer);
}

std::filesystem::path exe_dir() {
    return exe_path().parent_path();
}

std::filesystem::path default_mc_dir() {
    const std::filesystem::path appdata = env_path(L"APPDATA");
    if (!appdata.empty()) return appdata / L".minecraft";
    return std::filesystem::current_path() / L".minecraft";
}

std::filesystem::path local_app_data_dir() {
    const std::filesystem::path local = env_path(L"LOCALAPPDATA");
    if (!local.empty()) return local;
    return std::filesystem::temp_directory_path();
}

Result<void> ensure_app_dirs() {
    const std::filesystem::path root = local_app_data_dir() / L"CraftLauncher";
    const std::filesystem::path dirs[] = {
        root,
        root / L"cache",
        root / L"jdk",
        root / L"natives",
    };

    std::error_code ec;
    for (const auto& dir : dirs) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return Error::make(5, "无法创建启动器数据目录",
                               path_to_utf8(dir) + ": " + ec.message());
        }
    }
    return {};
}

} // namespace cl
