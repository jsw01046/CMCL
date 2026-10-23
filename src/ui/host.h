#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/process.h"
#include "core/result.h"
#include "mc/auth.h"
#include "mc/download.h"

namespace cl::ui {

class Host {
public:
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool initialize(HINSTANCE instance, int show_command);
    int run();

private:
    static constexpr UINT kBridgeMessage = WM_APP + 1;

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                        LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    void create_webview();
    void on_resize();
    void on_web_message(const std::wstring& json);
    void handle_request(const std::string& json);
    void dispatch_request(int id, const std::string& method,
                          const nlohmann::json& params);

    void send_json(const std::string& json);
    void post_json(std::string json);
    void reply_ok(int id, nlohmann::json result);
    void reply_error(int id, const Error& error);
    void reply_error(int id, std::string message, std::string context = {});
    void push_event(const std::string& name, nlohmann::json data);
    void push_download_progress(const mc::DownloadProgress& progress);

    void start_job(std::function<void()> job);

    void rpc_app_info(int id);
    void rpc_app_open_url(int id, const nlohmann::json& params);
    void rpc_instance_list(int id, const nlohmann::json& params);
    void rpc_instance_inspect(int id, const nlohmann::json& params);
    void rpc_instance_rename(int id, const nlohmann::json& params);
    void rpc_instance_delete(int id, const nlohmann::json& params);
    void rpc_instance_open_folder(int id, const nlohmann::json& params);
    void rpc_instance_open_saves(int id, const nlohmann::json& params);
    void rpc_instance_settings_get(int id, const nlohmann::json& params);
    void rpc_instance_settings_set(int id, const nlohmann::json& params);
    void rpc_instance_resources_list(int id, const nlohmann::json& params);
    void rpc_instance_resources_delete(int id, const nlohmann::json& params);
    void rpc_instance_resources_toggle(int id, const nlohmann::json& params);
    void rpc_instance_resources_open(int id, const nlohmann::json& params);
    void rpc_java_list(int id, const nlohmann::json& params);
    void rpc_java_available(int id, const nlohmann::json& params);
    void rpc_java_download(int id, const nlohmann::json& params);
    void rpc_java_delete(int id, const nlohmann::json& params);
    void rpc_java_uninstall(int id, const nlohmann::json& params);
    void rpc_download_versions(int id, const nlohmann::json& params);
    void rpc_download_install(int id, const nlohmann::json& params);
    void rpc_download_repair(int id, const nlohmann::json& params);
    void rpc_download_cancel(int id);
    void rpc_download_mirrors(int id);
    void rpc_download_mirrors_refresh(int id, const nlohmann::json& params);
    void rpc_loader_versions(int id, const nlohmann::json& params);
    void rpc_loader_install(int id, const nlohmann::json& params);
    
    void rpc_modrinth_search(int id, const nlohmann::json& params);
    void rpc_modrinth_versions(int id, const nlohmann::json& params);
    void rpc_modrinth_download(int id, const nlohmann::json& params);

    void rpc_modpack_install(int id, const nlohmann::json& params);
    void rpc_dialog_open_modpack(int id);
    void rpc_crash_save_report(int id, const nlohmann::json& params);
    void rpc_crash_open_folder(int id, const nlohmann::json& params);

    void rpc_auth_status(int id, const nlohmann::json& params);
    void rpc_auth_device_start(int id, const nlohmann::json& params);
    void rpc_auth_device_poll(int id, const nlohmann::json& params);
    void rpc_auth_logout(int id, const nlohmann::json& params);

    void rpc_game_launch(int id, const nlohmann::json& params);
    void rpc_game_stop(int id);
    void rpc_game_status(int id);

    void rpc_window_minimize(int id);
    void rpc_window_toggle_maximize(int id);
    void rpc_window_close(int id);
    void rpc_window_drag(int id);
    void rpc_window_resize(int id, const nlohmann::json& params);
    void rpc_window_state(int id);

    void show_webview_error(HRESULT hr);

    std::optional<std::pair<int, int>> load_window_size_dip() const;
    void save_window_size() const;

    HWND hwnd_ = nullptr;
    std::filesystem::path web_root_;
    std::filesystem::path user_data_dir_;
    std::filesystem::path window_state_path_;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
        environment_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
        controller_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2WebMessageReceivedEventHandler>
        message_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationCompletedEventHandler>
        navigation_handler_;
    EventRegistrationToken message_token_{};
    EventRegistrationToken navigation_token_{};
    bool webview_ready_ = false;

    std::mutex jobs_mutex_;
    std::vector<std::jthread> jobs_;

    std::mutex game_mutex_;
    std::unique_ptr<Process> game_;
    std::atomic<bool> launching_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> download_busy_{false};
    std::atomic<bool> download_cancel_{false};
    std::atomic<bool> closing_{false};

    std::mutex run_log_mutex_;
    std::vector<std::string> run_log_;
    std::chrono::steady_clock::time_point run_started_{};

    // 设备码只保存在宿主端，前端拿不到 device_code。
    std::mutex auth_mutex_;
    std::optional<mc::MsDeviceCode> auth_device_;
};

} // namespace cl::ui