// 微软设备码登录的手动探针（需要联网，不加入 ctest）。
// 用法：
//   auth_probe begin              申请设备码并打印
//   auth_probe poll <device_code> 轮询一次
//   auth_probe refresh <token>    用 refresh_token 换新令牌（会输出账号名）
#include <cstdio>
#include <cstring>
#include <string>

#include "mc/auth.h"

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "begin";

    if (mode == "begin") {
        auto code = cl::mc::ms_device_code_begin();
        if (!code) {
            std::printf("ERROR: %s\n", code.error().to_string().c_str());
            return 1;
        }
        std::printf("user_code=%s\n", code.value().user_code.c_str());
        std::printf("uri=%s\n", code.value().verification_uri.c_str());
        std::printf("expires_in=%d\n", code.value().expires_in);
        std::printf("interval=%d\n", code.value().interval);
        std::printf("device_code=%s\n", code.value().device_code.c_str());
        return 0;
    }

    if (mode == "poll") {
        if (argc < 3) {
            std::printf("缺少 device_code\n");
            return 2;
        }
        cl::mc::MsDeviceCode code;
        code.device_code = argv[2];
        code.interval = 5;
        auto result = cl::mc::ms_device_code_poll(code);
        if (!result) {
            std::printf("ERROR: %s\n", result.error().to_string().c_str());
            return 1;
        }
        std::printf("status=%s\n",
                    cl::mc::ms_poll_status_name(result.value().status));
        std::printf("message=%s\n", result.value().message.c_str());
        if (result.value().status == cl::mc::MsPollStatus::Success) {
            std::printf("name=%s\nuuid=%s\n",
                        result.value().account.name.c_str(),
                        result.value().account.uuid.c_str());
        }
        return 0;
    }

    if (mode == "persist") {
        // 已有真实账号时绝不覆盖，避免测试数据冲掉用户登录。
        if (auto existing = cl::mc::ms_account_load();
            existing && existing.value()) {
            std::printf("existing=%s\n",
                        existing.value()->name.c_str());
            std::printf("skipped=已有账号，未写入测试数据\n");
            return 0;
        }

        cl::mc::MsAccount account;
        account.name = "PersistenceTest";
        account.uuid = "00000000-0000-0000-0000-000000000000";
        account.access_token = "test-access-token";
        account.refresh_token = "test-refresh-token";
        account.expires_at = 4102444800;  // 2100-01-01

        auto saved = cl::mc::ms_account_save(account);
        if (!saved) {
            std::printf("SAVE ERROR: %s\n", saved.error().to_string().c_str());
            return 1;
        }
        std::printf("saved=ok\n");

        auto loaded = cl::mc::ms_account_load();
        if (!loaded) {
            std::printf("LOAD ERROR: %s\n", loaded.error().to_string().c_str());
            return 1;
        }
        if (!loaded.value()) {
            std::printf("loaded=null\n");
            return 1;
        }
        std::printf("loaded_name=%s\n", loaded.value()->name.c_str());
        std::printf("loaded_uuid=%s\n", loaded.value()->uuid.c_str());
        std::printf("token_match=%s\n",
                    loaded.value()->access_token == account.access_token &&
                            loaded.value()->refresh_token == account.refresh_token
                        ? "yes"
                        : "no");

        auto cleared = cl::mc::ms_account_clear();
        std::printf("cleared=%s\n", cleared ? "ok" : "error");
        return 0;
    }

    if (mode == "refresh") {
        if (argc < 3) {
            std::printf("缺少 refresh_token\n");
            return 2;
        }
        cl::mc::MsAccount account;
        account.refresh_token = argv[2];
        auto result = cl::mc::ms_refresh_account(account);
        if (!result) {
            std::printf("ERROR: %s\n", result.error().to_string().c_str());
            return 1;
        }
        std::printf("name=%s\nuuid=%s\nexpires_at=%lld\n",
                    result.value().name.c_str(), result.value().uuid.c_str(),
                    result.value().expires_at);
        return 0;
    }

    std::printf("未知模式：%s\n", mode.c_str());
    return 2;
}
