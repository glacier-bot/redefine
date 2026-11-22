#include "panel_window.hpp"

#include <RichEdit.h>
#include <Windowsx.h>
#include <wil/resource.h>

#include <algorithm>
#include <memory>
#include <optional>

namespace
{
constexpr wchar_t kPanelClassName[] = L"RedefinePanelWindow";
constexpr UINT_PTR kSaveTimer = 2;
constexpr UINT kSaveCompletedMessage = WM_APP + 20;
constexpr int kEditorId = 2001;

bool Contains(const D2D1_RECT_F& rectangle, float x, float y)
{
    return x >= rectangle.left && x <= rectangle.right && y >= rectangle.top && y <= rectangle.bottom;
}

std::wstring EditorText(HWND editor)
{
    const int length = GetWindowTextLengthW(editor);
    std::wstring text(static_cast<size_t>(length), L'\0');
    GetWindowTextW(editor, text.data(), length + 1);
    return text;
}
}

PanelWindow::~PanelWindow()
{
    QueuePendingSave();
    StopSaveWorker();
    if (window_ != nullptr && IsWindow(window_))
    {
        DestroyWindow(window_);
    }
    if (richEditLibrary_ != nullptr)
    {
        FreeLibrary(richEditLibrary_);
    }
}

bool PanelWindow::Create(HINSTANCE instance)
{
    instance_ = instance;
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kPanelClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    richEditLibrary_ = LoadLibraryW(L"Msftedit.dll");
    if (richEditLibrary_ == nullptr)
    {
        return false;
    }

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kPanelClassName,
        L"资源管理器备注原型",
        WS_POPUP,
        0,
        0,
        360,
        480,
        nullptr,
        nullptr,
        instance,
        this);
    if (window_ == nullptr)
    {
        return false;
    }

    editor_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        MSFTEDIT_CLASS,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditorId)),
        instance,
        nullptr);
    if (editor_ == nullptr)
    {
        return false;
    }

    SendMessageW(editor_, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
    SendMessageW(editor_, EM_SETEVENTMASK, 0, ENM_CHANGE);
    SendMessageW(editor_, EM_EXLIMITTEXT, 0, 1024 * 1024);
    SendMessageW(editor_, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
    SendMessageW(editor_, EM_SETUNDOLIMIT, 100, 0);

    CHARFORMAT2W format = {};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET | CFM_COLOR;
    format.yHeight = 280;
    format.bCharSet = DEFAULT_CHARSET;
    format.crTextColor = RGB(32, 36, 42);
    wcscpy_s(format.szFaceName, L"Microsoft YaHei UI");
    SendMessageW(editor_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));

    EnableWindow(editor_, FALSE);
    saveThread_ = std::thread(&PanelWindow::SaveWorker, this);
    return true;
}

HWND PanelWindow::Hwnd() const
{
    return window_;
}

void PanelWindow::Hide()
{
    QueuePendingSave();
    if (window_ != nullptr)
    {
        ShowWindow(window_, SW_HIDE);
    }
}

void PanelWindow::ToggleCollapsed()
{
    collapsed_ = !collapsed_;
    Reposition();
    InvalidateRect(window_, nullptr, FALSE);
}

void PanelWindow::ToggleDockSide()
{
    dockRight_ = !dockRight_;
    Reposition();
}

void PanelWindow::ShowFor(HWND explorer, const ExplorerSnapshot& snapshot)
{
    if (explorer == nullptr || !IsWindow(explorer) || IsIconic(explorer))
    {
        Hide();
        return;
    }
    if (window_ == nullptr && !Create(instance_))
    {
        return;
    }
    if (explorer_ != explorer)
    {
        SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(explorer));
    }

    explorer_ = explorer;
    snapshot_ = snapshot;
    RefreshTarget();

    RECT client = {};
    GetClientRect(explorer, &client);
    POINT origin = {};
    ClientToScreen(explorer, &origin);

    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth < 160 || clientHeight < 120)
    {
        Hide();
        return;
    }

    const UINT dpi = GetDpiForWindow(explorer);
    const int margin = MulDiv(12, dpi, 96);
    const int collapsedSize = MulDiv(54, dpi, 96);

    int width = collapsedSize;
    int height = collapsedSize;
    int x = 0;
    int y = 0;
    if (collapsed_)
    {
        x = dockRight_ ? origin.x + clientWidth - width - margin : origin.x + margin;
        y = origin.y + (clientHeight - height) / 2;
    }
    else
    {
        width = std::min(MulDiv(350, dpi, 96), clientWidth * 2 / 5);
        width = std::max(width, MulDiv(260, dpi, 96));
        width = std::min(width, clientWidth - margin * 2);
        height = clientHeight - margin * 2;
        x = dockRight_ ? origin.x + clientWidth - width - margin : origin.x + margin;
        y = origin.y + margin;
    }

    SetWindowPos(
        window_,
        HWND_TOP,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateEditorLayout();
    InvalidateRect(window_, nullptr, FALSE);
}

void PanelWindow::Reposition()
{
    if (explorer_ != nullptr)
    {
        ShowFor(explorer_, snapshot_);
    }
}

void PanelWindow::RefreshTarget()
{
    std::wstring path;
    std::wstring description;
    std::wstring error;
    std::optional<ObjectIdentity> identity;

    if (!snapshot_.error.empty())
    {
        error = snapshot_.error;
    }
    else if (snapshot_.selection_count > 1)
    {
        error = L"已选中多项，请选择单个文件或文件夹后编辑备注";
    }
    else if (!snapshot_.file_system_location)
    {
        error = L"当前位置不是可识别的本地文件系统目录";
    }
    else
    {
        path = snapshot_.selection_count == 1 ? snapshot_.selected_path : snapshot_.location;
        identity = ReadObjectIdentity(path, error);
        if (identity)
        {
            description = identity->kind == NoteObjectKind::Directory ? L"当前文件夹\n" : L"选中文件\n";
            description += path;
        }
    }

    if (identity && targetValid_ && path == targetPath_ && *identity == targetIdentity_)
    {
        return;
    }

    QueuePendingSave();
    ++generation_;
    targetPath_ = path;
    targetDescription_ = description;
    targetValid_ = identity.has_value();
    if (identity)
    {
        targetIdentity_ = *identity;
    }

    loadingText_ = true;
    SetWindowTextW(editor_, L"");
    loadingText_ = false;
    EnableWindow(editor_, targetValid_);

    if (!targetValid_)
    {
        noteStatus_ = error;
        targetDescription_.clear();
        UpdateEditorLayout();
        return;
    }

    StoredNote note;
    const auto storedNote = repository_.Load(targetIdentity_, targetPath_, error);
    if (!error.empty())
    {
        noteStatus_ = error;
        targetValid_ = false;
        EnableWindow(editor_, FALSE);
        UpdateEditorLayout();
        return;
    }
    if (storedNote)
    {
        note = *storedNote;
    }

    loadingText_ = true;
    SetWindowTextW(editor_, note.markdown.c_str());
    loadingText_ = false;
    SendMessageW(editor_, EM_SETMODIFY, FALSE, 0);
    SendMessageW(editor_, EM_SETSEL, 0, 0);
    noteStatus_ = storedNote ? L"备注已加载，修改后自动保存" : L"尚无备注，输入后自动保存";
    UpdateEditorLayout();
}

void PanelWindow::OnEditChanged()
{
    if (loadingText_ || !targetValid_)
    {
        return;
    }
    noteStatus_ = L"有未保存的修改";
    KillTimer(window_, kSaveTimer);
    SetTimer(window_, kSaveTimer, 600, nullptr);
    InvalidateRect(window_, nullptr, FALSE);
}

void PanelWindow::QueuePendingSave()
{
    if (window_ == nullptr || KillTimer(window_, kSaveTimer) == 0 || !targetValid_)
    {
        return;
    }

    SaveRequest request;
    request.generation = generation_;
    request.identity = targetIdentity_;
    request.path = targetPath_;
    request.markdown = EditorText(editor_);
    {
        std::lock_guard lock(saveMutex_);
        saveQueue_.push_back(std::move(request));
    }
    noteStatus_ = L"正在保存…";
    saveCondition_.notify_one();
    InvalidateRect(window_, nullptr, FALSE);
}

void PanelWindow::SaveWorker()
{
    while (true)
    {
        SaveRequest request;
        {
            std::unique_lock lock(saveMutex_);
            saveCondition_.wait(lock, [this]() { return stoppingSaveWorker_ || !saveQueue_.empty(); });
            if (saveQueue_.empty())
            {
                if (stoppingSaveWorker_)
                {
                    return;
                }
                continue;
            }
            request = std::move(saveQueue_.front());
            saveQueue_.pop_front();
        }

        std::wstring error;
        repository_.Save(request.identity, request.path, request.markdown, error);
        auto result = std::make_unique<SaveResult>();
        result->generation = request.generation;
        result->error = std::move(error);
        if (PostMessageW(window_, kSaveCompletedMessage, 0, reinterpret_cast<LPARAM>(result.get())))
        {
            result.release();
        }
    }
}

void PanelWindow::StopSaveWorker()
{
    {
        std::lock_guard lock(saveMutex_);
        stoppingSaveWorker_ = true;
    }
    saveCondition_.notify_one();
    if (saveThread_.joinable())
    {
        saveThread_.join();
    }
}

void PanelWindow::UpdateEditorLayout()
{
    if (editor_ == nullptr)
    {
        return;
    }
    RECT client = {};
    GetClientRect(window_, &client);
    const UINT dpi = GetDpiForWindow(window_);
    const int margin = MulDiv(16, dpi, 96);
    const int top = MulDiv(180, dpi, 96);
    const int width = std::max(0, static_cast<int>(client.right) - margin * 2);
    const int height = std::max(0, static_cast<int>(client.bottom) - top - margin);
    const bool visible = !collapsed_ && height > 0;
    ShowWindow(editor_, visible ? SW_SHOW : SW_HIDE);
    if (visible)
    {
        SetWindowPos(editor_, nullptr, margin, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

bool PanelWindow::EnsureResources()
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
    const auto size = D2D1::SizeU(client.right - client.left, client.bottom - client.top);
    if (FAILED(d2dFactory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(window_, size),
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
    if (FAILED(createFormat(20.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_.put())) ||
        FAILED(createFormat(13.5F, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_.put())) ||
        FAILED(createFormat(12.5F, DWRITE_FONT_WEIGHT_NORMAL, statusFormat_.put())) ||
        FAILED(createFormat(13.0F, DWRITE_FONT_WEIGHT_NORMAL, buttonFormat_.put())))
    {
        return false;
    }

    bodyFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    statusFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

void PanelWindow::Render()
{
    if (!EnsureResources())
    {
        return;
    }

    RECT client = {};
    GetClientRect(window_, &client);
    const float width = static_cast<float>(client.right);
    const float height = static_cast<float>(client.bottom);

    wil::com_ptr<ID2D1SolidColorBrush> background;
    wil::com_ptr<ID2D1SolidColorBrush> border;
    wil::com_ptr<ID2D1SolidColorBrush> text;
    wil::com_ptr<ID2D1SolidColorBrush> secondaryText;
    wil::com_ptr<ID2D1SolidColorBrush> accent;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xF7F8FA), background.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xC7CCD4), border.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x20242A), text.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x626B78), secondaryText.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xE7EEF8), accent.put());

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0xDDE1E6));
    const auto panelRectangle = D2D1::RoundedRect(D2D1::RectF(1.0F, 1.0F, width - 1.0F, height - 1.0F), 12.0F, 12.0F);
    renderTarget_->FillRoundedRectangle(panelRectangle, background.get());
    renderTarget_->DrawRoundedRectangle(panelRectangle, border.get(), 1.0F);

    if (collapsed_)
    {
        renderTarget_->DrawText(
            L"备注",
            2,
            buttonFormat_.get(),
            D2D1::RectF(0.0F, 0.0F, width, height),
            text.get());
    }
    else
    {
        const bool controlsVisible = hovering_ || GetForegroundWindow() == window_;
        const float titleRight = controlsVisible ? width - 188.0F : width - 16.0F;
        const wchar_t* title = !targetValid_ ? L"对象备注" : targetIdentity_.kind == NoteObjectKind::Directory ? L"文件夹备注" : L"文件备注";
        renderTarget_->DrawText(
            title,
            static_cast<UINT32>(wcslen(title)),
            titleFormat_.get(),
            D2D1::RectF(16.0F, 12.0F, std::max(16.0F, titleRight), 46.0F),
            text.get());

        renderTarget_->DrawText(
            targetDescription_.c_str(),
            static_cast<UINT32>(targetDescription_.size()),
            bodyFormat_.get(),
            D2D1::RectF(16.0F, 52.0F, width - 16.0F, 134.0F),
            text.get());
        renderTarget_->DrawText(
            noteStatus_.c_str(),
            static_cast<UINT32>(noteStatus_.size()),
            statusFormat_.get(),
            D2D1::RectF(16.0F, 138.0F, width - 16.0F, 172.0F),
            secondaryText.get());

        collapseButton_ = D2D1::RectF(width - 88.0F, 12.0F, width - 12.0F, 42.0F);
        sideButton_ = D2D1::RectF(width - 174.0F, 12.0F, width - 96.0F, 42.0F);
        if (controlsVisible)
        {
            const auto drawButton = [this](const D2D1_RECT_F& rectangle, const wchar_t* label, UINT32 length, ID2D1Brush* fill, ID2D1Brush* foreground) {
                renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rectangle, 6.0F, 6.0F), fill);
                renderTarget_->DrawText(label, length, buttonFormat_.get(), rectangle, foreground);
            };
            drawButton(sideButton_, L"换侧", 2, accent.get(), text.get());
            drawButton(collapseButton_, L"收起", 2, accent.get(), text.get());
        }
    }

    if (const HRESULT result = renderTarget_->EndDraw(); result == D2DERR_RECREATE_TARGET)
    {
        renderTarget_.reset();
    }
}

void PanelWindow::UpdateHoverState(bool hovering)
{
    if (hovering_ == hovering)
    {
        return;
    }
    hovering_ = hovering;
    InvalidateRect(window_, nullptr, FALSE);
}

LRESULT PanelWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
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
        if (renderTarget_)
        {
            renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        UpdateEditorLayout();
        return 0;
    case WM_DPICHANGED:
        Reposition();
        return 0;
    case WM_MOUSEMOVE:
    {
        UpdateHoverState(true);
        TRACKMOUSEEVENT tracking = {};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = window_;
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        POINT cursor = {};
        GetCursorPos(&cursor);
        RECT bounds = {};
        GetWindowRect(window_, &bounds);
        UpdateHoverState(PtInRect(&bounds, cursor) != FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (collapsed_)
        {
            ToggleCollapsed();
            return 0;
        }
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        const bool controlsVisible = hovering_ || GetForegroundWindow() == window_;
        if (controlsVisible && Contains(collapseButton_, x, y))
        {
            ToggleCollapsed();
        }
        else if (controlsVisible && Contains(sideButton_, x, y))
        {
            ToggleDockSide();
        }
        return 0;
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == editor_ && HIWORD(wParam) == EN_CHANGE)
        {
            OnEditChanged();
        }
        return 0;
    case WM_TIMER:
        if (wParam == kSaveTimer)
        {
            QueuePendingSave();
        }
        return 0;
    case kSaveCompletedMessage:
    {
        std::unique_ptr<SaveResult> result(reinterpret_cast<SaveResult*>(lParam));
        if (result && result->generation == generation_)
        {
            noteStatus_ = result->error.empty() ? L"已自动保存" : result->error;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_NCDESTROY:
        editor_ = nullptr;
        window_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

LRESULT CALLBACK PanelWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    PanelWindow* panel = reinterpret_cast<PanelWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        panel = static_cast<PanelWindow*>(create->lpCreateParams);
        panel->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    return panel != nullptr ? panel->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}
