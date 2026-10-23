#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cl {

// Windows 内部一律用 UTF-16（std::filesystem::path），JSON / URL / 日志用 UTF-8。
std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

std::filesystem::path path_from_utf8(std::string_view s);
std::string path_to_utf8(const std::filesystem::path& p);

inline constexpr unsigned int kCodePageUtf8 = 65001;
inline constexpr unsigned int kCodePageGbk = 936;

bool is_valid_utf8(std::string_view bytes);

// Java 7/8 在中文系统上通常输出 GBK，Java 18+ 默认 UTF-8。
// 先按 UTF-8 校验，失败再按 fallback_code_page 解码。
std::string decode_to_utf8(std::string_view bytes, unsigned int fallback_code_page);

bool is_chinese_locale();
unsigned int default_fallback_code_page();

} // namespace cl
