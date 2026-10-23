#include "mc/modrinth.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "core/crypto.h"
#include "core/encoding.h"
#include "core/http.h"
#include "core/log.h"

namespace cl::mc {

namespace {

constexpr const char* kApiBase = "https://api.modrinth.com/v2";
constexpr const char* kUserAgent =
    "CMCL/0.5.1 (C++ Minecraft Launcher; local Windows client)";

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string url_encode(std::string_view text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string json_string(const nlohmann::json& json, const char* key,
                        std::string fallback = {}) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

unsigned long long json_ull(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end()) return 0;
    if (it->is_number_unsigned()) return it->get<unsigned long long>();
    if (it->is_number_integer()) {
        const long long value = it->get<long long>();
        return value > 0 ? static_cast<unsigned long long>(value) : 0;
    }
    return 0;
}

int json_int(const nlohmann::json& json, const char* key, int fallback = 0) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number_integer()) return fallback;
    return it->get<int>();
}

bool json_bool(const nlohmann::json& json, const char* key, bool fallback = false) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

std::vector<std::string> json_strings(const nlohmann::json& json,
                                      const char* key) {
    std::vector<std::string> result;
    const auto it = json.find(key);
    if (it == json.end() || !it->is_array()) return result;
    for (const auto& item : *it) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

Result<std::string> api_get(const std::string& url) {
    HttpOptions options;
    options.timeout_seconds = 25;
    options.headers["User-Agent"] = kUserAgent;
    return http_get(url, options);
}

Result<nlohmann::json> parse_json(const std::string& body,
                                  const char* what) {
    try {
        return nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        return Error::make(100, std::string("解析 Modrinth ") + what + "失败",
                           ex.what());
    }
}

ModrinthFile parse_file(const nlohmann::json& json) {
    ModrinthFile file;
    file.filename = json_string(json, "filename");
    file.url = json_string(json, "url");
    file.size = json_ull(json, "size");
    file.primary = json_bool(json, "primary");
    if (const auto hashes = json.find("hashes");
        hashes != json.end() && hashes->is_object()) {
        file.sha1 = lower_copy(json_string(*hashes, "sha1"));
    }
    return file;
}

ModrinthVersion parse_version(const nlohmann::json& json) {
    ModrinthVersion version;
    version.id = json_string(json, "id");
    version.project_id = json_string(json, "project_id");
    version.name = json_string(json, "name");
    version.version_number = json_string(json, "version_number");
    version.version_type = json_string(json, "version_type");
    version.date_published = json_string(json, "date_published");
    version.game_versions = json_strings(json, "game_versions");
    version.loaders = json_strings(json, "loaders");
    if (const auto files = json.find("files");
        files != json.end() && files->is_array()) {
        for (const auto& item : *files) {
            if (!item.is_object()) continue;
            ModrinthFile file = parse_file(item);
            if (!file.filename.empty() && !file.url.empty()) {
                version.files.push_back(std::move(file));
            }
        }
    }
    return version;
}

bool safe_filename(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        return false;
    }
    return true;
}

} // namespace

const ModrinthFile* ModrinthVersion::primary_file() const {
    for (const auto& file : files) {
        if (file.primary) return &file;
    }
    return files.empty() ? nullptr : &files.front();
}

Result<ModrinthSearchResult> search_modrinth(
    const std::string& query, const std::string& project_type,
    const std::string& game_version, const std::string& loader,
    const std::string& index, int offset, int limit) {
    const std::string type = lower_copy(project_type.empty() ? "mod" : project_type);
    limit = (std::max)(1, (std::min)(limit, 50));
    offset = (std::max)(0, offset);

    std::string facets = "[[\"project_type:" + type + "\"]";
    if (!game_version.empty()) {
        facets += ",[\"versions:" + game_version + "\"]";
    }
    if (!loader.empty() && lower_copy(loader) != "vanilla") {
        facets += ",[\"categories:" + lower_copy(loader) + "\"]";
    }
    facets += "]";

    const std::string sort =
        index.empty() ? (query.empty() ? "downloads" : "relevance") : index;
    std::string url = std::string(kApiBase) + "/search?limit=" +
                      std::to_string(limit) + "&offset=" +
                      std::to_string(offset) + "&index=" + url_encode(sort) +
                      "&facets=" + url_encode(facets);
    if (!query.empty()) {
        url += "&query=" + url_encode(query);
    }

    auto body = api_get(url);
    if (!body) return body.error();
    auto json = parse_json(body.value(), "搜索响应");
    if (!json) return json.error();
    if (!json.value().is_object()) {
        return Error::make(101, "Modrinth 搜索响应不是对象");
    }

    ModrinthSearchResult result;
    result.total_hits = json_int(json.value(), "total_hits");
    result.offset = json_int(json.value(), "offset", offset);
    result.limit = json_int(json.value(), "limit", limit);
    const auto hits = json.value().find("hits");
    if (hits == json.value().end() || !hits->is_array()) return result;

    for (const auto& hit : *hits) {
        if (!hit.is_object()) continue;
        ModrinthProject project;
        project.id = json_string(hit, "project_id");
        project.slug = json_string(hit, "slug");
        project.title = json_string(hit, "title");
        project.description = json_string(hit, "description");
        project.icon_url = json_string(hit, "icon_url");
        project.author = json_string(hit, "author");
        project.project_type = json_string(hit, "project_type", type);
        project.date_modified = json_string(hit, "date_modified");
        project.downloads = json_ull(hit, "downloads");
        project.follows = json_ull(hit, "follows");
        project.categories = json_strings(hit, "categories");
        project.game_versions = json_strings(hit, "versions");
        if (!project.id.empty()) result.projects.push_back(std::move(project));
    }
    Log::info("Modrinth 搜索：" + query + " -> " +
              std::to_string(result.projects.size()) + "/" +
              std::to_string(result.total_hits));
    return result;
}

Result<std::vector<ModrinthVersion>> list_modrinth_versions(
    const std::string& project_id, const std::string& game_version,
    const std::string& loader) {
    if (project_id.empty()) return Error::make(102, "缺少 Modrinth 项目 ID");

    std::string url = std::string(kApiBase) + "/project/" +
                      url_encode(project_id) + "/version";
    std::vector<std::string> params;
    if (!game_version.empty()) {
        params.push_back("game_versions=" +
                         url_encode("[\"" + game_version + "\"]"));
    }
    const std::string load = lower_copy(loader);
    if (!load.empty() && load != "vanilla" && load != "any") {
        params.push_back("loaders=" + url_encode("[\"" + load + "\"]"));
    }
    for (size_t i = 0; i < params.size(); ++i) {
        url += (i == 0 ? "?" : "&") + params[i];
    }

    auto body = api_get(url);
    if (!body) return body.error();
    auto json = parse_json(body.value(), "版本列表");
    if (!json) return json.error();
    if (!json.value().is_array()) {
        return Error::make(103, "Modrinth 版本列表不是数组");
    }

    std::vector<ModrinthVersion> versions;
    for (const auto& item : json.value()) {
        if (!item.is_object()) continue;
        ModrinthVersion version = parse_version(item);
        if (!version.id.empty()) versions.push_back(std::move(version));
    }
    std::sort(versions.begin(), versions.end(),
              [](const ModrinthVersion& a, const ModrinthVersion& b) {
                  return a.date_published > b.date_published;
              });
    return versions;
}

Result<ModrinthVersion> get_modrinth_version(const std::string& version_id) {
    if (version_id.empty()) return Error::make(104, "缺少 Modrinth 版本 ID");
    auto body = api_get(std::string(kApiBase) + "/version/" +
                        url_encode(version_id));
    if (!body) return body.error();
    auto json = parse_json(body.value(), "版本信息");
    if (!json) return json.error();
    if (!json.value().is_object()) {
        return Error::make(105, "Modrinth 版本信息不是对象");
    }
    ModrinthVersion version = parse_version(json.value());
    if (version.id.empty()) {
        return Error::make(106, "Modrinth 版本缺少 ID");
    }
    return version;
}

Result<std::string> download_modrinth_file(
    const ModrinthVersion& version, const std::filesystem::path& target_dir,
    const DownloadProgressCallback& progress, DownloadCancelToken& cancel) {
    const ModrinthFile* file = version.primary_file();
    if (file == nullptr) {
        return Error::make(107, "这个版本没有可下载的文件");
    }
    if (!safe_filename(file->filename)) {
        return Error::make(108, "资源文件名不合法", file->filename);
    }

    std::error_code ec;
    std::filesystem::create_directories(target_dir, ec);
    if (ec) {
        return Error::make(109, "无法创建资源目录",
                           path_to_utf8(target_dir) + ": " + ec.message());
    }

    const std::filesystem::path target =
        target_dir / path_from_utf8(file->filename);
    if (std::filesystem::is_regular_file(target, ec)) {
        if (file->sha1.empty()) {
            if (std::filesystem::file_size(target, ec) == file->size && !ec) {
                return path_to_utf8(target);
            }
        } else {
            auto hash = sha1_file(target);
            if (hash && lower_copy(hash.value()) == file->sha1) {
                return path_to_utf8(target);
            }
        }
    }

    if (progress) {
        DownloadProgress item;
        item.stage = "资源";
        item.message = "正在下载 " + version.name;
        item.file = file->filename;
        item.total_files = 1;
        item.total_bytes = file->size;
        progress(item);
    }

    HttpOptions options;
    options.expected_size = file->size;
    options.timeout_seconds = 120;
    options.headers["User-Agent"] = kUserAgent;
    auto result = http_download(
        file->url, target, options,
        [&](const HttpProgress& http) {
            if (!progress) return;
            DownloadProgress item;
            item.stage = "资源";
            item.message = "正在下载 " + version.name;
            item.file = file->filename;
            item.total_files = 1;
            item.completed_files = 0;
            item.downloaded_bytes = http.downloaded;
            item.total_bytes = http.total > 0 ? http.total : file->size;
            progress(item);
        },
        &cancel);
    if (!result) return result.error();

    if (!file->sha1.empty()) {
        auto hash = sha1_file(target);
        if (!hash || lower_copy(hash.value()) != file->sha1) {
            std::filesystem::remove(target, ec);
            return Error::make(110, "资源 SHA-1 校验失败", file->filename);
        }
    }

    if (progress) {
        DownloadProgress item;
        item.stage = "完成";
        item.message = "下载完成：" + version.name;
        item.file = file->filename;
        item.total_files = 1;
        item.completed_files = 1;
        item.downloaded_bytes = file->size;
        item.total_bytes = file->size;
        progress(item);
    }
    Log::info("Modrinth 资源下载完成：" + file->filename);
    return path_to_utf8(target);
}

} // namespace cl::mc
