#pragma once

#include <Windows.h>

#include <string>

struct ExplorerSnapshot
{
    std::wstring location;
    std::wstring selected_path;
    long selection_count = 0;
    bool file_system_location = false;
    std::wstring error;
};

class ExplorerTracker
{
public:
    static bool IsFileExplorerWindow(HWND window);

    ExplorerSnapshot Query(HWND window) const;
};
