#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/result.h"

namespace cl::mc {

struct MirrorReplacement {
    std::string from;
    std::string to;
};

struct MirrorSource {
    std::string id;
    std::string name;
    std::string manifest_url;
    std::vector<MirrorReplacement> replacements;
    int priority = 100;
    bool builtin = true;
};

// 内置镜像。之后可以从远程 JSON 覆盖或追加。
std::vector<MirrorSource> builtin_mirrors();

// 内置镜像 + %LOCALAPPDATA%\CraftLauncher\mirrors.json 中的自定义镜像。
std::vector<MirrorSource> load_mirrors();

std::filesystem::path mirror_cache_path();

// 远程镜像目录格式（GitHub 上放这个 JSON 即可）：
// {
//   "mirrors": [
//     {
//       "id": "my-mirror",
//       "name": "My Mirror",
//       "manifest": "https://example.com/mc/game/version_manifest_v2.json",
//       "priority": 15,
//       "replacements": [
//         {"from": "https://piston-meta.mojang.com", "to": "https://example.com"}
//       ]
//     }
//   ]
// }
Result<void> save_mirror_list(const nlohmann::json& json);
Result<nlohmann::json> fetch_mirror_list(const std::string& url);
Result<void> refresh_mirror_list(const std::string& url);

// 把官方 URL 替换成镜像 URL；没有匹配时原样返回。
std::string map_mirror_url(const std::string& url, const MirrorSource& mirror);

} // namespace cl::mc
