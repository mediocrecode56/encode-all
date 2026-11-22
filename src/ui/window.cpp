#include "window.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
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
std::counting_semaphore<4> gProbeSlots(4);  // a dropped folder probes four files at a time, not all at once

// What a dropped folder contributes: media files only (a single file is always taken and probed).
bool mediaExtension(const std::wstring& path) {
    static const wchar_t* ext[] = {L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".wmv", L".asf", L".flv", L".ts", L".m2ts", L".mts", L".mpg",
                                   L".mpeg", L".vob", L".3gp", L".ogv", L".mxf", L".dv", L".y4m", L".m4a", L".mp3", L".flac", L".wav", L".aac", L".ac3",
                                   L".eac3", L".dts", L".opus", L".ogg", L".oga", L".wma", L".aiff", L".aif", L".alac", L".ape", L".wv", L".mka", L".thd",
                                   L".caf", L".w64", L".rf64"};
    std::wstring e = lower(extension(path));
    for (auto x : ext)
        if (e == x) return true;
    return false;
}

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
    int w = settings_.winW > 0 ? settings_.winW : MulDiv(900, mdx, 96), h = settings_.winH > 0 ? settings_.winH : MulDiv(640, mdx, 96);
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
        if (w == VK_RETURN) {
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
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == 1 /* ctrl+a */)) return 0;
    // Links pasted into the link box are read at once (all of them, one per line or not); other text pastes normally.
    if (m == WM_PASTE && id == 100 + E_URLS && SendMessageW(parent, WM_APP_PASTE, 0, 0)) return 0;
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
        edits_[i] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)(100 + i), inst_, nullptr);
        SendMessageW(edits_[i], WM_SETFONT, (WPARAM)fonts_.normal, FALSE);
        SendMessageW(edits_[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
        SendMessageW(edits_[i], EM_LIMITTEXT, i == E_URLS ? 1 << 16 : 512, 0);
        WNDPROC old = (WNDPROC)SetWindowLongPtrW(edits_[i], GWLP_WNDPROC, (LONG_PTR)editProc);
        if (!gEditProc) gEditProc = old;
    }
    SendMessageW(edits_[E_URLS], EM_SETCUEBANNER, TRUE, (LPARAM)L"Paste links, or type one and press Enter");
    RegisterDragDrop(hwnd_, new DropTarget([this](std::vector<std::wstring> files, std::wstring text) {
        if (!files.empty()) {
            addFiles(files, tab_ == 2 ? 2 : 1);
            return;
        }
        analyzeText(text);
    }));
    thumbs_.setSize(S(80), S(45));
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
        SendMessageW(e, WM_SETFONT, (WPARAM)fonts_.normal, FALSE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
    }
    thumbs_.setSize(S(80), S(45));
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
    if (clipping_) {
        it.clip = clip_;
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

int MainWindow::iconButtonText(int right, int y, const std::wstring& label, int icon, std::function<void()> fn, unsigned style) {
    int w = S(14) + tw(label, style) + S(10) + iconW() + S(10);
    Item& it = add(K_Button, RECT{right - w, y + S(3), right, y + rowH() - S(3)}, label, style | (style & S_Disabled ? 0u : S_Click));
    it.value = icon + 1;
    if (!(style & S_Disabled)) it.click = std::move(fn);
    return right - w;
}

int MainWindow::toggle(int x, int y, const std::wstring& label, bool on, std::function<void()> fn) {
    int w = checkBox() + S(7) + tw(label);
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
                       int maxW, unsigned style) {
    bool off = style & S_Disabled;
    if (!label.empty()) x = text(x, y, label, S_Mid) + S(8);
    int w = std::max<int>(tw(value) + S(9) + S(26), S(64));
    if (maxW > 0 && w > maxW) w = std::max<int>(maxW, S(64));
    Item& it = add(K_Box, RECT{x, y + S(3), x + w, y + rowH() - S(3)}, value, off ? S_Disabled : S_Click);
    if (!off) {
        it.click = std::move(open);
        it.wheel = std::move(wheel);
    }
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

void MainWindow::region(const RECT& r, std::function<void(int)> wheel) {
    Item& it = add(K_Region, r);
    it.wheel = std::move(wheel);
}

void MainWindow::placeEdit(Edit e, const RECT& r, bool frame) {
    editPlaced_[e] = true;
    editRects_[e] = r;
    if (frame) add(K_Frame, RECT{r.left - 1, r.top - 1, r.right + 1, r.bottom + 1});
}

int MainWindow::rowIcon(int right, int top, int size, int icon, const std::wstring& tip, std::function<void()> fn) {
    Item& it = add(K_Icon, RECT{right - size, top, right, top + size}, L"", fn ? S_Click : S_Disabled);
    it.value = icon;
    it.tip = tip;
    it.click = std::move(fn);
    return right - size;
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
                       std::function<void(int)> wheel, unsigned style) {
    if (x > left && x + fieldW(caption, value) > right) {
        y += rowH();
        x = left;
    }
    x = choice(x, y, caption, value, std::move(open), std::move(wheel), 0, style);
}

void MainWindow::rowNote(int x, int y, int right, const std::wstring& note) {
    if (!note.empty() && right - x > S(140)) text(x, y, note, S_Mid, right - x);
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

// Icon buttons name themselves in the header's hint line; only that line repaints.
void MainWindow::setHint(const std::wstring& tip) {
    if (tip == hoverTip_) return;
    hoverTip_ = tip;
    if (hintIdx_ >= items_.size()) return;
    items_[hintIdx_].text = tip.empty() ? hintDefault_ : tip;
    InvalidateRect(hwnd_, &items_[hintIdx_].r, FALSE);
}

void MainWindow::scrollBy(int& off, int delta, int maxOff) {
    off = std::clamp(off - delta, 0, std::max(0, maxOff));
    relayout();
}

// ---------- layout and paint ----------

void MainWindow::relayout(bool full) {
    // Never nested: setting an edit's text during a build sends EN_CHANGE, whose handler lays out again.
    if (!hwnd_ || !fonts_.normal || building_) return;
    prev_.swap(items_);
    items_.clear();
    hintIdx_ = SIZE_MAX;
    for (bool& p : editPlaced_) p = false;
    building_ = true;
    build();
    building_ = false;
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
    // Hover follows the mouse through structural changes, and so does the hint it shows.
    hover_ = hitTest(mouse_);
    hoverTip_ = hover_ >= 0 ? items_[hover_].tip : L"";
    if (hintIdx_ < items_.size()) items_[hintIdx_].text = hoverTip_.empty() ? hintDefault_ : hoverTip_;
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
                if (it.kind == K_Button && it.value > 0) {  // value: icon + 1, drawn right of the label
                    int iw = iconW();
                    p.text(RECT{it.r.left + S(14), it.r.top, it.r.right - iw - S(4), it.r.bottom}, it.text, c, f, DT_LEFT);
                    p.icon((int)it.value - 1, RECT{it.r.right - iw - S(10), it.r.top, it.r.right - S(10), it.r.bottom}, c, iw);
                } else {
                    p.text(it.r, it.text, c, f, DT_CENTER);
                }
                break;
            }
            case K_Check: {
                int b = checkBox();
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
                bool off = it.style & S_Disabled;
                bool lit = (hov || press) && !off;
                if (lit) p.fill(it.r, theme_.hover);
                p.frame(it.r, lit ? theme_.ink : theme_.line);
                p.text(RECT{it.r.left + S(9), it.r.top, it.r.right - S(22), it.r.bottom}, it.text, off ? theme_.mid : theme_.ink, fontFor(it.style & ~S_Disabled));
                p.icon(I_Chevron, RECT{it.r.right - S(22), it.r.top, it.r.right - S(4), it.r.bottom}, lit ? theme_.ink : off ? theme_.line : theme_.mid, S(14));
                break;
            }
            case K_Icon: {
                bool on = (it.style & S_Invert) || press;
                if (on) p.fill(it.r, theme_.ink);
                else if (hov) p.fill(it.r, theme_.hover);
                COLORREF c = on ? theme_.bg : (it.style & S_Disabled) ? theme_.line : (it.style & S_Mid) && !hov ? theme_.mid : theme_.ink;
                p.icon((int)it.value, it.r, c, std::min<int>(S(23), (int)(it.r.bottom - it.r.top) - S(8)));
                break;
            }
            case K_Chip: {
                p.fill(it.r, theme_.off);
                p.frame(it.r, theme_.line);
                p.text(it.r, it.text, theme_.mid, fonts_.monoBold, DT_CENTER);
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
                p.icon(glyph[std::clamp((int)it.value, 0, 3)], it.r, c, S(13));
                break;
            }
            case K_Fill: p.fill(it.r, (it.style & S_Invert) ? theme_.ink : theme_.off); break;
            case K_Line: p.fill(it.r, theme_.line); break;
            case K_Frame: p.frame(it.r, theme_.line); break;
            case K_Progress: {
                if (it.style & S_Mid) {  // a list row's hairline progress: no track frame
                    p.fill(it.r, theme_.line);
                    RECT fill = it.r;
                    fill.right = it.value >= 0 ? it.r.left + (int)std::lround((it.r.right - it.r.left) * std::clamp(it.value, 0.0, 1.0)) : it.r.left;
                    if (fill.right > fill.left) p.fill(fill, theme_.ink);
                    break;
                }
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

void MainWindow::refreshTasks() {
    tasks_ = queue_.snapshot();
    bool found = !selTask_;
    for (auto& t : tasks_) found = found || t.id == selTask_;
    if (!found) selTask_ = 0;
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
        startAll(tab_);
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
    if (overlay_ != Overlay::None) return false;
    // The list: arrows select, Space starts, pauses or resumes, Delete removes (or cancels a running task).
    if ((k == VK_UP || k == VK_DOWN) && !rowIds_.empty()) {
        int i = 0;
        for (; i < (int)rowIds_.size(); i++)
            if (rowIds_[i] == sel_[tab_]) break;
        if (i == (int)rowIds_.size()) i = k == VK_UP ? (int)rowIds_.size() : -1;
        i = std::clamp(i + (k == VK_UP ? -1 : 1), 0, (int)rowIds_.size() - 1);
        select(tab_, rowIds_[i]);
        reveal_ = true;
        relayout();
        return true;
    }
    if ((k == VK_SPACE || k == VK_DELETE) && !sel_[tab_].empty()) {
        for (auto& r : collectRows(tab_)) {
            if (!(r.id == sel_[tab_])) continue;
            if (k == VK_DELETE) {
                removeRow(r);
            } else if (r.type == ListRow::Task) {
                if (r.task->state == TaskState::Paused) queue_.resume(r.task->id);
                else if (r.task->state == TaskState::Running || r.task->state == TaskState::Waiting) queue_.pause(r.task->id);
                else if (r.task->state == TaskState::Failed || r.task->state == TaskState::Canceled) queue_.retry(r.task->id);
            } else {
                startRow(r);
            }
            break;
        }
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
            mm->ptMinTrackSize = {S(940), S(640)};
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
                setHint(h >= 0 ? items_[h].tip : L"");
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
            setHint(L"");
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
            } else if (code == 0x7004) {  // focus left
                if (id == E_CUSTOM && customDone_) commitCustom(true);
            } else if (code == EN_CHANGE && (id == E_SEGA || id == E_SEGB || id == E_RANGE)) {
                int n = GetWindowTextLengthW(edits_[id]);
                std::wstring t(n, L'\0');
                GetWindowTextW(edits_[id], t.data(), n + 1);
                uint64_t key = id == E_RANGE ? rangeKey_ : sel_[0].key;
                for (auto& g : groups_) {
                    if (g.key != key) continue;
                    if (id == E_RANGE) {
                        applyRanges(g, t);
                    } else {
                        int sub = sel_[0].sub;
                        for (size_t e = 0; e < g.segA.size(); e++)
                            if ((g.playlist ? g.entries[e].index : 0) == sub) (id == E_SEGA ? g.segA : g.segB)[e] = trim(t);
                    }
                }
                relayout();
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
        case WM_APP_CAPS: convertPlans_.clear(); detectEncodersIfNeeded(); relayout(); return 0;
        case WM_APP_EDITMENU: editMenu((HWND)w, l); return 0;
        case WM_APP_PASTE: return pasteLinks() ? 1 : 0;
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
    if (!urls.empty()) analyzeText(urls);
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
    }
    relayout(true);
}

void MainWindow::flash(const std::wstring& msg, UINT ms) {
    flash_ = msg;
    SetTimer(hwnd_, T_FLASH, ms, nullptr);
    relayout();
}

void MainWindow::select(int tab, const RowId& id) {
    sel_[tab] = id;
    selTask_ = id.task;
    if (tab != 0 || !id.key) return;
    // The segment boxes edit the selected link (or playlist entry).
    for (auto& g : groups_) {
        if (g.key != id.key) continue;
        for (size_t e = 0; e < g.segA.size(); e++)
            if ((g.playlist ? g.entries[e].index : 0) == id.sub) {
                SetWindowTextW(edits_[E_SEGA], g.segA[e].c_str());
                SetWindowTextW(edits_[E_SEGB], g.segB[e].c_str());
            }
    }
}

int64_t MainWindow::newOrder() {
    // Milliseconds since 1970 (exact in a JSON number), never repeated, so newer items sort first.
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t ms = (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000 - 11644473600000ULL);
    lastOrder_ = std::max(ms, lastOrder_ + 1);
    return lastOrder_;
}

void MainWindow::removeGroup(int gi) {
    if (gi < 0 || gi >= (int)groups_.size()) return;
    groups_.erase(groups_.begin() + gi);
}

void MainWindow::removeRow(const ListRow& r) {
    if (r.type == ListRow::Task) {
        const TaskView& t = *r.task;
        if (t.state == TaskState::Running || t.state == TaskState::Paused) queue_.cancel(t.id);
        else queue_.remove(t.id);
        return;
    }
    if (tab_ == 0) {
        if (r.idx < 0 || r.idx >= (int)groups_.size()) return;
        DlGroup& g = groups_[r.idx];
        if (r.type == ListRow::Pending && g.playlist && r.entry >= 0 && r.entry < (int)g.entries.size() && g.entries.size() > 1) {
            g.entries.erase(g.entries.begin() + r.entry);
            g.checked.erase(g.checked.begin() + r.entry);
            g.segA.erase(g.segA.begin() + r.entry);
            g.segB.erase(g.segB.begin() + r.entry);
            syncRange(g.key);
        } else {
            removeGroup(r.idx);
        }
    } else {
        auto& list = tab_ == 2 ? remux_ : conv_;
        if (r.idx >= 0 && r.idx < (int)list.size()) {
            convertPlans_.erase(list[r.idx].key);
            list.erase(list.begin() + r.idx);
        }
    }
    if (sel_[tab_] == r.id) sel_[tab_] = RowId{};
    relayout(true);
}

void MainWindow::openUrl(const std::wstring& url) {
    std::wstring l = lower(url);
    if (startsWith(l, L"https://") || startsWith(l, L"http://")) ShellExecuteW(hwnd_, nullptr, url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::openPath(const std::wstring& path) {
    if (fileExists(path)) ShellExecuteW(hwnd_, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else flash(L"File not found: " + path, 4000);
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
    d->SetOptions(o | (multi ? (DWORD)FOS_ALLOWMULTISELECT : 0u) | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
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
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && mediaExtension(fd.cFileName)) files.push_back(joinPath(f, fd.cFileName));
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        } else if (fileExists(f)) {
            files.push_back(f);
        }
    }
    if (files.empty()) {
        flash(L"No media files there", 3000);
        return;
    }
    tab_ = tab;
    overlay_ = Overlay::None;
    auto& list = tab == 2 ? remux_ : conv_;
    for (auto& f : files) {
        bool dup = false;
        for (auto& r : list) dup = dup || lower(r.path) == lower(f);
        if (dup) continue;
        FileRow row;
        row.key = nextKey_++;
        row.order = newOrder();
        row.path = f;
        list.push_back(row);
        uint64_t key = row.key;
        spawn([this, key, f] {
            WorkResult r;
            r.type = 2;
            r.key = key;
            if (!deps_.waitReady(Dep::FFmpeg, stop_)) return;
            gProbeSlots.acquire();
            deps_.acquire(Dep::FFmpeg);  // an update never swaps FFprobe under a running probe
            if (!probeFile(deps_.ffprobe(), f, r.media, &stop_)) r.error = r.media.error;
            deps_.release(Dep::FFmpeg);
            gProbeSlots.release();
            {
                std::lock_guard<std::mutex> lk(resMu_);
                results_.push_back(std::move(r));
            }
            PostMessageW(hwnd_, WM_APP_RESULT, 0, 0);
        });
    }
    listScroll_[tab] = 0;  // newest first: show what was just added
    relayout(true);
}

void MainWindow::analyzeUrls() {
    int n = GetWindowTextLengthW(edits_[E_URLS]);
    std::wstring t(n, L'\0');
    GetWindowTextW(edits_[E_URLS], t.data(), n + 1);
    if (extractUrls(t).empty()) return;
    SetWindowTextW(edits_[E_URLS], L"");
    analyzeText(t);
}

bool MainWindow::pasteLinks() {
    std::wstring t;
    if (!OpenClipboard(hwnd_)) return false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) t = p;
        GlobalUnlock(h);
    }
    CloseClipboard();
    // A lone word is typed text, not a link: let it paste.
    std::wstring l = lower(trim(t));
    if (extractUrls(t).empty() || (!startsWith(l, L"http") && l.find(L'/') == std::wstring::npos && l.find(L'\n') == std::wstring::npos)) return false;
    analyzeText(t);
    return true;
}

void MainWindow::analyzeText(const std::wstring& text) {
    auto urls = extractUrls(text);
    if (urls.empty()) return;
    Settings s = settingsCopy();
    for (auto& u : urls) {
        bool dup = false;
        for (auto& g : groups_) dup = dup || g.url == u;
        if (dup) continue;
        DlGroup g;
        g.key = nextKey_++;
        g.order = newOrder();
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
            deps_.acquire(Dep::Deno);
            auto c = runCapture(deps_.ytdlp(), buildInfoArgs(u, true, deps_.deno(), &auth), po, 180000, &stop_);
            deps_.release(Dep::Deno);
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
    listScroll_[0] = 0;
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
                break;
            }
        } else if (r.type == 2) {
            for (auto* list : {&conv_, &remux_})
                for (auto& f : *list) {
                    if (f.key != r.key) continue;
                    f.busy = false;
                    f.error = r.error;
                    f.info = r.media;
                }
        }
    }
    relayout(true);
}

// ---------- starting ----------

int MainWindow::queueGroup(DlGroup& g, int onlyEntry, const Settings& s, uint64_t* firstTask) {
    int n = 0;
    auto note = [&](uint64_t id) {
        if (firstTask && !*firstTask) *firstTask = id;
        n++;
    };
    if (!g.playlist) {
        json::Value spec = makeDownloadSpec(s, g.url, g.infoFile, g.segA.empty() ? L"" : g.segA[0], g.segB.empty() ? L"" : g.segB[0]);
        spec.set("order", (long long)g.order);
        spec.set("thumb", g.info.thumb);
        note(queue_.add(TaskKind::Download, g.title, spec));
        g.entries.clear();
        return n;
    }
    for (size_t i = 0; i < g.entries.size();) {
        bool go = onlyEntry >= 0 ? (int)i == onlyEntry : g.checked[i] != 0;
        if (!go) {
            i++;
            continue;
        }
        auto& e = g.entries[i];
        json::Value spec = makeDownloadSpec(s, widen(e.url), L"", g.segA[i], g.segB[i]);
        spec.set("order", (long long)g.order);
        spec.set("orderSub", e.index);
        spec.set("thumb", e.thumb);
        note(queue_.add(TaskKind::Download, widen(e.title), spec));
        g.entries.erase(g.entries.begin() + i);
        g.checked.erase(g.checked.begin() + i);
        g.segA.erase(g.segA.begin() + i);
        g.segB.erase(g.segB.begin() + i);
    }
    return n;
}

const Plan& MainWindow::convertPlan(const FileRow& f) {
    auto [it, inserted] = convertPlans_.try_emplace(f.key);
    if (inserted) {
        ConvertSpec spec = view_.convert;
        spec.accel = view_.encoder;
        std::lock_guard<std::mutex> lk(capsMu_);
        it->second = planConvert(f.info, spec, caps_);
    }
    return it->second;
}

void MainWindow::previewFile(const RowId& id) {
    if (tab_ != 1) return;
    auto it = std::find_if(conv_.begin(), conv_.end(), [&](const FileRow& f) { return f.key == id.key; });
    if (it == conv_.end() || !deps_.ready(Dep::FFmpeg)) return;
    const Plan& p = convertPlan(*it);
    const VideoPreview v = p.preview;
    if (v.sourceIndex < 0) return;
    const auto path = it->path;
    const StreamInfo* source = it->info.video();
    const double duration = source && source->duration > 0 ? source->duration : it->info.duration;
    const std::wstring title = fileName(path) + L" · first video track\nFraming and filters shown; compression and HDR display may differ.";
    auto render = [this, v, path, duration](const std::atomic<bool>& cancel) -> std::pair<HBITMAP, std::wstring> {
        static std::mt19937 rng(std::random_device{}());
        const double at = duration > 0 ? duration * std::uniform_real_distribution<double>(0.15, 0.75)(rng) : 0;
        std::wstring filters = v.filters;
        if (!filters.empty()) filters += L",";
        std::wstring wh = std::to_wstring(v.width) + L":" + std::to_wstring(v.height);
        filters += L"scale=" + wh + L":flags=lanczos,setsar=1";
        std::vector<std::wstring> args{L"-hide_banner", L"-nostdin", L"-v", L"error", L"-ss", fmtFixed(at, 3), L"-i", path,
                                       L"-map", L"0:" + std::to_wstring(v.sourceIndex), L"-vf", filters,
                                       L"-frames:v", L"1", L"-c:v", L"rawvideo", L"-threads:v", L"1", L"-pix_fmt", L"bgra", L"-f", L"rawvideo", L"pipe:1"};
        deps_.acquire(Dep::FFmpeg);
        ProcOpts opts;
        opts.lowPriority = true;
        Capture c = runCapture(deps_.ffmpeg(), args, opts, 25000, &cancel);
        if (!cancel && c.code == 0 && c.out.empty() && at > 0) {
            args[5] = L"0";  // a short video track may end before the container duration
            c = runCapture(deps_.ffmpeg(), args, opts, 25000, &cancel);
        }
        deps_.release(Dep::FFmpeg);
        if (cancel) return {};
        size_t bytes = (size_t)v.width * v.height * 4;
        if (c.code != 0 || c.out.size() != bytes)
            return {nullptr, c.timedOut ? L"Preview timed out." : !c.startError.empty() ? c.startError : !c.err.empty() ? widen(trim(c.err).substr(0, 220)) : L"No frame could be decoded."};
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), v.width, -v.height, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return {nullptr, L"Could not allocate the preview image."};
        std::memcpy(bits, c.out.data(), bytes);
        return {bitmap, {}};
    };
    runPreview(hwnd_, title, v.width, v.height, render, theme_, fonts_, dpi_);
}

bool MainWindow::fileReady(int tab, const FileRow& f, const Settings& s) {
    if (f.busy || !f.info.ok) return false;
    if (tab == 1) return convertPlan(f).ok;
    if (tab != 2) return true;
    return planRemux(f.info, s.remuxSpec(f.remux)).ok;
}

uint64_t MainWindow::queueFile(int tab, const FileRow& f, const Settings& s) {
    if (!fileReady(tab, f, s)) return 0;
    json::Value spec = json::Value::object();
    spec.set("input", f.path);
    spec.set("order", (long long)f.order);
    if (tab == 2) {
        spec.set("outDir", s.remuxDir);
        spec.set("remux", s.remuxSpec(f.remux).toJson());
        return queue_.add(TaskKind::Remux, fileName(f.path), spec);
    }
    spec.set("outDir", s.convertDir);
    spec.set("convert", s.convert.toJson());
    return queue_.add(TaskKind::Convert, fileName(f.path), spec);
}

// One item: a link, a playlist entry, a whole playlist (its checked entries) or a file. The started
// item becomes a task in the same place and keeps the selection.
void MainWindow::startRow(const ListRow& r) {
    if (r.type == ListRow::Task) return;
    Settings s = settingsCopy();
    uint64_t id = 0;
    if (tab_ == 0) {
        if (r.idx < 0 || r.idx >= (int)groups_.size()) return;
        DlGroup& g = groups_[r.idx];
        if (g.busy || !g.error.empty()) return;
        queueGroup(g, r.type == ListRow::Head ? -1 : r.entry, s, &id);
        if (g.entries.empty()) removeGroup(r.idx);
        else syncRange(g.key);
    } else {
        auto& list = tab_ == 2 ? remux_ : conv_;
        if (r.idx < 0 || r.idx >= (int)list.size()) return;
        id = queueFile(tab_, list[r.idx], s);
        if (!id) return;
        convertPlans_.erase(list[r.idx].key);
        list.erase(list.begin() + r.idx);
    }
    if (id && sel_[tab_] == r.id && r.type != ListRow::Head) select(tab_, RowId{id, 0, 0});
    refreshTasks();
    relayout(true);
}

void MainWindow::startAll(int tab) {
    Settings s = settingsCopy();
    int n = 0;
    if (tab == 0) {
        for (size_t i = 0; i < groups_.size();) {
            DlGroup& g = groups_[i];
            if (g.busy || !g.error.empty()) {
                i++;
                continue;
            }
            n += queueGroup(g, -1, s, nullptr);
            if (g.entries.empty()) {
                groups_.erase(groups_.begin() + i);
            } else {
                syncRange(g.key);
                i++;  // a playlist keeps its unchecked entries
            }
        }
    } else {
        auto& list = tab == 2 ? remux_ : conv_;
        for (size_t i = 0; i < list.size();) {
            if (queueFile(tab, list[i], s)) {
                convertPlans_.erase(list[i].key);
                list.erase(list.begin() + i);
                n++;
            } else {
                i++;
            }
        }
    }
    if (!n) return;
    const wchar_t* what[3][2] = {{L" download", L" downloads"}, {L" conversion", L" conversions"}, {L" remux", L" remuxes"}};
    flash(std::to_wstring(n) + what[tab][n == 1 ? 0 : 1] + L" started");
    refreshTasks();
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

void MainWindow::repairTools(bool rebuild) {
    if (deps_.repairing()) return;
    if (rebuild && !runConfirm(hwnd_, L"Rebuild managed tools?",
        L"Download and test fresh copies of yt-dlp, FFmpeg, FFprobe and Deno. This replaces the current tools and can replace their rollback copies. Running tasks finish with the tools they started with. Your settings, media, history and queue stay unchanged.",
        L"rebuild tools", L"cancel", theme_, fonts_, dpi_)) return;
    std::vector<std::wstring> errors;
    for (auto it = tasks_.rbegin(); it != tasks_.rend() && errors.size() < 36; ++it) {
        if (it->state != TaskState::Failed) continue;
        errors.push_back(it->error + L" " + it->hint);
        auto log = queue_.taskLog(it->id);
        for (auto ln = log.rbegin(); ln != log.rend() && ln - log.rbegin() < 3; ++ln) errors.push_back(*ln);
    }
    for (auto& g : groups_)
        if (!g.error.empty() && errors.size() < 40) errors.push_back(g.error + L" " + g.hint);
    if (!deps_.startRepair(rebuild, std::move(errors), settingsCopy())) {
        flash(L"Tool maintenance is already running. Try again when it finishes.");
        return;
    }
    relayout();
}

void MainWindow::detectEncodersIfNeeded() {
    if (capsRunning_ || !deps_.ready(Dep::FFmpeg)) return;
    std::string hash = deps_.ffmpegHash();
    if (settingsCopy().encoderCapsFor == hash) {
        std::lock_guard<std::mutex> lk(capsMu_);
        if (caps_.probed) return;
    }
    capsRunning_ = true;
    spawn([this] {
        deps_.acquire(Dep::FFmpeg);
        std::string hash = deps_.ffmpegHash();
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
