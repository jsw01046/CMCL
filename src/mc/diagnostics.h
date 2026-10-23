#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cl::mc {

struct AnalysisContext {
    std::string game_version;
    std::string loader;
    std::string java_version;
    int java_major = 0;
    int max_memory_mb = 0;
};

struct AnalysisFinding {
    std::string id;        // 稳定的特征 ID，例如 memory.heap
    std::string category;  // memory / java / loader / dependency / conflict / mixin ...
    std::string title;
    std::string detail;
    std::string severity;  // "info" / "warn" / "error"
    std::string time;      // 日志里的时间戳（如果有）
    std::string mod_a;     // 主要模组 / 缺失的前置
    std::string mod_b;     // 冲突方 / 请求依赖的模组
    std::string event;     // 命中的原始日志行
    std::string evidence;  // 命中行附近的上下文
    std::vector<std::string> suggestions;
};

struct RunSummary {
    std::string title;
    std::string detail;
    std::vector<std::string> suggestions;
    std::string severity;  // "info" / "warn" / "error"
    bool normal = false;
    bool unknown = false;
    AnalysisContext context;
    std::vector<AnalysisFinding> findings;
};

// 根据游戏输出和退出码生成面向玩家的总结。
RunSummary analyze_run(int exit_code, const std::vector<std::string>& lines);
RunSummary analyze_run(int exit_code, const std::vector<std::string>& lines,
                       const AnalysisContext& context);

// 给 CLI / 启动器日志使用的多行文本。
std::string format_run_summary(const RunSummary& summary);

// 读取日志文件末尾若干行，供总结补充控制台里没有的信息。
std::vector<std::string> read_log_tail(const std::filesystem::path& path,
                                       size_t max_lines = 800);

} // namespace cl::mc
