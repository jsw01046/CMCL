#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "mc/mirrors.h"

namespace cl::mc {

enum class DownloadSource {
    Auto,
    Official,
    Mirror,
};

DownloadSource parse_download_source(std::string_view text);
std::string download_source_name(DownloadSource source);

struct VersionSummary {
    std::string id;
    std::string type;
    std::string url;
    std::string sha1;
    unsigned long long size = 0;
    std::string release_time;
};

struct VersionManifest {
    std::string latest_release;
    std::string latest_snapshot;
    std::vector<VersionSummary> versions;
};

struct DownloadProgress {
    std::string stage;
    std::string message;
    std::string file;
    int completed_files = 0;
    int total_files = 0;
    unsigned long long downloaded_bytes = 0;
    unsigned long long total_bytes = 0;
};

using DownloadProgressCallback = std::function<void(const DownloadProgress&)>;
using DownloadCancelToken = std::atomic<bool>;

struct DownloadRequest {
    std::filesystem::path mc_dir;
    std::string version_id;
    std::string instance_name;  // 为空时使用 version_id
    DownloadSource source = DownloadSource::Auto;
};

// 获取 Mojang 版本清单；失败时优先回退到本地缓存。
Result<VersionManifest> fetch_version_manifest(DownloadSource source,
                                               bool force_refresh = false);

// 安装原版：版本 JSON、client jar、libraries、asset index 和 assets。
Result<void> install_version(const DownloadRequest& request,
                             const DownloadProgressCallback& progress,
                             DownloadCancelToken& cancel);

// 校验并补齐已安装实例缺失/损坏的文件（支持 inheritsFrom 合并后的版本）。
Result<void> repair_version(const DownloadRequest& request,
                            const DownloadProgressCallback& progress,
                            DownloadCancelToken& cancel);

// 当前可用镜像（内置 + 自定义缓存）。
std::vector<MirrorSource> available_mirrors();

// 从远程镜像目录更新自定义镜像列表。
Result<void> refresh_mirrors(const std::string& url);

} // namespace cl::mc
