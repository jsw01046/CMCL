#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"
#include "mc/download.h"

namespace cl::mc {

struct ModrinthProject {
    std::string id;
    std::string slug;
    std::string title;
    std::string description;
    std::string icon_url;
    std::string author;
    std::string project_type;
    std::string date_modified;
    unsigned long long downloads = 0;
    unsigned long long follows = 0;
    std::vector<std::string> categories;
    std::vector<std::string> game_versions;
};

struct ModrinthSearchResult {
    std::vector<ModrinthProject> projects;
    int total_hits = 0;
    int offset = 0;
    int limit = 20;
};

struct ModrinthFile {
    std::string filename;
    std::string url;
    std::string sha1;
    unsigned long long size = 0;
    bool primary = false;
};

struct ModrinthVersion {
    std::string id;
    std::string project_id;
    std::string name;
    std::string version_number;
    std::string version_type;
    std::string date_published;
    std::vector<std::string> game_versions;
    std::vector<std::string> loaders;
    std::vector<ModrinthFile> files;

    [[nodiscard]] const ModrinthFile* primary_file() const;
};

// project_type: mod / modpack / resourcepack / shader / datapack
Result<ModrinthSearchResult> search_modrinth(
    const std::string& query, const std::string& project_type,
    const std::string& game_version, const std::string& loader,
    const std::string& index, int offset, int limit);

// 获取项目兼容指定游戏版本/加载器的版本，新版本在前。
Result<std::vector<ModrinthVersion>> list_modrinth_versions(
    const std::string& project_id, const std::string& game_version,
    const std::string& loader);

Result<ModrinthVersion> get_modrinth_version(const std::string& version_id);

// 下载版本的主文件到 target_dir，带进度、SHA-1 校验和取消。
Result<std::string> download_modrinth_file(
    const ModrinthVersion& version, const std::filesystem::path& target_dir,
    const DownloadProgressCallback& progress, DownloadCancelToken& cancel);

} // namespace cl::mc
