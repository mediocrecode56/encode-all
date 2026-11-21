// The main window. Everything is drawn as a flat list of items rebuilt on change; a diff of the
// old and new lists decides which rectangles repaint. Text entry uses a few native EDIT controls.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

enum : UINT { WM_APP_QUEUE = WM_APP + 1, WM_APP_DEPS, WM_APP_RESULT, WM_APP_THUMB, WM_APP_UPDATE, WM_APP_CAPS, WM_APP_EDITMENU, WM_APP_PASTE };

constexpr unsigned S_Bold = 1, S_Mid = 2, S_Invert = 4, S_Disabled = 8, S_Click = 16, S_Right = 32, S_Selected = 64, S_Big = 128, S_Center = 256,
                   S_Mono = 512;
constexpr UINT_PTR T_QUEUE = 1, T_FLASH = 2, T_HEALTHY = 3, T_SAVE = 4;
enum Kind { K_Text, K_Fill, K_Line, K_Progress, K_Thumb, K_Region, K_Frame, K_Button, K_Check, K_Box, K_Cell, K_WinBtn, K_Icon, K_Bar, K_Chip };
// K_Icon: a square icon button (value = Icon, S_Invert when its view is open; without S_Click a plain glyph).
// K_Bar: a scroll position mark. K_Chip: a framed tag (a file's extension where a download shows its thumbnail).

struct Item {
    RECT r{};
    int kind = K_Text;
    unsigned style = 0;
    std::wstring text;
    std::function<void()> click, rclick;
    std::function<void(int)> wheel;
    std::wstring tip;  // shown in the header's hint while the mouse is over the item (icon buttons)
    HBITMAP bmp = nullptr;
    double value = 0;
    RECT clip{};
    bool clipped = false;
};

enum Edit { E_URLS, E_RANGE, E_SEGA, E_SEGB, E_CUSTOM, E_COUNT };
enum class Overlay { None, Log, Settings, History };

struct DlGroup {
    uint64_t key = 0;
    int64_t order = 0;  // list position (newest first), carried into the tasks it starts
    std::wstring url;
    bool busy = true, playlist = false;
    std::wstring error, hint, title;
    YtInfo info;
    std::wstring infoFile;
    std::vector<YtEntry> entries;
    std::vector<char> checked;
    std::vector<std::wstring> segA, segB;
};
std::wstring rangesText(const DlGroup& g);                 // checked entries by playlist number: "1-3,7"
bool applyRanges(DlGroup& g, const std::wstring& text);  // the range box back to checks

struct FileRow {
    uint64_t key = 0;
    int64_t order = 0;
    std::wstring path;
    MediaInfo info;
    bool busy = true;
    std::wstring error;
    RemuxSpec remux;
};

// One row of a section's list: an item not started yet (a link, a playlist entry, a file) or a task.
// Pending rows are keyed by their item (key, sub: -1 playlist head, 0 single, else the entry's playlist
// index); started ones by task id. A started item keeps its order, so it stays where it was.
struct RowId {
    uint64_t task = 0, key = 0;
    int sub = 0;
    bool empty() const { return !task && !key; }
    bool operator==(const RowId& o) const { return task == o.task && key == o.key && sub == o.sub; }
};

struct ListRow {
    enum Type { Pending, Head, Task } type = Pending;
    int idx = -1;    // group or file index (pending)
    int entry = -1;  // playlist entry position (pending), -1 single
    const TaskView* task = nullptr;
    int64_t order = 0;
    int sub = 0;
    RowId id;
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
    void buildSection(RECT area);  // input bar, options, list head and the list of the current tab
    void buildDownloadOptions(int& y, const RECT& a);
    void buildConvertOptions(int& y, const RECT& a);
    void buildRemuxOptions(int& y, const RECT& a);
    int buildRow(const ListRow& r, int left, int right, int y, bool selected);  // returns its height
    int buildRowDetail(const ListRow& r, int left, int right, int y);         // the selected row's extra lines
    std::vector<ListRow> collectRows(int tab);
    void withRow(const RowId& id, const std::function<void(const ListRow&)>& fn);  // runs fn on the row if it is still listed
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
    // A labelled button with an icon right of the label, laid out right to left; returns its left edge.
    int iconButtonText(int right, int y, const std::wstring& label, int icon, std::function<void()> fn, unsigned style = 0);
    int iconW() const { return S(19); }  // the glyph in a labelled button
    int check(int x, int y, bool on, std::function<void()> fn);  // a box without a label (list rows)
    HFONT fontFor(unsigned style) const;
    int choice(int x, int y, const std::wstring& label, const std::wstring& value, std::function<void()> open, std::function<void(int)> wheel = nullptr,
               int maxW = 0);
    int seg(int x, int y, const std::vector<std::wstring>& opts, int sel, std::function<void(int)> fn);
    int iconButton(int x, int y, int icon, bool on, std::function<void()> fn);  // returns the left edge (laid out right to left)
    int fieldLabel(int x, int y, const std::wstring& label, unsigned style = S_Mid);  // fixed label column; returns the control x
    int labelW() const { return S(130); }
    int checkBox() const { return S(17); }  // a checkbox square; toggles measure their width with it
    void scrollbar(const RECT& view, int offset, int content);
    int sectionTitle(int x, int y, const std::wstring& title);  // bold title; returns its right edge
    void emptyState(const RECT& r, const std::wstring& line1, const std::wstring& line2 = L"");
    void hline(int x0, int x1, int y);
    void region(const RECT& r, std::function<void(int)> wheel);
    void placeEdit(Edit e, const RECT& r, bool frame = true);
    int rowIcon(int right, int top, int size, int icon, const std::wstring& tip, std::function<void()> fn);  // right to left; returns the left edge
    void beginClip(const RECT& r) { clip_ = r; clipping_ = true; }
    void endClip() { clipping_ = false; }
    // field: an inline caption and a dropdown, flowing left to right and wrapping to the control column.
    void field(int& x, int& y, int left, int right, const std::wstring& caption, const std::wstring& value, std::function<void()> open,
               std::function<void(int)> wheel = nullptr);
    int fieldW(const std::wstring& caption, const std::wstring& value);
    int popup(const std::vector<std::wstring>& items, int checked);  // the app's own dropdown, under the clicked control
    int popup(const std::vector<std::wstring>& items, const std::vector<int>& checked);  // sectioned: one check per section
    int rowH() const { return fonts_.lineH; }
    int barH() const { return S(40); }  // the top bar: drag area, title, window buttons (Weekly-style chrome)
    int itemH() const { return rowH() * 2 + S(30); }  // a list row: two lines beside its thumbnail, with room above and below
    int denseH() const { return fonts_.denseH; }

    // actions
    void setTab(int t);
    void setOverlay(Overlay o);
    void flash(const std::wstring& msg, UINT ms = 4000);
    void select(int tab, const RowId& id);  // also loads the segment boxes of a pending download
    void removeRow(const ListRow& r);
    void removeGroup(int gi);
    void syncRange(uint64_t key);  // the range box follows checks changed elsewhere
    void startRow(const ListRow& r);  // starts one item (a playlist head starts its checked entries)
    void startAll(int tab);
    int queueGroup(DlGroup& g, int onlyEntry, const Settings& s, uint64_t* firstTask);  // queues and removes entries; returns the count
    uint64_t queueFile(int tab, const FileRow& f, const Settings& s);  // 0 when the remux plan fails
    bool fileReady(int tab, const FileRow& f, const Settings& s);
    int64_t newOrder();
    void openUrl(const std::wstring& url);
    void openPath(const std::wstring& path);
    void spawn(std::function<void()> fn);  // a worker thread; finished ones are joined as new ones start
    void reapWorkers();
    void editMenu(HWND edit, LPARAM at);
    void analyzeUrls();                     // the link box: Enter
    void analyzeText(const std::wstring& t);  // pasted, dropped or passed on the command line
    bool pasteLinks();                      // Ctrl+V into the link box: true when the clipboard held links
    void addFiles(const std::vector<std::wstring>& files, int tab);
    void pickFiles(int tab);
    std::vector<std::wstring> openFiles(const wchar_t* typeName, const wchar_t* spec, bool multi);
    // An inline text box over the clicked control (at least 160 px wide); Enter or leaving it commits, Esc cancels.
    void customValue(const std::wstring& initial, const RECT& at, std::function<void(const std::wstring&)> done);
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
    RECT anchor_{};  // the control whose click is running (client coordinates), for dropdown placement
    bool anchorValid_ = false;
    RECT client_{};

    // state
    Settings settings_;
    std::mutex settingsMu_;
    Deps deps_;
    Queue queue_;
    AppUpdater updater_;
    EncoderCaps caps_;
    std::mutex capsMu_;
    std::atomic<bool> capsRunning_{false};
    Thumbs thumbs_;
    int tab_ = 0;
    Overlay overlay_ = Overlay::None;
    std::vector<DlGroup> groups_;
    std::vector<FileRow> conv_, remux_;
    RowId sel_[3];
    int listScroll_[3] = {};
    bool reveal_ = false;            // scroll the selected row into view on the next layout
    std::vector<RowId> rowIds_;      // the current tab's rows in order (keyboard)
    uint64_t rangeKey_ = 0;          // the playlist whose head holds the range box
    std::vector<CustomPreset> presets_;
    int customPreset_ = -1;
    std::vector<TaskView> tasks_;
    uint64_t selTask_ = 0;           // the selected row's task (log view)
    int64_t lastOrder_ = 0;
    std::wstring hoverTip_, hintDefault_;  // the header's hint: the hovered item's tip, else the last notice
    size_t hintIdx_ = SIZE_MAX;
    void setHint(const std::wstring& tip);
    Settings view_;     // the settings the current layout was built from
    RECT listView_{};   // the visible list (edits inside it are placed only when wholly visible)
    int logScroll_ = 0, settingsScroll_ = 0;
    bool logApp_ = false;
    int historyScroll_ = 0;
    std::vector<HistoryItem> history_;
    std::wstring flash_;  // one-line notice in the header
    ULONGLONG flashUntil_ = 0;
    std::function<void(const std::wstring&)> customDone_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> queuePosted_{false};
    std::atomic<ULONGLONG> lastQueuePost_{0};
    std::mutex resMu_;
    std::vector<WorkResult> results_;
    struct Worker {
        std::thread t;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Worker> workers_;
    std::thread updateThread_;
    std::atomic<bool> updateBusy_{false};
    uint64_t nextKey_ = 1;
    bool timerOn_ = false;
    bool healthy_ = false;
    bool building_ = false;  // inside build(): relayout() is ignored
};

}  // namespace ea::ui
