#include "gfx.h"

#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <vector>

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
    // 13 px Segoe UI and 12 px Consolas: readable at the compact scale (see scale()), which sizes only space.
    normal = mk(13, FW_NORMAL, L"Segoe UI", VARIABLE_PITCH | FF_SWISS);
    bold = mk(13, FW_SEMIBOLD, L"Segoe UI", VARIABLE_PITCH | FF_SWISS);
    big = mk(17, FW_SEMIBOLD, L"Segoe UI", VARIABLE_PITCH | FF_SWISS);
    mono = mk(12, FW_NORMAL, L"Consolas", FIXED_PITCH | FF_MODERN);
    monoBold = mk(12, FW_BOLD, L"Consolas", FIXED_PITCH | FF_MODERN);
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, normal);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    charW = tm.tmAveCharWidth;
    int h = tm.tmHeight;
    textH = h;
    SelectObject(dc, mono);
    GetTextMetricsW(dc, &tm);
    denseH = tm.tmHeight + MulDiv(4, dpi, 96);
    h = std::max<int>(h, tm.tmHeight);
    lineH = h + MulDiv(7, dpi, 96);
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

namespace {

// GDI+ draws the line icons with anti-aliasing; GDI pens alias diagonals and curves.
bool gdipReady() {
    static ULONG_PTR token = 0;
    static bool ok = [] {
        Gdiplus::GdiplusStartupInput in;
        return Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok;
    }();
    return ok;
}

}  // namespace

void Painter::icon(int k, const RECT& r, COLORREF c, int size) {
    if (!gdipReady()) return;
    using namespace Gdiplus;
    Graphics g(dc_);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeNone);  // integer coordinates are pixel centres
    // Stroke: about 1/12 of the glyph in whole pixels, so straight lines stay sharp at every scale.
    bool chrome = k >= I_Min;
    float sw = chrome ? std::max(1.0f, std::floor(size / 10.0f)) : std::max(1.0f, std::round(size / 12.0f));
    // Axis-aligned strokes land on whole pixels (odd widths on centres, even widths between them).
    bool odd = ((int)std::lround(sw)) % 2 == 1 && std::fabs(sw - std::round(sw)) < 0.01f;
    auto snap = [&](float v) { return odd ? std::round(v) : std::floor(v) + 0.5f; };
    float cx = snap((r.left + r.right - 1) / 2.0f), cy = snap((r.top + r.bottom - 1) / 2.0f);
    float h = std::max(3.0f, size / 2.0f);
    Color col(255, GetRValue(c), GetGValue(c), GetBValue(c));
    Pen pen(col, sw);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(chrome ? LineCapFlat : LineCapRound);
    pen.SetEndCap(chrome ? LineCapFlat : LineCapRound);
    auto line = [&](float x0, float y0, float x1, float y1) { g.DrawLine(&pen, x0, y0, x1, y1); };
    switch (k) {
        case I_History: {  // clock face with two hands
            float R = std::round(h * 0.86f);
            g.DrawEllipse(&pen, cx - R, cy - R, 2 * R, 2 * R);
            line(cx, cy, cx, cy - R * 0.58f);
            line(cx, cy, cx + R * 0.42f, cy + R * 0.24f);
            break;
        }
        case I_Log: {  // lines of text with bullets
            float gap = std::round(h * 0.56f);  // whole pixels: every line stays on the grid
            SolidBrush b(col);
            float dot = std::max(1.0f, sw * 0.75f);
            for (int i = -1; i <= 1; i++) {
                float y = cy + i * gap;
                g.FillEllipse(&b, cx - h * 0.82f - dot, y - dot, dot * 2, dot * 2);
                line(cx - h * 0.38f, y, i == 1 ? cx + h * 0.36f : cx + h * 0.86f, y);
            }
            break;
        }
        case I_Settings: {  // gear: eight teeth around a hole, as an outline
            const float pi = 3.14159265f;
            std::vector<PointF> pts;
            float ro = h * 0.94f, ri = h * 0.70f;
            for (int t = 0; t < 8; t++) {
                float a = t * pi / 4;
                const float d[4] = {-0.30f, -0.15f, 0.15f, 0.30f};
                const float rr[4] = {ri, ro, ro, ri};
                for (int j = 0; j < 4; j++) pts.push_back(PointF(cx + std::cos(a + d[j]) * rr[j], cy + std::sin(a + d[j]) * rr[j]));
            }
            g.DrawPolygon(&pen, pts.data(), (INT)pts.size());
            float hr = h * 0.30f;
            g.DrawEllipse(&pen, cx - hr, cy - hr, 2 * hr, 2 * hr);
            break;
        }
        case I_Close: {
            float q = h * 0.62f;
            line(cx - q, cy - q, cx + q, cy + q);
            line(cx - q, cy + q, cx + q, cy - q);
            break;
        }
        case I_Check: {
            float q = h * 0.72f;
            PointF p[3] = {PointF(cx - q, cy + q * 0.02f), PointF(cx - q * 0.32f, cy + q * 0.66f), PointF(cx + q, cy - q * 0.62f)};
            g.DrawLines(&pen, p, 3);
            break;
        }
        case I_Chevron:
        case I_Up:
        case I_Down: {
            float q = h * 0.5f, dy = (k == I_Up) ? -1.0f : 1.0f;
            PointF p[3] = {PointF(cx - q, cy - dy * q * 0.5f), PointF(cx, cy + dy * q * 0.5f), PointF(cx + q, cy - dy * q * 0.5f)};
            g.DrawLines(&pen, p, 3);
            break;
        }
        case I_Play: {  // outlined triangle pointing right
            float q = h * 0.78f;
            PointF p[3] = {PointF(cx - q * 0.62f, cy - q), PointF(cx + q * 0.9f, cy), PointF(cx - q * 0.62f, cy + q)};
            g.DrawPolygon(&pen, p, 3);
            break;
        }
        case I_Pause: {
            float q = std::round(h * 0.36f);
            line(cx - q, cy - h * 0.72f, cx - q, cy + h * 0.72f);
            line(cx + q, cy - h * 0.72f, cx + q, cy + h * 0.72f);
            break;
        }
        case I_Retry: {  // circular arrow, clockwise, the head near the top
            float R = h * 0.78f;
            const float pi = 3.14159265f;
            g.DrawArc(&pen, cx - R, cy - R, 2 * R, 2 * R, -40.0f, 290.0f);
            float te = (-40.0f + 290.0f) * pi / 180.0f;
            PointF e(cx + R * std::cos(te), cy + R * std::sin(te));
            PointF d(-std::sin(te), std::cos(te)), n(std::cos(te), std::sin(te));  // direction of travel, outward
            float a = h * 0.46f;
            line(e.X, e.Y, e.X - a * d.X + a * 0.8f * n.X, e.Y - a * d.Y + a * 0.8f * n.Y);
            line(e.X, e.Y, e.X - a * d.X - a * 0.8f * n.X, e.Y - a * d.Y - a * 0.8f * n.Y);
            break;
        }
        case I_Folder: {
            PointF p[6] = {PointF(cx - h * 0.9f, cy - h * 0.66f), PointF(cx - h * 0.22f, cy - h * 0.66f), PointF(cx + h * 0.02f, cy - h * 0.4f),
                           PointF(cx + h * 0.9f, cy - h * 0.4f),  PointF(cx + h * 0.9f, cy + h * 0.7f),   PointF(cx - h * 0.9f, cy + h * 0.7f)};
            g.DrawPolygon(&pen, p, 6);
            break;
        }
        case I_Open: {  // a box with an arrow leaving its top-right corner
            PointF p[5] = {PointF(cx - h * 0.1f, cy - h * 0.72f), PointF(cx - h * 0.8f, cy - h * 0.72f), PointF(cx - h * 0.8f, cy + h * 0.8f),
                           PointF(cx + h * 0.72f, cy + h * 0.8f), PointF(cx + h * 0.72f, cy + h * 0.1f)};
            g.DrawLines(&pen, p, 5);
            line(cx - h * 0.05f, cy + h * 0.05f, cx + h * 0.86f, cy - h * 0.86f);
            PointF a[3] = {PointF(cx + h * 0.24f, cy - h * 0.86f), PointF(cx + h * 0.86f, cy - h * 0.86f), PointF(cx + h * 0.86f, cy - h * 0.24f)};
            g.DrawLines(&pen, a, 3);
            break;
        }
        case I_Link: {  // two chain links at 45 degrees
            g.TranslateTransform(cx, cy);
            g.RotateTransform(-45.0f);
            float lw = h * 1.12f, lh = h * 0.62f, r = lh / 2;
            for (float x0 : {-h * 1.0f, -h * 0.12f}) {
                GraphicsPath path;
                path.AddArc(x0, -r, lh, lh, 90.0f, 180.0f);
                path.AddArc(x0 + lw - lh, -r, lh, lh, 270.0f, 180.0f);
                path.CloseFigure();
                g.DrawPath(&pen, &path);
            }
            g.ResetTransform();
            break;
        }
        case I_Plus: {
            float q = h * 0.72f;
            line(cx - q, cy, cx + q, cy);
            line(cx, cy - q, cx, cy + q);
            break;
        }
        case I_Download: {  // an arrow down onto a tray
            float q = h * 0.82f;
            float tray = snap(cy + q * 0.86f);
            line(cx, cy - q, cx, cy + q * 0.42f);
            PointF a[3] = {PointF(cx - q * 0.52f, cy - q * 0.1f), PointF(cx, cy + q * 0.42f), PointF(cx + q * 0.52f, cy - q * 0.1f)};
            g.DrawLines(&pen, a, 3);
            line(cx - q * 0.86f, tray, cx + q * 0.86f, tray);
            break;
        }
        case I_Go: {  // an arrow to the right: start
            float q = h * 0.78f;
            line(cx - q, cy, cx + q * 0.9f, cy);
            PointF a[3] = {PointF(cx + q * 0.28f, cy - q * 0.62f), PointF(cx + q * 0.9f, cy), PointF(cx + q * 0.28f, cy + q * 0.62f)};
            g.DrawLines(&pen, a, 3);
            break;
        }
        // Window buttons: square glyphs of whole pixels, like Windows' own.
        case I_Min: line(cx - h, cy, cx + h, cy); break;
        case I_Max: g.DrawRectangle(&pen, cx - h, cy - h, 2 * h, 2 * h); break;
        case I_Restore: {
            float o = std::round(h * 0.4f);
            g.DrawRectangle(&pen, cx - h, cy - h + o, 2 * h - o, 2 * h - o);
            PointF p[4] = {PointF(cx - h + o, cy - h + o), PointF(cx - h + o, cy - h), PointF(cx + h, cy - h), PointF(cx + h, cy + h - o)};
            g.DrawLines(&pen, p, 4);
            break;
        }
        case I_WinClose: {
            g.SetPixelOffsetMode(PixelOffsetModeHalf);
            line(cx - h, cy - h, cx + h + 1, cy + h + 1);
            line(cx - h, cy + h + 1, cx + h + 1, cy - h);
            break;
        }
        default: break;
    }
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
