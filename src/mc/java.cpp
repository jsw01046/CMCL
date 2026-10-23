#include "mc/java.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

#include "core/encoding.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/process.h"

namespace cl::mc {

namespace {

std::mutex g_java_cache_mutex;
std::vector<JavaInfo> g_java_cache;
std::wstring g_java_cache_key;
std::chrono::steady_clock::time_point g_java_cache_time{};

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

struct SimpleVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

std::optional<SimpleVersion> parse_vanilla_version(const std::string& id) {
    int major = 0;
    int minor = 0;
    int patch = 0;
    const int count = std::sscanf(id.c_str(), "%d.%d.%d", &major, &minor, &patch);
    if (count < 2) return std::nullopt;
    return SimpleVersion{major, minor, patch};
}

bool version_ge(const SimpleVersion& v, int a, int b, int c) {
    if (v.major != a) return v.major > a;
    if (v.minor != b) return v.minor > b;
    return v.patch >= c;
}

bool version_le(const SimpleVersion& v, int a, int b, int c) {
    if (v.major != a) return v.major < a;
    if (v.minor != b) return v.minor < b;
    return v.patch <= c;
}

bool is_forge_instance(const ResolvedVersion& version) {
    for (const Library& library : version.merged.libraries) {
        if (library.name.rfind("net.minecraftforge:forge", 0) == 0 ||
            library.name.rfind("net.minecraftforge:minecraftforge", 0) == 0) {
            return true;
        }
    }
    if (version.merged.main_class) {
        if (version.merged.main_class->find("launchwrapper") != std::string::npos ||
            version.merged.main_class->find("cpw.mods.fml") != std::string::npos) {
            return true;
        }
    }
    return false;
}

int major_from_version(const std::string& version) {
    try {
        if (version.rfind("1.", 0) == 0) {
            return std::stoi(version.substr(2));
        }
        return std::stoi(version);
    } catch (...) {
        return 0;
    }
}

void add_candidate(std::vector<std::filesystem::path>& candidates,
                   const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        candidates.push_back(path);
    }
}

void scan_java_root(const std::filesystem::path& root,
                    std::vector<std::filesystem::path>& candidates) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    add_candidate(candidates, root / L"java.exe");
    add_candidate(candidates, root / L"bin" / L"java.exe");

    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        const auto& sub = entry.path();
        add_candidate(candidates, sub / L"bin" / L"java.exe");
        for (const auto& inner : std::filesystem::directory_iterator(sub, ec)) {
            if (ec) break;
            if (!inner.is_directory(ec)) continue;
            add_candidate(candidates, inner.path() / L"bin" / L"java.exe");
        }
    }
}

void scan_runtime_dir(const std::filesystem::path& root,
                      std::vector<std::filesystem::path>& candidates) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    int visited = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (++visited > 4000) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() != L"java.exe") continue;
        if (it->path().parent_path().filename() != L"bin") continue;
        candidates.push_back(it->path());
    }
}

bool path_under(const std::filesystem::path& path,
                const std::filesystem::path& root) {
    std::error_code ec;
    const std::filesystem::path normalized_path =
        std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const std::filesystem::path normalized_root =
        std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const std::filesystem::path relative =
        normalized_path.lexically_relative(normalized_root);
    if (relative.empty()) return false;
    for (const auto& part : relative) {
        if (part == L"..") return false;
    }
    return true;
}

std::vector<std::filesystem::path> path_entries() {
    std::vector<std::filesystem::path> result;
    wchar_t buffer[32768];
    const DWORD size = ::GetEnvironmentVariableW(L"PATH", buffer, 32768);
    if (size == 0 || size >= 32768) return result;

    std::wstring text(buffer, size);
    size_t start = 0;
    while (start <= text.size()) {
        const size_t pos = text.find(L';', start);
        std::wstring item = text.substr(
            start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
        if (!item.empty()) {
            if (item.front() == L'"' && item.back() == L'"') {
                item = item.substr(1, item.size() - 2);
            }
            result.emplace_back(item);
        }
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    return result;
}

} // namespace

Result<JavaInfo> probe_java(const std::filesystem::path& exe) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exe, ec)) {
        return Error::make(1, "Java 可执行文件不存在", path_to_utf8(exe));
    }

    std::string output;
    const std::wstring command = build_command_line(
        {exe.wstring(), L"-XshowSettings:properties", L"-version"});
    auto started = Process::start(
        command, exe.parent_path(), [&](std::string_view line) {
            output.append(line);
            output.push_back('\n');
        });
    if (!started) return started.error();

    auto process = std::move(started).value();
    process->wait();

    std::string version;
    std::string home;
    std::string arch;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string cleaned = trim(line);
        if (const size_t eq = cleaned.find('='); eq != std::string::npos) {
            const std::string key = trim(cleaned.substr(0, eq));
            const std::string value = trim(cleaned.substr(eq + 1));
            if (key == "java.version") version = value;
            else if (key == "java.home") home = value;
            else if (key == "os.arch") arch = value;
            continue;
        }
        // openjdk version "21.0.1" / java version "1.8.0_392"
        if (version.empty()) {
            const size_t first = cleaned.find('"');
            if (first != std::string::npos) {
                const size_t last = cleaned.find('"', first + 1);
                if (last != std::string::npos) {
                    version = cleaned.substr(first + 1, last - first - 1);
                }
            }
        }
    }

    if (version.empty()) {
        return Error::make(2, "无法解析 Java 版本", path_to_utf8(exe));
    }

    JavaInfo info;
    info.exe = exe;
    info.version = version;
    info.major = major_from_version(version);
    info.home = home.empty() ? exe.parent_path().parent_path()
                             : path_from_utf8(home);
    if (arch.find("64") != std::string::npos) {
        info.bits = 64;
    } else if (arch.find("86") != std::string::npos ||
               arch.find("i386") != std::string::npos) {
        info.bits = 32;
    }

    if (info.major <= 0) {
        return Error::make(3, "无法识别 Java 主版本", version);
    }
    return info;
}

std::vector<JavaInfo> detect_java(const std::filesystem::path& mc_dir,
                                  bool force_refresh) {
    const std::wstring cache_key = mc_dir.wstring();
    {
        std::lock_guard lock(g_java_cache_mutex);
        if (!force_refresh && !g_java_cache.empty() &&
            cache_key == g_java_cache_key &&
            std::chrono::steady_clock::now() - g_java_cache_time <
                std::chrono::seconds(60)) {
            return g_java_cache;
        }
    }

    std::vector<std::filesystem::path> candidates;

    wchar_t buffer[32768];
    if (const DWORD size = ::GetEnvironmentVariableW(L"JAVA_HOME", buffer, 32768);
        size > 0 && size < 32768) {
        add_candidate(candidates, std::filesystem::path(buffer) / L"bin" / L"java.exe");
    }

    for (const auto& entry : path_entries()) {
        add_candidate(candidates, entry / L"java.exe");
    }

    std::vector<std::filesystem::path> roots;
    if (const DWORD size = ::GetEnvironmentVariableW(L"ProgramFiles", buffer, 32768);
        size > 0 && size < 32768) {
        const std::filesystem::path pf(buffer);
        roots.push_back(pf / L"Java");
        roots.push_back(pf / L"Eclipse Adoptium");
        roots.push_back(pf / L"Microsoft");
        roots.push_back(pf / L"Zulu");
        roots.push_back(pf / L"BellSoft");
        roots.push_back(pf / L"Amazon Corretto");
        roots.push_back(pf / L"Semeru");
    }
    if (const DWORD size = ::GetEnvironmentVariableW(L"ProgramFiles(x86)", buffer, 32768);
        size > 0 && size < 32768) {
        const std::filesystem::path pf86(buffer);
        roots.push_back(pf86 / L"Java");
        roots.push_back(pf86 / L"Eclipse Adoptium");
        roots.push_back(pf86 / L"Microsoft");
        roots.push_back(pf86 / L"Zulu");
        roots.push_back(pf86 / L"BellSoft");
        roots.push_back(pf86 / L"Amazon Corretto");
        roots.push_back(pf86 / L"Semeru");
    }
    if (const DWORD size = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
        size > 0 && size < 32768) {
        roots.push_back(std::filesystem::path(buffer) / L"Programs" /
                        L"Eclipse Adoptium");
        roots.push_back(std::filesystem::path(buffer) / L"Programs" / L"Java");
    }

    for (const auto& root : roots) scan_java_root(root, candidates);

    // 启动器自带的 runtime：当前游戏目录 + 系统默认 .minecraft。
    if (!mc_dir.empty()) scan_runtime_dir(mc_dir / L"runtime", candidates);
    if (const DWORD size = ::GetEnvironmentVariableW(L"APPDATA", buffer, 32768);
        size > 0 && size < 32768) {
        const std::filesystem::path appdata(buffer);
        scan_runtime_dir(appdata / L".minecraft" / L"runtime", candidates);
    }

    // IntelliJ / Gradle 下载的 JDK。
    if (const DWORD size = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, 32768);
        size > 0 && size < 32768) {
        const std::filesystem::path profile(buffer);
        scan_java_root(profile / L".jdks", candidates);
        scan_java_root(profile / L".gradle" / L"jdks", candidates);
    }

    // 启动器自己下载的 OpenJDK。目录被删掉时自动补回来。
    const std::filesystem::path managed_root =
        local_app_data_dir() / L"CraftLauncher" / L"jdk";
    std::error_code managed_ec;
    std::filesystem::create_directories(managed_root, managed_ec);
    scan_runtime_dir(managed_root, candidates);

    // 去重并按路径排序，保证结果稳定。
    std::set<std::wstring> seen;
    std::vector<std::filesystem::path> unique;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        // JDK 自带的 jre\bin\java.exe 与 jdk\bin\java.exe 是同一套运行时，
        // 只保留 JDK 根目录下的那个，避免列表里出现重复项。
        const auto bin = candidate.parent_path();
        const auto home = bin.parent_path();
        if (bin.filename() == L"bin" && home.filename() == L"jre" &&
            std::filesystem::is_regular_file(
                home.parent_path() / L"bin" / L"java.exe", ec)) {
            continue;
        }
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(candidate, ec);
        std::wstring key = (ec ? candidate : canonical).wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        if (!seen.insert(key).second) continue;
        unique.push_back(candidate);
    }

    std::vector<JavaInfo> result;
    int probed = 0;
    for (const auto& exe : unique) {
        if (probed++ >= 32) break;
        auto info = probe_java(exe);
        if (!info) {
            Log::debug("Java 探测失败：" + path_to_utf8(exe) + " -> " +
                       info.error().to_string());
            continue;
        }
        JavaInfo java = std::move(info).value();
        java.managed = path_under(java.exe, managed_root);
        result.push_back(std::move(java));
    }

    std::sort(result.begin(), result.end(), [](const JavaInfo& a, const JavaInfo& b) {
        if (a.major != b.major) return a.major > b.major;
        if (a.bits != b.bits) return a.bits > b.bits;  // 优先 64 位
        return a.exe.wstring() < b.exe.wstring();
    });

    Log::info("检测到 " + std::to_string(result.size()) + " 个可用 Java");
    {
        std::lock_guard lock(g_java_cache_mutex);
        g_java_cache = result;
        g_java_cache_key = cache_key;
        g_java_cache_time = std::chrono::steady_clock::now();
    }
    return result;
}

int required_java_major(const ResolvedVersion& version) {
    int required = 21;
    const auto vanilla = parse_vanilla_version(version.vanilla_id);
    const bool forge = is_forge_instance(version);

    if (forge && vanilla && version_ge(*vanilla, 1, 6, 1) &&
        version_le(*vanilla, 1, 7, 2)) {
        // 与 PCL 的规则一致：Forge 1.6.1 - 1.7.2 需要 Java 7。
        return 7;
    }
    if (version.merged.java_major) {
        return *version.merged.java_major;
    }
    if (vanilla && vanilla->major == 1) {
        if (vanilla->minor <= 16) {
            required = 8;
        } else if (vanilla->minor == 17) {
            required = 16;
        } else if (vanilla->minor <= 20) {
            required = 17;
            // 1.20.5 起需要 Java 21。
            if (vanilla->minor == 20 && vanilla->patch >= 5) required = 21;
        } else {
            required = 21;
        }
    }
    return required;
}

Result<JavaInfo> select_java(
    const ResolvedVersion& version,
    const std::optional<std::filesystem::path>& explicit_exe,
    const std::filesystem::path& mc_dir) {
    const int required = required_java_major(version);
    if (explicit_exe) {
        auto info = probe_java(*explicit_exe);
        if (!info) {
            return Error::make(10, "指定的 Java 不可用", info.error().to_string());
        }
        const JavaInfo java = std::move(info).value();
        if (java.major < required) {
            return Error::make(
                13, "Java 版本过低",
                "此版本至少需要 Java " + std::to_string(required) +
                    "，当前指定的是 Java " + java.version + "（" +
                    path_to_utf8(java.exe) + "）");
        }
        return java;
    }

    const std::vector<JavaInfo> javas = detect_java(mc_dir);
    if (javas.empty()) {
        return Error::make(11, "没有找到可用的 Java",
                           "请安装 JDK，或用 --java 指定 java.exe");
    }

    for (const JavaInfo& java : javas) {
        if (java.major == required && java.bits == 64) {
            Log::info("选择 Java " + java.version + "：" + path_to_utf8(java.exe));
            return java;
        }
    }
    for (const JavaInfo& java : javas) {
        if (java.major == required) {
            Log::info("选择 Java " + java.version + "：" + path_to_utf8(java.exe));
            return java;
        }
    }

    if (required == 7) {
        for (const JavaInfo& java : javas) {
            if (java.major == 8 && java.bits == 64) {
                Log::warn("没有找到 64 位 Java 7，改用 Java 8");
                return java;
            }
        }
        for (const JavaInfo& java : javas) {
            if (java.major == 8) {
                Log::warn("没有找到 Java 7，改用 Java 8");
                return java;
            }
        }
        return Error::make(12, "此版本需要 Java 7",
                           "未找到 Java 7，请安装 Java 7 或用 --java 指定");
    }

    const JavaInfo* best = nullptr;
    for (const JavaInfo& java : javas) {
        if (java.major < required) continue;
        if (best == nullptr || java.major < best->major) best = &java;
    }
    if (best != nullptr) {
        Log::warn("没有找到 Java " + std::to_string(required) + "，改用 Java " +
                  best->version);
        return *best;
    }

    return Error::make(
        13, "Java 版本过低",
        "此版本至少需要 Java " + std::to_string(required) +
            "，当前可用的最高版本是 Java " + javas.front().version +
            "。请安装 " + std::to_string(required) +
            " 或更高版本的 64 位 JDK（可在“设置 → Java”中下载）。");
}

} // namespace cl::mc
