#include "core/archive.h"

#include <windows.h>

#include "core/encoding.h"
#include "core/log.h"
#include "core/process.h"

namespace cl {

namespace {

std::filesystem::path find_tar() {
    wchar_t system_dir[MAX_PATH]{};
    const UINT size = ::GetSystemDirectoryW(system_dir, MAX_PATH);
    if (size > 0 && size < MAX_PATH) {
        std::filesystem::path candidate =
            std::filesystem::path(system_dir) / L"tar.exe";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

} // namespace

Result<void> extract_zip(const std::filesystem::path& archive,
                         const std::filesystem::path& dest,
                         const std::vector<std::string>& excludes) {
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        return Error::make(1, "无法创建解压目录", path_to_utf8(dest) + ": " + ec.message());
    }

    const std::filesystem::path tar = find_tar();
    if (tar.empty()) {
        return Error::make(2, "找不到系统 tar.exe，无法解压原生库",
                           "需要 Windows 10 1803 或更高版本");
    }

    const std::wstring command = build_command_line(
        {tar.wstring(), L"-xf", archive.wstring(), L"-C", dest.wstring()});

    auto process_result = Process::start(command, dest, {});
    if (!process_result) return process_result.error();
    auto process = std::move(process_result).value();
    const unsigned long code = process->wait();
    if (code != 0) {
        return Error::make(3, "解压失败", path_to_utf8(archive) + "，tar 退出码 " +
                                   std::to_string(code));
    }

    // 原生库 jar 里的 META-INF 不能留在 natives 目录里。
    std::filesystem::remove_all(dest / L"META-INF", ec);

    for (const std::string& item : excludes) {
        if (item.empty()) continue;
        const std::filesystem::path target = dest / path_from_utf8(item);
        if (item.back() == '/' || item.back() == '\\') {
            std::filesystem::remove_all(target, ec);
        } else {
            std::filesystem::remove(target, ec);
        }
    }

    return {};
}

} // namespace cl
