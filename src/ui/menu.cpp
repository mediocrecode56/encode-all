#include "menu.h"

#include <algorithm>

namespace ea::ui {

namespace {

const wchar_t* kMenuClass = L"EncodeAllMenu";

struct Menu {
    const std::vector<std::wstring>* items = nullptr;
    const Theme* theme = nullptr;
    const Fonts* fonts = nullptr;
    int dpi = 96;
    int checked = -1, hover = -1, pressed = -1;
    int scroll = 0, contentH = 0, viewH = 0, width = 0;
    int rowH = 0, sepH = 0, padY = 0, padL = 0, padR = 0;
    std::vector<int> top;  // y of each item in content coordinates
    int result = -1;
    bool done = false;
    HWND hwnd = nullptr;

    int S(int v) const { return MulDiv(v, dpi, 96); }
    bool sep(int i) const { return (*items)[i] == L"-"; }
    int height(int i) const { return sep(i) ? sepH : rowH; }

    void layout() {
        top.clear();
        int y = padY;
        for (size_t i = 0; i < items->size(); i++) {
            top.push_back(y);
            y += height((int)i);
        }
        contentH = y + padY;
    }
    int itemAt(POINT p) const {
        if (p.x < 0 || p.x >= width || p.y < 0 || p.y >= viewH) return -1;
        int y = p.y + scroll;
        for (size_t i = 0; i < items->size(); i++)
            if (y >= top[i] && y < top[i] + height((int)i)) return sep((int)i) ? -1 : (int)i;
        return -1;
    }
    void clampScroll() { scroll = std::clamp(scroll, 0, std::max(0, contentH - viewH)); }
    void reveal(int i) {
        if (i < 0) return;
        if (top[i] - padY < scroll) scroll = top[i] - padY;
        if (top[i] + rowH + padY > scroll + viewH) scroll = top[i] + rowH + padY - viewH;
        clampScroll();
    }
    void move(int dir) {
        int n = (int)items->size();
        if (!n) return;
        int i = hover >= 0 ? hover : (checked >= 0 ? checked - dir : (dir > 0 ? -1 : n));
        for (int k = 0; k < n; k++) {
            i = (i + dir + n) % n;
            if (!sep(i)) break;
        }
        hover = i;
        reveal(i);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void finish(int r) {
        result = r;
        done = true;
    }

    void paint(HDC dc) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(m, bmp);
        Painter p(m, *theme, *fonts);
        p.fill(rc, theme->bg);
        bool scrollable = contentH > viewH;
        int right = rc.right - 1 - (scrollable ? S(6) : 0);
        for (size_t i = 0; i < items->size(); i++) {
            int y = top[i] - scroll;
            int h = height((int)i);
            if (y + h < 0 || y > rc.bottom) continue;
            if (sep((int)i)) {
                p.hline(S(10), right - S(10), y + h / 2, theme->line);
                continue;
            }
            RECT row{1, y, right, y + h};
            bool hot = (int)i == hover;
            if (hot) p.fill(row, (int)i == pressed ? theme->line : theme->hover);
            if ((int)i == checked) p.icon(I_Check, RECT{1, y, padL, y + h}, theme->ink, S(12));
            p.text(RECT{padL, y, right - S(8), y + h}, (*items)[i], theme->ink, (int)i == checked ? fonts->bold : fonts->normal);
        }
        if (scrollable) {
            int track = rc.bottom - S(8);
            int th = std::max(S(16), track * viewH / contentH);
            int ty = S(4) + (track - th) * scroll / std::max(1, contentH - viewH);
            p.fill(RECT{rc.right - S(5), ty, rc.right - S(2), ty + th}, theme->line);
        }
        p.frame(rc, theme->mid);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, oldBmp);
        DeleteObject(bmp);
        DeleteDC(m);
    }
};

LRESULT CALLBACK menuProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Menu* mn = (Menu*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
            break;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (mn) mn->paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (mn && !mn->done && (HWND)l != h) mn->finish(-1);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
    }
    return DefWindowProcW(h, msg, w, l);
}

}  // namespace

int runMenu(HWND owner, const RECT& anchor, const std::vector<std::wstring>& items, int checked, const Theme& theme, const Fonts& fonts, int dpi) {
    if (items.empty()) return -1;
    static bool registered = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = menuProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kMenuClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    Menu mn;
    mn.items = &items;
    mn.theme = &theme;
    mn.fonts = &fonts;
    mn.dpi = dpi;
    mn.checked = checked >= 0 && checked < (int)items.size() && items[checked] != L"-" ? checked : -1;
    mn.rowH = fonts.lineH - mn.S(2);
    mn.sepH = mn.S(9);
    mn.padY = mn.S(4);
    mn.padL = mn.S(30);
    mn.padR = mn.S(22);
    mn.layout();
    // Width: the widest label (bold for the checked one), at least the control's width.
    HDC dc = GetDC(owner);
    int textW = 0;
    for (size_t i = 0; i < items.size(); i++) {
        HGDIOBJ old = SelectObject(dc, (int)i == mn.checked ? fonts.bold : fonts.normal);
        SIZE sz{};
        GetTextExtentPoint32W(dc, items[i].c_str(), (int)items[i].size(), &sz);
        SelectObject(dc, old);
        textW = std::max<int>(textW, sz.cx);
    }
    ReleaseDC(owner, dc);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    RECT work = mi.rcWork;
    int w = std::max<int>(textW + mn.padL + mn.padR, anchor.right - anchor.left);
    w = std::min<int>(w, (work.right - work.left) * 2 / 3);
    int h = mn.contentH;
    int below = work.bottom - anchor.bottom, above = anchor.top - work.top;
    int y;
    if (h <= below || below >= above) {
        h = std::min(h, below);
        y = anchor.bottom;
    } else {
        h = std::min(h, above);
        y = anchor.top - h;
    }
    h = std::max(h, std::min(mn.contentH, mn.rowH * 3));
    int x = std::clamp<int>(anchor.left, work.left, std::max<int>(work.left, work.right - w));
    mn.width = w;
    mn.viewH = h;
    mn.reveal(mn.checked);
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kMenuClass, L"", WS_POPUP, x, y, w, h, owner, nullptr, inst, &mn);
    if (!hw) return -1;
    mn.hwnd = hw;
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);
    SetCapture(hw);
    MSG m;
    while (!mn.done) {
        if (!GetMessageW(&m, nullptr, 0, 0)) {
            PostQuitMessage((int)m.wParam);
            break;
        }
        switch (m.message) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                switch (m.wParam) {
                    case VK_UP: mn.move(-1); break;
                    case VK_DOWN: mn.move(1); break;
                    case VK_RETURN:
                    case VK_SPACE:
                        if (mn.hover >= 0) mn.finish(mn.hover);
                        else if (mn.checked >= 0) mn.finish(mn.checked);
                        break;
                    case VK_ESCAPE:
                    case VK_TAB:
                    case VK_MENU:
                    case VK_F10: mn.finish(-1); break;
                    default: break;
                }
                continue;
            case WM_CHAR:
            case WM_SYSCHAR:
            case WM_KEYUP:
            case WM_SYSKEYUP:
                continue;
            case WM_MOUSEWHEEL: {
                int notches = GET_WHEEL_DELTA_WPARAM(m.wParam) / WHEEL_DELTA;
                if (!notches) notches = GET_WHEEL_DELTA_WPARAM(m.wParam) > 0 ? 1 : -1;
                mn.scroll -= notches * mn.rowH * 3;
                mn.clampScroll();
                InvalidateRect(hw, nullptr, FALSE);
                continue;
            }
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_LBUTTONDBLCLK: {
                // Captured: positions arrive relative to the menu, wherever the mouse is.
                POINT pt{(short)LOWORD(m.lParam), (short)HIWORD(m.lParam)};
                if (m.hwnd != hw) {
                    ClientToScreen(m.hwnd, &pt);
                    ScreenToClient(hw, &pt);
                }
                int i = mn.itemAt(pt);
                bool inside = pt.x >= 0 && pt.y >= 0 && pt.x < mn.width && pt.y < mn.viewH;
                if (m.message == WM_MOUSEMOVE) {
                    if (i != mn.hover && (i >= 0 || inside)) {
                        mn.hover = i;
                        InvalidateRect(hw, nullptr, FALSE);
                    }
                } else if (m.message == WM_LBUTTONDOWN || m.message == WM_LBUTTONDBLCLK) {
                    if (!inside) mn.finish(-1);
                    else {
                        mn.pressed = i;
                        mn.hover = i;
                        InvalidateRect(hw, nullptr, FALSE);
                    }
                } else if (m.message == WM_LBUTTONUP) {
                    if (inside && i >= 0) mn.finish(i);
                    mn.pressed = -1;
                } else if (m.message == WM_RBUTTONUP) {
                    if (inside && i >= 0) mn.finish(i);
                } else if (!inside) {
                    mn.finish(-1);
                }
                continue;
            }
            default: break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (!mn.done && GetForegroundWindow() != owner) mn.finish(-1);
    }
    mn.done = true;
    if (GetCapture() == hw) ReleaseCapture();
    DestroyWindow(hw);
    return mn.result;
}

}  // namespace ea::ui
