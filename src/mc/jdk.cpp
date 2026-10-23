#include "mc/jdk.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "core/archive.h"
#include "core/crypto.h"
#include "core/encoding.h"
#include "core/http.h"
#include "core/log.h"
#include "core/paths.h"

namespace cl::mc {

namespace {

constexpr auto kCacheTtl = std::chrono::minutes(10);
const std::vector<int> kFeatures = {8, 11, 17, 21, 25};

std::mutex g_release_mutex;
std::vector<JdkRelease> g_release_cache;
std::chrono::steady_clock::time_point g_release_time{};

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

std::string safe_name(std::string text) {
    for (char& c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '.' && c != '-' && c != '_') c = '-';
    }
    while (text.find("--") != std::string::npos) {
        const size_t pos = text.find("--");
        text.replace(pos, 2, "-");
    }
    while (!text.empty() && text.front() == '-') text.erase(text.begin());
    while (!text.empty() && text.back() == '-') text.pop_back();
    return text.empty() ? "jdk" : text;
}

std::filesystem::path find_java_exe(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return {};
    std::filesystem::path fallback;
    int visited = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied,
             ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (++visited > 20000) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() != L"java.exe") continue;
        if (it->path().parent_path().filename() != L"bin") continue;
        // JDK 8 会同时带 jre\bin\java.exe，优先返回 JDK 根目录下的 bin。
        const auto home = it->path().parent_path().parent_path();
        if (home.filename() == L"jre" &&
            std::filesystem::is_regular_file(
                home.parent_path() / L"bin" / L"java.exe", ec)) {
            if (fallback.empty()) fallback = it->path();
            continue;
        }
        return it->path();
    }
    return fallback;
}

bool path_under(const std::filesystem::path& path,
                const std::filesystem::path& root) {
    std::error_code ec;
    const auto normalized_path = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const auto normalized_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const auto relative = normalized_path.lexically_relative(normalized_root);
    if (relative.empty()) return false;
    for (const auto& part : relative) {
        if (part == L"..") return false;
    }
    return true;
}

Result<std::vector<JdkRelease>> fetch_releases_network() {
    std::vector<JdkRelease> result;
    std::string last_error;

    for (const int feature : kFeatures) {
        const std::string url =
            "https://api.adoptium.net/v3/assets/latest/" +
            std::to_string(feature) +
            "/hotspot?architecture=x64&image_type=jdk&os=windows&vendor=eclipse";
        HttpOptions options;
        options.timeout_seconds = 25;
        auto body = http_get(url, options);
        if (!body) {
            last_error = body.error().to_string();
            continue;
        }

        try {
            nlohmann::json json = nlohmann::json::parse(body.value());
            if (!json.is_array() || json.empty() || !json[0].is_object()) {
                last_error = "Adoptium 返回空列表";
                continue;
            }
            const auto& entry = json[0];
            const auto binary = entry.find("binary");
            if (binary == entry.end() || !binary->is_object()) continue;
            const auto package = binary->find("package");
            if (package == binary->end() || !package->is_object()) continue;

            JdkRelease release;
            release.feature = feature;
            release.release_name = json_string(entry, "release_name");
            const auto version = entry.find("version");
            if (version != entry.end() && version->is_object()) {
                release.version = json_string(*version, "openjdk_version");
            }
            release.file_name = json_string(*package, "name");
            release.url = json_string(*package, "link");
            release.sha256 = lower_copy(json_string(*package, "checksum"));
            release.size = json_ull(*package, "size");
            if (release.file_name.empty() || release.url.empty()) continue;
            release.mirror_url =
                "https://mirrors.tuna.tsinghua.edu.cn/Adoptium/" +
                std::to_string(feature) + "/jdk/x64/windows/" +
                release.file_name;
            result.push_back(std::move(release));
        } catch (const std::exception& ex) {
            last_error = ex.what();
        }
    }

    if (result.empty()) {
        return Error::make(1, "无法获取 OpenJDK 版本列表", last_error);
    }
    return result;
}

Result<void> download_release(const JdkRelease& release,
                              const std::filesystem::path& archive,
                              const DownloadProgressCallback& progress,
                              DownloadCancelToken& cancel) {
    std::vector<std::string> urls;
    if (!release.mirror_url.empty()) urls.push_back(release.mirror_url);
    if (!release.url.empty()) urls.push_back(release.url);

    std::string last_error;
    for (const auto& url : urls) {
        if (cancel.load()) return Error::make(499, "下载已取消");

        HttpOptions options;
        options.expected_size = release.size;
        options.timeout_seconds = 60;
        auto result = http_download(
            url, archive, options,
            [&](const HttpProgress& http) {
                if (!progress) return;
                DownloadProgress item;
                item.stage = "Java 运行环境";
                item.message = "正在下载 Temurin JDK " +
                               std::to_string(release.feature) + "…";
                item.file = release.file_name;
                item.total_files = 1;
                item.downloaded_bytes = http.downloaded;
                item.total_bytes = http.total;
                progress(item);
            },
            &cancel);
        if (!result) {
            last_error = result.error().to_string();
            std::error_code ec;
            std::filesystem::remove(archive, ec);
            continue;
        }

        std::error_code ec;
        const auto size = std::filesystem::file_size(archive, ec);
        if (ec || (release.size > 0 && size != release.size)) {
            last_error = "JDK 安装包大小不正确";
            std::filesystem::remove(archive, ec);
            continue;
        }
        auto hash = sha256_file(archive);
        if (!hash || lower_copy(hash.value()) != release.sha256) {
            last_error = "JDK 安装包 SHA-256 校验失败";
            std::filesystem::remove(archive, ec);
            continue;
        }
        return {};
    }
    return Error::make(2, "OpenJDK 下载失败", last_error);
}

} // namespace

std::filesystem::path managed_jdk_root() {
    return local_app_data_dir() / L"CraftLauncher" / L"jdk";
}

bool is_managed_jdk_path(const std::filesystem::path& path) {
    return path_under(path, managed_jdk_root());
}

namespace {

Result<void> recycle_directory(const std::filesystem::path& dir) {
    const std::wstring root = dir.root_name().wstring();
    if (!root.empty()) {
        const std::wstring drive = root + L"\\";
        if (::GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) {
            return Error::make(21, "只支持删除本地磁盘上的 Java",
                               path_to_utf8(dir));
        }
    }

    std::wstring from = dir.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
                       FOF_NOERRORUI;

    const int result = ::SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        return Error::make(22, "删除 Java 失败",
                           "Shell 错误码 " + std::to_string(result));
    }
    return {};
}

} // namespace

Result<void> delete_managed_jdk(const std::filesystem::path& java_exe) {
    std::error_code ec;
    // 只接受 <managed>/<安装目录>/.../bin/java.exe，避免把别的路径交给删除逻辑。
    if (java_exe.filename() != L"java.exe" ||
        java_exe.parent_path().filename() != L"bin" ||
        !is_managed_jdk_path(java_exe)) {
        return Error::make(20, "只能删除启动器下载的 Java",
                           path_to_utf8(java_exe));
    }

    const auto root = std::filesystem::weakly_canonical(managed_jdk_root(), ec);
    if (ec) {
        return Error::make(23, "无法定位 JDK 目录",
                           path_to_utf8(managed_jdk_root()));
    }
    const auto exe = std::filesystem::weakly_canonical(java_exe, ec);
    if (ec) return Error::make(23, "无法定位 Java", path_to_utf8(java_exe));

    const auto relative = exe.lexically_relative(root);
    if (relative.empty()) {
        return Error::make(20, "只能删除启动器下载的 Java",
                           path_to_utf8(java_exe));
    }
    const auto first = *relative.begin();
    if (first == L"." || first == L".." || first == L".cache") {
        return Error::make(20, "只能删除启动器下载的 Java",
                           path_to_utf8(java_exe));
    }

    const std::filesystem::path install_dir = root / first;
    if (!std::filesystem::is_directory(install_dir, ec)) {
        // 重复删除或已经被其它操作移走：结果与成功一致，方便前端幂等重试。
        Log::info("JDK 目录已不存在，跳过删除：" + path_to_utf8(install_dir));
        return {};
    }

    auto result = recycle_directory(install_dir);
    if (!result) return result;
    Log::info("已删除启动器下载的 JDK：" + path_to_utf8(install_dir));
    return {};
}

Result<void> open_java_uninstaller(const std::filesystem::path& java_exe) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(java_exe, ec)) {
        return Error::make(25, "Java 不存在", path_to_utf8(java_exe));
    }

    const auto home = java_exe.parent_path().parent_path();
    const std::vector<std::filesystem::path> candidates = {
        home / L"uninstall.exe",
        home / L"Uninstall.exe",
        home / L"bin" / L"uninstall.exe",
        home.parent_path() / L"uninstall.exe",
        home.parent_path() / L"Uninstall.exe",
    };

    for (const auto& candidate : candidates) {
        if (!std::filesystem::is_regular_file(candidate, ec)) continue;
        const auto launched = ::ShellExecuteW(
            nullptr, L"open", candidate.c_str(), nullptr, home.c_str(),
            SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) > 32) {
            Log::info("打开 Java 卸载程序：" + path_to_utf8(candidate));
            return {};
        }
    }

    // 第三方 JDK 通常由 MSI 安装，目录里没有卸载程序，
    // 这时打开 Windows 自带的“应用和功能”。
    auto launched = ::ShellExecuteW(
        nullptr, L"open", L"ms-settings:appsfeatures", nullptr, nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        launched = ::ShellExecuteW(nullptr, L"open", L"appwiz.cpl", nullptr,
                                   nullptr, SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        return Error::make(26, "无法打开卸载程序",
                           "请在 Windows 设置 → 应用 中手动卸载 Java");
    }

    Log::info("已打开 Windows 应用列表，供手动卸载 Java：" +
              path_to_utf8(java_exe));
    return {};
}

Result<std::vector<JdkRelease>> fetch_openjdk_releases(bool force_refresh) {
    std::lock_guard lock(g_release_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!force_refresh && !g_release_cache.empty() &&
        now - g_release_time < kCacheTtl) {
        return g_release_cache;
    }

    auto releases = fetch_releases_network();
    if (!releases) return releases.error();
    g_release_cache = releases.value();
    g_release_time = now;
    return g_release_cache;
}

Result<JavaInfo> install_openjdk(int feature, bool force,
                                 const DownloadProgressCallback& progress,
                                 DownloadCancelToken& cancel) {
    auto releases = fetch_openjdk_releases(false);
    if (!releases) return releases.error();

    auto find_release = [&](const std::vector<JdkRelease>& list)
        -> const JdkRelease* {
        for (const auto& release : list) {
            if (release.feature == feature) return &release;
        }
        return nullptr;
    };

    const JdkRelease* release = find_release(releases.value());
    if (release == nullptr) {
        releases = fetch_openjdk_releases(true);
        if (releases) release = find_release(releases.value());
    }
    if (release == nullptr) {
        return Error::make(3, "没有找到这个 OpenJDK 版本",
                           std::to_string(feature));
    }

    const std::filesystem::path root = managed_jdk_root();
    const std::string id = safe_name(
        "temurin-" + std::to_string(feature) + "-" + release->version);
    const std::filesystem::path final_dir = root / path_from_utf8(id);
    const std::filesystem::path temp_dir = root / path_from_utf8(id + ".extract");
    const std::filesystem::path cache_dir = root / L".cache";

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        return Error::make(4, "无法创建 JDK 目录", path_to_utf8(cache_dir));
    }

    // 已经安装过就直接复用；force 时删除后重新下载。
    if (!force) {
        if (const auto existing = find_java_exe(final_dir); !existing.empty()) {
            auto info = probe_java(existing);
            if (info) {
                JavaInfo java = std::move(info).value();
                java.managed = true;
                return java;
            }
        }
    }

    const std::filesystem::path archive =
        cache_dir / path_from_utf8(release->file_name);
    std::filesystem::remove(archive, ec);

    auto downloaded = download_release(*release, archive, progress, cancel);
    if (!downloaded) return downloaded.error();

    if (progress) {
        DownloadProgress item;
        item.stage = "解压 Java";
        item.message = "正在解压 Temurin JDK " + std::to_string(feature) + "…";
        item.file = release->file_name;
        item.completed_files = 1;
        item.total_files = 1;
        item.downloaded_bytes = release->size;
        item.total_bytes = release->size;
        progress(item);
    }

    std::filesystem::remove_all(temp_dir, ec);
    auto extracted = extract_zip(archive, temp_dir);
    std::filesystem::remove(archive, ec);
    if (!extracted) {
        std::filesystem::remove_all(temp_dir, ec);
        return extracted.error();
    }

    if (const auto java_exe = find_java_exe(temp_dir); java_exe.empty()) {
        std::filesystem::remove_all(temp_dir, ec);
        return Error::make(5, "JDK 解压后没有找到 java.exe",
                           path_to_utf8(temp_dir));
    }

    // 新版本已经完整解压并通过检查，这时才替换旧目录；
    // 下载或解压失败时原有 JDK 保持不变。
    std::filesystem::remove_all(final_dir, ec);
    std::filesystem::rename(temp_dir, final_dir, ec);
    if (ec) {
        std::filesystem::remove_all(temp_dir, ec);
        return Error::make(6, "无法完成 JDK 安装",
                           path_to_utf8(final_dir) + ": " + ec.message());
    }

    const auto java_exe = find_java_exe(final_dir);
    if (java_exe.empty()) {
        return Error::make(7, "JDK 安装后没有找到 java.exe",
                           path_to_utf8(final_dir));
    }
    auto info = probe_java(java_exe);
    if (!info) return info.error();

    JavaInfo java = std::move(info).value();
    java.managed = true;
    if (progress) {
        DownloadProgress item;
        item.stage = "完成";
        item.message = "OpenJDK 安装完成：" + java.version;
        item.completed_files = 1;
        item.total_files = 1;
        item.downloaded_bytes = release->size;
        item.total_bytes = release->size;
        progress(item);
    }
    Log::info("OpenJDK 安装完成：" + java.version + " -> " +
              path_to_utf8(java.exe));
    return java;
}

} // namespace cl::mc
