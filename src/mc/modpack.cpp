#include "mc/modpack.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <zlib.h>

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

// Java 8 在命令行里会把部分 Unicode（例如 ☆）按系统代码页转坏，
// 导致 classpath 里的 jar 完全读不到。导入时统一使用 ASCII 实例名，
// 原始名称仍保存在 cmcl-modpack.json 的 name 字段里。
std::string ascii_instance_name(const std::string& raw,
                                const std::string& fallback) {
    for (const unsigned char c : raw) {
        if (c >= 0x80) return sanitize_instance_name(fallback);
    }
    return sanitize_instance_name(raw);
}

std::string parse_game_version(const std::string& text) {
    static const std::regex pattern(R"((\d+\.\d+(?:\.\d+)?))");
    std::smatch match;
    if (std::regex_search(text, match, pattern)) return match[1].str();
    return {};
}

struct ImportCompatInfo {
    bool legacy_forge = false;
    std::string game_version;
};

// 识别 1.6.4 - 1.12.2 的 Forge（LaunchWrapper）。
// 这些版本的 FML 在打二进制补丁时还需要服务端独有的类。
ImportCompatInfo inspect_compat(const nlohmann::json& version_json,
                                const std::string& game_version_hint) {
    ImportCompatInfo info;
    const std::string main_class = json_string(version_json, "mainClass");
    const std::string main_lower = lower_copy(main_class);
    bool launchwrapper = main_lower.find("launchwrapper") != std::string::npos;
    bool forge = false;
    std::string forge_version;

    if (const auto it = version_json.find("libraries");
        it != version_json.end() && it->is_array()) {
        for (const auto& library : *it) {
            if (!library.is_object()) continue;
            const std::string name = json_string(library, "name");
            const std::string lower = lower_copy(name);
            if (lower.rfind("net.minecraftforge:minecraftforge:", 0) == 0 ||
                lower.rfind("net.minecraftforge:forge:", 0) == 0) {
                forge = true;
                const size_t colon = name.rfind(':');
                if (colon != std::string::npos) {
                    forge_version = name.substr(colon + 1);
                }
            }
            if (lower.rfind("net.minecraft:launchwrapper:", 0) == 0) {
                launchwrapper = true;
            }
        }
    }
    if (main_lower.find("bootstraplauncher") != std::string::npos) {
        forge = false;
    }
    if (!forge || !launchwrapper) return info;

    info.legacy_forge = true;
    info.game_version = parse_game_version(game_version_hint);
    if (info.game_version.empty()) {
        info.game_version = parse_game_version(json_string(version_json, "jar"));
    }
    if (info.game_version.empty()) {
        // 只对版本线明确的 Forge 做兜底；不确定时宁可不处理。
        if (forge_version.rfind("9.", 0) == 0) {
            info.game_version = "1.6.4";
        } else if (forge_version.rfind("10.", 0) == 0) {
            info.game_version = "1.7.10";
        } else if (forge_version.rfind("14.", 0) == 0) {
            info.game_version = "1.12.2";
        }
    }
    return info;
}

void add_library_entry(nlohmann::json& version_json, const std::string& name) {
    if (!version_json.contains("libraries") ||
        !version_json["libraries"].is_array()) {
        version_json["libraries"] = nlohmann::json::array();
    }
    for (const auto& library : version_json["libraries"]) {
        if (library.is_object() &&
            json_string(library, "name") == name) {
            return;
        }
    }
    version_json["libraries"].insert(
        version_json["libraries"].begin(),
        nlohmann::json::object({{"name", name}}));
}

bool has_bspkrs_mod(const std::filesystem::path& game_dir) {
    std::error_code ec;
    const std::filesystem::path mods = game_dir / L"mods";
    for (std::filesystem::directory_iterator it(mods, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = lower_copy(path_to_utf8(it->path().filename()));
        if (name.find("bspkrs") != std::string::npos) return true;
    }
    return false;
}

// bspkrsCore 的更新检查地址已经失效，会在构造 ModVersionChecker 时
// 对空数组取下标而崩溃（ArmorStatusHUD 等依赖它的模组也会一起崩）。
void fix_bspkrs_config(const std::filesystem::path& game_dir,
                       bool create_if_missing) {
    std::error_code ec;
    const std::filesystem::path config =
        game_dir / L"config" / L"bspkrsCore.cfg";
    if (std::filesystem::is_regular_file(config, ec)) {
        std::ifstream input(config, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), {});
        input.close();
        if (text.find("B:allowUpdateCheck=true") != std::string::npos) {
            size_t pos = 0;
            while ((pos = text.find("B:allowUpdateCheck=true", pos)) !=
                   std::string::npos) {
                text.replace(pos, std::string("B:allowUpdateCheck=true").size(),
                             "B:allowUpdateCheck=false");
                pos += std::string("B:allowUpdateCheck=false").size();
            }
        } else if (text.find("B:allowUpdateCheck") == std::string::npos) {
            const size_t general = text.find("general {");
            if (general != std::string::npos) {
                const size_t line = text.find('\n', general);
                if (line != std::string::npos) {
                    text.insert(line + 1,
                                "    B:allowUpdateCheck=false\n");
                }
            } else {
                text += "\n\ngeneral {\n    B:allowUpdateCheck=false\n}\n";
            }
        }
        std::ofstream output(config, std::ios::binary | std::ios::trunc);
        if (output) output << text;
        return;
    }
    if (!create_if_missing) return;

    std::filesystem::create_directories(config.parent_path(), ec);
    std::ofstream output(config, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "# Configuration file\n\n"
              "general {\n"
              "    B:allowDebugOutput=false\n"
              "    B:allowUpdateCheck=false\n"
              "    I:updateTimeoutMilliseconds=3000\n"
              "}\n";
}

// 写一个只存储、不压缩的 zip。Java 可以正常读取，避免为了一个附加 jar
// 引入新的压缩库；服务端独有类总量不大，体积可以接受。
void zip_put16(std::ofstream& out, std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
    };
    out.write(bytes, 2);
}

void zip_put32(std::ofstream& out, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
    };
    out.write(bytes, 4);
}

Result<void> write_zip_store(
    const std::filesystem::path& destination,
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& files) {
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Error::make(250, "无法创建兼容 jar",
                           path_to_utf8(destination));
    }

    struct Entry {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t size = 0;
        std::uint32_t offset = 0;
    };
    std::vector<Entry> entries;

    for (const auto& relative : files) {
        std::ifstream input(root / relative, std::ios::binary);
        if (!input) continue;
        std::string data((std::istreambuf_iterator<char>(input)), {});
        input.close();

        Entry entry;
        entry.name = relative.generic_string();
        entry.size = static_cast<std::uint32_t>(data.size());
        entry.crc = static_cast<std::uint32_t>(::crc32(
            0L, reinterpret_cast<const Bytef*>(data.data()),
            static_cast<uInt>(data.size())));
        if (entry.name.empty()) continue;
        entry.offset = static_cast<std::uint32_t>(out.tellp());

        zip_put32(out, 0x04034b50);
        zip_put16(out, 20);      // version needed
        zip_put16(out, 0x0800);  // UTF-8 names
        zip_put16(out, 0);       // stored
        zip_put16(out, 0);       // time
        zip_put16(out, 0x0021);  // 1980-01-01
        zip_put32(out, entry.crc);
        zip_put32(out, entry.size);
        zip_put32(out, entry.size);
        zip_put16(out, static_cast<std::uint16_t>(entry.name.size()));
        zip_put16(out, 0);
        out.write(entry.name.data(),
                  static_cast<std::streamsize>(entry.name.size()));
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        entries.push_back(std::move(entry));
    }

    if (entries.empty()) {
        return Error::make(251, "没有需要写入兼容 jar 的类");
    }

    const std::uint32_t central_offset =
        static_cast<std::uint32_t>(out.tellp());
    for (const auto& entry : entries) {
        zip_put32(out, 0x02014b50);
        zip_put16(out, 20);      // version made by
        zip_put16(out, 20);      // version needed
        zip_put16(out, 0x0800);  // UTF-8 names
        zip_put16(out, 0);       // stored
        zip_put16(out, 0);
        zip_put16(out, 0x0021);
        zip_put32(out, entry.crc);
        zip_put32(out, entry.size);
        zip_put32(out, entry.size);
        zip_put16(out, static_cast<std::uint16_t>(entry.name.size()));
        zip_put16(out, 0);
        zip_put16(out, 0);
        zip_put16(out, 0);
        zip_put16(out, 0);
        zip_put32(out, 0);
        zip_put32(out, entry.offset);
        out.write(entry.name.data(),
                  static_cast<std::streamsize>(entry.name.size()));
    }
    const std::uint32_t central_size =
        static_cast<std::uint32_t>(out.tellp()) - central_offset;

    zip_put32(out, 0x06054b50);
    zip_put16(out, 0);
    zip_put16(out, 0);
    zip_put16(out, static_cast<std::uint16_t>(entries.size()));
    zip_put16(out, static_cast<std::uint16_t>(entries.size()));
    zip_put32(out, central_size);
    zip_put32(out, central_offset);
    zip_put16(out, 0);

    if (!out) {
        return Error::make(252, "写入兼容 jar 失败",
                           path_to_utf8(destination));
    }
    return {};
}

std::filesystem::path temp_compat_dir(const char* tag) {
    static unsigned counter = 0;
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        (std::string("cmcl-") + tag + "-" + std::to_string(stamp) + "-" +
         std::to_string(++counter));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path;
}

Result<std::filesystem::path> ensure_server_jar(
    const std::filesystem::path& mc_dir, const std::string& game_version,
    DownloadCancelToken& cancel) {
    const std::filesystem::path destination =
        mc_dir / L"libraries" / L"net" / L"minecraft" /
        L"minecraft_server" / path_from_utf8(game_version) /
        path_from_utf8("minecraft_server-" + game_version + ".jar");
    if (cancel.load(std::memory_order_relaxed)) {
        return Error::make(499, "导入已取消");
    }

    nlohmann::json server;
    std::error_code ec;
    const std::filesystem::path local_json =
        mc_dir / L"versions" / path_from_utf8(game_version) /
        (path_from_utf8(game_version).wstring() + L".json");
    if (std::filesystem::is_regular_file(local_json, ec)) {
        std::ifstream input(local_json, std::ios::binary);
        try {
            nlohmann::json json;
            input >> json;
            if (const auto downloads = json.find("downloads");
                downloads != json.end()) {
                if (const auto item = downloads->find("server");
                    item != downloads->end()) {
                    server = *item;
                }
            }
        } catch (const std::exception&) {
        }
    }

    if (!server.is_object()) {
        HttpOptions options;
        options.timeout_seconds = 30;
        options.headers["User-Agent"] = kUserAgent;
        auto manifest_text = http_get(
            "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
            options);
        if (!manifest_text) return manifest_text.error();

        std::string version_url;
        try {
            const nlohmann::json manifest =
                nlohmann::json::parse(manifest_text.value());
            for (const auto& version : manifest.at("versions")) {
                if (json_string(version, "id") == game_version) {
                    version_url = json_string(version, "url");
                    break;
                }
            }
        } catch (const std::exception& ex) {
            return Error::make(253, "解析版本清单失败", ex.what());
        }
        if (version_url.empty()) {
            return Error::make(254, "找不到对应的原版版本", game_version);
        }

        auto version_text = http_get(version_url, options);
        if (!version_text) return version_text.error();
        try {
            const nlohmann::json json =
                nlohmann::json::parse(version_text.value());
            const auto downloads = json.find("downloads");
            if (downloads == json.end()) {
                return Error::make(255, "版本 JSON 没有 downloads", game_version);
            }
            const auto item = downloads->find("server");
            if (item == downloads->end()) {
                return Error::make(256, "该版本没有官方服务端 jar", game_version);
            }
            server = *item;
        } catch (const std::exception& ex) {
            return Error::make(257, "解析版本 JSON 失败", ex.what());
        }
    }

    const std::string url = json_string(server, "url");
    const std::string sha1 = lower_copy(json_string(server, "sha1"));
    unsigned long long size = 0;
    if (server.contains("size")) {
        if (server["size"].is_number_unsigned()) {
            size = server["size"].get<unsigned long long>();
        } else if (server["size"].is_number_integer()) {
            const long long value = server["size"].get<long long>();
            if (value > 0) size = static_cast<unsigned long long>(value);
        }
    }
    if (url.empty() || sha1.empty()) {
        return Error::make(258, "官方服务端信息不完整", game_version);
    }

    if (std::filesystem::is_regular_file(destination, ec)) {
        auto hash = sha1_file(destination);
        if (hash && lower_copy(hash.value()) == sha1) return destination;
        std::filesystem::remove(destination, ec);
    }

    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        return Error::make(259, "无法创建服务端缓存目录",
                           path_to_utf8(destination.parent_path()));
    }

    HttpOptions options;
    options.expected_size = size;
    options.timeout_seconds = 120;
    options.headers["User-Agent"] = kUserAgent;

    // 不用 http_download 的 .part + 改名：部分杀毒/索引服务会在文件刚写完时
    // 短暂占用它，导致 MoveFileEx 失败。服务端 jar 只有几 MB，直接读入内存
    // 后写到目标路径，避免改名窗口。
    auto body = http_get(url, options);
    if (!body) return body.error();

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Error::make(260, "无法写入服务端 jar",
                           path_to_utf8(destination));
    }
    output.write(body.value().data(),
                 static_cast<std::streamsize>(body.value().size()));
    output.close();
    if (!output) {
        std::filesystem::remove(destination, ec);
        return Error::make(260, "写入服务端 jar 失败",
                           path_to_utf8(destination));
    }

    auto hash = sha1_file(destination);
    if (!hash || lower_copy(hash.value()) != sha1) {
        std::filesystem::remove(destination, ec);
        return Error::make(260, "服务端 jar 校验失败", game_version);
    }
    return destination;
}

Result<std::filesystem::path> create_server_extra_jar(
    const std::filesystem::path& mc_dir, const std::string& game_version,
    const std::filesystem::path& pack_jar,
    const std::filesystem::path& server_jar) {
    const std::filesystem::path pack_root = temp_compat_dir("pack");
    const std::filesystem::path server_root = temp_compat_dir("server");
    const std::filesystem::path stage_root = temp_compat_dir("stage");
    auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(pack_root, ignored);
        std::filesystem::remove_all(server_root, ignored);
        std::filesystem::remove_all(stage_root, ignored);
    };

    auto pack_extract = extract_zip(pack_jar, pack_root);
    if (!pack_extract) {
        cleanup();
        return pack_extract.error();
    }
    auto server_extract = extract_zip(server_jar, server_root);
    if (!server_extract) {
        cleanup();
        return server_extract.error();
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(server_root, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path relative =
            std::filesystem::relative(it->path(), server_root, ec);
        if (ec || relative.empty()) continue;
        if (std::filesystem::exists(pack_root / relative, ec)) continue;
        const std::filesystem::path target = stage_root / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) continue;
        std::filesystem::copy_file(
            it->path(), target,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) continue;
        files.push_back(relative);
    }
    if (files.empty()) {
        cleanup();
        return Error::make(261, "没有可补充的服务端类", game_version);
    }

    const std::filesystem::path destination =
        mc_dir / L"libraries" / L"net" / L"minecraft" /
        L"minecraft_server_extra" / path_from_utf8(game_version) /
        path_from_utf8("minecraft_server_extra-" + game_version + ".jar");
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        cleanup();
        return Error::make(262, "无法创建兼容 jar 目录",
                           path_to_utf8(destination.parent_path()));
    }
    auto written = write_zip_store(destination, stage_root, files);
    cleanup();
    if (!written) return written.error();
    return destination;
}

Result<void> apply_import_compat(
    const std::filesystem::path& mc_dir,
    const std::filesystem::path& version_dir, const std::string& instance,
    nlohmann::json& version_json, const std::string& game_version_hint,
    const DownloadProgressCallback& progress, DownloadCancelToken& cancel) {
    const ImportCompatInfo info =
        inspect_compat(version_json, game_version_hint);
    const std::filesystem::path game_dir = version_dir / L"game";

    fix_bspkrs_config(game_dir,
                      info.legacy_forge || has_bspkrs_mod(game_dir));

    if (!info.legacy_forge || info.game_version.empty()) return {};
    if (cancel.load(std::memory_order_relaxed)) {
        return Error::make(499, "导入已取消");
    }

    report(progress, "兼容修复",
           "正在准备旧版 Forge 的服务端依赖…", info.game_version);
    auto server = ensure_server_jar(mc_dir, info.game_version, cancel);
    if (!server) return server.error();

    const std::filesystem::path pack_jar =
        version_dir / (path_from_utf8(instance).wstring() + L".jar");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pack_jar, ec)) {
        return Error::make(263, "找不到实例主 jar", path_to_utf8(pack_jar));
    }

    auto extra = create_server_extra_jar(mc_dir, info.game_version,
                                         pack_jar, server.value());
    if (!extra) return extra.error();

    // 完整服务端 jar 会覆盖客户端类（CrashReport 等），只能使用
    // 只含服务端独有类的附加 jar。移除指向完整服务端的库条目，
    // 但保留 libraries 目录里的服务端 jar 文件本身。
    if (version_json.contains("libraries") &&
        version_json["libraries"].is_array()) {
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& library : version_json["libraries"]) {
            const std::string name =
                library.is_object() ? json_string(library, "name") : "";
            if (name.rfind("net.minecraft:minecraft_server:", 0) == 0) {
                continue;
            }
            filtered.push_back(library);
        }
        version_json["libraries"] = std::move(filtered);
    }

    add_library_entry(version_json,
                      "net.minecraft:minecraft_server_extra:" +
                          info.game_version);
    const std::filesystem::path json_path =
        version_dir / (path_from_utf8(instance).wstring() + L".json");
    std::ofstream output(json_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Error::make(264, "无法写入兼容处理后的版本 JSON",
                           path_to_utf8(json_path));
    }
    output << version_json.dump(2);
    Log::info("旧版 Forge 兼容处理完成：" + info.game_version +
              "，服务端附加类 -> " + path_to_utf8(extra.value()));
    return {};
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
    const ImportCompatInfo compat_info =
        inspect_compat(version_json, original_id);
    const std::string parsed_hint = parse_game_version(original_id);
    const std::string version_hint =
        parsed_hint.empty() ? compat_info.game_version : parsed_hint;
    const std::string fallback =
        version_hint.empty() ? "Modpack" : "Modpack-" + version_hint;
    const std::string instance = unique_instance_name(
        request.mc_dir, ascii_instance_name(display_name, fallback));

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

    // 自动修复旧 Forge / bspkrs / Java 8 路径问题。失败只记录，
    // 不让一个可选修复阻断整个整合包导入。
    auto compat = apply_import_compat(request.mc_dir, version_dir, instance,
                                      version_json, original_id, progress,
                                      cancel);
    if (!compat) {
        if (compat.error().code == 499) return compat.error();
        Log::warn("整合包自动兼容处理失败：" +
                  compat.error().to_string());
    }

    ModpackImportResult result;
    result.instance_id = instance;
    result.name = display_name;
    result.pack_version = original_id;
    result.game_version = version_hint.empty() ? original_id : version_hint;
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
    const std::string instance = unique_instance_name(
        request.mc_dir,
        ascii_instance_name(requested_name, "Modpack-" + game_version));

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

    {
        const std::filesystem::path version_json_path =
            version_dir /
            (path_from_utf8(instance).wstring() + L".json");
        std::ifstream version_input(version_json_path, std::ios::binary);
        if (version_input) {
            nlohmann::json version_json;
            try {
                version_input >> version_json;
            } catch (const std::exception&) {
                version_json = nlohmann::json();
            }
            if (version_json.is_object()) {
                auto compat = apply_import_compat(
                    request.mc_dir, version_dir, instance, version_json,
                    game_version, progress, cancel);
                if (!compat) {
                    if (compat.error().code == 499) return compat.error();
                    Log::warn("整合包自动兼容处理失败：" +
                              compat.error().to_string());
                }
            }
        }
    }

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
