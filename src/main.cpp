#include "application.hpp"

#include <wil/resource.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wil::unique_handle mutex(CreateMutexW(nullptr, TRUE, L"Local\\RedefinePrototype"));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult))
    {
        MessageBoxW(nullptr, L"无法初始化 COM，原型将退出。", L"资源管理器备注原型", MB_ICONERROR);
        return 1;
    }

    Application application(instance);
    const bool initialized = application.Initialize();
    if (!initialized)
    {
        MessageBoxW(nullptr, L"无法初始化原型窗口。", L"资源管理器备注原型", MB_ICONERROR);
    }
    const int result = initialized ? application.Run() : 1;

    CoUninitialize();
    return result;
}
