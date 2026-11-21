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
void allowDarkMenus(bool dark);  // uxtheme preferred app mode, so native scroll bars and dialogs follow the theme

// Line icons in the ink colour (no bitmaps, no colours).
// Anti-aliased (GDI+); the window glyphs (I_Min and after) keep whole-pixel strokes so they stay sharp.
enum Icon { I_History, I_Log, I_Settings, I_Close, I_Check, I_Chevron, I_Up, I_Down, I_Play, I_Pause, I_Retry, I_Folder, I_Open, I_Link, I_Plus,
            I_Min, I_Max, I_Restore, I_WinClose };

struct Fonts {
    // Segoe UI for labels and controls; Consolas for technical lines (streams, plans, logs, numbers).
    HFONT normal = nullptr, bold = nullptr, big = nullptr, mono = nullptr, monoBold = nullptr;
    int charW = 6, lineH = 23, denseH = 17, textH = 16;  // denseH: monospace lists (plans, logs); textH: one Segoe UI line
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
    // Draws icon k centred in r; size is the glyph box in pixels.
    void icon(int k, const RECT& r, COLORREF c, int size);
    HDC dc() const { return dc_; }

private:
    HDC dc_;
    const Theme& t_;
    const Fonts& f_;
};

// Layout sizes are design units: 0.7 px at 96 dpi (owner's request, 2026-11-21: the UI 30% smaller).
// Fonts are sized separately (Fonts::create) so text stays readable.
constexpr int kUiScale = 70;
inline int scale(int v, int dpi) { return MulDiv(v, dpi * kUiScale, 96 * 100); }

}  // namespace ea::ui
