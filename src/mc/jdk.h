#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"
#include "mc/download.h"
#include "mc/java.h"

namespace cl::mc {

struct JdkRelease {
    int feature = 0;
    std::string version;
    std::string release_name;
    std::string file_name;
    std::string url;
    std::string mirror_url;
    std::string sha256;
    unsigned long long size = 0;
};

// 从 Adoptium API 获取 Windows x64 JDK 最新版本。
Result<std::vector<JdkRelease>> fetch_openjdk_releases(bool force_refresh = false);

std::filesystem::path managed_jdk_root();
bool is_managed_jdk_path(const std::filesystem::path& path);

// 下载并解压 Temurin JDK 到启动器管理目录。
// force 为 true 时删除已有目录并重新下载安装。
Result<JavaInfo> install_openjdk(int feature, bool force,
                                 const DownloadProgressCallback& progress,
                                 DownloadCancelToken& cancel);

// 删除启动器下载的 JDK 安装目录（移到回收站）。
Result<void> delete_managed_jdk(const std::filesystem::path& java_exe);

// 第三方 Java：打开它自带的卸载程序；找不到时打开 Windows 的“应用和功能”。
Result<void> open_java_uninstaller(const std::filesystem::path& java_exe);

} // namespace cl::mc
