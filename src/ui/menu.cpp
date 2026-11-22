#include "menu.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

namespace ea::ui {

namespace {

const wchar_t* kMenuClass = L"EncodeAllMenu";

struct Menu {
    const std::vector<std::wstring>* items = nullptr;
    const Theme* theme = nullptr;
    const Fonts* fonts = nullptr;
    int dpi = 96;
    std::vector<int> checks;  // checked items (one per section in sectioned menus)
    int hover = -1, pressed = -1;
    int scroll = 0, contentH = 0, viewH = 0, width = 0;
    int rowH = 0, sepH = 0, headH = 0, padY = 0, padL = 0, padR = 0;
    std::vector<int> top;  // y of each item in content coordinates
    int result = -1;
    bool done = false;
    HWND hwnd = nullptr;

    int S(int v) const { return scale(v, dpi); }
    bool sep(int i) const { return (*items)[i] == L"-"; }
    bool head(int i) const { return !(*items)[i].empty() && (*items)[i][0] == L'#'; }
    bool inert(int i) const { return sep(i) || head(i); }
    bool isChecked(int i) const { return std::find(checks.begin(), checks.end(), i) != checks.end(); }
    int firstCheck() const { return checks.empty() ? -1 : checks.front(); }
    int height(int i) const { return sep(i) ? sepH : head(i) ? headH : rowH; }

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
            if (y >= top[i] && y < top[i] + height((int)i)) return inert((int)i) ? -1 : (int)i;
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
        int i = hover >= 0 ? hover : (firstCheck() >= 0 ? firstCheck() - dir : (dir > 0 ? -1 : n));
        for (int k = 0; k < n; k++) {
            i = (i + dir + n) % n;
            if (!inert(i)) break;
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
            if (head((int)i)) {  // section title: small, mid ink, no hover
                p.text(RECT{S(10), y + S(2), right - S(8), y + h}, (*items)[i].substr(1), theme->mid, fonts->bold);
                continue;
            }
            RECT row{1, y, right, y + h};
            bool hot = (int)i == hover;
            bool on = isChecked((int)i);
            if (hot) p.fill(row, (int)i == pressed ? theme->line : theme->hover);
            if (on) p.icon(I_Check, RECT{1, y, padL, y + h}, theme->ink, S(14));
            p.text(RECT{padL, y, right - S(8), y + h}, (*items)[i], theme->ink, on ? fonts->bold : fonts->normal);
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
    return runMenu(owner, anchor, items, checked >= 0 ? std::vector<int>{checked} : std::vector<int>{}, theme, fonts, dpi);
}

int runMenu(HWND owner, const RECT& anchor, const std::vector<std::wstring>& items, const std::vector<int>& checked, const Theme& theme, const Fonts& fonts,
            int dpi) {
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
    for (int c : checked)
        if (c >= 0 && c < (int)items.size() && !mn.inert(c)) mn.checks.push_back(c);
    mn.rowH = fonts.lineH - mn.S(2);
    mn.sepH = mn.S(9);
    mn.headH = fonts.denseH + mn.S(4);
    mn.padY = mn.S(4);
    mn.padL = mn.S(30);
    mn.padR = mn.S(22);
    mn.layout();
    // Width: the widest label (bold for the checked one), at least the control's width.
    HDC dc = GetDC(owner);
    int textW = 0;
    for (size_t i = 0; i < items.size(); i++) {
        HGDIOBJ old = SelectObject(dc, mn.isChecked((int)i) || mn.head((int)i) ? fonts.bold : fonts.normal);
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
    mn.reveal(mn.firstCheck());
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
                        else if (mn.firstCheck() >= 0 && mn.checks.size() == 1) mn.finish(mn.firstCheck());
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

// ---------- confirmation dialog ----------

namespace {

const wchar_t* kConfirmClass = L"EncodeAllConfirm";
constexpr UINT kFrameReady = WM_APP + 1;

struct Confirm {
    const std::wstring *title = nullptr, *message = nullptr, *labels[2] = {};
    const Theme* theme = nullptr;
    const Fonts* fonts = nullptr;
    int dpi = 96;
    int focus = 0, hover = -1, pressed = -1;  // 0 cancel, 1 confirm
    RECT btn[2] = {};
    RECT msgRect{}, picture{};
    HBITMAP frame = nullptr;
    std::wstring frameError;
    bool preview = false, loading = false;
    bool done = false, result = false, next = false;

    int S(int v) const { return scale(v, dpi); }
    void choose(int i) {
        if (i == 1 && preview) { if (!loading) next = true; }
        else { result = i == 1; done = true; }
    }
    int at(POINT p) const {
        for (int i = 0; i < 2; i++)
            if (PtInRect(&btn[i], p)) return i;
        return -1;
    }
    void paint(HWND h, HDC dc) {
        RECT rc;
        GetClientRect(h, &rc);
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(m, bmp);
        Painter p(m, *theme, *fonts);
        p.fill(rc, theme->bg);
        int pad = S(20);
        p.text(RECT{pad, pad, rc.right - pad, pad + fonts->lineH}, *title, theme->ink, fonts->big);
        HGDIOBJ of = SelectObject(m, fonts->normal);
        SetTextColor(m, theme->ink);
        RECT mr = msgRect;
        DrawTextW(m, message->c_str(), (int)message->size(), &mr, DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(m, of);
        if (preview) {
            p.frame(picture, theme->line);
            if (frame) {
                RECT inner{picture.left + 1, picture.top + 1, picture.right - 1, picture.bottom - 1};
                p.bitmap(frame, inner);
            } else {
                RECT inner{picture.left + pad, picture.top + pad, picture.right - pad, picture.bottom - pad};
                HGDIOBJ oldFont = SelectObject(m, fonts->normal);
                SetTextColor(m, theme->mid);
                const std::wstring label = loading ? L"Rendering a frame…" : frameError;
                DrawTextW(m, label.c_str(), (int)label.size(), &inner, DT_WORDBREAK | DT_NOPREFIX | DT_CENTER);
                SelectObject(m, oldFont);
            }
        }
        for (int i = 0; i < 2; i++) {
            // The focused button is inverted; the other is a framed box that fills on hover.
            bool disabled = i == 1 && loading;
            bool filled = !disabled && (focus == i || pressed == i);
            if (filled) p.fill(btn[i], theme->ink);
            else if (hover == i) p.fill(btn[i], theme->hover);
            p.frame(btn[i], filled || hover == i ? theme->ink : theme->mid);
            p.text(btn[i], *labels[i], filled ? theme->bg : disabled ? theme->mid : theme->ink, fonts->bold, DT_CENTER);
        }
        p.frame(rc, theme->mid);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, old);
        DeleteObject(bmp);
        DeleteDC(m);
    }
};

LRESULT CALLBACK confirmProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Confirm* c = (Confirm*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams); break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (c) c->paint(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCHITTEST: {  // drag anywhere outside the buttons
            LRESULT r = DefWindowProcW(h, msg, w, l);
            POINT p{(short)LOWORD(l), (short)HIWORD(l)};
            ScreenToClient(h, &p);
            return r == HTCLIENT && c && c->at(p) < 0 ? HTCAPTION : r;
        }
        case WM_MOUSEMOVE: {
            if (!c) break;
            int i = c->at(POINT{(short)LOWORD(l), (short)HIWORD(l)});
            if (i != c->hover) {
                c->hover = i;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, h, 0};
            TrackMouseEvent(&te);
            SetCursor(LoadCursor(nullptr, i >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            if (c && c->hover >= 0) {
                c->hover = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (c) {
                c->pressed = c->at(POINT{(short)LOWORD(l), (short)HIWORD(l)});
                if (c->pressed >= 0) SetCapture(h);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (c) {
                int i = c->at(POINT{(short)LOWORD(l), (short)HIWORD(l)});
                int was = c->pressed;
                c->pressed = -1;
                ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
                if (i >= 0 && i == was) c->choose(i);
            }
            return 0;
        case WM_KEYDOWN:
            if (!c) break;
            if (w == VK_ESCAPE) c->done = true;
            else if (w == VK_RETURN || w == VK_SPACE) c->choose(c->focus);
            else if (w == VK_LEFT || w == VK_RIGHT || w == VK_TAB) {
                c->focus = 1 - c->focus;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_CLOSE:
            if (c) c->done = true;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) return TRUE;
            break;
    }
    return DefWindowProcW(h, msg, w, l);
}

using RenderFrame = std::function<std::pair<HBITMAP, std::wstring>(const std::atomic<bool>&)>;

bool runDialog(HWND owner, const std::wstring& title, const std::wstring& message, const std::wstring& confirm, const std::wstring& cancel,
               const Theme& theme, const Fonts& fonts, int dpi, int pictureW = 0, int pictureH = 0, const RenderFrame& render = {}) {
    static bool registered = false;
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = confirmProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kConfirmClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    Confirm c;
    c.title = &title;
    c.message = &message;
    c.labels[0] = &cancel;
    c.labels[1] = &confirm;
    c.theme = &theme;
    c.fonts = &fonts;
    c.dpi = dpi;
    c.preview = c.loading = (bool)render;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    RECT work = mi.rcWork;
    int pad = c.S(20), w = c.S(c.preview ? 920 : 440);
    w = std::min<int>(w, work.right - work.left);
    int imageW = 0, imageH = 0;
    if (c.preview) {
        double k = std::min((double)(w - 2 * pad - 2) / pictureW,
                            (double)std::max(fonts.lineH, std::min<int>(c.S(680), work.bottom - work.top - 8 * fonts.lineH)) / pictureH);
        imageW = std::max(1, (int)std::lround(pictureW * k)) + 2;
        imageH = std::max(1, (int)std::lround(pictureH * k)) + 2;
        w = std::max(c.S(510), imageW + 2 * pad);
        w = std::min<int>(w, work.right - work.left);
    }
    // Measure the wrapped message, then size the window around it.
    HDC dc = GetDC(owner);
    HGDIOBJ old = SelectObject(dc, fonts.normal);
    RECT mr{pad, 0, w - pad, 0};
    DrawTextW(dc, message.c_str(), (int)message.size(), &mr, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    int bw = 0;
    SelectObject(dc, fonts.bold);
    for (auto* s : {&confirm, &cancel}) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, s->c_str(), (int)s->size(), &sz);
        bw = std::max<int>(bw, sz.cx + c.S(32));
    }
    SelectObject(dc, old);
    ReleaseDC(owner, dc);
    int msgTop = pad + fonts.lineH + c.S(6);
    int msgH = mr.bottom - mr.top;
    c.msgRect = RECT{pad, msgTop, w - pad, msgTop + msgH};
    int by = msgTop + msgH + c.S(20);
    if (c.preview) {
        c.picture = RECT{(w - imageW) / 2, by, (w + imageW) / 2, by + imageH};
        by += imageH + c.S(16);
    }
    int bh = fonts.lineH;
    c.btn[1] = RECT{w - pad - bw, by, w - pad, by + bh};
    c.btn[0] = RECT{c.btn[1].left - c.S(8) - bw, by, c.btn[1].left - c.S(8), by + bh};
    int h = by + bh + pad;
    RECT orc;
    GetWindowRect(owner, &orc);
    int x = (orc.left + orc.right - w) / 2, y = (orc.top + orc.bottom - h) / 2;
    x = std::clamp<int>(x, work.left, std::max<int>(work.left, work.right - w));
    y = std::clamp<int>(y, work.top, std::max<int>(work.top, work.bottom - h));
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW, kConfirmClass, title.c_str(), WS_POPUP, x, y, w, h, owner, nullptr, inst, &c);
    if (!hw) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
    SetFocus(hw);
    std::atomic<bool> stop{false};
    std::pair<HBITMAP, std::wstring> frame;
    std::mutex frameMu;
    std::thread worker;
    auto renderNext = [&] {
        if (worker.joinable()) worker.join();
        c.loading = true;
        c.next = false;
        InvalidateRect(hw, nullptr, FALSE);
        worker = std::thread([&] {
            auto r = render(stop);
            {
                std::lock_guard<std::mutex> lk(frameMu);
                frame = std::move(r);
            }
            PostMessageW(hw, kFrameReady, 0, 0);
        });
    };
    if (render) renderNext();
    MSG m;
    while (!c.done) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) {
            if (!got) PostQuitMessage((int)m.wParam);
            break;
        }
        if (m.hwnd == hw && m.message == kFrameReady) {
            std::lock_guard<std::mutex> lk(frameMu);
            if (c.frame) DeleteObject(c.frame);
            c.frame = std::exchange(frame.first, nullptr);
            c.frameError = std::move(frame.second);
            c.loading = false;
            InvalidateRect(hw, nullptr, FALSE);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (c.next && !c.done) renderNext();
    }
    stop = true;
    DestroyWindow(hw);
    if (worker.joinable()) worker.join();
    if (frame.first) DeleteObject(frame.first);
    if (c.frame) DeleteObject(c.frame);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return c.result;
}

}  // namespace

bool runConfirm(HWND owner, const std::wstring& title, const std::wstring& message, const std::wstring& confirm, const std::wstring& cancel,
                const Theme& theme, const Fonts& fonts, int dpi) {
    return runDialog(owner, title, message, confirm, cancel, theme, fonts, dpi);
}

void runPreview(HWND owner, const std::wstring& caption, int w, int h, const RenderFrame& render, const Theme& theme, const Fonts& fonts, int dpi) {
    runDialog(owner, L"Conversion preview", caption, L"another frame", L"close", theme, fonts, dpi, w, h, render);
}

}  // namespace ea::ui
