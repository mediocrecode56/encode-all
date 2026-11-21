// Layout of every section. Each build function appends items for one area of the window.
// Shared grammar: a bold section title, then rows with a fixed label column (mid ink, upper case)
// and the controls to its right; lists scroll with a thin position mark; empty lists say what to do.
#include "window.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "../app/runners.h"
#include "../core/util.h"
#include "../version.h"

namespace ea::ui {

namespace {

const std::vector<std::wstring> kTargets = {L"H.265 MP4", L"H.264 MP4", L"H.265 MOV", L"H.264 MOV", L"DNxHR LB MOV", L"DNxHR SQ MOV", L"DNxHR HQ MOV",
                                            L"DNxHR HQX MOV", L"DNxHR 444 MOV", L"copy video MP4", L"copy video MOV", L"FLAC", L"MP3", L"WAV"};

int targetIndex(const ConvertSpec& s) {
    if (s.audio == AudioTarget::FLAC) return 11;
    if (s.audio == AudioTarget::MP3) return 12;
    if (s.audio == AudioTarget::WAV) return 13;
    bool mov = s.container == Container::MOV;
    switch (s.vcodec) {
        case VCodec::H265: return mov ? 2 : 0;
        case VCodec::H264: return mov ? 3 : 1;
        case VCodec::DNxHR_LB: return 4;
        case VCodec::DNxHR_SQ: return 5;
        case VCodec::DNxHR_HQ: return 6;
        case VCodec::DNxHR_HQX: return 7;
        case VCodec::DNxHR_444: return 8;
        case VCodec::Copy: return mov ? 10 : 9;
    }
    return 0;
}

void setTarget(ConvertSpec& s, int i) {
    s.audio = AudioTarget::None;
    s.container = Container::MP4;
    switch (i) {
        case 0: s.vcodec = VCodec::H265; break;
        case 1: s.vcodec = VCodec::H264; break;
        case 2: s.vcodec = VCodec::H265; s.container = Container::MOV; break;
        case 3: s.vcodec = VCodec::H264; s.container = Container::MOV; break;
        case 4: s.vcodec = VCodec::DNxHR_LB; s.container = Container::MOV; break;
        case 5: s.vcodec = VCodec::DNxHR_SQ; s.container = Container::MOV; break;
        case 6: s.vcodec = VCodec::DNxHR_HQ; s.container = Container::MOV; break;
        case 7: s.vcodec = VCodec::DNxHR_HQX; s.container = Container::MOV; break;
        case 8: s.vcodec = VCodec::DNxHR_444; s.container = Container::MOV; break;
        case 9: s.vcodec = VCodec::Copy; break;
        case 10: s.vcodec = VCodec::Copy; s.container = Container::MOV; break;
        case 11: s.audio = AudioTarget::FLAC; break;
        case 12: s.audio = AudioTarget::MP3; break;
        case 13: s.audio = AudioTarget::WAV; break;
    }
    if (s.rate == RateMode::Lossless && isDnxhr(s.vcodec)) s.rate = RateMode::Quality;
}

std::wstring qualityText(const ConvertSpec& s) {
    if (isDnxhr(s.vcodec)) return L"fixed by profile";
    if (s.vcodec == VCodec::Copy) return L"copy";
    if (s.rate == RateMode::Lossless) return L"lossless";
    if (s.rate == RateMode::Bitrate) return fmtFixed(s.mbps, s.mbps < 10 ? 1 : 0) + L" Mb/s";
    return L"CRF " + std::to_wstring(s.quality);
}

// Size, frame rate and pixel format in a few words ("none" when all follow the source).
std::wstring filtersText(const ConvertSpec& s) {
    std::wstring r;
    auto add = [&](const std::wstring& p) { r += (r.empty() ? L"" : L", ") + p; };
    if (s.width && s.height) add(std::to_wstring(s.width) + L"x" + std::to_wstring(s.height));
    else if (s.height) add(std::to_wstring(s.height) + L"p");
    else if (s.width) add(std::to_wstring(s.width) + L"w");
    if (s.fps > 0) add(fmtFps(s.fps) + L" fps");
    if (s.pixFmt == "yuv420p") add(L"8-bit 4:2:0");
    else if (s.pixFmt == "yuv420p10le") add(L"10-bit 4:2:0");
    else if (s.pixFmt == "yuv422p10le") add(L"10-bit 4:2:2");
    else if (s.pixFmt == "yuv444p10le") add(L"10-bit 4:4:4");
    else if (!s.pixFmt.empty()) add(widen(s.pixFmt));
    return r.empty() ? L"none" : r;
}

std::wstring audioText(const ConvertSpec& s) {
    switch (s.acodec) {
        case ACodec::Copy: return L"copy";
        case ACodec::AAC: return L"AAC " + std::to_wstring(s.aacKbps) + L"k";
        case ACodec::PCM: return s.pcmBits == 16 ? L"PCM 16-bit" : s.pcmBits == 24 ? L"PCM 24-bit" : L"PCM exact";
    }
    return L"";
}

// "h264_nvenc" -> "NVENC", "libx265" -> "x265".
std::wstring encoderLabel(const std::string& enc) {
    if (contains(enc, "nvenc")) return L"NVENC";
    if (contains(enc, "qsv")) return L"Quick Sync";
    if (contains(enc, "amf")) return L"AMF";
    if (enc == "libx264") return L"x264";
    if (enc == "libx265") return L"x265";
    return widen(enc);
}

std::wstring shortMedia(const MediaInfo& m) {
    std::wstring r;
    if (const StreamInfo* v = m.video()) {
        r += widen(v->codec) + L" " + std::to_wstring(v->width) + L"x" + std::to_wstring(v->height);
        if (v->fps > 0) r += L" " + fmtFps(v->fps);
        if (v->bitDepth > 8) r += L" " + std::to_wstring(v->bitDepth) + L"b";
        if (v->hdr()) r += L" HDR";
        r += L"  \u00b7  ";
    }
    int na = m.count("audio");
    if (const StreamInfo* a = m.audio()) {
        r += widen(a->codec) + L" " + (a->layout.empty() ? std::to_wstring(a->channels) + L"ch" : widen(a->layout));
        if (na > 1) r += L" +" + std::to_wstring(na - 1);
        r += L"  \u00b7  ";
    }
    int ns = m.count("subtitle");
    if (ns) r += std::to_wstring(ns) + L" subs  \u00b7  ";
    r += fmtDuration(m.duration) + L"  \u00b7  " + fmtBytes((double)m.size);
    if (m.bitrate > 0) r += L"  \u00b7  " + fmtBitrate(m.bitrate);
    return r;
}

// What a download will fetch with the current options.
DlPrefs dlPrefs(const Settings& s) {
    DlPrefs p;
    p.out = s.dlOut;
    p.preferH264 = (s.dlOut == DlOut::MP4 || s.dlOut == DlOut::MOV) && !s.dlHevc;
    p.crf = s.dlCrf;
    p.hevc = s.dlHevc;
    return p;
}

// "C:\clip.mkv" -> "MKV" (the tag a file shows where a download shows its thumbnail).
std::wstring extTag(const std::wstring& path) {
    std::wstring e = extension(path);
    if (!e.empty() && e[0] == L'.') e.erase(0, 1);
    for (auto& c : e) c = (wchar_t)towupper(c);
    return e.size() > 5 ? e.substr(0, 5) : e;
}
const wchar_t* classTag(QualityClass c) { return c == QualityClass::Copy ? L"COPY" : c == QualityClass::Lossless ? L"LOSSLESS" : L"LOSSY"; }

}  // namespace

// Checked playlist entries as "1-3,7", by their playlist numbers (what the rows show), so the text
// stays right after entries are started or removed.
std::wstring rangesText(const DlGroup& g) {
    std::wstring r;
    int n = (int)std::min(g.entries.size(), g.checked.size());
    for (int i = 0; i < n; i++) {
        if (!g.checked[i]) continue;
        int j = i;
        while (j + 1 < n && g.checked[j + 1] && g.entries[j + 1].index == g.entries[j].index + 1) j++;
        r += (r.empty() ? L"" : L",") + std::to_wstring(g.entries[i].index) + (j > i ? L"-" + std::to_wstring(g.entries[j].index) : L"");
        i = j;
    }
    return r;
}

// The range box's text back to checks; false (nothing changed) while it does not parse.
bool applyRanges(DlGroup& g, const std::wstring& text) {
    int maxIndex = 0;
    for (auto& e : g.entries) maxIndex = std::max(maxIndex, e.index);
    std::vector<int> idx;
    if (!g.playlist || !parseRanges(text, maxIndex, idx)) return false;
    std::vector<char> want(maxIndex + 1, 0);
    for (int i : idx) want[i] = 1;
    for (size_t e = 0; e < g.entries.size() && e < g.checked.size(); e++) g.checked[e] = g.entries[e].index > 0 && want[g.entries[e].index];
    return true;
}

void MainWindow::build() {
    int pad = S(20);
    RECT area{pad, 0, client_.right - pad, client_.bottom - S(14)};
    if (area.right - area.left < S(200)) return;
    buildHeader(area);
    switch (overlay_) {
        case Overlay::Log: buildLog(area); break;
        case Overlay::Settings: buildSettings(area); break;
        case Overlay::History: buildHistory(area); break;
        default: buildSection(area);
    }
}

void MainWindow::buildHeader(RECT& a) {
    int rh = rowH(), bh = barH();
    int y = (bh - rh) / 2;  // bar content is centred vertically
    // Window buttons: full bar height, flush with the top-right corner.
    int bw = S(46);
    int wx = client_.right;
    for (int k = 2; k >= 0; k--) {
        wx -= bw;
        Item& wb = add(K_WinBtn, RECT{wx, 0, wx + bw, bh}, L"", S_Click);
        wb.value = k == 1 && IsZoomed(hwnd_) ? 3 : k;  // 0 minimise, 1 maximise, 2 close, 3 restore
        wb.click = [this, k] {
            if (k == 0) PostMessageW(hwnd_, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            else if (k == 1) PostMessageW(hwnd_, WM_SYSCOMMAND, IsZoomed(hwnd_) ? SC_RESTORE : SC_MAXIMIZE, 0);
            else PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        };
    }
    int x = text(a.left, y, L"ENCODE ALL", S_Big);
    text(x + S(10), y, L"v" EA_VERSION_W, S_Mid);
    // Right side, laid out right to left: an update ready to apply, then the tool status.
    int rx = wx - S(16);
    UpdateState us = updater_.state();
    if (us.staged) {
        std::wstring label = L"restart to update " + us.stagedVersion;
        int w = tw(label, S_Bold) + S(24);
        rx -= w;
        Item& it = add(K_Button, RECT{rx, y + S(3), rx + w, y + rh - S(3)}, label, S_Bold | S_Click);
        it.click = [this] {
            if (queue_.running() > 0) {
                flash(L"Finish or pause running tasks first; the update also applies on the next start.", 5000);
                return;
            }
            if (AppUpdater::relaunch()) DestroyWindow(hwnd_);
        };
        rx -= S(14);
    }
    std::wstring st;
    bool problem = false;
    for (int i = 0; i < kDeps; i++) {
        DepView v = deps_.view((Dep)i);
        if (v.busy && !v.status.empty()) {
            st = std::wstring(depName((Dep)i)) + L": " + v.status;
            break;
        }
        if (!v.present && !v.error.empty()) {
            st = L"! " + std::wstring(depName((Dep)i)) + L": " + v.error;
            problem = true;
            break;
        }
        if (!v.present) st = L"preparing tools";
    }
    if (st.empty()) st = L"tools ready";
    int sw = std::min<int>(tw(st), std::max<int>(0, rx - (a.left + S(260))));
    rx -= sw;
    Item& s = add(K_Text, RECT{rx, y, rx + sw, y + rh}, st, (problem ? S_Bold : S_Mid) | S_Click);
    s.click = [this] { setOverlay(Overlay::Settings); };
    hline(0, client_.right, bh - 1);
    // Sections on the left (with how many of their tasks are active), views as icons on the far right.
    int active[3] = {};
    for (auto& t : tasks_)
        if (t.state == TaskState::Running || t.state == TaskState::Waiting || t.state == TaskState::Paused) active[(int)t.kind]++;
    y = bh + S(10);
    x = a.left;
    const wchar_t* names[] = {L"DOWNLOAD", L"CONVERT", L"REMUX"};
    for (int i = 0; i < 3; i++) {
        std::wstring label = names[i];
        if (active[i]) label += L"  " + std::to_wstring(active[i]);
        int w = tw(label, S_Bold) + S(36);
        bool on = overlay_ == Overlay::None && tab_ == i;
        Item& it = add(K_Cell, RECT{x, y, x + w, y + rh}, label, S_Bold | S_Click | (on ? S_Invert : 0));
        it.click = [this, i] { setTab(i); };
        if (active[i]) it.tip = std::to_wstring(active[i]) + (active[i] == 1 ? L" task" : L" tasks") + L" running, waiting or paused";
        x += w + S(2);
    }
    auto toggleView = [this](Overlay o) { setOverlay(overlay_ == o ? Overlay::None : o); };
    int ix = a.right;
    ix = iconButton(ix, y, I_Settings, overlay_ == Overlay::Settings, [toggleView] { toggleView(Overlay::Settings); }) - S(4);
    items_.back().tip = L"settings (Ctrl+,)";
    ix = iconButton(ix, y, I_Log, overlay_ == Overlay::Log, [toggleView] { toggleView(Overlay::Log); }) - S(4);
    items_.back().tip = L"log (Ctrl+L)";
    ix = iconButton(ix, y, I_History, overlay_ == Overlay::History, [toggleView] { toggleView(Overlay::History); });
    items_.back().tip = L"download history";
    // One hint line: what the hovered control does, else the last notice or update error.
    hintDefault_ = !flash_.empty() ? flash_ : us.error;
    hintIdx_ = items_.size();
    add(K_Text, RECT{x + S(16), y, ix - S(16), y + rh}, hintDefault_, S_Mid | S_Right);
    y += rh + S(10);
    hline(a.left, a.right, y);
    a.top = y + S(16);
}

// ---------- sections ----------
// Every section works the same way: a bar at the top takes links or files, the options below it apply
// to what you start, and the list holds every item, before and after it starts. Each row opens its
// link or file, shows its folder and starts (or pauses, resumes, retries) on its own; "start all" at
// the top right starts every item that is ready. A click on a row shows its details under it.

void MainWindow::syncRange(uint64_t key) {
    if (key != rangeKey_) return;
    for (auto& g : groups_)
        if (g.key == key) SetWindowTextW(edits_[E_RANGE], rangesText(g).c_str());
}

std::vector<ListRow> MainWindow::collectRows(int tab) {
    std::vector<ListRow> rows;
    for (auto& t : tasks_) {
        if ((int)t.kind != tab) continue;
        ListRow r;
        r.type = ListRow::Task;
        r.task = &t;
        r.order = t.order ? t.order : (int64_t)t.id;  // tasks from before 1.0.3 sort below everything new
        r.sub = t.sub;
        r.id.task = t.id;
        rows.push_back(r);
    }
    if (tab == 0) {
        for (int gi = 0; gi < (int)groups_.size(); gi++) {
            DlGroup& g = groups_[gi];
            ListRow r;
            r.idx = gi;
            r.order = g.order;
            r.id.key = g.key;
            if (g.playlist && !g.busy && g.error.empty()) {
                r.type = ListRow::Head;
                r.sub = r.id.sub = -1;
                rows.push_back(r);
                r.type = ListRow::Pending;
                for (int e = 0; e < (int)g.entries.size(); e++) {
                    r.entry = e;
                    r.sub = r.id.sub = std::max(1, g.entries[e].index);
                    rows.push_back(r);
                }
            } else {
                rows.push_back(r);
            }
        }
    } else {
        auto& list = tab == 2 ? remux_ : conv_;
        for (int i = 0; i < (int)list.size(); i++) {
            ListRow r;
            r.idx = i;
            r.order = list[i].order;
            r.id.key = list[i].key;
            rows.push_back(r);
        }
    }
    // Newest first; a playlist's entries in playlist order under its head, started or not.
    std::stable_sort(rows.begin(), rows.end(), [](const ListRow& a, const ListRow& b) { return a.order != b.order ? a.order > b.order : a.sub < b.sub; });
    return rows;
}

void MainWindow::withRow(const RowId& id, const std::function<void(const ListRow&)>& fn) {
    // Actions look their row up again when they run: the list may have changed since it was drawn.
    for (auto& r : collectRows(tab_))
        if (r.id == id) {
            fn(r);
            return;
        }
}

void MainWindow::buildSection(RECT a) {
    int rh = rowH();
    int y = a.top;
    view_ = settingsCopy();
    // The bar: links on Download, files on Convert and Remux.
    int bh = rh + S(18);
    RECT bar{a.left, y, a.right, y + bh};
    int ic = S(42);  // the leading icon's column
    if (tab_ == 0) {
        int pw = tw(L"paste") + S(36);
        Item& pa = add(K_Cell, RECT{bar.right - pw, bar.top + 1, bar.right - 1, bar.bottom - 1}, L"paste", S_Click);
        pa.tip = L"read every link on the clipboard (Ctrl+V)";
        pa.click = [this] {
            if (!pasteLinks()) flash(L"No links on the clipboard", 2500);
        };
        add(K_Line, RECT{bar.right - pw - 1, bar.top + S(10), bar.right - pw, bar.bottom - S(10)});
        Item& li = add(K_Icon, RECT{bar.left, bar.top, bar.left + ic, bar.bottom}, L"", S_Mid);
        li.value = I_Link;
        int eh = fonts_.textH + S(4);
        int ey = bar.top + (bh - eh) / 2;
        placeEdit(E_URLS, RECT{bar.left + ic, ey, bar.right - pw - S(12), ey + eh}, false);
    } else {
        Item& b = add(K_Text, bar, L"", S_Click);
        b.click = [this] { pickFiles(tab_); };
        b.tip = L"choose media files (Ctrl+O)";
        Item& pi = add(K_Icon, RECT{bar.left, bar.top, bar.left + ic, bar.bottom}, L"", 0);
        pi.value = I_Plus;
        int ty = bar.top + (bh - rh) / 2;
        int x = text(bar.left + ic, ty, L"Add files", S_Bold) + S(12);
        text(x, ty, tab_ == 1 ? L"or drop them on the window: video and audio, one by one or a whole folder"
                              : L"or drop them on the window: their tracks are copied into MP4 or MOV",
             S_Mid, bar.right - x - S(16));
    }
    add(K_Frame, bar);
    y = bar.bottom + S(14);
    if (tab_ == 0) buildDownloadOptions(y, a);
    else if (tab_ == 1) buildConvertOptions(y, a);
    else buildRemuxOptions(y, a);
    y += S(8);
    hline(a.left, a.right, y);
    y += S(10);

    // The list head: what is in the list, then the bulk actions at the top right.
    std::vector<ListRow> rows = collectRows(tab_);
    rowIds_.clear();
    for (auto& r : rows) rowIds_.push_back(r.id);
    int ready = 0, run = 0, wait = 0, paused = 0, failed = 0, done = 0, reading = 0;
    double est = 0;
    DlPrefs prefs = dlPrefs(view_);
    for (auto& r : rows) {
        if (r.type == ListRow::Task) {
            switch (r.task->state) {
                case TaskState::Running: run++; break;
                case TaskState::Waiting: wait++; break;
                case TaskState::Paused: paused++; break;
                case TaskState::Failed: failed++; break;
                default: done++;
            }
        } else if (r.type == ListRow::Pending && tab_ == 0) {
            DlGroup& g = groups_[r.idx];
            if (g.busy) reading++;
            else if (!g.error.empty()) failed++;
            else if (!g.playlist) {
                ready++;
                Selection sl = selectFormats(g.info, prefs);
                if (sl.ok) est += sl.estBytes;
            } else if (g.checked[r.entry]) {
                ready++;
            }
        } else if (r.type == ListRow::Pending) {
            const FileRow& f = (tab_ == 2 ? remux_ : conv_)[r.idx];
            if (f.busy) reading++;
            else if (fileReady(tab_, f, view_)) ready++;
            else if (!f.error.empty()) failed++;
        }
    }
    int x = sectionTitle(a.left, y, tab_ == 0 ? L"LINKS" : L"FILES") + S(16);
    std::wstring sum;
    auto part = [&](int n, const std::wstring& what) {
        if (n) sum += (sum.empty() ? L"" : L"  \u00b7  ") + std::to_wstring(n) + L" " + what;
    };
    part(reading, L"reading");
    part(ready, L"ready");
    if (est > 0) sum += L" (~" + fmtBytes(est) + L")";
    part(run, L"running");
    part(wait, L"waiting");
    part(paused, L"paused");
    part(failed, L"failed");
    part(done, L"finished");
    int rx = a.right;
    std::wstring go = ready ? L"start all  " + std::to_wstring(ready) : L"start all";
    int gw = tw(go, S_Bold) + S(32);
    rx -= gw;
    Item& gb = add(K_Button, RECT{rx, y + S(3), rx + gw, y + rh - S(3)}, go, S_Bold | (ready ? S_Invert | S_Click : S_Disabled));
    if (ready) gb.click = [this] { startAll(tab_); };
    gb.tip = L"start every item that is ready (Ctrl+Enter)";
    rx -= S(10);
    auto rb = [&](const std::wstring& l, const std::wstring& tip, std::function<void()> fn) {
        rx -= tw(l) + S(20);
        button(rx, y, l, std::move(fn));
        items_.back().tip = tip;
        rx -= S(10);
    };
    int kind = tab_;
    if (paused) rb(L"resume all", L"resume every paused task here", [this, kind] { queue_.resumeAll(kind); });
    else if (run || wait) rb(L"pause all", L"pause every running and waiting task here", [this, kind] { queue_.pauseAll(kind); });
    if (done) rb(L"clear finished", L"remove finished and canceled tasks from the list (the files stay)", [this, kind] { queue_.clearFinished(kind); });
    text(x, y, sum, S_Mid, rx - x - S(12));
    y += rh + S(6);

    RECT list{a.left, y, a.right, a.bottom};
    listView_ = list;
    hline(a.left, a.right, list.top);
    if (rows.empty()) {
        if (tab_ == 0) emptyState(list, L"Paste a link above", L"Every link on the clipboard is read at once. Ctrl+V works anywhere on this page.");
        else emptyState(list, L"Drop files here", L"or click the bar above (Ctrl+O). Each file waits here until you start it.");
        return;
    }
    // The playlist that holds the range box: the chosen one if it is still here, else the first.
    if (tab_ == 0) {
        bool found = false;
        uint64_t first = 0;
        for (auto& g : groups_)
            if (g.playlist && !g.busy && g.error.empty()) {
                found = found || g.key == rangeKey_;
                if (!first) first = g.key;
            }
        if (!found && first) {
            rangeKey_ = first;
            for (auto& g : groups_)
                if (g.key == first) SetWindowTextW(edits_[E_RANGE], rangesText(g).c_str());
        }
    }
    int rowRight = a.right - S(10);  // room for the scroll mark
    int viewH = list.bottom - list.top - 1;
    size_t mark = items_.size();
    // Rows outside the view are skipped (only their height counts), except the selected one, whose
    // details decide its height.
    auto pass = [&](int scroll, int& selTop, int& selBottom) {
        for (Edit e : {E_SEGA, E_SEGB, E_RANGE}) editPlaced_[e] = false;
        beginClip(RECT{list.left, list.top + 1, list.right, list.bottom});
        int ry = list.top + 1 - scroll;
        selTop = selBottom = INT32_MIN;
        for (auto& r : rows) {
            bool sel = r.id == sel_[tab_];
            int h = r.type == ListRow::Head ? rh + S(14) : itemH();
            if (sel || (ry + h > list.top && ry < list.bottom)) h = buildRow(r, list.left, rowRight, ry, sel);
            if (sel) {
                selTop = ry - list.top - 1 + scroll;
                selBottom = selTop + h;
            }
            ry += h;
        }
        endClip();
        return ry + scroll - list.top - 1;
    };
    int selTop, selBottom;
    int total = pass(listScroll_[tab_], selTop, selBottom);
    int scroll = std::clamp(listScroll_[tab_], 0, std::max(0, total - viewH));
    if (reveal_ && selTop != INT32_MIN) {
        if (selBottom > scroll + viewH) scroll = selBottom - viewH;
        if (selTop < scroll) scroll = selTop;
        scroll = std::clamp(scroll, 0, std::max(0, total - viewH));
    }
    reveal_ = false;
    if (scroll != listScroll_[tab_]) {
        items_.resize(mark);
        listScroll_[tab_] = scroll;
        total = pass(scroll, selTop, selBottom);
    }
    region(list, [this, total, viewH](int n) { scrollBy(listScroll_[tab_], n * rowH() * 3, total - viewH); });
    scrollbar(RECT{list.left, list.top + 1, list.right, list.bottom}, scroll, total);
}

void MainWindow::buildDownloadOptions(int& y, const RECT& a) {
    const Settings& s = view_;
    int rh = rowH();
    // Video and audio-only outputs are two separate rows so an audio-only choice is unmistakable;
    // the label of the active row is in full ink.
    bool audio = dlOutIsAudio(s.dlOut);
    int x = fieldLabel(a.left, y, L"VIDEO", audio ? S_Mid : 0);
    x = seg(x, y, {L"MP4", L"MOV", L"Original"}, audio ? -1 : (int)s.dlOut, [this](int i) {
        updateSetting([i](Settings& st) { st.dlOut = (DlOut)i; });
        relayout();
    });
    if (s.dlOut == DlOut::MP4 || s.dlOut == DlOut::MOV) {
        x = seg(x, y, {L"H.264", L"H.265"}, s.dlHevc ? 1 : 0, [this](int i) {
            updateSetting([i](Settings& st) { st.dlHevc = i == 1; });
            relayout();
        });
        x = text(x, y, L"QUALITY", S_Mid) + S(10);
        int qi = s.dlCrf <= 12 ? 0 : s.dlCrf <= 14 ? 1 : 2;
        x = seg(x, y, {L"extreme", L"high", L"normal"}, qi, [this](int i) {
            const int crf[] = {12, 14, 16};
            updateSetting([&](Settings& st) { st.dlCrf = crf[i]; });
            relayout();
        });
    }
    y += rh;
    x = fieldLabel(a.left, y, L"AUDIO ONLY", audio ? 0 : S_Mid);
    seg(x, y, {L"MP3 320", L"WAV", L"Original"}, audio ? (int)s.dlOut - (int)DlOut::AudioMP3 : -1, [this](int i) {
        updateSetting([i](Settings& st) { st.dlOut = (DlOut)((int)DlOut::AudioMP3 + i); });
        relayout();
    });
    y += rh;
    x = fieldLabel(a.left, y, L"SAVE TO");
    int pathMax = std::max<int>(S(200), (a.right - x) * 2 / 5);
    x = choice(x, y, L"", s.dlDir, [this] {
        int r = popup({L"open folder", L"choose folder..."}, -1);
        if (r == 0) openFolder(settingsCopy().dlDir);
        else if (r == 1) {
            std::wstring d = pickFolder(settingsCopy().dlDir);
            if (!d.empty()) updateSetting([&](Settings& st) { st.dlDir = d; });
        }
        relayout();
    }, nullptr, pathMax);
    int togglesW = 0;
    for (auto* l : {L"metadata", L"chapters", L"thumbnail", L"subtitles"}) togglesW += checkBox() + S(7) + tw(l) + S(16);
    if (x + togglesW > a.right) {
        y += rh;
        x = a.left + labelW();
    }
    x = toggle(x, y, L"metadata", s.metadata, [this] { updateSetting([](Settings& st) { st.metadata = !st.metadata; }); relayout(); });
    x = toggle(x, y, L"chapters", s.chapters, [this] { updateSetting([](Settings& st) { st.chapters = !st.chapters; }); relayout(); });
    x = toggle(x, y, L"thumbnail", s.thumbnail, [this] { updateSetting([](Settings& st) { st.thumbnail = !st.thumbnail; }); relayout(); });
    toggle(x, y, L"subtitles", s.subs, [this] { updateSetting([](Settings& st) { st.subs = !st.subs; }); relayout(); });
    y += rh;
}

void MainWindow::buildConvertOptions(int& y, const RECT& a) {
    const Settings& s = view_;
    const ConvertSpec& cs = s.convert;
    int rh = rowH();
    auto change = [this](const std::function<void(ConvertSpec&)>& f) {
        updateSetting([&](Settings& st) {
            f(st.convert);
            st.convertPreset = Preset::Custom;
        });
        customPreset_ = -1;
        relayout();
    };
    // PRESET: the four built-in presets, then one dropdown for saved presets (pick, save, delete).
    int x = fieldLabel(a.left, y, L"PRESET");
    int ps = s.convertPreset == Preset::Custom ? -1 : (int)s.convertPreset;
    x = seg(x, y, {L"lossless", L"max quality", L"balanced", L"compact"}, ps, [this](int i) {
        updateSetting([i](Settings& st) {
            applyPreset(st.convert, (Preset)i);
            st.convertPreset = (Preset)i;
        });
        customPreset_ = -1;
        relayout();
    });
    bool savedOn = customPreset_ >= 0 && customPreset_ < (int)presets_.size();
    std::wstring mine = savedOn ? presets_[customPreset_].name : s.convertPreset == Preset::Custom ? L"custom" : L"my presets";
    choice(x, y, L"", mine, [this] {
        RECT at = anchor_;
        std::vector<std::wstring> o;
        int first = presets_.empty() ? 0 : 1, n = (int)presets_.size();
        if (n) o.push_back(L"#SAVED");
        for (auto& p : presets_) o.push_back(p.name);
        if (n) o.push_back(L"-");
        int saveAt = (int)o.size();
        o.push_back(L"save current settings...");
        int delStart = -1;
        if (n) {
            o.push_back(L"#DELETE");
            delStart = (int)o.size();
            for (auto& p : presets_) o.push_back(L"delete " + p.name);
        }
        int r = popup(o, customPreset_ >= 0 ? customPreset_ + first : -1);
        if (r < 0) return;
        if (n && r >= first && r < first + n) {
            ConvertSpec sp = presets_[r - first].spec;
            updateSetting([&](Settings& st) {
                st.convert = sp;
                st.convertPreset = Preset::Custom;
            });
            customPreset_ = r - first;
        } else if (r == saveAt) {
            std::wstring init = customPreset_ >= 0 && customPreset_ < n ? presets_[customPreset_].name : L"my preset";
            customValue(init, at, [this](const std::wstring& name) {
                if (name.empty()) return;
                Settings st = settingsCopy();
                bool replaced = false;
                for (auto& p : presets_)
                    if (p.name == name) {
                        p.spec = st.convert;
                        replaced = true;
                    }
                if (!replaced) presets_.push_back({name, st.convert});
                savePresets(presets_);
                for (size_t i = 0; i < presets_.size(); i++)
                    if (presets_[i].name == name) customPreset_ = (int)i;
                flash(L"preset saved: " + name, 3000);
            });
        } else if (delStart >= 0 && r >= delStart) {
            std::wstring name = presets_[r - delStart].name;
            presets_.erase(presets_.begin() + (r - delStart));
            savePresets(presets_);
            customPreset_ = -1;
            flash(L"preset deleted: " + name, 3000);
        }
        relayout();
    });
    if (customDone_) placeEdit(E_CUSTOM, editRects_[E_CUSTOM]);
    y += rh;
    // TARGET: everything about the output in one row, the same dropdown control throughout; it wraps
    // to the control column on narrow windows. The encoder is automatic (Settings can pin one).
    EncoderCaps caps;
    {
        std::lock_guard<std::mutex> lk(capsMu_);
        caps = caps_;
    }
    bool audioTarget = cs.audio != AudioTarget::None;
    bool encodesVideo = !audioTarget && cs.vcodec != VCodec::Copy;
    int ti = targetIndex(cs);
    int left = a.left + labelW();
    x = fieldLabel(a.left, y, L"TARGET");
    field(x, y, left, a.right, L"", kTargets[ti], [this, ti] {
        // Grouped: video, intermediate, copy, audio only.
        std::vector<std::wstring> o;
        std::vector<int> map;
        const std::pair<int, const wchar_t*> groups[] = {{0, L"#VIDEO"}, {4, L"#INTERMEDIATE (DNxHR)"}, {9, L"#COPY VIDEO"}, {11, L"#AUDIO ONLY"}};
        for (int i = 0; i < (int)kTargets.size(); i++) {
            for (auto& g : groups)
                if (g.first == i) {
                    o.push_back(g.second);
                    map.push_back(-1);
                }
            o.push_back(kTargets[i]);
            map.push_back(i);
        }
        int checked = (int)(std::find(map.begin(), map.end(), ti) - map.begin());
        int r = popup(o, checked);
        if (r < 0 || map[r] < 0) return;
        int t = map[r];
        updateSetting([t](Settings& st) {
            setTarget(st.convert, t);
            st.convertPreset = Preset::Custom;
        });
        customPreset_ = -1;
        relayout();
    }, [this, ti](int n) {
        int r = std::clamp(ti - n, 0, (int)kTargets.size() - 1);
        updateSetting([r](Settings& st) {
            setTarget(st.convert, r);
            st.convertPreset = Preset::Custom;
        });
        customPreset_ = -1;
        relayout();
    });
    if (encodesVideo && !isDnxhr(cs.vcodec)) {
        field(x, y, left, a.right, L"QUALITY", qualityText(cs), [this, change] {
            RECT at = anchor_;
            std::vector<std::wstring> o = {L"lossless", L"CRF 16 (very high)", L"CRF 18", L"CRF 20", L"CRF 22 (balanced)", L"CRF 24", L"CRF 26", L"CRF 28 (compact)",
                                           L"CRF 30", L"-", L"4 Mb/s", L"8 Mb/s", L"12 Mb/s", L"20 Mb/s", L"35 Mb/s", L"50 Mb/s", L"-", L"custom CRF...", L"custom Mb/s..."};
            ConvertSpec cur = settingsCopy().convert;
            int checked = -1;
            const int crf[] = {0, 16, 18, 20, 22, 24, 26, 28, 30};
            const double mb[] = {4, 8, 12, 20, 35, 50};
            if (cur.rate == RateMode::Lossless) checked = 0;
            for (int i = 1; i <= 8; i++)
                if (cur.rate == RateMode::Quality && cur.quality == crf[i]) checked = i;
            for (int i = 0; i < 6; i++)
                if (cur.rate == RateMode::Bitrate && std::fabs(cur.mbps - mb[i]) < 0.01) checked = 10 + i;
            int r = popup(o, checked);
            if (r < 0) return;
            if (r == 0) change([](ConvertSpec& c) { c.rate = RateMode::Lossless; });
            else if (r <= 8) change([&](ConvertSpec& c) { c.rate = RateMode::Quality; c.quality = crf[r]; });
            else if (r >= 10 && r <= 15) change([&](ConvertSpec& c) { c.rate = RateMode::Bitrate; c.mbps = mb[r - 10]; });
            else if (r == 17 || r == 18) {
                bool isCrf = r == 17;
                std::wstring init = isCrf ? std::to_wstring(cur.quality) : fmtFixed(cur.mbps, 1);
                customValue(init, at, [isCrf, change](const std::wstring& v) {
                    double d = _wtof(v.c_str());
                    if (isCrf && d >= 0 && d <= 51) change([&](ConvertSpec& c) { c.rate = RateMode::Quality; c.quality = (int)d; });
                    else if (!isCrf && d > 0.1 && d < 2000) change([&](ConvertSpec& c) { c.rate = RateMode::Bitrate; c.mbps = d; });
                });
            }
        }, [change](int n) {
            change([n](ConvertSpec& c) {
                if (c.rate == RateMode::Quality) c.quality = std::clamp(c.quality - n, 0, 51);
                else if (c.rate == RateMode::Bitrate) c.mbps = std::clamp(c.mbps + n * (c.mbps >= 20 ? 5.0 : 1.0), 0.5, 500.0);
            });
        });
        field(x, y, left, a.right, L"SPEED", widen(cs.speed), [this, change] {
            std::vector<std::wstring> o = {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow", L"slower", L"veryslow"};
            int cur = 5;
            for (size_t i = 0; i < o.size(); i++)
                if (narrow(o[i]) == settingsCopy().convert.speed) cur = (int)i;
            int r = popup(o, cur);
            if (r >= 0) change([&](ConvertSpec& c) { c.speed = narrow(o[r]); });
        });
    }
    if (!audioTarget) {
        field(x, y, left, a.right, L"AUDIO", audioText(cs), [this, change] {
            std::vector<std::wstring> o = {L"copy (bit-for-bit)", L"-", L"AAC 128k", L"AAC 160k", L"AAC 192k", L"AAC 256k", L"AAC 320k", L"-",
                                           L"PCM exact (lossless)", L"PCM 16-bit", L"PCM 24-bit"};
            ConvertSpec cur = settingsCopy().convert;
            const int k[] = {128, 160, 192, 256, 320};
            int checked = cur.acodec == ACodec::Copy ? 0 : cur.acodec == ACodec::PCM ? (cur.pcmBits == 16 ? 9 : cur.pcmBits == 24 ? 10 : 8) : -1;
            for (int i = 0; i < 5; i++)
                if (cur.acodec == ACodec::AAC && cur.aacKbps == k[i]) checked = 2 + i;
            int r = popup(o, checked);
            if (r == 0) change([](ConvertSpec& c) { c.acodec = ACodec::Copy; });
            else if (r >= 2 && r <= 6) change([&](ConvertSpec& c) { c.acodec = ACodec::AAC; c.aacKbps = k[r - 2]; });
            else if (r >= 8) change([&](ConvertSpec& c) { c.acodec = ACodec::PCM; c.pcmBits = r == 8 ? 0 : r == 9 ? 16 : 24; });
        });
        if (encodesVideo)
            field(x, y, left, a.right, L"FILTERS", filtersText(cs), [this, change] {
                // One list, three sections: size, frame rate, pixel format (one check each).
                RECT at = anchor_;
                ConvertSpec cur = settingsCopy().convert;
                std::vector<std::wstring> o = {L"#SIZE", L"source", L"2160p", L"1440p", L"1080p", L"720p", L"480p", L"custom WxH...",
                                               L"#FRAME RATE", L"source", L"23.976", L"24", L"25", L"29.97", L"30", L"50", L"59.94", L"60",
                                               L"#PIXEL FORMAT", L"source", L"8-bit 4:2:0", L"10-bit 4:2:0", L"10-bit 4:2:2", L"10-bit 4:4:4"};
                const int hgt[] = {0, 2160, 1440, 1080, 720, 480};
                const double fps[] = {0, 24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60};
                const char* pix[] = {"", "yuv420p", "yuv420p10le", "yuv422p10le", "yuv444p10le"};
                std::vector<int> checks;
                int sizeCheck = cur.width ? 7 : -1;
                for (int i = 0; i < 6; i++)
                    if (cur.width == 0 && cur.height == hgt[i]) sizeCheck = 1 + i;
                checks.push_back(sizeCheck);
                for (int i = 0; i < 9; i++)
                    if (std::fabs(cur.fps - fps[i]) < 0.001) checks.push_back(9 + i);
                for (int i = 0; i < 5; i++)
                    if (cur.pixFmt == pix[i]) checks.push_back(19 + i);
                int r = popup(o, checks);
                if (r >= 1 && r <= 6) change([&](ConvertSpec& c) { c.height = hgt[r - 1]; c.width = 0; });
                else if (r == 7)
                    customValue(cur.width && cur.height ? std::to_wstring(cur.width) + L"x" + std::to_wstring(cur.height) : L"1920x1080", at,
                                [change](const std::wstring& v) {
                                    int w2 = 0, h2 = 0;
                                    if (swscanf(v.c_str(), L"%dx%d", &w2, &h2) == 2 && w2 > 0 && h2 > 0 && w2 <= 16384 && h2 <= 16384)
                                        change([&](ConvertSpec& c) { c.width = w2 & ~1; c.height = h2 & ~1; });
                                });
                else if (r >= 9 && r <= 17) change([&](ConvertSpec& c) { c.fps = fps[r - 9]; });
                else if (r >= 19 && r <= 23) change([&](ConvertSpec& c) { c.pixFmt = pix[r - 19]; });
            });
        if (isDnxhr(cs.vcodec) && a.right - x > S(160)) text(x, y, L"fixed rate per profile, visually lossless", S_Mid, a.right - x);
        else if (cs.vcodec == VCodec::Copy && a.right - x > S(160)) text(x, y, L"video copied bit-for-bit", S_Mid, a.right - x);
    } else {
        if (cs.audio == AudioTarget::FLAC)
            field(x, y, left, a.right, L"LEVEL", std::to_wstring(cs.flacLevel), [this, change] {
                std::vector<std::wstring> o;
                for (int i = 0; i <= 12; i++) o.push_back(std::to_wstring(i) + (i == 0 ? L" (fastest)" : i == 8 ? L" (default)" : i == 12 ? L" (smallest)" : L""));
                int r = popup(o, settingsCopy().convert.flacLevel);
                if (r >= 0) change([r](ConvertSpec& c) { c.flacLevel = r; });
            }, [change](int n) { change([n](ConvertSpec& c) { c.flacLevel = std::clamp(c.flacLevel + n, 0, 12); }); });
        if (cs.audio == AudioTarget::MP3)
            field(x, y, left, a.right, L"BITRATE", cs.mp3Vbr ? L"VBR V" + std::to_wstring(cs.mp3Quality) : L"CBR " + std::to_wstring(cs.mp3Kbps) + L"k", [this, change] {
                std::vector<std::wstring> o = {L"#CONSTANT", L"CBR 320k (maximum)", L"CBR 256k", L"CBR 192k", L"CBR 128k",
                                               L"#VARIABLE", L"VBR V0 (~245k)", L"VBR V2 (~190k)", L"VBR V4 (~165k)"};
                const int k[] = {320, 256, 192, 128};
                const int v[] = {0, 2, 4};
                ConvertSpec cur = settingsCopy().convert;
                int checked = -1;
                for (int i = 0; i < 4; i++)
                    if (!cur.mp3Vbr && cur.mp3Kbps == k[i]) checked = 1 + i;
                for (int i = 0; i < 3; i++)
                    if (cur.mp3Vbr && cur.mp3Quality == v[i]) checked = 6 + i;
                int r = popup(o, checked);
                if (r >= 1 && r <= 4) change([&](ConvertSpec& c) { c.mp3Vbr = false; c.mp3Kbps = k[r - 1]; });
                else if (r >= 6) change([&](ConvertSpec& c) { c.mp3Vbr = true; c.mp3Quality = v[r - 6]; });
            });
        if (cs.audio != AudioTarget::MP3)
            field(x, y, left, a.right, L"BITS", cs.bitDepth ? std::to_wstring(cs.bitDepth) + (cs.bitDepth == 32 ? L" float" : L"") : L"source (exact)", [this, change] {
                ConvertSpec cur = settingsCopy().convert;
                std::vector<std::wstring> o = {L"source (exact)", L"16", L"24"};
                if (cur.audio == AudioTarget::WAV) o.push_back(L"32 float");
                const int b[] = {0, 16, 24, 32};
                int checked = -1;
                for (int i = 0; i < (int)o.size(); i++)
                    if (cur.bitDepth == b[i]) checked = i;
                int r = popup(o, checked);
                if (r >= 0) change([&](ConvertSpec& c) { c.bitDepth = b[r]; });
            });
        field(x, y, left, a.right, L"RATE", cs.sampleRate ? std::to_wstring(cs.sampleRate) : L"source", [this, change] {
            std::vector<std::wstring> o = {L"source", L"44100", L"48000", L"96000"};
            const int r2[] = {0, 44100, 48000, 96000};
            int cur = settingsCopy().convert.sampleRate, checked = -1;
            for (int i = 0; i < 4; i++)
                if (cur == r2[i]) checked = i;
            int r = popup(o, checked);
            if (r >= 0) change([&](ConvertSpec& c) { c.sampleRate = r2[r]; });
        });
        field(x, y, left, a.right, L"CHANNELS", cs.channels ? (cs.channels == 1 ? L"mono" : cs.channels == 2 ? L"stereo" : std::to_wstring(cs.channels)) : L"source",
              [this, change] {
                  std::vector<std::wstring> o = {L"source", L"stereo", L"mono"};
                  const int c2[] = {0, 2, 1};
                  int cur = settingsCopy().convert.channels, checked = -1;
                  for (int i = 0; i < 3; i++)
                      if (cur == c2[i]) checked = i;
                  int r = popup(o, checked);
                  if (r >= 0) change([&](ConvertSpec& c) { c.channels = c2[r]; });
              });
    }
    y += rh;
    // SAVE TO, with the stream options on the same row (as on Download), and the encoder at the right.
    x = fieldLabel(a.left, y, L"SAVE TO");
    x = choice(x, y, L"", s.convertDir.empty() ? L"next to the source" : s.convertDir, [this] {
        int r = popup({L"next to the source", L"choose folder..."}, settingsCopy().convertDir.empty() ? 0 : 1);
        if (r == 0) updateSetting([](Settings& st) { st.convertDir.clear(); });
        else if (r == 1) {
            std::wstring d = pickFolder(settingsCopy().convertDir);
            if (!d.empty()) updateSetting([&](Settings& st) { st.convertDir = d; });
        }
        relayout();
    }, nullptr, (a.right - x) / 3);
    if (!audioTarget) {
        x = toggle(x, y, L"subtitles", cs.keepSubs, [change] { change([](ConvertSpec& c) { c.keepSubs = !c.keepSubs; }); });
        if (cs.container != Container::MKV) x = toggle(x, y, L"faststart", cs.faststart, [change] { change([](ConvertSpec& c) { c.faststart = !c.faststart; }); });
    }
    if (encodesVideo) {
        std::string enc = pickEncoder(cs.vcodec, s.encoder, cs.rate, caps);
        std::wstring ev = encoderLabel(enc) + (s.encoder == Accel::Auto ? L" (fastest here)" : L" (set in Settings)");
        if (!caps.probed && capsRunning_) ev = encoderLabel(enc) + L" (looking for graphics encoders...)";
        int capW = tw(L"ENCODER", S_Mid) + S(10), ew = tw(ev);
        int ex = a.right - ew;
        if (ex - capW > x) {
            text(ex - capW, y, L"ENCODER", S_Mid);
            Item& ei = add(K_Text, RECT{ex, y, a.right, y + rh}, ev, S_Click);
            ei.tip = L"choose the encoder in Settings";
            ei.click = [this] {
                settingsScroll_ = 0;
                setOverlay(Overlay::Settings);
            };
        }
    }
    y += rh;
}

void MainWindow::buildRemuxOptions(int& y, const RECT& a) {
    const Settings& s = view_;
    int rh = rowH();
    // One row, as on Convert: container, then audio kept as it is or decoded to linear PCM
    // (exact by default; a fixed depth is optional).
    int x = fieldLabel(a.left, y, L"CONTAINER");
    x = seg(x, y, {L"MP4", L"MOV"}, s.remuxContainer == Container::MOV ? 1 : 0, [this](int i) {
        updateSetting([i](Settings& st) { st.remuxContainer = i == 0 ? Container::MP4 : Container::MOV; });
        for (auto& f : remux_) f.remux.actions.clear();  // defaults depend on the container
        relayout();
    });
    x = text(x, y, L"AUDIO", S_Mid) + S(10);
    x = seg(x, y, {L"Original", L"Linear PCM"}, s.remuxPcm ? 1 : 0, [this](int i) {
        updateSetting([i](Settings& st) { st.remuxPcm = i == 1; });
        for (auto& f : remux_)
            for (auto it = f.remux.actions.begin(); it != f.remux.actions.end();) {
                bool audioTrack = false;
                for (auto& st : f.info.streams) audioTrack = audioTrack || (st.index == it->first && st.isAudio());
                it = audioTrack ? f.remux.actions.erase(it) : std::next(it);
            }
        relayout();
    });
    std::wstring note = s.remuxPcm ? L"every audio track is decoded to PCM" : L"audio copied bit-for-bit; PCM only where the container cannot hold a track";
    if (s.remuxPcm) {
        field(x, y, a.left + labelW(), a.right, L"DEPTH", std::wstring(pcmBitsName(s.remuxPcmBits)) + (s.remuxPcmBits == 0 ? L" (lossless)" : L""), [this] {
            std::vector<std::wstring> o = {L"exact (lossless: source depth, float for lossy sources)", L"16-bit", L"24-bit", L"32-bit float"};
            const int b[] = {0, 16, 24, 32};
            int cur = settingsCopy().remuxPcmBits, checked = 0;
            for (int i = 0; i < 4; i++)
                if (cur == b[i]) checked = i;
            int r = popup(o, checked);
            if (r >= 0) updateSetting([&](Settings& st) { st.remuxPcmBits = b[r]; });
            relayout();
        });
    }
    if (a.right - x > S(120)) text(x, y, note, S_Mid, a.right - x);
    y += rh;
    x = fieldLabel(a.left, y, L"SAVE TO");
    choice(x, y, L"", s.remuxDir.empty() ? L"next to the source" : s.remuxDir, [this] {
        int r = popup({L"next to the source", L"choose folder..."}, settingsCopy().remuxDir.empty() ? 0 : 1);
        if (r == 0) updateSetting([](Settings& st) { st.remuxDir.clear(); });
        else if (r == 1) {
            std::wstring d = pickFolder(settingsCopy().remuxDir);
            if (!d.empty()) updateSetting([&](Settings& st) { st.remuxDir = d; });
        }
        relayout();
    }, nullptr, (a.right - x) / 2);
    y += rh;
}

// ---------- list rows ----------

int MainWindow::buildRow(const ListRow& r, int left, int right, int y, bool selected) {
    int rh = rowH();
    int is = S(36), gap = S(2);
    RowId id = r.id;
    auto act = [this, id](void (MainWindow::*fn)(const ListRow&)) {
        return [this, id, fn] { withRow(id, [this, fn](const ListRow& x) { (this->*fn)(x); }); };
    };

    if (r.type == ListRow::Head) {  // a playlist: its entries follow
        DlGroup& g = groups_[r.idx];
        int h = rh + S(14);
        Item& bg = add(K_Text, RECT{left, y, right, y + h - 1}, L"", S_Click | (selected ? S_Selected : 0));
        bg.click = [this, id] {
            select(0, sel_[0] == id ? RowId{} : id);
            relayout();
        };
        int nSel = 0;
        for (char c : g.checked) nSel += c;
        int iy = y + (h - is) / 2;
        int ix = right - S(4);
        ix = rowIcon(ix, iy, is, I_Close, L"remove the playlist (started entries stay)", act(&MainWindow::removeRow)) - gap;
        ix = rowIcon(ix, iy, is, I_Folder, L"open the download folder", [this] { openFolder(settingsCopy().dlDir); }) - gap;
        ix = rowIcon(ix, iy, is, I_Open, L"open the playlist page", [this, u = g.url] { openUrl(u); }) - gap;
        ix = rowIcon(ix, iy, is, I_Play, nSel ? L"start the checked entries" : L"", nSel ? std::function<void()>(act(&MainWindow::startRow)) : nullptr);
        int ty = y + (h - rh) / 2;
        int x = text(left + S(8), ty, L"PLAYLIST", S_Bold) + S(14);
        uint64_t key = g.key;
        x = button(x, ty, L"all", [this, key] {
            for (auto& gg : groups_)
                if (gg.key == key) std::fill(gg.checked.begin(), gg.checked.end(), 1);
            syncRange(key);
            relayout();
        });
        x = button(x, ty, L"none", [this, key] {
            for (auto& gg : groups_)
                if (gg.key == key) std::fill(gg.checked.begin(), gg.checked.end(), 0);
            syncRange(key);
            relayout();
        });
        x = text(x + S(4), ty, L"RANGE", S_Mid) + S(10);
        int cw = fonts_.charW * 13;
        RECT er{x, ty + S(5), x + cw, ty + rh - S(5)};
        if (key == rangeKey_ && er.top >= listView_.top && er.bottom <= listView_.bottom) {
            placeEdit(E_RANGE, er);
        } else {
            Item& it = add(K_Text, RECT{x, ty, x + cw, ty + rh}, rangesText(g), S_Mid | S_Click);
            it.tip = L"type a range such as 1-5,8";
            it.click = [this, key] {
                rangeKey_ = key;  // the one range box moves to this playlist and shows its own selection
                for (auto& gg : groups_)
                    if (gg.key == key) SetWindowTextW(edits_[E_RANGE], rangesText(gg).c_str());
                relayout(true);
            };
        }
        x += cw + S(14);
        text(x, ty, std::to_wstring(nSel) + L"/" + std::to_wstring(g.entries.size()) + L"  " + g.title, S_Mid, ix - x - S(10));
        hline(left, right, y + h - 1);
        return h;
    }

    // A row: background (selects it), a thumbnail or extension tag, two lines, status and actions.
    std::wstring title, sub, status, chip, openTip, folderTip, closeTip = L"remove";
    unsigned titleSt = S_Bold, subSt = S_Mid | S_Mono, statusSt = S_Mid | S_Mono;
    std::string thumb;
    double prog = -2;  // -2: no bar
    int primaryIcon = -1;
    std::wstring primaryTip;
    std::function<void()> primary, open, folder;
    bool checkbox = false, checked = false;
    std::function<void()> onCheck;
    auto segText = [](const std::wstring& a, const std::wstring& b) {
        if (a.empty() && b.empty()) return std::wstring();
        return L"  \u00b7  segment " + (a.empty() ? L"0" : a) + L" - " + (b.empty() ? L"end" : b);
    };
    if (r.type == ListRow::Task) {
        const TaskView& t = *r.task;
        uint64_t tid = t.id;
        title = t.title;
        thumb = t.thumb;
        std::wstring pct = t.progress >= 0 ? std::to_wstring((int)(t.progress * 100)) + L"%" : L"";
        switch (t.state) {
            case TaskState::Running:
                status = pct;
                if (!t.speed.empty()) status += (status.empty() ? L"" : L"  ") + t.speed;
                if (t.eta >= 0) status += (status.empty() ? L"" : L"  ") + fmtDuration(t.eta);
                if (status.empty()) status = L"running";
                statusSt = S_Mono;
                sub = t.stage;
                prog = t.progress;
                primaryIcon = I_Pause;
                primaryTip = L"pause (Space)";
                primary = [this, tid] { queue_.pause(tid); };
                break;
            case TaskState::Waiting:
                status = L"waiting";
                sub = t.stage.empty() || t.stage == L"starting" ? L"starts when a slot is free (Settings: parallel tasks)" : t.stage;
                if (t.progress >= 0) prog = t.progress;
                primaryIcon = I_Pause;
                primaryTip = L"hold it (Space)";
                primary = [this, tid] { queue_.pause(tid); };
                break;
            case TaskState::Paused:
                status = L"paused" + (pct.empty() ? L"" : L" " + pct);
                sub = t.stage.empty() ? L"paused" : t.stage;
                if (t.progress >= 0) prog = t.progress;
                primaryIcon = I_Play;
                primaryTip = L"resume (Space)";
                primary = [this, tid] { queue_.resume(tid); };
                break;
            case TaskState::Failed:
            case TaskState::Canceled:
                status = t.state == TaskState::Failed ? L"failed" : L"canceled";
                statusSt = t.state == TaskState::Failed ? S_Bold | S_Mono : S_Mid | S_Mono;
                sub = t.state == TaskState::Failed ? t.error + (t.hint.empty() ? L"" : L"  \u2192 " + t.hint) : L"canceled";
                primaryIcon = I_Retry;
                primaryTip = L"try again (F5)";
                primary = [this, tid] { queue_.retry(tid); };
                break;
            case TaskState::Done:
                status = classTag(t.cls);
                statusSt = S_Mono | (t.cls == QualityClass::Lossy ? S_Bold : S_Mid);
                sub = t.result.empty() ? fileName(t.output) : t.result;
                break;
        }
        bool active = t.state == TaskState::Running || t.state == TaskState::Paused;
        closeTip = active ? L"cancel" : L"remove from the list (the file stays)";
        std::wstring out = t.output, src = t.source;
        bool doneOut = t.state == TaskState::Done && !out.empty();
        if (tab_ == 0) {
            openTip = L"open the link";
            open = [this, src] { openUrl(src); };
        } else {
            openTip = doneOut ? L"play the result" : L"play the source";
            open = [this, p = doneOut ? out : src] { openPath(p); };
            chip = extTag(doneOut ? out : src);
        }
        if (tab_ == 0 && thumb.empty()) chip = extTag(out).empty() ? L"LINK" : extTag(out);  // tasks from before 1.0.3 kept no thumbnail
        std::wstring dir = doneOut ? out : !t.outDir.empty() ? t.outDir : tab_ == 0 ? view_.dlDir : src;
        folderTip = doneOut ? L"show the result in its folder" : L"open the output folder";
        folder = [this, dir] { openFolder(dir); };
    } else if (tab_ == 0) {
        DlGroup& g = groups_[r.idx];
        std::wstring url = g.url;
        if (g.busy) {
            title = url;
            titleSt = 0;
            sub = L"reading the link...";
        } else if (!g.error.empty()) {
            title = url;
            titleSt = 0;
            sub = g.error + (g.hint.empty() ? L"" : L"  \u2192 " + g.hint);
            subSt = S_Bold;
            status = L"failed";
            statusSt = S_Bold | S_Mono;
            primaryIcon = I_Retry;
            primaryTip = L"read the link again";
            int gi = r.idx;
            primary = [this, gi, url] {
                removeGroup(gi);
                analyzeText(url);
            };
        } else if (!g.playlist) {
            title = g.title;
            thumb = g.info.thumb;
            Selection sl = selectFormats(g.info, dlPrefs(view_));
            sub = fmtDuration(g.info.duration) + L"  \u00b7  " + (sl.ok ? sl.summary : L"! " + sl.error) + segText(g.segA[0], g.segB[0]);
            status = sl.ok && sl.estBytes > 0 ? L"~" + fmtBytes(sl.estBytes) : L"ready";
            primaryIcon = I_Play;
            primaryTip = L"start this download (Space)";
            primary = act(&MainWindow::startRow);
        } else {
            const YtEntry& e = g.entries[r.entry];
            url = widen(e.url);
            title = widen(e.title);
            thumb = e.thumb;
            checkbox = true;
            checked = g.checked[r.entry] != 0;
            uint64_t key = g.key;
            int ei = r.entry;
            onCheck = [this, key, ei] {
                for (auto& gg : groups_)
                    if (gg.key == key && ei < (int)gg.checked.size()) gg.checked[ei] = !gg.checked[ei];
                syncRange(key);
                relayout();
            };
            if (!checked) titleSt = 0;
            sub = std::to_wstring(e.index) + L".  " + fmtDuration(e.duration) + segText(g.segA[r.entry], g.segB[r.entry]);
            status = checked ? L"ready" : L"skipped";
            primaryIcon = I_Play;
            primaryTip = L"start this entry now";
            primary = act(&MainWindow::startRow);
        }
        openTip = L"open the link";
        open = [this, url] { openUrl(url); };
        folderTip = L"open the download folder";
        folder = [this] { openFolder(settingsCopy().dlDir); };
    } else {
        const FileRow& f = (tab_ == 2 ? remux_ : conv_)[r.idx];
        title = fileName(f.path);
        chip = extTag(f.path);
        bool ready = fileReady(tab_, f, view_);
        if (f.busy) {
            sub = L"reading the file...";
        } else if (!f.error.empty()) {
            sub = L"! " + f.error;
            subSt = S_Bold;
            status = L"cannot read";
            statusSt = S_Bold | S_Mono;
        } else {
            sub = shortMedia(f.info);
            status = ready ? L"ready" : L"see tracks";
        }
        if (ready) {
            primaryIcon = I_Play;
            primaryTip = tab_ == 1 ? L"convert this file (Space)" : L"remux this file (Space)";
            primary = act(&MainWindow::startRow);
        }
        std::wstring p = f.path;
        openTip = L"play the file";
        open = [this, p] { openPath(p); };
        folderTip = L"show the file in its folder";
        folder = [this, p] { openFolder(p); };
    }

    int h = itemH();
    size_t bgIdx = items_.size();
    Item& bg = add(K_Text, RECT{left, y, right, y + h - 1}, L"", S_Click | (selected ? S_Selected : 0));
    bg.click = [this, id] {
        select(tab_, sel_[tab_] == id ? RowId{} : id);  // a second click folds the details away
        relayout();
    };
    int iy = y + (h - is) / 2;
    int ix = right - S(4);
    ix = rowIcon(ix, iy, is, I_Close, closeTip, act(&MainWindow::removeRow)) - gap;
    ix = rowIcon(ix, iy, is, I_Folder, folderTip, folder) - gap;
    ix = rowIcon(ix, iy, is, I_Open, openTip, open) - gap;
    if (primaryIcon >= 0) rowIcon(ix, iy, is, primaryIcon, primaryTip, primary);
    ix -= is;  // the column stays even when there is nothing to start
    int textRight = ix - S(16);
    int x = left + S(8) + (r.sub > 0 ? S(24) : 0);  // playlist entries sit under their head
    if (checkbox) {
        check(x, y + (h - rh) / 2, checked, onCheck);
        items_.back().tip = checked ? L"skip this entry in start all" : L"include this entry in start all";
    }
    if (r.sub > 0) x += S(28);
    int vw = S(96), vh = S(54);
    RECT vis{x, y + (h - vh) / 2, x + vw, y + (h - vh) / 2 + vh};
    if (tab_ == 0 && (!thumb.empty() || r.type != ListRow::Task)) {
        Item& th = add(K_Thumb, vis);
        th.bmp = thumb.empty() ? nullptr : thumbs_.get(thumb);
    } else {
        add(K_Chip, vis, chip.empty() ? L"?" : chip);
    }
    int tx = vis.right + S(14);
    int line1 = y + (h - rh * 2) / 2 + S(1);
    int sw = status.empty() ? 0 : tw(status, statusSt);
    add(K_Text, RECT{textRight - sw, line1, textRight, line1 + rh}, status, statusSt);
    text(tx, line1, title, titleSt, textRight - sw - S(16) - tx);
    add(K_Text, RECT{tx, line1 + rh - S(3), textRight, line1 + rh * 2 - S(3)}, sub, subSt);
    if (prog > -2) {
        Item& b = add(K_Progress, RECT{tx, y + h - S(7), textRight, y + h - S(7) + std::max(2, S(3))}, L"", S_Mid);
        b.value = prog;
    }
    if (selected) h += buildRowDetail(r, tx, right - S(8), y + h);
    items_[bgIdx].r.bottom = y + h - 1;
    hline(left, right, y + h - 1);
    return h;
}

// The selected row's details: what will happen (plan, tracks, segment) before it starts, what
// happened (result, error, output, log) after.
int MainWindow::buildRowDetail(const ListRow& r, int left, int right, int y0) {
    int rh = rowH(), dh = denseH();
    int y = y0;
    auto inView = [&](const RECT& e) { return e.top >= listView_.top && e.bottom <= listView_.bottom; };
    if (r.type == ListRow::Task) {
        const TaskView& t = *r.task;
        if (t.state == TaskState::Failed) {  // the whole error (the row shows only its start)
            add(K_Text, RECT{left, y, right, y + dh}, t.error, S_Bold | S_Mono);
            y += dh;
            if (!t.hint.empty()) {
                add(K_Text, RECT{left, y, right, y + dh}, t.hint, S_Mono);
                y += dh;
            }
        }
        if (!t.source.empty()) {
            add(K_Text, RECT{left, y, right, y + dh}, (t.kind == TaskKind::Download ? L"from  " : L"source  ") + t.source, S_Mid | S_Mono);
            y += dh;
        }
        if (!t.output.empty()) {
            add(K_Text, RECT{left, y, right, y + dh}, L"to  " + t.output, S_Mid | S_Mono);
            y += dh;
        }
        y += S(4);
        int x = button(left, y, L"log", [this, tid = t.id] {
            selTask_ = tid;
            logApp_ = false;
            setOverlay(Overlay::Log);
        });
        if (t.state == TaskState::Done && !t.output.empty()) {
            std::wstring out = t.output;
            x = button(x, y, L"play", [this, out] { openPath(out); });
            if (t.kind != TaskKind::Remux) x = button(x, y, L"convert", [this, out] { addFiles({out}, 1); });
            button(x, y, L"remux", [this, out] { addFiles({out}, 2); });
        } else if (t.kind == TaskKind::Download && !t.source.empty()) {
            button(x, y, L"copy link", [this, src = t.source] {
                copyText(src);
                flash(L"link copied", 2000);
            });
        }
        return y + rh + S(8) - y0;
    }
    if (tab_ == 0) {
        DlGroup& g = groups_[r.idx];
        if (g.busy) return 0;
        if (!g.error.empty()) {
            add(K_Text, RECT{left, y, right, y + dh}, g.error, S_Bold | S_Mono);
            y += dh;
            if (!g.hint.empty()) {
                add(K_Text, RECT{left, y, right, y + dh}, g.hint, S_Mid | S_Mono);
                y += dh;
            }
            return y + S(8) - y0;
        }
        // A segment of this video (or entry); empty boxes download the whole thing.
        int cw = fonts_.charW * 11;
        int x = text(left, y, L"SEGMENT  from", S_Mid) + S(10);
        RECT ea{x, y + S(5), x + cw, y + rh - S(5)};
        if (inView(ea)) placeEdit(E_SEGA, ea);
        x += cw + S(14);
        x = text(x, y, L"to", S_Mid) + S(10);
        RECT eb{x, y + S(5), x + cw, y + rh - S(5)};
        if (inView(eb)) placeEdit(E_SEGB, eb);
        x += cw + S(14);
        text(x, y, L"m:ss or seconds; empty = the whole video", S_Mid, right - x);
        y += rh;
        std::wstring url = g.playlist ? widen(g.entries[r.entry].url) : g.url;
        add(K_Text, RECT{left, y, right, y + dh}, url, S_Mid | S_Mono);
        y += dh;
        if (!g.playlist && !g.info.uploader.empty()) {
            add(K_Text, RECT{left, y, right, y + dh}, L"by " + widen(g.info.uploader), S_Mid | S_Mono);
            y += dh;
        }
        return y + S(8) - y0;
    }
    auto& list = tab_ == 2 ? remux_ : conv_;
    FileRow& f = list[r.idx];
    add(K_Text, RECT{left, y, right, y + dh}, f.path, S_Mid | S_Mono);
    y += dh + S(4);
    if (!f.info.ok) return y + S(4) - y0;
    const Settings& s = view_;
    RECT area{left, y, right, y};
    uint64_t key = f.key;
    if (tab_ == 1) {
        EncoderCaps caps;
        {
            std::lock_guard<std::mutex> lk(capsMu_);
            caps = caps_;
        }
        ConvertSpec planSpec = s.convert;
        planSpec.accel = s.encoder;
        Plan p = planConvert(f.info, planSpec, caps);
        Advice adv = adviseConvert(f.info, s.convert);
        if (adv.mode != Advice::Convert && !adv.text.empty()) {
            int xx = text(left, y, adv.text, S_Bold, right - left - S(170));
            button(xx + S(14), y, L"send to remux", [this, key] {
                for (size_t i = 0; i < conv_.size(); i++) {
                    if (conv_[i].key != key) continue;
                    FileRow f2 = conv_[i];
                    conv_.erase(conv_.begin() + i);
                    f2.key = nextKey_++;
                    remux_.push_back(f2);
                    sel_[2] = RowId{0, f2.key, 0};
                    setTab(2);
                    return;
                }
            });
            y += rh;
        } else if (!adv.text.empty()) {
            text(left, y, adv.text, S_Mid, right - left);
            y += rh;
        }
        buildPlan(area, p, y);
        y += S(6);
        text(left, y, L"SOURCE  " + describeMedia(f.info), S_Bold, right - left);
        y += rh;
        for (auto& st : f.info.streams) {
            add(K_Text, RECT{left + S(16), y, right, y + dh}, describeStream(st), S_Mid | S_Mono);
            y += dh;
        }
        return y + S(10) - y0;
    }
    // Remux: container advice, then every track with what happens to it.
    RemuxSpec rs = f.remux;
    rs.container = s.remuxContainer;
    rs.audioPcm = s.remuxPcm;
    rs.pcmBits = s.remuxPcmBits;
    ContainerAdvice adv = adviseContainer(s.remuxContainer, f.info);
    if (adv.best != s.remuxContainer) {
        std::wstring t = std::wstring(containerName(s.remuxContainer)) + L" cannot keep " + std::to_wstring(adv.convertible + adv.impossible) +
                         L" track(s) bit-for-bit. " + containerName(adv.best) + L" keeps every track with no compromise.";
        int xx = text(left, y, t, S_Bold, right - left - S(130));
        Container best = adv.best;
        button(xx + S(14), y, std::wstring(L"use ") + containerName(best), [this, best] {
            updateSetting([best](Settings& st) { st.remuxContainer = best; });
            for (auto& ff : remux_) ff.remux.actions.clear();
            relayout();
        });
        y += rh;
    } else if (adv.convertible + adv.impossible == 0) {
        text(left, y, std::wstring(L"Every track fits ") + containerName(s.remuxContainer) + L" bit-for-bit.", S_Mid, right - left);
        y += rh;
    } else {
        text(left, y, L"Neither MP4 nor MOV holds every track as it is: the list shows what happens to each.", S_Mid, right - left);
        y += rh;
    }
    int boxW = S(220);
    for (auto& st : f.info.streams) {
        auto it = rs.actions.find(st.index);
        TrackAction act = it != rs.actions.end() ? it->second : defaultAction(s.remuxContainer, st, s.remuxPcm);
        auto allowed = allowedActions(s.remuxContainer, st);
        if (std::find(allowed.begin(), allowed.end(), act) == allowed.end()) act = defaultAction(s.remuxContainer, st, s.remuxPcm);
        int sid = st.index;
        int ax = right - boxW;
        text(left, y, describeStream(st), (act == TrackAction::Exclude ? S_Mid : 0) | S_Mono, ax - left - S(16));
        if (st.type == "data") {
            add(K_Text, RECT{ax + S(9), y, right, y + rh}, st.codec == "bin_data" && f.info.chapters ? L"chapter track" : L"not carried", S_Mid);
            y += rh;
            continue;
        }
        std::wstring label = trackActionName(act);
        if (act == TrackAction::ToPCM) label = std::wstring(L"PCM ") + pcmBitsName(s.remuxPcmBits);
        if (fitStream(s.remuxContainer, st) == Fit::No && act == TrackAction::Exclude) label = L"EXCLUDED (cannot fit)";
        Item& ch = add(K_Box, RECT{ax, y + S(3), right, y + rh - S(3)}, label, S_Click | (act == TrackAction::Copy ? 0 : S_Bold));
        int pcmBits = s.remuxPcmBits;
        ch.click = [this, key, sid, allowed, act, pcmBits] {
            std::vector<std::wstring> o;
            int cur = -1;
            for (size_t i = 0; i < allowed.size(); i++) {
                std::wstring n = trackActionName(allowed[i]);
                if (allowed[i] == TrackAction::Copy) n += L" (bit-for-bit)";
                if (allowed[i] == TrackAction::ToPCM) n = std::wstring(L"PCM ") + pcmBitsName(pcmBits) + (pcmBits == 0 || pcmBits == 32 ? L" (lossless decode)" : L" (rounded)");
                if (allowed[i] == TrackAction::ToText) n += L" (mov_text, styling lost)";
                if (allowed[i] == act) cur = (int)i;
                o.push_back(n);
            }
            int r2 = popup(o, cur);
            if (r2 >= 0)
                for (auto& ff : remux_)
                    if (ff.key == key) ff.remux.actions[sid] = allowed[r2];
            relayout();
        };
        y += rh;
    }
    Plan p = planRemux(f.info, rs);
    y += S(6);
    buildPlan(area, p, y);
    return y + S(10) - y0;
}

void MainWindow::buildPlan(RECT& area, const Plan& p, int& y) {
    int rh = rowH();
    int left = area.left, right = area.right;
    if (!p.ok) {
        text(left, y, L"! " + p.error, S_Bold, right - left);
        y += rh;
        return;
    }
    int x = text(left, y, className(p.cls), S_Bold, right - left - S(200));
    text(x + S(16), y, L"size " + p.estText, S_Mid, right - x - S(16));
    y += rh;
    int dh = denseH();
    for (auto& l : p.lines) {
        add(K_Text, RECT{left + S(16), y, right, y + dh}, l, S_Mono);
        y += dh;
    }
    y += S(4);
    for (auto& w : p.warnings) {
        add(K_Text, RECT{left + S(16), y, right, y + dh}, L"! " + w, S_Bold);
        y += dh;
    }
}


// ---------- views: log, history, settings ----------
// Each view has the same head: a bold title with its summary, actions on the right, and a close icon.

void MainWindow::buildLog(RECT a) {
    int rh = rowH();
    int y = a.top;
    int x = sectionTitle(a.left, y, L"LOG") + S(16);
    std::wstring taskTitle;
    for (auto& t : tasks_)
        if (t.id == selTask_) taskTitle = t.title;
    bool app = logApp_ || !selTask_;
    x = seg(x, y, {L"selected task", L"application"}, app ? 1 : 0, [this](int i) {
        logApp_ = i == 1;
        logScroll_ = 0;
        relayout(true);
    });
    std::vector<std::wstring> lines = app ? recentLog(2000) : queue_.taskLog(selTask_);
    int rx = iconButton(a.right, y, I_Close, false, [this] { setOverlay(Overlay::None); }) - S(12);
    auto rb = [&](const std::wstring& l, std::function<void()> fn, unsigned st = 0) {
        rx -= tw(l) + S(20);
        button(rx, y, l, std::move(fn), st);
        rx -= S(8);
    };
    rb(L"open log file", [this] { ShellExecuteW(hwnd_, nullptr, joinPath(dataDir(), L"encodeall.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    rb(L"copy", [this, lines] {
        std::wstring all;
        for (auto& l : lines) all += l + L"\r\n";
        copyText(all);
        flash(L"log copied", 2000);
    }, lines.empty() ? S_Disabled : 0);
    std::wstring sub = app ? L"downloads, conversions, remuxes and tool updates" : taskTitle;
    text(x + S(4), y, sub, S_Mid, rx - x - S(12));
    y += rh + S(8);
    // The lines sit on a quiet panel; times are dimmed, commands dimmed, failures bold.
    RECT panel{a.left, y, a.right, a.bottom};
    add(K_Fill, panel);
    int dh = denseH();
    RECT list{panel.left + S(12), panel.top + S(8), panel.right - S(12), panel.bottom - S(8)};
    int visible = std::max<int>(1, (list.bottom - list.top) / dh);
    int maxOff = std::max<int>(0, (int)lines.size() - visible);
    logScroll_ = std::clamp(logScroll_, 0, maxOff);
    region(panel, [this, maxOff](int n) {
        logScroll_ = std::clamp(logScroll_ + n * 3, 0, maxOff);
        relayout();
    });
    if (lines.empty()) {
        emptyState(panel, app ? L"Nothing logged yet" : L"No output yet", app ? L"Finished and failed tasks and tool updates are listed here." : L"The tool's output appears here while the task runs.");
        return;
    }
    beginClip(list);
    int tsW = tw(L"0000-00-00 00:00:00", S_Mono) + S(16);
    int start = std::max<int>(0, (int)lines.size() - visible - logScroll_);
    int yy = list.top;
    for (int i = start; i < (int)lines.size() && yy < list.bottom; i++) {
        const std::wstring& l = lines[i];
        bool err = l.find(L"ERROR") != std::wstring::npos || startsWith(l, L"! ") || startsWith(l, L"failed") || l.find(L"failed:") != std::wstring::npos;
        // App lines start with "YYYY-MM-DD HH:MM:SS  ".
        bool stamped = l.size() > 21 && l[4] == L'-' && l[7] == L'-' && l[10] == L' ' && l[13] == L':';
        int lx = list.left;
        std::wstring msg = l;
        if (stamped) {
            add(K_Text, RECT{lx, yy, lx + tsW, yy + dh}, l.substr(0, 19), S_Mono | S_Mid);
            lx += tsW;
            msg = trim(l.substr(19));
        }
        unsigned st = S_Mono | (err ? S_Bold : (startsWith(msg, L">") ? S_Mid : 0));
        add(K_Text, RECT{lx, yy, list.right - S(6), yy + dh}, msg, st);
        yy += dh;
    }
    endClip();
    scrollbar(RECT{list.left, list.top, panel.right - S(4), list.bottom}, (maxOff - logScroll_) * dh, (int)lines.size() * dh);
}

void MainWindow::buildHistory(RECT a) {
    int rh = rowH();
    int y = a.top;
    int x = sectionTitle(a.left, y, L"HISTORY") + S(16);
    text(x, y, history_.empty() ? L"" : std::to_wstring(history_.size()) + L" downloads, newest first. Click one to show its file.", S_Mid);
    int rx = iconButton(a.right, y, I_Close, false, [this] { setOverlay(Overlay::None); }) - S(12);
    Settings s = settingsCopy();
    rx -= tw(L"open downloads folder") + S(20);
    button(rx, y, L"open downloads folder", [this, d = s.dlDir] { openFolder(d); });
    y += rh + S(8);
    if (history_.empty()) {
        emptyState(RECT{a.left, y, a.right, a.bottom}, L"No downloads yet", L"Finished downloads are listed here with their files.");
        return;
    }
    // Column heads.
    int whenW = tw(L"0000-00-00 00:00", S_Mono) + S(24);
    int colTitle = a.left + S(8) + whenW;
    int colFile = colTitle + (a.right - colTitle) * 2 / 5;
    add(K_Text, RECT{a.left + S(8), y, colTitle, y + rh}, L"WHEN", S_Mid);
    add(K_Text, RECT{colTitle, y, colFile, y + rh}, L"TITLE", S_Mid);
    add(K_Text, RECT{colFile, y, a.right, y + rh}, L"FILE", S_Mid);
    y += rh;
    hline(a.left, a.right, y - S(2));
    RECT list{a.left, y, a.right, a.bottom};
    int visible = std::max<int>(1, (list.bottom - list.top) / rh);
    int maxOff = std::max<int>(0, (int)history_.size() - visible);
    historyScroll_ = std::clamp(historyScroll_, 0, maxOff);
    region(list, [this, maxOff](int n) {
        historyScroll_ = std::clamp(historyScroll_ - n * 3, 0, maxOff);
        relayout();
    });
    int rowRight = a.right - S(8);
    for (int i = historyScroll_; i < (int)history_.size() && y + rh <= list.bottom; i++) {
        auto& h = history_[i];
        Item& it = add(K_Text, RECT{a.left, y, rowRight, y + rh}, L"", S_Click);
        std::wstring p = h.path;
        it.click = [this, p] { openFolder(p); };
        bool there = fileExists(h.path);
        add(K_Text, RECT{a.left + S(8), y, colTitle - S(8), y + rh}, h.when.substr(0, 16), S_Mono | S_Mid);
        add(K_Text, RECT{colTitle, y, colFile - S(16), y + rh}, h.title, there ? 0 : S_Mid);
        add(K_Text, RECT{colFile, y, rowRight - S(8), y + rh}, there ? h.path : L"moved or deleted:  " + h.path, S_Mid);
        y += rh;
    }
    scrollbar(list, historyScroll_ * rh, (int)history_.size() * rh);
}

void MainWindow::buildSettings(RECT a) {
    int rh = rowH();
    Settings s = settingsCopy();
    int y = a.top;
    sectionTitle(a.left, y, L"SETTINGS");
    text(a.left + tw(L"SETTINGS", S_Bold) + S(16), y, L"saved as you change them", S_Mid);
    iconButton(a.right, y, I_Close, false, [this] { setOverlay(Overlay::None); });
    y += rh + S(4);
    RECT view{a.left, y, a.right, a.bottom};
    // Two columns: what the setting is, and its control. Kept to a readable width on wide windows.
    int colL = a.left + S(4);
    int colC = a.left + std::min<int>(S(250), (a.right - a.left) / 3);
    int maxR = std::min<int>(a.right - S(12), colC + S(900));
    int y0 = view.top - settingsScroll_;
    y = y0;
    beginClip(view);
    bool first = true;
    auto section = [&](const std::wstring& t) {
        y += first ? S(4) : S(18);
        first = false;
        text(colL, y, t, S_Bold);
        y += rh;
        hline(colL, maxR, y - S(4));
        y += S(4);
    };
    auto row = [&](const std::wstring& label) {
        if (!label.empty()) add(K_Text, RECT{colL + S(8), y, colC - S(16), y + rh}, label, 0);
        return colC;
    };
    auto note = [&](const std::wstring& t) {
        add(K_Text, RECT{colC, y - S(4), maxR, y - S(4) + denseH()}, t, S_Mid);
        y += denseH();
    };
    auto upd = [this](const std::function<void(Settings&)>& f) {
        updateSetting(f);
        queue_.kick();
        relayout();
    };
    auto num = [&](const std::wstring& label, int v, int lo, int hi, std::function<void(Settings&, int)> set) {
        int x = row(label);
        choice(x, y, L"", std::to_wstring(v), [this, lo, hi, v, upd, set] {
            std::vector<std::wstring> o;
            for (int i = lo; i <= hi; i++) o.push_back(std::to_wstring(i));
            int r = popup(o, v - lo);
            if (r >= 0) upd([&](Settings& st) { set(st, lo + r); });
        }, [upd, v, lo, hi, set](int n) { upd([&](Settings& st) { set(st, std::clamp(v + n, lo, hi)); }); });
        y += rh;
    };

    EncoderCaps caps;
    {
        std::lock_guard<std::mutex> lk(capsMu_);
        caps = caps_;
    }
    section(L"ENCODING");
    {
        int x = row(L"Encoder");
        std::string fastest = pickEncoder(VCodec::H264, Accel::Auto, RateMode::Quality, caps);
        std::wstring v = s.encoder == Accel::Auto ? L"automatic: fastest available (" + encoderLabel(fastest) + L")"
                         : s.encoder == Accel::Software ? L"software (x264 / x265)" : accelName(s.encoder);
        choice(x, y, L"", v, [this, caps, upd] {
            const char* hw[] = {"", "", "h264_nvenc", "h264_qsv", "h264_amf"};
            std::vector<std::wstring> o = {L"automatic: fastest available", L"software (x264 / x265)", L"NVENC (NVIDIA)", L"Quick Sync (Intel)", L"AMF (AMD)"};
            for (int i = 2; i < 5; i++)
                if (caps.probed && !caps.has(hw[i]) && !caps.has(std::string(hw[i]).replace(0, 4, "hevc"))) o[i] += L"  (not found here)";
            int r = popup(o, (int)settingsCopy().encoder);
            if (r >= 0) upd([r](Settings& st) { st.encoder = (Accel)r; });
        });
        y += rh;
        note(L"For every conversion and MP4/MOV download. Automatic: graphics card first, then software.");
        x = row(L"Graphics encoders");
        std::wstring e;
        if (caps.probed) {
            auto has = [&](const char* a2, const char* b2) { return caps.has(a2) || caps.has(b2); };
            e = std::wstring(L"NVENC ") + (has("h264_nvenc", "hevc_nvenc") ? L"yes" : L"no") + L"     Quick Sync " + (has("h264_qsv", "hevc_qsv") ? L"yes" : L"no") +
                L"     AMF " + (has("h264_amf", "hevc_amf") ? L"yes" : L"no");
        } else {
            e = capsRunning_ ? L"looking for graphics encoders..." : L"not checked yet";
        }
        x = text(x, y, e, S_Mono) + S(16);
        button(x, y, L"check again", [this] {
            updateSetting([](Settings& st) { st.encoderCapsFor.clear(); });
            {
                std::lock_guard<std::mutex> lk(capsMu_);
                caps_.probed = false;
            }
            detectEncodersIfNeeded();
            relayout();
        }, capsRunning_ ? S_Disabled : 0);
        y += rh;
        x = row(L"Priority");
        toggle(x, y, L"run encoders at low priority (the PC stays responsive)", s.lowPriority, [upd] { upd([](Settings& st) { st.lowPriority = !st.lowPriority; }); });
        y += rh;
    }
    section(L"TASKS");
    num(L"Parallel downloads", s.maxDownloads, 1, 8, [](Settings& st, int v) { st.maxDownloads = v; });
    num(L"Parallel conversions", s.maxEncodes, 1, 4, [](Settings& st, int v) { st.maxEncodes = v; });
    num(L"Parallel remuxes", s.maxRemux, 1, 8, [](Settings& st, int v) { st.maxRemux = v; });
    section(L"DOWNLOADS");
    {
        int x = row(L"Fragments per download");
        choice(x, y, L"", std::to_wstring(s.fragments), [this, upd] {
            std::vector<std::wstring> o = {L"1", L"4", L"8", L"16", L"32"};
            const int v[] = {1, 4, 8, 16, 32};
            int cur = settingsCopy().fragments, checked = -1;
            for (int i = 0; i < 5; i++)
                if (cur == v[i]) checked = i;
            int r = popup(o, checked);
            if (r >= 0) upd([&](Settings& st) { st.fragments = v[r]; });
        });
        y += rh;
        x = row(L"Sign-in cookies");
        std::wstring ck = s.cookiesFile.empty() ? (s.cookiesBrowser.empty() ? L"none" : s.cookiesBrowser) : L"file: " + fileName(s.cookiesFile);
        choice(x, y, L"", ck, [this, upd] {
            std::vector<std::wstring> o = {L"none", L"edge", L"chrome", L"firefox", L"brave", L"opera", L"vivaldi", L"chromium", L"-", L"cookies.txt file..."};
            Settings cur = settingsCopy();
            int checked = cur.cookiesFile.empty() && cur.cookiesBrowser.empty() ? 0 : !cur.cookiesFile.empty() ? 9 : -1;
            for (int i = 1; i <= 7; i++)
                if (cur.cookiesFile.empty() && cur.cookiesBrowser == o[i]) checked = i;
            int r = popup(o, checked);
            if (r == 0) upd([](Settings& st) { st.cookiesBrowser.clear(); st.cookiesFile.clear(); });
            else if (r >= 1 && r <= 7) upd([&](Settings& st) { st.cookiesBrowser = o[r]; st.cookiesFile.clear(); });
            else if (r == 9) {
                auto f = openFiles(L"cookies.txt", L"*.txt", false);
                if (!f.empty()) upd([&](Settings& st) { st.cookiesFile = f[0]; st.cookiesBrowser.clear(); });
            }
        });
        y += rh;
        note(L"Lets yt-dlp use a browser's sign-in for private, members-only or age-restricted videos.");
        x = row(L"Original download");
        toggle(x, y, L"keep it next to the converted file (MP4, MOV, MP3, WAV)", s.keepAudioSource, [upd] { upd([](Settings& st) { st.keepAudioSource = !st.keepAudioSource; }); });
        y += rh;
    }
    section(L"APPEARANCE");
    {
        int x = row(L"Theme");
        seg(x, y, {L"follow Windows", L"dark", L"light"}, s.theme, [this](int i) {
            updateSetting([i](Settings& st) { st.theme = i; });
            applyTheme();
            relayout(true);
        });
        y += rh;
    }
    section(L"UPDATES");
    {
        UpdateState us = updater_.state();
        int x = row(L"Encode All");
        x = text(x, y, L"v" EA_VERSION_W, S_Mono) + S(16);
        bool checking = us.checking || updateBusy_;
        x = button(x, y, L"check now", [this] {
            if (updateBusy_) return;  // never join a running check on the UI thread
            if (updateThread_.joinable()) updateThread_.join();
            updateBusy_ = true;
            updateThread_ = std::thread([this] {
                updater_.checkAndStage(stop_);
                updateBusy_ = false;
                PostMessageW(hwnd_, WM_APP_UPDATE, 0, 0);
            });
            relayout();
        }, checking ? S_Disabled : 0);
        text(x + S(4), y, checking ? L"checking..." : us.error.empty() ? us.status : us.error, us.error.empty() ? S_Mid : S_Bold, maxR - x);
        y += rh;
        x = row(L"Automatic updates");
        toggle(x, y, L"update Encode All (applies on the next start)", s.autoUpdateApp, [upd] { upd([](Settings& st) { st.autoUpdateApp = !st.autoUpdateApp; }); });
        y += rh;
        toggle(row(L""), y, L"update yt-dlp, FFmpeg and Deno (never while they are working)", s.autoUpdateDeps, [upd] { upd([](Settings& st) { st.autoUpdateDeps = !st.autoUpdateDeps; }); });
        y += rh;
        toggle(row(L""), y, L"try the yt-dlp nightly build after an extraction failure", s.nightlyFallback, [upd] { upd([](Settings& st) { st.nightlyFallback = !st.nightlyFallback; }); });
        y += rh;
    }
    section(L"TOOLS");
    {
        int verW = std::min<int>(S(300), maxR - colC - S(200));
        for (int i = 0; i < kDeps; i++) {
            Dep d = (Dep)i;
            DepView v = deps_.view(d);
            int x = row(depName(d));
            std::wstring st = v.present ? v.version + (d == Dep::YtDlp ? L"  (" + v.channel + L")" : L"") : L"not installed";
            if (v.busy) st += L"  " + v.status;
            if (!v.error.empty()) st += L"  ! " + v.error;
            add(K_Text, RECT{x, y, x + verW, y + rh}, st, S_Mono | (v.error.empty() ? 0 : S_Bold));
            x += verW + S(12);
            x = button(x, y, L"update", [this, d] {
                spawn([this, d] {
                    if (d == Dep::YtDlp) {
                        std::wstring n;
                        deps_.updateYtDlpNow(false, n);
                    } else {
                        deps_.startMaintenance(true, true);
                    }
                    PostMessageW(hwnd_, WM_APP_DEPS, 0, 0);
                });
            }, v.busy ? S_Disabled : 0);
            button(x, y, L"roll back", [this, d] {
                std::wstring note2;
                deps_.rollback(d, note2);
                flash(note2, 5000);
            }, v.hasPrevious && !v.busy ? 0 : S_Disabled);
            y += rh;
        }
    }
    section(L"DATA");
    {
        int x = row(L"Data folder");
        std::wstring dd = dataDir() + (isPortable() ? L"  (portable)" : L"");
        int w = std::min<int>(tw(dd, S_Mono), maxR - x - S(80));
        add(K_Text, RECT{x, y, x + w, y + rh}, dd, S_Mono | S_Mid);
        button(x + w + S(12), y, L"open", [this] { openFolder(dataDir()); });
        y += rh;
        x = row(L"Licences");
        text(x, y, L"yt-dlp: Unlicense.  FFmpeg: GPLv3.  Deno: MIT.  Encode All: MIT.  Details in THIRD_PARTY.md.", S_Mid, maxR - x);
        y += rh + S(8);
    }
    endClip();
    int contentH = y - y0;
    int maxOff = std::max<int>(0, contentH - (view.bottom - view.top));
    settingsScroll_ = std::clamp(settingsScroll_, 0, maxOff);
    scrollbar(view, settingsScroll_, contentH);
    // The region goes first in hit order for wheel only (items above take clicks).
    Item r;
    r.kind = K_Region;
    r.r = view;
    r.wheel = [this, maxOff](int n) { scrollBy(settingsScroll_, n * rowH() * 3, maxOff); };
    items_.insert(items_.begin(), r);
}

}  // namespace ea::ui
