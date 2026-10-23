#include "core/encoding.h"

#include <windows.h>

namespace cl {

std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                                           static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()),
                          out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                           static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                          out.data(), size);
    return out;
}

std::filesystem::path path_from_utf8(std::string_view s) {
    return std::filesystem::path(to_wide(s));
}

std::string path_to_utf8(const std::filesystem::path& p) {
    return to_utf8(p.wstring());
}

bool is_valid_utf8(std::string_view bytes) {
    if (bytes.empty()) return true;
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                 static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

std::string decode_to_utf8(std::string_view bytes, unsigned int fallback_code_page) {
    if (bytes.empty()) return {};
    if (is_valid_utf8(bytes)) return std::string(bytes);

    const int size = ::MultiByteToWideChar(
        fallback_code_page, 0, bytes.data(), static_cast<int>(bytes.size()),
        nullptr, 0);
    if (size <= 0) return std::string(bytes);

    std::wstring wide(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(fallback_code_page, 0, bytes.data(),
                          static_cast<int>(bytes.size()), wide.data(), size);
    return to_utf8(wide);
}

bool is_chinese_locale() {
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
}

unsigned int default_fallback_code_page() {
    // 中文 Java 7/8 的日志基本是 GBK，即使系统 ACP 被设成了 65001。
    return is_chinese_locale() ? kCodePageGbk : ::GetACP();
}

} // namespace cl
