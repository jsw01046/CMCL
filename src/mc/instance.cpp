#include "mc/instance.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>

#include "core/encoding.h"
#include "core/log.h"
#include "mc/version.h"

namespace cl::mc {

namespace {

std::filesystem::path instance_dir(const std::filesystem::path& mc_dir,
                                   const std::string& id) {
    return mc_dir / L"versions" / path_from_utf8(id);
}

std::optional<std::wstring> resource_subdir(const std::string& kind) {
    if (kind == "mods") return L"mods";
    if (kind == "resourcepacks") return L"resourcepacks";
    if (kind == "shaderpacks") return L"shaderpacks";
    if (kind == "datapacks") return L"datapacks";
    return std::nullopt;
}

bool safe_resource_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('\0') != std::string::npos) return false;
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        return false;
    }
    return true;
}

bool has_disabled_suffix(const std::string& name) {
    constexpr std::string_view suffix = ".disabled";
    return name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_temporary_resource(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    const auto ends_with = [&lower](std::string_view suffix) {
        return lower.size() >= suffix.size() &&
            lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) ==
                0;
    };
    return ends_with(".part") || ends_with(".tmp") || lower == "desktop.ini";
}

long long file_time_to_unix(std::filesystem::file_time_type time) {
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(
               system.time_since_epoch())
        .count();
}

Result<void> move_path_to_recycle_bin(const std::filesystem::path& path,
                                      const std::string& label) {
    // 只对本地固定磁盘使用回收站，避免网络盘上直接永久删除。
    const std::wstring root = path.root_name().wstring();
    if (!root.empty()) {
        const std::wstring drive = root + L"\\";
        if (::GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) {
            return Error::make(41, "只支持删除本地磁盘上的" + label,
                               path_to_utf8(path));
        }
    }

    std::wstring from = path.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
                       FOF_NOERRORUI;

    const int result = ::SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        return Error::make(42, "删除" + label + "失败",
                           "Shell 错误码 " + std::to_string(result));
    }
    return {};
}

bool valid_instance_name(const std::string& name) {
    if (name.empty() || name.size() > 200 || name == "." || name == "..") {
        return false;
    }

    constexpr std::string_view invalid = "<>:\"/\\|?*";
    for (const unsigned char c : name) {
        if (c < 0x20) return false;
        if (invalid.find(static_cast<char>(c)) != std::string_view::npos) {
            return false;
        }
    }
    if (name.back() == ' ' || name.back() == '.') return false;

    std::string base = name;
    if (const size_t dot = base.find('.'); dot != std::string::npos) {
        base.resize(dot);
    }
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    static const std::set<std::string> reserved = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
        "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return reserved.find(base) == reserved.end();
}

Result<void> write_json_file(const std::filesystem::path& path,
                             const nlohmann::json& json) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return Error::make(20, "无法写入 JSON", path_to_utf8(path));
    file << json.dump(2);
    if (!file) return Error::make(21, "写入 JSON 失败", path_to_utf8(path));
    return {};
}

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string loader_from_id(std::string id) {
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (id.find("neoforge") != std::string::npos) return "neoforge";
    if (id.find("forge") != std::string::npos) return "forge";
    if (id.find("fabric") != std::string::npos) return "fabric";
    if (id.find("quilt") != std::string::npos) return "quilt";
    if (id.find("optifine") != std::string::npos) return "optifine";
    return "vanilla";
}

// 通过加载器库和主类判断实例属于原版还是某个模组加载器。
std::string loader_from_json(const nlohmann::json& json) {
    bool forge = false;
    bool neoforge = false;
    bool fabric = false;
    bool quilt = false;
    bool optifine = false;

    if (const auto libraries = json.find("libraries");
        libraries != json.end() && libraries->is_array()) {
        for (const auto& library : *libraries) {
            if (!library.is_object()) continue;
            const auto name_it = library.find("name");
            if (name_it == library.end() || !name_it->is_string()) continue;
            const std::string name = name_it->get<std::string>();

            if (starts_with(name, "net.neoforged:")) neoforge = true;
            if (starts_with(name, "net.minecraftforge:forge:") ||
                starts_with(name, "net.minecraftforge:minecraftforge:") ||
                starts_with(name, "net.minecraftforge:fmlloader:") ||
                starts_with(name, "net.minecraftforge:forgeclient:")) {
                forge = true;
            }
            if (starts_with(name, "net.fabricmc:fabric-loader:")) fabric = true;
            if (starts_with(name, "org.quiltmc:quilt-loader:")) quilt = true;
            if (starts_with(name, "optifine:OptiFine:") ||
                starts_with(name, "optifine:launchwrapper-of:")) {
                optifine = true;
            }
        }
    }

    const std::string main_class = json.value("mainClass", std::string{});
    if (starts_with(main_class, "net.neoforged.")) neoforge = true;
    if (starts_with(main_class, "net.minecraftforge.")) forge = true;
    if (starts_with(main_class, "net.fabricmc.")) fabric = true;
    if (starts_with(main_class, "org.quiltmc.")) quilt = true;
    if (starts_with(main_class, "optifine.")) optifine = true;

    // 1.6.x 的 Forge 通过 LaunchWrapper tweak 启动，库名不总是带 forge。
    if (const auto args = json.find("minecraftArguments");
        args != json.end() && args->is_string()) {
        const std::string text = args->get<std::string>();
        if (text.find("FMLTweaker") != std::string::npos ||
            text.find("fml.common.launcher") != std::string::npos) {
            forge = true;
        }
    }

    if (neoforge) return "neoforge";
    if (quilt) return "quilt";
    if (forge) return "forge";
    if (fabric) return "fabric";
    if (optifine) return "optifine";
    return {};
}

std::string resolve_loader(const std::filesystem::path& versions_dir,
                           const nlohmann::json& json,
                           const std::string& id) {
    std::string loader = loader_from_json(json);
    if (!loader.empty()) return loader;

    std::set<std::string> seen{id};
    std::string parent = json.value("inheritsFrom", std::string{});
    for (int depth = 0; depth < 8 && !parent.empty(); ++depth) {
        if (!seen.insert(parent).second) break;
        const std::filesystem::path parent_id = path_from_utf8(parent);
        auto parent_json =
            load_json(versions_dir / parent_id / (parent_id.wstring() + L".json"));
        if (!parent_json || !parent_json.value().is_object()) break;

        loader = loader_from_json(parent_json.value());
        if (!loader.empty()) return loader;
        parent = parent_json.value().value("inheritsFrom", std::string{});
    }

    return loader_from_id(id);
}

} // namespace

bool is_valid_instance_name(std::string_view name) {
    return valid_instance_name(std::string(name));
}

Result<void> ensure_game_dir(const std::filesystem::path& mc_dir) {
    if (mc_dir.empty()) {
        return Error::make(3, "游戏目录为空");
    }

    std::error_code ec;
    const bool existed = std::filesystem::is_directory(mc_dir, ec);
    if (existed) ec.clear();

    const std::filesystem::path dirs[] = {
        mc_dir,
        mc_dir / L"versions",
        mc_dir / L"libraries",
        mc_dir / L"assets",
        mc_dir / L"logs",
    };
    for (const auto& dir : dirs) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return Error::make(4, "无法初始化游戏目录",
                               path_to_utf8(dir) + ": " + ec.message());
        }
    }
    if (!existed) {
        Log::info("已创建游戏目录：" + path_to_utf8(mc_dir));
    }
    return {};
}

Result<std::vector<InstanceSummary>> scan_instances(
    const std::filesystem::path& mc_dir) {
    auto ensured = ensure_game_dir(mc_dir);
    if (!ensured) return ensured.error();

    const std::filesystem::path versions_dir = mc_dir / L"versions";
    std::error_code ec;

    std::vector<InstanceSummary> result;
    for (const auto& entry : std::filesystem::directory_iterator(versions_dir, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        const std::filesystem::path dir = entry.path();
        const std::wstring folder = dir.filename().wstring();
        InstanceSummary summary;
        summary.id = path_to_utf8(folder);

        const std::filesystem::path json_path = dir / (folder + L".json");
        if (!std::filesystem::exists(json_path, ec)) {
            summary.valid = false;
            summary.error = "缺少 " + summary.id + ".json";
            result.push_back(std::move(summary));
            continue;
        }

        auto loaded = load_json(json_path);
        if (!loaded) {
            summary.valid = false;
            summary.error = loaded.error().to_string();
            result.push_back(std::move(summary));
            continue;
        }

        const nlohmann::json& json = loaded.value();
        if (!json.is_object()) {
            summary.valid = false;
            summary.error = "JSON 不是对象";
            result.push_back(std::move(summary));
            continue;
        }

        if (const auto it = json.find("type"); it != json.end() && it->is_string()) {
            summary.type = it->get<std::string>();
        }
        if (const auto it = json.find("mainClass");
            it != json.end() && it->is_string()) {
            summary.main_class = it->get<std::string>();
        }
        if (const auto it = json.find("inheritsFrom");
            it != json.end() && it->is_string()) {
            summary.inherits_from = it->get<std::string>();
        }
        summary.loader = resolve_loader(versions_dir, json, summary.id);
        if (auto settings = load_instance_settings(dir)) {
            summary.isolate = settings.value().isolate;
        }

        result.push_back(std::move(summary));
    }

    std::sort(result.begin(), result.end(),
              [](const InstanceSummary& a, const InstanceSummary& b) {
                  return a.id < b.id;
              });
    Log::info("扫描到 " + std::to_string(result.size()) + " 个版本实例");
    return result;
}

Result<InstanceSettings> load_instance_settings(
    const std::filesystem::path& version_dir) {
    const std::filesystem::path file = version_dir / L"craftlauncher.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return InstanceSettings{};
    }

    auto loaded = load_json(file);
    if (!loaded) return loaded.error();
    if (!loaded.value().is_object()) {
        return Error::make(22, "实例设置不是对象", path_to_utf8(file));
    }

    InstanceSettings settings;
    if (const auto it = loaded.value().find("isolate");
        it != loaded.value().end() && it->is_boolean()) {
        settings.isolate = it->get<bool>();
    }
    if (const auto it = loaded.value().find("isolateSaves");
        it != loaded.value().end() && it->is_boolean()) {
        settings.isolate_saves = it->get<bool>();
    }
    return settings;
}

Result<void> save_instance_settings(const std::filesystem::path& version_dir,
                                    const InstanceSettings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(version_dir, ec);
    if (ec) {
        return Error::make(23, "无法创建实例目录",
                           path_to_utf8(version_dir) + ": " + ec.message());
    }
    return write_json_file(
        version_dir / L"craftlauncher.json",
        nlohmann::json{{"isolate", settings.isolate},
                       {"isolateSaves", settings.isolate_saves}});
}

Result<std::string> rename_instance(const std::filesystem::path& mc_dir,
                                    const std::string& old_id,
                                    const std::string& new_id) {
    if (!valid_instance_name(new_id)) {
        return Error::make(30, "版本名不合法", new_id);
    }
    if (old_id == new_id) return new_id;

    const std::filesystem::path old_dir = instance_dir(mc_dir, old_id);
    const std::filesystem::path new_dir = instance_dir(mc_dir, new_id);
    std::error_code ec;

    if (!std::filesystem::is_directory(old_dir, ec)) {
        return Error::make(31, "版本目录不存在", path_to_utf8(old_dir));
    }
    if (std::filesystem::exists(new_dir, ec)) {
        return Error::make(32, "目标版本名已存在", new_id);
    }

    std::filesystem::rename(old_dir, new_dir, ec);
    if (ec) {
        return Error::make(33, "重命名版本目录失败",
                           path_to_utf8(old_dir) + ": " + ec.message());
    }

    const std::filesystem::path old_path = path_from_utf8(old_id);
    const std::filesystem::path new_path = path_from_utf8(new_id);
    const std::filesystem::path old_jar = new_dir / (old_path.wstring() + L".jar");
    const std::filesystem::path new_jar = new_dir / (new_path.wstring() + L".jar");
    const std::filesystem::path old_json =
        new_dir / (old_path.wstring() + L".json");
    const std::filesystem::path new_json =
        new_dir / (new_path.wstring() + L".json");

    bool jar_renamed = false;
    const auto rollback = [&] {
        std::error_code rollback_ec;
        if (jar_renamed) {
            std::filesystem::rename(new_jar, old_jar, rollback_ec);
        }
        std::filesystem::rename(new_dir, old_dir, rollback_ec);
    };

    if (std::filesystem::is_regular_file(old_jar, ec)) {
        std::filesystem::rename(old_jar, new_jar, ec);
        if (ec) {
            rollback();
            return Error::make(34, "重命名主 jar 失败",
                               path_to_utf8(old_jar) + ": " + ec.message());
        }
        jar_renamed = true;
    }

    auto loaded = load_json(old_json);
    if (!loaded) {
        rollback();
        return Error::make(35, "读取版本 JSON 失败", loaded.error().to_string());
    }

    nlohmann::json json = std::move(loaded).value();
    json["id"] = new_id;
    if (const auto it = json.find("jar");
        it != json.end() && it->is_string() && it->get<std::string>() == old_id) {
        json["jar"] = new_id;
    }

    auto written = write_json_file(new_json, json);
    if (!written) {
        std::error_code remove_ec;
        std::filesystem::remove(new_json, remove_ec);
        rollback();
        return written.error();
    }
    std::filesystem::remove(old_json, ec);

    // 修正其它实例里对旧版本名的 inheritsFrom / jar 引用。
    const std::filesystem::path versions_dir = mc_dir / L"versions";
    std::error_code scan_ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(versions_dir, scan_ec)) {
        if (scan_ec) break;
        if (!entry.is_directory(scan_ec)) continue;
        if (entry.path() == new_dir) continue;

        const std::wstring folder = entry.path().filename().wstring();
        const std::filesystem::path child_json =
            entry.path() / (folder + L".json");
        if (!std::filesystem::is_regular_file(child_json, scan_ec)) continue;

        auto child = load_json(child_json);
        if (!child) continue;
        nlohmann::json& child_json_data = child.value();
        if (!child_json_data.is_object()) continue;

        bool changed = false;
        if (const auto it = child_json_data.find("inheritsFrom");
            it != child_json_data.end() && it->is_string() &&
            it->get<std::string>() == old_id) {
            child_json_data["inheritsFrom"] = new_id;
            changed = true;
        }
        if (const auto it = child_json_data.find("jar");
            it != child_json_data.end() && it->is_string() &&
            it->get<std::string>() == old_id) {
            child_json_data["jar"] = new_id;
            changed = true;
        }
        if (changed) {
            auto saved = write_json_file(child_json, child_json_data);
            if (!saved) {
                Log::warn("更新子版本引用失败：" + path_to_utf8(child_json) +
                          " -> " + saved.error().to_string());
            }
        }
    }

    Log::info("版本已重命名：" + old_id + " -> " + new_id);
    return new_id;
}

Result<void> delete_instance(const std::filesystem::path& mc_dir,
                             const std::string& id) {
    const std::filesystem::path dir = instance_dir(mc_dir, id);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return Error::make(40, "版本目录不存在", path_to_utf8(dir));
    }

    auto moved = move_path_to_recycle_bin(dir, "版本");
    if (!moved) return moved.error();
    Log::info("版本已移入回收站：" + id);
    return {};
}

Result<std::filesystem::path> instance_saves_dir(
    const std::filesystem::path& mc_dir, const std::string& id) {
    const std::filesystem::path version_dir = instance_dir(mc_dir, id);
    std::error_code ec;
    if (!std::filesystem::is_directory(version_dir, ec)) {
        return Error::make(52, "版本目录不存在", path_to_utf8(version_dir));
    }

    auto settings = load_instance_settings(version_dir);
    if (!settings) return settings.error();

    std::filesystem::path saves;
    if (settings.value().isolate || settings.value().isolate_saves) {
        saves = version_dir / L"game" / L"saves";
    } else {
        saves = mc_dir / L"saves";
    }

    std::filesystem::create_directories(saves, ec);
    if (ec) {
        return Error::make(53, "无法创建存档目录",
                           path_to_utf8(saves) + ": " + ec.message());
    }
    return saves;
}

Result<void> open_instance_saves(const std::filesystem::path& mc_dir,
                                 const std::string& id) {
    auto saves = instance_saves_dir(mc_dir, id);
    if (!saves) return saves.error();

    const HINSTANCE result = ::ShellExecuteW(
        nullptr, L"open", saves.value().c_str(), nullptr, nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return Error::make(54, "无法打开存档文件夹",
                           path_to_utf8(saves.value()));
    }
    return {};
}

Result<void> open_instance_folder(const std::filesystem::path& mc_dir,
                                  const std::string& id) {
    const std::filesystem::path dir = instance_dir(mc_dir, id);
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return Error::make(50, "版本目录不存在", path_to_utf8(dir));
    }
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return Error::make(51, "无法打开文件夹", path_to_utf8(dir));
    }
    return {};
}

Result<std::filesystem::path> instance_resource_dir(
    const std::filesystem::path& mc_dir, const std::string& id,
    const std::string& kind) {
    const auto sub = resource_subdir(kind);
    if (!sub) return Error::make(60, "未知的资源类型", kind);

    if (id.empty()) return mc_dir / sub.value();

    const std::filesystem::path version_dir = instance_dir(mc_dir, id);
    std::error_code ec;
    if (!std::filesystem::is_directory(version_dir, ec)) {
        return Error::make(61, "版本目录不存在", path_to_utf8(version_dir));
    }

    auto settings = load_instance_settings(version_dir);
    if (!settings) return settings.error();

    if (settings.value().isolate) {
        return version_dir / L"game" / sub.value();
    }
    if (settings.value().isolate_saves) {
        // 存档单独隔离时，启动阶段会把 game/mods 等做成指向全局目录的联接。
        // 覆盖层还没建立时直接使用全局目录，避免提前创建真实目录而破坏共享。
        const std::filesystem::path overlay =
            version_dir / L"game" / sub.value();
        if (std::filesystem::exists(overlay, ec)) return overlay;
    }
    return mc_dir / sub.value();
}

Result<InstanceResourceList> list_instance_resources(
    const std::filesystem::path& mc_dir, const std::string& id,
    const std::string& kind) {
    auto dir = instance_resource_dir(mc_dir, id, kind);
    if (!dir) return dir.error();

    std::error_code ec;
    std::filesystem::create_directories(dir.value(), ec);
    if (ec) {
        return Error::make(62, "无法创建资源目录",
                           path_to_utf8(dir.value()) + ": " + ec.message());
    }

    InstanceResourceList result;
    result.dir = dir.value();
    const std::filesystem::path version_dir = instance_dir(mc_dir, id);
    if (!id.empty()) {
        if (auto settings = load_instance_settings(version_dir)) {
            result.isolate = settings.value().isolate;
            result.isolate_saves = settings.value().isolate_saves;
        }
    }

    // 目录联接或直接落在全局目录时，资源实际是与其它实例共用的。
    if (!result.isolate) {
        if (const auto sub = resource_subdir(kind)) {
            std::error_code eq_ec;
            result.shared =
                std::filesystem::equivalent(result.dir, mc_dir / sub.value(),
                                            eq_ec) && !eq_ec;
        }
    }

    for (std::filesystem::directory_iterator it(dir.value(), ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        ResourceEntry entry;
        entry.name = path_to_utf8(it->path().filename());
        if (is_temporary_resource(entry.name)) continue;
        entry.directory = it->is_directory(entry_ec);
        entry_ec.clear();
        if (!entry.directory) {
            entry.size = it->file_size(entry_ec);
            if (entry_ec) entry.size = 0;
            entry_ec.clear();
        }
        const auto modified = it->last_write_time(entry_ec);
        if (!entry_ec) entry.modified = file_time_to_unix(modified);
        entry.enabled = !has_disabled_suffix(entry.name);
        result.entries.push_back(std::move(entry));
    }
    if (ec) {
        return Error::make(63, "读取资源目录失败",
                           path_to_utf8(dir.value()) + ": " + ec.message());
    }

    std::sort(result.entries.begin(), result.entries.end(),
              [](const ResourceEntry& a, const ResourceEntry& b) {
                  if (a.directory != b.directory) return a.directory;
                  std::string left = a.name;
                  std::string right = b.name;
                  std::transform(left.begin(), left.end(), left.begin(),
                                 [](unsigned char c) {
                                     return static_cast<char>(std::tolower(c));
                                 });
                  std::transform(right.begin(), right.end(), right.begin(),
                                 [](unsigned char c) {
                                     return static_cast<char>(std::tolower(c));
                                 });
                  return left < right;
              });
    return result;
}

Result<void> delete_instance_resource(const std::filesystem::path& mc_dir,
                                      const std::string& id,
                                      const std::string& kind,
                                      const std::string& name) {
    if (!safe_resource_name(name)) {
        return Error::make(64, "资源文件名不合法", name);
    }

    auto dir = instance_resource_dir(mc_dir, id, kind);
    if (!dir) return dir.error();

    const std::filesystem::path target = dir.value() / path_from_utf8(name);
    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        return Error::make(65, "资源不存在", path_to_utf8(target));
    }
    // 目录联接可能指向共享目录，直接删会把目标一起带走。
    const DWORD attributes = ::GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Error::make(73, "不能删除目录联接", path_to_utf8(target));
    }

    auto moved = move_path_to_recycle_bin(target, "资源");
    if (!moved) return moved.error();
    Log::info("资源已移入回收站：" + name + " (" + path_to_utf8(dir.value()) + ")");
    return {};
}

Result<std::string> toggle_instance_resource(const std::filesystem::path& mc_dir,
                                             const std::string& id,
                                             const std::string& kind,
                                             const std::string& name) {
    if (!safe_resource_name(name)) {
        return Error::make(66, "资源文件名不合法", name);
    }

    auto dir = instance_resource_dir(mc_dir, id, kind);
    if (!dir) return dir.error();

    const std::filesystem::path source = dir.value() / path_from_utf8(name);
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        return Error::make(67, "资源不存在", path_to_utf8(source));
    }

    constexpr std::string_view suffix = ".disabled";
    std::string next;
    if (has_disabled_suffix(name)) {
        next = name.substr(0, name.size() - suffix.size());
    } else {
        next = name + std::string(suffix);
    }
    if (!safe_resource_name(next)) {
        return Error::make(68, "新的资源文件名不合法", next);
    }

    const std::filesystem::path target = dir.value() / path_from_utf8(next);
    if (std::filesystem::exists(target, ec)) {
        return Error::make(69, "目标文件已存在", path_to_utf8(target));
    }

    std::filesystem::rename(source, target, ec);
    if (ec) {
        return Error::make(70, "重命名资源失败",
                           path_to_utf8(source) + " -> " + path_to_utf8(target) +
                               ": " + ec.message());
    }
    Log::info("资源状态已切换：" + name + " -> " + next);
    return next;
}

Result<void> open_instance_resources(const std::filesystem::path& mc_dir,
                                     const std::string& id,
                                     const std::string& kind) {
    auto dir = instance_resource_dir(mc_dir, id, kind);
    if (!dir) return dir.error();

    std::error_code ec;
    std::filesystem::create_directories(dir.value(), ec);
    if (ec) {
        return Error::make(71, "无法创建资源目录",
                           path_to_utf8(dir.value()) + ": " + ec.message());
    }

    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", dir.value().c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return Error::make(72, "无法打开资源文件夹", path_to_utf8(dir.value()));
    }
    return {};
}

} // namespace cl::mc
