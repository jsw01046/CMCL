#include "ui/host.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include "resource.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

#include "core/encoding.h"
#include "core/log.h"
#include "core/paths.h"
#include "mc/diagnostics.h"
#include "mc/download.h"
#include "mc/instance.h"
#include "mc/java.h"
#include "mc/jdk.h"
#include "mc/launch.h"
#include "mc/loader.h"
#include "mc/mirrors.h"
#include "mc/version.h"
#include "mc/modrinth.h"
#include "mc/modpack.h"

namespace cl::ui {

using mc::delete_instance;
using mc::delete_instance_resource;
using mc::detect_java;
using mc::DownloadProgress;
using mc::DownloadRequest;
using mc::import_modrinth_modpack;
using mc::InstanceSettings;
using mc::list_instance_resources;
using mc::ModpackImportRequest;
using mc::open_instance_resources;
using mc::parse_download_source;
using mc::launch_prepared;
using mc::LaunchOptions;
using mc::load_instance_settings;
using mc::ms_account_clear;
using mc::ms_account_ensure;
using mc::ms_account_expired;
using mc::ms_account_load;
using mc::ms_account_save;
using mc::ms_device_code_begin;
using mc::ms_device_code_poll;
using mc::ms_poll_status_name;
using mc::MsAccount;
using mc::MsDeviceCode;
using mc::MsPollStatus;
using mc::open_instance_folder;
using mc::open_instance_saves;
using mc::prepare_launch;
using mc::rename_instance;
using mc::resolve_version;
using mc::save_instance_settings;
using mc::scan_instances;
using mc::toggle_instance_resource;

namespace {

// 以 96 DPI 为基准的逻辑尺寸，实际窗口会按 DPI 缩放并夹到工作区内。
constexpr int kInitialWidthDip = 1280;
constexpr int kInitialHeightDip = 800;
constexpr int kMinWidthDip = 1024;
constexpr int kMinHeightDip = 640;
constexpr int kWindowMarginDip = 24;
constexpr wchar_t kWindowTitle[] = L"CMCL - C++ Minecraft Launcher";

std::string param_string(const nlohmann::json& params, const char* key,
                         std::string fallback = {}) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

int param_int(const nlohmann::json& params, const char* key, int fallback) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_number_integer()) return fallback;
    return it->get<int>();
}

bool param_bool(const nlohmann::json& params, const char* key, bool fallback) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

int classpath_count(const std::string& classpath) {
    if (classpath.empty()) return 0;
    int count = 1;
    for (const char c : classpath) {
        if (c == ';') ++count;
    }
    return count;
}

} // namespace

Host::Host() = default;

Host::~Host() {
    closing_ = true;
    {
        std::lock_guard lock(game_mutex_);
        if (game_) game_->terminate();
    }
    for (auto& job : jobs_) {
        if (job.joinable()) job.join();
    }
}

bool Host::initialize(HINSTANCE instance, int show_command) {
    const HICON icon_big = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CMCL_ICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), 0));
    const HICON icon_small = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CMCL_ICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Host::window_proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = icon_big;
    wc.hIconSm = icon_small;
    wc.lpszClassName = L"CMCLWindow";

    if (::RegisterClassExW(&wc) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log::error("注册窗口类失败");
        return false;
    }

    hwnd_ = ::CreateWindowExW(
        WS_EX_APPWINDOW, wc.lpszClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW & ~WS_CAPTION, CW_USEDEFAULT, CW_USEDEFAULT,
        kInitialWidthDip, kInitialHeightDip, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        Log::error("创建窗口失败");
        return false;
    }

    // 任务栏按钮创建后再设置一次，避免窗口图标被 WebView2 初始化覆盖。
    ::SetWindowTextW(hwnd_, kWindowTitle);
    if (icon_big != nullptr) {
        ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                       reinterpret_cast<LPARAM>(icon_big));
    }
    if (icon_small != nullptr) {
        ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                       reinterpret_cast<LPARAM>(icon_small));
    }
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);

    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                            sizeof(corner));

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    ::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST),
                      &monitor_info);
    const RECT work = monitor_info.rcWork;

    const UINT dpi = ::GetDpiForWindow(hwnd_);
    window_state_path_ =
        local_app_data_dir() / L"CraftLauncher" / L"window.json";

    int target_width_dip = kInitialWidthDip;
    int target_height_dip = kInitialHeightDip;
    if (const auto saved = load_window_size_dip()) {
        target_width_dip = saved->first;
        target_height_dip = saved->second;
    }

    const int margin = ::MulDiv(kWindowMarginDip, static_cast<int>(dpi), 96) * 2;
    const int max_width = std::max(
        320, static_cast<int>(work.right - work.left) - margin);
    const int max_height = std::max(
        240, static_cast<int>(work.bottom - work.top) - margin);
    const int min_width =
        std::min(::MulDiv(kMinWidthDip, static_cast<int>(dpi), 96), max_width);
    const int min_height =
        std::min(::MulDiv(kMinHeightDip, static_cast<int>(dpi), 96), max_height);
    const int width = std::clamp(
        ::MulDiv(target_width_dip, static_cast<int>(dpi), 96), min_width,
        max_width);
    const int height = std::clamp(
        ::MulDiv(target_height_dip, static_cast<int>(dpi), 96), min_height,
        max_height);
    const int x = work.left + std::max(
        0, static_cast<int>((work.right - work.left - width) / 2));
    const int y = work.top + std::max(
        0, static_cast<int>((work.bottom - work.top - height) / 2));
    ::SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER);
    Log::info("窗口尺寸：" + std::to_string(width) + "x" +
              std::to_string(height) + "，DPI " + std::to_string(dpi));

    ::ShowWindow(hwnd_, show_command);
    ::UpdateWindow(hwnd_);
    create_webview();
    return true;
}

int Host::run() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK Host::window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }

    auto* self = reinterpret_cast<Host*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->handle_message(message, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Host::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCCALCSIZE:
            if (wparam) {
                if (::IsZoomed(hwnd_)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                    MONITORINFO info{};
                    info.cbSize = sizeof(info);
                    ::GetMonitorInfoW(
                        ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST),
                        &info);
                    params->rgrc[0] = info.rcWork;
                }
                return 0;
            }
            break;

        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) on_resize();
            if (webview_ready_) {
                push_event("window.state",
                           {{"maximized", ::IsZoomed(hwnd_) != 0}});
            }
            return 0;

        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top, SWP_NOZORDER);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const UINT dpi = ::GetDpiForWindow(hwnd_);
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            ::GetMonitorInfoW(
                ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST),
                &monitor_info);
            const int margin =
                ::MulDiv(kWindowMarginDip, static_cast<int>(dpi), 96) * 2;
            const int max_width = std::max(
                320, static_cast<int>(monitor_info.rcWork.right -
                                      monitor_info.rcWork.left) - margin);
            const int max_height = std::max(
                240, static_cast<int>(monitor_info.rcWork.bottom -
                                      monitor_info.rcWork.top) - margin);
            info->ptMinTrackSize.x = std::min(
                ::MulDiv(kMinWidthDip, static_cast<int>(dpi), 96), max_width);
            info->ptMinTrackSize.y = std::min(
                ::MulDiv(kMinHeightDip, static_cast<int>(dpi), 96), max_height);
            return 0;
        }

        case kBridgeMessage: {
            std::unique_ptr<std::string> payload(
                reinterpret_cast<std::string*>(lparam));
            if (payload && webview_ && !closing_) send_json(*payload);
            return 0;
        }

        case WM_CLOSE: {
            bool running = false;
            {
                std::lock_guard lock(game_mutex_);
                running = game_ && game_->running();
            }
            if (running) {
                const int answer = ::MessageBoxW(
                    hwnd_,
                    L"游戏正在运行，确定要退出启动器吗？\n退出会同时结束游戏进程。",
                    L"CMCL - C++ Minecraft Launcher", MB_YESNO | MB_ICONWARNING);
                if (answer != IDYES) return 0;
            }
            save_window_size();
            closing_ = true;
            {
                std::lock_guard lock(game_mutex_);
                if (game_) game_->terminate();
            }
            ::DestroyWindow(hwnd_);
            return 0;
        }

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wparam, lparam);
}

void Host::create_webview() {
    web_root_ = exe_dir() / L"web";
    user_data_dir_ = local_app_data_dir() / L"CraftLauncher" / L"WebView2";

    std::error_code ec;
    std::filesystem::create_directories(user_data_dir_, ec);
    if (!std::filesystem::is_directory(web_root_, ec)) {
        ::MessageBoxW(hwnd_,
                      L"找不到 web 资源目录。\n请确认编译产物旁边存在 web\\index.html。",
                      L"CMCL - C++ Minecraft Launcher", MB_ICONERROR);
        closing_ = true;
        ::DestroyWindow(hwnd_);
        return;
    }

    environment_handler_ = Microsoft::WRL::Callback<
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (closing_) return S_OK;
            if (FAILED(result) || environment == nullptr) {
                show_webview_error(result);
                return S_OK;
            }
            environment_ = environment;

            controller_handler_ = Microsoft::WRL::Callback<
                ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this](HRESULT controller_result,
                       ICoreWebView2Controller* controller) -> HRESULT {
                    if (closing_) return S_OK;
                    if (FAILED(controller_result) || controller == nullptr) {
                        show_webview_error(controller_result);
                        return S_OK;
                    }
                    controller_ = controller;
                    on_resize();

                    if (FAILED(controller_->get_CoreWebView2(&webview_)) ||
                        !webview_) {
                        show_webview_error(E_FAIL);
                        return S_OK;
                    }

                    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                    if (SUCCEEDED(webview_->get_Settings(&settings)) &&
                        settings) {
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE);
                    }

                    message_handler_ = Microsoft::WRL::Callback<
                        ICoreWebView2WebMessageReceivedEventHandler>(
                        [this](ICoreWebView2*,
                               ICoreWebView2WebMessageReceivedEventArgs* args)
                            -> HRESULT {
                            LPWSTR raw = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) &&
                                raw != nullptr) {
                                std::wstring json(raw);
                                ::CoTaskMemFree(raw);

                                // 页面拖入文件时会通过 postMessageWithAdditionalObjects
                                // 附带 File 对象，从这里拿到真实路径。
                                std::vector<std::string> dropped;
                                Microsoft::WRL::ComPtr<
                                    ICoreWebView2WebMessageReceivedEventArgs2>
                                    args2;
                                if (SUCCEEDED(args->QueryInterface(
                                        IID_PPV_ARGS(&args2))) &&
                                    args2) {
                                    Microsoft::WRL::ComPtr<
                                        ICoreWebView2ObjectCollectionView>
                                        objects;
                                    if (SUCCEEDED(args2->get_AdditionalObjects(
                                            &objects)) &&
                                        objects) {
                                        UINT32 count = 0;
                                        objects->get_Count(&count);
                                        for (UINT32 i = 0; i < count; ++i) {
                                            Microsoft::WRL::ComPtr<IUnknown>
                                                value;
                                            if (FAILED(objects->GetValueAtIndex(
                                                    i, &value)) ||
                                                !value) {
                                                continue;
                                            }
                                            Microsoft::WRL::ComPtr<
                                                ICoreWebView2File>
                                                file;
                                            if (FAILED(value.As(&file)) ||
                                                !file) {
                                                continue;
                                            }
                                            LPWSTR path = nullptr;
                                            if (SUCCEEDED(file->get_Path(
                                                    &path)) &&
                                                path != nullptr) {
                                                dropped.push_back(
                                                    path_to_utf8(
                                                        std::filesystem::
                                                            path(path)));
                                                ::CoTaskMemFree(path);
                                            }
                                        }
                                    }
                                }
                                if (!dropped.empty()) {
                                    push_event("files.dropped",
                                               {{"paths", dropped}});
                                } else {
                                    on_web_message(json);
                                }
                            }
                            return S_OK;
                        });
                    webview_->add_WebMessageReceived(message_handler_.Get(),
                                                     &message_token_);

                    navigation_handler_ = Microsoft::WRL::Callback<
                        ICoreWebView2NavigationCompletedEventHandler>(
                        [this](ICoreWebView2*,
                               ICoreWebView2NavigationCompletedEventArgs* args)
                            -> HRESULT {
                            BOOL success = FALSE;
                            if (args) args->get_IsSuccess(&success);
                            if (!success) {
                                Log::error("WebView2 页面加载失败");
                            }
                            return S_OK;
                        });
                    webview_->add_NavigationCompleted(navigation_handler_.Get(),
                                                      &navigation_token_);

                    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
                    if (FAILED(webview_.As(&webview3)) || !webview3) {
                        show_webview_error(E_NOINTERFACE);
                        return S_OK;
                    }
                    const HRESULT mapping =
                        webview3->SetVirtualHostNameToFolderMapping(
                            L"app.craftlauncher.local", web_root_.c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                    if (FAILED(mapping)) {
                        show_webview_error(mapping);
                        return S_OK;
                    }

                    webview_->Navigate(
                        L"https://app.craftlauncher.local/index.html#/launch");
                    ::SetWindowTextW(hwnd_, kWindowTitle);
                    webview_ready_ = true;
                    Log::info("WebView2 初始化完成");
                    return S_OK;
                });
            environment_->CreateCoreWebView2Controller(
                hwnd_, controller_handler_.Get());
            return S_OK;
        });

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data_dir_.c_str(), nullptr, environment_handler_.Get());
    if (FAILED(hr)) {
        show_webview_error(hr);
    }
}

void Host::on_resize() {
    if (!controller_) return;
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    controller_->put_Bounds(client);
}

void Host::show_webview_error(HRESULT hr) {
    wchar_t code[32];
    swprintf_s(code, sizeof(code) / sizeof(code[0]), L"0x%08X",
               static_cast<unsigned int>(hr));
    std::wstring text = L"无法初始化 WebView2。\n\nHRESULT: ";
    text += code;
    text += L"\n\n请确认已安装 Microsoft Edge WebView2 Runtime：\n"
            L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
    ::MessageBoxW(hwnd_, text.c_str(), L"CMCL - C++ Minecraft Launcher",
                  MB_ICONERROR);
    closing_ = true;
    if (::IsWindow(hwnd_)) ::DestroyWindow(hwnd_);
}

std::optional<std::pair<int, int>> Host::load_window_size_dip() const {
    std::ifstream file(window_state_path_, std::ios::binary);
    if (!file) return std::nullopt;
    try {
        nlohmann::json json;
        file >> json;
        if (!json.contains("width") || !json["width"].is_number_integer() ||
            !json.contains("height") || !json["height"].is_number_integer()) {
            return std::nullopt;
        }
        const int width = json["width"].get<int>();
        const int height = json["height"].get<int>();
        if (width < 320 || height < 240 || width > 10000 || height > 10000) {
            return std::nullopt;
        }
        return std::make_pair(width, height);
    } catch (...) {
        return std::nullopt;
    }
}

void Host::save_window_size() const {
    if (hwnd_ == nullptr) return;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!::GetWindowPlacement(hwnd_, &placement)) return;

    const RECT& normal = placement.rcNormalPosition;
    const int width_px = normal.right - normal.left;
    const int height_px = normal.bottom - normal.top;
    if (width_px <= 0 || height_px <= 0) return;

    const UINT dpi = ::GetDpiForWindow(hwnd_);
    const int width_dip = ::MulDiv(width_px, 96, static_cast<int>(dpi));
    const int height_dip = ::MulDiv(height_px, 96, static_cast<int>(dpi));

    std::error_code ec;
    std::filesystem::create_directories(window_state_path_.parent_path(), ec);
    std::ofstream file(window_state_path_, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << nlohmann::json{{"width", width_dip}, {"height", height_dip}}.dump(2);
}

void Host::on_web_message(const std::wstring& json) {
    if (closing_) return;
    handle_request(to_utf8(json));
}

void Host::handle_request(const std::string& json) {
    if (closing_) return;
    try {
        const nlohmann::json request = nlohmann::json::parse(json);
        const int id = request.value("id", 0);
        const std::string method = request.value("method", "");
        nlohmann::json params = nlohmann::json::object();
        if (const auto it = request.find("params");
            it != request.end() && it->is_object()) {
            params = *it;
        }
        dispatch_request(id, method, params);
    } catch (const std::exception& ex) {
        reply_error(0, "请求 JSON 无效", ex.what());
    }
}

void Host::dispatch_request(int id, const std::string& method,
                            const nlohmann::json& params) {
    if (method == "app.info") {
        rpc_app_info(id);
    } else if (method == "app.open_url") {
        rpc_app_open_url(id, params);
    } else if (method == "instance.list") {
        start_job([this, id, params] { rpc_instance_list(id, params); });
    } else if (method == "instance.inspect") {
        start_job([this, id, params] { rpc_instance_inspect(id, params); });
    } else if (method == "instance.rename") {
        start_job([this, id, params] { rpc_instance_rename(id, params); });
    } else if (method == "instance.delete") {
        start_job([this, id, params] { rpc_instance_delete(id, params); });
    } else if (method == "instance.open_folder") {
        rpc_instance_open_folder(id, params);
    } else if (method == "instance.open_saves") {
        rpc_instance_open_saves(id, params);
    } else if (method == "instance.settings.get") {
        rpc_instance_settings_get(id, params);
    } else if (method == "instance.settings.set") {
        rpc_instance_settings_set(id, params);
    } else if (method == "instance.resources.list") {
        start_job([this, id, params] { rpc_instance_resources_list(id, params); });
    } else if (method == "instance.resources.delete") {
        start_job([this, id, params] {
            rpc_instance_resources_delete(id, params);
        });
    } else if (method == "instance.resources.toggle") {
        start_job([this, id, params] {
            rpc_instance_resources_toggle(id, params);
        });
    } else if (method == "instance.resources.open") {
        rpc_instance_resources_open(id, params);
    } else if (method == "java.list") {
        start_job([this, id, params] { rpc_java_list(id, params); });
    } else if (method == "java.available") {
        start_job([this, id, params] { rpc_java_available(id, params); });
    } else if (method == "java.download") {
        rpc_java_download(id, params);
    } else if (method == "java.delete") {
        start_job([this, id, params] { rpc_java_delete(id, params); });
    } else if (method == "java.uninstall") {
        start_job([this, id, params] { rpc_java_uninstall(id, params); });
    } else if (method == "download.versions") {
        start_job([this, id, params] { rpc_download_versions(id, params); });
    } else if (method == "download.install") {
        rpc_download_install(id, params);
    } else if (method == "download.repair") {
        rpc_download_repair(id, params);
    } else if (method == "download.cancel") {
        rpc_download_cancel(id);
    } else if (method == "download.mirrors") {
        rpc_download_mirrors(id);
    } else if (method == "download.mirrors.refresh") {
        start_job([this, id, params] {
            rpc_download_mirrors_refresh(id, params);
        });
    } else if (method == "loader.versions") {
        start_job([this, id, params] { rpc_loader_versions(id, params); });
    } else if (method == "loader.install") {
        rpc_loader_install(id, params);
    } else if (method == "modrinth.search") {
        start_job([this, id, params] { rpc_modrinth_search(id, params); });
    } else if (method == "modrinth.versions") {
        start_job([this, id, params] { rpc_modrinth_versions(id, params); });
    } else if (method == "modrinth.download") {
        rpc_modrinth_download(id, params);
    } else if (method == "modpack.install") {
        rpc_modpack_install(id, params);
    } else if (method == "dialog.open_modpack") {
        rpc_dialog_open_modpack(id);
    } else if (method == "crash.save_report") {
        rpc_crash_save_report(id, params);
    } else if (method == "crash.open_folder") {
        rpc_crash_open_folder(id, params);
    } else if (method == "auth.status") {
        start_job([this, id, params] { rpc_auth_status(id, params); });
    } else if (method == "auth.device_start") {
        start_job([this, id, params] { rpc_auth_device_start(id, params); });
    } else if (method == "auth.device_poll") {
        start_job([this, id, params] { rpc_auth_device_poll(id, params); });
    } else if (method == "auth.logout") {
        start_job([this, id, params] { rpc_auth_logout(id, params); });
    } else if (method == "game.launch") {
        rpc_game_launch(id, params);
    } else if (method == "game.stop") {
        rpc_game_stop(id);
    } else if (method == "game.status") {
        rpc_game_status(id);
    } else if (method == "window.minimize") {
        rpc_window_minimize(id);
    } else if (method == "window.toggle_maximize") {
        rpc_window_toggle_maximize(id);
    } else if (method == "window.close") {
        rpc_window_close(id);
    } else if (method == "window.drag") {
        rpc_window_drag(id);
    } else if (method == "window.resize") {
        rpc_window_resize(id, params);
    } else if (method == "window.state") {
        rpc_window_state(id);
    } else {
        reply_error(id, "未知方法", method);
    }
}

void Host::send_json(const std::string& json) {
    if (!webview_ || closing_) return;
    webview_->PostWebMessageAsJson(to_wide(json).c_str());
}

void Host::post_json(std::string json) {
    if (closing_ || hwnd_ == nullptr || !::IsWindow(hwnd_)) return;
    auto* payload = new std::string(std::move(json));
    if (!::PostMessageW(hwnd_, kBridgeMessage, 0,
                        reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void Host::reply_ok(int id, nlohmann::json result) {
    post_json(nlohmann::json{{"id", id}, {"result", std::move(result)}}.dump());
}

void Host::reply_error(int id, const Error& error) {
    post_json(nlohmann::json{{"id", id},
                             {"error",
                              {{"message", error.message},
                               {"context", error.context}}}}
                  .dump());
}

void Host::reply_error(int id, std::string message, std::string context) {
    post_json(nlohmann::json{{"id", id},
                             {"error",
                              {{"message", std::move(message)},
                               {"context", std::move(context)}}}}
                  .dump());
}

void Host::push_event(const std::string& name, nlohmann::json data) {
    post_json(nlohmann::json{{"event", name}, {"data", std::move(data)}}.dump());
}

void Host::start_job(std::function<void()> job) {
    std::lock_guard lock(jobs_mutex_);
    jobs_.emplace_back(
        [job = std::move(job)](std::stop_token) mutable { job(); });
}

void Host::rpc_app_info(int id) {
    reply_ok(id, nlohmann::json{
                     {"name", "CMCL"},
                     {"full_name", "C++ Minecraft Launcher"},
                     {"version", "0.5.1"},
                     {"default_mc_dir", path_to_utf8(default_mc_dir())},
                     {"log_path",
                      path_to_utf8(exe_dir() / L"logs" /
                                   L"cmcl-ui.log")}});
}

void Host::rpc_app_open_url(int id, const nlohmann::json& params) {
    const std::string url = param_string(params, "url", "");
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        reply_error(id, "只支持打开 HTTP/HTTPS 链接");
        return;
    }
    const std::wstring wide = to_wide(url);
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", wide.c_str(),
                                             nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        reply_error(id, "无法打开浏览器", url);
        return;
    }
    reply_ok(id, {{"opened", true}});
}

void Host::rpc_instance_list(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));

    auto instances = scan_instances(path_from_utf8(mc_dir));
    if (!instances) {
        reply_error(id, instances.error());
        return;
    }

    const int removed_logs = cleanup_game_logs(path_from_utf8(mc_dir));
    if (removed_logs > 0) {
        Log::info("已清理 " + std::to_string(removed_logs) +
                  " 个超过 7 天的游戏日志");
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto& instance : instances.value()) {
        nlohmann::json entry = {
            {"id", instance.id},
            {"type", instance.type},
            {"loader", instance.loader},
            {"main_class", instance.main_class},
            {"valid", instance.valid},
            {"isolate", instance.isolate},
            {"error", instance.error},
        };
        if (instance.inherits_from) {
            entry["inherits_from"] = *instance.inherits_from;
        }
        list.push_back(std::move(entry));
    }

    reply_ok(id, nlohmann::json{{"mc_dir", mc_dir}, {"instances", list}});
}

void Host::rpc_instance_inspect(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "version", "");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    LaunchOptions options;
    options.mc_dir = path_from_utf8(mc_dir);
    options.version_id = version_id;
    options.player_name = param_string(params, "username", "Player");
    options.max_memory_mb = param_int(params, "max_mem", 4096);
    options.min_memory_mb = param_int(params, "min_mem", 512);
    options.width = param_int(params, "width", 854);
    options.height = param_int(params, "height", 480);
    options.fullscreen = param_bool(params, "fullscreen", false);
    options.server = param_string(params, "server", "");
    const std::string java_path = param_string(params, "java", "");
    if (!java_path.empty()) options.java_exe = path_from_utf8(java_path);

    const std::filesystem::path version_dir =
        options.mc_dir / L"versions" / path_from_utf8(version_id);
    if (auto settings = load_instance_settings(version_dir)) {
        options.isolate = settings.value().isolate;
        options.isolate_saves = settings.value().isolate_saves;
    }

    auto prepared = prepare_launch(options, false);
    if (prepared) {
        const auto& launch = prepared.value();
        reply_ok(
            id,
            nlohmann::json{
                {"id", launch.version.id},
                {"vanilla_id", launch.version.vanilla_id},
                {"jar_id", launch.version.jar_id},
                {"type", launch.version.merged.type},
                {"main_class", launch.main_class},
                {"required_java", mc::required_java_major(launch.version)},
                {"folder", path_to_utf8(version_dir)},
                {"isolate", options.isolate},
                {"isolate_saves", options.isolate_saves},
                {"game_dir", path_to_utf8(launch.game_dir)},
                {"saves_dir", path_to_utf8(launch.saves_dir)},
                {"natives_dir", path_to_utf8(launch.natives_dir)},
                {"classpath_count", classpath_count(launch.classpath)},
                {"command_length",
                 static_cast<unsigned long long>(launch.command_line.size())},
                {"java",
                 {{"version", launch.java_version},
                  {"major", launch.java_major},
                  {"bits", launch.java_bits},
                  {"path", path_to_utf8(launch.java_exe)}}},
                {"jvm_args", launch.jvm_args},
                {"game_args", launch.game_args},
            });
        return;
    }

    nlohmann::json result = {
        {"id", version_id},
        {"folder", path_to_utf8(version_dir)},
        {"isolate", options.isolate},
        {"isolate_saves", options.isolate_saves},
        {"error",
         {{"message", prepared.error().message},
          {"context", prepared.error().context}}},
    };
    if (auto saves = mc::instance_saves_dir(options.mc_dir, version_id)) {
        result["saves_dir"] = path_to_utf8(saves.value());
    }
    if (auto resolved = resolve_version(options.mc_dir / L"versions",
                                        version_id)) {
        const auto& version = resolved.value();
        result["vanilla_id"] = version.vanilla_id;
        result["jar_id"] = version.jar_id;
        result["type"] = version.merged.type;
        result["main_class"] = version.merged.main_class.value_or("");
        result["required_java"] = mc::required_java_major(version);
    }
    reply_ok(id, std::move(result));
}

void Host::rpc_instance_rename(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string old_id = param_string(params, "id", "");
    const std::string new_id = param_string(params, "new_id", "");
    if (old_id.empty() || new_id.empty()) {
        reply_error(id, "版本名不能为空");
        return;
    }

    auto renamed = rename_instance(path_from_utf8(mc_dir), old_id, new_id);
    if (!renamed) {
        reply_error(id, renamed.error());
        return;
    }
    reply_ok(id, {{"id", renamed.value()}});
}

void Host::rpc_instance_delete(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    auto deleted = delete_instance(path_from_utf8(mc_dir), version_id);
    if (!deleted) {
        reply_error(id, deleted.error());
        return;
    }
    reply_ok(id, {{"deleted", true}});
}

void Host::rpc_instance_open_folder(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    auto opened = open_instance_folder(path_from_utf8(mc_dir), version_id);
    if (!opened) {
        reply_error(id, opened.error());
        return;
    }
    reply_ok(id, {{"opened", true}});
}

void Host::rpc_instance_open_saves(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    auto opened = open_instance_saves(path_from_utf8(mc_dir), version_id);
    if (!opened) {
        reply_error(id, opened.error());
        return;
    }
    reply_ok(id, {{"opened", true}});
}

void Host::rpc_instance_settings_get(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    const std::filesystem::path version_dir =
        path_from_utf8(mc_dir) / L"versions" / path_from_utf8(version_id);
    auto settings = load_instance_settings(version_dir);
    if (!settings) {
        reply_error(id, settings.error());
        return;
    }
    reply_ok(id, {{"isolate", settings.value().isolate},
                  {"isolate_saves", settings.value().isolate_saves}});
}

void Host::rpc_instance_settings_set(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    const std::filesystem::path version_dir =
        path_from_utf8(mc_dir) / L"versions" / path_from_utf8(version_id);
    std::error_code ec;
    if (!std::filesystem::is_directory(version_dir, ec)) {
        reply_error(id, "版本目录不存在", path_to_utf8(version_dir));
        return;
    }

    InstanceSettings settings;
    if (auto current = load_instance_settings(version_dir)) {
        settings = current.value();
    }
    if (params.contains("isolate") && params["isolate"].is_boolean()) {
        settings.isolate = params["isolate"].get<bool>();
    }
    if (params.contains("isolate_saves") && params["isolate_saves"].is_boolean()) {
        settings.isolate_saves = params["isolate_saves"].get<bool>();
    }

    auto saved = save_instance_settings(version_dir, settings);
    if (!saved) {
        reply_error(id, saved.error());
        return;
    }
    reply_ok(id, {{"isolate", settings.isolate},
                  {"isolate_saves", settings.isolate_saves}});
}

void Host::rpc_instance_resources_list(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    const std::string kind = param_string(params, "kind", "mods");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    auto resources = list_instance_resources(path_from_utf8(mc_dir), version_id,
                                             kind);
    if (!resources) {
        reply_error(id, resources.error());
        return;
    }

    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : resources.value().entries) {
        entries.push_back({{"name", entry.name},
                           {"directory", entry.directory},
                           {"enabled", entry.enabled},
                           {"size", entry.size},
                           {"modified", entry.modified}});
    }
    reply_ok(id, {{"dir", path_to_utf8(resources.value().dir)},
                  {"isolate", resources.value().isolate},
                  {"isolate_saves", resources.value().isolate_saves},
                  {"shared", resources.value().shared},
                  {"entries", std::move(entries)}});
}

void Host::rpc_instance_resources_delete(int id,
                                         const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    const std::string kind = param_string(params, "kind", "mods");
    const std::string name = param_string(params, "name", "");
    if (version_id.empty() || name.empty()) {
        reply_error(id, "缺少版本或资源文件名");
        return;
    }

    auto deleted = delete_instance_resource(path_from_utf8(mc_dir), version_id,
                                            kind, name);
    if (!deleted) {
        reply_error(id, deleted.error());
        return;
    }
    reply_ok(id, {{"deleted", true}, {"name", name}});
}

void Host::rpc_instance_resources_toggle(int id,
                                         const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    const std::string kind = param_string(params, "kind", "mods");
    const std::string name = param_string(params, "name", "");
    if (version_id.empty() || name.empty()) {
        reply_error(id, "缺少版本或资源文件名");
        return;
    }

    auto renamed = toggle_instance_resource(path_from_utf8(mc_dir), version_id,
                                            kind, name);
    if (!renamed) {
        reply_error(id, renamed.error());
        return;
    }
    reply_ok(id, {{"name", renamed.value()}});
}

void Host::rpc_instance_resources_open(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "id", "");
    const std::string kind = param_string(params, "kind", "mods");
    if (version_id.empty()) {
        reply_error(id, "没有指定版本");
        return;
    }

    auto opened = open_instance_resources(path_from_utf8(mc_dir), version_id,
                                          kind);
    if (!opened) {
        reply_error(id, opened.error());
        return;
    }
    reply_ok(id, {{"opened", true}});
}

void Host::rpc_java_list(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const auto javas = detect_java(path_from_utf8(mc_dir), true);
    nlohmann::json list = nlohmann::json::array();
    for (const auto& java : javas) {
        list.push_back({{"version", java.version},
                        {"major", java.major},
                        {"bits", java.bits},
                        {"managed", java.managed},
                        {"path", path_to_utf8(java.exe)}});
    }
    reply_ok(id, nlohmann::json{{"javas", list}});
}

void Host::rpc_java_available(int id, const nlohmann::json& params) {
    const bool force = param_bool(params, "force", false);
    auto releases = mc::fetch_openjdk_releases(force);
    if (!releases) {
        reply_error(id, releases.error());
        return;
    }
    nlohmann::json list = nlohmann::json::array();
    for (const auto& release : releases.value()) {
        list.push_back({{"feature", release.feature},
                        {"version", release.version},
                        {"release_name", release.release_name},
                        {"file_name", release.file_name},
                        {"url", release.url},
                        {"mirror_url", release.mirror_url},
                        {"sha256", release.sha256},
                        {"size", release.size}});
    }
    reply_ok(id, {{"releases", std::move(list)}});
}

void Host::rpc_java_download(int id, const nlohmann::json& params) {
    bool expected = false;
    if (!download_busy_.compare_exchange_strong(expected, true)) {
        reply_error(id, "已有下载任务在进行，请稍候");
        return;
    }

    const int feature = param_int(params, "feature", 0);
    if (feature < 8 || feature > 30) {
        download_busy_ = false;
        reply_error(id, "不支持的 Java 主版本", std::to_string(feature));
        return;
    }
    const bool force = param_bool(params, "force", false);

    download_cancel_ = false;
    start_job([this, id, feature, force] {
        auto result = mc::install_openjdk(
            feature, force,
            [this](const DownloadProgress& progress) {
                push_download_progress(progress);
            },
            download_cancel_);
        download_busy_ = false;
        if (closing_) return;
        if (!result) {
            reply_error(id, result.error());
            return;
        }
        const auto& java = result.value();
        reply_ok(id, {{"installed", true},
                      {"java",
                       {{"version", java.version},
                        {"major", java.major},
                        {"bits", java.bits},
                        {"managed", java.managed},
                        {"path", path_to_utf8(java.exe)}}}});
    });
}

void Host::rpc_java_delete(int id, const nlohmann::json& params) {
    const std::string path = param_string(params, "path", "");
    if (path.empty()) {
        reply_error(id, "缺少 Java 路径");
        return;
    }
    auto result = mc::delete_managed_jdk(path_from_utf8(path));
    if (!result) {
        reply_error(id, result.error());
        return;
    }
    reply_ok(id, {{"deleted", true}});
}

void Host::rpc_java_uninstall(int id, const nlohmann::json& params) {
    const std::string path = param_string(params, "path", "");
    if (path.empty()) {
        reply_error(id, "缺少 Java 路径");
        return;
    }
    auto result = mc::open_java_uninstaller(path_from_utf8(path));
    if (!result) {
        reply_error(id, result.error());
        return;
    }
    reply_ok(id, {{"opened", true}});
}

void Host::push_download_progress(const mc::DownloadProgress& progress) {
    push_event("download.progress",
               nlohmann::json{
                   {"stage", progress.stage},
                   {"message", progress.message},
                   {"file", progress.file},
                   {"completed", progress.completed_files},
                   {"total", progress.total_files},
                   {"bytes", progress.downloaded_bytes},
                   {"total_bytes", progress.total_bytes},
               });
}

void Host::rpc_download_versions(int id, const nlohmann::json& params) {
    const auto source =
        parse_download_source(param_string(params, "source", "auto"));
    const bool force = param_bool(params, "force", false);

    auto manifest = mc::fetch_version_manifest(source, force);
    if (!manifest) {
        reply_error(id, manifest.error());
        return;
    }

    nlohmann::json versions = nlohmann::json::array();
    for (const auto& version : manifest.value().versions) {
        versions.push_back({{"id", version.id},
                            {"type", version.type},
                            {"url", version.url},
                            {"sha1", version.sha1},
                            {"size", version.size},
                            {"release_time", version.release_time}});
    }
    reply_ok(id, {{"latest_release", manifest.value().latest_release},
                  {"latest_snapshot", manifest.value().latest_snapshot},
                  {"source", mc::download_source_name(source)},
                  {"versions", std::move(versions)}});
}

void Host::rpc_download_install(int id, const nlohmann::json& params) {
    bool expected = false;
    if (!download_busy_.compare_exchange_strong(expected, true)) {
        reply_error(id, "已有下载任务在进行，请稍候");
        return;
    }

    DownloadRequest request;
    request.mc_dir =
        path_from_utf8(param_string(params, "mc_dir",
                                    path_to_utf8(default_mc_dir())));
    request.version_id = param_string(params, "version", "");
    request.instance_name = param_string(params, "name", "");
    request.source =
        parse_download_source(param_string(params, "source", "auto"));

    if (request.version_id.empty()) {
        download_busy_ = false;
        reply_error(id, "没有指定 Minecraft 版本");
        return;
    }

    download_cancel_ = false;
    start_job([this, id, request] {
        auto result = mc::install_version(
            request,
            [this](const DownloadProgress& progress) {
                push_download_progress(progress);
            },
            download_cancel_);
        download_busy_ = false;
        if (closing_) return;
        if (!result) {
            reply_error(id, result.error());
        } else {
            reply_ok(id, {{"installed", true}});
        }
    });
}

void Host::rpc_download_repair(int id, const nlohmann::json& params) {
    bool expected = false;
    if (!download_busy_.compare_exchange_strong(expected, true)) {
        reply_error(id, "已有下载任务在进行，请稍候");
        return;
    }

    DownloadRequest request;
    request.mc_dir =
        path_from_utf8(param_string(params, "mc_dir",
                                    path_to_utf8(default_mc_dir())));
    request.version_id = param_string(params, "version", "");
    if (request.version_id.empty()) {
        request.version_id = param_string(params, "id", "");
    }
    request.source =
        parse_download_source(param_string(params, "source", "auto"));

    if (request.version_id.empty()) {
        download_busy_ = false;
        reply_error(id, "没有指定版本");
        return;
    }

    download_cancel_ = false;
    start_job([this, id, request] {
        auto result = mc::repair_version(
            request,
            [this](const DownloadProgress& progress) {
                push_download_progress(progress);
            },
            download_cancel_);
        download_busy_ = false;
        if (closing_) return;
        if (!result) {
            reply_error(id, result.error());
        } else {
            reply_ok(id, {{"repaired", true}});
        }
    });
}

void Host::rpc_download_cancel(int id) {
    if (!download_busy_.load()) {
        reply_error(id, "没有正在进行的下载任务");
        return;
    }
    download_cancel_ = true;
    reply_ok(id, {{"cancelling", true}});
}

void Host::rpc_download_mirrors(int id) {
    nlohmann::json mirrors = nlohmann::json::array();
    for (const auto& mirror : mc::available_mirrors()) {
        nlohmann::json replacements = nlohmann::json::array();
        for (const auto& replacement : mirror.replacements) {
            replacements.push_back({{"from", replacement.from},
                                    {"to", replacement.to}});
        }
        mirrors.push_back({{"id", mirror.id},
                           {"name", mirror.name},
                           {"manifest", mirror.manifest_url},
                           {"priority", mirror.priority},
                           {"builtin", mirror.builtin},
                           {"replacements", std::move(replacements)}});
    }
    reply_ok(id, {{"mirrors", std::move(mirrors)}});
}

void Host::rpc_download_mirrors_refresh(int id,
                                        const nlohmann::json& params) {
    const std::string url = param_string(params, "url", "");
    auto result = mc::refresh_mirrors(url);
    if (!result) {
        reply_error(id, result.error());
        return;
    }
    reply_ok(id, {{"refreshed", true}});
}

void Host::rpc_loader_versions(int id, const nlohmann::json& params) {
    const std::string loader = param_string(params, "loader", "");
    const std::string game_version = param_string(params, "game_version", "");
    auto kind = mc::parse_loader_kind(loader);
    if (!kind) {
        reply_error(id, "未知的加载器", loader);
        return;
    }
    if (game_version.empty()) {
        reply_error(id, "没有指定 Minecraft 版本");
        return;
    }

    auto versions = mc::fetch_loader_versions(kind.value(), game_version);
    if (!versions) {
        reply_error(id, versions.error());
        return;
    }
    nlohmann::json list = nlohmann::json::array();
    for (const auto& version : versions.value()) {
        list.push_back({{"version", version.version},
                        {"stable", version.stable}});
    }
    reply_ok(id, {{"loader", loader},
                  {"game_version", game_version},
                  {"versions", std::move(list)}});
}

void Host::rpc_loader_install(int id, const nlohmann::json& params) {
    bool expected = false;
    if (!download_busy_.compare_exchange_strong(expected, true)) {
        reply_error(id, "已有下载任务在进行，请稍候");
        return;
    }

    const std::string loader = param_string(params, "loader", "");
    auto kind = mc::parse_loader_kind(loader);
    if (!kind) {
        download_busy_ = false;
        reply_error(id, "未知的加载器", loader);
        return;
    }

    mc::LoaderInstallRequest request;
    request.mc_dir =
        path_from_utf8(param_string(params, "mc_dir",
                                    path_to_utf8(default_mc_dir())));
    request.game_version = param_string(params, "game_version", "");
    request.loader_version = param_string(params, "loader_version", "");
    request.instance_name = param_string(params, "name", "");
    request.kind = kind.value();
    request.source =
        parse_download_source(param_string(params, "source", "auto"));
    if (request.game_version.empty() || request.loader_version.empty()) {
        download_busy_ = false;
        reply_error(id, "缺少 Minecraft 或加载器版本");
        return;
    }

    download_cancel_ = false;
    start_job([this, id, request] {
        auto result = mc::install_loader(
            request,
            [this](const DownloadProgress& progress) {
                push_download_progress(progress);
            },
            download_cancel_);
        download_busy_ = false;
        if (closing_) return;
        if (!result) {
            reply_error(id, result.error());
            return;
        }
        reply_ok(id, {{"installed", true},
                      {"name", request.instance_name.empty()
                                   ? request.game_version + "-" +
                                         mc::loader_display_name(request.kind)
                                   : request.instance_name}});
    });
}

void Host::rpc_modrinth_search(int id, const nlohmann::json& params) {
    auto result = mc::search_modrinth(
        param_string(params, "query", ""),
        param_string(params, "type", "mod"),
        param_string(params, "game_version", ""),
        param_string(params, "loader", ""),
        param_string(params, "index", "relevance"),
        param_int(params, "offset", 0),
        param_int(params, "limit", 20));
    if (!result) {
        reply_error(id, result.error());
        return;
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto& project : result.value().projects) {
        list.push_back({
            {"id", project.id},
            {"slug", project.slug},
            {"title", project.title},
            {"description", project.description},
            {"icon_url", project.icon_url},
            {"author", project.author},
            {"project_type", project.project_type},
            {"date_modified", project.date_modified},
            {"downloads", project.downloads},
            {"follows", project.follows},
            {"categories", project.categories},
            {"game_versions", project.game_versions},
        });
    }
    reply_ok(id, {{"projects", std::move(list)},
                  {"total_hits", result.value().total_hits},
                  {"offset", result.value().offset},
                  {"limit", result.value().limit}});
}

void Host::rpc_modrinth_versions(int id, const nlohmann::json& params) {
    auto result = mc::list_modrinth_versions(
        param_string(params, "project_id", ""),
        param_string(params, "game_version", ""),
        param_string(params, "loader", ""));
    if (!result) {
        reply_error(id, result.error());
        return;
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto& version : result.value()) {
        const mc::ModrinthFile* file = version.primary_file();
        list.push_back({
            {"id", version.id},
            {"name", version.name},
            {"version_number", version.version_number},
            {"version_type", version.version_type},
            {"date_published", version.date_published},
            {"game_versions", version.game_versions},
            {"loaders", version.loaders},
            {"file",
             file == nullptr
                 ? nlohmann::json::object()
                 : nlohmann::json{{"filename", file->filename},
                                  {"size", file->size},
                                  {"sha1", file->sha1}}},
        });
    }
    reply_ok(id, {{"versions", std::move(list)}});
}

void Host::rpc_modrinth_download(int id, const nlohmann::json& params) {
    const std::string version_id = param_string(params, "version_id", "");
    if (version_id.empty()) {
        reply_error(id, "缺少 Modrinth 版本 ID");
        return;
    }

    bool expected = false;
    if (!download_busy_.compare_exchange_strong(expected, true)) {
        reply_error(id, "已有下载任务在进行，请稍候");
        return;
    }

    const std::string mc_dir_text =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string instance_id = param_string(params, "instance_id", "");
    const std::string kind = param_string(params, "kind", "mod");

    const std::filesystem::path mc_dir = path_from_utf8(mc_dir_text);
    std::filesystem::path target_dir;
    if (kind == "modpack") {
        target_dir = mc_dir / L"modpacks";
    } else {
        std::string resource_kind;
        if (kind == "mod") {
            resource_kind = "mods";
        } else if (kind == "resourcepack") {
            resource_kind = "resourcepacks";
        } else if (kind == "shader") {
            resource_kind = "shaderpacks";
        } else if (kind == "datapack") {
            resource_kind = "datapacks";
        } else {
            download_busy_ = false;
            reply_error(id, "未知的资源类型", kind);
            return;
        }
        if (kind == "mod" && instance_id.empty()) {
            download_busy_ = false;
            reply_error(id, "安装 Mod 需要先选择一个实例");
            return;
        }

        auto dir = mc::instance_resource_dir(mc_dir, instance_id, resource_kind);
        if (!dir) {
            download_busy_ = false;
            reply_error(id, dir.error());
            return;
        }
        target_dir = dir.value();
    }

    download_cancel_ = false;
    start_job([this, id, version_id, target_dir, kind] {
        auto version = mc::get_modrinth_version(version_id);
        if (!version) {
            download_busy_ = false;
            if (!closing_) reply_error(id, version.error());
            return;
        }
        auto result = mc::download_modrinth_file(
            version.value(), target_dir,
            [this](const DownloadProgress& progress) {
                push_download_progress(progress);
            },
            download_cancel_);
        download_busy_ = false;
        if (closing_) return;
        if (!result) {
            reply_error(id, result.error());
            return;
        }
        reply_ok(id, {{"path", result.value()},
                      {"kind", kind},
                      {"filename", path_to_utf8(
                                       std::filesystem::path(
                                           path_from_utf8(result.value()))
                                           .filename())}});
    });
}

void Host::rpc_modpack_install(int id, const nlohmann::json& params) {
    bool expected = false;
    if (!download_busy_.compare_exchange_strong(expected, true)) {
        reply_error(id, "已有下载任务在进行，请稍候");
        return;
    }

    const std::string mc_dir_text =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "version_id", "");
    const std::string archive_text = param_string(params, "path", "");
    const std::string instance_name = param_string(params, "instance_name", "");
    const std::string source_text = param_string(params, "source", "");
    if (version_id.empty() && archive_text.empty()) {
        download_busy_ = false;
        reply_error(id, "缺少整合包版本或文件路径");
        return;
    }

    download_cancel_ = false;
    start_job([this, id, mc_dir_text, version_id, archive_text, instance_name,
               source_text] {
        std::filesystem::path archive = path_from_utf8(archive_text);

        if (!version_id.empty()) {
            auto version = mc::get_modrinth_version(version_id);
            if (!version) {
                download_busy_ = false;
                if (!closing_) reply_error(id, version.error());
                return;
            }
            const mc::ModrinthFile* file = version.value().primary_file();
            std::string filename = file == nullptr ? std::string{} : file->filename;
            std::transform(filename.begin(), filename.end(), filename.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (filename.size() < 7 ||
                filename.compare(filename.size() - 7, 7, ".mrpack") != 0) {
                download_busy_ = false;
                if (!closing_) {
                    reply_error(id, "这个项目的主文件不是 .mrpack 整合包");
                }
                return;
            }

            const std::filesystem::path modpacks =
                path_from_utf8(mc_dir_text) / L"modpacks";
            auto downloaded = mc::download_modrinth_file(
                version.value(), modpacks,
                [this](const DownloadProgress& progress) {
                    push_download_progress(progress);
                },
                download_cancel_);
            if (!downloaded) {
                download_busy_ = false;
                if (!closing_) reply_error(id, downloaded.error());
                return;
            }
            archive = path_from_utf8(downloaded.value());
        } else {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(archive, ec)) {
                download_busy_ = false;
                if (!closing_) {
                    reply_error(id, "整合包文件不存在", archive_text);
                }
                return;
            }
            std::string extension = path_to_utf8(archive.extension());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            if (extension != ".mrpack" && extension != ".zip") {
                download_busy_ = false;
                if (!closing_) {
                    reply_error(id, "只支持 .mrpack 或 .zip 整合包",
                                archive_text);
                }
                return;
            }
        }

        ModpackImportRequest request;
        request.mc_dir = path_from_utf8(mc_dir_text);
        request.archive = archive;
        request.instance_name = instance_name;
        request.source = mc::parse_download_source(source_text);

        auto result = import_modrinth_modpack(
            request,
            [this](const DownloadProgress& progress) {
                push_download_progress(progress);
            },
            download_cancel_);
        download_busy_ = false;
        if (closing_) return;
        if (!result) {
            reply_error(id, result.error());
            return;
        }
        reply_ok(id, {{"instance_id", result.value().instance_id},
                      {"name", result.value().name},
                      {"pack_version", result.value().pack_version},
                      {"game_version", result.value().game_version},
                      {"loader", result.value().loader},
                      {"loader_version", result.value().loader_version},
                      {"files_downloaded", result.value().files_downloaded},
                      {"files_skipped", result.value().files_skipped}});
    });
}

void Host::rpc_dialog_open_modpack(int id) {
    wchar_t file[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter =
        L"整合包 (*.mrpack;*.zip)\0*.mrpack;*.zip\0"
        L"Modrinth 整合包 (*.mrpack)\0*.mrpack\0"
        L"ZIP 压缩包 (*.zip)\0*.zip\0"
        L"所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    dialog.lpstrTitle = L"选择整合包";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameW(&dialog)) {
        reply_ok(id, {{"path", ""}});
        return;
    }
    reply_ok(id, {{"path", path_to_utf8(std::filesystem::path(file))}});
}

void Host::rpc_crash_save_report(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string text = param_string(params, "text", "");
    if (text.empty()) {
        reply_error(id, "崩溃报告内容为空");
        return;
    }
    if (text.size() > 4 * 1024 * 1024) {
        reply_error(id, "崩溃报告过大", "请只保留崩溃相关的日志片段");
        return;
    }

    const std::filesystem::path crash_dir =
        path_from_utf8(mc_dir) / L"crash-reports";
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    if (ec) {
        reply_error(id, "无法创建 crash-reports 目录",
                    path_to_utf8(crash_dir) + ": " + ec.message());
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    const std::filesystem::path path =
        crash_dir / path_from_utf8(std::string("CMCL-崩溃分析-") + stamp +
                                   ".txt");

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        reply_error(id, "无法写入崩溃报告", path_to_utf8(path));
        return;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) {
        reply_error(id, "写入崩溃报告失败", path_to_utf8(path));
        return;
    }
    reply_ok(id, {{"path", path_to_utf8(path)}});
}

void Host::rpc_crash_open_folder(int id, const nlohmann::json& params) {
    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::filesystem::path dir =
        path_from_utf8(mc_dir) / L"crash-reports";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        reply_error(id, "无法创建 crash-reports 目录",
                    path_to_utf8(dir) + ": " + ec.message());
        return;
    }
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        reply_error(id, "无法打开 crash-reports 目录", path_to_utf8(dir));
        return;
    }
    reply_ok(id, {{"opened", true}});
}

void Host::rpc_auth_status(int id, const nlohmann::json&) {
    auto loaded = ms_account_load();
    if (!loaded) {
        reply_error(id, loaded.error());
        return;
    }
    if (!loaded.value()) {
        reply_ok(id, {{"logged_in", false}});
        return;
    }

    const MsAccount& account = *loaded.value();
    reply_ok(id, {{"logged_in", true},
                  {"account",
                   {{"name", account.name},
                    {"uuid", account.uuid},
                    {"expires_at", account.expires_at},
                    {"expired", ms_account_expired(account, 0)}}}});
}

void Host::rpc_auth_device_start(int id, const nlohmann::json&) {
    auto code = ms_device_code_begin();
    if (!code) {
        reply_error(id, code.error());
        return;
    }

    {
        std::lock_guard lock(auth_mutex_);
        auth_device_ = code.value();
    }

    reply_ok(id, {{"user_code", code.value().user_code},
                  {"verification_uri", code.value().verification_uri},
                  {"message", code.value().message},
                  {"expires_in", code.value().expires_in},
                  {"interval", code.value().interval}});
}

void Host::rpc_auth_device_poll(int id, const nlohmann::json&) {
    MsDeviceCode code;
    {
        std::lock_guard lock(auth_mutex_);
        if (!auth_device_) {
            reply_error(id, "请先获取设备码");
            return;
        }
        code = *auth_device_;
    }

    auto polled = ms_device_code_poll(code);
    if (!polled) {
        reply_error(id, polled.error());
        return;
    }

    const mc::MsPollResult result = std::move(polled).value();
    if (result.status == MsPollStatus::Success) {
        auto saved = ms_account_save(result.account);
        {
            std::lock_guard lock(auth_mutex_);
            auth_device_.reset();
        }
        nlohmann::json reply = {
            {"status", ms_poll_status_name(result.status)},
            {"message", result.message},
            {"account",
             {{"name", result.account.name}, {"uuid", result.account.uuid}}},
        };
        if (!saved) {
            reply["warning"] =
                "登录成功，但账号无法保存到本地：" +
                saved.error().to_string();
        }
        reply_ok(id, std::move(reply));
        return;
    }

    if (result.status != MsPollStatus::Pending &&
        result.status != MsPollStatus::SlowDown) {
        std::lock_guard lock(auth_mutex_);
        auth_device_.reset();
    }

    reply_ok(id, {{"status", ms_poll_status_name(result.status)},
                  {"message", result.message},
                  {"interval", result.interval}});
}

void Host::rpc_auth_logout(int id, const nlohmann::json&) {
    {
        std::lock_guard lock(auth_mutex_);
        auth_device_.reset();
    }
    auto cleared = ms_account_clear();
    if (!cleared) {
        reply_error(id, cleared.error());
        return;
    }
    reply_ok(id, {{"logged_in", false}});
}

void Host::rpc_game_launch(int id, const nlohmann::json& params) {
    bool expected = false;
    if (!launching_.compare_exchange_strong(expected, true)) {
        reply_error(id, "正在启动中，请稍候");
        return;
    }
    {
        std::lock_guard lock(game_mutex_);
        if (game_ && game_->running()) {
            launching_ = false;
            reply_error(id, "已有游戏在运行");
            return;
        }
        game_.reset();
    }

    const std::string mc_dir =
        param_string(params, "mc_dir", path_to_utf8(default_mc_dir()));
    const std::string version_id = param_string(params, "version", "");
    if (version_id.empty()) {
        launching_ = false;
        reply_error(id, "没有指定版本");
        return;
    }

    LaunchOptions options;
    options.mc_dir = path_from_utf8(mc_dir);
    options.version_id = version_id;
    options.player_name = param_string(params, "username", "Player");
    options.max_memory_mb = param_int(params, "max_mem", 4096);
    options.min_memory_mb = param_int(params, "min_mem", 512);
    options.width = param_int(params, "width", 854);
    options.height = param_int(params, "height", 480);
    options.fullscreen = param_bool(params, "fullscreen", false);
    InstanceSettings instance_settings;
    if (auto loaded = load_instance_settings(
            options.mc_dir / L"versions" / path_from_utf8(version_id))) {
        instance_settings = loaded.value();
    }
    options.isolate = instance_settings.isolate;
    options.isolate_saves = instance_settings.isolate_saves;
    if (const auto it = params.find("isolate");
        it != params.end() && it->is_boolean()) {
        options.isolate = it->get<bool>();
    }
    if (const auto it = params.find("isolate_saves");
        it != params.end() && it->is_boolean()) {
        options.isolate_saves = it->get<bool>();
    }
    options.server = param_string(params, "server", "");
    const std::string java_path = param_string(params, "java", "");
    if (!java_path.empty()) options.java_exe = path_from_utf8(java_path);

    {
        std::lock_guard lock(run_log_mutex_);
        run_log_.clear();
    }
    stop_requested_ = false;
    run_started_ = std::chrono::steady_clock::now();

    std::optional<MsAccount> account;
    if (auto loaded = ms_account_load()) {
        account = loaded.value();
    } else {
        Log::warn("读取本地微软账号失败，将离线启动：" +
                  loaded.error().to_string());
    }

    start_job([this, id, options, account]() mutable {
        if (closing_) {
            launching_ = false;
            return;
        }

        if (account) {
            // 访问令牌通常 24 小时过期；过期时用 refresh_token 换新。
            auto ensured = ms_account_ensure(*account, 300);
            if (!ensured) {
                launching_ = false;
                reply_error(id, ensured.error());
                return;
            }
            const MsAccount& current = ensured.value();
            options.player_name = current.name;
            options.uuid = current.uuid;
            options.access_token = current.access_token;
            options.user_type = "msa";

            if (current.access_token != account->access_token ||
                current.refresh_token != account->refresh_token) {
                if (auto saved = ms_account_save(current); !saved) {
                    Log::warn("保存刷新后的微软账号失败：" +
                              saved.error().to_string());
                }
            }
            Log::info("使用微软账号启动：" + current.name);
        }

        auto prepared = prepare_launch(options, true);
        if (!prepared) {
            launching_ = false;
            reply_error(id, prepared.error());
            return;
        }
        if (closing_) {
            launching_ = false;
            return;
        }

        auto started = launch_prepared(
            prepared.value(), [this](std::string_view line) {
                const std::string text(line);
                {
                    std::lock_guard lock(run_log_mutex_);
                    run_log_.push_back(text);
                    if (run_log_.size() > 8000) {
                        run_log_.erase(run_log_.begin(),
                                       run_log_.begin() + 2000);
                    }
                }
                push_event("game.log", {{"line", text}});
            });
        if (!started) {
            launching_ = false;
            reply_error(id, started.error());
            return;
        }

        auto process = std::move(started).value();
        if (closing_) {
            process->terminate();
            launching_ = false;
            return;
        }

        const unsigned long pid = process->pid();
        {
            std::lock_guard lock(game_mutex_);
            game_ = std::move(process);
        }
        launching_ = false;
        reply_ok(id, {{"started", true}, {"pid", pid}});

        Process* running = nullptr;
        {
            std::lock_guard lock(game_mutex_);
            running = game_.get();
        }
        const unsigned long code = running ? running->wait() : 0;
        {
            std::lock_guard lock(game_mutex_);
            if (game_.get() == running) game_.reset();
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - run_started_)
                                 .count();

        std::vector<std::string> run_lines;
        {
            std::lock_guard lock(run_log_mutex_);
            run_lines = run_log_;
        }

        const std::filesystem::path latest_log =
            options.mc_dir / L"logs" / L"latest.log";
        std::error_code log_ec;
        const auto write_time =
            std::filesystem::last_write_time(latest_log, log_ec);
        if (!log_ec &&
            (std::filesystem::file_time_type::clock::now() - write_time) <
                std::chrono::minutes(10)) {
            const auto tail = mc::read_log_tail(latest_log);
            run_lines.insert(run_lines.end(), tail.begin(), tail.end());
        }

        // 读取最近 30 分钟内生成的崩溃报告，补充 latest.log 里没有的
        // Description / Caused by / 模组列表。
        std::filesystem::path crash_report;
        std::vector<std::string> crash_lines;
        const std::filesystem::path crash_dir =
            options.mc_dir / L"crash-reports";
        std::error_code crash_ec;
        std::filesystem::file_time_type newest_crash{};
        for (std::filesystem::directory_iterator it(crash_dir, crash_ec), end;
             !crash_ec && it != end; it.increment(crash_ec)) {
            if (!it->is_regular_file(crash_ec)) continue;
            const auto modified = it->last_write_time(crash_ec);
            if (crash_ec) continue;
            if ((std::filesystem::file_time_type::clock::now() - modified) >
                std::chrono::minutes(30)) {
                continue;
            }
            if (crash_report.empty() || modified > newest_crash) {
                crash_report = it->path();
                newest_crash = modified;
            }
        }
        if (!crash_report.empty()) {
            crash_lines = mc::read_log_tail(crash_report, 500);
            run_lines.push_back("========== crash report: " +
                                path_to_utf8(crash_report.filename()) +
                                " ==========");
            run_lines.insert(run_lines.end(), crash_lines.begin(),
                             crash_lines.end());
        }

        const bool manual_stop = stop_requested_.exchange(false);
        mc::RunSummary summary;
        if (manual_stop) {
            summary.title = "游戏已停止";
            summary.detail = "已通过启动器停止游戏。";
            summary.suggestions = {
                "如果画面没有立即消失，请稍等几秒。",
                "下次可以优先在游戏内退出，让世界正常保存。",
            };
            summary.severity = "info";
            summary.normal = true;
        } else {
            mc::AnalysisContext context;
            context.game_version = prepared.value().version.vanilla_id;
            context.java_version = prepared.value().java_version;
            context.java_major = prepared.value().java_major;
            context.max_memory_mb = options.max_memory_mb;
            if (auto scanned = mc::scan_instances(options.mc_dir)) {
                for (const auto& item : scanned.value()) {
                    if (item.id == options.version_id) {
                        context.loader = item.loader;
                        break;
                    }
                }
            }
            summary =
                mc::analyze_run(static_cast<int>(code), run_lines, context);
        }

        nlohmann::json suggestions = nlohmann::json::array();
        for (const auto& suggestion : summary.suggestions) {
            suggestions.push_back(suggestion);
        }

        nlohmann::json findings_json = nlohmann::json::array();
        for (const auto& finding : summary.findings) {
            nlohmann::json items = nlohmann::json::array();
            for (const auto& suggestion : finding.suggestions) {
                items.push_back(suggestion);
            }
            findings_json.push_back(
                {{"id", finding.id},
                 {"category", finding.category},
                 {"title", finding.title},
                 {"detail", finding.detail},
                 {"severity", finding.severity},
                 {"time", finding.time},
                 {"mod_a", finding.mod_a},
                 {"mod_b", finding.mod_b},
                 {"event", finding.event},
                 {"evidence", finding.evidence},
                 {"suggestions", std::move(items)}});
        }

        std::string log_tail_text;
        const size_t log_begin =
            run_lines.size() > 250 ? run_lines.size() - 250 : 0;
        for (size_t i = log_begin; i < run_lines.size(); ++i) {
            if (!log_tail_text.empty()) log_tail_text += '\n';
            log_tail_text += run_lines[i];
            if (log_tail_text.size() > 96 * 1024) {
                log_tail_text.resize(96 * 1024);
                log_tail_text += "\n...";
                break;
            }
        }
        std::string crash_tail_text;
        const size_t crash_begin =
            crash_lines.size() > 300 ? crash_lines.size() - 300 : 0;
        for (size_t i = crash_begin; i < crash_lines.size(); ++i) {
            if (!crash_tail_text.empty()) crash_tail_text += '\n';
            crash_tail_text += crash_lines[i];
            if (crash_tail_text.size() > 64 * 1024) {
                crash_tail_text.resize(64 * 1024);
                crash_tail_text += "\n...";
                break;
            }
        }
        push_event("game.exit",
                   {{"code", static_cast<long long>(code)},
                    {"manual", manual_stop}});
        push_event("game.summary",
                   {{"title", summary.title},
                    {"detail", summary.detail},
                    {"severity", summary.severity},
                    {"normal", summary.normal},
                    {"unknown", summary.unknown},
                    {"manual", manual_stop},
                    {"instance", options.version_id},
                    {"suggestions", std::move(suggestions)},
                    {"findings", std::move(findings_json)},
                    {"context",
                     {{"game_version", summary.context.game_version},
                      {"loader", summary.context.loader},
                      {"java_version", summary.context.java_version},
                      {"java_major", summary.context.java_major},
                      {"max_memory_mb", summary.context.max_memory_mb}}},
                    {"log_tail", std::move(log_tail_text)},
                    {"crash_report_path", path_to_utf8(crash_report)},
                    {"crash_report_tail", std::move(crash_tail_text)},
                    {"exit_code", static_cast<long long>(code)},
                    {"duration_seconds", elapsed}});
        Log::info("运行总结：" + summary.title + " — " + summary.detail);
    });
}

void Host::rpc_game_status(int id) {
    std::lock_guard lock(game_mutex_);
    const bool running = game_ && game_->running();
    reply_ok(id, {{"running", running},
                  {"pid", running ? game_->pid() : 0}});
}

void Host::rpc_game_stop(int id) {
    {
        std::lock_guard lock(game_mutex_);
        if (!game_ || !game_->running()) {
            reply_error(id, "没有正在运行的游戏");
            return;
        }
        stop_requested_ = true;
        game_->terminate();
    }
    reply_ok(id, {{"stopped", true}});
}

void Host::rpc_window_minimize(int id) {
    ::ShowWindow(hwnd_, SW_MINIMIZE);
    reply_ok(id, {{"minimized", true}});
}

void Host::rpc_window_toggle_maximize(int id) {
    const bool maximized = ::IsZoomed(hwnd_) != 0;
    ::ShowWindow(hwnd_, maximized ? SW_RESTORE : SW_MAXIMIZE);
    reply_ok(id, {{"maximized", !maximized}});
}

void Host::rpc_window_close(int id) {
    reply_ok(id, {{"closing", true}});
    ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void Host::rpc_window_drag(int id) {
    reply_ok(id, {{"dragging", true}});
    POINT point{};
    ::GetCursorPos(&point);
    ::ReleaseCapture();
    ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION,
                   MAKELPARAM(point.x, point.y));
}

void Host::rpc_window_resize(int id, const nlohmann::json& params) {
    if (::IsZoomed(hwnd_)) {
        reply_error(id, "窗口已最大化");
        return;
    }

    const std::string edge = param_string(params, "edge", "");
    int hit = 0;
    if (edge == "left") hit = HTLEFT;
    else if (edge == "right") hit = HTRIGHT;
    else if (edge == "top") hit = HTTOP;
    else if (edge == "bottom") hit = HTBOTTOM;
    else if (edge == "top-left") hit = HTTOPLEFT;
    else if (edge == "top-right") hit = HTTOPRIGHT;
    else if (edge == "bottom-left") hit = HTBOTTOMLEFT;
    else if (edge == "bottom-right") hit = HTBOTTOMRIGHT;
    if (hit == 0) {
        reply_error(id, "无效的缩放方向", edge);
        return;
    }

    reply_ok(id, {{"resizing", true}});
    POINT point{};
    ::GetCursorPos(&point);
    ::ReleaseCapture();
    ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, hit,
                   MAKELPARAM(point.x, point.y));
}

void Host::rpc_window_state(int id) {
    reply_ok(id, {{"maximized", ::IsZoomed(hwnd_) != 0}});
}

} // namespace cl::ui