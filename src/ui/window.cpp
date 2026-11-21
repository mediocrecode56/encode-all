#include "window.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <semaphore>

#include "../app/runners.h"
#include "../core/process.h"
#include "../core/util.h"
#include "../media/hwenc.h"
#include "../version.h"
#include "menu.h"

namespace ea::ui {

namespace {

const wchar_t* kClass = L"EncodeAllMain";
std::counting_semaphore<4> gAnalyzeSlots(4);

// OLE drop target: files go to Convert/Remux, links to Download.
class DropTarget : public IDropTarget {
public:
    explicit DropTarget(std::function<void(std::vector<std::wstring>, std::wstring)> fn) : fn_(std::move(fn)) {}
    virtual ~DropTarget() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *p = this;
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        ok_ = accepts(d);
        *eff = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override {
        *eff = ok_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        *eff = DROPEFFECT_NONE;
        std::vector<std::wstring> files;
        std::wstring text;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (SUCCEEDED(d->GetData(&fe, &sm))) {
            HDROP h = (HDROP)GlobalLock(sm.hGlobal);
            if (h) {
                UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; i++) {
                    UINT len = DragQueryFileW(h, i, nullptr, 0);
                    std::wstring f(len, L'\0');
                    DragQueryFileW(h, i, f.data(), len + 1);
                    files.push_back(f);
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
        } else {
            static CLIPFORMAT urlFmt = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocatorW");
            for (CLIPFORMAT cf : {urlFmt, (CLIPFORMAT)CF_UNICODETEXT}) {
                FORMATETC ft{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                if (SUCCEEDED(d->GetData(&ft, &sm))) {
                    const wchar_t* p = (const wchar_t*)GlobalLock(sm.hGlobal);
                    if (p) text = p;
                    GlobalUnlock(sm.hGlobal);
                    ReleaseStgMedium(&sm);
                    break;
                }
            }
        }
        if (!files.empty() || !text.empty()) {
            *eff = DROPEFFECT_COPY;
            fn_(files, text);
        }
        return S_OK;
    }

private:
    bool accepts(IDataObject* d) {
        static CLIPFORMAT urlFmt = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocatorW");
        for (CLIPFORMAT cf : {(CLIPFORMAT)CF_HDROP, urlFmt, (CLIPFORMAT)CF_UNICODETEXT}) {
            FORMATETC fe{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            if (d->QueryGetData(&fe) == S_OK) return true;
        }
        return false;
    }
    std::function<void(std::vector<std::wstring>, std::wstring)> fn_;
    ULONG ref_ = 1;
    bool ok_ = false;
};

WNDPROC gEditProc = nullptr;

std::vector<std::wstring> extractUrls(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        std::wstring t = trim(cur);
        cur.clear();
        while (!t.empty() && (t.back() == L',' || t.back() == L';' || t.back() == L')' || t.back() == L'>')) t.pop_back();
        while (!t.empty() && (t.front() == L'<' || t.front() == L'(' || t.front() == L'"')) t.erase(0, 1);
        if (t.empty()) return;
        std::wstring l = lower(t);
        if (startsWith(l, L"http://") || startsWith(l, L"https://")) out.push_back(t);
        else if (l.find(L'.') != std::wstring::npos && l.find(L' ') == std::wstring::npos && l.find(L'\\') == std::wstring::npos && l.size() > 4)
            out.push_back(L"https://" + t);
    };
    for (wchar_t c : text) {
        if (c == L'\n' || c == L'\r' || c == L' ' || c == L'\t') flush();
        else cur += c;
    }
    flush();
    std::vector<std::wstring> uniq;
    for (auto& u : out)
        if (std::find(uniq.begin(), uniq.end(), u) == uniq.end()) uniq.push_back(u);
    return uniq;
}

}  // namespace

// ---------- creation ----------

bool MainWindow::create(HINSTANCE inst, int show, const std::vector<std::wstring>& args) {
    inst_ = inst;
    loadSettings(settings_);
    tab_ = settings_.tab;
    presets_ = loadPresets();
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    POINT pt{settings_.winX, settings_.winY};
    HMONITOR mon = MonitorFromPoint(pt.x == INT32_MIN ? POINT{0, 0} : pt, MONITOR_DEFAULTTOPRIMARY);
    UINT mdx = 96, mdy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        if (auto f = (GetDpiForMonitorFn)(void*)GetProcAddress(sh, "GetDpiForMonitor")) f(mon, 0, &mdx, &mdy);
    int w = settings_.winW > 0 ? settings_.winW : MulDiv(1120, mdx, 96), h = settings_.winH > 0 ? settings_.winH : MulDiv(780, mdx, 96);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (settings_.winX != INT32_MIN) {
        RECT r{settings_.winX, settings_.winY, settings_.winX + w, settings_.winY + h};
        HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (m) {
            x = settings_.winX;
            y = settings_.winY;
        }
    }
    theme_.set(settings_.theme == 0 ? systemPrefersDark() : settings_.theme == 1);
    hwnd_ = CreateWindowExW(0, kClass, L"Encode All", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, settings_.winMax ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd_);
    openArgs(args);
    return true;
}

LRESULT CALLBACK MainWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self = (MainWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        self = (MainWindow*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    }
    if (self) return self->handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    HWND parent = GetParent(h);
    int id = GetDlgCtrlID(h);
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN && !(id == 100 + E_URLS && (GetKeyState(VK_SHIFT) & 0x8000))) {
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, 0x7001), (LPARAM)h);
            return 0;
        }
        if (w == VK_ESCAPE) {
            SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, 0x7002), (LPARAM)h);
            return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1 /* ctrl+a */) && !(id == 100 + E_URLS && w == VK_RETURN && (GetKeyState(VK_SHIFT) & 0x8000)))
        return 0;
    if (m == WM_PASTE && id == 100 + E_URLS) {
        LRESULT r = CallWindowProcW(gEditProc, h, m, w, l);
        PostMessageW(parent, WM_COMMAND, MAKEWPARAM(id, 0x7003), (LPARAM)h);
        return r;
    }
    if (m == WM_KILLFOCUS) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, 0x7004), (LPARAM)h);
    if (m == WM_CONTEXTMENU) {  // the app's own menu instead of the native edit menu
        SendMessageW(parent, WM_APP_EDITMENU, (WPARAM)h, l);
        return 0;
    }
    return CallWindowProcW(gEditProc, h, m, w, l);
}

void MainWindow::onCreate() {
    dpi_ = GetDpiForWindow(hwnd_);
    fonts_.create(dpi_);
    measureDc_ = CreateCompatibleDC(nullptr);
    applyTheme();
    for (int i = 0; i < E_COUNT; i++) {
        DWORD style = WS_CHILD | ES_AUTOHSCROLL;
        if (i == E_URLS) style = WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
        edits_[i] = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)(100 + i), inst_, nullptr);
        SendMessageW(edits_[i], WM_SETFONT, (WPARAM)(i == E_URLS ? fonts_.mono : fonts_.normal), FALSE);
        SendMessageW(edits_[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
        SendMessageW(edits_[i], EM_LIMITTEXT, i == E_URLS ? 1 << 20 : 512, 0);
        WNDPROC old = (WNDPROC)SetWindowLongPtrW(edits_[i], GWLP_WNDPROC, (LONG_PTR)editProc);
        if (!gEditProc) gEditProc = old;
    }
    RegisterDragDrop(hwnd_, new DropTarget([this](std::vector<std::wstring> files, std::wstring text) {
        if (!files.empty()) {
            addFiles(files, tab_ == 2 ? 2 : 1);
            return;
        }
        auto urls = extractUrls(text);
        if (urls.empty()) return;
        setTab(0);
        std::wstring cur;
        int n = GetWindowTextLengthW(edits_[E_URLS]);
        cur.resize(n);
        GetWindowTextW(edits_[E_URLS], cur.data(), n + 1);
        for (auto& u : urls) cur += (cur.empty() ? L"" : L"\r\n") + u;
        SetWindowTextW(edits_[E_URLS], cur.c_str());
        analyzeUrls();
    }));
    thumbs_.setSize(S(64), S(36));
    thumbs_.start([this] { PostMessageW(hwnd_, WM_APP_THUMB, 0, 0); });
    cleanTmp();
    // Tools, queue and runners.
    caps_ = EncoderCaps::fromJson(settings_.encoderCaps);
    deps_.init(dataDir(), [this] { PostMessageW(hwnd_, WM_APP_DEPS, 0, 0); });
    queue_.setRunner(TaskKind::Download, runDownload);
    queue_.setRunner(TaskKind::Convert, runConvert);
    queue_.setRunner(TaskKind::Remux, runRemux);
    queue_.init(&deps_, [this] { return settingsCopy(); },
                [this] {
                    std::lock_guard<std::mutex> lk(capsMu_);
                    return caps_;
                },
                [this](bool force) { onQueueNotify(force); });
    refreshTasks();
    deps_.startMaintenance(settings_.autoUpdateDeps, false);
    detectEncodersIfNeeded();
    if (settings_.autoUpdateApp && nowUnix() - settings_.lastAppCheck > 6 * 3600) {
        updateBusy_ = true;
        updateThread_ = std::thread([this] {
            for (int i = 0; i < 40 && !stop_; i++) Sleep(100);  // let the window settle first; quits promptly on close
            if (!stop_) updater_.checkAndStage(stop_);
            updateBusy_ = false;
            PostMessageW(hwnd_, WM_APP_UPDATE, 0, 0);
        });
        updateSetting([](Settings& s) { s.lastAppCheck = nowUnix(); });
    }
    SetTimer(hwnd_, T_HEALTHY, 15000, nullptr);
    // Apply the frameless WM_NCCALCSIZE now that the window exists.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    relayout(true);
}

void MainWindow::onDestroy() {
    stop_ = true;
    RevokeDragDrop(hwnd_);
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(hwnd_, &wp);
    updateSetting([&](Settings& s) {
        s.winX = wp.rcNormalPosition.left;
        s.winY = wp.rcNormalPosition.top;
        s.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        s.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        s.winMax = wp.showCmd == SW_SHOWMAXIMIZED;
        s.tab = tab_;
    });
    queue_.shutdown();
    deps_.shutdown();
    thumbs_.stop();
    KillTimer(hwnd_, T_ANIM);
    for (auto& w : workers_)
        if (w.t.joinable()) w.t.join();
    if (updateThread_.joinable()) updateThread_.join();
    if (backBmp_) DeleteObject(backBmp_);
    if (backDc_) DeleteDC(backDc_);
    if (measureDc_) DeleteDC(measureDc_);
    fonts_.destroy();
    PostQuitMessage(0);
}

void MainWindow::applyTheme() {
    bool dark = settings_.theme == 0 ? systemPrefersDark() : settings_.theme == 1;
    theme_.set(dark);
    applyDarkTitle(hwnd_, dark);
    allowDarkMenus(dark);
    for (HWND e : edits_)
        if (e) InvalidateRect(e, nullptr, TRUE);
}

void MainWindow::onDpi(int dpi, const RECT* r) {
    dpi_ = dpi;
    fonts_.create(dpi);
    for (HWND e : edits_) {
        SendMessageW(e, WM_SETFONT, (WPARAM)(e == edits_[E_URLS] ? fonts_.mono : fonts_.normal), FALSE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
    }
    thumbs_.setSize(S(64), S(36));
    if (r) SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    relayout(true);
}

void MainWindow::onSize() {
    GetClientRect(hwnd_, &client_);
    relayout(true);
}

// ---------- settings ----------

Settings MainWindow::settingsCopy() {
    std::lock_guard<std::mutex> lk(settingsMu_);
    return settings_;
}

void MainWindow::updateSetting(const std::function<void(Settings&)>& f) {
    {
        std::lock_guard<std::mutex> lk(settingsMu_);
        f(settings_);
    }
    // Debounced on the UI thread; workers save directly (SetTimer needs the window's thread).
    if (hwnd_ && GetWindowThreadProcessId(hwnd_, nullptr) == GetCurrentThreadId()) SetTimer(hwnd_, T_SAVE, 400, nullptr);
    else saveSettingsNow();
}

void MainWindow::saveSettingsNow() {
    Settings s = settingsCopy();
    saveSettings(s);
}

// ---------- items ----------

Item& MainWindow::add(int kind, const RECT& r, const std::wstring& t, unsigned style) {
    items_.emplace_back();
    Item& it = items_.back();
    it.kind = kind;
    it.r = r;
    it.text = t;
    it.style = style;
    if (clipping_ || bounded_) {
        RECT c = clipping_ ? clip_ : bound_;
        if (clipping_ && bounded_) IntersectRect(&c, &clip_, &bound_);
        it.clip = c;
        it.clipped = true;
    }
    return it;
}

HFONT MainWindow::fontFor(unsigned style) const {
    if (style & S_Big) return fonts_.big;
    if (style & S_Mono) return (style & S_Bold) ? fonts_.monoBold : fonts_.mono;
    return (style & S_Bold) ? fonts_.bold : fonts_.normal;
}

int MainWindow::tw(const std::wstring& s, unsigned style) {
    HGDIOBJ old = SelectObject(measureDc_, fontFor(style));
    SIZE sz{};
    GetTextExtentPoint32W(measureDc_, s.c_str(), (int)s.size(), &sz);
    SelectObject(measureDc_, old);
    return sz.cx;
}

int MainWindow::text(int x, int y, const std::wstring& s, unsigned style, int maxW) {
    int w = tw(s, style);
    if (maxW != 0 && w > maxW) w = std::max(0, maxW);  // 0 = no limit; a negative room draws nothing
    add(K_Text, RECT{x, y, x + w, y + rowH()}, s, style);
    return x + w;
}

// Controls are drawn as boxes inside the row, with a small vertical inset.
int MainWindow::button(int x, int y, const std::wstring& label, std::function<void()> fn, unsigned style) {
    int w = tw(label, style) + S(20);
    Item& it = add(K_Button, RECT{x, y + S(3), x + w, y + rowH() - S(3)}, label, style | (style & S_Disabled ? 0u : S_Click));
    if (!(style & S_Disabled)) it.click = std::move(fn);
    return x + w + S(8);
}

int MainWindow::toggle(int x, int y, const std::wstring& label, bool on, std::function<void()> fn) {
    int w = S(15) + S(7) + tw(label);
    Item& it = add(K_Check, RECT{x, y, x + w, y + rowH()}, label, S_Click);
    it.value = on ? 1 : 0;
    it.click = std::move(fn);
    return x + w + S(16);
}

int MainWindow::check(int x, int y, bool on, std::function<void()> fn) {
    Item& it = add(K_Check, RECT{x, y, x + S(22), y + rowH()}, L"", S_Click);
    it.value = on ? 1 : 0;
    it.click = std::move(fn);
    return x + S(22);
}

int MainWindow::choice(int x, int y, const std::wstring& label, const std::wstring& value, std::function<void()> open, std::function<void(int)> wheel,
                       int maxW) {
    if (!label.empty()) x = text(x, y, label, S_Mid) + S(8);
    int w = std::max<int>(tw(value) + S(9) + S(26), S(64));
    if (maxW > 0 && w > maxW) w = std::max<int>(maxW, S(64));
    Item& it = add(K_Box, RECT{x, y + S(3), x + w, y + rowH() - S(3)}, value, S_Click);
    it.click = std::move(open);
    it.wheel = std::move(wheel);
    return x + w + S(16);
}

int MainWindow::seg(int x, int y, const std::vector<std::wstring>& opts, int sel, std::function<void(int)> fn) {
    int x0 = x;
    for (size_t i = 0; i < opts.size(); i++) {
        int w = tw(opts[i]) + S(22);
        Item& it = add(K_Cell, RECT{x, y + S(3), x + w, y + rowH() - S(3)}, opts[i], ((int)i == sel ? S_Invert : 0u) | S_Click);
        it.click = [fn, i] { fn((int)i); };
        x += w;
        if (i + 1 < opts.size()) add(K_Line, RECT{x, y + S(3), x + 1, y + rowH() - S(3)});
    }
    add(K_Frame, RECT{x0, y + S(3), x, y + rowH() - S(3)});
    return x + S(16);
}

void MainWindow::hline(int x0, int x1, int y) { add(K_Line, RECT{x0, y, x1, y + 1}); }

void MainWindow::progressBar(const RECT& r, double v) {
    Item& it = add(K_Progress, r);
    it.value = v;
}

void MainWindow::region(const RECT& r, std::function<void(int)> wheel) {
    Item& it = add(K_Region, r);
    it.wheel = std::move(wheel);
}

void MainWindow::placeEdit(Edit e, const RECT& r) {
    editPlaced_[e] = true;
    editRects_[e] = r;
    add(K_Frame, RECT{r.left - 1, r.top - 1, r.right + 1, r.bottom + 1});
}

int MainWindow::popup(const std::vector<std::wstring>& opts, int checked) {
    return popup(opts, checked >= 0 ? std::vector<int>{checked} : std::vector<int>{});
}

int MainWindow::popup(const std::vector<std::wstring>& opts, const std::vector<int>& checked) {
    RECT a;
    if (anchorValid_) {
        a = anchor_;
        MapWindowPoints(hwnd_, nullptr, (POINT*)&a, 2);
    } else {
        POINT p;
        GetCursorPos(&p);
        a = RECT{p.x, p.y, p.x, p.y};
    }
    // The list replaces the hover state of the control that opened it.
    pressed_ = -1;
    int r = runMenu(hwnd_, a, opts, checked, theme_, fonts_, dpi_);
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(hwnd_, &p);
    mouse_ = p;
    int h = hitTest(p);
    if (h != hover_) {
        hover_ = h;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return r;
}

int MainWindow::iconButton(int x, int y, int icon, bool on, std::function<void()> fn) {
    int s = rowH() - S(4);
    Item& it = add(K_Icon, RECT{x - s, y + S(2), x, y + S(2) + s}, L"", S_Click | (on ? S_Invert : 0));
    it.value = icon;
    it.click = std::move(fn);
    return x - s;
}

int MainWindow::fieldLabel(int x, int y, const std::wstring& label, unsigned style) {
    add(K_Text, RECT{x, y, x + labelW() - S(8), y + rowH()}, label, style);
    return x + labelW();
}

int MainWindow::fieldW(const std::wstring& caption, const std::wstring& value) {
    int w = std::max<int>(tw(value) + S(35), S(64));
    if (!caption.empty()) w += tw(caption, S_Mid) + S(8);
    return w;
}

void MainWindow::field(int& x, int& y, int left, int right, const std::wstring& caption, const std::wstring& value, std::function<void()> open,
                       std::function<void(int)> wheel) {
    if (x > left && x + fieldW(caption, value) > right) {
        y += rowH();
        x = left;
    }
    x = choice(x, y, caption, value, std::move(open), std::move(wheel));
}

int MainWindow::sectionTitle(int x, int y, const std::wstring& title) { return text(x, y, title, S_Bold); }

void MainWindow::emptyState(const RECT& r, const std::wstring& l1, const std::wstring& l2) {
    int h = rowH() * (l2.empty() ? 1 : 2);
    int y = r.top + std::max<int>(0, (r.bottom - r.top - h) / 2);
    add(K_Text, RECT{r.left, y, r.right, y + rowH()}, l1, S_Center);
    if (!l2.empty()) add(K_Text, RECT{r.left, y + rowH() - S(4), r.right, y + rowH() * 2 - S(4)}, l2, S_Mid | S_Center);
}

void MainWindow::scrollbar(const RECT& v, int offset, int content) {
    int viewH = v.bottom - v.top;
    if (content <= viewH || viewH <= S(20)) return;
    int th = std::max<int>(S(18), (int)((long long)viewH * viewH / content));
    int ty = v.top + (int)((long long)(viewH - th) * offset / std::max(1, content - viewH));
    add(K_Bar, RECT{v.right - S(3), ty, v.right, ty + th});
}

void MainWindow::scrollBy(int& off, int delta, int maxOff) {
    off = std::clamp(off - delta, 0, std::max(0, maxOff));
    relayout();
}

// ---------- layout and paint ----------

void MainWindow::relayout(bool full) {
    if (!hwnd_ || !fonts_.normal) return;
    prev_.swap(items_);
    items_.clear();
    for (bool& p : editPlaced_) p = false;
    build();
    // Native edits follow the layout.
    HDWP dw = BeginDeferWindowPos(E_COUNT);
    for (int i = 0; i < E_COUNT; i++) {
        HWND e = edits_[i];
        if (!e) continue;
        bool vis = IsWindowVisible(e);
        if (editPlaced_[i]) {
            const RECT& r = editRects_[i];
            RECT cur;
            GetWindowRect(e, &cur);
            MapWindowPoints(nullptr, hwnd_, (POINT*)&cur, 2);
            if (!vis || !EqualRect(&cur, &r))
                dw = DeferWindowPos(dw, e, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else if (vis) {
            if (GetFocus() == e) SetFocus(hwnd_);
            dw = DeferWindowPos(dw, e, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
    }
    EndDeferWindowPos(dw);
    // Hover follows the mouse through structural changes.
    hover_ = hitTest(mouse_);
    if (full || prev_.size() != items_.size()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    for (size_t i = 0; i < items_.size(); i++) {
        const Item &a = items_[i], &b = prev_[i];
        if (!EqualRect(&a.r, &b.r) || a.text != b.text || a.style != b.style || a.value != b.value || a.bmp != b.bmp || a.kind != b.kind ||
            a.clipped != b.clipped || (a.clipped && !EqualRect(&a.clip, &b.clip))) {
            InvalidateRect(hwnd_, &a.r, FALSE);
            InvalidateRect(hwnd_, &b.r, FALSE);
        }
    }
}

void MainWindow::onPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    int w = client_.right, h = client_.bottom;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd_, &ps);
        return;
    }
    if (!backDc_ || backSize_.cx != w || backSize_.cy != h) {
        if (backBmp_) DeleteObject(backBmp_);
        if (!backDc_) backDc_ = CreateCompatibleDC(dc);
        backBmp_ = CreateCompatibleBitmap(dc, w, h);
        SelectObject(backDc_, backBmp_);
        backSize_ = {w, h};
    }
    HDC m = backDc_;
    SelectClipRgn(m, nullptr);
    IntersectClipRect(m, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
    Painter p(m, theme_, fonts_);
    p.fill(ps.rcPaint, theme_.bg);
    for (size_t i = 0; i < items_.size(); i++) {
        const Item& it = items_[i];
        RECT tmp;
        if (!IntersectRect(&tmp, &it.r, &ps.rcPaint)) continue;
        int saved = 0;
        if (it.clipped) {
            saved = SaveDC(m);
            IntersectClipRect(m, it.clip.left, it.clip.top, it.clip.right, it.clip.bottom);
        }
        bool hov = (int)i == hover_;
        bool press = (int)i == pressed_ && hov;
        COLORREF fg = (it.style & (S_Mid | S_Disabled)) ? theme_.mid : theme_.ink;
        HFONT f = fontFor(it.style);
        switch (it.kind) {
            case K_Text: {
                if ((it.style & S_Invert) || press) {
                    p.fill(it.r, theme_.ink);
                    fg = theme_.bg;
                } else if (it.style & S_Selected) {
                    p.fill(it.r, theme_.off);
                } else if (hov) {
                    p.fill(it.r, theme_.hover);
                }
                if (hov && (it.style & S_Click) && !(it.style & S_Invert)) fg = theme_.ink;
                UINT al = (it.style & S_Right) ? DT_RIGHT : (it.style & S_Center) ? DT_CENTER : DT_LEFT;
                p.text(it.r, it.text, fg, f, al);
                break;
            }
            case K_Button:
            case K_Cell: {
                bool filled = (it.style & S_Invert) || press;
                if (filled) p.fill(it.r, theme_.ink);
                else if (hov) p.fill(it.r, theme_.hover);
                if (it.kind == K_Button) p.frame(it.r, (it.style & S_Disabled) ? theme_.line : (hov || filled) ? theme_.ink : theme_.mid);
                COLORREF c = filled ? theme_.bg : (it.style & S_Disabled) ? theme_.mid : theme_.ink;
                p.text(it.r, it.text, c, f, DT_CENTER);
                break;
            }
            case K_Check: {
                int b = S(15);
                int top = it.r.top + (it.r.bottom - it.r.top - b) / 2;
                RECT box{it.r.left, top, it.r.left + b, top + b};
                if (hov) p.fill(box, theme_.hover);
                p.frame(box, hov ? theme_.ink : theme_.mid);
                if (it.value > 0) {
                    RECT in{box.left + S(3), box.top + S(3), box.right - S(3), box.bottom - S(3)};
                    p.fill(in, theme_.ink);
                }
                if (!it.text.empty()) p.text(RECT{box.right + S(7), it.r.top, it.r.right, it.r.bottom}, it.text, theme_.ink, f);
                break;
            }
            case K_Box: {
                if (hov || press) p.fill(it.r, theme_.hover);
                p.frame(it.r, hov ? theme_.ink : theme_.line);
                p.text(RECT{it.r.left + S(9), it.r.top, it.r.right - S(22), it.r.bottom}, it.text, theme_.ink, f);
                p.icon(I_Chevron, RECT{it.r.right - S(22), it.r.top, it.r.right - S(4), it.r.bottom}, hov ? theme_.ink : theme_.mid, S(12));
                break;
            }
            case K_Icon: {
                bool on = (it.style & S_Invert) || press;
                if (on) p.fill(it.r, theme_.ink);
                else if (hov) p.fill(it.r, theme_.hover);
                p.icon((int)it.value, it.r, on ? theme_.bg : theme_.ink, S(16));
                break;
            }
            case K_Bar: p.fill(it.r, theme_.line); break;
            case K_WinBtn: {
                // Weekly's chrome: inverted on hover, thin glyphs.
                COLORREF c = theme_.ink;
                if (hov || press) {
                    p.fill(it.r, theme_.ink);
                    c = theme_.bg;
                }
                const int glyph[] = {I_Min, I_Max, I_WinClose, I_Restore};
                p.icon(glyph[std::clamp((int)it.value, 0, 3)], it.r, c, S(10));
                break;
            }
            case K_Fill: p.fill(it.r, (it.style & S_Invert) ? theme_.ink : theme_.off); break;
            case K_Clear: p.fill(it.r, theme_.bg); break;
            case K_Line: p.fill(it.r, theme_.line); break;
            case K_Frame: p.frame(it.r, theme_.line); break;
            case K_Progress: {
                int h = std::min<int>(S(8), (int)(it.r.bottom - it.r.top));
                int top = it.r.top + (it.r.bottom - it.r.top - h) / 2;
                RECT track{it.r.left, top, it.r.right, top + h};
                p.fill(track, theme_.off);
                p.frame(track, theme_.line);
                if (it.value >= 0) {
                    RECT fill = track;
                    fill.right = track.left + (int)std::lround((track.right - track.left) * std::clamp(it.value, 0.0, 1.0));
                    if (fill.right > fill.left) p.fill(fill, theme_.ink);
                }
                break;
            }
            case K_Thumb:
                if (it.bmp) p.bitmap(it.bmp, it.r);
                else {
                    p.fill(it.r, theme_.off);
                    p.text(it.r, L"...", theme_.mid, fonts_.mono, DT_CENTER);
                }
                break;
            default: break;
        }
        if (saved) RestoreDC(m, saved);
    }
    BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, m, ps.rcPaint.left,
           ps.rcPaint.top, SRCCOPY);
    EndPaint(hwnd_, &ps);
}

// ---------- events ----------

// Active work first: running, paused, queued, failed, then finished tasks newest first.
void MainWindow::refreshTasks() {
    tasks_ = queue_.snapshot();
    auto rank = [](TaskState s) {
        switch (s) {
            case TaskState::Running: return 0;
            case TaskState::Paused: return 1;
            case TaskState::Waiting: return 2;
            case TaskState::Failed: return 3;
            default: return 4;
        }
    };
    std::stable_sort(tasks_.begin(), tasks_.end(), [&](const TaskView& a, const TaskView& b) {
        int ra = rank(a.state), rb = rank(b.state);
        if (ra != rb) return ra < rb;
        return ra == 4 ? a.id > b.id : a.id < b.id;
    });
}

int MainWindow::hitTest(POINT p) const {
    for (int i = (int)items_.size() - 1; i >= 0; i--) {
        const Item& it = items_[i];
        if ((it.style & S_Click) && PtInRect(&it.r, p) && (!it.clipped || PtInRect(&it.clip, p))) return i;
    }
    return -1;
}

bool MainWindow::editFocused() const {
    HWND f = GetFocus();
    for (HWND e : edits_)
        if (e && f == e) return true;
    return false;
}

void MainWindow::onQueueNotify(bool force) {
    (void)force;
    if (!queuePosted_.exchange(true)) PostMessageW(hwnd_, WM_APP_QUEUE, 0, 0);
}

bool MainWindow::preTranslate(MSG& m) {
    if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN) return false;
    if (m.hwnd != hwnd_ && GetParent(m.hwnd) != hwnd_) return false;
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    WPARAM k = m.wParam;
    if (ctrl && k >= '1' && k <= '3') {
        setTab((int)(k - '1'));
        return true;
    }
    if (ctrl && k == 'O') {
        pickFiles(tab_ == 2 ? 2 : 1);
        return true;
    }
    if (ctrl && k == 'L') {
        setOverlay(overlay_ == Overlay::Log ? Overlay::None : Overlay::Log);
        return true;
    }
    if (ctrl && k == VK_OEM_COMMA) {
        setOverlay(overlay_ == Overlay::Settings ? Overlay::None : Overlay::Settings);
        return true;
    }
    if (ctrl && k == VK_RETURN) {
        if (tab_ == 0) startDownloads();
        else if (tab_ == 1) startConvert();
        else startRemux();
        return true;
    }
    if (k == VK_F5 && selTask_) {
        queue_.retry(selTask_);
        return true;
    }
    if (k == VK_ESCAPE && overlay_ != Overlay::None && !editFocused()) {
        setOverlay(Overlay::None);
        return true;
    }
    if (editFocused()) return false;
    if (ctrl && k == 'V' && tab_ == 0) {
        SetFocus(edits_[E_URLS]);
        SendMessageW(edits_[E_URLS], WM_PASTE, 0, 0);
        return true;
    }
    if ((k == VK_UP || k == VK_DOWN) && !tasks_.empty()) {
        int i = 0;
        for (; i < (int)tasks_.size(); i++)
            if (tasks_[i].id == selTask_) break;
        i = std::clamp(i + (k == VK_UP ? -1 : 1), 0, (int)tasks_.size() - 1);
        selTask_ = tasks_[i].id;
        revealTask_ = true;
        relayout();
        return true;
    }
    if (k == VK_SPACE && selTask_) {
        for (auto& t : tasks_)
            if (t.id == selTask_) {
                if (t.state == TaskState::Paused) queue_.resume(t.id);
                else if (t.state == TaskState::Running || t.state == TaskState::Waiting) queue_.pause(t.id);
            }
        return true;
    }
    if (k == VK_DELETE && selTask_) {
        for (auto& t : tasks_)
            if (t.id == selTask_ && t.state != TaskState::Running && t.state != TaskState::Paused) queue_.remove(t.id);
        return true;
    }
    return false;
}

LRESULT MainWindow::handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: onCreate(); return 0;
        case WM_DESTROY: onDestroy(); return 0;
        case WM_CLOSE: {
            int running = queue_.running();
            if (running > 0) {
                std::wstring msg = running == 1 ? L"1 task is running. It comes back waiting on the next start: "
                                                : std::to_wstring(running) + L" tasks are running. They come back waiting on the next start: ";
                msg += L"downloads resume where they stopped, conversions start again.";
                if (!runConfirm(hwnd_, L"Quit Encode All?", msg, L"quit", L"keep working", theme_, fonts_, dpi_)) return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        }
        case WM_NCCALCSIZE:
            if (w) {
                // No caption or frame (Weekly-style). Maximised windows overhang the monitor by the frame, so trim it.
                if (IsZoomed(hwnd_)) {
                    auto* p = (NCCALCSIZE_PARAMS*)l;
                    int f = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi_) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                    p->rgrc[0].left += f;
                    p->rgrc[0].right -= f;
                    p->rgrc[0].top += f;
                    p->rgrc[0].bottom -= f;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd_, &pt);
            RECT r;
            GetClientRect(hwnd_, &r);
            if (!IsZoomed(hwnd_)) {
                int e = S(5);
                bool left = pt.x < e, right = pt.x >= r.right - e, top = pt.y < e, bottom = pt.y >= r.bottom - e;
                if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
                if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
            }
            if (pt.y < barH() && hitTest(pt) < 0) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: onPaint(); return 0;
        case WM_SIZE: onSize(); return 0;
        case WM_NCACTIVATE: return DefWindowProcW(hwnd_, m, w, -1);  // no default caption repaint over our bar
        case WM_GETMINMAXINFO: {
            auto mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize = {S(860), S(600)};
            return 0;
        }
        case WM_DPICHANGED: onDpi(HIWORD(w), (RECT*)l); return 0;
        case WM_SETTINGCHANGE:
            if (l && settings_.theme == 0 && wcscmp((const wchar_t*)l, L"ImmersiveColorSet") == 0) {
                applyTheme();
                relayout(true);
            }
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)w;
            SetTextColor(dc, theme_.ink);
            SetBkColor(dc, theme_.bg);
            return (LRESULT)theme_.bgBrush;
        }
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            mouse_ = p;
            int h = hitTest(p);
            if (h != hover_) {
                if (hover_ >= 0 && hover_ < (int)items_.size()) InvalidateRect(hwnd_, &items_[hover_].r, FALSE);
                if (h >= 0) InvalidateRect(hwnd_, &items_[h].r, FALSE);
                hover_ = h;
                SetCursor(LoadCursor(nullptr, h >= 0 ? IDC_HAND : IDC_ARROW));
            }
            TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&te);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && (HWND)w == hwnd_) {
                SetCursor(LoadCursor(nullptr, hover_ >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            mouse_ = {-1, -1};
            if (hover_ >= 0 && hover_ < (int)items_.size()) InvalidateRect(hwnd_, &items_[hover_].r, FALSE);
            hover_ = -1;
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            // Hit-test at the click itself: touch, pen and automation clicks come without a hover.
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            mouse_ = p;
            hover_ = hitTest(p);
            SetFocus(hwnd_);
            if (customDone_) commitCustom(false);
            pressed_ = hover_ < (int)items_.size() ? hover_ : -1;
            if (pressed_ >= 0) {
                InvalidateRect(hwnd_, &items_[pressed_].r, FALSE);
                SetCapture(hwnd_);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int p = pressed_;
            pressed_ = -1;
            ReleaseCapture();
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (p >= 0 && p < (int)items_.size() && hitTest(pt) == p) {
                auto fn = items_[p].click;
                InvalidateRect(hwnd_, &items_[p].r, FALSE);
                anchor_ = items_[p].r;
                anchorValid_ = true;
                if (fn) fn();
                anchorValid_ = false;
            }
            return 0;
        }
        case WM_RBUTTONUP:
            hover_ = hitTest(POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (hover_ >= 0 && hover_ < (int)items_.size()) {
                auto fn = items_[hover_].rclick;
                if (fn) fn();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd_, &p);
            int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
            if (!notches) notches = GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1;
            for (int i = (int)items_.size() - 1; i >= 0; i--) {
                const Item& it = items_[i];
                if (it.wheel && PtInRect(&it.r, p) && (!it.clipped || PtInRect(&it.clip, p))) {
                    auto fn = it.wheel;
                    fn(notches);
                    break;
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w) - 100;
            int code = HIWORD(w);
            if (id < 0 || id >= E_COUNT) break;
            if (code == 0x7001) {  // Enter
                if (id == E_URLS) analyzeUrls();
                else if (id == E_CUSTOM) commitCustom(true);
                else SetFocus(hwnd_);
            } else if (code == 0x7002) {  // Esc
                if (id == E_CUSTOM) commitCustom(false);
                else SetFocus(hwnd_);
            } else if (code == 0x7003) {  // pasted into the URL box
                analyzeUrls();
            } else if (code == 0x7004) {  // focus left
                if (id == E_CUSTOM && customDone_) commitCustom(true);
            } else if (code == EN_CHANGE) {
                int n = GetWindowTextLengthW(edits_[id]);
                std::wstring t(n, L'\0');
                GetWindowTextW(edits_[id], t.data(), n + 1);
                if ((id == E_SEGA || id == E_SEGB) && selGroup_ >= 0 && selGroup_ < (int)groups_.size()) {
                    auto& g = groups_[selGroup_];
                    int e = g.playlist ? selEntry_ : 0;
                    if (e >= 0 && e < (int)g.segA.size()) (id == E_SEGA ? g.segA : g.segB)[e] = trim(t);
                    relayout();
                } else if (id == E_RANGE && selGroup_ >= 0 && selGroup_ < (int)groups_.size()) {
                    auto& g = groups_[selGroup_];
                    std::vector<int> idx;
                    if (g.playlist && parseRanges(t, (int)g.entries.size(), idx)) {
                        std::fill(g.checked.begin(), g.checked.end(), 0);
                        for (int i : idx) g.checked[i - 1] = 1;
                        relayout();
                    }
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (w == T_QUEUE) {
                KillTimer(hwnd_, T_QUEUE);
                timerOn_ = false;
                queuePosted_ = false;
                refreshTasks();
                relayout();
            } else if (w == T_FLASH) {
                KillTimer(hwnd_, T_FLASH);
                flash_.clear();
                relayout();
            } else if (w == T_HEALTHY) {
                KillTimer(hwnd_, T_HEALTHY);
                AppUpdater::markHealthy();
            } else if (w == T_SAVE) {
                KillTimer(hwnd_, T_SAVE);
                saveSettingsNow();
            } else if (w == T_ANIM) {
                stepQueueAnim();
            }
            return 0;
        case WM_APP_QUEUE: {
            // At most ~8 refreshes a second however chatty the processes are.
            static ULONGLONG last = 0;
            ULONGLONG now = GetTickCount64();
            if (now - last < 120) {
                if (!timerOn_) {
                    SetTimer(hwnd_, T_QUEUE, (UINT)(120 - (now - last)), nullptr);
                    timerOn_ = true;
                }
                return 0;
            }
            last = now;
            queuePosted_ = false;
            refreshTasks();
            relayout();
            return 0;
        }
        case WM_APP_DEPS:
            detectEncodersIfNeeded();
            queue_.kick();
            relayout();
            return 0;
        case WM_APP_RESULT: applyResults(); return 0;
        case WM_APP_THUMB: relayout(); return 0;
        case WM_APP_UPDATE: relayout(); return 0;
        case WM_APP_CAPS: relayout(); return 0;
        case WM_APP_EDITMENU: editMenu((HWND)w, l); return 0;
        case WM_COPYDATA: {
            auto cd = (COPYDATASTRUCT*)l;
            if (cd->dwData == 0xEA01 && cd->lpData) {
                std::wstring s((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
                std::vector<std::wstring> args;
                for (auto& part : splitLines(replaceAll(s, std::wstring(1, L'\0'), L"\n"))) args.push_back(part);
                openArgs(args);
            }
            return TRUE;
        }
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ---------- actions ----------

void MainWindow::openArgs(const std::vector<std::wstring>& args) {
    std::vector<std::wstring> files;
    std::wstring urls;
    for (auto& a : args) {
        if (a.empty() || a[0] == L'-') continue;
        if (fileExists(a)) files.push_back(a);
        else urls += a + L"\r\n";
    }
    if (!files.empty()) addFiles(files, 1);
    if (!urls.empty()) {
        setTab(0);
        SetWindowTextW(edits_[E_URLS], urls.c_str());
        analyzeUrls();
    }
}

void MainWindow::setTab(int t) {
    tab_ = std::clamp(t, 0, 2);
    setOverlay(Overlay::None);
}

void MainWindow::setOverlay(Overlay o) {
    if (o != overlay_) {
        overlay_ = o;
        if (o == Overlay::History) {
            history_ = loadHistory(500);
            historyScroll_ = 0;
        }
        if (o == Overlay::Log) logScroll_ = 0;
        animateQueue();
    }
    relayout(true);
}

// The queue slides out of the way under Settings and back elsewhere: 160 ms, ease-out. The views
// take their final layout at once; only the queue block moves, so nothing else jumps or redraws twice.
void MainWindow::animateQueue() {
    double target = overlay_ == Overlay::Settings ? 0 : 1;
    if (target == animTo_ && (animOn_ || queueOpen_ == target)) return;
    animFrom_ = queueOpen_;
    animTo_ = target;
    animStart_ = GetTickCount64();
    if (!animOn_) {
        SetTimer(hwnd_, T_ANIM, 8, nullptr);
        animOn_ = true;
    }
}

void MainWindow::stepQueueAnim() {
    const double dur = 160;
    double t = std::min(1.0, (GetTickCount64() - animStart_) / dur);
    double e = 1 - (1 - t) * (1 - t) * (1 - t);
    queueOpen_ = animFrom_ + (animTo_ - animFrom_) * e;
    if (t >= 1) {
        queueOpen_ = animTo_;
        KillTimer(hwnd_, T_ANIM);
        animOn_ = false;
    }
    relayout();
    UpdateWindow(hwnd_);  // paint each frame now
}

void MainWindow::flash(const std::wstring& msg, UINT ms) {
    flash_ = msg;
    SetTimer(hwnd_, T_FLASH, ms, nullptr);
    relayout();
}

void MainWindow::removeGroup(int gi) {
    if (gi < 0 || gi >= (int)groups_.size()) return;
    groups_.erase(groups_.begin() + gi);
    if (selGroup_ == gi) {
        selGroup_ = groups_.empty() ? -1 : std::min(gi, (int)groups_.size() - 1);
        selEntry_ = 0;
    } else if (selGroup_ > gi) {
        selGroup_--;
    }
}

void MainWindow::removeFile(int tab, int i) {
    auto& list = tab == 2 ? remux_ : conv_;
    int& sel = tab == 2 ? selRemux_ : selConv_;
    if (i < 0 || i >= (int)list.size()) return;
    list.erase(list.begin() + i);
    if (sel > i || sel >= (int)list.size()) sel--;
    if (sel < 0 && !list.empty()) sel = 0;
    detailScroll_ = 0;
    relayout(true);
}

void MainWindow::spawn(std::function<void()> fn) {
    reapWorkers();
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([fn = std::move(fn), done] {
        fn();
        *done = true;
    });
    workers_.push_back({std::move(t), done});
}

void MainWindow::reapWorkers() {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (*it->done) {
            it->t.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void MainWindow::editMenu(HWND e, LPARAM at) {
    POINT p{GET_X_LPARAM(at), GET_Y_LPARAM(at)};
    if (p.x == -1 && p.y == -1) {  // from the keyboard: under the box
        RECT r;
        GetWindowRect(e, &r);
        p = {r.left + S(8), r.bottom};
    }
    DWORD a = 0, b = 0;
    SendMessageW(e, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    SetFocus(e);
    std::vector<std::wstring> items = {L"cut", L"copy", L"paste", L"-", L"select all"};
    int r = runMenu(hwnd_, RECT{p.x, p.y, p.x, p.y}, items, -1, theme_, fonts_, dpi_);
    if (r == 0 && a != b) SendMessageW(e, WM_CUT, 0, 0);
    else if (r == 1 && a != b) SendMessageW(e, WM_COPY, 0, 0);
    else if (r == 2) SendMessageW(e, WM_PASTE, 0, 0);
    else if (r == 4) SendMessageW(e, EM_SETSEL, 0, -1);
}

void MainWindow::copyText(const std::wstring& t) {
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}

void MainWindow::openFolder(const std::wstring& path) {
    if (fileExists(path)) {
        std::wstring args = L"/select," + quoteArg(path);
        ShellExecuteW(hwnd_, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (dirExists(path)) {
        ShellExecuteW(hwnd_, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (dirExists(dirName(path))) {
        ShellExecuteW(hwnd_, nullptr, dirName(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

std::wstring MainWindow::pickFolder(const std::wstring& initial) {
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD o;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IShellItem* init = nullptr;
    if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
        d->SetFolder(init);
        init->Release();
    }
    if (SUCCEEDED(d->Show(hwnd_))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = p;
            CoTaskMemFree(p);
            it->Release();
        }
    }
    d->Release();
    return out;
}

void MainWindow::pickFiles(int tab) {
    auto files = openFiles(L"Media files", L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.mts;*.m2ts;*.ts;*.mxf;*.wmv;*.flv;*.mpg;*.mpeg;*.vob;*.3gp;*.ogv;"
                                           L"*.mp3;*.m4a;*.aac;*.flac;*.wav;*.opus;*.ogg;*.wma;*.aiff;*.aif;*.ac3;*.eac3;*.dts;*.mka;*.alac;*.ape;*.wv",
                           true);
    if (!files.empty()) addFiles(files, tab);
}

// The shell's file dialog (the one system dialog the app keeps: it is the file system, not a control).
std::vector<std::wstring> MainWindow::openFiles(const wchar_t* typeName, const wchar_t* spec, bool multi) {
    std::vector<std::wstring> files;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return files;
    DWORD o;
    d->GetOptions(&o);
    d->SetOptions(o | (multi ? FOS_ALLOWMULTISELECT : 0) | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    COMDLG_FILTERSPEC f[] = {{typeName, spec}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, f);
    if (SUCCEEDED(d->Show(hwnd_))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(d->GetResults(&arr))) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it))) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) files.push_back(p);
                    CoTaskMemFree(p);
                    it->Release();
                }
            }
            arr->Release();
        }
    }
    d->Release();
    return files;
}

void MainWindow::addFiles(const std::vector<std::wstring>& in, int tab) {
    std::vector<std::wstring> files;
    for (auto& f : in) {
        if (dirExists(f)) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(joinPath(f, L"*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(joinPath(f, fd.cFileName));
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        } else if (fileExists(f)) {
            files.push_back(f);
        }
    }
    tab_ = tab;
    overlay_ = Overlay::None;
    animateQueue();
    auto& list = tab == 2 ? remux_ : conv_;
    for (auto& f : files) {
        bool dup = false;
        for (auto& r : list) dup = dup || lower(r.path) == lower(f);
        if (dup) continue;
        FileRow row;
        row.key = nextKey_++;
        row.path = f;
        list.push_back(row);
        uint64_t key = row.key;
        spawn([this, key, f] {
            WorkResult r;
            r.type = 2;
            r.key = key;
            if (!deps_.waitReady(Dep::FFmpeg, stop_)) return;
            if (!probeFile(deps_.ffprobe(), f, r.media, &stop_)) r.error = r.media.error;
            {
                std::lock_guard<std::mutex> lk(resMu_);
                results_.push_back(std::move(r));
            }
            PostMessageW(hwnd_, WM_APP_RESULT, 0, 0);
        });
    }
    if (tab == 2 && selRemux_ < 0 && !remux_.empty()) selRemux_ = 0;
    if (tab == 1 && selConv_ < 0 && !conv_.empty()) selConv_ = 0;
    relayout(true);
}

void MainWindow::analyzeUrls() {
    int n = GetWindowTextLengthW(edits_[E_URLS]);
    std::wstring t(n, L'\0');
    GetWindowTextW(edits_[E_URLS], t.data(), n + 1);
    auto urls = extractUrls(t);
    if (urls.empty()) return;
    SetWindowTextW(edits_[E_URLS], L"");
    Settings s = settingsCopy();
    for (auto& u : urls) {
        bool dup = false;
        for (auto& g : groups_) dup = dup || g.url == u;
        if (dup) continue;
        DlGroup g;
        g.key = nextKey_++;
        g.url = u;
        g.title = u;
        groups_.push_back(g);
        uint64_t key = g.key;
        spawn([this, key, u, s] {
            WorkResult r;
            r.type = 1;
            r.key = key;
            if (!deps_.waitReady(Dep::YtDlp, stop_) || !deps_.waitReady(Dep::Deno, stop_)) return;
            gAnalyzeSlots.acquire();
            DlOptions auth;
            auth.cookiesBrowser = s.cookiesBrowser;
            auth.cookiesFile = s.cookiesFile;
            ProcOpts po;
            po.env = {{L"PYTHONIOENCODING", L"utf-8"}, {L"PYTHONUTF8", L"1"}};
            deps_.acquire(Dep::YtDlp);
            auto c = runCapture(deps_.ytdlp(), buildInfoArgs(u, true, deps_.deno(), &auth), po, 180000, &stop_);
            deps_.release(Dep::YtDlp);
            gAnalyzeSlots.release();
            if (stop_) return;
            if (!c.started) {
                r.error = c.startError;
            } else if (c.code != 0) {
                std::wstring msg, hint;
                classifyError(c.err, msg, hint);
                r.error = msg.empty() ? L"yt-dlp could not read this link" : msg;
                r.hint = hint;
            } else {
                r.yt = parseYtJson(c.out);
                if (!r.yt.ok) r.error = r.yt.error;
                else if (!r.yt.playlist) {
                    r.infoFile = joinPath(tmpDir(), L"info-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(key) + L".json");
                    writeFileAtomic(r.infoFile, c.out);
                }
            }
            {
                std::lock_guard<std::mutex> lk(resMu_);
                results_.push_back(std::move(r));
            }
            PostMessageW(hwnd_, WM_APP_RESULT, 0, 0);
        });
    }
    tab_ = 0;
    overlay_ = Overlay::None;
    animateQueue();
    relayout(true);
}

void MainWindow::applyResults() {
    std::vector<WorkResult> rs;
    {
        std::lock_guard<std::mutex> lk(resMu_);
        rs.swap(results_);
    }
    for (auto& r : rs) {
        if (r.type == 1) {
            for (auto& g : groups_) {
                if (g.key != r.key) continue;
                g.busy = false;
                g.error = r.error;
                g.hint = r.hint;
                if (!r.error.empty()) break;
                g.playlist = r.yt.playlist;
                if (g.playlist) {
                    g.title = widen(r.yt.playlistTitle);
                    g.entries = r.yt.entries;
                    g.checked.assign(g.entries.size(), 1);
                    g.segA.assign(g.entries.size(), L"");
                    g.segB.assign(g.entries.size(), L"");
                } else {
                    g.info = r.yt.info;
                    g.title = widen(g.info.title);
                    g.infoFile = r.infoFile;
                    g.checked.assign(1, 1);
                    g.segA.assign(1, L"");
                    g.segB.assign(1, L"");
                }
                if (selGroup_ < 0) {
                    selGroup_ = (int)(&g - groups_.data());
                    selEntry_ = 0;
                }
                break;
            }
        } else if (r.type == 2) {
            for (auto* list : {&conv_, &remux_})
                for (auto& f : *list) {
                    if (f.key != r.key) continue;
                    f.busy = false;
                    f.error = r.error;
                    f.info = r.media;
                    if (!r.error.empty()) f.checked = false;
                }
        }
    }
    relayout(true);
}

void MainWindow::startDownloads() {
    Settings s = settingsCopy();
    int n = 0;
    std::vector<DlGroup> keep;
    for (auto& g : groups_) {
        if (g.busy || !g.error.empty()) {
            keep.push_back(g);
            continue;
        }
        if (g.playlist) {
            for (size_t i = 0; i < g.entries.size(); i++) {
                if (!g.checked[i]) continue;
                auto& e = g.entries[i];
                queue_.add(TaskKind::Download, widen(e.title), makeDownloadSpec(s, widen(e.url), L"", g.segA[i], g.segB[i]));
                n++;
            }
        } else if (!g.checked.empty() && g.checked[0]) {
            queue_.add(TaskKind::Download, g.title, makeDownloadSpec(s, g.url, g.infoFile, g.segA[0], g.segB[0]));
            n++;
        }
    }
    if (n == 0) return;
    groups_ = keep;
    selGroup_ = groups_.empty() ? -1 : 0;
    selEntry_ = 0;
    dlScroll_ = 0;
    flash(std::to_wstring(n) + (n == 1 ? L" download queued" : L" downloads queued"));
    relayout(true);
}

void MainWindow::startConvert() {
    Settings s = settingsCopy();
    int n = 0;
    std::vector<FileRow> keep;
    for (auto& f : conv_) {
        if (f.busy || !f.info.ok || !f.checked) {
            keep.push_back(f);
            continue;
        }
        json::Value spec = json::Value::object();
        spec.set("input", f.path);
        spec.set("outDir", s.convertDir);
        spec.set("convert", s.convert.toJson());
        queue_.add(TaskKind::Convert, fileName(f.path), spec);
        n++;
    }
    if (!n) return;
    conv_ = keep;
    selConv_ = conv_.empty() ? -1 : 0;
    flash(std::to_wstring(n) + (n == 1 ? L" conversion queued" : L" conversions queued"));
    relayout(true);
}

void MainWindow::startRemux() {
    Settings s = settingsCopy();
    int n = 0;
    std::vector<FileRow> keep;
    for (auto& f : remux_) {
        if (f.busy || !f.info.ok || !f.checked) {
            keep.push_back(f);
            continue;
        }
        RemuxSpec rs = f.remux;
        rs.container = s.remuxContainer;
        rs.audioPcm = s.remuxPcm;
        rs.pcmBits = s.remuxPcmBits;
        if (!planRemux(f.info, rs).ok) {
            keep.push_back(f);
            continue;
        }
        json::Value spec = json::Value::object();
        spec.set("input", f.path);
        spec.set("outDir", s.remuxDir);
        spec.set("remux", rs.toJson());
        queue_.add(TaskKind::Remux, fileName(f.path), spec);
        n++;
    }
    if (!n) return;
    remux_ = keep;
    selRemux_ = remux_.empty() ? -1 : 0;
    flash(std::to_wstring(n) + (n == 1 ? L" remux queued" : L" remuxes queued"));
    relayout(true);
}

void MainWindow::customValue(const std::wstring& initial, const RECT& at, std::function<void(const std::wstring&)> done) {
    customDone_ = std::move(done);
    RECT r = at;
    if (r.right - r.left < S(160)) r.right = r.left + S(160);
    editRects_[E_CUSTOM] = r;
    SetWindowTextW(edits_[E_CUSTOM], initial.c_str());
    relayout();
    SetFocus(edits_[E_CUSTOM]);
    SendMessageW(edits_[E_CUSTOM], EM_SETSEL, 0, -1);
}

void MainWindow::commitCustom(bool ok) {
    auto fn = std::move(customDone_);
    customDone_ = nullptr;
    if (!fn) return;
    int n = GetWindowTextLengthW(edits_[E_CUSTOM]);
    std::wstring t(n, L'\0');
    GetWindowTextW(edits_[E_CUSTOM], t.data(), n + 1);
    if (GetFocus() == edits_[E_CUSTOM]) SetFocus(hwnd_);
    if (ok) fn(trim(t));
    relayout(true);
}

void MainWindow::detectEncodersIfNeeded() {
    if (capsRunning_ || !deps_.ready(Dep::FFmpeg)) return;
    std::string hash = deps_.ffmpegHash();
    {
        std::lock_guard<std::mutex> lk(capsMu_);
        if (caps_.probed && !hash.empty() && settings_.encoderCapsFor == hash) return;
    }
    capsRunning_ = true;
    spawn([this, hash] {
        deps_.acquire(Dep::FFmpeg);
        EncoderCaps c = detectEncoders(deps_.ffmpeg(), &stop_);
        deps_.release(Dep::FFmpeg);
        if (stop_) return;
        {
            std::lock_guard<std::mutex> lk(capsMu_);
            caps_ = c;
        }
        updateSetting([&](Settings& s) {
            s.encoderCaps = c.toJson();
            s.encoderCapsFor = hash;
        });
        capsRunning_ = false;
        PostMessageW(hwnd_, WM_APP_CAPS, 0, 0);
    });
}

}  // namespace ea::ui
