#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/result.h"
#include "mc/version.h"

namespace cl::mc {

struct JavaInfo {
    std::filesystem::path home;
    std::filesystem::path exe;
    std::string version;
    int major = 0;
    int bits = 64;
    bool managed = false;  // 由 CMCL 下载并管理
};

Result<JavaInfo> probe_java(const std::filesystem::path& exe);

// 扫描 JAVA_HOME、PATH、常见安装目录，以及启动器自带的 runtime。
// 结果缓存 60 秒；force_refresh=true 时重新探测（UI 的“检测 Java”按钮）。
std::vector<JavaInfo> detect_java(const std::filesystem::path& mc_dir = {},
                                  bool force_refresh = false);

// 该版本要求的最低 Java 主版本（Forge 1.6.1–1.7.2 返回 7）。
int required_java_major(const ResolvedVersion& version);

// 按版本 JSON 的 javaVersion.majorVersion 选择 Java；没有精确匹配时选最接近的更高版本。
// 低于最低要求时直接报错，不会退回旧 Java。
Result<JavaInfo> select_java(
    const ResolvedVersion& version,
    const std::optional<std::filesystem::path>& explicit_exe,
    const std::filesystem::path& mc_dir = {});

} // namespace cl::mc
