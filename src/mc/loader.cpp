#include "mc/loader.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/archive.h"
#include "core/crypto.h"
#include "core/encoding.h"
#include "core/http.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/process.h"
#include "mc/instance.h"
#include "mc/java.h"
#include "mc/version.h"

namespace cl::mc {

namespace {

constexpr const char* kFabricMeta = "https://meta.fabricmc.net";
constexpr const char* kQuiltMeta = "https://meta.quiltmc.org";
constexpr const char* kBmclApi = "https://bmclapi2.bangbang93.com";
constexpr const char* kForgeMaven = "https://maven.minecraftforge.net";
constexpr const char* kNeoForgeMaven = "https://maven.neoforged.net/releases";

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool contains_ci(const std::string& text, const std::string& needle) {
    return lower_copy(text).find(lower_copy(needle)) != std::string::npos;
}

std::string json_string(const nlohmann::json& json, const char* key,
                        std::string fallback = {}) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

bool json_bool(const nlohmann::json& json, const char* key, bool fallback) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

std::vector<long long> version_numbers(const std::string& text) {
    std::vector<long long> result;
    long long current = -1;
    for (const unsigned char c : text) {
        if (std::isdigit(c)) {
            if (current < 0) current = 0;
            current = current * 10 + (c - '0');
        } else if (current >= 0) {
            result.push_back(current);
            current = -1;
        }
    }
    if (current >= 0) result.push_back(current);
    return result;
}

bool version_newer(const std::string& a, const std::string& b) {
    const auto left = version_numbers(a);
    const auto right = version_numbers(b);
    const size_t count = (std::max)(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) {
        const long long l = i < left.size() ? left[i] : 0;
        const long long r = i < right.size() ? right[i] : 0;
        if (l != r) return l > r;
    }
    return a > b;
}

void sort_versions(std::vector<LoaderVersionInfo>& versions) {
    std::sort(versions.begin(), versions.end(),
              [](const LoaderVersionInfo& a, const LoaderVersionInfo& b) {
                  if (a.stable != b.stable) return a.stable;
                  return version_newer(a.version, b.version);
              });
}

Result<std::string> get_text(const std::string& url) {
    HttpOptions options;
    options.timeout_seconds = 40;
    return http_get(url, options);
}

Result<std::vector<LoaderVersionInfo>> fabric_versions(
    const std::string& game_version) {
    auto body = get_text(std::string(kFabricMeta) + "/v2/versions/loader/" +
                         game_version);
    if (!body) return body.error();

    std::vector<LoaderVersionInfo> result;
    try {
        const auto json = nlohmann::json::parse(body.value());
        if (!json.is_array()) {
            return Error::make(60, "Fabric 版本列表格式不正确");
        }
        for (const auto& entry : json) {
            if (!entry.is_object()) continue;
            const auto loader = entry.find("loader");
            if (loader == entry.end() || !loader->is_object()) continue;
            LoaderVersionInfo info;
            info.version = json_string(*loader, "version");
            info.stable = json_bool(*loader, "stable", true);
            if (!info.version.empty()) result.push_back(std::move(info));
        }
    } catch (const std::exception& ex) {
        return Error::make(61, "无法解析 Fabric 版本列表", ex.what());
    }
    if (result.empty()) {
        return Error::make(62, "这个 Minecraft 版本没有可用的 Fabric",
                           game_version);
    }
    sort_versions(result);
    return result;
}

Result<std::vector<LoaderVersionInfo>> quilt_versions(
    const std::string& game_version) {
    auto body = get_text(std::string(kQuiltMeta) + "/v3/versions/loader/" +
                         game_version);
    if (!body) return body.error();

    std::vector<LoaderVersionInfo> result;
    try {
        const auto json = nlohmann::json::parse(body.value());
        if (!json.is_array()) {
            return Error::make(63, "Quilt 版本列表格式不正确");
        }
        for (const auto& entry : json) {
            if (!entry.is_object()) continue;
            const auto loader = entry.find("loader");
            if (loader == entry.end() || !loader->is_object()) continue;
            LoaderVersionInfo info;
            info.version = json_string(*loader, "version");
            const std::string lower = lower_copy(info.version);
            info.stable = lower.find("beta") == std::string::npos &&
                          lower.find("alpha") == std::string::npos &&
                          lower.find("rc") == std::string::npos;
            if (!info.version.empty()) result.push_back(std::move(info));
        }
    } catch (const std::exception& ex) {
        return Error::make(64, "无法解析 Quilt 版本列表", ex.what());
    }
    if (result.empty()) {
        return Error::make(65, "这个 Minecraft 版本没有可用的 Quilt",
                           game_version);
    }
    sort_versions(result);
    return result;
}

std::vector<std::string> parse_maven_versions(const std::string& body,
                                              const std::string& prefix,
                                              bool strip_prefix) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (true) {
        const size_t start = body.find("<version>", pos);
        if (start == std::string::npos) break;
        const size_t value_start = start + 9;
        const size_t end = body.find("</version>", value_start);
        if (end == std::string::npos) break;
        std::string value = body.substr(value_start, end - value_start);
        pos = end + 10;
        if (!prefix.empty() && value.rfind(prefix, 0) != 0) continue;
        if (strip_prefix && !prefix.empty()) value = value.substr(prefix.size());
        if (!value.empty()) result.push_back(std::move(value));
    }
    return result;
}

Result<std::vector<LoaderVersionInfo>> forge_versions(
    const std::string& game_version) {
    std::vector<LoaderVersionInfo> result;

    // BMCLAPI 的 Forge 列表在中国网络下通常可用，返回的 version 已经是加载器版本。
    auto body = get_text(std::string(kBmclApi) + "/forge/minecraft/" +
                         game_version);
    if (body) {
        try {
            const auto json = nlohmann::json::parse(body.value());
            if (json.is_array()) {
                for (const auto& entry : json) {
                    if (!entry.is_object()) continue;
                    LoaderVersionInfo info;
                    info.version = json_string(entry, "version");
                    if (!info.version.empty()) result.push_back(std::move(info));
                }
            }
        } catch (...) {
            // 下面继续尝试官方 maven metadata。
        }
    }

    if (result.empty()) {
        auto xml = get_text(std::string(kForgeMaven) +
                            "/net/minecraftforge/forge/maven-metadata.xml");
        if (!xml) return xml.error();
        for (auto& version : parse_maven_versions(
                 xml.value(), game_version + "-", true)) {
            result.push_back(LoaderVersionInfo{std::move(version), true});
        }
    }

    if (result.empty()) {
        return Error::make(66, "这个 Minecraft 版本没有可用的 Forge",
                           game_version);
    }
    sort_versions(result);
    return result;
}

std::string neoforge_prefix(const std::string& game_version) {
    const auto parts = version_numbers(game_version);
    if (parts.size() < 2) return {};
    // 1.21.1 -> 21.1.；1.21.10 -> 21.10.
    const long long major = parts[0] == 1 ? parts[1] : parts[0];
    const long long minor = parts[0] == 1
                                ? (parts.size() > 2 ? parts[2] : 0)
                                : (parts.size() > 1 ? parts[1] : 0);
    return std::to_string(major) + "." + std::to_string(minor) + ".";
}

Result<std::vector<LoaderVersionInfo>> neoforge_versions(
    const std::string& game_version) {
    std::vector<LoaderVersionInfo> result;

    auto body = get_text(std::string(kBmclApi) + "/neoforge/list/" +
                         game_version);
    if (body) {
        try {
            const auto json = nlohmann::json::parse(body.value());
            if (json.is_array()) {
                for (const auto& entry : json) {
                    if (!entry.is_object()) continue;
                    LoaderVersionInfo info;
                    info.version = json_string(entry, "version");
                    const std::string lower = lower_copy(info.version);
                    info.stable = lower.find("beta") == std::string::npos;
                    if (!info.version.empty()) result.push_back(std::move(info));
                }
            }
        } catch (...) {
            // 回退到官方 metadata。
        }
    }

    if (result.empty()) {
        auto xml = get_text(std::string(kNeoForgeMaven) +
                            "/net/neoforged/neoforge/maven-metadata.xml");
        if (!xml) return xml.error();
        const std::string prefix = neoforge_prefix(game_version);
        for (auto& version : parse_maven_versions(xml.value(), prefix, false)) {
            const std::string lower = lower_copy(version);
            result.push_back(LoaderVersionInfo{
                std::move(version), lower.find("beta") == std::string::npos});
        }
    }

    if (result.empty()) {
        return Error::make(67, "这个 Minecraft 版本没有可用的 NeoForge",
                           game_version);
    }
    sort_versions(result);
    return result;
}

Result<void> write_json(const std::filesystem::path& path,
                        const nlohmann::json& json) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Error::make(68, "无法创建加载器版本目录",
                           path_to_utf8(path.parent_path()) + ": " +
                               ec.message());
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Error::make(69, "无法写入加载器 JSON", path_to_utf8(path));
    }
    out << json.dump(2);
    if (!out) {
        return Error::make(70, "写入加载器 JSON 失败", path_to_utf8(path));
    }
    return {};
}

void report_progress(const DownloadProgressCallback& progress,
                     const std::string& stage, const std::string& message,
                     const std::string& file = {}, int completed = 0,
                     int total = 0) {
    if (!progress) return;
    DownloadProgress item;
    item.stage = stage;
    item.message = message;
    item.file = file;
    item.completed_files = completed;
    item.total_files = total;
    progress(item);
}

Result<void> ensure_vanilla(const LoaderInstallRequest& request,
                            const DownloadProgressCallback& progress,
                            DownloadCancelToken& cancel) {
    const std::filesystem::path json_path =
        request.mc_dir / L"versions" / path_from_utf8(request.game_version) /
        (path_from_utf8(request.game_version).wstring() + L".json");
    std::error_code ec;
    if (std::filesystem::is_regular_file(json_path, ec)) return {};

    report_progress(progress, "加载器",
                    "正在安装加载器所需的原版 " + request.game_version + "…");
    DownloadRequest vanilla;
    vanilla.mc_dir = request.mc_dir;
    vanilla.version_id = request.game_version;
    vanilla.instance_name = request.game_version;
    vanilla.source = request.source;
    return install_version(vanilla, progress, cancel);
}

bool safe_relative_path(const std::string& text) {
    if (text.empty()) return false;
    const std::filesystem::path path = path_from_utf8(text);
    if (path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path) {
        if (part == L"..") return false;
    }
    return true;
}

Result<void> ensure_launcher_profiles(
    const std::filesystem::path& mc_dir) {
    const std::filesystem::path path = mc_dir / L"launcher_profiles.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) return {};
    return write_json(path, nlohmann::json{{"profiles", nlohmann::json::object()},
                                           {"settings", nlohmann::json::object()},
                                           {"version", 3}});
}

Result<void> prefetch_installer_libraries(
    const std::filesystem::path& installer,
    const std::filesystem::path& mc_dir,
    const DownloadProgressCallback& progress, DownloadCancelToken& cancel) {
    const std::filesystem::path profile_dir =
        installer.parent_path() /
        (installer.stem().wstring() + L".profile");
    const std::filesystem::path profile_path =
        profile_dir / L"install_profile.json";

    std::error_code ec;
    if (!std::filesystem::is_regular_file(profile_path, ec)) {
        std::filesystem::remove_all(profile_dir, ec);
        auto extracted = extract_zip(installer, profile_dir);
        if (!extracted) return extracted.error();
    }

    auto loaded = load_json(profile_path);
    if (!loaded || !loaded.value().is_object()) {
        return Error::make(86, "无法读取 NeoForge install_profile.json",
                           path_to_utf8(profile_path));
    }

    const auto libraries = loaded.value().find("libraries");
    if (libraries == loaded.value().end() || !libraries->is_array()) {
        return Error::make(87, "NeoForge install_profile 没有 libraries");
    }

    struct Artifact {
        std::string path;
        std::string sha1;
        std::string url;
    };
    std::vector<Artifact> artifacts;
    for (const auto& library : *libraries) {
        if (!library.is_object()) continue;
        const auto downloads = library.find("downloads");
        if (downloads == library.end() || !downloads->is_object()) continue;
        const auto artifact = downloads->find("artifact");
        if (artifact == downloads->end() || !artifact->is_object()) continue;
        Artifact item;
        item.path = json_string(*artifact, "path");
        item.sha1 = lower_copy(json_string(*artifact, "sha1"));
        item.url = json_string(*artifact, "url");
        if (safe_relative_path(item.path)) {
            artifacts.push_back(std::move(item));
        }
    }
    if (artifacts.empty()) return {};

    int completed = 0;
    const int total = static_cast<int>(artifacts.size());
    for (const auto& artifact : artifacts) {
        if (cancel.load(std::memory_order_relaxed)) {
            return Error::make(499, "下载已取消");
        }
        const std::filesystem::path destination =
            mc_dir / L"libraries" / path_from_utf8(artifact.path);
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            return Error::make(88, "无法创建加载器库目录",
                               path_to_utf8(destination.parent_path()));
        }

        bool valid = false;
        if (std::filesystem::is_regular_file(destination, ec) && !ec) {
            if (artifact.sha1.empty()) {
                valid = std::filesystem::file_size(destination, ec) > 0 && !ec;
            } else {
                auto hash = sha1_file(destination);
                valid = hash && lower_copy(hash.value()) == artifact.sha1;
            }
        }
        if (valid) {
            ++completed;
            report_progress(progress, "加载器",
                            "NeoForge 依赖已就绪 " + std::to_string(completed) +
                                "/" + std::to_string(total),
                            artifact.path, completed, total);
            continue;
        }

        std::vector<std::string> urls;
        urls.push_back(std::string(kBmclApi) + "/maven/" + artifact.path);
        if (!artifact.url.empty()) urls.push_back(artifact.url);

        std::string last_error;
        bool downloaded = false;
        for (const auto& url : urls) {
            HttpOptions options;
            options.timeout_seconds = 45;
            auto result = http_download(url, destination, options, {},
                                        &cancel);
            if (!result) {
                last_error = result.error().to_string();
                std::filesystem::remove(destination, ec);
                continue;
            }
            if (!artifact.sha1.empty()) {
                auto hash = sha1_file(destination);
                if (!hash || lower_copy(hash.value()) != artifact.sha1) {
                    last_error = "SHA-1 校验失败";
                    std::filesystem::remove(destination, ec);
                    continue;
                }
            }
            downloaded = true;
            break;
        }
        if (!downloaded) {
            return Error::make(89, "NeoForge 依赖下载失败",
                               artifact.path + "\n" + last_error);
        }

        ++completed;
        report_progress(progress, "加载器",
                        "正在准备 NeoForge 依赖 " + std::to_string(completed) +
                            "/" + std::to_string(total),
                        artifact.path, completed, total);
    }
    return {};
}

std::string install_name_for(const LoaderInstallRequest& request) {
    if (!request.instance_name.empty()) return request.instance_name;
    return request.game_version + "-" + loader_display_name(request.kind);
}

Result<std::string> run_installer(const LoaderInstallRequest& request,
                                  const JavaInfo& java,
                                  const std::filesystem::path& installer,
                                  bool offline,
                                  const DownloadProgressCallback& progress,
                                  DownloadCancelToken& cancel) {
    std::mutex output_mutex;
    std::string output_tail;
    std::vector<std::wstring> arguments{
        java.exe.wstring(), L"-jar", installer.wstring(), L"--installClient",
        request.mc_dir.wstring()};
    if (offline) arguments.push_back(L"--offline");
    const std::wstring command = build_command_line(arguments);
    auto process = Process::start(
        command, request.mc_dir,
        [&](std::string_view line) {
            std::string text(line);
            while (!text.empty() &&
                   (text.back() == '\r' || text.back() == '\n')) {
                text.pop_back();
            }
            if (text.empty()) return;
            {
                std::lock_guard lock(output_mutex);
                output_tail += text;
                output_tail.push_back('\n');
                if (output_tail.size() > 4000) {
                    output_tail.erase(0, output_tail.size() - 4000);
                }
            }
            report_progress(progress, "加载器",
                            loader_display_name(request.kind) + " 安装器：" +
                                text.substr(0, 300));
        },
        default_fallback_code_page());
    if (!process) {
        return Error::make(71, "无法启动加载器安装器",
                           process.error().to_string());
    }

    while (process.value()->running()) {
        if (cancel.load(std::memory_order_relaxed)) {
            process.value()->terminate();
            break;
        }
        ::Sleep(200);
    }
    const unsigned long code = process.value()->wait();
    if (cancel.load(std::memory_order_relaxed)) {
        return Error::make(499, "下载已取消");
    }
    if (code != 0) {
        std::lock_guard lock(output_mutex);
        return Error::make(72, "加载器安装器运行失败",
                           "退出码 " + std::to_string(code) + "\n" +
                               output_tail);
    }
    return output_tail;
}

Result<std::string> find_installed_loader(
    const std::filesystem::path& mc_dir, const std::string& game_version,
    LoaderKind kind, const std::string& loader_version,
    const std::set<std::wstring>& before) {
    const std::filesystem::path versions_dir = mc_dir / L"versions";
    const std::string token = loader_kind_key(kind);
    std::string best;
    bool best_new = false;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        const std::wstring folder = entry.path().filename().wstring();
        const std::filesystem::path json_path =
            entry.path() / (folder + L".json");
        if (!std::filesystem::is_regular_file(json_path, ec)) continue;

        auto loaded = load_json(json_path);
        if (!loaded || !loaded.value().is_object()) continue;
        const auto& json = loaded.value();
        const std::string id = json_string(json, "id",
                                           path_to_utf8(entry.path().filename()));
        const std::string parent = json_string(json, "inheritsFrom");
        const bool is_new = before.find(folder) == before.end();
        const bool name_match = contains_ci(id, token);
        const bool version_match = loader_version.empty() ||
                                   id.find(loader_version) != std::string::npos;
        // 加载器版本必须继承目标原版。
        const bool parent_ok = parent == game_version;

        // 只接受能识别为加载器的目录：id 必须含 loader token，
        // 且 inheritsFrom 必须指向目标原版。原版目录（id 不含 token）直接排除。
        if (!name_match) continue;
        if (!parent_ok) continue;

        // 版本号匹配优先；不匹配时只有"安装器新建"的目录才接受。
        if (!version_match && !is_new) continue;

        if (best.empty() || (is_new && !best_new) ||
            (is_new == best_new && version_newer(id, best))) {
            best = id;
            best_new = is_new;
        }
    }
    if (best.empty()) {
        return Error::make(73, "安装器没有生成加载器版本",
                           std::string("未找到 ") + loader_display_name(kind) +
                               " " + loader_version);
    }
    return best;
}

std::filesystem::path installer_cache_path(const std::string& name) {
    return local_app_data_dir() / L"CraftLauncher" / L"cache" / L"loaders" /
           path_from_utf8(name);
}

Result<void> download_installer(
    const std::vector<std::string>& urls, const std::filesystem::path& target,
    const DownloadProgressCallback& progress, DownloadCancelToken& cancel,
    std::string& last_error) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return Error::make(74, "无法创建加载器缓存目录",
                           path_to_utf8(target.parent_path()) + ": " +
                               ec.message());
    }
    if (std::filesystem::is_regular_file(target, ec) &&
        std::filesystem::file_size(target, ec) > 0 && !ec) {
        report_progress(progress, "加载器", "使用已缓存的加载器安装器",
                        path_to_utf8(target));
        return {};
    }

    for (const auto& url : urls) {
        if (cancel.load(std::memory_order_relaxed)) {
            return Error::make(499, "下载已取消");
        }
        HttpOptions options;
        options.timeout_seconds = 90;
        auto result = http_download(
            url, target, options,
            [&](const HttpProgress& http) {
                if (!progress) return;
                DownloadProgress item;
                item.stage = "加载器";
                item.message = "正在下载加载器安装器…";
                item.file = url;
                item.total_files = 1;
                item.downloaded_bytes = http.downloaded;
                item.total_bytes = http.total;
                progress(item);
            },
            &cancel);
        if (result) return {};
        last_error = result.error().to_string();
        std::filesystem::remove(target, ec);
    }
    return Error::make(75, "加载器安装器下载失败", last_error);
}

} // namespace

std::optional<LoaderKind> parse_loader_kind(std::string_view text) {
    const std::string value = lower_copy(std::string(text));
    if (value == "forge") return LoaderKind::Forge;
    if (value == "neoforge") return LoaderKind::NeoForge;
    if (value == "fabric") return LoaderKind::Fabric;
    if (value == "quilt") return LoaderKind::Quilt;
    return std::nullopt;
}

std::string loader_kind_key(LoaderKind kind) {
    switch (kind) {
        case LoaderKind::Forge: return "forge";
        case LoaderKind::NeoForge: return "neoforge";
        case LoaderKind::Fabric: return "fabric";
        case LoaderKind::Quilt: return "quilt";
    }
    return "fabric";
}

std::string loader_display_name(LoaderKind kind) {
    switch (kind) {
        case LoaderKind::Forge: return "Forge";
        case LoaderKind::NeoForge: return "NeoForge";
        case LoaderKind::Fabric: return "Fabric";
        case LoaderKind::Quilt: return "Quilt";
    }
    return "Loader";
}

Result<std::vector<LoaderVersionInfo>> fetch_loader_versions(
    LoaderKind kind, const std::string& game_version) {
    if (game_version.empty()) {
        return Error::make(76, "没有指定 Minecraft 版本");
    }
    switch (kind) {
        case LoaderKind::Fabric: return fabric_versions(game_version);
        case LoaderKind::Quilt: return quilt_versions(game_version);
        case LoaderKind::Forge: return forge_versions(game_version);
        case LoaderKind::NeoForge: return neoforge_versions(game_version);
    }
    return Error::make(77, "未知的加载器");
}

Result<void> install_loader(const LoaderInstallRequest& request,
                            const DownloadProgressCallback& progress,
                            DownloadCancelToken& cancel) {
    if (request.game_version.empty()) {
        return Error::make(78, "没有指定 Minecraft 版本");
    }
    const std::string instance = install_name_for(request);
    if (!is_valid_instance_name(instance)) {
        return Error::make(79, "安装名称不合法", instance);
    }
    if (!std::filesystem::is_directory(request.mc_dir)) {
        return Error::make(80, "游戏目录不存在",
                           path_to_utf8(request.mc_dir));
    }

    const std::filesystem::path versions_dir = request.mc_dir / L"versions";
    const std::filesystem::path instance_dir =
        versions_dir / path_from_utf8(instance);
    std::error_code ec;
    if (std::filesystem::exists(instance_dir, ec)) {
        return Error::make(81, "同名版本已存在", instance);
    }

    CL_TRY_VOID(ensure_vanilla(request, progress, cancel));
    if (cancel.load(std::memory_order_relaxed)) {
        return Error::make(499, "下载已取消");
    }

    if (request.kind == LoaderKind::Fabric ||
        request.kind == LoaderKind::Quilt) {
        report_progress(progress, "加载器",
                        "正在获取 " + loader_display_name(request.kind) +
                            " 配置…");
        const std::string base =
            request.kind == LoaderKind::Fabric ? kFabricMeta : kQuiltMeta;
        const std::string api =
            request.kind == LoaderKind::Fabric
                ? "/v2/versions/loader/"
                : "/v3/versions/loader/";
        const std::string url = base + api + request.game_version + "/" +
                                request.loader_version + "/profile/json";
        auto body = get_text(url);
        if (!body) return body.error();

        nlohmann::json profile;
        try {
            profile = nlohmann::json::parse(body.value());
        } catch (const std::exception& ex) {
            return Error::make(82, "加载器 JSON 解析失败", ex.what());
        }
        if (!profile.is_object()) {
            return Error::make(83, "加载器 JSON 不是对象");
        }
        profile["id"] = instance;
        profile["inheritsFrom"] = request.game_version;
        profile.erase("jar");

        const std::filesystem::path json_path =
            instance_dir / (path_from_utf8(instance).wstring() + L".json");
        CL_TRY_VOID(write_json(json_path, profile));

        DownloadRequest repair;
        repair.mc_dir = request.mc_dir;
        repair.version_id = instance;
        repair.source = request.source;
        CL_TRY_VOID(repair_version(repair, progress, cancel));
        report_progress(progress, "完成",
                        loader_display_name(request.kind) + " 安装完成");
        Log::info(loader_display_name(request.kind) + " 安装完成：" + instance);
        return {};
    }

    // Forge / NeoForge：下载官方（或 BMCLAPI）安装器，调用 --installClient。
    const std::string full_forge =
        request.game_version + "-" + request.loader_version;
    std::vector<std::string> installer_urls;
    std::string installer_name;
    if (request.kind == LoaderKind::Forge) {
        installer_name = "forge-" + full_forge + "-installer.jar";
        installer_urls.push_back(
            std::string(kBmclApi) + "/maven/net/minecraftforge/forge/" +
            full_forge + "/" + installer_name);
        installer_urls.push_back(
            std::string(kForgeMaven) + "/net/minecraftforge/forge/" +
            full_forge + "/" + installer_name);
    } else {
        installer_name =
            "neoforge-" + request.loader_version + "-installer.jar";
        installer_urls.push_back(
            std::string(kBmclApi) + "/maven/net/neoforged/neoforge/" +
            request.loader_version + "/" + installer_name);
        installer_urls.push_back(
            std::string(kNeoForgeMaven) + "/net/neoforged/neoforge/" +
            request.loader_version + "/" + installer_name);
    }

    const std::filesystem::path installer =
        installer_cache_path(installer_name);
    std::string download_error;
    CL_TRY_VOID(download_installer(installer_urls, installer, progress, cancel,
                                   download_error));

    auto resolved = resolve_version(versions_dir, request.game_version);
    if (!resolved) return resolved.error();
    auto java = select_java(resolved.value(), std::nullopt, request.mc_dir);
    if (!java) return java.error();
    CL_TRY_VOID(ensure_launcher_profiles(request.mc_dir));

    bool offline = false;
    if (request.kind == LoaderKind::NeoForge) {
        // maven.neoforged.net 在部分网络不可达。先把安装器依赖通过
        // BMCLAPI 预取到 libraries，再让安装器离线完成注入。
        auto prefetched = prefetch_installer_libraries(
            installer, request.mc_dir, progress, cancel);
        if (prefetched) {
            offline = true;
        } else {
            Log::warn("NeoForge 依赖预取失败，尝试在线安装：" +
                      prefetched.error().to_string());
        }
    }

    report_progress(progress, "加载器",
                    "正在运行 " + loader_display_name(request.kind) +
                        " 安装器（Java " + java.value().version + "）…");
    std::set<std::wstring> before;
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir, ec)) {
        if (entry.is_directory(ec)) before.insert(entry.path().filename().wstring());
    }
    auto installer_output = run_installer(request, java.value(), installer,
                                          offline, progress, cancel);
    if (!installer_output) return installer_output.error();

    auto installed = find_installed_loader(
        request.mc_dir, request.game_version, request.kind,
        request.loader_version, before);
    if (!installed) {
        return Error::make(84, installed.error().message,
                           installed.error().context + "\n" +
                               installer_output.value());
    }

    std::string installed_id = installed.value();

    // 校验安装器产物确实是加载器版本，防止 find_installed_loader 在边界情况下
    // 误选原版目录后被 rename_instance 改名，破坏原版安装。
    {
        const std::filesystem::path json_path =
            versions_dir / path_from_utf8(installed_id) /
            (path_from_utf8(installed_id).wstring() + L".json");
        auto loaded = load_json(json_path);
        if (!loaded || !loaded.value().is_object()) {
            return Error::make(85, "安装器产物无法读取", installed_id);
        }
        const auto& json = loaded.value();
        const std::string id = json_string(json, "id", installed_id);
        const std::string parent = json_string(json, "inheritsFrom");
        if (!contains_ci(id, loader_kind_key(request.kind)) ||
            parent != request.game_version) {
            return Error::make(
                85, "安装器产物不是加载器版本",
                installed_id + " (id=" + id + ", inheritsFrom=" + parent + ")");
        }
    }

    if (installed_id != instance) {
        if (std::filesystem::exists(versions_dir / path_from_utf8(instance),
                                    ec)) {
            return Error::make(85, "安装器生成的版本与目标名称冲突",
                               installed_id + " -> " + instance);
        }
        auto renamed = rename_instance(request.mc_dir, installed_id, instance);
        if (!renamed) return renamed.error();
        installed_id = renamed.value();
    }

    DownloadRequest repair;
    repair.mc_dir = request.mc_dir;
    repair.version_id = installed_id;
    repair.source = request.source;
    CL_TRY_VOID(repair_version(repair, progress, cancel));
    report_progress(progress, "完成",
                    loader_display_name(request.kind) + " 安装完成");
    Log::info(loader_display_name(request.kind) + " 安装完成：" +
              installed_id);
    return {};
}

} // namespace cl::mc