// Dropdowns, context menus and confirmations drawn by the app itself (no native Windows menus or
// message boxes): borderless popups with the app's theme and fonts, run modally.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "gfx.h"

namespace ea::ui {

// items: "-" is a separator, "#TITLE" a section title (not selectable). checked: the current choice (-1 for none).
// anchor (screen coordinates): the control that opened the list; it opens below it (above when there
// is no room) and is at least as wide. Returns the chosen index into items, or -1.
int runMenu(HWND owner, const RECT& anchor, const std::vector<std::wstring>& items, int checked, const Theme& theme, const Fonts& fonts, int dpi);
// Sectioned menus: one check per section (for example size, frame rate and pixel format in one list).
int runMenu(HWND owner, const RECT& anchor, const std::vector<std::wstring>& items, const std::vector<int>& checked, const Theme& theme, const Fonts& fonts,
            int dpi);

// The app's own confirmation dialog (no MessageBox): a title, a wrapped message and two buttons,
// centred on the owner, modal. The safe choice (cancel) is the default. Returns true for confirm.
bool runConfirm(HWND owner, const std::wstring& title, const std::wstring& message, const std::wstring& confirm, const std::wstring& cancel,
                const Theme& theme, const Fonts& fonts, int dpi);

}  // namespace ea::ui
