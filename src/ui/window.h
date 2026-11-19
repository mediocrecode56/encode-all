// The main window. Everything is drawn as a flat list of items rebuilt on change; a diff of the
// old and new lists decides which rectangles repaint. Text entry uses a few native EDIT controls.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../app/appupdate.h"
#include "../app/deps.h"
#include "../app/queue.h"
#include "../app/settings.h"
#include "../dl/ytdlp.h"
#include "../media/encode.h"
#include "../media/probe.h"
#include "gfx.h"
#include "thumbs.h"

namespace ea::ui {

enum : UINT { WM_APP_QUEUE = WM_APP + 1, WM_APP_DEPS, WM_APP_RESULT, WM_APP_THUMB, WM_APP_UPDATE, WM_APP_CAPS };

constexpr unsigned S_Bold = 1, S_Mid = 2, S_Invert = 4, S_Disabled = 8, S_Click = 16, S_Right = 32, S_Selected = 64, S_Big = 128, S_Center = 256,
                   S_Mono = 512;
constexpr UINT_PTR T_QUEUE = 1, T_FLASH = 2, T_HEALTHY = 3, T_SAVE = 4;
enum Kind { K_Text, K_Fill, K_Line, K_Progress, K_Thumb, K_Region, K_Frame, K_Button, K_Check, K_Box, K_Cell, K_WinBtn };

struct Item {
    RECT r{};
    int kind = K_Text;
    unsigned style = 0;
    std::wstring text;
    std::function<void()> click, rclick;
    std::function<void(int)> wheel;
    HBITMAP bmp = nullptr;
    double value = 0;
    RECT clip{};
    bool clipped = false;
};

enum Edit { E_URLS, E_RANGE, E_SEGA, E_SEGB, E_CUSTOM, E_COUNT };
enum class Overlay { None, Log, Settings, History };

struct DlGroup {
    uint64_t key = 0;
    std::wstring url;
    bool busy = true, playlist = false;
    std::wstring error, hint, title;
    YtInfo info;
    std::wstring infoFile;
    std::vector<YtEntry> entries;
    std::vector<char> checked;
    std::vector<std::wstring> segA, segB;
};

struct FileRow {
    uint64_t key = 0;
    std::wstring path;
    MediaInfo info;
    bool busy = true, checked = true;
    std::wstring error;
    RemuxSpec remux;
};

struct WorkResult {
    int type = 0;  // 1 analyse, 2 probe
    uint64_t key = 0;
    YtResult yt;
    std::wstring infoFile, error, hint;
    MediaInfo media;
};

class MainWindow {
public:
    bool create(HINSTANCE inst, int show, const std::vector<std::wstring>& args);
    HWND hwnd() const { return hwnd_; }
    bool preTranslate(MSG& m);
    void openArgs(const std::vector<std::wstring>& args);

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT handle(UINT m, WPARAM w, LPARAM l);
    void onCreate();
    void onDestroy();
    void onPaint();
    void onSize();
    void onDpi(int dpi, const RECT* suggested);
    void applyTheme();

    // layout
    void relayout(bool full = false);
    void build();
    void buildHeader(RECT& area);
    void buildDownload(RECT area);
    void buildConvert(RECT area);
    void buildRemux(RECT area);
    void buildQueue(RECT area);
    void buildLog(RECT area);
    void buildSettings(RECT area);
    void buildHistory(RECT area);
    void buildPlan(RECT& area, const Plan& p, int& y);

    // item helpers
    Item& add(int kind, const RECT& r, const std::wstring& text = L"", unsigned style = 0);
    int tw(const std::wstring& s, unsigned style = 0);
    int S(int v) const { return scale(v, dpi_); }
    int text(int x, int y, const std::wstring& s, unsigned style = 0, int maxW = 0);
    int button(int x, int y, const std::wstring& label, std::function<void()> fn, unsigned style = 0);
    int toggle(int x, int y, const std::wstring& label, bool on, std::function<void()> fn);
    int check(int x, int y, bool on, std::function<void()> fn);  // a box without a label (list rows)
    HFONT fontFor(unsigned style) const;
    int choice(int x, int y, const std::wstring& label, const std::wstring& value, std::function<void()> open, std::function<void(int)> wheel = nullptr);
    int seg(int x, int y, const std::vector<std::wstring>& opts, int sel, std::function<void(int)> fn);
    void hline(int x0, int x1, int y);
    void progressBar(const RECT& r, double v);
    void region(const RECT& r, std::function<void(int)> wheel);
    void placeEdit(Edit e, const RECT& r);
    void beginClip(const RECT& r) { clip_ = r; clipping_ = true; }
    void endClip() { clipping_ = false; }
    int popup(const std::vector<std::wstring>& items, int checked);
    int rowH() const { return fonts_.lineH; }
    int barH() const { return S(40); }  // the top bar: drag area, title, window buttons (Weekly-style chrome)
    int denseH() const { return fonts_.denseH; }

    // actions
    void setTab(int t);
    void analyzeUrls();
    void startDownloads();
    void addFiles(const std::vector<std::wstring>& files, int tab);
    void pickFiles(int tab);
    void startConvert();
    void startRemux();
    void customValue(const std::wstring& prompt, const std::wstring& initial, const RECT& at, std::function<void(const std::wstring&)> done);
    void commitCustom(bool ok);
    std::wstring pickFolder(const std::wstring& initial);
    void saveSettingsNow();
    Settings settingsCopy();
    void updateSetting(const std::function<void(Settings&)>& f);
    void onQueueNotify(bool force);
    void applyResults();
    void detectEncodersIfNeeded();
    void openFolder(const std::wstring& path);
    void copyText(const std::wstring& t);
    bool editFocused() const;
    int hitTest(POINT p) const;
    void refreshTasks();
    void scrollBy(int& offset, int delta, int maxOffset);

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HWND edits_[E_COUNT] = {};
    RECT editRects_[E_COUNT] = {};
    bool editPlaced_[E_COUNT] = {};
    int dpi_ = 96;
    Theme theme_;
    Fonts fonts_;
    HDC measureDc_ = nullptr;
    HBITMAP backBmp_ = nullptr;
    HDC backDc_ = nullptr;
    SIZE backSize_{};
    std::vector<Item> items_, prev_;
    RECT clip_{};
    bool clipping_ = false;
    int hover_ = -1, pressed_ = -1;
    POINT mouse_{-1, -1};
    RECT client_{};

    // state
    Settings settings_;
    std::mutex settingsMu_;
    Deps deps_;
    Queue queue_;
    AppUpdater updater_;
    EncoderCaps caps_;
    std::mutex capsMu_;
    bool capsRunning_ = false;
    Thumbs thumbs_;
    int tab_ = 0;
    Overlay overlay_ = Overlay::None;
    std::vector<DlGroup> groups_;
    int selGroup_ = -1, selEntry_ = -1;
    int dlScroll_ = 0;
    std::vector<FileRow> conv_, remux_;
    int selConv_ = -1, selRemux_ = -1;
    int convScroll_ = 0, remuxScroll_ = 0, detailScroll_ = 0;
    bool advanced_ = false;
    std::vector<CustomPreset> presets_;
    int customPreset_ = -1;
    std::vector<TaskView> tasks_;
    uint64_t selTask_ = 0;
    bool revealTask_ = false;
    int queueScroll_ = 0, logScroll_ = 0, settingsScroll_ = 0;
    bool logApp_ = false;
    std::vector<HistoryItem> history_;
    std::wstring flash_;  // one-line notice in the header
    ULONGLONG flashUntil_ = 0;
    std::function<void(const std::wstring&)> customDone_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> queuePosted_{false};
    std::atomic<ULONGLONG> lastQueuePost_{0};
    std::mutex resMu_;
    std::vector<WorkResult> results_;
    std::vector<std::thread> workers_;
    std::thread updateThread_;
    uint64_t nextKey_ = 1;
    bool timerOn_ = false;
    bool healthy_ = false;
};

}  // namespace ea::ui
