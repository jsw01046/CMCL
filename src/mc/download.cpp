#include "mc/download.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "core/crypto.h"
#include "core/encoding.h"
#include "core/http.h"
#include "core/log.h"
#include "core/paths.h"
#include "mc/instance.h"
#include "mc/version.h"

namespace cl::mc {

namespace {

constexpr const char* kOfficialManifest =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
constexpr const char* kResourcesBase =
    "https://resources.download.minecraft.net/";
// 官方源低于约 1 MB/s 就认为偏慢，后续文件改用镜像；
// 镜像低于 128 KB/s 再尝试下一个镜像。
constexpr unsigned long long kOfficialMinSpeed = 1024ULL * 1024;
constexpr unsigned long long kMirrorMinSpeed = 128ULL * 1024;
constexpr auto kManifestTtl = std::chrono::minutes(10);

std::mutex g_manifest_mutex;
std::optional<VersionManifest> g_manifest;
std::chrono::steady_clock::time_point g_manifest_time;

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool equals_hex(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower_copy(a) == lower_copy(b);
}

bool is_sha1(const std::string& text) {
    if (text.size() != 40) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool safe_relative(const std::string& text) {
    if (text.empty()) return false;
    const std::filesystem::path path = path_from_utf8(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == L"..") return false;
    }
    return true;
}

std::filesystem::path manifest_cache_path() {
    return local_app_data_dir() / L"CraftLauncher" / L"cache" /
           L"version_manifest_v2.json";
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

std::string json_string(const nlohmann::json& json, const char* key,
                        std::string fallback = {}) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

Result<VersionManifest> parse_manifest_json(const nlohmann::json& json) {
    if (!json.is_object()) {
        return Error::make(1, "版本清单不是对象");
    }
    const auto versions = json.find("versions");
    if (versions == json.end() || !versions->is_array()) {
        return Error::make(2, "版本清单缺少 versions 数组");
    }

    VersionManifest manifest;
    if (const auto latest = json.find("latest");
        latest != json.end() && latest->is_object()) {
        manifest.latest_release = json_string(*latest, "release");
        manifest.latest_snapshot = json_string(*latest, "snapshot");
    }

    for (const auto& entry : *versions) {
        if (!entry.is_object()) continue;
        VersionSummary summary;
        summary.id = json_string(entry, "id");
        summary.type = json_string(entry, "type");
        summary.url = json_string(entry, "url");
        summary.sha1 = lower_copy(json_string(entry, "sha1"));
        summary.size = json_ull(entry, "size");
        summary.release_time = json_string(entry, "releaseTime");
        if (summary.release_time.empty()) {
            summary.release_time = json_string(entry, "time");
        }
        if (summary.id.empty()) continue;
        manifest.versions.push_back(std::move(summary));
    }

    if (manifest.versions.empty()) {
        return Error::make(3, "版本清单为空");
    }
    return manifest;
}

Result<VersionManifest> load_manifest_cache() {
    const auto path = manifest_cache_path();
    std::ifstream file(path, std::ios::binary);
    if (!file) return Error::make(4, "没有版本清单缓存", path_to_utf8(path));

    try {
        nlohmann::json json;
        file >> json;
        return parse_manifest_json(json);
    } catch (const std::exception& ex) {
        return Error::make(5, "版本清单缓存损坏",
                           path_to_utf8(path) + ": " + ex.what());
    }
}

void save_manifest_cache(const std::string& body) {
    const auto path = manifest_cache_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << body;
}

Result<void> write_json_file(const std::filesystem::path& path,
                             const nlohmann::json& json) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return Error::make(6, "无法创建版本目录",
                               path_to_utf8(path.parent_path()) + ": " +
                                   ec.message());
        }
    }

    std::filesystem::path temporary = path;
    temporary += L".new";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Error::make(7, "无法写入 JSON", path_to_utf8(temporary));
        }
        file << json.dump(2);
        if (!file) {
            return Error::make(8, "写入 JSON 失败", path_to_utf8(temporary));
        }
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        std::filesystem::remove(temporary, ec);
        return Error::make(9, "无法保存 JSON",
                           path_to_utf8(path) + " (Win32 " +
                               std::to_string(error) + ")");
    }
    return {};
}

struct DownloadJob {
    std::string url;
    std::filesystem::path destination;
    std::string sha1;
    unsigned long long size = 0;
};

struct SourceCandidate {
    std::string id;
    std::string name;
    bool official = false;
    MirrorSource mirror;
};

class SourceSelector {
public:
    explicit SourceSelector(DownloadSource source) {
        if (source != DownloadSource::Mirror) {
            candidates_.push_back(SourceCandidate{
                "official", "官方", true, {}});
        }
        if (source != DownloadSource::Official) {
            for (const auto& mirror : available_mirrors()) {
                candidates_.push_back(
                    SourceCandidate{mirror.id, mirror.name, false, mirror});
            }
        }
        if (candidates_.empty()) {
            candidates_.push_back(SourceCandidate{
                "official", "官方", true, {}});
        }
        speeds_.assign(candidates_.size(), 0.0);
        failures_.assign(candidates_.size(), 0);
        success_streak_.assign(candidates_.size(), 0);
    }

    [[nodiscard]] size_t size() const { return candidates_.size(); }

    [[nodiscard]] bool is_official(size_t index) const {
        return candidates_[index].official;
    }

    [[nodiscard]] std::string name(size_t index) const {
        return candidates_[index].name;
    }

    [[nodiscard]] std::string map_url(size_t index, const std::string& url) const {
        if (candidates_[index].official) return url;
        return map_mirror_url(url, candidates_[index].mirror);
    }

    [[nodiscard]] unsigned long long min_speed(size_t index) const {
        return candidates_[index].official ? kOfficialMinSpeed : kMirrorMinSpeed;
    }

    [[nodiscard]] size_t preferred() const {
        std::lock_guard lock(mutex_);
        return preferred_;
    }

    [[nodiscard]] std::vector<size_t> order() const {
        std::lock_guard lock(mutex_);
        std::vector<size_t> indices(candidates_.size());
        for (size_t i = 0; i < indices.size(); ++i) indices[i] = i;

        const size_t preferred = preferred_;
        std::stable_sort(indices.begin(), indices.end(),
                         [&](size_t a, size_t b) {
                             if ((a == preferred) != (b == preferred)) {
                                 return a == preferred;
                             }
                             if (speeds_[a] != speeds_[b]) {
                                 return speeds_[a] > speeds_[b];
                             }
                             if (failures_[a] != failures_[b]) {
                                 return failures_[a] < failures_[b];
                             }
                             return a < b;
                         });
        return indices;
    }

    void record_success(size_t index, double bytes_per_second) {
        std::lock_guard lock(mutex_);
        if (index >= candidates_.size()) return;
        failures_[index] = 0;
        success_streak_[index] =
            (std::min)(success_streak_[index] + 1, 100);
        speeds_[index] = speeds_[index] == 0.0
                             ? bytes_per_second
                             : speeds_[index] * 0.65 + bytes_per_second * 0.35;

        if (index == preferred_) return;
        // 只有连续成功、且明显更快时才回切，避免并发下载时源来回横跳。
        if (success_streak_[index] < 2) return;
        if (!cooldown_elapsed(kSpeedSwitchCooldown)) return;

        const double current = speeds_[preferred_];
        if (speeds_[index] <= (std::max)(current * 1.50, 1536.0 * 1024)) return;

        const size_t old = preferred_;
        preferred_ = index;
        last_switch_ = std::chrono::steady_clock::now();
        Log::info("下载源切换到更快的源：" + candidates_[old].name + " -> " +
                  candidates_[index].name);
    }

    void record_failure(size_t index, bool slow) {
        std::lock_guard lock(mutex_);
        if (index >= candidates_.size()) return;
        failures_[index]++;
        speeds_[index] = 0.0;
        success_streak_[index] = 0;

        if (index != preferred_) return;
        if (!cooldown_elapsed(slow ? kSlowSwitchCooldown
                                   : kFailureSwitchCooldown)) {
            return;
        }

        size_t next = index;
        double best = -1.0;
        for (size_t i = 0; i < candidates_.size(); ++i) {
            if (i == index) continue;
            if (speeds_[i] > best) {
                best = speeds_[i];
                next = i;
            }
        }

        if (best <= 0.0) {
            // 其它源都还没有速度样本。慢速时如果当前源不比别人差，就
            // 保持不动；硬失败时才轮换到下一个源。
            int best_failures = 1000000;
            next = index;
            for (size_t i = 0; i < candidates_.size(); ++i) {
                if (i == index) continue;
                if (failures_[i] < best_failures) {
                    best_failures = failures_[i];
                    next = i;
                }
            }
            if (slow) {
                if (next == index || failures_[index] <= best_failures) return;
            } else if (next == index && candidates_.size() > 1) {
                next = (index + 1) % candidates_.size();
            }
        }

        if (next != index) {
            preferred_ = next;
            last_switch_ = std::chrono::steady_clock::now();
            Log::warn(std::string("下载源 ") + candidates_[index].name +
                      (slow ? " 速度过慢，" : " 下载失败，") + "切换到 " +
                      candidates_[next].name);
        }
    }

private:
    static constexpr auto kSpeedSwitchCooldown = std::chrono::seconds(10);
    static constexpr auto kSlowSwitchCooldown = std::chrono::seconds(10);
    static constexpr auto kFailureSwitchCooldown = std::chrono::seconds(2);

    [[nodiscard]] bool cooldown_elapsed(
        std::chrono::seconds seconds) const {
        if (last_switch_.time_since_epoch().count() == 0) return true;
        return std::chrono::steady_clock::now() - last_switch_ >= seconds;
    }

    std::vector<SourceCandidate> candidates_;
    std::vector<double> speeds_;
    std::vector<int> failures_;
    std::vector<int> success_streak_;
    size_t preferred_ = 0;
    std::chrono::steady_clock::time_point last_switch_{};
    mutable std::mutex mutex_;
};

class ProgressTracker {
public:
    explicit ProgressTracker(DownloadProgressCallback callback)
        : callback_(std::move(callback)) {}

    void set_totals(int files, unsigned long long bytes) {
        {
            std::lock_guard lock(mutex_);
            total_files_ = files;
            total_bytes_ = bytes;
        }
        report(true);
    }

    void set_completed(int files, unsigned long long bytes) {
        completed_.store(files, std::memory_order_relaxed);
        downloaded_.store(static_cast<long long>(bytes),
                          std::memory_order_relaxed);
        report(true);
    }

    void set_stage(std::string stage, std::string message) {
        {
            std::lock_guard lock(mutex_);
            stage_ = std::move(stage);
            message_ = std::move(message);
        }
        report(true);
    }

    void set_file(const std::filesystem::path& file, const std::string& source) {
        {
            std::lock_guard lock(mutex_);
            file_ = path_to_utf8(file);
            if (!source.empty()) file_ += "  [" + source + "]";
        }
        report(false);
    }

    void add_bytes(long long delta) {
        if (delta == 0) return;
        downloaded_.fetch_add(delta, std::memory_order_relaxed);
        report(false);
    }

    void file_done() {
        completed_.fetch_add(1, std::memory_order_relaxed);
        report(false);
    }

    [[nodiscard]] unsigned long long bytes() const {
        const long long value =
            downloaded_.load(std::memory_order_relaxed);
        return static_cast<unsigned long long>(value > 0 ? value : 0);
    }

    void report(bool force) {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_report_ < std::chrono::milliseconds(120)) {
            return;
        }
        last_report_ = now;

        if (!callback_) return;
        DownloadProgress progress;
        progress.stage = stage_;
        progress.message = message_;
        progress.file = file_;
        progress.completed_files = completed_.load(std::memory_order_relaxed);
        progress.total_files = total_files_;
        const long long downloaded =
            downloaded_.load(std::memory_order_relaxed);
        progress.downloaded_bytes = static_cast<unsigned long long>(
            downloaded > 0 ? downloaded : 0);
        progress.total_bytes = total_bytes_;
        try {
            callback_(progress);
        } catch (...) {
            // 进度回调不能影响下载本身。
        }
    }

private:
    DownloadProgressCallback callback_;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point last_report_{};
    std::string stage_;
    std::string message_;
    std::string file_;
    int total_files_ = 0;
    unsigned long long total_bytes_ = 0;
    std::atomic<int> completed_{0};
    std::atomic<long long> downloaded_{0};
};

bool existing_file_valid(const DownloadJob& job) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(job.destination, ec)) return false;
    if (job.size > 0) {
        const auto size = std::filesystem::file_size(job.destination, ec);
        if (ec || size != job.size) return false;
    }
    if (!job.sha1.empty()) {
        auto hash = sha1_file(job.destination);
        if (!hash || !equals_hex(hash.value(), job.sha1)) return false;
    }
    return true;
}

std::vector<DownloadJob> dedup_jobs(std::vector<DownloadJob> jobs) {
    std::vector<DownloadJob> result;
    std::set<std::wstring> seen;
    result.reserve(jobs.size());
    for (auto& job : jobs) {
        if (job.url.empty() || job.destination.empty()) continue;
        if (seen.insert(job.destination.wstring()).second) {
            result.push_back(std::move(job));
        }
    }
    return result;
}

Result<void> run_jobs(const std::string& stage,
                      std::vector<DownloadJob> jobs,
                      SourceSelector& selector,
                      ProgressTracker& tracker,
                      DownloadCancelToken& cancel) {
    jobs = dedup_jobs(std::move(jobs));
    if (jobs.empty()) return {};

    unsigned long long total_bytes = 0;
    for (const auto& job : jobs) total_bytes += job.size;
    tracker.set_totals(static_cast<int>(jobs.size()), total_bytes);

    tracker.set_stage(stage, "正在校验已有文件…");
    std::vector<DownloadJob> pending;
    pending.reserve(jobs.size());
    int valid_files = 0;
    unsigned long long valid_bytes = 0;
    int checked = 0;

    for (const auto& job : jobs) {
        if (cancel.load(std::memory_order_relaxed)) {
            return Error::make(499, "下载已取消");
        }
        tracker.set_file(job.destination, "");
        if (existing_file_valid(job)) {
            ++valid_files;
            valid_bytes += job.size;
            ++checked;
            tracker.set_completed(valid_files, valid_bytes);
        } else {
            pending.push_back(job);
            ++checked;
            if (checked % 32 == 0) {
                tracker.set_stage(stage, "正在校验已有文件… " +
                                             std::to_string(checked) + "/" +
                                             std::to_string(jobs.size()));
            }
        }
    }
    tracker.set_completed(valid_files, valid_bytes);

    if (pending.empty()) {
        tracker.set_stage(stage, "文件已完整，无需下载");
        return {};
    }

    tracker.set_stage(stage, "正在下载 " + std::to_string(pending.size()) +
                                 " 个文件…");

    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex error_mutex;
    std::optional<Error> first_error;

    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;
    const size_t worker_count = (std::max)(
        static_cast<size_t>(1),
        (std::min)({static_cast<size_t>(8), pending.size(),
                    static_cast<size_t>(hardware)}));

    const auto worker = [&] {
        try {
            while (!stop.load(std::memory_order_relaxed) &&
                   !cancel.load(std::memory_order_relaxed)) {
                const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= pending.size()) break;

                const DownloadJob& job = pending[index];
                bool success = false;
                std::string last_error = "没有可用的下载源";

                for (const size_t source : selector.order()) {
                    if (stop.load(std::memory_order_relaxed) ||
                        cancel.load(std::memory_order_relaxed)) {
                        break;
                    }

                    for (int attempt = 0; attempt < 2; ++attempt) {
                        if (stop.load(std::memory_order_relaxed) ||
                            cancel.load(std::memory_order_relaxed)) {
                            break;
                        }

                        const std::string url = selector.map_url(source, job.url);
                        if (url.empty()) {
                            last_error = "下载地址为空";
                            break;
                        }

                        tracker.set_file(job.destination, selector.name(source));
                        unsigned long long attempt_bytes = 0;
                        const auto started = std::chrono::steady_clock::now();

                        HttpOptions options;
                        options.expected_size = job.size;
                        options.timeout_seconds = 30;
                        options.min_speed_bps = selector.min_speed(source);
                        options.speed_probe_seconds =
                            selector.is_official(source) ? 4 : 6;

                        const auto result = http_download(
                            url, job.destination, options,
                            [&](const HttpProgress& progress) {
                                if (progress.downloaded > attempt_bytes) {
                                    const unsigned long long delta =
                                        progress.downloaded - attempt_bytes;
                                    attempt_bytes = progress.downloaded;
                                    tracker.add_bytes(
                                        static_cast<long long>(delta));
                                }
                            },
                            &cancel);

                        if (!result) {
                            if (attempt_bytes > 0) {
                                tracker.add_bytes(
                                    -static_cast<long long>(attempt_bytes));
                                attempt_bytes = 0;
                            }
                            last_error = result.error().to_string();
                            const bool slow = result.error().code == 490;
                            selector.record_failure(source, slow);
                            if (cancel.load(std::memory_order_relaxed)) break;
                            if (slow || result.error().code == 403 ||
                                result.error().code == 404) {
                                break;
                            }
                            continue;
                        }

                        Log::debug("HTTP 成功，开始校验：" +
                                   path_to_utf8(job.destination));
                        std::error_code ec;
                        bool valid = true;
                        if (job.size > 0) {
                            const auto size =
                                std::filesystem::file_size(job.destination, ec);
                            if (ec || size != job.size) valid = false;
                        }
                        Log::debug("文件大小校验完成");
                        if (valid && !job.sha1.empty()) {
                            auto hash = sha1_file(job.destination);
                            if (!hash || !equals_hex(hash.value(), job.sha1)) {
                                valid = false;
                            }
                        }
                        Log::debug("SHA-1 校验完成");
                        if (!valid) {
                            std::filesystem::remove(job.destination, ec);
                            if (attempt_bytes > 0) {
                                tracker.add_bytes(
                                    -static_cast<long long>(attempt_bytes));
                                attempt_bytes = 0;
                            }
                            last_error = "文件大小或 SHA-1 校验失败";
                            selector.record_failure(source, false);
                            break;
                        }

                        const double seconds = (std::max)(
                            0.001,
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - started)
                                .count());
                        const unsigned long long bytes =
                            attempt_bytes > 0 ? attempt_bytes : job.size;
                        selector.record_success(
                            source, static_cast<double>(bytes) / seconds);
                        tracker.file_done();
                        Log::debug("文件完成：" + path_to_utf8(job.destination));
                        success = true;
                        break;
                    }
                    if (success) break;
                }

                if (!success && !cancel.load(std::memory_order_relaxed)) {
                    std::lock_guard lock(error_mutex);
                    if (!first_error) {
                        first_error = Error::make(
                            80, "文件下载失败",
                            path_to_utf8(job.destination) + ": " + last_error);
                    }
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        } catch (const std::exception& ex) {
            std::lock_guard lock(error_mutex);
            if (!first_error) {
                first_error = Error::make(81, "下载线程异常", ex.what());
            }
            stop.store(true, std::memory_order_relaxed);
        }
    };

    std::atomic<bool> monitor_stop{false};
    std::thread monitor;
    if (total_bytes >= 8ULL * 1024 * 1024 && pending.size() >= 4) {
        monitor = std::thread([&] {
            auto base_time = std::chrono::steady_clock::now();
            auto base_bytes = tracker.bytes();
            while (!stop.load(std::memory_order_relaxed) &&
                   !cancel.load(std::memory_order_relaxed) &&
                   !monitor_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                const auto now = std::chrono::steady_clock::now();
                const double elapsed =
                    std::chrono::duration<double>(now - base_time).count();
                if (elapsed < 8.0) continue;

                const size_t preferred = selector.preferred();
                const unsigned long long bytes = tracker.bytes();
                const double speed = bytes > base_bytes
                                         ? static_cast<double>(bytes - base_bytes) /
                                               elapsed
                                         : 0.0;
                if (speed < static_cast<double>(selector.min_speed(preferred))) {
                    Log::warn("整体下载速度偏慢（" +
                              std::to_string(static_cast<unsigned long long>(
                                  speed / 1024)) +
                              " KiB/s），切换下载源");
                    selector.record_failure(preferred, true);
                    base_time = now;
                    base_bytes = bytes;
                }
            }
        });
    }

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();

    monitor_stop = true;
    if (monitor.joinable()) monitor.join();

    if (cancel.load(std::memory_order_relaxed)) {
        return Error::make(499, "下载已取消");
    }
    if (first_error) return *first_error;

    tracker.set_stage(stage, "完成");
    return {};
}

Result<void> fetch_manifest_network(DownloadSource source,
                                    const std::string& official_url,
                                    VersionManifest& manifest,
                                    std::string& last_error) {
    std::vector<std::string> urls;
    if (source != DownloadSource::Mirror) urls.push_back(official_url);
    if (source != DownloadSource::Official) {
        for (const auto& mirror : available_mirrors()) {
            if (!mirror.manifest_url.empty()) urls.push_back(mirror.manifest_url);
        }
    }

    std::set<std::string> seen;
    for (const auto& url : urls) {
        if (!seen.insert(url).second) continue;
        HttpOptions options;
        options.timeout_seconds = 20;
        auto body = http_get(url, options);
        if (!body) {
            last_error = url + ": " + body.error().to_string();
            continue;
        }
        try {
            auto parsed = parse_manifest_json(nlohmann::json::parse(body.value()));
            if (!parsed) {
                last_error = url + ": " + parsed.error().to_string();
                continue;
            }
            manifest = std::move(parsed).value();
            save_manifest_cache(body.value());
            Log::info("版本清单已更新：" + url);
            return {};
        } catch (const std::exception& ex) {
            last_error = url + ": " + ex.what();
        }
    }
    return Error::make(10, "无法获取版本清单", last_error);
}

std::vector<DownloadJob> library_jobs(const ResolvedVersion& version,
                                      const std::filesystem::path& mc_dir,
                                      std::vector<std::string>& unavailable) {
    std::vector<DownloadJob> jobs;
    FeatureSet features;
    features.custom_resolution = true;

    const auto add = [&](const DownloadInfo& info,
                         const std::string& fallback_path,
                         const std::string& fallback_url) {
        std::string path = info.path.empty() ? fallback_path : info.path;
        std::string url = info.url.empty() ? fallback_url : info.url;
        if (path.empty()) return;
        if (!safe_relative(path)) {
            Log::warn("忽略不安全的库路径：" + path);
            return;
        }
        if (url.empty()) {
            const std::filesystem::path full =
                mc_dir / L"libraries" / path_from_utf8(path);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(full, ec)) {
                unavailable.push_back(path);
            }
            return;
        }
        jobs.push_back(DownloadJob{
            url, mc_dir / L"libraries" / path_from_utf8(path), info.sha1,
            info.size});
    };

    for (const Library& library : version.merged.libraries) {
        if (!rules_allow(library.rules, features)) continue;

        const auto natives = library.natives_artifact();
        if (natives) {
            add(*natives, "", "");
        }

        if (library.artifact) {
            add(*library.artifact, library.artifact_path(),
                library.artifact_url());
        } else if (!natives) {
            DownloadInfo synthesized;
            synthesized.path = library.artifact_path();
            synthesized.url = library.artifact_url();
            if (!synthesized.path.empty() && !synthesized.url.empty()) {
                add(synthesized, "", "");
            }
        }
    }
    return jobs;
}

Result<void> download_assets(const Version& version,
                             const std::filesystem::path& mc_dir,
                             const std::string& stage_prefix,
                             SourceSelector& selector,
                             ProgressTracker& tracker,
                             DownloadCancelToken& cancel) {
    if (!version.asset_index || version.asset_index->url.empty()) {
        Log::warn("版本没有 assetIndex，跳过资源文件");
        return {};
    }

    const std::string index_id =
        version.asset_index_id.value_or(version.assets.value_or("legacy"));
    if (index_id.empty()) {
        Log::warn("assetIndex 缺少 id，跳过资源文件");
        return {};
    }

    const std::filesystem::path index_path =
        mc_dir / L"assets" / L"indexes" / path_from_utf8(index_id + ".json");
    std::vector<DownloadJob> index_job{
        DownloadJob{version.asset_index->url, index_path,
                    version.asset_index->sha1, version.asset_index->size}};
    CL_TRY_VOID(run_jobs(stage_prefix + "索引", std::move(index_job), selector,
                         tracker, cancel));

    auto index_json = load_json(index_path);
    if (!index_json) return index_json.error();
    const auto objects_it = index_json.value().find("objects");
    if (objects_it == index_json.value().end() || !objects_it->is_object()) {
        return Error::make(82, "资源索引缺少 objects",
                           path_to_utf8(index_path));
    }

    std::vector<DownloadJob> jobs;
    jobs.reserve(objects_it->size());
    for (auto it = objects_it->begin(); it != objects_it->end(); ++it) {
        if (!it.value().is_object()) continue;
        std::string hash = lower_copy(json_string(it.value(), "hash"));
        if (!is_sha1(hash)) continue;
        const unsigned long long size = json_ull(it.value(), "size");
        const std::string folder = hash.substr(0, 2);
        const std::filesystem::path destination =
            mc_dir / L"assets" / L"objects" / path_from_utf8(folder) /
            path_from_utf8(hash);
        const std::string url = std::string(kResourcesBase) + folder + "/" + hash;
        jobs.push_back(DownloadJob{url, destination, hash, size});
    }

    CL_TRY_VOID(run_jobs(stage_prefix, std::move(jobs), selector, tracker,
                         cancel));

    const bool legacy = index_id == "legacy" ||
                        version.assets.value_or("") == "pre-1.6";
    if (legacy) {
        const std::filesystem::path virtual_dir =
            mc_dir / L"assets" / L"virtual" / L"legacy";
        int copied = 0;
        for (auto it = objects_it->begin(); it != objects_it->end(); ++it) {
            if (!it.value().is_object()) continue;
            const std::string key = it.key();
            if (!safe_relative(key)) continue;
            const std::string hash = lower_copy(json_string(it.value(), "hash"));
            if (!is_sha1(hash)) continue;

            const std::filesystem::path source =
                mc_dir / L"assets" / L"objects" / path_from_utf8(hash.substr(0, 2)) /
                path_from_utf8(hash);
            const std::filesystem::path destination =
                virtual_dir / path_from_utf8(key);
            std::error_code ec;
            if (std::filesystem::is_regular_file(destination, ec)) continue;
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (ec) continue;
            if (!::CreateHardLinkW(destination.c_str(), source.c_str(), nullptr)) {
                std::filesystem::copy_file(source, destination,
                                           std::filesystem::copy_options::
                                               overwrite_existing,
                                           ec);
            }
            if (!ec) ++copied;
        }
        if (copied > 0) {
            Log::info("已生成 legacy 虚拟资源：" + std::to_string(copied) + " 个");
        }
    }
    return {};
}

std::string request_name(const DownloadRequest& request) {
    return request.instance_name.empty() ? request.version_id
                                         : request.instance_name;
}

Result<void> install_version_impl(const DownloadRequest& request,
                                  const DownloadProgressCallback& progress,
                                  DownloadCancelToken& cancel) {
    if (request.version_id.empty()) {
        return Error::make(20, "没有指定 Minecraft 版本");
    }
    if (!std::filesystem::is_directory(request.mc_dir)) {
        return Error::make(21, "游戏目录不存在", path_to_utf8(request.mc_dir));
    }

    const std::string name = request_name(request);
    if (!is_valid_instance_name(name)) {
        return Error::make(22, "安装名称不合法", name);
    }

    const std::filesystem::path versions_dir = request.mc_dir / L"versions";
    const std::filesystem::path version_dir = versions_dir / path_from_utf8(name);
    const std::filesystem::path json_path =
        version_dir / (path_from_utf8(name).wstring() + L".json");

    std::error_code ec;
    if (std::filesystem::is_regular_file(json_path, ec)) {
        return Error::make(23, "同名版本已存在",
                           name + "，可以使用“修复”补齐文件");
    }
    std::filesystem::create_directories(version_dir, ec);
    if (ec) {
        return Error::make(24, "无法创建版本目录",
                           path_to_utf8(version_dir) + ": " + ec.message());
    }

    ProgressTracker tracker(progress);
    tracker.set_stage("准备", "正在获取版本清单…");

    auto manifest_result = fetch_version_manifest(request.source, false);
    if (!manifest_result) return manifest_result.error();
    const VersionManifest manifest = std::move(manifest_result).value();

    const VersionSummary* found = nullptr;
    for (const auto& entry : manifest.versions) {
        if (entry.id == request.version_id) {
            found = &entry;
            break;
        }
    }
    if (found == nullptr) {
        return Error::make(25, "版本清单中没有这个版本", request.version_id);
    }
    if (found->url.empty()) {
        return Error::make(26, "版本没有 JSON 下载地址", request.version_id);
    }

    SourceSelector selector(request.source);

    const std::filesystem::path raw_json = version_dir / L"version.json.download";
    std::vector<DownloadJob> version_job{
        DownloadJob{found->url, raw_json, found->sha1, found->size}};
    CL_TRY_VOID(run_jobs("版本 JSON", std::move(version_job), selector, tracker,
                         cancel));

    auto loaded = load_json(raw_json);
    if (!loaded) {
        std::filesystem::remove(raw_json, ec);
        return loaded.error();
    }
    nlohmann::json json = std::move(loaded).value();
    if (!json.is_object()) {
        std::filesystem::remove(raw_json, ec);
        return Error::make(27, "版本 JSON 不是对象", request.version_id);
    }

    json["id"] = name;
    if (json.contains("jar")) json["jar"] = name;
    CL_TRY_VOID(write_json_file(json_path, json));
    std::filesystem::remove(raw_json, ec);

    const Version version = parse_version(json);
    if (!version.client || version.client->url.empty()) {
        return Error::make(28, "版本没有客户端下载地址", request.version_id);
    }

    const std::filesystem::path client_jar =
        version_dir / (path_from_utf8(name).wstring() + L".jar");
    std::vector<DownloadJob> client_job{
        DownloadJob{version.client->url, client_jar, version.client->sha1,
                    version.client->size}};
    CL_TRY_VOID(run_jobs("客户端", std::move(client_job), selector, tracker,
                         cancel));

    std::vector<std::string> unavailable;
    ResolvedVersion resolved;
    resolved.id = name;
    resolved.vanilla_id = name;
    resolved.jar_id = name;
    resolved.merged = version;
    auto library_jobs_list = library_jobs(resolved, request.mc_dir, unavailable);
    if (!unavailable.empty()) {
        std::string detail = "有 " + std::to_string(unavailable.size()) +
                             " 个库文件没有下载地址";
        for (size_t i = 0; i < unavailable.size() && i < 5; ++i) {
            detail += "\n  " + unavailable[i];
        }
        return Error::make(29, detail);
    }
    CL_TRY_VOID(run_jobs("库文件", std::move(library_jobs_list), selector,
                         tracker, cancel));

    CL_TRY_VOID(download_assets(version, request.mc_dir, "游戏资源", selector,
                                tracker, cancel));

    tracker.set_stage("完成", "安装完成：" + name);
    Log::info("原版安装完成：" + name + "（" + request.version_id + "）");
    return {};
}

} // namespace

DownloadSource parse_download_source(std::string_view text) {
    const std::string value = lower_copy(std::string(text));
    if (value == "official" || value == "mojang") return DownloadSource::Official;
    if (value == "mirror" || value == "mirrors" || value == "bmclapi") {
        return DownloadSource::Mirror;
    }
    return DownloadSource::Auto;
}

std::string download_source_name(DownloadSource source) {
    switch (source) {
        case DownloadSource::Official: return "official";
        case DownloadSource::Mirror: return "mirror";
        default: return "auto";
    }
}

Result<VersionManifest> fetch_version_manifest(DownloadSource source,
                                               bool force_refresh) {
    std::lock_guard lock(g_manifest_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!force_refresh && g_manifest &&
        now - g_manifest_time < kManifestTtl) {
        return *g_manifest;
    }

    std::string last_error;
    VersionManifest manifest;
    auto network = fetch_manifest_network(source, kOfficialManifest, manifest,
                                           last_error);
    if (network) {
        g_manifest = manifest;
        g_manifest_time = now;
        return manifest;
    }

    auto cached = load_manifest_cache();
    if (cached) {
        Log::warn("在线获取版本清单失败，使用本地缓存：" +
                  network.error().to_string());
        g_manifest = cached.value();
        g_manifest_time = now;
        return cached.value();
    }
    return network.error();
}

std::vector<MirrorSource> available_mirrors() {
    return load_mirrors();
}

Result<void> refresh_mirrors(const std::string& url) {
    return refresh_mirror_list(url);
}

Result<void> install_version(const DownloadRequest& request,
                             const DownloadProgressCallback& progress,
                             DownloadCancelToken& cancel) {
    return install_version_impl(request, progress, cancel);
}

Result<void> repair_version(const DownloadRequest& request,
                            const DownloadProgressCallback& progress,
                            DownloadCancelToken& cancel) {
    if (request.version_id.empty()) {
        return Error::make(30, "没有指定版本");
    }

    const std::filesystem::path versions_dir = request.mc_dir / L"versions";
    const std::filesystem::path id_path = path_from_utf8(request.version_id);
    const std::filesystem::path version_dir = versions_dir / id_path;
    const std::filesystem::path json_path =
        version_dir / (id_path.wstring() + L".json");

    std::error_code ec;
    if (!std::filesystem::is_regular_file(json_path, ec)) {
        DownloadRequest install = request;
        install.instance_name = request.version_id;
        return install_version_impl(install, progress, cancel);
    }

    ProgressTracker tracker(progress);
    tracker.set_stage("准备", "正在解析版本…");

    auto resolved_result = resolve_version(versions_dir, request.version_id);
    if (!resolved_result) return resolved_result.error();
    ResolvedVersion resolved = std::move(resolved_result).value();

    SourceSelector selector(request.source);
    const std::string jar_id = resolved.jar_id;
    const std::filesystem::path jar_path =
        versions_dir / path_from_utf8(jar_id) /
        (path_from_utf8(jar_id).wstring() + L".jar");

    if (resolved.merged.client && !resolved.merged.client->url.empty()) {
        std::vector<DownloadJob> client_job{
            DownloadJob{resolved.merged.client->url, jar_path,
                        resolved.merged.client->sha1,
                        resolved.merged.client->size}};
        CL_TRY_VOID(run_jobs("客户端", std::move(client_job), selector, tracker,
                             cancel));
    } else if (!std::filesystem::is_regular_file(jar_path, ec)) {
        return Error::make(31, "缺少客户端 jar，且版本没有下载地址",
                           path_to_utf8(jar_path));
    }

    std::vector<std::string> unavailable;
    auto jobs = library_jobs(resolved, request.mc_dir, unavailable);
    CL_TRY_VOID(run_jobs("库文件", std::move(jobs), selector, tracker, cancel));

    CL_TRY_VOID(download_assets(resolved.merged, request.mc_dir, "游戏资源",
                                selector, tracker, cancel));

    if (!unavailable.empty()) {
        std::string detail = "有 " + std::to_string(unavailable.size()) +
                             " 个库文件没有下载地址";
        for (size_t i = 0; i < unavailable.size() && i < 5; ++i) {
            detail += "\n  " + unavailable[i];
        }
        return Error::make(32, detail);
    }

    tracker.set_stage("完成", "修复完成：" + request.version_id);
    Log::info("实例修复完成：" + request.version_id);
    return {};
}

} // namespace cl::mc
