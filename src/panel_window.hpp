#pragma once

#include "explorer_tracker.hpp"
#include "notes_repository.hpp"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/com.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class PanelWindow
{
public:
    PanelWindow() = default;
    ~PanelWindow();

    bool Create(HINSTANCE instance);
    void ShowFor(HWND explorer, const ExplorerSnapshot& snapshot);
    void Hide();
    void ToggleCollapsed();
    void ToggleDockSide();

    HWND Hwnd() const;

private:
    struct SaveRequest
    {
        std::uint64_t generation = 0;
        ObjectIdentity identity;
        std::wstring path;
        std::wstring markdown;
    };

    struct SaveResult
    {
        std::uint64_t generation = 0;
        std::wstring error;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureResources();
    void Render();
    void Reposition();
    void UpdateEditorLayout();
    void UpdateHoverState(bool hovering);
    void RefreshTarget();
    void OnEditChanged();
    void QueuePendingSave();
    void SaveWorker();
    void StopSaveWorker();

    HWND window_ = nullptr;
    HWND editor_ = nullptr;
    HWND explorer_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HMODULE richEditLibrary_ = nullptr;
    ExplorerSnapshot snapshot_;
    NotesRepository repository_;
    ObjectIdentity targetIdentity_;
    std::wstring targetPath_;
    std::wstring targetDescription_;
    std::wstring noteStatus_;
    std::uint64_t generation_ = 0;
    bool targetValid_ = false;
    bool collapsed_ = false;
    bool dockRight_ = true;
    bool hovering_ = false;
    bool loadingText_ = false;
    D2D1_RECT_F collapseButton_ = {};
    D2D1_RECT_F sideButton_ = {};

    std::mutex saveMutex_;
    std::condition_variable saveCondition_;
    std::deque<SaveRequest> saveQueue_;
    std::thread saveThread_;
    bool stoppingSaveWorker_ = false;

    wil::com_ptr<ID2D1Factory> d2dFactory_;
    wil::com_ptr<ID2D1HwndRenderTarget> renderTarget_;
    wil::com_ptr<IDWriteFactory> dwriteFactory_;
    wil::com_ptr<IDWriteTextFormat> titleFormat_;
    wil::com_ptr<IDWriteTextFormat> bodyFormat_;
    wil::com_ptr<IDWriteTextFormat> statusFormat_;
    wil::com_ptr<IDWriteTextFormat> buttonFormat_;
};
