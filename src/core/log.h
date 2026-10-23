#pragma once

#include <filesystem>
#include <string_view>

namespace cl {

class Log {
public:
    static void init(const std::filesystem::path& file, bool debug_enabled);
    static void shutdown();

    static void debug(std::string_view message);
    static void info(std::string_view message);
    static void warn(std::string_view message);
    static void error(std::string_view message);

    [[nodiscard]] static bool debug_enabled();

private:
    static void write(const char* level, std::string_view message);
};

// 删除游戏目录 logs/ 和 crash-reports/ 中超过 days 天的旧日志，返回删除数量。
int cleanup_game_logs(const std::filesystem::path& game_dir, int days = 7);

} // namespace cl
