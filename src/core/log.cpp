#include "core/log.h"

#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/encoding.h"

namespace cl {

namespace {

std::mutex g_mutex;
std::ofstream g_file;
bool g_debug = false;
bool g_initialized = false;

const char* level_name(const char* level) { return level; }

std::string archive_timestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04u%02u%02u-%02u%02u%02u-%03u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond, st.wMilliseconds);
    return buffer;
}

// 用 zlib 的 gzip 封装写 .gz；全部走 std::ofstream，避免 zlib 的 ANSI 路径问题。
bool gzip_file(const std::filesystem::path& source,
               const std::filesystem::path& dest) {
    std::ifstream in(source, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    std::vector<char> input(64 * 1024);
    std::vector<char> output(64 * 1024);
    int flush = Z_NO_FLUSH;
    bool ok = true;

    do {
        in.read(input.data(), static_cast<std::streamsize>(input.size()));
        const auto count = in.gcount();
        if (in.bad()) {
            ok = false;
            break;
        }
        if (count == 0) flush = Z_FINISH;

        stream.next_in = reinterpret_cast<Bytef*>(input.data());
        stream.avail_in = static_cast<uInt>(count);
        do {
            stream.next_out = reinterpret_cast<Bytef*>(output.data());
            stream.avail_out = static_cast<uInt>(output.size());

            const int result = deflate(&stream, flush);
            if (result == Z_STREAM_ERROR) {
                ok = false;
                break;
            }

            const size_t produced = output.size() - stream.avail_out;
            out.write(output.data(), static_cast<std::streamsize>(produced));
            if (!out) {
                ok = false;
                break;
            }
            if (result == Z_STREAM_END) break;
        } while (stream.avail_out == 0);
    } while (ok && flush != Z_FINISH);

    deflateEnd(&stream);
    out.close();

    if (!ok || !out) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        return false;
    }
    return true;
}

// 保留最新的 keep_count 个归档，并删除超过 days 天的归档。
int cleanup_archives(const std::filesystem::path& directory,
                     const std::wstring& stem, int keep_count, int days) {
    struct Archive {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
    };

    std::vector<Archive> archives;
    std::error_code ec;
    const std::wstring prefix = stem + L"-";
    for (std::filesystem::directory_iterator it(directory, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::wstring name = it->path().filename().wstring();
        if (name.rfind(prefix, 0) != 0) continue;
        // 前缀后必须紧跟时间戳数字，避免 craftlauncher 的清理误删
        // craftlauncher-ui 的归档。
        if (name.size() <= prefix.size() ||
            !std::iswdigit(name[prefix.size()])) {
            continue;
        }
        if (!name.ends_with(L".log.gz")) continue;
        archives.push_back({it->path(), it->last_write_time(ec)});
    }

    std::sort(archives.begin(), archives.end(),
              [](const Archive& a, const Archive& b) {
                  return a.time > b.time;
              });

    const auto cutoff = std::filesystem::file_time_type::clock::now() -
                        std::chrono::hours(24 * days);
    int removed = 0;
    for (size_t i = 0; i < archives.size(); ++i) {
        const bool keep = i < static_cast<size_t>(keep_count) &&
                          archives[i].time >= cutoff;
        if (keep) continue;
        std::error_code remove_ec;
        if (std::filesystem::remove(archives[i].path, remove_ec)) ++removed;
    }
    return removed;
}

std::string rotate_log_file(const std::filesystem::path& file) {
    std::string note;
    std::error_code ec;
    const std::filesystem::path directory =
        file.parent_path().empty() ? std::filesystem::current_path()
                                   : file.parent_path();

    if (std::filesystem::is_regular_file(file, ec)) {
        const auto size = std::filesystem::file_size(file, ec);
        if (!ec && size == 0) {
            std::filesystem::remove(file, ec);
        } else if (!ec && size > 0) {
            const std::filesystem::path archive =
                directory /
                (file.stem().wstring() + L"-" + to_wide(archive_timestamp()) +
                 L".log.gz");
            if (gzip_file(file, archive)) {
                std::filesystem::remove(file, ec);
                note = "上次日志已压缩：" + path_to_utf8(archive.filename());
            } else {
                note = "上次日志压缩失败，将继续写入原文件";
            }
        }
    }

    const int removed =
        cleanup_archives(directory, file.stem().wstring(), 1, 7);
    if (removed > 0) {
        if (!note.empty()) note += "；";
        note += "已清理 " + std::to_string(removed) +
                " 个旧日志归档（仅保留最近一份，超过 7 天的也会删除）";
    }
    return note;
}

bool is_old_log(const std::wstring& name) {
    if (name == L"latest.log") return false;
    return name.ends_with(L".log.gz") || name.ends_with(L".log");
}

} // namespace

void Log::init(const std::filesystem::path& file, bool debug_enabled) {
    std::error_code ec;
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }

    // 先压缩上一次的日志，再打开新文件。
    const std::string rotation_note = rotate_log_file(file);

    std::lock_guard lock(g_mutex);
    g_debug = debug_enabled;
    if (g_file.is_open()) g_file.close();
    g_file.open(file, std::ios::binary | std::ios::app);
    g_initialized = g_file.is_open();
    if (g_initialized) {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        char header[128];
        std::snprintf(header, sizeof(header),
                      "===== CMCL log %04u-%02u-%02u %02u:%02u:%02u =====\r\n",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        g_file << header;
        if (!rotation_note.empty()) {
            g_file << "[rotation] " << rotation_note << "\r\n";
        }
        g_file.flush();
    }
}

void Log::shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
    g_initialized = false;
}

bool Log::debug_enabled() { return g_debug; }

void Log::debug(std::string_view message) {
    if (g_debug) write("DEBUG", message);
}

void Log::info(std::string_view message) { write("INFO", message); }

void Log::warn(std::string_view message) { write("WARN", message); }

void Log::error(std::string_view message) { write("ERROR", message); }

void Log::write(const char* level, std::string_view message) {
    std::lock_guard lock(g_mutex);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "[%02u:%02u:%02u.%03u] [%s] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  level_name(level));

    if (g_initialized) {
        g_file << prefix;
        g_file.write(message.data(), static_cast<std::streamsize>(message.size()));
        g_file << "\r\n";
        g_file.flush();
    }

    // 警告和错误同时回显到控制台，方便 CLI 使用。
    if (level[0] == 'W' || level[0] == 'E') {
        std::fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(message.size()), message.data());
    } else if (g_debug) {
        std::fprintf(stderr, "%s%.*s\n", prefix, static_cast<int>(message.size()), message.data());
    }
}

int cleanup_game_logs(const std::filesystem::path& game_dir, int days) {
    const auto cutoff = std::filesystem::file_time_type::clock::now() -
                        std::chrono::hours(24 * days);
    int removed = 0;

    const auto clean_directory = [&](const std::filesystem::path& directory,
                                     const auto& match) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(directory, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::wstring name = it->path().filename().wstring();
            if (!match(name)) continue;
            const auto time = it->last_write_time(ec);
            if (ec || time >= cutoff) continue;
            std::error_code remove_ec;
            if (std::filesystem::remove(it->path(), remove_ec)) ++removed;
        }
    };

    clean_directory(game_dir / L"logs", is_old_log);
    clean_directory(game_dir / L"crash-reports",
                    [](const std::wstring& name) {
                        return name.rfind(L"crash-", 0) == 0 &&
                               name.ends_with(L".txt");
                    });
    return removed;
}

} // namespace cl
