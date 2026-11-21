#include "explorer_tracker.hpp"

#include <ShlObj.h>
#include <ShlDisp.h>
#include <ExDisp.h>
#include <shlwapi.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <optional>

namespace
{
std::wstring LocationPath(IWebBrowser2* browser, bool& isFileSystemLocation)
{
    BSTR rawUrl = nullptr;
    if (SUCCEEDED(browser->get_LocationURL(&rawUrl)) && rawUrl != nullptr)
    {
        wil::unique_bstr url(rawUrl);
        DWORD size = 32768;
        std::wstring path(size, L'\0');
        if (SUCCEEDED(PathCreateFromUrlW(url.get(), path.data(), &size, 0)))
        {
            path.resize(size);
            if (!path.empty() && path.back() == L'\0')
            {
                path.pop_back();
            }
            isFileSystemLocation = true;
            return path;
        }
    }

    BSTR rawName = nullptr;
    if (SUCCEEDED(browser->get_LocationName(&rawName)) && rawName != nullptr)
    {
        wil::unique_bstr name(rawName);
        return name.get();
    }
    return L"无法识别当前位置";
}

ExplorerSnapshot ReadBrowser(IWebBrowser2* browser)
{
    ExplorerSnapshot snapshot;
    snapshot.location = LocationPath(browser, snapshot.file_system_location);

    wil::com_ptr<IDispatch> document;
    if (FAILED(browser->get_Document(document.put())))
    {
        return snapshot;
    }

    wil::com_ptr<IShellFolderViewDual> view;
    if (FAILED(document->QueryInterface(IID_PPV_ARGS(view.put()))))
    {
        return snapshot;
    }

    wil::com_ptr<FolderItems> items;
    if (FAILED(view->SelectedItems(items.put())) || !items)
    {
        return snapshot;
    }

    items->get_Count(&snapshot.selection_count);
    if (snapshot.selection_count == 1)
    {
        VARIANT firstItem;
        VariantInit(&firstItem);
        firstItem.vt = VT_I4;
        firstItem.lVal = 0;

        wil::com_ptr<FolderItem> item;
        if (SUCCEEDED(items->Item(firstItem, item.put())) && item)
        {
            BSTR rawPath = nullptr;
            if (SUCCEEDED(item->get_Path(&rawPath)) && rawPath != nullptr)
            {
                wil::unique_bstr path(rawPath);
                snapshot.selected_path = path.get();
            }
        }
    }
    return snapshot;
}
}

bool ExplorerTracker::IsFileExplorerWindow(HWND window)
{
    if (window == nullptr || !IsWindow(window))
    {
        return false;
    }

    wchar_t className[64] = {};
    return GetClassNameW(window, className, static_cast<int>(std::size(className))) > 0 &&
        _wcsicmp(className, L"CabinetWClass") == 0;
}

ExplorerSnapshot ExplorerTracker::Query(HWND window) const
{
    ExplorerSnapshot snapshot;
    wil::com_ptr<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(shellWindows.put()))))
    {
        snapshot.error = L"无法连接 Windows Shell";
        return snapshot;
    }

    long count = 0;
    if (FAILED(shellWindows->get_Count(&count)))
    {
        snapshot.error = L"无法枚举资源管理器窗口";
        return snapshot;
    }

    std::optional<ExplorerSnapshot> fallback;
    std::wstring activeTitle(GetWindowTextLengthW(window) + 1, L'\0');
    if (activeTitle.size() > 1)
    {
        const int titleLength = GetWindowTextW(window, activeTitle.data(), static_cast<int>(activeTitle.size()));
        activeTitle.resize(titleLength);
        const size_t separator = activeTitle.rfind(L" - ");
        if (separator != std::wstring::npos)
        {
            activeTitle.resize(separator);
        }
        const size_t additionalTabs = activeTitle.find(L" 和 ");
        if (additionalTabs != std::wstring::npos)
        {
            activeTitle.resize(additionalTabs);
        }
    }
    else
    {
        activeTitle.clear();
    }

    for (long index = 0; index < count; ++index)
    {
        VARIANT itemIndex;
        VariantInit(&itemIndex);
        itemIndex.vt = VT_I4;
        itemIndex.lVal = index;

        wil::com_ptr<IDispatch> dispatch;
        if (FAILED(shellWindows->Item(itemIndex, dispatch.put())))
        {
            continue;
        }

        wil::com_ptr<IWebBrowser2> browser;
        if (FAILED(dispatch->QueryInterface(IID_PPV_ARGS(browser.put()))))
        {
            continue;
        }

        SHANDLE_PTR browserWindow = 0;
        if (FAILED(browser->get_HWND(&browserWindow)) ||
            reinterpret_cast<HWND>(browserWindow) != window)
        {
            continue;
        }

        ExplorerSnapshot current = ReadBrowser(browser.get());
        BSTR rawName = nullptr;
        if (SUCCEEDED(browser->get_LocationName(&rawName)) && rawName != nullptr)
        {
            wil::unique_bstr name(rawName);
            if (!activeTitle.empty() && activeTitle == name.get())
            {
                return current;
            }
        }
        if (!fallback.has_value())
        {
            fallback = std::move(current);
        }
    }

    if (fallback.has_value())
    {
        return std::move(*fallback);
    }
    snapshot.error = L"未找到对应的 Shell 窗口";
    return snapshot;
}
