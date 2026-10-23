#include "mc/modpack.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/archive.h"
#include "core/crypto.h"
#include "core/encoding.h"
#include "core/http.h"
#include "core/log.h"
#include "core/paths.h"
#include "mc/instance.h"
#include "mc/loader.h"

namespace cl::mc {

namespace {

constexpr const char* kUserAgent =
    "CMCL/0.5.1 (C++ Minecraft Launcher; local Windows client)";

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string json_string(const nlohmann::json& json, const char* key,
                        std::string fallback = {}) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
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

void report(const DownloadProgressCallback& progress, std::string stage,
            std::string message, std::string file = {},
            int completed = 0, int total = 0,
            unsigned long long bytes = 0,
            unsigned long long total_bytes = 0) {
    if (!progress) return;
    DownloadProgress item;
    item.stage = std::move(stage);
    item.message = std::move(message);
    item.file = std::move(file);
    item.completed_files = completed;
    item.total_files = total;
    item.downloaded_bytes = bytes;
    item.total_bytes = total_bytes;
    progress(item);
}

struct TempDir {
    std::filesystem::path path;

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string sanitize_instance_name(const std::string& raw) {
    std::string out;
    constexpr std::string_view invalid = "<>:\"/\\|?*";
    for (const unsigned char c : raw) {
        if (c < 0x20) continue;
        out.push_back(invalid.find(static_cast<char>(c)) == std::string_view::npos
                          ? static_cast<char>(c)
                          : '-');
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    size_t start = 0;
    while (start < out.size() && out[start] == ' ') ++start;
    out.erase(0, start);

    if (out.empty() || out.size() > 180 || !is_valid_instance_name(out)) {
        out = "Modpack";
    }
    return out;
}

std::string unique_instance_name(const std::filesystem::path& mc_dir,
                                 const std::string& base) {
    const std::filesystem::path versions = mc_dir / L"versions";
    std::string candidate = base;
    int suffix = 2;
    std::error_code ec;
    while (std::filesystem::exists(versions / path_from_utf8(candidate), ec)) {
        candidate = base + "-" + std::to_string(suffix++);
    }
    return candidate;
}

bool safe_relative_path(const std::string& text,
                        std::filesystem::path& result) {
    if (text.empty()) return false;
    std::filesystem::path path = path_from_utf8(text);
    if (path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == L"..") return false;
    }
    path = path.lexically_normal();
    if (path.empty() || path == L".") return false;
    result = path;
    return true;
}

bool verify_file(const std::filesystem::path& path, const std::string& sha1,
                 const std::string& sha512, unsigned long long size) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    if (!sha1.empty()) {
        auto hash = sha1_file(path);
        return hash && lower_copy(hash.value()) == lower_copy(sha1);
    }
    if (!sha512.empty()) {
        auto hash = sha512_file(path);
        return hash && lower_copy(hash.value()) == lower_copy(sha512);
    }
    if (size > 0) {
        return std::filesystem::file_size(path, ec) == size && !ec;
    }
    return true;
}

Result<void> copy_tree_merge(const std::filesystem::path& source,
                             const std::filesystem::path& destination,
                             int& copied) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(source, ec)) {
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            return Error::make(210, "无法创建目标目录",
                               path_to_utf8(destination.parent_path()) + ": " +
                                   ec.message());
        }
        std::filesystem::copy_file(
            source, destination,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            return Error::make(211, "复制文件失败",
                               path_to_utf8(source) + " -> " +
                                   path_to_utf8(destination) + ": " +
                                   ec.message());
        }
        ++copied;
        return {};
    }
    if (!std::filesystem::is_directory(source, ec)) return {};
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        return Error::make(210, "无法创建覆盖文件目录",
                           path_to_utf8(destination) + ": " + ec.message());
    }

    for (std::filesystem::directory_iterator it(source, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::filesystem::path target =
            destination / it->path().filename();
        if (it->is_directory(ec)) {
            auto nested = copy_tree_merge(it->path(), target, copied);
            if (!nested) return nested.error();
        } else {
            std::filesystem::copy_file(
                it->path(), target,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return Error::make(211, "复制整合包覆盖文件失败",
                                   path_to_utf8(it->path()) + " -> " +
                                       path_to_utf8(target) + ": " +
                                       ec.message());
            }
            ++copied;
        }
    }
    if (ec) {
        return Error::make(212, "读取整合包覆盖文件失败",
                           path_to_utf8(source) + ": " + ec.message());
    }
    return {};
}

std::filesystem::path find_minecraft_root(const std::filesystem::path& temp) {
    const auto usable = [](const std::filesystem::path& path) {
        std::error_code ec;
        return std::filesystem::is_directory(path, ec) &&
            (std::filesystem::is_directory(path / L"versions", ec) ||
             std::filesystem::is_directory(path / L"mods", ec) ||
             std::filesystem::is_directory(path / L"config", ec));
    };

    if (usable(temp)) return temp;

    std::error_code ec;
    for (std::filesystem::directory_iterator top(temp, ec), end;
         !ec && top != end; top.increment(ec)) {
        if (!top->is_directory(ec)) continue;
        const std::wstring name = top->path().filename().wstring();
        if (name == L"overrides" || name == L"client-overrides") continue;
        if (usable(top->path())) return top->path();
        for (std::filesystem::directory_iterator child(top->path(), ec), end2;
             !ec && child != end2; child.increment(ec)) {
            if (child->is_directory(ec) && usable(child->path())) {
                return child->path();
            }
        }
    }
    return {};
}

// PCL / 普通 .minecraft 目录导出：版本 JSON、库和资源已经打包在压缩包里。
Result<ModpackImportResult> import_minecraft_folder(
    const ModpackImportRequest& request,
    const std::filesystem::path& source_mc,
    const DownloadProgressCallback& progress, DownloadCancelToken& cancel) {
    std::error_code ec;
    const std::filesystem::path versions = source_mc / L"versions";
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator it(versions, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::filesystem::path dir = it->path();
        const std::filesystem::path json =
            dir / (dir.filename().wstring() + L".json");
        if (std::filesystem::is_regular_file(json, ec)) {
            candidates.push_back(dir);
        }
    }
    if (candidates.empty()) {
        return Error::make(230, "压缩包里没有可用的版本 JSON",
                           "只支持带 .minecraft/versions/<版本>/<版本>.json 的整合包");
    }

    // PCL 导出通常只有一个版本；有多个时优先带 PCL/Setup.ini 的那个。
    std::sort(candidates.begin(), candidates.end(),
              [](const std::filesystem::path& a,
                 const std::filesystem::path& b) {
                  return a.filename().wstring() < b.filename().wstring();
              });
    std::filesystem::path chosen = candidates.front();
    for (const auto& dir : candidates) {
        if (std::filesystem::is_regular_file(dir / L"PCL" / L"Setup.ini", ec)) {
            chosen = dir;
            break;
        }
    }

    const std::string original_id = path_to_utf8(chosen.filename());
    std::ifstream version_file(
        chosen / (chosen.filename().wstring() + L".json"), std::ios::binary);
    nlohmann::json version_json;
    try {
        version_file >> version_json;
    } catch (const std::exception& ex) {
        return Error::make(231, "版本 JSON 解析失败", ex.what());
    }
    if (!version_json.is_object()) {
        return Error::make(232, "版本 JSON 不是对象", original_id);
    }

    std::string display_name = request.instance_name;
    if (display_name.empty()) {
        display_name = path_to_utf8(request.archive.stem());
    }
    const std::string instance =
        unique_instance_name(request.mc_dir,
                             sanitize_instance_name(display_name));

    report(progress, "整合包", "正在导入 Minecraft 版本…", original_id);
    int copied = 0;

    // 先合并 libraries / assets，游戏启动时直接使用。
    auto libs = copy_tree_merge(source_mc / L"libraries",
                                request.mc_dir / L"libraries", copied);
    if (!libs) return libs.error();
    auto assets = copy_tree_merge(source_mc / L"assets",
                                  request.mc_dir / L"assets", copied);
    if (!assets) return assets.error();

    const std::filesystem::path version_dir =
        request.mc_dir / L"versions" / path_from_utf8(instance);
    std::filesystem::create_directories(version_dir, ec);
    if (ec) {
        return Error::make(233, "无法创建实例目录",
                           path_to_utf8(version_dir) + ": " + ec.message());
    }
    auto version_copy = copy_tree_merge(chosen, version_dir, copied);
    if (!version_copy) return version_copy.error();

    // 版本目录和 JAR/JSON 文件名都改成实例名，避免和已有版本冲突。
    const std::wstring old_name = chosen.filename().wstring();
    const std::wstring new_name = path_from_utf8(instance).wstring();
    const std::filesystem::path old_json = version_dir / (old_name + L".json");
    const std::filesystem::path new_json = version_dir / (new_name + L".json");
    const std::filesystem::path old_jar = version_dir / (old_name + L".jar");
    const std::filesystem::path new_jar = version_dir / (new_name + L".jar");

    version_json["id"] = instance;
    if (version_json.contains("jar")) version_json["jar"] = instance;
    {
        std::ofstream output(new_json, std::ios::binary | std::ios::trunc);
        if (!output) {
            return Error::make(234, "无法写入版本 JSON",
                               path_to_utf8(new_json));
        }
        output << version_json.dump(2);
    }
    if (old_json != new_json) std::filesystem::remove(old_json, ec);
    if (std::filesystem::is_regular_file(old_jar, ec) && old_jar != new_jar) {
        std::filesystem::rename(old_jar, new_jar, ec);
        if (ec) {
            return Error::make(235, "无法重命名版本 JAR",
                               path_to_utf8(old_jar) + ": " + ec.message());
        }
    }

    InstanceSettings settings;
    settings.isolate = true;
    settings.isolate_saves = true;
    auto saved = save_instance_settings(version_dir, settings);
    if (!saved) return saved.error();

    const std::filesystem::path game_dir = version_dir / L"game";
    std::filesystem::create_directories(game_dir, ec);
    if (ec) {
        return Error::make(236, "无法创建游戏目录",
                           path_to_utf8(game_dir) + ": " + ec.message());
    }

    report(progress, "整合包", "正在复制 mods、资源包和存档…");
    const std::set<std::wstring> skip = {
        L"versions", L"libraries", L"assets", L"natives",
        L"logs", L"crash-reports", L"PCL",
    };
    for (std::filesystem::directory_iterator it(source_mc, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (cancel.load(std::memory_order_relaxed)) {
            return Error::make(499, "下载已取消");
        }
        const std::wstring name = it->path().filename().wstring();
        if (skip.find(name) != skip.end()) continue;
        auto merged = copy_tree_merge(it->path(), game_dir / name, copied);
        if (!merged) return merged.error();
    }

    ModpackImportResult result;
    result.instance_id = instance;
    result.name = display_name;
    result.pack_version = original_id;
    result.game_version = original_id;
    result.loader_version.clear();
    result.files_downloaded = 0;
    result.files_skipped = 0;
    if (auto scanned = scan_instances(request.mc_dir)) {
        for (const auto& item : scanned.value()) {
            if (item.id == instance) {
                result.loader = item.loader;
                break;
            }
        }
    }
    if (result.loader.empty()) result.loader = "vanilla";

    nlohmann::json metadata{
        {"source", "minecraft-folder"},
        {"name", display_name},
        {"original_version", original_id},
        {"game_version", result.game_version},
        {"loader", result.loader},
        {"copied_files", copied},
    };
    std::ofstream meta_file(version_dir / L"cmcl-modpack.json",
                            std::ios::binary | std::ios::trunc);
    if (meta_file) meta_file << metadata.dump(2);

    report(progress, "完成", "整合包安装完成：" + display_name, instance);
    Log::info("整合包导入完成：" + display_name + " -> " + instance +
              "（版本 " + original_id + "，复制 " + std::to_string(copied) +
              " 个文件）");
    return result;
}

} // namespace

Result<ModpackImportResult> import_modrinth_modpack(
    const ModpackImportRequest& request,
    const DownloadProgressCallback& progress,
    DownloadCancelToken& cancel) {
    std::error_code ec;
    if (!std::filesystem::is_directory(request.mc_dir, ec)) {
        return Error::make(200, "游戏目录不存在",
                           path_to_utf8(request.mc_dir));
    }
    if (!std::filesystem::is_regular_file(request.archive, ec)) {
        return Error::make(201, "整合包文件不存在",
                           path_to_utf8(request.archive));
    }

    const std::filesystem::path temp_root =
        local_app_data_dir() / L"CraftLauncher" / L"cache" / L"modpack-import";
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        return Error::make(202, "无法创建整合包临时目录",
                           path_to_utf8(temp_root) + ": " + ec.message());
    }
    TempDir cleanup{temp_root};

    report(progress, "整合包", "正在解压整合包…");
    auto extracted = extract_zip(request.archive, temp_root);
    if (!extracted) return extracted.error();

    // PCL / 普通 .minecraft 导出：没有 Modrinth 清单，但自带版本 JSON、
    // libraries 和 assets，可以直接作为实例导入。
    const std::filesystem::path modrinth_index =
        temp_root / L"modrinth.index.json";
    if (!std::filesystem::is_regular_file(modrinth_index, ec)) {
        const std::filesystem::path source_mc = find_minecraft_root(temp_root);
        if (!source_mc.empty()) {
            return import_minecraft_folder(request, source_mc, progress, cancel);
        }
    }

    const std::filesystem::path index_path = temp_root / L"modrinth.index.json";
    std::ifstream index_file(index_path, std::ios::binary);
    if (!index_file) {
        if (std::filesystem::is_regular_file(temp_root / L"manifest.json", ec) ||
            std::filesystem::is_regular_file(temp_root / L"minecraftinstance.json",
                                             ec)) {
            return Error::make(
                203, "检测到 CurseForge 整合包",
                "当前只支持 Modrinth .mrpack（或包含 modrinth.index.json 的 "
                ".zip）；CurseForge 整合包导入尚未接入。");
        }
        return Error::make(
            203, "整合包缺少 modrinth.index.json",
            "只支持 Modrinth .mrpack，或包含 modrinth.index.json 的 .zip：" +
                path_to_utf8(request.archive));
    }

    nlohmann::json index;
    try {
        index_file >> index;
    } catch (const std::exception& ex) {
        return Error::make(204, "整合包清单解析失败", ex.what());
    }
    if (!index.is_object()) {
        return Error::make(205, "整合包清单不是对象");
    }

    const std::string pack_name = json_string(index, "name", "Modrinth 整合包");
    const std::string pack_version = json_string(index, "versionId");
    const std::string pack_summary = json_string(index, "summary");

    const auto deps_it = index.find("dependencies");
    if (deps_it == index.end() || !deps_it->is_object()) {
        return Error::make(206, "整合包缺少 dependencies");
    }
    const nlohmann::json& deps = *deps_it;
    const std::string game_version = json_string(deps, "minecraft");
    if (game_version.empty()) {
        return Error::make(207, "整合包没有指定 Minecraft 版本");
    }

    std::optional<LoaderKind> loader_kind;
    std::string loader_version;
    if (!json_string(deps, "neoforge").empty()) {
        loader_kind = LoaderKind::NeoForge;
        loader_version = json_string(deps, "neoforge");
    } else if (!json_string(deps, "forge").empty()) {
        loader_kind = LoaderKind::Forge;
        loader_version = json_string(deps, "forge");
    } else if (!json_string(deps, "quilt-loader").empty()) {
        loader_kind = LoaderKind::Quilt;
        loader_version = json_string(deps, "quilt-loader");
    } else if (!json_string(deps, "fabric-loader").empty()) {
        loader_kind = LoaderKind::Fabric;
        loader_version = json_string(deps, "fabric-loader");
    }

    if (loader_kind) {
        // Modrinth 的 Forge 依赖通常只写加载器版本，兼容带有 MC 前缀的写法。
        if (*loader_kind == LoaderKind::Forge) {
            const std::string prefix = game_version + "-";
            if (loader_version.rfind(prefix, 0) == 0) {
                loader_version = loader_version.substr(prefix.size());
            }
        }
        if (loader_version.empty()) {
            auto versions = fetch_loader_versions(*loader_kind, game_version);
            if (!versions) return versions.error();
            if (versions.value().empty()) {
                return Error::make(208, "没有可用的加载器版本",
                                   loader_display_name(*loader_kind));
            }
            loader_version = versions.value().front().version;
        }
    }

    const std::string requested_name = request.instance_name.empty()
                                           ? pack_name
                                           : request.instance_name;
    const std::string instance =
        unique_instance_name(request.mc_dir,
                             sanitize_instance_name(requested_name));

    report(progress, "整合包",
           loader_kind ? "正在安装 Minecraft " + game_version + " 和 " +
                             loader_display_name(*loader_kind)
                       : "正在安装 Minecraft " + game_version,
           instance);

    if (loader_kind) {
        LoaderInstallRequest install;
        install.mc_dir = request.mc_dir;
        install.game_version = game_version;
        install.loader_version = loader_version;
        install.instance_name = instance;
        install.kind = *loader_kind;
        install.source = request.source;
        auto installed = install_loader(install, progress, cancel);
        if (!installed) return installed.error();
    } else {
        DownloadRequest install;
        install.mc_dir = request.mc_dir;
        install.version_id = game_version;
        install.instance_name = instance;
        install.source = request.source;
        auto installed = install_version(install, progress, cancel);
        if (!installed) return installed.error();
    }

    if (cancel.load(std::memory_order_relaxed)) {
        return Error::make(499, "下载已取消");
    }

    const std::filesystem::path version_dir =
        request.mc_dir / L"versions" / path_from_utf8(instance);
    if (!std::filesystem::is_directory(version_dir, ec)) {
        return Error::make(209, "整合包实例目录创建失败",
                           path_to_utf8(version_dir));
    }

    // 整合包默认完全隔离，避免 mods / config 污染其它实例。
    InstanceSettings settings;
    settings.isolate = true;
    settings.isolate_saves = true;
    auto saved = save_instance_settings(version_dir, settings);
    if (!saved) return saved.error();

    const std::filesystem::path game_dir = version_dir / L"game";
    std::filesystem::create_directories(game_dir, ec);
    if (ec) {
        return Error::make(213, "无法创建整合包游戏目录",
                           path_to_utf8(game_dir) + ": " + ec.message());
    }

    struct PackFile {
        std::filesystem::path relative;
        std::string sha1;
        std::string sha512;
        unsigned long long size = 0;
        std::vector<std::string> urls;
    };

    std::vector<PackFile> files;
    const auto files_it = index.find("files");
    if (files_it != index.end() && files_it->is_array()) {
        for (const auto& item : *files_it) {
            if (!item.is_object()) continue;

            if (const auto env = item.find("env");
                env != item.end() && env->is_object()) {
                if (lower_copy(json_string(*env, "client", "required")) ==
                    "unsupported") {
                    continue;
                }
            }

            PackFile file;
            const std::string path_text = json_string(item, "path");
            if (!safe_relative_path(path_text, file.relative)) {
                return Error::make(214, "整合包文件路径不合法", path_text);
            }

            if (const auto hashes = item.find("hashes");
                hashes != item.end() && hashes->is_object()) {
                file.sha1 = lower_copy(json_string(*hashes, "sha1"));
                file.sha512 = lower_copy(json_string(*hashes, "sha512"));
            }
            if (const auto size = item.find("fileSize");
                size != item.end()) {
                if (size->is_number_unsigned()) {
                    file.size = size->get<unsigned long long>();
                } else if (size->is_number_integer()) {
                    const auto value = size->get<long long>();
                    if (value > 0) {
                        file.size = static_cast<unsigned long long>(value);
                    }
                }
            }
            file.urls = json_strings(item, "downloads");
            if (file.urls.empty()) {
                return Error::make(215, "整合包文件没有下载地址", path_text);
            }
            files.push_back(std::move(file));
        }
    }

    ModpackImportResult result;
    result.instance_id = instance;
    result.name = pack_name;
    result.pack_version = pack_version;
    result.game_version = game_version;
    result.loader =
        loader_kind ? loader_kind_key(*loader_kind) : std::string("vanilla");
    result.loader_version = loader_version;

    const int total = static_cast<int>(files.size());
    for (int i = 0; i < total; ++i) {
        if (cancel.load(std::memory_order_relaxed)) {
            return Error::make(499, "下载已取消");
        }

        const PackFile& file = files[static_cast<size_t>(i)];
        const std::filesystem::path destination = game_dir / file.relative;
        const std::string display = path_to_utf8(file.relative);

        if (verify_file(destination, file.sha1, file.sha512, file.size)) {
            ++result.files_skipped;
            report(progress, "整合包", "已存在，跳过", display, i, total,
                   file.size, file.size);
            continue;
        }

        report(progress, "整合包", "正在下载整合包文件", display, i, total, 0,
               file.size);

        std::string last_error;
        bool downloaded = false;
        for (const auto& url : file.urls) {
            if (cancel.load(std::memory_order_relaxed)) {
                return Error::make(499, "下载已取消");
            }
            HttpOptions options;
            options.expected_size = file.size;
            options.timeout_seconds = 120;
            options.headers["User-Agent"] = kUserAgent;

            auto fetched = http_download(
                url, destination, options,
                [&](const HttpProgress& http) {
                    report(progress, "整合包", "正在下载整合包文件", display, i,
                           total, http.downloaded,
                           http.total > 0 ? http.total : file.size);
                },
                &cancel);
            if (fetched) {
                downloaded = true;
                break;
            }
            last_error = fetched.error().to_string();
            std::filesystem::remove(destination, ec);
        }

        if (!downloaded) {
            return Error::make(216, "整合包文件下载失败",
                               display + "\n" + last_error);
        }
        if (!verify_file(destination, file.sha1, file.sha512, file.size)) {
            std::filesystem::remove(destination, ec);
            return Error::make(217, "整合包文件校验失败", display);
        }
        ++result.files_downloaded;
        report(progress, "整合包", "整合包文件已下载", display, i + 1, total,
               file.size, file.size);
    }

    if (cancel.load(std::memory_order_relaxed)) {
        return Error::make(499, "下载已取消");
    }

    report(progress, "整合包", "正在解压整合包覆盖文件…");
    int overrides = 0;
    auto base_overrides =
        copy_tree_merge(temp_root / L"overrides", game_dir, overrides);
    if (!base_overrides) return base_overrides.error();
    auto client_overrides =
        copy_tree_merge(temp_root / L"client-overrides", game_dir, overrides);
    if (!client_overrides) return client_overrides.error();

    nlohmann::json metadata{
        {"source", "modrinth"},
        {"name", pack_name},
        {"version_id", pack_version},
        {"summary", pack_summary},
        {"game_version", game_version},
        {"loader", result.loader},
        {"loader_version", loader_version},
        {"files_downloaded", result.files_downloaded},
        {"files_skipped", result.files_skipped},
        {"overrides", overrides},
    };
    std::ofstream meta_file(version_dir / L"cmcl-modpack.json",
                            std::ios::binary | std::ios::trunc);
    if (meta_file) meta_file << metadata.dump(2);

    report(progress, "完成", "整合包安装完成：" + pack_name, instance);
    Log::info("整合包安装完成：" + pack_name + " -> " + instance + "（下载 " +
              std::to_string(result.files_downloaded) + "，跳过 " +
              std::to_string(result.files_skipped) + "，覆盖 " +
              std::to_string(overrides) + "）");
    return result;
}

} // namespace cl::mc
