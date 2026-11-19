#include "gfx.h"

#include <dwmapi.h>

#include <algorithm>

namespace ea::ui {

void Theme::set(bool d) {
    dark = d;
    if (d) {
        bg = RGB(0, 0, 0);
        ink = RGB(255, 255, 255);
        mid = RGB(153, 153, 153);
        line = RGB(77, 77, 77);
        off = RGB(26, 26, 26);
        hover = RGB(38, 38, 38);
    } else {
        bg = RGB(255, 255, 255);
        ink = RGB(0, 0, 0);
        mid = RGB(102, 102, 102);
        line = RGB(179, 179, 179);
        off = RGB(240, 240, 240);
        hover = RGB(228, 228, 228);
    }
    if (bgBrush) DeleteObject(bgBrush);
    bgBrush = CreateSolidBrush(bg);
}

bool systemPrefersDark() {
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return true;
    return v == 0;
}

void applyDarkTitle(HWND h, bool dark) {
    BOOL v = dark;
    if (FAILED(DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof v))) DwmSetWindowAttribute(h, 19, &v, sizeof v);
    DWORD square = 1;  // DWMWCP_DONOTROUND, as in Weekly
    DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &square, sizeof square);
}

void allowDarkMenus(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set = (SetMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (Flush)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set) set(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush) flush();
}

void Fonts::create(int dpi) {
    destroy();
    auto mk = [&](int px, int weight, const wchar_t* face, DWORD pitch) {
        return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, pitch, face);
    };
    normal = mk(14, FW_NORMAL, L"Segoe UI", VARIABLE_PITCH | FF_SWISS);
    bold = mk(14, FW_SEMIBOLD, L"Segoe UI", VARIABLE_PITCH | FF_SWISS);
    big = mk(18, FW_SEMIBOLD, L"Segoe UI", VARIABLE_PITCH | FF_SWISS);
    mono = mk(13, FW_NORMAL, L"Consolas", FIXED_PITCH | FF_MODERN);
    monoBold = mk(13, FW_BOLD, L"Consolas", FIXED_PITCH | FF_MODERN);
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, normal);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    charW = tm.tmAveCharWidth;
    ascent = tm.tmAscent;
    int h = tm.tmHeight;
    SelectObject(dc, mono);
    GetTextMetricsW(dc, &tm);
    denseH = tm.tmHeight + MulDiv(5, dpi, 96);
    h = std::max<int>(h, tm.tmHeight);
    lineH = h + MulDiv(11, dpi, 96);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
}

void Fonts::destroy() {
    for (HFONT* f : {&normal, &bold, &big, &mono, &monoBold})
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
}

void Painter::fill(const RECT& r, COLORREF c) {
    SetDCBrushColor(dc_, c);
    FillRect(dc_, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void Painter::frame(const RECT& r, COLORREF c) {
    hline(r.left, r.right, r.top, c);
    hline(r.left, r.right, r.bottom - 1, c);
    vline(r.left, r.top, r.bottom, c);
    vline(r.right - 1, r.top, r.bottom, c);
}

void Painter::hline(int x0, int x1, int y, COLORREF c) {
    RECT r{x0, y, x1, y + 1};
    fill(r, c);
}

void Painter::vline(int x, int y0, int y1, COLORREF c) {
    RECT r{x, y0, x + 1, y1};
    fill(r, c);
}

int Painter::text(const RECT& r, const std::wstring& s, COLORREF c, HFONT font, UINT align) {
    if (s.empty()) return 0;
    HGDIOBJ old = SelectObject(dc_, font ? font : f_.normal);
    SetTextColor(dc_, c);
    RECT rr = r;
    DrawTextW(dc_, s.c_str(), (int)s.size(), &rr, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SIZE sz{};
    GetTextExtentPoint32W(dc_, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc_, old);
    return sz.cx;
}

int Painter::measure(const std::wstring& s, HFONT font) {
    HGDIOBJ old = SelectObject(dc_, font ? font : f_.normal);
    SIZE sz{};
    GetTextExtentPoint32W(dc_, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc_, old);
    return sz.cx;
}

std::wstring Painter::fit(const std::wstring& s, int maxPx, HFONT font) {
    if (maxPx <= 0) return L"";
    HGDIOBJ old = SelectObject(dc_, font ? font : f_.normal);
    int n = 0;
    SIZE sz{};
    GetTextExtentExPointW(dc_, s.c_str(), (int)s.size(), maxPx, &n, nullptr, &sz);
    std::wstring r = s;
    if (n < (int)s.size()) {
        int dots = 0;
        SIZE d{};
        GetTextExtentPoint32W(dc_, L"...", 3, &d);
        GetTextExtentExPointW(dc_, s.c_str(), (int)s.size(), maxPx - d.cx, &dots, nullptr, &sz);
        r = s.substr(0, std::max(0, dots)) + L"...";
    }
    SelectObject(dc_, old);
    return r;
}

void Painter::bitmap(HBITMAP b, const RECT& r) {
    if (!b) return;
    HDC mem = CreateCompatibleDC(dc_);
    HGDIOBJ old = SelectObject(mem, b);
    BITMAP bm;
    GetObject(b, sizeof bm, &bm);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (bm.bmWidth == w && bm.bmHeight == h) {
        BitBlt(dc_, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(dc_, HALFTONE);
        StretchBlt(dc_, r.left, r.top, w, h, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

}  // namespace ea::ui
