#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/result.h"

namespace cl {

// 解压 zip 到目录。
// 当前实现调用 Windows 自带的 tar.exe（Windows 10 1803+ 内置 bsdtar，
// 可以直接读 zip）。M3 会替换成 miniz/libzip 的内嵌实现。
Result<void> extract_zip(const std::filesystem::path& archive,
                         const std::filesystem::path& dest,
                         const std::vector<std::string>& excludes = {});

} // namespace cl
