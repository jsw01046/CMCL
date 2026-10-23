#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/process.h"
#include "core/result.h"
#include "mc/version.h"

namespace cl::mc {

struct LaunchOptions {
    std::filesystem::path mc_dir;
    std::string version_id;

    std::string player_name = "Player";
    std::string uuid;  // 为空时按离线 UUID 生成
    std::string access_token = "0";
    std::string user_type = "legacy";

    std::optional<std::filesystem::path> java_exe;
    int min_memory_mb = 512;
    int max_memory_mb = 4096;
    int width = 854;
    int height = 480;
    bool demo = false;
    bool fullscreen = false;
    bool isolate = false;  // 隔离存档/模组/配置到 versions/<id>
    bool isolate_saves = true;  // 只隔离存档，其它数据仍共享
    std::string server;  // "host" 或 "host:port"

    std::vector<std::string> extra_jvm_args;
    std::vector<std::string> extra_game_args;
};

struct PreparedLaunch {
    ResolvedVersion version;
    std::filesystem::path java_exe;
    std::filesystem::path game_dir;
    std::filesystem::path saves_dir;
    std::filesystem::path natives_dir;
    std::string main_class;
    std::string classpath;
    std::string java_version;
    int java_major = 0;
    int java_bits = 64;
    std::vector<std::string> jvm_args;
    std::vector<std::string> game_args;
    std::wstring command_line;
    // 游戏输出解码用的回退代码页（Java 7/8 中文为 936，Java 18+ 为 UTF-8）。
    unsigned int output_code_page = 65001;
};

// 解析版本、选择 Java、校验库文件、释放 natives、拼出完整命令行。
// extract_natives=false 时只做检查，不真的解压（供 inspect 使用）。
Result<PreparedLaunch> prepare_launch(const LaunchOptions& options,
                                      bool extract_natives = true);

Result<std::unique_ptr<Process>> launch_prepared(
    const PreparedLaunch& prepared, Process::OutputFn on_output);

} // namespace cl::mc
