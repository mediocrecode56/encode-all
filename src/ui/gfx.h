// Theme, fonts, DPI and a tiny GDI painter. Monochrome by design (see BIBLE.md).
#pragma once
#include <windows.h>

#include <string>

namespace ea::ui {

struct Theme {
    bool dark = true;
    COLORREF bg, ink, mid, line, off, hover;
    HBRUSH bgBrush = nullptr;
    void set(bool darkMode);
};

bool systemPrefersDark();
void applyDarkTitle(HWND h, bool dark);
void allowDarkMenus(bool dark);  // uxtheme preferred app mode, so popup menus follow the theme

struct Fonts {
    // Segoe UI for labels and controls; Consolas for technical lines (streams, plans, logs, numbers).
    HFONT normal = nullptr, bold = nullptr, big = nullptr, mono = nullptr, monoBold = nullptr;
    int charW = 7, lineH = 18, denseH = 16, ascent = 13;  // denseH: monospace lists (plans, logs)
    void create(int dpi);
    void destroy();
};

class Painter {
public:
    Painter(HDC dc, const Theme& t, const Fonts& f) : dc_(dc), t_(t), f_(f) { SetBkMode(dc, TRANSPARENT); }
    void fill(const RECT& r, COLORREF c);
    void frame(const RECT& r, COLORREF c);
    void hline(int x0, int x1, int y, COLORREF c);
    void vline(int x, int y0, int y1, COLORREF c);
    // Draws text clipped to r, vertically centred; returns the width used.
    int text(const RECT& r, const std::wstring& s, COLORREF c, HFONT font = nullptr, UINT align = DT_LEFT);
    int measure(const std::wstring& s, HFONT font = nullptr);
    std::wstring fit(const std::wstring& s, int maxPx, HFONT font = nullptr);
    void bitmap(HBITMAP b, const RECT& r);
    HDC dc() const { return dc_; }

private:
    HDC dc_;
    const Theme& t_;
    const Fonts& f_;
};

inline int scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

}  // namespace ea::ui
