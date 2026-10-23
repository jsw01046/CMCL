#include "mc/launch.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "core/archive.h"
#include "core/crypto.h"
#include "core/encoding.h"
#include "core/log.h"
#include "core/process.h"
#include "mc/instance.h"
#include "mc/java.h"

namespace cl::mc {

namespace {

bool should_skip_shared_entry(const std::wstring& name) {
    static const wchar_t* kSkip[] = {
        L"saves", L"versions", L"libraries", L"assets",
        L"logs", L"crash-reports", L"natives",
    };
    std::wstring lower = name;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(::towlower(c));
    for (const wchar_t* item : kSkip) {
        if (lower == item) return true;
    }
    return false;
}

// WIN32_LEAN_AND_MEAN 下部分 SDK 不暴露 REPARSE_DATA_BUFFER，按官方布局声明。
struct JunctionReparseData {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1];
};

bool create_directory_junction(const std::filesystem::path& link,
                               const std::filesystem::path& target) {
    // 直接写 NTFS 目录联接，避免经过 cmd.exe，路径里的 & / % 不会被解释。
    if (!::CreateDirectoryW(link.c_str(), nullptr)) {
        Log::warn("无法创建目录联接：" + path_to_utf8(link));
        return false;
    }

    HANDLE handle = ::CreateFileW(
        link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        ::RemoveDirectoryW(link.c_str());
        Log::warn("无法打开目录联接：" + path_to_utf8(link));
        return false;
    }

    const std::wstring substitute = L"\\??\\" + target.wstring();
    const std::wstring printable = target.wstring();
    const DWORD substitute_bytes =
        static_cast<DWORD>((substitute.size() + 1) * sizeof(wchar_t));
    const DWORD printable_bytes =
        static_cast<DWORD>((printable.size() + 1) * sizeof(wchar_t));
    const size_t data_size =
        FIELD_OFFSET(JunctionReparseData, PathBuffer) + substitute_bytes +
        printable_bytes;

    std::vector<BYTE> buffer(data_size, 0);
    auto* reparse = reinterpret_cast<JunctionReparseData*>(buffer.data());
    reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->ReparseDataLength = static_cast<USHORT>(
        8 + substitute_bytes + printable_bytes);
    reparse->SubstituteNameOffset = 0;
    reparse->SubstituteNameLength =
        static_cast<USHORT>(substitute_bytes - sizeof(wchar_t));
    reparse->PrintNameOffset = static_cast<USHORT>(substitute_bytes);
    reparse->PrintNameLength =
        static_cast<USHORT>(printable_bytes - sizeof(wchar_t));

    std::memcpy(reparse->PathBuffer, substitute.c_str(), substitute_bytes);
    std::memcpy(reinterpret_cast<BYTE*>(reparse->PathBuffer) +
                    substitute_bytes,
                printable.c_str(), printable_bytes);

    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    ::CloseHandle(handle);
    if (!ok) {
        ::RemoveDirectoryW(link.c_str());
        Log::warn("无法设置目录联接：" + path_to_utf8(link) + " -> " +
                  path_to_utf8(target));
        return false;
    }
    return true;
}

// 存档单独隔离时，把游戏目录做成一个轻量覆盖层：
// saves 使用实例自己的真实目录，其它 mods/config/资源包等通过联接继续共享。
void link_shared_entries(const std::filesystem::path& mc_dir,
                         const std::filesystem::path& overlay) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(mc_dir, ec);
         !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        const std::filesystem::path source = it->path();
        const std::wstring name = source.filename().wstring();
        if (should_skip_shared_entry(name)) continue;

        const std::filesystem::path target = overlay / name;
        if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            continue;
        }

        if (it->is_directory(ec)) {
            const auto absolute = std::filesystem::absolute(source, ec);
            const auto destination = std::filesystem::absolute(target, ec);
            if (!ec) {
                create_directory_junction(destination, absolute);
            }
        } else if (it->is_regular_file(ec)) {
            if (!::CreateHardLinkW(target.c_str(), source.c_str(), nullptr)) {
                if (!::CopyFileW(source.c_str(), target.c_str(), TRUE)) {
                    Log::warn("无法共享文件：" + path_to_utf8(source));
                }
            }
        }
    }
}

Result<void> prepare_saves_overlay(const std::filesystem::path& mc_dir,
                                   const std::filesystem::path& overlay) {
    std::error_code ec;
    std::filesystem::create_directories(overlay / L"saves", ec);
    if (ec) {
        return Error::make(5, "无法创建实例存档目录",
                           path_to_utf8(overlay / L"saves") + ": " +
                               ec.message());
    }
    link_shared_entries(mc_dir, overlay);
    return {};
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

// 覆盖式设置一个带值的参数：删掉已有的同名参数，再追加到末尾。
void set_option(std::vector<std::string>& args, const std::string& flag,
                const std::string& value) {
    for (auto it = args.begin(); it != args.end();) {
        if (*it == flag) {
            it = args.erase(it);
            if (it != args.end() && !it->empty() && it->front() != '-') {
                it = args.erase(it);
            }
        } else {
            ++it;
        }
    }
    args.push_back(flag);
    args.push_back(value);
}

void set_switch(std::vector<std::string>& args, const std::string& flag,
                bool enabled) {
    args.erase(std::remove(args.begin(), args.end(), flag), args.end());
    if (enabled) args.push_back(flag);
}

void dedup_exact(std::vector<std::string>& args) {
    std::set<std::string> seen;
    std::vector<std::string> result;
    result.reserve(args.size());
    for (auto& arg : args) {
        if (seen.insert(arg).second) result.push_back(std::move(arg));
    }
    args = std::move(result);
}

void append_all(std::vector<std::string>& target,
                const std::vector<std::string>& source) {
    target.insert(target.end(), source.begin(), source.end());
}

// Java 8 在非 GBK 代码页（例如系统开启 UTF-8 后的 65001）下无法解析
// 命令行里的非 ASCII 路径，表现为“找不到主类”。把进程工作目录设成
// .minecraft，并把 classpath / natives 都换成相对路径，可以完全绕开。
std::string relative_utf8(const std::filesystem::path& target,
                          const std::filesystem::path& base) {
    const std::filesystem::path relative = target.lexically_relative(base);
    if (relative.empty()) return path_to_utf8(target);
    return path_to_utf8(relative);
}

bool is_ascii(const std::wstring& text) {
    for (const wchar_t c : text) {
        if (c > 0x7F) return false;
    }
    return true;
}

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Java 8 的 native 加载器在 ACP=65001 时同样会把非 ASCII 路径转坏。
// 把 natives 释放到纯 ASCII 的 %LOCALAPPDATA%\CraftLauncher\natives\<hash>。
std::filesystem::path ascii_natives_dir(const std::filesystem::path& mc_dir,
                                        const std::string& instance_id) {
    wchar_t buffer[32768];
    std::filesystem::path base;
    for (const wchar_t* name : {L"LOCALAPPDATA", L"ProgramData"}) {
        const DWORD size = ::GetEnvironmentVariableW(name, buffer, 32768);
        if (size > 0 && size < 32768) {
            std::filesystem::path candidate(buffer);
            if (is_ascii(candidate.wstring())) {
                base = std::move(candidate);
                break;
            }
        }
    }
    if (base.empty()) {
        base = std::filesystem::temp_directory_path();
    }

    const std::uint64_t hash =
        fnv1a(path_to_utf8(mc_dir) + "|" + instance_id);
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hash));
    return base / L"CraftLauncher" / L"natives" / path_from_utf8(hex);
}

std::map<std::string, std::string> make_placeholders(
    const LaunchOptions& options, const ResolvedVersion& version,
    const std::filesystem::path& game_dir,
    const std::filesystem::path& natives_dir, const std::string& classpath,
    const std::string& uuid, bool absolute_paths) {
    const std::filesystem::path assets_root = options.mc_dir / L"assets";
    const std::string assets_index = version.merged.asset_index_id.value_or("legacy");
    const std::string version_type =
        version.merged.type.empty() ? "release" : version.merged.type;

    const auto resolve_path = [&](const std::filesystem::path& path) {
        return absolute_paths ? path_to_utf8(path) : relative_utf8(path, game_dir);
    };

    std::map<std::string, std::string> placeholders;
    placeholders["${auth_player_name}"] = options.player_name;
    placeholders["${version_name}"] = options.version_id;
    placeholders["${game_directory}"] = resolve_path(game_dir);
    placeholders["${assets_root}"] = resolve_path(assets_root);
    placeholders["${assets_index_name}"] = assets_index;
    placeholders["${auth_uuid}"] = uuid;
    placeholders["${auth_access_token}"] = options.access_token;
    placeholders["${auth_session}"] =
        "token:" + options.access_token + ":" + uuid;
    placeholders["${clientid}"] = "";
    placeholders["${auth_xuid}"] = "";
    placeholders["${user_type}"] = options.user_type;
    placeholders["${version_type}"] = version_type;
    // natives 目录本身已经是纯 ASCII，使用绝对路径。
    placeholders["${natives_directory}"] = path_to_utf8(natives_dir);
    placeholders["${launcher_name}"] = "CMCL";
    placeholders["${launcher_version}"] = "0.5.1";
    placeholders["${classpath}"] = classpath;
    placeholders["${classpath_separator}"] = ";";
    placeholders["${library_directory}"] =
        resolve_path(options.mc_dir / L"libraries");
    placeholders["${resolution_width}"] = std::to_string(options.width);
    placeholders["${resolution_height}"] = std::to_string(options.height);
    placeholders["${game_assets}"] =
        resolve_path(assets_root / L"virtual" / L"legacy");
    return placeholders;
}

void parse_server(const std::string& server, std::string& host, std::string& port) {
    host = server;
    port = "25565";
    if (host.empty()) return;

    if (host.front() == '[') {
        const size_t close = host.find(']');
        if (close != std::string::npos) {
            if (close + 1 < host.size() && host[close + 1] == ':') {
                port = host.substr(close + 2);
            }
            host = host.substr(1, close - 1);
        }
        return;
    }

    const size_t first = host.find(':');
    if (first != std::string::npos && host.find(':', first + 1) == std::string::npos) {
        port = host.substr(first + 1);
        host = host.substr(0, first);
    }
}

// 1.5.2 及更早版本从 jar 的 /lang/<代码>.lang 读取语言文件，文件名区分大小写。
// 新版把 options.txt 写成 zh_cn，旧版会找不到 zh_cn.lang 并在启动时 NPE。
std::string normalize_legacy_language(std::string code) {
    const size_t underscore = code.find('_');
    if (underscore == std::string::npos || underscore == 0 ||
        underscore + 1 >= code.size()) {
        return code;
    }
    for (size_t i = 0; i < underscore; ++i) {
        code[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(code[i])));
    }
    for (size_t i = underscore + 1; i < code.size(); ++i) {
        code[i] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(code[i])));
    }
    return code;
}

void fix_legacy_options_language(const std::filesystem::path& game_dir) {
    const std::filesystem::path options = game_dir / L"options.txt";
    std::ifstream input(options, std::ios::binary);
    if (!input) return;

    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    bool changed = false;
    size_t pos = 0;
    while ((pos = text.find("lang:", pos)) != std::string::npos) {
        if (pos != 0 && text[pos - 1] != '\n' && text[pos - 1] != '\r') {
            pos += 5;
            continue;
        }
        const size_t start = pos + 5;
        const size_t end = text.find_first_of("\r\n", start);
        const size_t length =
            end == std::string::npos ? text.size() - start : end - start;
        const std::string old = text.substr(start, length);
        const std::string normalized = normalize_legacy_language(old);
        if (!normalized.empty() && normalized != old) {
            text.replace(start, length, normalized);
            changed = true;
            Log::info("旧版语言代码已修正：" + old + " -> " + normalized);
        }
        pos = start + (normalized.empty() ? length : normalized.size());
    }

    if (!changed) return;
    std::ofstream output(options, std::ios::binary | std::ios::trunc);
    if (!output) {
        Log::warn("无法写入 options.txt，旧版语言代码未修正");
        return;
    }
    output << text;
}

} // namespace

Result<PreparedLaunch> prepare_launch(const LaunchOptions& options,
                                      bool extract_natives) {
    if (options.version_id.empty()) {
        return Error::make(1, "没有指定版本");
    }
    std::error_code ec;
    auto ensured = ensure_game_dir(options.mc_dir);
    if (!ensured) return ensured.error();

    auto resolved_result = resolve_version(options.mc_dir / L"versions",
                                           options.version_id);
    if (!resolved_result) return resolved_result.error();
    ResolvedVersion version = std::move(resolved_result).value();

    Log::info("解析版本：" + version.id + "（原版 " + version.vanilla_id + "，主 jar " +
              version.jar_id + "）");

    if (!version.merged.main_class || version.merged.main_class->empty()) {
        return Error::make(3, "版本 JSON 中没有 mainClass", version.id);
    }

    std::string uuid = options.uuid;
    if (uuid.empty()) {
        auto generated = offline_uuid(options.player_name);
        if (!generated) return generated.error();
        uuid = std::move(generated).value();
    }

    std::filesystem::path game_dir = options.mc_dir;
    const std::filesystem::path version_dir =
        options.mc_dir / L"versions" / path_from_utf8(version.id);
    if (options.isolate) {
        // 完整隔离也使用 game 子目录，保证切换“存档单独隔离”时存档路径不变。
        game_dir = version_dir / L"game";
        std::error_code dir_ec;
        std::filesystem::create_directories(game_dir / L"saves", dir_ec);
        if (dir_ec) {
            return Error::make(13, "无法创建隔离目录",
                               path_to_utf8(game_dir) + ": " + dir_ec.message());
        }
    } else if (options.isolate_saves) {
        // 只隔离存档：saves 放在实例自己的 game 目录，其它数据用联接共享。
        game_dir = version_dir / L"game";
        auto overlay = prepare_saves_overlay(options.mc_dir, game_dir);
        if (!overlay) return overlay.error();
    } else {
        std::error_code dir_ec;
        std::filesystem::create_directories(game_dir / L"saves", dir_ec);
        if (dir_ec) {
            return Error::make(14, "无法创建存档目录",
                               path_to_utf8(game_dir / L"saves") + ": " +
                                   dir_ec.message());
        }
    }
    // 1.5.2 等旧版的语言资源文件名区分大小写。
    if (version.merged.assets.value_or("") == "pre-1.6") {
        fix_legacy_options_language(game_dir);
    }

    const std::filesystem::path natives_dir =
        ascii_natives_dir(options.mc_dir, version.id);

    FeatureSet features;
    features.demo_user = options.demo;
    features.custom_resolution = true;

    auto java_result = select_java(version, options.java_exe, options.mc_dir);
    if (!java_result) return java_result.error();
    const JavaInfo java = std::move(java_result).value();

    // Java 9+ 能正确处理 Unicode 绝对路径，而且现代 Forge/NeoForge 的
    // module path 检查要求 classpath 与 module path 指向同一个绝对路径。
    // Java 7/8 在 UTF-8 ACP 下必须用相对路径，见上面的说明。
    const bool absolute_paths = java.major >= 9;

    // ---- classpath ----
    std::vector<std::string> classpath_entries;
    std::vector<std::string> missing;
    for (const Library& library : version.merged.libraries) {
        if (!rules_allow(library.rules, features)) continue;

        const bool has_natives = library.natives_profile("windows").has_value();
        // 只有 natives 分类、没有主 artifact 的库不进入 classpath。
        if (has_natives && !library.artifact) continue;

        const std::string relative = library.artifact_path();
        if (relative.empty()) continue;

        const std::filesystem::path full =
            (options.mc_dir / L"libraries" / path_from_utf8(relative))
                .lexically_normal();
        if (!std::filesystem::is_regular_file(full, ec)) {
            missing.push_back(relative);
        }
        classpath_entries.push_back(absolute_paths ? path_to_utf8(full)
                                                   : relative_utf8(full, game_dir));
    }

    if (!missing.empty()) {
        std::string detail = "缺少 " + std::to_string(missing.size()) + " 个库文件";
        for (size_t i = 0; i < missing.size() && i < 5; ++i) {
            detail += "\n  " + missing[i];
        }
        if (missing.size() > 5) detail += "\n  ...";
        return Error::make(4, detail, "请先补全版本文件（下载功能将在 M3 实现）");
    }

    // 判断是否是 Forge / NeoForge 系实例。这些加载器通过 --fml.mcVersion
    // 自行定位原版 jar，classpath 里不能再放原版 jar，否则 BootstrapLauncher
    // 的 securejarhandler 会把它当成额外的 secure jar 模块加载，
    // 与 FML 注入的 minecraft 模块导出同一个包，触发 ResolutionException。
    const bool is_forge_like = [&] {
        for (const Library& library : version.merged.libraries) {
            if (library.name.rfind("net.minecraftforge:", 0) == 0 ||
                library.name.rfind("net.neoforged:", 0) == 0) {
                return true;
            }
        }
        if (version.merged.main_class) {
            const auto& mc = *version.merged.main_class;
            if (mc.find("bootstraplauncher") != std::string::npos ||
                mc.find("launchwrapper") != std::string::npos ||
                mc.find("cpw.mods.fml") != std::string::npos) {
                return true;
            }
        }
        return false;
    }();

    const std::filesystem::path client_jar =
        (options.mc_dir / L"versions" / path_from_utf8(version.jar_id) /
         (path_from_utf8(version.jar_id).wstring() + L".jar"))
            .lexically_normal();
    // 无论是否加入 classpath，原版 jar 都必须存在，否则加载器启动时会报错。
    if (!std::filesystem::is_regular_file(client_jar, ec)) {
        return Error::make(5, "缺少客户端 jar", path_to_utf8(client_jar));
    }
    // 只有 1.13+ 的 Forge/NeoForge（BootstrapLauncher / ModLauncher）才需要
    // 把原版 jar 从 classpath 排除。1.6.4 - 1.12.2 的 Forge 使用 LaunchWrapper，
    // 原版 jar 必须在 classpath 里，否则 FML 找不到客户端类，会按服务端处理，
    // 然后去补服务端专属类，触发 ClassPatchManager 的空指针。
    const bool legacy_launchwrapper =
        version.merged.main_class &&
        version.merged.main_class->find("launchwrapper") != std::string::npos;
    if (is_forge_like && !legacy_launchwrapper) {
        Log::info("Forge/NeoForge 实例，原版 jar 不加入 classpath：" +
                  path_to_utf8(client_jar));
    } else {
        classpath_entries.push_back(absolute_paths
                                        ? path_to_utf8(client_jar)
                                        : relative_utf8(client_jar, game_dir));
    }
    dedup_exact(classpath_entries);

    std::string classpath;
    for (size_t i = 0; i < classpath_entries.size(); ++i) {
        if (i != 0) classpath.push_back(';');
        classpath += classpath_entries[i];
    }
    Log::info("classpath 条目数：" + std::to_string(classpath_entries.size()));

    // ---- natives ----
    if (extract_natives) {
        std::filesystem::create_directories(natives_dir, ec);
        for (const Library& library : version.merged.libraries) {
            if (!rules_allow(library.rules, features)) continue;
            const auto natives = library.natives_artifact();
            if (!natives) continue;

            const std::filesystem::path jar =
                options.mc_dir / L"libraries" / path_from_utf8(natives->path);
            if (!std::filesystem::is_regular_file(jar, ec)) {
                return Error::make(6, "缺少原生库", path_to_utf8(jar));
            }

            auto extracted = extract_zip(jar, natives_dir, library.extract_exclude);
            if (!extracted) {
                return Error::make(7, "释放原生库失败",
                                   path_to_utf8(jar) + ": " +
                                       extracted.error().to_string());
            }
        }
    }

    const auto placeholders = make_placeholders(
        options, version, game_dir, natives_dir, classpath, uuid, absolute_paths);

    // ---- JVM 参数 ----
    std::vector<std::string> jvm;
    const int min_memory_mb =
        std::min(options.min_memory_mb, options.max_memory_mb);
    jvm.push_back("-Xms" + std::to_string(min_memory_mb) + "m");
    jvm.push_back("-Xmx" + std::to_string(options.max_memory_mb) + "m");
    jvm.push_back("-Djava.library.path=" + path_to_utf8(natives_dir));
    jvm.push_back("-Dminecraft.launcher.brand=CMCL");
    jvm.push_back("-Dminecraft.launcher.version=0.5.1");
    jvm.push_back("-Dlog4j2.formatMsgNoLookups=true");
    // Forge 1.6.4 / 1.7.10 的默认容错参数，PCL 也默认带上：
    // OptiFine 等 jar mod 会改写核心 jar，不开这两个参数 FML 会直接退出。
    jvm.push_back("-Dfml.ignoreInvalidMinecraftCertificates=true");
    jvm.push_back("-Dfml.ignorePatchDiscrepancies=true");

    // 统一 stdout/stderr 编码，避免中文日志乱码。
    if (java.major >= 19) {
        jvm.push_back("-Dstdout.encoding=UTF-8");
        jvm.push_back("-Dstderr.encoding=UTF-8");
    } else {
        jvm.push_back("-Dsun.stdout.encoding=UTF-8");
        jvm.push_back("-Dsun.stderr.encoding=UTF-8");
    }

    for (const Version& part : version.chain) {
        append_all(jvm, expand_args(part.jvm_args, features, placeholders));
    }
    if (!has_flag(jvm, "-cp") && !has_flag(jvm, "-classpath")) {
        jvm.push_back("-cp");
        jvm.push_back(classpath);
    }
    for (const std::string& extra : options.extra_jvm_args) {
        jvm.push_back(apply_placeholders(extra, placeholders));
    }

    // Forge/NeoForge 的版本 JSON 会带 -DignoreList=client-extra,${version_name}.jar，
    // 但 ${version_name} 是加载器实例名，加载器目录里并没有这个 jar。
    // 显式把原版 jar 名补进 ignoreList，作为原版 jar 不进 classpath 之外的双保险。
    if (is_forge_like) {
        constexpr std::string_view kIgnorePrefix = "-DignoreList=";
        const std::string vanilla_jar = version.jar_id + ".jar";
        for (std::string& arg : jvm) {
            if (arg.rfind(kIgnorePrefix, 0) != 0) continue;
            std::string list = arg.substr(kIgnorePrefix.size());
            if (list.find(vanilla_jar) != std::string::npos) break;
            if (!list.empty()) list.push_back(',');
            list += vanilla_jar;
            arg = std::string(kIgnorePrefix) + list;
            Log::info("已修正 ignoreList：" + arg);
            break;
        }
    }
    // 注意：不能按 token 去重。现代版本会重复出现 --add-opens / --add-exports，
    // 去掉重复的 flag 会留下孤立的参数值，Java 会把它当成主类。

    // ---- 游戏参数 ----
    std::vector<std::string> game;
    for (const Version& part : version.chain) {
        if (!part.game_args.empty()) {
            append_all(game, expand_args(part.game_args, features, placeholders));
        } else if (part.minecraft_arguments && !part.minecraft_arguments->empty()) {
            for (const std::string& item : split_legacy_args(*part.minecraft_arguments)) {
                game.push_back(apply_placeholders(item, placeholders));
            }
        }
    }

    set_option(game, "--width", std::to_string(options.width));
    set_option(game, "--height", std::to_string(options.height));
    set_switch(game, "--demo", options.demo);
    if (options.fullscreen) set_switch(game, "--fullscreen", true);

    if (!options.server.empty()) {
        std::string host;
        std::string port;
        parse_server(options.server, host, port);
        set_option(game, "--server", host);
        set_option(game, "--port", port);
    }

    // 极老的版本可能没有用户名等占位符，兜底补齐。
    if (!has_flag(game, "--username")) {
        game.push_back("--username");
        game.push_back(options.player_name);
        game.push_back("--uuid");
        game.push_back(uuid);
        game.push_back("--accessToken");
        game.push_back(options.access_token);
        game.push_back("--userType");
        game.push_back(options.user_type);
        game.push_back("--versionType");
        game.push_back(version.merged.type.empty() ? "release" : version.merged.type);
    }

    for (const std::string& extra : options.extra_game_args) {
        game.push_back(apply_placeholders(extra, placeholders));
    }

    // ---- 命令行 ----
    std::vector<std::wstring> command;
    command.push_back(java.exe.wstring());
    for (const auto& arg : jvm) command.push_back(to_wide(arg));
    command.push_back(to_wide(*version.merged.main_class));
    for (const auto& arg : game) command.push_back(to_wide(arg));

    PreparedLaunch prepared;
    prepared.version = std::move(version);
    prepared.java_exe = java.exe;
    prepared.game_dir = game_dir;
    prepared.saves_dir = game_dir / L"saves";
    prepared.natives_dir = natives_dir;
    prepared.main_class = *prepared.version.merged.main_class;
    prepared.classpath = std::move(classpath);
    prepared.java_version = java.version;
    prepared.java_major = java.major;
    prepared.java_bits = java.bits;
    prepared.jvm_args = std::move(jvm);
    prepared.game_args = std::move(game);
    prepared.command_line = build_command_line(command);
    prepared.output_code_page =
        java.major >= 18 ? kCodePageUtf8 : default_fallback_code_page();

    Log::info("Java：" + java.version + " (" + path_to_utf8(java.exe) + ")");
    Log::info("命令行长度：" + std::to_string(prepared.command_line.size()));
    if (prepared.command_line.size() > 30000) {
        Log::warn("命令行接近 Windows 上限（32767），后续需要改用 JavaWrapper / @argfile");
    }

    return prepared;
}

Result<std::unique_ptr<Process>> launch_prepared(
    const PreparedLaunch& prepared, Process::OutputFn on_output) {
    return Process::start(prepared.command_line, prepared.game_dir,
                          std::move(on_output), prepared.output_code_page);
}

} // namespace cl::mc