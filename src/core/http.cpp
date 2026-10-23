#include "core/http.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include "core/encoding.h"
#include "core/log.h"

namespace cl {

namespace {

constexpr wchar_t kUserAgent[] = L"CMCL/0.5.1";

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    ~WinHttpHandle() { reset(); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.release()) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }

    void reset(HINTERNET handle = nullptr) {
        if (handle_ != nullptr) ::WinHttpCloseHandle(handle_);
        handle_ = handle;
    }

    [[nodiscard]] HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    HINTERNET release() {
        HINTERNET handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    HINTERNET handle_ = nullptr;
};

struct SessionHolder {
    HINTERNET handle = nullptr;
    ~SessionHolder() {
        if (handle != nullptr) ::WinHttpCloseHandle(handle);
    }
};

HINTERNET thread_session() {
    thread_local SessionHolder holder{[] {
        HINTERNET session = ::WinHttpOpen(
            kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) {
            // 老系统没有 AUTOMATIC_PROXY，退回默认代理设置。
            session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session != nullptr) {
            ::WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
        }
        return session;
    }()};
    return holder.handle;
}

std::string win32_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "Win32 错误 " + std::to_string(code);
    }
    std::wstring message(buffer, length);
    ::LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
        message.pop_back();
    }
    return to_utf8(message);
}

Error win32_failure(const char* what, const std::string& url) {
    const DWORD code = ::GetLastError();
    return Error::make(static_cast<int>(code), what,
                       url + ": " + win32_error(code));
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

Result<ParsedUrl> parse_url(const std::string& url) {
    const std::wstring wide = to_wide(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);

    wchar_t host[512]{};
    wchar_t path[8192]{};
    wchar_t extra[8192]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));

    if (!::WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
                           &components)) {
        return Error::make(1, "URL 无效", url);
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS &&
        components.nScheme != INTERNET_SCHEME_HTTP) {
        return Error::make(2, "只支持 HTTP/HTTPS", url);
    }

    ParsedUrl parsed;
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
        parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (parsed.path.empty()) parsed.path = L"/";
    parsed.port = components.nPort;
    parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return parsed;
}

using Sink = std::function<void(const char*, size_t)>;

struct RequestResult {
    unsigned long long content_length = 0;
    unsigned long long received = 0;
};

Result<RequestResult> perform_request(
    const std::string& url, const HttpOptions& options, const Sink& sink,
    const HttpProgressCallback& progress, const std::atomic<bool>* cancel,
    const char* method = "GET", const std::string* request_body = nullptr,
    const std::string* content_type = nullptr, bool allow_error_status = false,
    int* status_out = nullptr) {
    auto parsed = parse_url(url);
    if (!parsed) return parsed.error();

    const HINTERNET session = thread_session();
    if (session == nullptr) {
        return Error::make(3, "无法初始化 WinHTTP", win32_error(::GetLastError()));
    }

    WinHttpHandle connection(
        ::WinHttpConnect(session, parsed.value().host.c_str(), parsed.value().port, 0));
    if (!connection) return win32_failure("无法连接下载服务器", url);

    const std::wstring wide_method = to_wide(method != nullptr ? method : "GET");
    WinHttpHandle request(::WinHttpOpenRequest(
        connection.get(), wide_method.c_str(), parsed.value().path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parsed.value().secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return win32_failure("无法创建 HTTP 请求", url);

    // 拼接自定义请求头（Modrinth 等 API 需要 User-Agent）
    std::wstring headers_str;
    for (const auto& [key, value] : options.headers) {
        headers_str += to_wide(key) + L": " + to_wide(value) + L"\r\n";
    }
    if (content_type != nullptr && !content_type->empty()) {
        headers_str += L"Content-Type: " + to_wide(*content_type) + L"\r\n";
    }

    const bool has_body = request_body != nullptr && !request_body->empty();
    const DWORD body_size =
        has_body ? static_cast<DWORD>(request_body->size()) : 0;
    if (!::WinHttpSendRequest(
            request.get(),
            headers_str.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                : headers_str.c_str(),
            headers_str.empty() ? 0 : static_cast<DWORD>(-1L),
            has_body ? const_cast<char*>(request_body->data())
                     : WINHTTP_NO_REQUEST_DATA,
            body_size, body_size, 0)) {
        return win32_failure("发送 HTTP 请求失败", url);
    }

    if (!::WinHttpReceiveResponse(request.get(), nullptr)) {
        return win32_failure("接收 HTTP 响应失败", url);
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!::WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        return win32_failure("读取 HTTP 状态码失败", url);
    }
    if (status_out != nullptr) *status_out = static_cast<int>(status);
    if (status < 200 || status >= 300) {
        if (!allow_error_status) {
            return Error::make(static_cast<int>(status),
                               "HTTP " + std::to_string(status), url);
        }
    }

    DWORD content_length = 0;
    DWORD length_size = sizeof(content_length);
    if (!::WinHttpQueryHeaders(
            request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_size,
            WINHTTP_NO_HEADER_INDEX)) {
        content_length = 0;
    }

    RequestResult result;
    result.content_length = content_length;
    const unsigned long long total = options.expected_size > 0
                                         ? options.expected_size
                                         : static_cast<unsigned long long>(content_length);
    if (progress) progress(HttpProgress{0, total});

    const auto started = std::chrono::steady_clock::now();
    std::array<char, 64 * 1024> buffer{};

    while (true) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            return Error::make(499, "下载已取消", url);
        }

        DWORD read = 0;
        if (!::WinHttpReadData(request.get(), buffer.data(),
                               static_cast<DWORD>(buffer.size()), &read)) {
            return win32_failure("读取下载数据失败", url);
        }
        if (read == 0) break;

        result.received += read;
        if (sink) sink(buffer.data(), read);
        if (progress) {
            progress(HttpProgress{result.received,
                                  total > 0 ? total : result.received});
        }

        if (options.min_speed_bps > 0) {
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started);
            if (elapsed.count() >= options.speed_probe_seconds) {
                const double speed =
                    static_cast<double>(result.received) / elapsed.count();
                // 文件本身很小的话不触发，避免误判。
                if (result.received > 512 * 1024 &&
                    speed < static_cast<double>(options.min_speed_bps)) {
                    char detail[128];
                    std::snprintf(detail, sizeof(detail), "%.0f KiB/s",
                                  speed / 1024.0);
                    return Error::make(490, "下载速度过慢",
                                       url + " (" + detail + ")");
                }
            }
        }
    }

    return result;
}

std::string trim_url(std::string url) {
    while (!url.empty() && url.back() == ' ') url.pop_back();
    return url;
}

} // namespace

Result<std::string> http_get(const std::string& url,
                             const HttpOptions& options) {
    std::string body;
    body.reserve(options.expected_size > 0
                     ? static_cast<size_t>(std::min<unsigned long long>(
                           options.expected_size, 16ULL * 1024 * 1024))
                     : 64 * 1024);

    Result<RequestResult> result =
        Error::make(14, "HTTP 请求失败", url);
    try {
        result = perform_request(
            trim_url(url), options,
            [&body](const char* data, size_t size) {
                if (body.size() + size > 64ULL * 1024 * 1024) {
                    throw std::length_error("HTTP 响应过大");
                }
                body.append(data, size);
            },
            {}, nullptr);
    } catch (const std::exception& ex) {
        return Error::make(14, "HTTP 响应处理失败", url + ": " + ex.what());
    }
    if (!result) return result.error();
    return body;
}

Result<HttpResponse> http_request(const std::string& url,
                                  const HttpRequestOptions& options) {
    HttpOptions base;
    base.timeout_seconds = options.timeout_seconds;
    base.headers = options.headers;

    std::string body;
    body.reserve(64 * 1024);
    int status = 0;

    Result<RequestResult> result =
        Error::make(14, "HTTP 请求失败", url);
    try {
        result = perform_request(
            trim_url(url), base,
            [&body](const char* data, size_t size) {
                if (body.size() + size > 64ULL * 1024 * 1024) {
                    throw std::length_error("HTTP 响应过大");
                }
                body.append(data, size);
            },
            {}, nullptr, options.method.c_str(), &options.body,
            &options.content_type, options.allow_error_status, &status);
    } catch (const std::exception& ex) {
        return Error::make(14, "HTTP 响应处理失败", url + ": " + ex.what());
    }
    if (!result) return result.error();
    return HttpResponse{status, std::move(body)};
}

Result<void> http_download(const std::string& url,
                           const std::filesystem::path& destination,
                           const HttpOptions& options,
                           const HttpProgressCallback& progress,
                           const std::atomic<bool>* cancel) {
    std::error_code ec;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            return Error::make(10, "无法创建下载目录",
                               path_to_utf8(destination.parent_path()) + ": " +
                                   ec.message());
        }
    }

    std::filesystem::path temporary = destination;
    temporary += L".part";

    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        return Error::make(11, "无法写入下载文件", path_to_utf8(temporary));
    }

    Result<RequestResult> result =
        Error::make(14, "HTTP 请求失败", url);
    try {
        result = perform_request(
            trim_url(url), options,
            [&file](const char* data, size_t size) {
                file.write(data, static_cast<std::streamsize>(size));
                if (!file) throw std::runtime_error("写入文件失败");
            },
            progress, cancel);
    } catch (const std::exception& ex) {
        result = Error::make(14, "HTTP 下载失败", url + ": " + ex.what());
    }

    file.close();
    if (!result) {
        std::filesystem::remove(temporary, ec);
        return result.error();
    }
    if (!file) {
        std::filesystem::remove(temporary, ec);
        return Error::make(12, "下载文件写入失败", path_to_utf8(temporary));
    }

    if (!::MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        std::filesystem::remove(temporary, ec);
        return Error::make(13, "无法保存下载文件",
                           path_to_utf8(destination) + ": " + win32_error(error));
    }
    Log::debug("HTTP 下载保存完成：" + path_to_utf8(destination));
    return {};
}

} // namespace cl
