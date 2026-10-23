#include <windows.h>
#include <objbase.h>

#include "core/log.h"
#include "core/paths.h"
#include "ui/host.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 单实例：避免两个窗口同时写同一份日志。
    HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"CMCL_UI_Singleton");
    if (mutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::MessageBoxW(nullptr, L"CMCL 已经在运行。",
                      L"CMCL - C++ Minecraft Launcher", MB_ICONINFORMATION);
        ::CloseHandle(mutex);
        return 0;
    }

    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    cl::Log::init(cl::exe_dir() / L"logs" / L"cmcl-ui.log", false);
    if (auto dirs = cl::ensure_app_dirs(); !dirs) {
        cl::Log::warn("初始化启动器目录失败：" + dirs.error().to_string());
    }

    int result = 1;
    {
        cl::ui::Host host;
        if (host.initialize(instance, show_command)) {
            result = host.run();
        }
    }

    cl::Log::shutdown();
    if (SUCCEEDED(com)) ::CoUninitialize();
    if (mutex != nullptr) ::CloseHandle(mutex);
    return result;
}
