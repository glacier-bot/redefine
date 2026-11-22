#include "orphan_notes_window.hpp"
#include "resource.h"

#include <Windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <re2/re2.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <utility>

namespace
{
constexpr wchar_t kWindowClassName[] = L"RedefineOrphanNotesWindow";
constexpr UINT kSearchResultMessage = WM_APP + 40;
constexpr UINT kRebindResultMessage = WM_APP + 41;
constexpr UINT_PTR kSearchTimer = 1;
constexpr UINT_PTR kRevisionTimer = 2;
constexpr int kSearchEditId = 3001;
constexpr int kRegexButtonId = 3002;
constexpr std::size_t kBatchSize = 32;
constexpr float kCardHeight = 156.0F;
constexpr float kCardStride = 166.0F;
constexpr float kListTop = 76.0F;

std::wstring WindowText(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    return text;
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Lowercase(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = LCMapStringW(
        LOCALE_INVARIANT,
        LCMAP_LOWERCASE,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (length <= 0)
    {
        return text;
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (LCMapStringW(
            LOCALE_INVARIANT,
            LCMAP_LOWERCASE,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            length) <= 0)
    {
        return text;
    }
    return result;
}

std::wstring OriginalName(const std::wstring& path)
{
    if (path.empty())
    {
        return L"原对象名称不可用";
    }
    const wchar_t* name = PathFindFileNameW(path.c_str());
    return name == nullptr || *name == L'\0' ? path : name;
}

std::wstring Summary(const std::wstring& markdown)
{
    std::wstring summary;
    summary.reserve(std::min<std::size_t>(markdown.size(), 180));
    for (const wchar_t character : markdown)
    {
        if (character == L'\r' || character == L'\n' || character == L'\t')
        {
            summary.push_back(L' ');
        }
        else
        {
            summary.push_back(character);
        }
        if (summary.size() == 180)
        {
            break;
        }
    }
    if (markdown.size() > summary.size())
    {
        summary.append(L"…");
    }
    return summary.empty() ? L"（空备注）" : summary;
}

std::wstring OrphanTime(std::int64_t value)
{
    const std::time_t time = static_cast<std::time_t>(value);
    std::tm local = {};
    if (value <= 0 || _localtime64_s(&local, &time) != 0)
    {
        return L"无主时间：未知";
    }
    wchar_t text[64] = {};
    swprintf_s(
        text,
        L"无主时间：%04d-%02d-%02d %02d:%02d:%02d",
        local.tm_year + 1900,
        local.tm_mon + 1,
        local.tm_mday,
        local.tm_hour,
        local.tm_min,
        local.tm_sec);
    return text;
}
}

OrphanNotesWindow::OrphanNotesWindow(std::filesystem::path databasePath) : repository_(std::move(databasePath))
{
}

OrphanNotesWindow::~OrphanNotesWindow()
{
    StopWorker();
    if (window_ != nullptr && IsWindow(window_))
    {
        DestroyWindow(window_);
    }
    if (controlFont_ != nullptr)
    {
        DeleteObject(controlFont_);
    }
}

bool OrphanNotesWindow::Create(HINSTANCE instance)
{
    if (window_ != nullptr)
    {
        return true;
    }
    instance_ = instance;
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPLICATION_ICON));
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPLICATION_ICON));
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    window_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        kWindowClassName,
        L"无主备注管理",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        900,
        640,
        nullptr,
        nullptr,
        instance,
        this);
    if (window_ == nullptr)
    {
        return false;
    }

    searchEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEditId)),
        instance,
        nullptr);
    regexButton_ = CreateWindowExW(
        0,
        L"BUTTON",
        L".*",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRegexButtonId)),
        instance,
        nullptr);
    if (searchEdit_ == nullptr || regexButton_ == nullptr)
    {
        return false;
    }

    SendMessageW(searchEdit_, EM_SETLIMITTEXT, 512, 0);
    controlFont_ = CreateFontW(
        -MulDiv(12, GetDpiForWindow(window_), 72),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
    SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
    SendMessageW(regexButton_, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
    originalEditProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        searchEdit_,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&OrphanNotesWindow::EditProc)));
    DragAcceptFiles(window_, TRUE);
    LayoutControls();
    workerThread_ = std::thread(&OrphanNotesWindow::SearchWorker, this);
    return true;
}

void OrphanNotesWindow::Show()
{
    if (window_ == nullptr && !Create(instance_))
    {
        MessageBoxW(nullptr, L"无法打开无主备注管理窗口。", L"资源管理器备注", MB_ICONERROR);
        return;
    }
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    RequestSearch(0);
    SetTimer(window_, kRevisionTimer, 250, nullptr);
    SetFocus(searchEdit_);
}

HWND OrphanNotesWindow::Hwnd() const
{
    return window_;
}

bool OrphanNotesWindow::EnsureResources()
{
    if (renderTarget_)
    {
        return true;
    }
    if (!d2dFactory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.put())))
    {
        return false;
    }
    if (!dwriteFactory_ && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.put()))))
    {
        return false;
    }

    RECT client = {};
    GetClientRect(window_, &client);
    if (FAILED(d2dFactory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(client.right, client.bottom)),
            renderTarget_.put())))
    {
        return false;
    }

    const auto createFormat = [this](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** format) {
        return dwriteFactory_->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"zh-CN",
            format);
    };
    if (FAILED(createFormat(15.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_.put())) ||
        FAILED(createFormat(11.5F, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_.put())) ||
        FAILED(createFormat(10.5F, DWRITE_FONT_WEIGHT_NORMAL, pathFormat_.put())) ||
        FAILED(createFormat(11.0F, DWRITE_FONT_WEIGHT_NORMAL, statusFormat_.put())))
    {
        return false;
    }

    const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    titleFormat_->SetTrimming(&trimming, nullptr);
    pathFormat_->SetTrimming(&trimming, nullptr);
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    pathFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    bodyFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return true;
}

D2D1_RECT_F OrphanNotesWindow::ListBounds() const
{
    RECT client = {};
    GetClientRect(window_, &client);
    const float scale = 96.0F / static_cast<float>(GetDpiForWindow(window_));
    return D2D1::RectF(
        16.0F,
        kListTop,
        std::max(16.0F, static_cast<float>(client.right) * scale - 16.0F),
        std::max(kListTop, static_cast<float>(client.bottom) * scale - 34.0F));
}

float OrphanNotesWindow::MaximumScroll() const
{
    const auto bounds = ListBounds();
    const float contentHeight = static_cast<float>(results_.size()) * kCardStride;
    return std::max(0.0F, contentHeight - (bounds.bottom - bounds.top));
}

void OrphanNotesWindow::ScrollTo(float offset)
{
    const float target = std::clamp(offset, 0.0F, MaximumScroll());
    if (scrollOffset_ == target)
    {
        return;
    }
    scrollOffset_ = target;
    UpdateScrollBar();
    InvalidateRect(window_, nullptr, FALSE);
}

void OrphanNotesWindow::UpdateScrollBar()
{
    if (window_ == nullptr)
    {
        return;
    }
    scrollOffset_ = std::clamp(scrollOffset_, 0.0F, MaximumScroll());
    const auto bounds = ListBounds();
    const int contentHeight = static_cast<int>(std::ceil(static_cast<float>(results_.size()) * kCardStride));
    SCROLLINFO information = {};
    information.cbSize = sizeof(information);
    information.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
    information.nMin = 0;
    information.nMax = std::max(0, contentHeight - 1);
    information.nPage = static_cast<UINT>(std::max(1.0F, bounds.bottom - bounds.top));
    information.nPos = static_cast<int>(scrollOffset_);
    SetScrollInfo(window_, SB_VERT, &information, TRUE);
}

void OrphanNotesWindow::LayoutControls()
{
    if (searchEdit_ == nullptr || regexButton_ == nullptr)
    {
        return;
    }
    RECT client = {};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    const int margin = MulDiv(16, dpi, 96);
    const int top = MulDiv(16, dpi, 96);
    const int height = MulDiv(32, dpi, 96);
    const int gap = MulDiv(8, dpi, 96);
    const int buttonWidth = MulDiv(52, dpi, 96);
    MoveWindow(searchEdit_, margin, top, std::max(1, static_cast<int>(client.right) - margin * 2 - buttonWidth - gap), height, TRUE);
    MoveWindow(regexButton_, client.right - margin - buttonWidth, top, buttonWidth, height, TRUE);
}

void OrphanNotesWindow::Render()
{
    if (!EnsureResources())
    {
        return;
    }
    const UINT dpi = GetDpiForWindow(window_);
    renderTarget_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    const auto list = ListBounds();
    const float width = list.right + 16.0F;
    const float height = list.bottom + 34.0F;

    wil::com_ptr<ID2D1SolidColorBrush> text;
    wil::com_ptr<ID2D1SolidColorBrush> secondaryText;
    wil::com_ptr<ID2D1SolidColorBrush> border;
    wil::com_ptr<ID2D1SolidColorBrush> card;
    wil::com_ptr<ID2D1SolidColorBrush> hoveredCard;
    wil::com_ptr<ID2D1SolidColorBrush> error;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x20242A), text.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x626B78), secondaryText.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xD7DCE3), border.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), card.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xE8F0FE), hoveredCard.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xC62828), error.put());

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0xF5F6F8));
    renderTarget_->DrawText(
        status_.c_str(),
        static_cast<UINT32>(status_.size()),
        statusFormat_.get(),
        D2D1::RectF(16.0F, 53.0F, width - 16.0F, 72.0F),
        statusError_ ? error.get() : secondaryText.get());

    renderTarget_->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (results_.empty() && !searching_ && !statusError_)
    {
        const std::wstring empty = WindowText(searchEdit_).empty() ? L"暂无无主备注" : L"没有匹配的无主备注";
        renderTarget_->DrawText(
            empty.c_str(),
            static_cast<UINT32>(empty.size()),
            bodyFormat_.get(),
            D2D1::RectF(list.left, list.top + 24.0F, list.right, list.top + 54.0F),
            secondaryText.get());
    }
    else
    {
        const std::size_t first = std::min(
            results_.size(),
            static_cast<std::size_t>(std::max(0.0F, scrollOffset_ / kCardStride)));
        for (std::size_t index = first; index < results_.size(); ++index)
        {
            const float top = list.top + static_cast<float>(index) * kCardStride - scrollOffset_;
            if (top > list.bottom)
            {
                break;
            }
            if (top + kCardHeight < list.top)
            {
                continue;
            }
            const auto& note = results_[index];
            const auto rectangle = D2D1::RectF(list.left, top, list.right, top + kCardHeight);
            const auto rounded = D2D1::RoundedRect(rectangle, 8.0F, 8.0F);
            renderTarget_->FillRoundedRectangle(rounded, hoveredCard_ == static_cast<int>(index) ? hoveredCard.get() : card.get());
            renderTarget_->DrawRoundedRectangle(rounded, border.get(), 1.0F);

            const std::wstring title = (note.kind == NoteObjectKind::Directory ? L"[文件夹] " : L"[文件] ") + OriginalName(note.originalPath);
            const std::wstring path = note.originalPath.empty() ? L"原路径不可用" : note.originalPath;
            const std::wstring summary = Summary(note.markdown);
            const std::wstring time = OrphanTime(note.orphanSince);
            renderTarget_->DrawText(
                title.c_str(),
                static_cast<UINT32>(title.size()),
                titleFormat_.get(),
                D2D1::RectF(list.left + 14.0F, top + 9.0F, list.right - 14.0F, top + 35.0F),
                text.get());
            renderTarget_->DrawText(
                path.c_str(),
                static_cast<UINT32>(path.size()),
                pathFormat_.get(),
                D2D1::RectF(list.left + 14.0F, top + 38.0F, list.right - 14.0F, top + 58.0F),
                secondaryText.get());
            const auto summaryBounds = D2D1::RectF(list.left + 14.0F, top + 64.0F, list.right - 14.0F, top + 111.0F);
            renderTarget_->PushAxisAlignedClip(summaryBounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            renderTarget_->DrawText(
                summary.c_str(),
                static_cast<UINT32>(summary.size()),
                bodyFormat_.get(),
                summaryBounds,
                text.get());
            renderTarget_->PopAxisAlignedClip();
            renderTarget_->DrawText(
                time.c_str(),
                static_cast<UINT32>(time.size()),
                statusFormat_.get(),
                D2D1::RectF(list.left + 14.0F, top + 124.0F, list.right - 14.0F, top + 146.0F),
                secondaryText.get());
        }
    }
    renderTarget_->PopAxisAlignedClip();

    const wchar_t* hint = L"拖放一个同类型且未绑定备注的对象到目标卡片；文件夹后代不会级联恢复。";
    renderTarget_->DrawText(
        hint,
        static_cast<UINT32>(wcslen(hint)),
        statusFormat_.get(),
        D2D1::RectF(16.0F, height - 27.0F, width - 16.0F, height - 7.0F),
        secondaryText.get());
    if (const HRESULT result = renderTarget_->EndDraw(); result == D2DERR_RECREATE_TARGET)
    {
        renderTarget_.reset();
    }
}

void OrphanNotesWindow::RequestSearch(UINT delay)
{
    ++requestedSearch_;
    searching_ = false;
    if (window_ == nullptr || imeComposing_ || !IsWindowVisible(window_))
    {
        return;
    }
    searching_ = true;
    statusError_ = false;
    status_ = L"正在搜索…";
    KillTimer(window_, kSearchTimer);
    SetTimer(window_, kSearchTimer, delay, nullptr);
    InvalidateRect(window_, nullptr, FALSE);
}

void OrphanNotesWindow::StartSearch()
{
    if (imeComposing_ || window_ == nullptr || !IsWindowVisible(window_))
    {
        return;
    }
    WorkItem request;
    request.generation = requestedSearch_.load(std::memory_order_acquire);
    request.revision = repository_.Revision();
    request.query = WindowText(searchEdit_);
    request.regularExpression = regularExpression_;
    searchRevision_ = request.revision;
    seenRevision_ = request.revision;
    searching_ = true;
    statusError_ = false;
    status_ = L"正在搜索…";
    scrollOffset_ = 0.0F;
    UpdateScrollBar();
    {
        std::lock_guard lock(workerMutex_);
        workQueue_.push_back(std::move(request));
    }
    workerCondition_.notify_one();
    InvalidateRect(window_, nullptr, FALSE);
}

void OrphanNotesWindow::SearchWorker()
{
    while (true)
    {
        WorkItem request;
        {
            std::unique_lock lock(workerMutex_);
            workerCondition_.wait(lock, [this]() { return stoppingWorker_ || !workQueue_.empty(); });
            if (stoppingWorker_)
            {
                return;
            }
            if (workQueue_.empty())
            {
                continue;
            }
            request = std::move(workQueue_.front());
            workQueue_.pop_front();
        }
        if (request.rebind)
        {
            auto result = std::make_unique<RebindResult>();
            result->noteId = request.noteId;
            result->path = request.path;
            repository_.RebindOrphan(request.noteId, request.path, result->error);
            if (PostMessageW(window_, kRebindResultMessage, 0, reinterpret_cast<LPARAM>(result.get())))
            {
                result.release();
            }
            continue;
        }
        if (request.generation == requestedSearch_.load(std::memory_order_acquire))
        {
            RunSearch(request);
        }
    }
}

void OrphanNotesWindow::RunSearch(const WorkItem& request)
{
    auto postResult = [this](std::unique_ptr<SearchResult> result) {
        if (PostMessageW(window_, kSearchResultMessage, 0, reinterpret_cast<LPARAM>(result.get())))
        {
            result.release();
        }
    };
    if (request.query.size() > 512)
    {
        auto result = std::make_unique<SearchResult>();
        result->generation = request.generation;
        result->revision = request.revision;
        result->error = L"搜索表达式不能超过 512 个字符";
        result->finished = true;
        postResult(std::move(result));
        return;
    }

    const std::string expression = request.regularExpression
        ? ToUtf8(request.query)
        : RE2::QuoteMeta(ToUtf8(Lowercase(request.query)));
    RE2 matcher(expression.empty() ? "(?:)" : expression);
    if (!matcher.ok())
    {
        auto result = std::make_unique<SearchResult>();
        result->generation = request.generation;
        result->revision = request.revision;
        result->error = L"正则表达式无效，请检查语法；已保留上一次有效结果";
        result->finished = true;
        postResult(std::move(result));
        return;
    }

    std::size_t total = 0;
    bool announced = false;
    std::vector<OrphanNote> pending;
    const auto postBatch = [&](bool finished, bool reset, std::size_t count, std::vector<OrphanNote>& notes) {
        auto result = std::make_unique<SearchResult>();
        result->generation = request.generation;
        result->revision = request.revision;
        result->total = count;
        result->notes = std::move(notes);
        result->resetResults = reset;
        result->finished = finished;
        postResult(std::move(result));
    };

    std::wstring error;
    repository_.VisitOrphanNotes(
        [&](const OrphanNote& note) {
            if (requestedSearch_.load(std::memory_order_acquire) != request.generation)
            {
                return false;
            }
            std::wstring searchable = note.markdown;
            searchable.push_back(L'\n');
            searchable.append(note.originalPath);
            searchable.push_back(L'\n');
            searchable.append(OriginalName(note.originalPath));
            if (!request.regularExpression)
            {
                searchable = Lowercase(searchable);
            }
            if (!RE2::PartialMatch(ToUtf8(searchable), matcher))
            {
                return true;
            }
            ++total;
            pending.push_back(note);
            if (pending.size() >= kBatchSize)
            {
                std::vector<OrphanNote> batch;
                batch.swap(pending);
                postBatch(false, !announced, total, batch);
                announced = true;
            }
            return true;
        },
        error);
    if (requestedSearch_.load(std::memory_order_acquire) != request.generation)
    {
        return;
    }
    if (!error.empty())
    {
        auto result = std::make_unique<SearchResult>();
        result->generation = request.generation;
        result->revision = request.revision;
        result->total = total;
        result->error = std::move(error);
        result->finished = true;
        postResult(std::move(result));
        return;
    }
    postBatch(true, !announced, total, pending);
}

void OrphanNotesWindow::StopWorker()
{
    ++requestedSearch_;
    {
        std::lock_guard lock(workerMutex_);
        stoppingWorker_ = true;
    }
    workerCondition_.notify_one();
    if (workerThread_.joinable())
    {
        workerThread_.join();
    }
}

void OrphanNotesWindow::OnSearchResult(std::unique_ptr<SearchResult> result)
{
    if (!result || result->generation != requestedSearch_.load(std::memory_order_acquire))
    {
        return;
    }
    if (result->revision != repository_.Revision())
    {
        RequestSearch(0);
        return;
    }
    seenRevision_ = result->revision;
    if (!result->error.empty())
    {
        searching_ = false;
        statusError_ = true;
        status_ = result->error;
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (result->resetResults)
    {
        results_.clear();
        hoveredCard_ = -1;
    }
    results_.insert(
        results_.end(),
        std::make_move_iterator(result->notes.begin()),
        std::make_move_iterator(result->notes.end()));
    totalMatches_ = result->total;
    searching_ = !result->finished;
    statusError_ = false;
    status_ = searching_
        ? L"已找到 " + std::to_wstring(totalMatches_) + L" 条，正在搜索…"
        : L"共 " + std::to_wstring(totalMatches_) + L" 条无主备注";
    UpdateScrollBar();
    InvalidateRect(window_, nullptr, FALSE);
}

int OrphanNotesWindow::CardAt(POINT point) const
{
    const auto bounds = ListBounds();
    const float scale = 96.0F / static_cast<float>(GetDpiForWindow(window_));
    const float x = static_cast<float>(point.x) * scale;
    const float y = static_cast<float>(point.y) * scale;
    if (x < bounds.left || x > bounds.right || y < bounds.top || y > bounds.bottom)
    {
        return -1;
    }
    const float position = y - bounds.top + scrollOffset_;
    const int index = static_cast<int>(position / kCardStride);
    if (index < 0 || static_cast<std::size_t>(index) >= results_.size() ||
        position - static_cast<float>(index) * kCardStride > kCardHeight)
    {
        return -1;
    }
    return index;
}

void OrphanNotesWindow::BeginRebind(std::int64_t noteId, const std::wstring& path)
{
    if (binding_)
    {
        statusError_ = true;
        status_ = L"另一项重新绑定操作正在进行，请稍候";
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    ++requestedSearch_;
    searching_ = false;
    binding_ = true;
    statusError_ = false;
    status_ = L"正在验证并重新绑定…";
    WorkItem request;
    request.noteId = noteId;
    request.path = path;
    request.rebind = true;
    {
        std::lock_guard lock(workerMutex_);
        workQueue_.push_back(std::move(request));
    }
    workerCondition_.notify_one();
    InvalidateRect(window_, nullptr, FALSE);
}

void OrphanNotesWindow::OnRebindResult(std::unique_ptr<RebindResult> result)
{
    if (!result)
    {
        return;
    }
    binding_ = false;
    statusError_ = !result->error.empty();
    if (statusError_)
    {
        status_ = result->error;
    }
    else
    {
        const auto found = std::find_if(results_.begin(), results_.end(), [&](const OrphanNote& note) {
            return note.id == result->noteId;
        });
        if (found != results_.end())
        {
            results_.erase(found);
            totalMatches_ = totalMatches_ > 0 ? totalMatches_ - 1 : 0;
        }
        seenRevision_ = repository_.Revision();
        status_ = L"已重新绑定到：" + result->path;
        UpdateScrollBar();
    }
    InvalidateRect(window_, nullptr, FALSE);
}

LRESULT OrphanNotesWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        BeginPaint(window_, &paint);
        Render();
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (renderTarget_ && FAILED(renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)))))
        {
            renderTarget_.reset();
        }
        LayoutControls();
        UpdateScrollBar();
        return 0;
    case WM_DPICHANGED:
    {
        const auto suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (controlFont_ != nullptr)
        {
            DeleteObject(controlFont_);
        }
        controlFont_ = CreateFontW(-MulDiv(12, HIWORD(wParam), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
        SendMessageW(regexButton_, WM_SETFONT, reinterpret_cast<WPARAM>(controlFont_), TRUE);
        LayoutControls();
        UpdateScrollBar();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kSearchEditId && HIWORD(wParam) == EN_CHANGE)
        {
            RequestSearch();
        }
        else if (LOWORD(wParam) == kRegexButtonId && HIWORD(wParam) == BN_CLICKED)
        {
            regularExpression_ = SendMessageW(regexButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            RequestSearch();
        }
        return 0;
    case WM_TIMER:
        if (wParam == kSearchTimer)
        {
            KillTimer(window_, kSearchTimer);
            StartSearch();
        }
        else if (wParam == kRevisionTimer && repository_.Revision() != seenRevision_)
        {
            RequestSearch(0);
        }
        return 0;
    case kSearchResultMessage:
        OnSearchResult(std::unique_ptr<SearchResult>(reinterpret_cast<SearchResult*>(lParam)));
        return 0;
    case kRebindResultMessage:
        OnRebindResult(std::unique_ptr<RebindResult>(reinterpret_cast<RebindResult*>(lParam)));
        return 0;
    case WM_VSCROLL:
    {
        const auto bounds = ListBounds();
        float target = scrollOffset_;
        switch (LOWORD(wParam))
        {
        case SB_LINEUP:
            target -= 40.0F;
            break;
        case SB_LINEDOWN:
            target += 40.0F;
            break;
        case SB_PAGEUP:
            target -= bounds.bottom - bounds.top;
            break;
        case SB_PAGEDOWN:
            target += bounds.bottom - bounds.top;
            break;
        case SB_TOP:
            target = 0.0F;
            break;
        case SB_BOTTOM:
            target = MaximumScroll();
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            target = static_cast<float>(HIWORD(wParam));
            break;
        default:
            return 0;
        }
        ScrollTo(target);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA * 48.0F;
        ScrollTo(scrollOffset_ - delta);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int hoveredCard = CardAt(point);
        if (hoveredCard_ != hoveredCard)
        {
            hoveredCard_ = hoveredCard;
            InvalidateRect(window_, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking = {};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window_;
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hoveredCard_ != -1)
        {
            hoveredCard_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        POINT point = {};
        DragQueryPoint(drop, &point);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count != 1)
        {
            statusError_ = true;
            status_ = L"每次只能拖入一个文件或文件夹";
        }
        else
        {
            const int card = CardAt(point);
            if (card < 0)
            {
                statusError_ = true;
                status_ = L"请将对象明确拖放到一张无主备注卡片上";
            }
            else
            {
                const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
                std::wstring path(static_cast<std::size_t>(length), L'\0');
                DragQueryFileW(drop, 0, path.data(), length + 1);
                BeginRebind(results_[card].id, path);
            }
        }
        DragFinish(drop);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        KillTimer(window_, kSearchTimer);
        KillTimer(window_, kRevisionTimer);
        ++requestedSearch_;
        searching_ = false;
        ShowWindow(window_, SW_HIDE);
        return 0;
    case WM_NCDESTROY:
        searchEdit_ = nullptr;
        regexButton_ = nullptr;
        window_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

LRESULT CALLBACK OrphanNotesWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto manager = reinterpret_cast<OrphanNotesWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        manager = static_cast<OrphanNotesWindow*>(create->lpCreateParams);
        manager->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(manager));
    }
    return manager != nullptr ? manager->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK OrphanNotesWindow::EditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto manager = reinterpret_cast<OrphanNotesWindow*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
    if (manager == nullptr)
    {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (message == WM_IME_STARTCOMPOSITION)
    {
        manager->imeComposing_ = true;
        KillTimer(manager->window_, kSearchTimer);
    }
    else if (message == WM_IME_ENDCOMPOSITION)
    {
        manager->imeComposing_ = false;
        manager->RequestSearch();
    }
    else if (message == WM_NCDESTROY)
    {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(manager->originalEditProc_));
    }
    return CallWindowProcW(manager->originalEditProc_, window, message, wParam, lParam);
}
