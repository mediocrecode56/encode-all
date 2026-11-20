// Dropdowns and context menus drawn by the app itself (no native Windows menus): a borderless popup
// with the app's theme and fonts, run modally like TrackPopupMenu.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "gfx.h"

namespace ea::ui {

// items: "-" is a separator. checked: the current choice (-1 for none).
// anchor (screen coordinates): the control that opened the list; it opens below it (above when there
// is no room) and is at least as wide. Returns the chosen index into items, or -1.
int runMenu(HWND owner, const RECT& anchor, const std::vector<std::wstring>& items, int checked, const Theme& theme, const Fonts& fonts, int dpi);

}  // namespace ea::ui
