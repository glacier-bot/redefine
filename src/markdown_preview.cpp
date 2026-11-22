#include "markdown_preview.hpp"

#include <md4c.h>
#include <wil/com.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
enum class BlockKind
{
    Paragraph,
    Heading,
    Code,
    HorizontalRule,
};

struct InlineStyle
{
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool link = false;

    bool operator==(const InlineStyle&) const = default;
};

struct InlineRun
{
    UINT32 offset = 0;
    UINT32 length = 0;
    InlineStyle style;
};

struct Block
{
    BlockKind kind = BlockKind::Paragraph;
    unsigned headingLevel = 0;
    unsigned quoteDepth = 0;
    unsigned listDepth = 0;
    std::wstring text;
    std::vector<InlineRun> runs;
    wil::com_ptr<IDWriteTextLayout> layout;
    float x = 0;
    float y = 0;
    float textY = 0;
    float height = 0;
    float textHeight = 0;
};

struct ListState
{
    bool ordered = false;
    unsigned next = 1;
    bool tight = false;
};

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::wstring ToWide(const MD_CHAR* text, MD_SIZE size)
{
    return Utf8ToWide(std::string_view(text, size));
}

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
}

struct MarkdownPreview::Implementation
{
    std::vector<Block> blocks;
    Block* current = nullptr;
    std::vector<ListState> lists;
    std::wstring pendingPrefix;
    unsigned quoteDepth = 0;
    int strongDepth = 0;
    int emphasisDepth = 0;
    int codeDepth = 0;
    int linkDepth = 0;
    float layoutWidth = -1.0F;
    float contentHeight = 0.0F;

    void BeginBlock(BlockKind kind, unsigned headingLevel = 0)
    {
        Block block;
        block.kind = kind;
        block.headingLevel = headingLevel;
        block.quoteDepth = quoteDepth;
        block.listDepth = static_cast<unsigned>(lists.size());
        blocks.push_back(std::move(block));
        current = &blocks.back();
        if (!pendingPrefix.empty())
        {
            Append(pendingPrefix, InlineStyle{});
            pendingPrefix.clear();
        }
    }

    void Append(const std::wstring& value, const InlineStyle& style)
    {
        if (current == nullptr || value.empty())
        {
            return;
        }
        const UINT32 offset = static_cast<UINT32>(current->text.size());
        current->text += value;
        if (!current->runs.empty() && current->runs.back().style == style)
        {
            current->runs.back().length += static_cast<UINT32>(value.size());
            return;
        }
        current->runs.push_back(InlineRun{offset, static_cast<UINT32>(value.size()), style});
    }

    static int EnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata)
    {
        auto& self = *static_cast<Implementation*>(userdata);
        switch (type)
        {
        case MD_BLOCK_QUOTE:
            ++self.quoteDepth;
            break;
        case MD_BLOCK_UL:
        {
            const auto* list = static_cast<const MD_BLOCK_UL_DETAIL*>(detail);
            self.lists.push_back(ListState{false, 1, list != nullptr && list->is_tight != 0});
            break;
        }
        case MD_BLOCK_OL:
        {
            const auto* list = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
            self.lists.push_back(ListState{true, list != nullptr ? list->start : 1, list != nullptr && list->is_tight != 0});
            break;
        }
        case MD_BLOCK_LI:
            if (!self.lists.empty())
            {
                auto& list = self.lists.back();
                self.pendingPrefix = list.ordered ? std::to_wstring(list.next++) + L". " : L"• ";
                if (list.tight)
                {
                    self.BeginBlock(BlockKind::Paragraph);
                }
            }
            break;
        case MD_BLOCK_HR:
            self.blocks.push_back(Block{BlockKind::HorizontalRule});
            break;
        case MD_BLOCK_H:
        {
            const auto* heading = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
            self.BeginBlock(BlockKind::Heading, heading != nullptr ? heading->level : 1);
            break;
        }
        case MD_BLOCK_CODE:
            self.BeginBlock(BlockKind::Code);
            break;
        case MD_BLOCK_P:
            self.BeginBlock(BlockKind::Paragraph);
            break;
        default:
            break;
        }
        return 0;
    }

    static int LeaveBlock(MD_BLOCKTYPE type, void*, void* userdata)
    {
        auto& self = *static_cast<Implementation*>(userdata);
        switch (type)
        {
        case MD_BLOCK_QUOTE:
            if (self.quoteDepth > 0)
            {
                --self.quoteDepth;
            }
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (!self.lists.empty())
            {
                self.lists.pop_back();
            }
            break;
        case MD_BLOCK_LI:
            if (!self.lists.empty() && self.lists.back().tight)
            {
                self.current = nullptr;
            }
            self.pendingPrefix.clear();
            break;
        case MD_BLOCK_H:
        case MD_BLOCK_CODE:
        case MD_BLOCK_P:
            self.current = nullptr;
            break;
        default:
            break;
        }
        return 0;
    }

    static int EnterSpan(MD_SPANTYPE type, void*, void* userdata)
    {
        auto& self = *static_cast<Implementation*>(userdata);
        switch (type)
        {
        case MD_SPAN_STRONG:
            ++self.strongDepth;
            break;
        case MD_SPAN_EM:
            ++self.emphasisDepth;
            break;
        case MD_SPAN_CODE:
            ++self.codeDepth;
            break;
        case MD_SPAN_A:
            ++self.linkDepth;
            break;
        default:
            break;
        }
        return 0;
    }

    static int LeaveSpan(MD_SPANTYPE type, void*, void* userdata)
    {
        auto& self = *static_cast<Implementation*>(userdata);
        switch (type)
        {
        case MD_SPAN_STRONG:
            self.strongDepth = std::max(0, self.strongDepth - 1);
            break;
        case MD_SPAN_EM:
            self.emphasisDepth = std::max(0, self.emphasisDepth - 1);
            break;
        case MD_SPAN_CODE:
            self.codeDepth = std::max(0, self.codeDepth - 1);
            break;
        case MD_SPAN_A:
            self.linkDepth = std::max(0, self.linkDepth - 1);
            break;
        default:
            break;
        }
        return 0;
    }

    static int Text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
    {
        auto& self = *static_cast<Implementation*>(userdata);
        if (type == MD_TEXT_HTML || type == MD_TEXT_NULLCHAR || self.current == nullptr)
        {
            return 0;
        }
        if (type == MD_TEXT_BR)
        {
            self.Append(L"\n", InlineStyle{});
            return 0;
        }
        if (type == MD_TEXT_SOFTBR)
        {
            self.Append(L" ", InlineStyle{});
            return 0;
        }
        InlineStyle style;
        style.bold = self.strongDepth > 0;
        style.italic = self.emphasisDepth > 0;
        style.code = self.codeDepth > 0 || self.current->kind == BlockKind::Code;
        style.link = self.linkDepth > 0;
        self.Append(ToWide(text, size), style);
        return 0;
    }

    void Parse(const std::wstring& markdown)
    {
        blocks.clear();
        current = nullptr;
        lists.clear();
        pendingPrefix.clear();
        quoteDepth = 0;
        strongDepth = emphasisDepth = codeDepth = linkDepth = 0;
        layoutWidth = -1.0F;
        contentHeight = 0.0F;

        const std::string utf8 = ToUtf8(markdown);
        MD_PARSER parser = {};
        parser.abi_version = 0;
        parser.flags = MD_FLAG_NOHTML;
        parser.enter_block = EnterBlock;
        parser.leave_block = LeaveBlock;
        parser.enter_span = EnterSpan;
        parser.leave_span = LeaveSpan;
        parser.text = Text;
        md_parse(utf8.data(), static_cast<MD_SIZE>(utf8.size()), &parser, this);
    }

    void EnsureLayouts(IDWriteFactory* factory, float width)
    {
        if (factory == nullptr || (layoutWidth == width && width > 0))
        {
            return;
        }
        layoutWidth = width;
        contentHeight = 0.0F;
        bool first = true;
        for (auto& block : blocks)
        {
            block.layout.reset();
            const float quoteIndent = static_cast<float>(block.quoteDepth) * 16.0F;
            const float listIndent = block.listDepth > 0 ? static_cast<float>(block.listDepth - 1) * 18.0F : 0.0F;
            block.x = quoteIndent + listIndent;
            if (block.kind == BlockKind::HorizontalRule)
            {
                block.y = contentHeight;
                block.height = 18.0F;
                contentHeight += block.height;
                first = false;
                continue;
            }

            float fontSize = 14.5F;
            DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
            const wchar_t* fontFamily = L"Microsoft YaHei UI";
            float spacing = 10.0F;
            if (block.kind == BlockKind::Heading)
            {
                constexpr float sizes[] = {26.0F, 23.0F, 20.0F, 18.0F, 16.5F, 15.0F};
                const unsigned level = std::clamp(block.headingLevel, 1u, 6u);
                fontSize = sizes[level - 1];
                weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
                spacing = 8.0F;
                if (!first)
                {
                    contentHeight += 6.0F;
                }
            }
            else if (block.kind == BlockKind::Code)
            {
                fontSize = 13.5F;
                fontFamily = L"Consolas";
                spacing = 12.0F;
            }

            wil::com_ptr<IDWriteTextFormat> format;
            if (FAILED(factory->CreateTextFormat(
                    fontFamily,
                    nullptr,
                    weight,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    fontSize,
                    L"zh-CN",
                    format.put())))
            {
                continue;
            }
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            const float padding = block.kind == BlockKind::Code ? 8.0F : 0.0F;
            const float availableWidth = std::max(20.0F, width - block.x - padding * 2.0F);
            if (FAILED(factory->CreateTextLayout(
                    block.text.c_str(),
                    static_cast<UINT32>(block.text.size()),
                    format.get(),
                    availableWidth,
                    100000.0F,
                    block.layout.put())))
            {
                continue;
            }

            for (const auto& run : block.runs)
            {
                const DWRITE_TEXT_RANGE range{run.offset, run.length};
                if (run.style.bold)
                {
                    block.layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
                }
                if (run.style.italic)
                {
                    block.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
                }
                if (run.style.link)
                {
                    block.layout->SetUnderline(TRUE, range);
                }
                if (run.style.code && block.kind != BlockKind::Code)
                {
                    block.layout->SetFontFamilyName(L"Consolas", range);
                }
            }

            DWRITE_TEXT_METRICS metrics = {};
            block.layout->GetMetrics(&metrics);
            block.textHeight = metrics.height;
            block.y = contentHeight;
            block.textY = block.y + padding;
            block.height = metrics.height + padding * 2.0F + spacing;
            contentHeight += block.height;
            first = false;
        }
        if (!blocks.empty() && contentHeight >= 10.0F)
        {
            contentHeight -= 4.0F;
        }
    }
};

MarkdownPreview::MarkdownPreview() : implementation_(std::make_unique<Implementation>())
{
}

MarkdownPreview::~MarkdownPreview() = default;

void MarkdownPreview::SetMarkdown(const std::wstring& markdown)
{
    implementation_->Parse(markdown);
}

bool MarkdownPreview::IsEmpty() const
{
    return implementation_->blocks.empty();
}

float MarkdownPreview::ContentHeight(IDWriteFactory* factory, float width)
{
    implementation_->EnsureLayouts(factory, width);
    return implementation_->contentHeight;
}

void MarkdownPreview::Render(
    ID2D1RenderTarget* target,
    IDWriteFactory* factory,
    const D2D1_RECT_F& bounds,
    float scrollOffset,
    ID2D1Brush* textBrush)
{
    const float width = bounds.right - bounds.left;
    implementation_->EnsureLayouts(factory, width);

    wil::com_ptr<ID2D1SolidColorBrush> codeBackground;
    wil::com_ptr<ID2D1SolidColorBrush> quote;
    target->CreateSolidColorBrush(D2D1::ColorF(0xEDF0F4), codeBackground.put());
    target->CreateSolidColorBrush(D2D1::ColorF(0x8796AA), quote.put());

    for (const auto& block : implementation_->blocks)
    {
        const float y = bounds.top + block.y - scrollOffset;
        if (y + block.height < bounds.top || y > bounds.bottom)
        {
            continue;
        }
        const float x = bounds.left + block.x;
        if (block.kind == BlockKind::HorizontalRule)
        {
            target->DrawLine(D2D1::Point2F(x, y + 8.0F), D2D1::Point2F(bounds.right, y + 8.0F), quote.get(), 1.0F);
            continue;
        }
        if (!block.layout)
        {
            continue;
        }
        if (block.kind == BlockKind::Code)
        {
            const auto rectangle = D2D1::RoundedRect(
                D2D1::RectF(x, y, bounds.right, y + block.textHeight + 16.0F),
                5.0F,
                5.0F);
            target->FillRoundedRectangle(rectangle, codeBackground.get());
        }
        for (unsigned depth = 0; depth < block.quoteDepth; ++depth)
        {
            const float lineX = bounds.left + static_cast<float>(depth) * 16.0F + 2.0F;
            target->DrawLine(D2D1::Point2F(lineX, y), D2D1::Point2F(lineX, y + block.textHeight), quote.get(), 2.0F);
        }
        target->DrawTextLayout(D2D1::Point2F(x + (block.kind == BlockKind::Code ? 8.0F : 0.0F), bounds.top + block.textY - scrollOffset), block.layout.get(), textBrush);
    }
}
