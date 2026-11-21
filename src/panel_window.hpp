#pragma once

#include "explorer_tracker.hpp"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wil/com.h>

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
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool EnsureResources();
    void Render();
    void Reposition();
    void UpdateHoverState(bool hovering);
    std::wstring ContentText() const;

    HWND window_ = nullptr;
    HWND explorer_ = nullptr;
    HINSTANCE instance_ = nullptr;
    ExplorerSnapshot snapshot_;
    bool collapsed_ = false;
    bool dockRight_ = true;
    bool hovering_ = false;
    D2D1_RECT_F collapseButton_ = {};
    D2D1_RECT_F sideButton_ = {};

    wil::com_ptr<ID2D1Factory> d2dFactory_;
    wil::com_ptr<ID2D1HwndRenderTarget> renderTarget_;
    wil::com_ptr<IDWriteFactory> dwriteFactory_;
    wil::com_ptr<IDWriteTextFormat> titleFormat_;
    wil::com_ptr<IDWriteTextFormat> bodyFormat_;
    wil::com_ptr<IDWriteTextFormat> buttonFormat_;
};
