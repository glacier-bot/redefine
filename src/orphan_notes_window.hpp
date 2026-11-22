#pragma once

#include "notes_repository.hpp"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/com.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class OrphanNotesWindow
{
public:
    OrphanNotesWindow() = default;
    explicit OrphanNotesWindow(std::filesystem::path databasePath);
    ~OrphanNotesWindow();

    bool Create(HINSTANCE instance);
    void Show();
    HWND Hwnd() const;

private:
    class DropTarget;

    struct WorkItem
    {
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
        std::int64_t noteId = 0;
        std::wstring query;
        std::wstring path;
        bool regularExpression = false;
        bool rebind = false;
    };

    struct SearchResult
    {
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
        std::size_t total = 0;
        std::vector<OrphanNote> notes;
        std::wstring error;
        bool resetResults = false;
        bool finished = false;
    };

    struct RebindResult
    {
        std::int64_t noteId = 0;
        std::wstring path;
        std::wstring error;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureResources();
    void Render();
    void LayoutControls();
    void UpdateScrollBar();
    void ScrollTo(float offset);
    void RequestSearch(UINT delay = 120);
    void StartSearch();
    void SearchWorker();
    void RunSearch(const WorkItem& request);
    void StopWorker();
    void BeginRebind(std::int64_t noteId, const std::wstring& path);
    void DropFiles(const std::vector<std::wstring>& paths, POINT point);
    void SetDropCard(int card);
    void OnSearchResult(std::unique_ptr<SearchResult> result);
    void OnRebindResult(std::unique_ptr<RebindResult> result);
    int CardAt(POINT point) const;
    D2D1_RECT_F ListBounds() const;
    float MaximumScroll() const;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND regexButton_ = nullptr;
    HFONT controlFont_ = nullptr;
    WNDPROC originalEditProc_ = nullptr;
    DropTarget* dropTarget_ = nullptr;
    NotesRepository repository_;
    std::vector<OrphanNote> results_;
    std::size_t totalMatches_ = 0;
    std::wstring status_ = L"正在准备无主备注…";
    bool statusError_ = false;
    bool regularExpression_ = false;
    bool imeComposing_ = false;
    bool searching_ = false;
    bool binding_ = false;
    bool dropSingleFile_ = false;
    int hoveredCard_ = -1;
    int dropCard_ = -1;
    float scrollOffset_ = 0.0F;
    std::uint64_t searchRevision_ = 0;
    std::uint64_t seenRevision_ = 0;

    std::mutex workerMutex_;
    std::condition_variable workerCondition_;
    std::deque<WorkItem> workQueue_;
    std::thread workerThread_;
    std::atomic_uint64_t requestedSearch_{0};
    bool stoppingWorker_ = false;

    wil::com_ptr<ID2D1Factory> d2dFactory_;
    wil::com_ptr<ID2D1HwndRenderTarget> renderTarget_;
    wil::com_ptr<IDWriteFactory> dwriteFactory_;
    wil::com_ptr<IDWriteTextFormat> titleFormat_;
    wil::com_ptr<IDWriteTextFormat> bodyFormat_;
    wil::com_ptr<IDWriteTextFormat> pathFormat_;
    wil::com_ptr<IDWriteTextFormat> statusFormat_;
};
