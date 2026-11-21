#pragma once

#include "explorer_tracker.hpp"
#include "panel_window.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <array>

class Application
{
public:
    explicit Application(HINSTANCE instance);
    ~Application();

    bool Initialize();
    int Run();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hook,
        DWORD event,
        HWND window,
        LONG objectId,
        LONG childId,
        DWORD eventThread,
        DWORD eventTime);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void OnWinEvent(DWORD event, HWND window);
    void AddHook(DWORD minimumEvent, DWORD maximumEvent);
    void ScheduleRefresh(UINT delay = 40);
    void Refresh();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void Shutdown();

    static Application* current_;

    HINSTANCE instance_ = nullptr;
    HWND messageWindow_ = nullptr;
    HWND explorer_ = nullptr;
    ExplorerTracker tracker_;
    PanelWindow panel_;
    std::array<HWINEVENTHOOK, 9> hooks_ = {};
    bool trayIconAdded_ = false;
    bool shuttingDown_ = false;
};
