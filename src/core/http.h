#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <map>

#include "core/result.h"

namespace cl {

struct HttpProgress {
    unsigned long long downloaded = 0;
    unsigned long long total = 0;
};

struct HttpOptions {
    unsigned long long expected_size = 0;
    int timeout_seconds = 30;
    // 速度探测：超过 probe_seconds 后，如果平均速度低于 min_speed_bps，
    // 中止本次下载，让上层切换到其它镜像源。0 表示不启用。
    unsigned long long min_speed_bps = 0;
    int speed_probe_seconds = 4;
    std::map<std::string, std::string> headers;
};

using HttpProgressCallback = std::function<void(const HttpProgress&)>;

// 通用请求选项：支持 POST 表单/JSON，并允许把 4xx 响应当作正常结果读取
//（OAuth 设备码轮询的 authorization_pending 就是 HTTP 400）。
struct HttpRequestOptions {
    std::string method = "GET";
    std::string body;
    std::string content_type;
    int timeout_seconds = 30;
    std::map<std::string, std::string> headers;
    bool allow_error_status = false;
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

// 发送 HTTP 请求并返回状态码和正文。正文上限 64 MiB。
Result<HttpResponse> http_request(const std::string& url,
                                  const HttpRequestOptions& options = {});

// GET 一个文本资源（版本清单等）。
Result<std::string> http_get(const std::string& url,
                             const HttpOptions& options = {});

// 下载到文件：先写 .part，成功后再替换目标文件。
Result<void> http_download(const std::string& url,
                           const std::filesystem::path& destination,
                           const HttpOptions& options = {},
                           const HttpProgressCallback& progress = {},
                           const std::atomic<bool>* cancel = nullptr);

} // namespace cl
