#include "panel_window.hpp"

#include <Windowsx.h>

#include <algorithm>
#include <string>

namespace
{
constexpr wchar_t kPanelClassName[] = L"RedefinePanelWindow";

bool Contains(const D2D1_RECT_F& rectangle, float x, float y)
{
    return x >= rectangle.left && x <= rectangle.right && y >= rectangle.top && y <= rectangle.bottom;
}
}

PanelWindow::~PanelWindow()
{
    if (window_ != nullptr && IsWindow(window_))
    {
        DestroyWindow(window_);
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
    return window_ != nullptr;
}

HWND PanelWindow::Hwnd() const
{
    return window_;
}

void PanelWindow::Hide()
{
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
    InvalidateRect(window_, nullptr, FALSE);
}

void PanelWindow::Reposition()
{
    if (explorer_ != nullptr)
    {
        ShowFor(explorer_, snapshot_);
    }
}

std::wstring PanelWindow::ContentText() const
{
    if (!snapshot_.error.empty())
    {
        return snapshot_.error;
    }

    std::wstring text = L"当前目录\n";
    text += snapshot_.location.empty() ? L"无法读取" : snapshot_.location;
    text += L"\n\n选中状态\n";
    if (snapshot_.selection_count == 0)
    {
        text += L"未选中项目，将使用当前文件夹";
    }
    else if (snapshot_.selection_count == 1)
    {
        text += snapshot_.selected_path.empty() ? L"已选中 1 项，但无法读取路径" : snapshot_.selected_path;
    }
    else
    {
        text += L"已选中 " + std::to_wstring(snapshot_.selection_count) + L" 项";
    }
    return text;
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
        FAILED(createFormat(15.0F, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_.put())) ||
        FAILED(createFormat(13.0F, DWRITE_FONT_WEIGHT_NORMAL, buttonFormat_.put())))
    {
        return false;
    }

    bodyFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
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
    wil::com_ptr<ID2D1SolidColorBrush> accent;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xF7F8FA), background.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xC7CCD4), border.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x20242A), text.put());
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
        renderTarget_->DrawText(
            L"备注跟踪原型",
            6,
            titleFormat_.get(),
            D2D1::RectF(16.0F, 12.0F, std::max(16.0F, titleRight), 46.0F),
            text.get());

        const std::wstring content = ContentText();
        renderTarget_->DrawText(
            content.c_str(),
            static_cast<UINT32>(content.size()),
            bodyFormat_.get(),
            D2D1::RectF(16.0F, 58.0F, width - 16.0F, height - 16.0F),
            text.get());

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
        UpdateHoverState(false);
        return 0;
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
    case WM_NCDESTROY:
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
