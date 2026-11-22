#pragma once

#include "explorer_tracker.hpp"
#include "markdown_preview.hpp"
#include "notes_repository.hpp"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/com.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class PanelWindow
{
public:
    PanelWindow() = default;
    explicit PanelWindow(std::filesystem::path databasePath);
    ~PanelWindow();

    bool Create(HINSTANCE instance);
    void ShowFor(HWND explorer, const ExplorerSnapshot& snapshot);
    void Hide();
    void ToggleCollapsed();
    void ToggleDockSide();
    void TogglePreview();
    void SetOrphanNotesHandler(std::function<void()> handler);
    void UnbindCurrentNote();
    bool CanUnbindCurrentNote() const;
    void HandleShellChange(LONG event, const std::wstring& path, const std::wstring& newPath);

    HWND Hwnd() const;

private:
    struct SaveRequest
    {
        std::uint64_t generation = 0;
        ObjectIdentity identity;
        std::wstring path;
        std::wstring markdown;
        std::wstring newPath;
        LONG shellEvent = 0;
        bool shellChange = false;
        bool unbind = false;
    };

    struct SaveResult
    {
        std::uint64_t generation = 0;
        std::wstring error;
        bool unbind = false;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureResources();
    void Render();
    void Reposition();
    void UpdateEditorLayout();
    D2D1_RECT_F ContentBounds() const;
    void UpdateHoverState(bool hovering);
    void RefreshTarget();
    void UpdateBindingState(bool bound);
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
    MarkdownPreview preview_;
    std::function<void()> orphanNotesHandler_;
    ObjectIdentity targetIdentity_;
    std::wstring targetPath_;
    std::wstring noteStatus_;
    std::wstring bindingText_ = L"未绑定";
    std::uint64_t generation_ = 0;
    bool noteBound_ = false;
    bool targetValid_ = false;
    bool collapsed_ = false;
    bool dockRight_ = true;
    bool hovering_ = false;
    bool loadingText_ = false;
    bool previewMode_ = false;
    bool unbinding_ = false;
    float previewScroll_ = 0.0F;
    float previewContentHeight_ = 0.0F;
    D2D1_RECT_F previewButton_ = {};
    D2D1_RECT_F collapseButton_ = {};
    D2D1_RECT_F sideButton_ = {};
    D2D1_RECT_F orphanNotesButton_ = {};
    D2D1_RECT_F unbindButton_ = {};

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
