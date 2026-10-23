#include "mc/auth.h"

#include <windows.h>
#include <dpapi.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/encoding.h"
#include "core/http.h"
#include "core/log.h"
#include "core/paths.h"

namespace cl::mc {

namespace {

constexpr const char* kXblUrl =
    "https://user.auth.xboxlive.com/user/authenticate";
constexpr const char* kXstsUrl = "https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr const char* kMinecraftLoginUrl =
    "https://api.minecraftservices.com/authentication/login_with_xbox";
constexpr const char* kMinecraftProfileUrl =
    "https://api.minecraftservices.com/minecraft/profile";
constexpr const char* kScope = "XboxLive.signin offline_access";

long long now_seconds() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 允许用环境变量或 %LOCALAPPDATA%\CraftLauncher\auth.json 覆盖客户端 ID：
//   { "client_id": "....", "tenant": "consumers" }
// 审核通过换 ID 时不需要改代码或重新打包。
std::string auth_config_value(const char* env_name, const char* json_key) {
    wchar_t buffer[256]{};
    const DWORD size =
        ::GetEnvironmentVariableW(to_wide(env_name).c_str(), buffer, 256);
    if (size > 0 && size < 256) {
        const std::string value = to_utf8(std::wstring(buffer, size));
        if (!value.empty()) return value;
    }

    std::ifstream file(local_app_data_dir() / L"CraftLauncher" / L"auth.json");
    if (file) {
        try {
            nlohmann::json json;
            file >> json;
            const auto it = json.find(json_key);
            if (it != json.end() && it->is_string()) {
                const std::string value = it->get<std::string>();
                if (!value.empty()) return value;
            }
        } catch (const std::exception&) {
            // 配置文件损坏时回退到内置值。
        }
    }
    return {};
}

std::string resolve_client_id() {
    static const std::string value = [] {
        const std::string custom =
            auth_config_value("CMCL_MS_CLIENT_ID", "client_id");
        return custom.empty() ? std::string(kMicrosoftClientId) : custom;
    }();
    return value;
}

std::string resolve_tenant() {
    static const std::string value = [] {
        std::string tenant =
            auth_config_value("CMCL_MS_TENANT", "tenant");
        if (tenant.empty()) tenant = kMicrosoftTenant;
        bool valid = !tenant.empty();
        for (const unsigned char c : tenant) {
            if (!(std::isalnum(c) || c == '-' || c == '.')) {
                valid = false;
                break;
            }
        }
        return valid ? tenant : std::string(kMicrosoftTenant);
    }();
    return value;
}

std::string auth_endpoint(const char* leaf) {
    return "https://login.microsoftonline.com/" + resolve_tenant() +
           "/oauth2/v2.0/" + leaf;
}

std::string url_encode(std::string_view text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string json_string(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

long long json_int(const nlohmann::json& json, const char* key,
                   long long fallback = 0) {
    const auto it = json.find(key);
    if (it == json.end()) return fallback;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number_unsigned()) {
        return static_cast<long long>(it->get<unsigned long long>());
    }
    return fallback;
}

struct JsonResponse {
    int status = 0;
    nlohmann::json body;
};

Result<JsonResponse> send_request(const std::string& url,
                                  const std::string& method,
                                  std::string body,
                                  const std::string& content_type) {
    HttpRequestOptions options;
    options.method = method;
    options.body = std::move(body);
    options.content_type = content_type;
    options.allow_error_status = true;
    options.timeout_seconds = 45;
    options.headers["Accept"] = "application/json";

    auto response = http_request(url, options);
    if (!response) return response.error();

    JsonResponse parsed;
    parsed.status = response.value().status;
    try {
        parsed.body = nlohmann::json::parse(response.value().body);
    } catch (const std::exception&) {
        return Error::make(21, "登录服务器返回了无法解析的数据",
                           url + " (HTTP " +
                               std::to_string(response.value().status) + ")");
    }
    return parsed;
}

Result<JsonResponse> post_form(const std::string& url,
                               const std::string& body) {
    return send_request(url, "POST", body,
                        "application/x-www-form-urlencoded");
}

Result<JsonResponse> post_json(const std::string& url,
                               const nlohmann::json& body) {
    return send_request(url, "POST", body.dump(), "application/json");
}

std::string format_uuid(std::string id) {
    if (id.size() != 32) return id;
    for (char& c : id) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) out.push_back('-');
        out.push_back(id[i]);
    }
    return out;
}

std::string first_line(std::string text);

// 把 Microsoft 账户令牌换成 Xbox Live → XSTS → Minecraft 令牌。
Result<MsAccount> complete_login(const std::string& ms_access_token,
                                 const std::string& refresh_token,
                                 long long expires_in) {
    // 1. Xbox Live 用户认证
    nlohmann::json xbl_body = {
        {"Properties",
         {{"AuthMethod", "RPS"},
          {"SiteName", "user.auth.xboxlive.com"},
          {"RpsTicket", "d=" + ms_access_token}}},
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType", "JWT"},
    };
    auto xbl = post_json(kXblUrl, xbl_body);
    if (!xbl) return xbl.error();
    if (xbl.value().status != 200) {
        return Error::make(22, "Xbox Live 认证失败",
                           "HTTP " + std::to_string(xbl.value().status));
    }
    const std::string xbl_token = json_string(xbl.value().body, "Token");
    std::string uhs;
    if (const auto it = xbl.value().body.find("DisplayClaims");
        it != xbl.value().body.end()) {
        if (const auto xui = it->find("xui");
            xui != it->end() && xui->is_array() && !xui->empty()) {
            uhs = json_string(xui->front(), "uhs");
        }
    }
    if (xbl_token.empty() || uhs.empty()) {
        return Error::make(23, "Xbox Live 认证返回的数据不完整");
    }

    // 2. XSTS 授权
    nlohmann::json xsts_body = {
        {"Properties",
         {{"SandboxId", "RETAIL"}, {"UserTokens", {xbl_token}}}},
        {"RelyingParty", "rp://api.minecraftservices.com/"},
        {"TokenType", "JWT"},
    };
    auto xsts = post_json(kXstsUrl, xsts_body);
    if (!xsts) return xsts.error();
    if (xsts.value().status == 401) {
        const long long xerr = json_int(xsts.value().body, "XErr");
        std::string message;
        switch (xerr) {
            case 2148916233:
                message =
                    "这个 Microsoft 账户还没有 Xbox 账户。请先到 xbox.com "
                    "创建档案后再登录。";
                break;
            case 2148916235:
                message = "当前地区不支持 Xbox Live，无法登录。";
                break;
            case 2148916236:
            case 2148916237:
                message = "这个账户需要在 xbox.com 完成成人验证。";
                break;
            case 2148916238:
                message =
                    "这是未成年账户，需要先由家长把它加入 Microsoft 家庭组。";
                break;
            default:
                message = "XSTS 授权被拒绝（XErr " +
                          std::to_string(xerr) + "）。";
                break;
        }
        return Error::make(24, message);
    }
    if (xsts.value().status != 200) {
        return Error::make(25, "XSTS 授权失败",
                           "HTTP " + std::to_string(xsts.value().status));
    }
    const std::string xsts_token = json_string(xsts.value().body, "Token");
    if (xsts_token.empty()) {
        return Error::make(26, "XSTS 授权返回的数据不完整");
    }
    // XSTS 响应里的 uhs 才是 Minecraft 需要的用户哈希，优先使用它。
    if (const auto claims = xsts.value().body.find("DisplayClaims");
        claims != xsts.value().body.end()) {
        if (const auto xui = claims->find("xui");
            xui != claims->end() && xui->is_array() && !xui->empty()) {
            const std::string xsts_uhs = json_string(xui->front(), "uhs");
            if (!xsts_uhs.empty()) uhs = xsts_uhs;
        }
    }

    // 3. 换取 Minecraft 访问令牌
    nlohmann::json mc_body = {
        {"identityToken", "XBL3.0 x=" + uhs + ";" + xsts_token},
    };
    auto login = post_json(kMinecraftLoginUrl, mc_body);
    if (!login) return login.error();
    if (login.value().status != 200) {
        const std::string server_error =
            json_string(login.value().body, "errorMessage");
        const bool app_not_allowed =
            server_error.find("Invalid app registration") != std::string::npos;
        std::string message;
        if (login.value().status == 403 && app_not_allowed) {
            message =
                "Minecraft 服务拒绝了应用注册（HTTP 403：Invalid app "
                "registration）。\n"
                "这不是账号或游戏所有权问题：自 2022 年起，新注册的 Azure "
                "应用必须由 Mojang 加入 Java 版 API 白名单，才能完成最后一步。\n"
                "申请地址：https://aka.ms/mce-reviewappid（提交客户端 ID 和"
                "租户 ID）。\n"
                "审核通过后用同一个客户端 ID 即可登录，不需要重新打包。";
        } else if (login.value().status == 403) {
            message =
                "Minecraft 登录被拒绝（HTTP 403）。\n"
                "常见原因：这个 Microsoft 账户没有购买 Minecraft: Java "
                "Edition，或 Xbox 隐私设置禁止多人游戏 / 需要成人验证。";
        } else {
            message = "Minecraft 登录失败（HTTP " +
                      std::to_string(login.value().status) + "）";
        }
        if (!server_error.empty()) {
            message += "\n服务器信息：" + first_line(server_error);
        }
        return Error::make(27, message);
    }
    const std::string access_token =
        json_string(login.value().body, "access_token");
    if (access_token.empty()) {
        return Error::make(28, "Minecraft 登录返回的数据不完整");
    }
    const long long token_expires = json_int(login.value().body, "expires_in",
                                             expires_in > 0 ? expires_in : 86400);

    // 4. 读取游戏档案（名字 + UUID）
    HttpRequestOptions profile_options;
    profile_options.method = "GET";
    profile_options.allow_error_status = true;
    profile_options.timeout_seconds = 30;
    profile_options.headers["Authorization"] = "Bearer " + access_token;
    profile_options.headers["Accept"] = "application/json";
    auto profile_response = http_request(kMinecraftProfileUrl, profile_options);
    if (!profile_response) return profile_response.error();

    if (profile_response.value().status == 404) {
        return Error::make(
            29,
            "这个 Microsoft 账户没有 Minecraft: Java Edition 档案。\n"
            "请确认已经购买游戏，并到 minecraft.net 完成账户迁移。");
    }
    if (profile_response.value().status == 401) {
        return Error::make(30, "Minecraft 档案读取被拒绝，请重新登录");
    }
    if (profile_response.value().status != 200) {
        return Error::make(31, "Minecraft 档案读取失败",
                           "HTTP " +
                               std::to_string(profile_response.value().status));
    }

    nlohmann::json profile;
    try {
        profile = nlohmann::json::parse(profile_response.value().body);
    } catch (const std::exception&) {
        return Error::make(32, "Minecraft 档案数据无法解析");
    }

    MsAccount account;
    account.name = json_string(profile, "name");
    account.uuid = format_uuid(json_string(profile, "id"));
    account.access_token = access_token;
    account.refresh_token = refresh_token;
    account.expires_at = now_seconds() + token_expires;
    if (account.name.empty() || account.uuid.empty()) {
        return Error::make(33, "Minecraft 档案缺少用户名或 UUID");
    }
    return account;
}

// 从 Azure 的 error_codes 里提取更具体的原因（比 error 字段准确）。
std::string azure_error_hint(const nlohmann::json& json) {
    const auto it = json.find("error_codes");
    if (it == json.end() || !it->is_array()) return {};

    for (const auto& code : *it) {
        if (!code.is_number_integer()) continue;
        const long long value = code.get<long long>();
        if (value == 700016) {
            return "Azure 应用不存在于个人账户租户中。请确认客户端 ID 正确，"
                   "并把“支持的账户类型”改为“仅个人 Microsoft 账户”。";
        }
        if (value == 7000218) {
            return "请在 Azure 应用的身份验证页面开启“允许公共客户端流”。";
        }
        if (value == 90002) {
            return "这个 Microsoft 账户尚未加入应用的可用租户。";
        }
    }
    return {};
}

std::string first_line(std::string text) {
    const auto pos = text.find_first_of("\r\n");
    if (pos != std::string::npos) text.resize(pos);
    if (text.size() > 300) text.resize(300);
    return text;
}

std::string oauth_error_message(const std::string& code,
                                const std::string& description) {
    if (code == "invalid_client") {
        return "Azure 应用配置无效：client_id 不存在，或没有开启“允许公共客户端流”。";
    }
    if (code == "unauthorized_client") {
        return "这个 Azure 应用不允许设备码登录，请检查“允许公共客户端流”。";
    }
    if (code == "invalid_grant" || code == "interaction_required") {
        return "微软登录已失效，请重新登录。";
    }
    if (code == "bad_verification_code") {
        return "设备码无效，请重新获取。";
    }
    if (code == "expired_token") {
        return "设备码已过期，请重新获取。";
    }
    if (code == "authorization_declined") {
        return "你在 Microsoft 登录页面拒绝了授权。";
    }
    if (!description.empty()) return description;
    return "Microsoft 登录失败：" + code;
}

std::filesystem::path account_file() {
    return local_app_data_dir() / L"CraftLauncher" / L"account.dat";
}

Error dpapi_error(const char* what) {
    return Error::make(40, what,
                       "Win32 错误 " + std::to_string(::GetLastError()));
}

const std::string& dpapi_entropy() {
    static const std::string value = "CMCL.MicrosoftAccount.v1";
    return value;
}

Result<std::vector<unsigned char>> protect_data(const std::string& plain) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(dpapi_entropy().data()));
    entropy.cbData = static_cast<DWORD>(dpapi_entropy().size());

    DATA_BLOB out{};
    if (!::CryptProtectData(&in, L"CMCL Microsoft account", &entropy, nullptr,
                            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return dpapi_error("无法加密账号数据");
    }
    std::vector<unsigned char> result(out.pbData, out.pbData + out.cbData);
    ::LocalFree(out.pbData);
    return result;
}

Result<std::string> unprotect_data(const std::vector<unsigned char>& cipher) {
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(cipher.data());
    in.cbData = static_cast<DWORD>(cipher.size());
    DATA_BLOB entropy{};
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(dpapi_entropy().data()));
    entropy.cbData = static_cast<DWORD>(dpapi_entropy().size());

    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return dpapi_error("无法解密账号数据");
    }
    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    ::LocalFree(out.pbData);
    return result;
}

} // namespace

const char* ms_poll_status_name(MsPollStatus status) {
    switch (status) {
        case MsPollStatus::Pending:
            return "pending";
        case MsPollStatus::SlowDown:
            return "slow_down";
        case MsPollStatus::Success:
            return "success";
        case MsPollStatus::Expired:
            return "expired";
        case MsPollStatus::Declined:
            return "declined";
        case MsPollStatus::Failed:
            return "failed";
    }
    return "failed";
}

Result<MsDeviceCode> ms_device_code_begin() {
    const std::string client_id = resolve_client_id();
    if (client_id.empty()) {
        return Error::make(
            49,
            "未配置 Microsoft 客户端 ID。\n"
            "请设置环境变量 CMCL_MS_CLIENT_ID，或创建 "
            "%LOCALAPPDATA%\\CraftLauncher\\auth.json：\n"
            "{\"client_id\":\"你的客户端 ID\",\"tenant\":\"consumers\"}");
    }

    std::string body = "client_id=" + url_encode(client_id) +
                       "&scope=" + url_encode(kScope);
    auto response = post_form(auth_endpoint("devicecode"), body);
    if (!response) return response.error();

    const auto& json = response.value().body;
    if (response.value().status != 200) {
        const std::string code = json_string(json, "error");
        const std::string description = json_string(json, "error_description");
        std::string message = azure_error_hint(json);
        if (message.empty()) message = oauth_error_message(code, description);
        const std::string detail = first_line(description);
        return Error::make(
            50, std::move(message),
            detail.empty() ? "HTTP " + std::to_string(response.value().status)
                           : detail);
    }

    MsDeviceCode code;
    code.device_code = json_string(json, "device_code");
    code.user_code = json_string(json, "user_code");
    code.verification_uri = json_string(json, "verification_uri");
    code.message = json_string(json, "message");
    code.expires_in = static_cast<int>(json_int(json, "expires_in", 900));
    code.interval = static_cast<int>(json_int(json, "interval", 5));
    if (code.interval < 1) code.interval = 5;

    if (code.device_code.empty() || code.user_code.empty()) {
        return Error::make(51, "设备码响应缺少必要字段");
    }
    if (code.verification_uri.empty()) {
        code.verification_uri = "https://microsoft.com/link";
    }
    return code;
}

Result<MsPollResult> ms_device_code_poll(const MsDeviceCode& code) {
    std::string body =
        "grant_type=" +
        url_encode("urn:ietf:params:oauth:grant-type:device_code") +
        "&client_id=" + url_encode(resolve_client_id()) +
        "&device_code=" + url_encode(code.device_code);

    auto response = post_form(auth_endpoint("token"), body);
    if (!response) return response.error();

    MsPollResult result;
    result.interval = code.interval;
    const auto& json = response.value().body;

    if (response.value().status >= 200 && response.value().status < 300) {
        const std::string ms_access = json_string(json, "access_token");
        const std::string refresh = json_string(json, "refresh_token");
        const long long expires_in = json_int(json, "expires_in", 3600);
        if (ms_access.empty()) {
            result.status = MsPollStatus::Failed;
            result.message = "Microsoft 登录响应缺少 access_token。";
            return result;
        }
        auto account = complete_login(ms_access, refresh, expires_in);
        if (!account) {
            result.status = MsPollStatus::Failed;
            result.message = account.error().to_string();
            return result;
        }
        result.status = MsPollStatus::Success;
        result.account = std::move(account).value();
        result.message = "登录成功";
        return result;
    }

    const std::string error = json_string(json, "error");
    const std::string description = json_string(json, "error_description");

    if (error == "authorization_pending") {
        result.status = MsPollStatus::Pending;
        result.message = "等待你在浏览器中完成登录…";
    } else if (error == "slow_down") {
        result.status = MsPollStatus::SlowDown;
        result.interval = code.interval + 5;
        result.message = "轮询过快，已延长间隔。";
    } else if (error == "expired_token") {
        result.status = MsPollStatus::Expired;
        result.message = "设备码已过期，请重新获取。";
    } else if (error == "authorization_declined") {
        result.status = MsPollStatus::Declined;
        result.message = "你在 Microsoft 登录页面拒绝了授权。";
    } else {
        result.status = MsPollStatus::Failed;
        result.message = oauth_error_message(error, description);
    }
    return result;
}

Result<MsAccount> ms_refresh_account(const MsAccount& account) {
    if (account.refresh_token.empty()) {
        return Error::make(60, "没有可用的刷新令牌，请重新登录");
    }

    std::string body = "grant_type=refresh_token&client_id=" +
                       url_encode(resolve_client_id()) + "&refresh_token=" +
                       url_encode(account.refresh_token) +
                       "&scope=" + url_encode(kScope);
    auto response = post_form(auth_endpoint("token"), body);
    if (!response) return response.error();

    if (response.value().status < 200 || response.value().status >= 300) {
        const std::string code = json_string(response.value().body, "error");
        return Error::make(
            61,
            oauth_error_message(code,
                                json_string(response.value().body,
                                            "error_description")),
            "HTTP " + std::to_string(response.value().status));
    }

    const auto& json = response.value().body;
    const std::string ms_access = json_string(json, "access_token");
    const std::string refresh = json_string(json, "refresh_token");
    const long long expires_in = json_int(json, "expires_in", 3600);
    if (ms_access.empty()) {
        return Error::make(62, "刷新登录时服务器没有返回 access_token");
    }

    return complete_login(ms_access,
                          refresh.empty() ? account.refresh_token : refresh,
                          expires_in);
}

bool ms_account_expired(const MsAccount& account, int within_seconds) {
    if (account.access_token.empty()) return true;
    return account.expires_at <= now_seconds() + within_seconds;
}

Result<MsAccount> ms_account_ensure(const MsAccount& account,
                                    int within_seconds) {
    if (!ms_account_expired(account, within_seconds)) return account;
    return ms_refresh_account(account);
}

Result<std::optional<MsAccount>> ms_account_load() {
    const std::filesystem::path path = account_file();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::optional<MsAccount>{};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error::make(70, "无法读取本地账号文件",
                           path_to_utf8(path));
    }
    std::vector<unsigned char> cipher((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
    file.close();
    if (cipher.empty()) {
        return std::optional<MsAccount>{};
    }

    auto plain = unprotect_data(cipher);
    if (!plain) {
        Log::warn("本地账号数据无法解密：" + plain.error().to_string());
        return std::optional<MsAccount>{};
    }

    try {
        const auto json = nlohmann::json::parse(plain.value());
        MsAccount account;
        account.name = json_string(json, "name");
        account.uuid = json_string(json, "uuid");
        account.access_token = json_string(json, "access_token");
        account.refresh_token = json_string(json, "refresh_token");
        account.expires_at = json_int(json, "expires_at", 0);
        if (account.name.empty() || account.access_token.empty()) {
            return std::optional<MsAccount>{};
        }
        return std::optional<MsAccount>{std::move(account)};
    } catch (const std::exception&) {
        Log::warn("本地账号数据已损坏，已忽略");
        return std::optional<MsAccount>{};
    }
}

Result<void> ms_account_save(const MsAccount& account) {
    if (account.access_token.empty() || account.refresh_token.empty()) {
        return Error::make(71, "账号数据不完整，无法保存");
    }

    const std::filesystem::path path = account_file();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Error::make(72, "无法创建账号数据目录",
                           path_to_utf8(path.parent_path()) + ": " +
                               ec.message());
    }

    const nlohmann::json json = {
        {"name", account.name},
        {"uuid", account.uuid},
        {"access_token", account.access_token},
        {"refresh_token", account.refresh_token},
        {"expires_at", account.expires_at},
        {"saved_at", now_seconds()},
    };
    auto cipher = protect_data(json.dump());
    if (!cipher) return cipher.error();

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Error::make(73, "无法写入账号文件",
                               path_to_utf8(temporary));
        }
        file.write(reinterpret_cast<const char*>(cipher.value().data()),
                   static_cast<std::streamsize>(cipher.value().size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, ec);
            return Error::make(74, "账号文件写入失败",
                               path_to_utf8(temporary));
        }
    }

    if (!::MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = ::GetLastError();
        std::filesystem::remove(temporary, ec);
        return Error::make(75, "无法保存账号文件",
                           path_to_utf8(path) + ": Win32 " +
                               std::to_string(code));
    }
    return {};
}

Result<void> ms_account_clear() {
    std::error_code ec;
    std::filesystem::remove(account_file(), ec);
    if (ec) {
        return Error::make(76, "无法删除账号文件", ec.message());
    }
    return {};
}

} // namespace cl::mc
