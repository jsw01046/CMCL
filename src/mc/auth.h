#pragma once

#include <optional>
#include <string>

#include "core/result.h"

namespace cl::mc {

// 仓库不包含任何 Azure 客户端 ID。使用前自行注册一个公共客户端应用：
//   账户类型 = 仅个人 Microsoft 账户
//   允许公共客户端流 = 是
// 然后通过环境变量或配置文件提供 ID（见 README「Microsoft 登录」）。
// 构建时也可以把 ID 写进这里，但不要提交到公开仓库。
inline constexpr const char* kMicrosoftClientId = "";

// 个人 Microsoft 账户使用 consumers 租户；工作/学校账户可改成 common。
inline constexpr const char* kMicrosoftTenant = "consumers";

struct MsDeviceCode {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    std::string message;
    int expires_in = 900;
    int interval = 5;
};

struct MsAccount {
    std::string name;
    std::string uuid;  // 带连字符的标准 UUID
    std::string access_token;
    std::string refresh_token;
    long long expires_at = 0;  // Minecraft 访问令牌过期时间（Unix 秒）
};

enum class MsPollStatus {
    Pending,   // 用户还没完成验证
    SlowDown,  // 轮询太快，需要延长间隔
    Success,
    Expired,   // 设备码过期
    Declined,  // 用户拒绝
    Failed,    // 其它错误
};

struct MsPollResult {
    MsPollStatus status = MsPollStatus::Pending;
    MsAccount account;
    std::string message;
    int interval = 5;
};

const char* ms_poll_status_name(MsPollStatus status);

// 1. 申请设备码，让用户到 microsoft.com/link 输入 user_code。
Result<MsDeviceCode> ms_device_code_begin();

// 2. 轮询。authorization_pending 是正常状态，不是错误。
Result<MsPollResult> ms_device_code_poll(const MsDeviceCode& code);

// 用 refresh_token 换新的 Xbox / Minecraft 令牌。
Result<MsAccount> ms_refresh_account(const MsAccount& account);

// 访问令牌是否在 within_seconds 内过期。
bool ms_account_expired(const MsAccount& account, int within_seconds = 300);

// 过期时自动刷新，未过期直接返回。
Result<MsAccount> ms_account_ensure(const MsAccount& account,
                                    int within_seconds = 300);

// 本地账号（DPAPI 加密，只有当前 Windows 用户能解密）。
Result<std::optional<MsAccount>> ms_account_load();
Result<void> ms_account_save(const MsAccount& account);
Result<void> ms_account_clear();

} // namespace cl::mc
