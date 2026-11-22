#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <memory>
#include <string>

class MarkdownPreview
{
public:
    MarkdownPreview();
    ~MarkdownPreview();

    void SetMarkdown(const std::wstring& markdown);
    bool IsEmpty() const;
    float ContentHeight(IDWriteFactory* factory, float width);
    void Render(
        ID2D1RenderTarget* target,
        IDWriteFactory* factory,
        const D2D1_RECT_F& bounds,
        float scrollOffset,
        ID2D1Brush* textBrush);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
