#include "application.hpp"

#include <algorithm>

namespace
{
constexpr wchar_t kMessageClassName[] = L"RedefineMessageWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kCommandToggle = 1001;
constexpr UINT kCommandSwitchSide = 1002;
constexpr UINT kCommandExit = 1003;
}

Application* Application::current_ = nullptr;

Application::Application(HINSTANCE instance) : instance_(instance)
{
}

Application::~Application()
{
    Shutdown();
}

bool Application::Initialize()
{
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kMessageClassName;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    messageWindow_ = CreateWindowExW(
        0,
        kMessageClassName,
        L"资源管理器备注原型消息窗口",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance_,
        this);
    if (messageWindow_ == nullptr || !panel_.Create(instance_))
    {
        return false;
    }
    current_ = this;

    AddTrayIcon();
    AddHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    Refresh();
    return true;
}

int Application::Run()
{
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void Application::AddHook(DWORD minimumEvent, DWORD maximumEvent, DWORD processId)
{
    const auto first = processId == 0 ? hooks_.begin() : hooks_.begin() + 1;
    const auto available = std::find_if(first, hooks_.end(), [](HWINEVENTHOOK hook) { return hook == nullptr; });
    if (available != hooks_.end())
    {
        *available = SetWinEventHook(
            minimumEvent,
            maximumEvent,
            nullptr,
            WinEventProc,
            processId,
            0,
            WINEVENT_OUTOFCONTEXT);
    }
}

void Application::BindExplorerHooks(HWND explorer)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(explorer, &processId);
    if (processId == 0 || processId == explorerProcessId_)
    {
        return;
    }

    ClearExplorerHooks();
    explorerProcessId_ = processId;
    AddHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, processId);
    AddHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, processId);
    AddHook(EVENT_OBJECT_VALUECHANGE, EVENT_OBJECT_VALUECHANGE, processId);
    AddHook(EVENT_OBJECT_SELECTION, EVENT_OBJECT_SELECTION, processId);
    AddHook(EVENT_OBJECT_SELECTIONWITHIN, EVENT_OBJECT_SELECTIONWITHIN, processId);
    AddHook(EVENT_OBJECT_HIDE, EVENT_OBJECT_HIDE, processId);
    AddHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, processId);
    AddHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, processId);
}

void Application::ClearExplorerHooks()
{
    for (auto hook = hooks_.begin() + 1; hook != hooks_.end(); ++hook)
    {
        if (*hook != nullptr)
        {
            UnhookWinEvent(*hook);
            *hook = nullptr;
        }
    }
    explorerProcessId_ = 0;
}

void CALLBACK Application::WinEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND window,
    LONG,
    LONG,
    DWORD,
    DWORD)
{
    if (current_ != nullptr)
    {
        current_->OnWinEvent(event, window);
    }
}

void Application::OnWinEvent(DWORD event, HWND window)
{
    if ((event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY) &&
        explorer_ != nullptr && window == explorer_)
    {
        KillTimer(messageWindow_, kRefreshTimer);
        explorer_ = nullptr;
        ClearExplorerHooks();
        panel_.Hide();
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND || event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND)
    {
        ScheduleRefresh();
        return;
    }
    if (explorer_ != nullptr && window != nullptr && (window == explorer_ || IsChild(explorer_, window)))
    {
        ScheduleRefresh();
    }
}

void Application::ScheduleRefresh(UINT delay)
{
    if (messageWindow_ == nullptr)
    {
        return;
    }
    KillTimer(messageWindow_, kRefreshTimer);
    SetTimer(messageWindow_, kRefreshTimer, delay, nullptr);
}

void Application::Refresh()
{
    HWND foreground = GetForegroundWindow();
    if (foreground == panel_.Hwnd() && explorer_ != nullptr && IsWindow(explorer_))
    {
        foreground = explorer_;
    }

    if (!ExplorerTracker::IsFileExplorerWindow(foreground))
    {
        explorer_ = nullptr;
        ClearExplorerHooks();
        panel_.Hide();
        return;
    }

    explorer_ = foreground;
    BindExplorerHooks(explorer_);
    panel_.ShowFor(explorer_, tracker_.Query(explorer_));
}

void Application::AddTrayIcon()
{
    NOTIFYICONDATAW icon = {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = messageWindow_;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, L"资源管理器备注原型");
    trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
}

void Application::RemoveTrayIcon()
{
    if (!trayIconAdded_)
    {
        return;
    }
    NOTIFYICONDATAW icon = {};
    icon.cbSize = sizeof(icon);
    icon.hWnd = messageWindow_;
    icon.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    trayIconAdded_ = false;
}

void Application::ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }
    AppendMenuW(menu, MF_STRING, kCommandToggle, L"展开或收起备注框");
    AppendMenuW(menu, MF_STRING, kCommandSwitchSide, L"切换左右位置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, L"退出");

    POINT cursor = {};
    GetCursorPos(&cursor);
    if (panel_.Hwnd() != nullptr)
    {
        SetForegroundWindow(panel_.Hwnd());
    }
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        cursor.x,
        cursor.y,
        0,
        messageWindow_,
        nullptr);
    DestroyMenu(menu);

    switch (command)
    {
    case kCommandToggle:
        panel_.ToggleCollapsed();
        ScheduleRefresh(0);
        break;
    case kCommandSwitchSide:
        panel_.ToggleDockSide();
        ScheduleRefresh(0);
        break;
    case kCommandExit:
        PostQuitMessage(0);
        break;
    default:
        break;
    }
}

LRESULT Application::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TIMER:
        if (wParam == kRefreshTimer)
        {
            KillTimer(messageWindow_, kRefreshTimer);
            Refresh();
        }
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP)
        {
            ShowTrayMenu();
        }
        else if (LOWORD(lParam) == WM_LBUTTONUP)
        {
            panel_.ToggleCollapsed();
            ScheduleRefresh(0);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(messageWindow_, message, wParam, lParam);
    }
}

LRESULT CALLBACK Application::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    Application* application = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        application = static_cast<Application*>(create->lpCreateParams);
        application->messageWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
    }
    return application != nullptr ? application->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

void Application::Shutdown()
{
    if (shuttingDown_)
    {
        return;
    }
    shuttingDown_ = true;
    current_ = nullptr;
    for (auto& hook : hooks_)
    {
        if (hook != nullptr)
        {
            UnhookWinEvent(hook);
            hook = nullptr;
        }
    }
    RemoveTrayIcon();
    panel_.Hide();
    if (messageWindow_ != nullptr && IsWindow(messageWindow_))
    {
        DestroyWindow(messageWindow_);
    }
    messageWindow_ = nullptr;
}
