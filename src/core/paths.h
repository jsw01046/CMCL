#pragma once

#include <filesystem>

#include "core/result.h"

namespace cl {

std::filesystem::path exe_path();
std::filesystem::path exe_dir();
std::filesystem::path default_mc_dir();
std::filesystem::path local_app_data_dir();

// 确保启动器自己的数据目录存在（cache / jdk / natives）。
// 目录丢失时自动重建，不会删除已有内容。
Result<void> ensure_app_dirs();

} // namespace cl
