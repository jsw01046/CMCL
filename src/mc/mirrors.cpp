#include "mc/mirrors.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

#include "core/encoding.h"
#include "core/http.h"
#include "core/log.h"
#include "core/paths.h"

namespace cl::mc {

namespace {

constexpr const char* kOfficialManifest =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

std::string json_string(const nlohmann::json& json, const char* key,
                        std::string fallback = {}) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

std::vector<MirrorSource> parse_mirrors(const nlohmann::json& json,
                                        bool builtin) {
    std::vector<MirrorSource> result;
    if (!json.is_object() && !json.is_array()) return result;

    const nlohmann::json* list = &json;
    if (json.is_object()) {
        const auto it = json.find("mirrors");
        if (it == json.end() || !it->is_array()) return result;
        list = &*it;
    }
    if (!list->is_array()) return result;

    for (const auto& entry : *list) {
        if (!entry.is_object()) continue;

        MirrorSource mirror;
        mirror.id = json_string(entry, "id");
        mirror.name = json_string(entry, "name", mirror.id);
        mirror.manifest_url = json_string(entry, "manifest");
        if (mirror.manifest_url.empty()) {
            mirror.manifest_url = json_string(entry, "manifest_url");
        }
        mirror.priority = entry.value("priority", 100);
        mirror.builtin = builtin;

        if (const auto replacements = entry.find("replacements");
            replacements != entry.end() && replacements->is_array()) {
            for (const auto& item : *replacements) {
                if (!item.is_object()) continue;
                MirrorReplacement replacement;
                replacement.from = json_string(item, "from");
                replacement.to = json_string(item, "to");
                if (!replacement.from.empty() && !replacement.to.empty()) {
                    mirror.replacements.push_back(std::move(replacement));
                }
            }
        }

        if (mirror.id.empty() || mirror.replacements.empty()) continue;
        if (mirror.manifest_url.empty()) {
            mirror.manifest_url = kOfficialManifest;
            for (const auto& replacement : mirror.replacements) {
                if (std::string(kOfficialManifest).rfind(replacement.from, 0) == 0) {
                    mirror.manifest_url =
                        map_mirror_url(kOfficialManifest, mirror);
                    break;
                }
            }
        }
        result.push_back(std::move(mirror));
    }
    return result;
}

} // namespace

std::vector<MirrorSource> builtin_mirrors() {
    // BMCLAPI 是国内最常用的完整镜像，覆盖版本 JSON、客户端、库和资源。
    MirrorSource bmclapi;
    bmclapi.id = "bmclapi";
    bmclapi.name = "BMCLAPI";
    bmclapi.manifest_url =
        "https://bmclapi2.bangbang93.com/mc/game/version_manifest_v2.json";
    bmclapi.priority = 10;
    bmclapi.replacements = {
        {"https://piston-meta.mojang.com", "https://bmclapi2.bangbang93.com"},
        {"https://launchermeta.mojang.com", "https://bmclapi2.bangbang93.com"},
        {"https://launcher.mojang.com", "https://bmclapi2.bangbang93.com"},
        {"https://resources.download.minecraft.net",
         "https://bmclapi2.bangbang93.com/assets"},
        {"https://libraries.minecraft.net",
         "https://bmclapi2.bangbang93.com/maven"},
        {"https://libraries.mojang.com",
         "https://bmclapi2.bangbang93.com/maven"},
    };

    // FastMinecraftMirror 直接按官方域名替换，适合作为第二备选。
    MirrorSource fast;
    fast.id = "fastmcmirror";
    fast.name = "FastMinecraftMirror";
    fast.manifest_url =
        "https://launchermeta.fastmcmirror.org/mc/game/version_manifest_v2.json";
    fast.priority = 20;
    fast.replacements = {
        {"https://piston-meta.mojang.com",
         "https://launchermeta.fastmcmirror.org"},
        {"https://launchermeta.mojang.com",
         "https://launchermeta.fastmcmirror.org"},
        {"https://launcher.mojang.com",
         "https://launcher.fastmcmirror.org"},
        {"https://resources.download.minecraft.net",
         "https://resources.fastmcmirror.org"},
        {"https://libraries.minecraft.net",
         "https://libraries.fastmcmirror.org"},
        {"https://libraries.mojang.com",
         "https://libraries.fastmcmirror.org"},
    };

    MirrorSource old;
    old.id = "bmclapi-old";
    old.name = "BMCLAPI (旧域名)";
    old.manifest_url =
        "https://bmclapi.bangbang93.com/mc/game/version_manifest_v2.json";
    old.priority = 40;
    old.replacements = {
        {"https://piston-meta.mojang.com", "https://bmclapi.bangbang93.com"},
        {"https://launchermeta.mojang.com", "https://bmclapi.bangbang93.com"},
        {"https://launcher.mojang.com", "https://bmclapi.bangbang93.com"},
        {"https://resources.download.minecraft.net",
         "https://bmclapi.bangbang93.com/assets"},
        {"https://libraries.minecraft.net",
         "https://bmclapi.bangbang93.com/maven"},
        {"https://libraries.mojang.com",
         "https://bmclapi.bangbang93.com/maven"},
    };

    return {std::move(bmclapi), std::move(fast), std::move(old)};
}

std::filesystem::path mirror_cache_path() {
    return local_app_data_dir() / L"CraftLauncher" / L"mirrors.json";
}

std::vector<MirrorSource> load_mirrors() {
    std::vector<MirrorSource> mirrors = builtin_mirrors();

    const auto path = mirror_cache_path();
    std::ifstream file(path, std::ios::binary);
    if (!file) return mirrors;

    try {
        nlohmann::json json;
        file >> json;
        auto custom = parse_mirrors(json, false);
        if (custom.empty()) return mirrors;

        std::map<std::string, size_t> index;
        for (size_t i = 0; i < mirrors.size(); ++i) index[mirrors[i].id] = i;
        for (auto& mirror : custom) {
            const auto it = index.find(mirror.id);
            if (it != index.end()) {
                mirrors[it->second] = std::move(mirror);
            } else {
                index[mirror.id] = mirrors.size();
                mirrors.push_back(std::move(mirror));
            }
        }
        std::sort(mirrors.begin(), mirrors.end(),
                  [](const MirrorSource& a, const MirrorSource& b) {
                      return a.priority < b.priority;
                  });
        Log::info("已加载 " + std::to_string(custom.size()) + " 个自定义镜像源");
    } catch (const std::exception& ex) {
        Log::warn(std::string("镜像源配置解析失败：") + ex.what());
    }
    return mirrors;
}

Result<void> save_mirror_list(const nlohmann::json& json) {
    const auto parsed = parse_mirrors(json, false);
    if (parsed.empty()) {
        return Error::make(60, "镜像源列表为空或格式不正确");
    }

    const auto path = mirror_cache_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Error::make(61, "无法创建镜像源目录",
                           path_to_utf8(path.parent_path()) + ": " + ec.message());
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return Error::make(62, "无法写入镜像源配置", path_to_utf8(path));
    }
    file << json.dump(2);
    if (!file) {
        return Error::make(63, "写入镜像源配置失败", path_to_utf8(path));
    }
    Log::info("已保存 " + std::to_string(parsed.size()) + " 个自定义镜像源");
    return {};
}

Result<nlohmann::json> fetch_mirror_list(const std::string& url) {
    HttpOptions options;
    options.timeout_seconds = 20;
    auto body = http_get(url, options);
    if (!body) return body.error();

    try {
        nlohmann::json json = nlohmann::json::parse(body.value());
        if (parse_mirrors(json, false).empty()) {
            return Error::make(64, "远程镜像源列表中没有有效条目", url);
        }
        return json;
    } catch (const std::exception& ex) {
        return Error::make(65, "镜像源列表解析失败",
                           url + ": " + ex.what());
    }
}

Result<void> refresh_mirror_list(const std::string& url) {
    if (url.empty()) {
        return Error::make(66, "没有配置镜像源列表地址");
    }
    auto json = fetch_mirror_list(url);
    if (!json) return json.error();
    return save_mirror_list(json.value());
}

std::string map_mirror_url(const std::string& url, const MirrorSource& mirror) {
    std::vector<MirrorReplacement> replacements = mirror.replacements;
    std::sort(replacements.begin(), replacements.end(),
              [](const MirrorReplacement& a, const MirrorReplacement& b) {
                  return a.from.size() > b.from.size();
              });

    for (const auto& replacement : replacements) {
        if (url.rfind(replacement.from, 0) != 0) continue;
        std::string result = replacement.to;
        std::string rest = url.substr(replacement.from.size());
        if (!result.empty() && result.back() == '/' && !rest.empty() &&
            rest.front() == '/') {
            rest.erase(0, 1);
        }
        return result + rest;
    }
    return url;
}

} // namespace cl::mc
