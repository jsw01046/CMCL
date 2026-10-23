#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "mc/download.h"

namespace cl::mc {

enum class LoaderKind {
    Forge,
    NeoForge,
    Fabric,
    Quilt,
};

std::optional<LoaderKind> parse_loader_kind(std::string_view text);
std::string loader_kind_key(LoaderKind kind);
std::string loader_display_name(LoaderKind kind);

struct LoaderVersionInfo {
    std::string version;
    bool stable = true;
};

struct LoaderInstallRequest {
    std::filesystem::path mc_dir;
    std::string game_version;
    std::string loader_version;
    std::string instance_name;
    LoaderKind kind = LoaderKind::Fabric;
    DownloadSource source = DownloadSource::Auto;
};

// 获取指定 Minecraft 版本可用的加载器版本，按版本号从新到旧排列。
Result<std::vector<LoaderVersionInfo>> fetch_loader_versions(
    LoaderKind kind, const std::string& game_version);

// 安装加载器实例：必要时先安装原版，再下载加载器库/运行安装器，最后校验文件。
Result<void> install_loader(const LoaderInstallRequest& request,
                            const DownloadProgressCallback& progress,
                            DownloadCancelToken& cancel);

} // namespace cl::mc
