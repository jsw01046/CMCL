#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"
#include "mc/download.h"

namespace cl::mc {

struct ModpackImportRequest {
    std::filesystem::path mc_dir;
    std::filesystem::path archive;  // .mrpack / .zip
    std::string instance_name;      // 为空时使用整合包名称
    DownloadSource source = DownloadSource::Auto;
};

struct ModpackImportResult {
    std::string instance_id;
    std::string name;
    std::string pack_version;
    std::string game_version;
    std::string loader;
    std::string loader_version;
    int files_downloaded = 0;
    int files_skipped = 0;
};

// 导入 Modrinth .mrpack：安装原版和依赖加载器，下载清单中的文件，
// 最后解压 overrides / client-overrides。
Result<ModpackImportResult> import_modrinth_modpack(
    const ModpackImportRequest& request,
    const DownloadProgressCallback& progress,
    DownloadCancelToken& cancel);

} // namespace cl::mc
