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
        r += L" | ";
    }
    int na = m.count("audio");
    if (const StreamInfo* a = m.audio()) {
        r += widen(a->codec) + L" " + (a->layout.empty() ? std::to_wstring(a->channels) + L"ch" : widen(a->layout));
        if (na > 1) r += L" +" + std::to_wstring(na - 1);
        r += L" | ";
    }
    int ns = m.count("subtitle");
    if (ns) r += std::to_wstring(ns) + L" subs | ";
    r += fmtDuration(m.duration) + L" | " + fmtBytes((double)m.size);
    if (m.bitrate > 0) r += L" | " + fmtBitrate(m.bitrate);
    return r;
}

const wchar_t* kindTag(TaskKind k) { return k == TaskKind::Download ? L"DL" : k == TaskKind::Convert ? L"CV" : L"RX"; }
const wchar_t* classTag(QualityClass c) { return c == QualityClass::Copy ? L"COPY" : c == QualityClass::Lossless ? L"LOSSLESS" : L"LOSSY"; }

}  // namespace

void MainWindow::build() {
    int pad = S(16);
    RECT area{pad, 0, client_.right - pad, client_.bottom - S(8)};
    if (area.right - area.left < S(200)) return;
    buildHeader(area);
    int total = area.bottom - area.top;
    int qh = std::clamp(total * 30 / 100, rowH() * 5, rowH() * 14);
    RECT q{area.left, area.bottom - qh, area.right, area.bottom};
    RECT sec{area.left, area.top, area.right, q.top - S(10)};
    switch (overlay_) {
        case Overlay::Log: buildLog(sec); break;
        case Overlay::Settings: buildSettings(sec); break;
        case Overlay::History: buildHistory(sec); break;
        default:
            if (tab_ == 0) buildDownload(sec);
            else if (tab_ == 1) buildConvert(sec);
            else buildRemux(sec);
    }
    hline(area.left, area.right, q.top - S(5));
    buildQueue(RECT{q.left, q.top + S(2), q.right, q.bottom});
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
    text(x + S(8), y, L"v" EA_VERSION_W, S_Mid);
    // Right side, laid out right to left: an update ready to apply, then the tool status.
    int rx = wx - S(16);
    UpdateState us = updater_.state();
    if (us.staged) {
        std::wstring label = L"restart to update " + us.stagedVersion;
        int w = tw(label, S_Bold) + S(20);
        rx -= w;
        Item& it = add(K_Button, RECT{rx, y + S(3), rx + w, y + rh - S(3)}, label, S_Bold | S_Click);
        it.click = [this] {
            if (queue_.running() > 0) {
                flash_ = L"Finish or pause running tasks first; the update also applies on the next start.";
                SetTimer(hwnd_, T_FLASH, 5000, nullptr);
                relayout();
                return;
            }
            if (AppUpdater::relaunch()) DestroyWindow(hwnd_);
        };
        rx -= S(12);
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
    s.click = [this] {
        overlay_ = Overlay::Settings;
        relayout(true);
    };
    hline(0, client_.right, bh - 1);
    // Sections on the left, views (history, log, settings) as icons on the far right of the same row.
    y = bh + S(8);
    x = a.left;
    const wchar_t* names[] = {L"DOWNLOAD", L"CONVERT", L"REMUX"};
    for (int i = 0; i < 3; i++) {
        int w = tw(names[i], S_Bold) + S(32);
        bool on = overlay_ == Overlay::None && tab_ == i;
        Item& it = add(K_Cell, RECT{x, y, x + w, y + rh}, names[i], S_Bold | S_Click | (on ? S_Invert : 0));
        it.click = [this, i] { setTab(i); };
        x += w + S(2);
    }
    auto toggleView = [this](Overlay o) {
        overlay_ = overlay_ == o ? Overlay::None : o;
        if (overlay_ == Overlay::History) {
            history_ = loadHistory(500);
            historyScroll_ = 0;
        }
        if (overlay_ == Overlay::Log) logScroll_ = 0;
        relayout(true);
    };
    int ix = a.right;
    ix = iconButton(ix, y, I_Settings, overlay_ == Overlay::Settings, [toggleView] { toggleView(Overlay::Settings); }) - S(4);
    ix = iconButton(ix, y, I_Log, overlay_ == Overlay::Log, [toggleView] { toggleView(Overlay::Log); }) - S(4);
    ix = iconButton(ix, y, I_History, overlay_ == Overlay::History, [toggleView] { toggleView(Overlay::History); });
    std::wstring hint = flash_.empty() ? us.error : flash_;
    add(K_Text, RECT{x + S(16), y, ix - S(16), y + rh}, hint, S_Mid | S_Right);
    y += rh + S(8);
    hline(a.left, a.right, y);
    a.top = y + S(12);
}

// ---------- download ----------

void MainWindow::buildDownload(RECT a) {
    int rh = rowH(), cw = fonts_.charW;
    int y = a.top;
    Settings s = settingsCopy();
    sectionTitle(a.left, y, L"LINKS");
    y += rh;
    int bw = std::max<int>(tw(L"analyse"), tw(L"paste")) + S(20);
    RECT er{a.left + 1, y + S(3), a.right - bw - S(12), y + rh * 2 + S(2)};
    placeEdit(E_URLS, er);
    int bx = a.right - bw;
    Item& an = add(K_Button, RECT{bx, y + S(3), a.right, y + rh - S(3)}, L"analyse", S_Click);
    an.click = [this] { analyzeUrls(); };
    Item& pa = add(K_Button, RECT{bx, y + rh + S(1), a.right, y + rh * 2 - S(5)}, L"paste", S_Click);
    pa.click = [this] {
        SetFocus(edits_[E_URLS]);
        SendMessageW(edits_[E_URLS], WM_PASTE, 0, 0);
    };
    y += rh * 2 + S(12);
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
        x = text(x, y, L"QUALITY", S_Mid) + S(8);
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
    int pathMax = std::max<int>(S(160), (a.right - x) * 2 / 5);
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
    for (auto* l : {L"metadata", L"chapters", L"thumbnail", L"subtitles"}) togglesW += S(15) + S(7) + tw(l) + S(16);
    if (x + togglesW > a.right) {
        y += rh;
        x = a.left + labelW();
    }
    x = toggle(x, y, L"metadata", s.metadata, [this] { updateSetting([](Settings& st) { st.metadata = !st.metadata; }); relayout(); });
    x = toggle(x, y, L"chapters", s.chapters, [this] { updateSetting([](Settings& st) { st.chapters = !st.chapters; }); relayout(); });
    x = toggle(x, y, L"thumbnail", s.thumbnail, [this] { updateSetting([](Settings& st) { st.thumbnail = !st.thumbnail; }); relayout(); });
    toggle(x, y, L"subtitles", s.subs, [this] { updateSetting([](Settings& st) { st.subs = !st.subs; }); relayout(); });
    y += rh + S(6);
    hline(a.left, a.right, y);
    y += S(6);

    // List of analysed links.
    int listBottom = a.bottom - rh * 2 - S(14);
    RECT list{a.left, y, a.right, listBottom};
    int thumbH = S(36), thumbW = S(64);
    int tall = std::max<int>(rh * 2, thumbH + S(6));
    struct Row {
        int g, e, h;  // e = -1 header, -2 single, -3 busy or failed, >=0 entry
    };
    std::vector<Row> rows;
    int totalH = 0;
    for (int g = 0; g < (int)groups_.size(); g++) {
        auto& gr = groups_[g];
        if (gr.busy || !gr.error.empty()) {
            rows.push_back({g, -3, gr.error.empty() ? rh : rh * 2});
        } else if (gr.playlist) {
            rows.push_back({g, -1, rh + S(4)});
            for (int e = 0; e < (int)gr.entries.size(); e++) rows.push_back({g, e, tall});
        } else {
            rows.push_back({g, -2, tall});
        }
    }
    for (auto& r : rows) totalH += r.h;
    int viewH = list.bottom - list.top;
    dlScroll_ = std::clamp(dlScroll_, 0, std::max<int>(0, totalH - viewH));
    region(list, [this, totalH, viewH](int n) { scrollBy(dlScroll_, n * rowH() * 3, totalH - viewH); });
    if (groups_.empty()) emptyState(list, L"Paste a link", L"Links are read as soon as they land in the box. Ctrl+V works anywhere on this page.");
    beginClip(list);
    int ry = list.top - dlScroll_;
    bool rangePlaced = false;
    int rowRight = a.right - S(8);  // room for the scroll mark
    for (auto& r : rows) {
        if (ry + r.h < list.top || ry > list.bottom) {
            ry += r.h;
            continue;
        }
        DlGroup& g = groups_[r.g];
        int gi = r.g;
        if (r.e == -3) {
            if (g.busy) {
                text(a.left + S(4), ry, L"analysing  " + g.url, S_Mid, rowRight - a.left);
            } else {
                int xx = text(a.left + S(4), ry, L"!", S_Bold) + S(10);
                xx = button(xx, ry, L"retry", [this, gi] {
                    std::wstring u = groups_[gi].url;
                    groups_.erase(groups_.begin() + gi);
                    SetWindowTextW(edits_[E_URLS], u.c_str());
                    analyzeUrls();
                });
                xx = button(xx, ry, L"remove", [this, gi] {
                    groups_.erase(groups_.begin() + gi);
                    selGroup_ = -1;
                    relayout(true);
                });
                text(xx + S(4), ry, g.url, S_Mid, rowRight - xx);
                add(K_Text, RECT{a.left + cw * 4, ry + rh, rowRight, ry + rh * 2}, g.error + (g.hint.empty() ? L"" : L"  -> " + g.hint));
            }
        } else if (r.e == -1) {
            int sel = 0;
            for (char c : g.checked) sel += c;
            int xx = text(a.left + S(4), ry + S(2), L"PLAYLIST", S_Bold) + S(12);
            xx = button(xx, ry + S(2), L"all", [this, gi] {
                std::fill(groups_[gi].checked.begin(), groups_[gi].checked.end(), 1);
                relayout();
            });
            xx = button(xx, ry + S(2), L"none", [this, gi] {
                std::fill(groups_[gi].checked.begin(), groups_[gi].checked.end(), 0);
                relayout();
            });
            xx = text(xx + S(4), ry + S(2), L"RANGE", S_Mid) + S(8);
            if (gi == (selGroup_ >= 0 ? selGroup_ : gi) && !rangePlaced && ry >= list.top && ry + rh <= list.bottom) {
                placeEdit(E_RANGE, RECT{xx, ry + S(2) + S(4), xx + cw * 14, ry + S(2) + rh - S(4)});
                rangePlaced = true;
                xx += cw * 14 + S(12);
            } else {
                Item& it = add(K_Text, RECT{xx, ry + S(2), xx + cw * 14, ry + S(2) + rh}, L"1-" + std::to_wstring(g.entries.size()), S_Mid | S_Click);
                it.click = [this, gi] {
                    selGroup_ = gi;
                    selEntry_ = 0;
                    relayout(true);
                };
                xx += cw * 14 + S(12);
            }
            xx = button(xx, ry + S(2), L"remove", [this, gi] {
                groups_.erase(groups_.begin() + gi);
                selGroup_ = -1;
                relayout(true);
            });
            text(xx + S(4), ry + S(2), std::to_wstring(sel) + L"/" + std::to_wstring(g.entries.size()) + L"  " + g.title, S_Mid, rowRight - xx);
        } else {
            int e = r.e >= 0 ? r.e : 0;
            bool selected = gi == selGroup_ && (r.e == -2 || e == selEntry_);
            Item& sel = add(K_Text, RECT{a.left, ry, rowRight, ry + r.h - S(2)}, L"", S_Click | (selected ? S_Selected : 0));
            sel.click = [this, gi, e] {
                selGroup_ = gi;
                selEntry_ = e;
                auto& gg = groups_[gi];
                SetWindowTextW(edits_[E_SEGA], gg.segA.size() > (size_t)e ? gg.segA[e].c_str() : L"");
                SetWindowTextW(edits_[E_SEGB], gg.segB.size() > (size_t)e ? gg.segB[e].c_str() : L"");
                relayout();
            };
            bool on = g.checked.size() > (size_t)e && g.checked[e];
            check(a.left + S(6), ry + (r.h - S(2) - rh) / 2, on, [this, gi, e] {
                auto& c = groups_[gi].checked;
                if ((size_t)e < c.size()) c[e] = !c[e];
                relayout();
            });
            int tx = a.left + S(34);
            std::string thumb = r.e == -2 ? g.info.thumb : g.entries[e].thumb;
            Item& th = add(K_Thumb, RECT{tx, ry + S(2), tx + thumbW, ry + S(2) + thumbH});
            th.bmp = thumbs_.get(thumb);
            tx += thumbW + S(12);
            double dur = r.e == -2 ? g.info.duration : g.entries[e].duration;
            std::wstring title = r.e == -2 ? g.title : widen(g.entries[e].title);
            std::wstring lead = (r.e >= 0 ? std::to_wstring(g.entries[e].index) + L".  " : L"") + fmtDuration(dur) + L"  ";
            int lx = text(tx, ry, lead, S_Mid | S_Mono);
            text(lx, ry, title, S_Bold, rowRight - lx);
            std::wstring sub;
            if (r.e == -2) {
                DlPrefs p;
                p.out = s.dlOut;
                p.preferH264 = (s.dlOut == DlOut::MP4 || s.dlOut == DlOut::MOV) && !s.dlHevc;
                p.crf = s.dlCrf;
                p.hevc = s.dlHevc;
                Selection sl = selectFormats(g.info, p);
                sub = sl.ok ? sl.summary + (sl.estBytes > 0 ? L"  ~" + fmtBytes(sl.estBytes) : L"") : L"! " + sl.error;
            }
            std::wstring segText;
            if (g.segA.size() > (size_t)e && (!g.segA[e].empty() || !g.segB[e].empty()))
                segText = L"segment " + (g.segA[e].empty() ? L"0" : g.segA[e]) + L" - " + (g.segB[e].empty() ? L"end" : g.segB[e]) + L"  | ";
            add(K_Text, RECT{tx, ry + rh - S(2), rowRight, ry + rh * 2 - S(2)}, segText + sub, S_Mid | S_Mono);
        }
        ry += r.h;
    }
    endClip();
    scrollbar(list, dlScroll_, totalH);
    // Selected item: segment editor, then the action bar.
    y = listBottom + S(6);
    hline(a.left, a.right, y - S(2));
    bool hasSel = selGroup_ >= 0 && selGroup_ < (int)groups_.size() && !groups_[selGroup_].busy && groups_[selGroup_].error.empty();
    if (hasSel) {
        x = fieldLabel(a.left, y + S(2), L"SEGMENT");
        x = text(x, y + S(2), L"from", S_Mid) + S(8);
        placeEdit(E_SEGA, RECT{x, y + S(2) + S(4), x + cw * 11, y + S(2) + rh - S(4)});
        x += cw * 11 + S(12);
        x = text(x, y + S(2), L"to", S_Mid) + S(8);
        placeEdit(E_SEGB, RECT{x, y + S(2) + S(4), x + cw * 11, y + S(2) + rh - S(4)});
        x += cw * 11 + S(12);
        text(x, y + S(2), L"m:ss or seconds; empty = whole video", S_Mid, a.right - x);
    }
    y += rh + S(8);
    int count = 0;
    double est = 0;
    for (auto& g : groups_) {
        if (g.busy || !g.error.empty()) continue;
        if (g.playlist) {
            for (char c : g.checked) count += c;
        } else if (!g.checked.empty() && g.checked[0]) {
            count++;
            DlPrefs p;
            p.out = s.dlOut;
            p.preferH264 = (s.dlOut == DlOut::MP4 || s.dlOut == DlOut::MOV) && !s.dlHevc;
            p.crf = s.dlCrf;
            p.hevc = s.dlHevc;
            Selection sl = selectFormats(g.info, p);
            if (sl.ok) est += sl.estBytes;
        }
    }
    std::wstring sum = std::to_wstring(count) + L" selected" + (est > 0 ? L"   ~" + fmtBytes(est) + L" (single videos)" : L"");
    text(a.left, y, sum, S_Mid, (a.right - a.left) / 2);
    std::wstring go = L"download " + std::to_wstring(count);
    int gw = tw(go, S_Bold) + S(24);
    int cwid = tw(L"clear") + S(20);
    button(a.right - gw - cwid - S(8), y, L"clear", [this] {
        groups_.clear();
        selGroup_ = -1;
        relayout(true);
    }, groups_.empty() ? S_Disabled : 0);
    Item& gb = add(K_Button, RECT{a.right - gw, y + S(3), a.right, y + rh - S(3)}, go, S_Bold | (count ? S_Invert | S_Click : S_Disabled));
    if (count) gb.click = [this] { startDownloads(); };
}

void MainWindow::buildConvert(RECT a) {
    int rh = rowH();
    int y = a.top;
    Settings s = settingsCopy();
    ConvertSpec& cs = s.convert;
    int x = sectionTitle(a.left, y, L"FILES") + S(16);
    x = button(x, y, L"add files", [this] { pickFiles(1); });
    button(x, y, L"clear", [this] {
        conv_.clear();
        selConv_ = -1;
        relayout(true);
    }, conv_.empty() ? S_Disabled : 0);
    y += rh + S(2);
    int listH = std::clamp((int)conv_.size(), 2, 5) * rh;
    RECT list{a.left, y, a.right, y + listH};
    int maxOff = std::max<int>(0, (int)conv_.size() * rh - listH);
    convScroll_ = std::clamp(convScroll_, 0, maxOff);
    region(list, [this, maxOff](int n) { scrollBy(convScroll_, n * rowH(), maxOff); });
    if (conv_.empty()) emptyState(list, L"Drop files here", L"or use add files (Ctrl+O)");
    beginClip(list);
    int rowRight = a.right - S(8);
    for (int i = 0; i < (int)conv_.size(); i++) {
        int ry = y + i * rh - convScroll_;
        if (ry + rh < list.top || ry > list.bottom) continue;
        FileRow& f = conv_[i];
        Item& row = add(K_Text, RECT{a.left, ry, rowRight, ry + rh}, L"", S_Click | (i == selConv_ ? S_Selected : 0));
        row.click = [this, i] {
            selConv_ = i;
            detailScroll_ = 0;
            relayout();
        };
        check(a.left + S(6), ry, f.checked, [this, i] {
            conv_[i].checked = !conv_[i].checked;
            relayout();
        });
        int nx = text(a.left + S(34), ry, fileName(f.path), S_Bold, (a.right - a.left) * 2 / 5);
        std::wstring info = f.busy ? L"analysing..." : !f.error.empty() ? L"! " + f.error : shortMedia(f.info);
        add(K_Text, RECT{nx + S(16), ry, rowRight - S(40), ry + rh}, info, S_Mid | S_Mono);
        Item& x2 = add(K_Icon, RECT{rowRight - S(28), ry + S(3), rowRight - S(2), ry + rh - S(3)}, L"", S_Click);
        x2.value = I_Close;
        x2.click = [this, i] {
            conv_.erase(conv_.begin() + i);
            if (selConv_ >= (int)conv_.size()) selConv_ = (int)conv_.size() - 1;
            relayout(true);
        };
    }
    endClip();
    scrollbar(list, convScroll_, (int)conv_.size() * rh);
    y += listH + S(6);
    hline(a.left, a.right, y);
    y += S(8);
    auto change = [this](const std::function<void(ConvertSpec&)>& f) {
        updateSetting([&](Settings& st) {
            f(st.convert);
            st.convertPreset = Preset::Custom;
        });
        customPreset_ = -1;
        relayout();
    };
    // Preset row.
    x = fieldLabel(a.left, y, L"PRESET");
    int ps = s.convertPreset == Preset::Custom ? 4 : (int)s.convertPreset;
    x = seg(x, y, {L"lossless", L"max quality", L"balanced", L"compact", L"custom"}, ps, [this](int i) {
        if (i == 4) return;
        updateSetting([i](Settings& st) {
            applyPreset(st.convert, (Preset)i);
            st.convertPreset = (Preset)i;
        });
        customPreset_ = -1;
        relayout();
    });
    std::wstring mine = customPreset_ >= 0 && customPreset_ < (int)presets_.size() ? presets_[customPreset_].name : L"saved presets";
    x = choice(x, y, L"", mine, [this] {
        std::vector<std::wstring> o;
        for (auto& p : presets_) o.push_back(p.name);
        if (!presets_.empty()) o.push_back(L"-");
        size_t delStart = o.size();
        for (auto& p : presets_) o.push_back(L"delete " + p.name);
        if (o.empty()) {
            flash_ = L"No saved presets yet: set things up, then 'save preset'.";
            SetTimer(hwnd_, T_FLASH, 4000, nullptr);
            relayout();
            return;
        }
        int r = popup(o, customPreset_);
        if (r < 0) return;
        if ((size_t)r < presets_.size()) {
            ConvertSpec sp = presets_[r].spec;
            updateSetting([&](Settings& st) {
                st.convert = sp;
                st.convertPreset = Preset::Custom;
            });
            customPreset_ = r;
        } else if ((size_t)r >= delStart) {
            presets_.erase(presets_.begin() + (r - delStart));
            savePresets(presets_);
            customPreset_ = -1;
        }
        relayout();
    }) - S(8);
    int bx = x;
    button(bx, y, L"save preset", [this, bx, y] {
        customValue(L"name", L"my preset", RECT{bx, y + S(4), bx + S(200), y + rowH() - S(4)}, [this](const std::wstring& name) {
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
            flash_ = L"preset saved: " + name;
            SetTimer(hwnd_, T_FLASH, 3000, nullptr);
        });
    });
    if (customDone_) placeEdit(E_CUSTOM, editRects_[E_CUSTOM]);
    y += rh + S(2);
    // Target row: the encoder is chosen automatically (fastest available); Settings can pin one.
    EncoderCaps caps;
    {
        std::lock_guard<std::mutex> lk(capsMu_);
        caps = caps_;
    }
    ConvertSpec planSpec = cs;
    planSpec.accel = s.encoder;
    int ti = targetIndex(cs);
    x = fieldLabel(a.left, y, L"TARGET");
    x = choice(x, y, L"", kTargets[ti], [this, ti] {
        int r = popup(kTargets, ti);
        if (r >= 0) {
            updateSetting([r](Settings& st) {
                setTarget(st.convert, r);
                st.convertPreset = Preset::Custom;
            });
            relayout();
        }
    }, [this, ti](int n) {
        int r = std::clamp(ti - n, 0, (int)kTargets.size() - 1);
        updateSetting([r](Settings& st) { setTarget(st.convert, r); st.convertPreset = Preset::Custom; });
        relayout();
    });
    bool audioTarget = cs.audio != AudioTarget::None;
    if (!audioTarget && cs.vcodec != VCodec::Copy && !isDnxhr(cs.vcodec)) {
        x = choice(x, y, L"QUALITY", qualityText(cs), [this, change, x, y] {
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
                customValue(L"", init, RECT{x, y + S(4), x + S(120), y + rowH() - S(4)}, [isCrf, change](const std::wstring& v) {
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
        x = choice(x, y, L"SPEED", widen(cs.speed), [this, change] {
            std::vector<std::wstring> o = {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow", L"slower", L"veryslow"};
            int cur = 5;
            for (size_t i = 0; i < o.size(); i++)
                if (narrow(o[i]) == settingsCopy().convert.speed) cur = (int)i;
            int r = popup(o, cur);
            if (r >= 0) change([&](ConvertSpec& c) { c.speed = narrow(o[r]); });
        });
        std::string enc = pickEncoder(cs.vcodec, s.encoder, cs.rate, caps);
        std::wstring ev = encoderLabel(enc) + (s.encoder == Accel::Auto ? L" (fastest here)" : L" (set in Settings)");
        if (!caps.probed && capsRunning_) ev = encoderLabel(enc) + L" (looking for graphics encoders...)";
        x = text(x, y, L"ENCODER", S_Mid) + S(8);
        int ew = tw(ev);
        Item& ei = add(K_Text, RECT{x, y, std::min<int>(x + ew, a.right), y + rh}, ev, S_Click);
        ei.click = [this] {
            overlay_ = Overlay::Settings;
            settingsScroll_ = 0;
            relayout(true);
        };
    } else if (isDnxhr(cs.vcodec)) {
        text(x, y, L"intermediate codec, fixed rate per profile (visually lossless, not mathematically)", S_Mid, a.right - x);
    } else if (cs.vcodec == VCodec::Copy && !audioTarget) {
        text(x, y, L"video copied bit-for-bit", S_Mid);
    }
    y += rh;
    // Audio row.
    x = fieldLabel(a.left, y, L"AUDIO");
    if (!audioTarget) {
        x = choice(x, y, L"", audioText(cs), [this, change] {
            std::vector<std::wstring> o = {L"copy (bit-for-bit)", L"AAC 128k", L"AAC 160k", L"AAC 192k", L"AAC 256k", L"AAC 320k", L"PCM exact (lossless)", L"PCM 16-bit", L"PCM 24-bit"};
            ConvertSpec cur = settingsCopy().convert;
            const int k[] = {0, 128, 160, 192, 256, 320};
            int checked = cur.acodec == ACodec::Copy ? 0 : cur.acodec == ACodec::PCM ? (cur.pcmBits == 16 ? 7 : cur.pcmBits == 24 ? 8 : 6) : -1;
            for (int i = 1; i <= 5; i++)
                if (cur.acodec == ACodec::AAC && cur.aacKbps == k[i]) checked = i;
            int r = popup(o, checked);
            if (r == 0) change([](ConvertSpec& c) { c.acodec = ACodec::Copy; });
            else if (r >= 1 && r <= 5) change([&](ConvertSpec& c) { c.acodec = ACodec::AAC; c.aacKbps = k[r]; });
            else if (r >= 6) change([&](ConvertSpec& c) { c.acodec = ACodec::PCM; c.pcmBits = r == 6 ? 0 : r == 7 ? 16 : 24; });
        });
        x = toggle(x, y, L"subtitles", cs.keepSubs, [change] { change([](ConvertSpec& c) { c.keepSubs = !c.keepSubs; }); });
        if (cs.container != Container::MKV) x = toggle(x, y, L"faststart", cs.faststart, [change] { change([](ConvertSpec& c) { c.faststart = !c.faststart; }); });
        if (cs.vcodec != VCodec::Copy)
            button(x, y, advanced_ ? L"fewer options" : L"more options", [this] {
                advanced_ = !advanced_;
                relayout(true);
            });
        y += rh;
        if (advanced_ && cs.vcodec != VCodec::Copy) {
            x = fieldLabel(a.left, y, L"PICTURE");
            std::wstring sz = cs.height ? std::to_wstring(cs.height) + L"p" : cs.width ? std::to_wstring(cs.width) + L"w" : L"source";
            if (cs.width && cs.height) sz = std::to_wstring(cs.width) + L"x" + std::to_wstring(cs.height);
            x = choice(x, y, L"SIZE", sz, [this, change, x, y] {
                std::vector<std::wstring> o = {L"source", L"2160p", L"1440p", L"1080p", L"720p", L"480p", L"custom WxH..."};
                const int h[] = {0, 2160, 1440, 1080, 720, 480};
                ConvertSpec cur = settingsCopy().convert;
                int checked = -1;
                for (int i = 0; i < 6; i++)
                    if (cur.width == 0 && cur.height == h[i]) checked = i;
                int r = popup(o, checked);
                if (r >= 0 && r <= 5) change([&](ConvertSpec& c) { c.height = h[r]; c.width = 0; });
                else if (r == 6)
                    customValue(L"", L"1920x1080", RECT{x, y + S(4), x + S(140), y + rowH() - S(4)}, [change](const std::wstring& v) {
                        int w2 = 0, h2 = 0;
                        if (swscanf(v.c_str(), L"%dx%d", &w2, &h2) == 2 && w2 > 0 && h2 > 0 && w2 <= 16384 && h2 <= 16384)
                            change([&](ConvertSpec& c) { c.width = w2 & ~1; c.height = h2 & ~1; });
                    });
            });
            x = choice(x, y, L"FPS", cs.fps > 0 ? fmtFps(cs.fps) : L"source", [this, change] {
                std::vector<std::wstring> o = {L"source", L"23.976", L"24", L"25", L"29.97", L"30", L"50", L"59.94", L"60"};
                const double f[] = {0, 24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60};
                double cur = settingsCopy().convert.fps;
                int checked = -1;
                for (int i = 0; i < 9; i++)
                    if (std::fabs(cur - f[i]) < 0.001) checked = i;
                int r = popup(o, checked);
                if (r >= 0) change([&](ConvertSpec& c) { c.fps = f[r]; });
            });
            choice(x, y, L"PIXELS", cs.pixFmt.empty() ? L"source" : widen(cs.pixFmt), [this, change] {
                std::vector<std::wstring> o = {L"source", L"yuv420p (8-bit 4:2:0)", L"yuv420p10le (10-bit 4:2:0)", L"yuv422p10le (10-bit 4:2:2)", L"yuv444p10le (10-bit 4:4:4)"};
                const char* v[] = {"", "yuv420p", "yuv420p10le", "yuv422p10le", "yuv444p10le"};
                std::string cur = settingsCopy().convert.pixFmt;
                int checked = -1;
                for (int i = 0; i < 5; i++)
                    if (cur == v[i]) checked = i;
                int r = popup(o, checked);
                if (r >= 0) change([&](ConvertSpec& c) { c.pixFmt = v[r]; });
            });
            y += rh;
        }
    } else {
        if (cs.audio == AudioTarget::FLAC)
            x = choice(x, y, L"LEVEL", std::to_wstring(cs.flacLevel), [this, change] {
                std::vector<std::wstring> o;
                for (int i = 0; i <= 12; i++) o.push_back(std::to_wstring(i) + (i == 0 ? L" (fastest)" : i == 8 ? L" (default)" : i == 12 ? L" (smallest)" : L""));
                int r = popup(o, settingsCopy().convert.flacLevel);
                if (r >= 0) change([r](ConvertSpec& c) { c.flacLevel = r; });
            }, [change](int n) { change([n](ConvertSpec& c) { c.flacLevel = std::clamp(c.flacLevel + n, 0, 12); }); });
        if (cs.audio == AudioTarget::MP3)
            x = choice(x, y, L"MP3", cs.mp3Vbr ? L"VBR V" + std::to_wstring(cs.mp3Quality) : L"CBR " + std::to_wstring(cs.mp3Kbps) + L"k", [this, change] {
                std::vector<std::wstring> o = {L"CBR 320k (maximum)", L"CBR 256k", L"CBR 192k", L"CBR 128k", L"VBR V0 (~245k)", L"VBR V2 (~190k)", L"VBR V4 (~165k)"};
                const int k[] = {320, 256, 192, 128};
                const int v[] = {0, 2, 4};
                ConvertSpec cur = settingsCopy().convert;
                int checked = -1;
                for (int i = 0; i < 4; i++)
                    if (!cur.mp3Vbr && cur.mp3Kbps == k[i]) checked = i;
                for (int i = 0; i < 3; i++)
                    if (cur.mp3Vbr && cur.mp3Quality == v[i]) checked = 4 + i;
                int r = popup(o, checked);
                if (r >= 0 && r <= 3) change([&](ConvertSpec& c) { c.mp3Vbr = false; c.mp3Kbps = k[r]; });
                else if (r >= 4) change([&](ConvertSpec& c) { c.mp3Vbr = true; c.mp3Quality = v[r - 4]; });
            });
        if (cs.audio != AudioTarget::MP3)
            x = choice(x, y, L"BITS", cs.bitDepth ? std::to_wstring(cs.bitDepth) + (cs.bitDepth == 32 ? L" float" : L"") : L"source (exact)", [this, change] {
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
        x = choice(x, y, L"RATE", cs.sampleRate ? std::to_wstring(cs.sampleRate) : L"source", [this, change] {
            std::vector<std::wstring> o = {L"source", L"44100", L"48000", L"96000"};
            const int r2[] = {0, 44100, 48000, 96000};
            int cur = settingsCopy().convert.sampleRate, checked = -1;
            for (int i = 0; i < 4; i++)
                if (cur == r2[i]) checked = i;
            int r = popup(o, checked);
            if (r >= 0) change([&](ConvertSpec& c) { c.sampleRate = r2[r]; });
        });
        choice(x, y, L"CHANNELS", cs.channels ? (cs.channels == 1 ? L"mono" : cs.channels == 2 ? L"stereo" : std::to_wstring(cs.channels)) : L"source", [this, change] {
            std::vector<std::wstring> o = {L"source", L"stereo", L"mono"};
            const int c2[] = {0, 2, 1};
            int cur = settingsCopy().convert.channels, checked = -1;
            for (int i = 0; i < 3; i++)
                if (cur == c2[i]) checked = i;
            int r = popup(o, checked);
            if (r >= 0) change([&](ConvertSpec& c) { c.channels = c2[r]; });
        });
        y += rh;
    }
    // Output folder.
    x = fieldLabel(a.left, y, L"SAVE TO");
    choice(x, y, L"", s.convertDir.empty() ? L"next to the source" : s.convertDir, [this] {
        int r = popup({L"next to the source", L"choose folder..."}, settingsCopy().convertDir.empty() ? 0 : 1);
        if (r == 0) updateSetting([](Settings& st) { st.convertDir.clear(); });
        else if (r == 1) {
            std::wstring d = pickFolder(settingsCopy().convertDir);
            if (!d.empty()) updateSetting([&](Settings& st) { st.convertDir = d; });
        }
        relayout();
    }, nullptr, (a.right - x) / 2);
    y += rh + S(4);
    hline(a.left, a.right, y);
    y += S(8);
    // Plan for the selected file.
    int actionY = a.bottom - rh;
    RECT detail{a.left, y, a.right, actionY - S(8)};
    if (selConv_ >= 0 && selConv_ < (int)conv_.size() && conv_[selConv_].info.ok) {
        FileRow& f = conv_[selConv_];
        Plan p = planConvert(f.info, planSpec, caps);
        Advice adv = adviseConvert(f.info, cs);
        int contentY = y - detailScroll_;
        beginClip(detail);
        int yy = contentY;
        if (adv.mode != Advice::Convert && !adv.text.empty()) {
            int xx = text(a.left, yy, adv.text, S_Bold, a.right - a.left - S(150));
            button(xx + S(12), yy, L"send to remux", [this] {
                FileRow f2 = conv_[selConv_];
                conv_.erase(conv_.begin() + selConv_);
                selConv_ = conv_.empty() ? -1 : 0;
                f2.key = nextKey_++;
                remux_.push_back(f2);
                selRemux_ = (int)remux_.size() - 1;
                setTab(2);
            });
            yy += rh;
        } else if (!adv.text.empty()) {
            text(a.left, yy, adv.text, S_Mid, a.right - a.left);
            yy += rh;
        }
        buildPlan(detail, p, yy);
        yy += S(6);
        text(a.left, yy, L"SOURCE  " + describeMedia(f.info), S_Bold, a.right - a.left - S(8));
        yy += rh;
        for (auto& st : f.info.streams) {
            add(K_Text, RECT{a.left + S(16), yy, a.right - S(8), yy + denseH()}, describeStream(st), S_Mid | S_Mono);
            yy += denseH();
        }
        endClip();
        int contentH = yy - contentY;
        int maxOff2 = std::max<int>(0, contentH - (detail.bottom - detail.top));
        detailScroll_ = std::clamp(detailScroll_, 0, maxOff2);
        region(detail, [this, maxOff2](int n) { scrollBy(detailScroll_, n * rowH() * 3, maxOff2); });
        scrollbar(detail, detailScroll_, contentH);
    } else if (!conv_.empty()) {
        text(a.left, y, L"Select an analysed file to see exactly what will happen.", S_Mid);
    }
    int count = 0;
    for (auto& f : conv_) count += f.checked && f.info.ok;
    text(a.left, actionY, std::to_wstring(count) + L" of " + std::to_wstring(conv_.size()) + L" file(s) selected", S_Mid);
    std::wstring go = L"convert " + std::to_wstring(count);
    int gw = tw(go, S_Bold) + S(24);
    Item& gb = add(K_Button, RECT{a.right - gw, actionY + S(3), a.right, actionY + rh - S(3)}, go, S_Bold | (count ? S_Invert | S_Click : S_Disabled));
    if (count) gb.click = [this] { startConvert(); };
}

void MainWindow::buildPlan(RECT& area, const Plan& p, int& y) {
    int rh = rowH();
    int left = area.left, right = area.right - S(8);
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

// ---------- remux ----------

void MainWindow::buildRemux(RECT a) {
    int rh = rowH();
    int y = a.top;
    Settings s = settingsCopy();
    int x = sectionTitle(a.left, y, L"FILES") + S(16);
    x = button(x, y, L"add files", [this] { pickFiles(2); });
    button(x, y, L"clear", [this] {
        remux_.clear();
        selRemux_ = -1;
        relayout(true);
    }, remux_.empty() ? S_Disabled : 0);
    y += rh + S(2);
    int listH = std::clamp((int)remux_.size(), 2, 4) * rh;
    RECT list{a.left, y, a.right, y + listH};
    int maxOff = std::max<int>(0, (int)remux_.size() * rh - listH);
    remuxScroll_ = std::clamp(remuxScroll_, 0, maxOff);
    region(list, [this, maxOff](int n) { scrollBy(remuxScroll_, n * rowH(), maxOff); });
    if (remux_.empty()) emptyState(list, L"Drop files here", L"or use add files (Ctrl+O)");
    beginClip(list);
    int rowRight = a.right - S(8);
    for (int i = 0; i < (int)remux_.size(); i++) {
        int ry = y + i * rh - remuxScroll_;
        if (ry + rh < list.top || ry > list.bottom) continue;
        FileRow& f = remux_[i];
        Item& row = add(K_Text, RECT{a.left, ry, rowRight, ry + rh}, L"", S_Click | (i == selRemux_ ? S_Selected : 0));
        row.click = [this, i] {
            selRemux_ = i;
            detailScroll_ = 0;
            relayout();
        };
        check(a.left + S(6), ry, f.checked, [this, i] {
            remux_[i].checked = !remux_[i].checked;
            relayout();
        });
        int nx = text(a.left + S(34), ry, fileName(f.path), S_Bold, (a.right - a.left) * 2 / 5);
        std::wstring info = f.busy ? L"analysing..." : !f.error.empty() ? L"! " + f.error : shortMedia(f.info);
        add(K_Text, RECT{nx + S(16), ry, rowRight - S(40), ry + rh}, info, S_Mid | S_Mono);
        Item& x2 = add(K_Icon, RECT{rowRight - S(28), ry + S(3), rowRight - S(2), ry + rh - S(3)}, L"", S_Click);
        x2.value = I_Close;
        x2.click = [this, i] {
            remux_.erase(remux_.begin() + i);
            if (selRemux_ >= (int)remux_.size()) selRemux_ = (int)remux_.size() - 1;
            relayout(true);
        };
    }
    endClip();
    scrollbar(list, remuxScroll_, (int)remux_.size() * rh);
    y += listH + S(6);
    hline(a.left, a.right, y);
    y += S(8);
    x = fieldLabel(a.left, y, L"CONTAINER");
    seg(x, y, {L"MP4", L"MOV"}, s.remuxContainer == Container::MOV ? 1 : 0, [this](int i) {
        updateSetting([i](Settings& st) { st.remuxContainer = i == 0 ? Container::MP4 : Container::MOV; });
        for (auto& f : remux_) f.remux.actions.clear();  // defaults depend on the container
        relayout();
    });
    y += rh;
    // Audio: kept as it is, or decoded to linear PCM (exact by default; a fixed depth is optional).
    x = fieldLabel(a.left, y, L"AUDIO");
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
    if (s.remuxPcm) {
        x = choice(x, y, L"DEPTH", std::wstring(pcmBitsName(s.remuxPcmBits)) + (s.remuxPcmBits == 0 ? L" (lossless)" : L""), [this] {
            std::vector<std::wstring> o = {L"exact (lossless: source depth, float for lossy sources)", L"16-bit", L"24-bit", L"32-bit float"};
            const int b[] = {0, 16, 24, 32};
            int cur = settingsCopy().remuxPcmBits, checked = 0;
            for (int i = 0; i < 4; i++)
                if (cur == b[i]) checked = i;
            int r = popup(o, checked);
            if (r >= 0) updateSetting([&](Settings& st) { st.remuxPcmBits = b[r]; });
            relayout();
        });
        text(x, y, L"every audio track is decoded to PCM", S_Mid, a.right - x);
    } else {
        text(x, y, L"copied bit-for-bit; PCM only where the container cannot hold a track", S_Mid, a.right - x);
    }
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
    y += rh + S(4);
    hline(a.left, a.right, y);
    y += S(8);
    int actionY = a.bottom - rh;
    RECT detail{a.left, y, a.right, actionY - S(8)};
    if (selRemux_ >= 0 && selRemux_ < (int)remux_.size() && remux_[selRemux_].info.ok) {
        FileRow& f = remux_[selRemux_];
        RemuxSpec rs = f.remux;
        rs.container = s.remuxContainer;
        rs.audioPcm = s.remuxPcm;
        rs.pcmBits = s.remuxPcmBits;
        ContainerAdvice adv = adviseContainer(s.remuxContainer, f.info);
        int contentY = y - detailScroll_;
        beginClip(detail);
        int yy = contentY;
        int rightEdge = a.right - S(8);
        if (adv.best != s.remuxContainer) {
            std::wstring t = std::wstring(containerName(s.remuxContainer)) + L" cannot keep " + std::to_wstring(adv.convertible + adv.impossible) +
                             L" track(s) bit-for-bit. " + containerName(adv.best) + L" keeps every track with no compromise.";
            int xx = text(a.left, yy, t, S_Bold, rightEdge - a.left - S(120));
            Container best = adv.best;
            button(xx + S(12), yy, std::wstring(L"use ") + containerName(best), [this, best] {
                updateSetting([best](Settings& st) { st.remuxContainer = best; });
                for (auto& ff : remux_) ff.remux.actions.clear();
                relayout();
            });
            yy += rh;
        } else if (adv.convertible + adv.impossible == 0) {
            text(a.left, yy, std::wstring(L"Every track fits ") + containerName(s.remuxContainer) + L" bit-for-bit.", S_Mid);
            yy += rh;
        } else {
            text(a.left, yy, std::wstring(L"Neither MP4 nor MOV holds every track as it is: the list shows what happens to each."), S_Mid, rightEdge - a.left);
            yy += rh;
        }
        sectionTitle(a.left, yy, L"TRACKS");
        yy += rh;
        int idx = selRemux_;
        int boxW = S(170);
        for (auto& st : f.info.streams) {
            auto it = rs.actions.find(st.index);
            TrackAction act = it != rs.actions.end() ? it->second : defaultAction(s.remuxContainer, st, s.remuxPcm);
            auto allowed = allowedActions(s.remuxContainer, st);
            if (std::find(allowed.begin(), allowed.end(), act) == allowed.end()) act = defaultAction(s.remuxContainer, st, s.remuxPcm);
            int sid = st.index;
            int ax = rightEdge - boxW;
            text(a.left + S(16), yy, describeStream(st), act == TrackAction::Exclude ? S_Mid : 0, ax - a.left - S(28));
            if (st.type == "data") {
                add(K_Text, RECT{ax + S(9), yy, rightEdge, yy + rh}, st.codec == "bin_data" && f.info.chapters ? L"chapter track" : L"not carried", S_Mid);
                yy += rh;
                continue;
            }
            std::wstring label = trackActionName(act);
            if (act == TrackAction::ToPCM) label = std::wstring(L"PCM ") + pcmBitsName(s.remuxPcmBits);
            if (fitStream(s.remuxContainer, st) == Fit::No && act == TrackAction::Exclude) label = L"EXCLUDED (cannot fit)";
            Item& ch = add(K_Box, RECT{ax, yy + S(3), rightEdge, yy + rh - S(3)}, label, S_Click | (act == TrackAction::Copy ? 0 : S_Bold));
            int pcmBits = s.remuxPcmBits;
            ch.click = [this, idx, sid, allowed, act, pcmBits] {
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
                int r = popup(o, cur);
                if (r >= 0 && idx < (int)remux_.size()) remux_[idx].remux.actions[sid] = allowed[r];
                relayout();
            };
            yy += rh;
        }
        Plan p = planRemux(f.info, rs);
        yy += S(6);
        buildPlan(detail, p, yy);
        endClip();
        int contentH = yy - contentY;
        int maxOff2 = std::max<int>(0, contentH - (detail.bottom - detail.top));
        detailScroll_ = std::clamp(detailScroll_, 0, maxOff2);
        region(detail, [this, maxOff2](int n) { scrollBy(detailScroll_, n * rowH() * 3, maxOff2); });
        scrollbar(detail, detailScroll_, contentH);
    } else if (!remux_.empty()) {
        text(a.left, y, L"Select an analysed file to see each track.", S_Mid);
    }
    int count = 0;
    for (auto& f : remux_) count += f.checked && f.info.ok;
    text(a.left, actionY, std::to_wstring(count) + L" of " + std::to_wstring(remux_.size()) + L" file(s) selected", S_Mid);
    std::wstring go = L"remux " + std::to_wstring(count);
    int gw = tw(go, S_Bold) + S(24);
    Item& gb = add(K_Button, RECT{a.right - gw, actionY + S(3), a.right, actionY + rh - S(3)}, go, S_Bold | (count ? S_Invert | S_Click : S_Disabled));
    if (count) gb.click = [this] { startRemux(); };
}

// ---------- queue ----------

void MainWindow::buildQueue(RECT a) {
    int rh = rowH();
    int y = a.top;
    int run = 0, wait = 0, fail = 0, done = 0, paused = 0;
    for (auto& t : tasks_) {
        run += t.state == TaskState::Running;
        wait += t.state == TaskState::Waiting;
        fail += t.state == TaskState::Failed;
        done += t.state == TaskState::Done || t.state == TaskState::Canceled;
        paused += t.state == TaskState::Paused;
    }
    int x = sectionTitle(a.left, y, L"QUEUE") + S(16);
    std::wstring counts;
    auto part = [&](int n, const wchar_t* what) {
        if (n) counts += (counts.empty() ? L"" : L"   ") + std::to_wstring(n) + L" " + what;
    };
    part(run, L"running");
    part(wait, L"queued");
    part(paused, L"paused");
    part(fail, L"failed");
    part(done, L"finished");
    text(x, y, counts, S_Mid);
    // Only the actions that apply right now.
    int rx = a.right;
    auto rb = [&](const std::wstring& l, std::function<void()> fn) {
        rx -= tw(l) + S(20);
        button(rx, y, l, std::move(fn));
        rx -= S(8);
    };
    if (done) rb(L"clear finished", [this] { queue_.clearFinished(); });
    if (paused) rb(L"resume all", [this] { queue_.resumeAll(); });
    if (run || wait) rb(L"pause all", [this] { queue_.pauseAll(); });
    y += rh + S(4);
    RECT list{a.left, y, a.right, a.bottom};
    int rowHt = rh + S(10);
    int selExtra = rh * 2;
    int totalH = (int)tasks_.size() * rowHt + (selTask_ ? selExtra : 0);
    int viewH = list.bottom - list.top;
    if (revealTask_ && selTask_) {
        int top = 0;
        for (auto& t : tasks_) {
            if (t.id == selTask_) break;
            top += rowHt;
        }
        if (top + rowHt + selExtra > queueScroll_ + viewH) queueScroll_ = top + rowHt + selExtra - viewH;
        if (top < queueScroll_) queueScroll_ = top;
        revealTask_ = false;
    }
    queueScroll_ = std::clamp(queueScroll_, 0, std::max<int>(0, totalH - viewH));
    region(list, [this, totalH, viewH](int n) { scrollBy(queueScroll_, n * rowH() * 2, totalH - viewH); });
    if (tasks_.empty()) {
        emptyState(list, L"Nothing queued", L"Downloads, conversions and remuxes you start appear here.");
        return;
    }
    beginClip(list);
    int ry = y - queueScroll_;
    int rowRight = a.right - S(8);
    for (auto& t : tasks_) {
        bool sel = t.id == selTask_;
        int h = rowHt + (sel ? selExtra : 0);
        if (ry + h < list.top || ry > list.bottom) {
            ry += h;
            continue;
        }
        uint64_t id = t.id;
        bool active = t.state == TaskState::Running || t.state == TaskState::Paused;
        Item& row = add(K_Text, RECT{a.left, ry, rowRight, ry + rowHt - S(2)}, L"", S_Click | (sel ? S_Selected : 0));
        row.click = [this, id] {
            selTask_ = selTask_ == id ? 0 : id;
            revealTask_ = true;
            relayout();
        };
        int line = ry + S(1);
        // Kind chip.
        RECT chip{a.left + S(4), line + S(6), a.left + S(40), line + rh - S(6)};
        add(K_Text, chip, kindTag(t.kind), S_Mono | S_Mid | S_Center);
        add(K_Frame, chip);
        int tx = a.left + S(52);
        // Right side: buttons, then one short status.
        int bx = rowRight;
        Item& xb = add(K_Icon, RECT{bx - S(26), line + S(3), bx, line + rh - S(3)}, L"", S_Click);
        xb.value = I_Close;
        xb.click = [this, id, active] {
            if (active) queue_.cancel(id);
            else queue_.remove(id);
        };
        bx -= S(26) + S(8);
        auto rowButton = [&](const std::wstring& label, std::function<void()> fn, unsigned st = 0) {
            int w = tw(label, st) + S(20);
            bx -= w;
            Item& b = add(K_Button, RECT{bx, line + S(3), bx + w, line + rh - S(3)}, label, st | S_Click);
            b.click = std::move(fn);
            bx -= S(8);
        };
        std::wstring status;
        unsigned statusStyle = S_Mid | S_Mono;
        if (t.state == TaskState::Running) {
            rowButton(L"pause", [this, id] { queue_.pause(id); });
            if (t.progress >= 0) status = std::to_wstring((int)(t.progress * 100)) + L"%";
            if (!t.speed.empty()) status += (status.empty() ? L"" : L"   ") + t.speed;
            if (t.eta >= 0) status += (status.empty() ? L"" : L"   ") + fmtDuration(t.eta);
            statusStyle = S_Mono;
        } else if (t.state == TaskState::Waiting) {
            rowButton(L"pause", [this, id] { queue_.pause(id); });
            status = t.stage.empty() || t.stage == L"starting" ? L"queued" : t.stage;
        } else if (t.state == TaskState::Paused) {
            rowButton(L"resume", [this, id] { queue_.resume(id); });
            status = t.progress >= 0 ? L"paused " + std::to_wstring((int)(t.progress * 100)) + L"%" : L"paused";
        } else if (t.state == TaskState::Failed || t.state == TaskState::Canceled) {
            rowButton(L"retry", [this, id] { queue_.retry(id); });
            status = t.state == TaskState::Failed ? L"failed" : L"canceled";
            statusStyle = t.state == TaskState::Failed ? (S_Bold | S_Mono) : (S_Mid | S_Mono);
        } else if (t.state == TaskState::Done) {
            if (!t.output.empty()) {
                std::wstring out = t.output;
                rowButton(L"show", [this, out] { openFolder(out); });
            }
            status = classTag(t.cls);
            statusStyle = S_Mono | (t.cls == QualityClass::Lossy ? S_Bold : S_Mid);
        }
        int sw = tw(status, statusStyle);
        bx -= sw + S(4);
        add(K_Text, RECT{bx, line, bx + sw, line + rh}, status, statusStyle);
        bx -= S(16);
        // Title, then the current step while running.
        int titleMax = std::max<int>(S(80), bx - tx);
        int tend = text(tx, line, t.title, t.state == TaskState::Running ? S_Bold : 0, titleMax);
        if (t.state == TaskState::Running && !t.stage.empty() && bx - tend > S(60))
            add(K_Text, RECT{tend + S(12), line, bx, line + rh}, t.stage, S_Mid);
        // Thin progress line under running and paused tasks.
        if (active || (t.state == TaskState::Waiting && t.progress >= 0)) {
            Item& bar = add(K_Progress, RECT{tx, line + rh - S(1), rowRight - S(2), line + rh + S(5)});
            bar.value = t.progress;
        }
        if (sel) {
            int yy = ry + rowHt;
            std::wstring l1;
            if (t.state == TaskState::Failed) l1 = t.error + (t.hint.empty() ? L"" : L"   " + t.hint);
            else if (!t.result.empty()) l1 = t.result;
            else l1 = t.stage.empty() ? std::wstring(stateName(t.state)) : t.stage;
            add(K_Text, RECT{tx, yy, rowRight, yy + denseH()}, l1, (t.state == TaskState::Failed ? S_Bold : S_Mid) | S_Mono);
            yy += denseH() + S(2);
            int xx = tx;
            xx = button(xx, yy, L"log", [this] {
                overlay_ = Overlay::Log;
                logApp_ = false;
                logScroll_ = 0;
                relayout(true);
            });
            if (!t.output.empty()) {
                std::wstring out = t.output;
                if (t.state == TaskState::Done) {
                    xx = button(xx, yy, L"play", [this, out] { ShellExecuteW(hwnd_, nullptr, out.c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
                    xx = button(xx, yy, L"convert", [this, out] { addFiles({out}, 1); });
                    xx = button(xx, yy, L"remux", [this, out] { addFiles({out}, 2); });
                }
                text(xx + S(8), yy, t.output, S_Mid, rowRight - xx - S(8));
            }
        }
        ry += h;
    }
    endClip();
    scrollbar(list, queueScroll_, totalH);
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
    int rx = iconButton(a.right, y, I_Close, false, [this] {
        overlay_ = Overlay::None;
        relayout(true);
    }) - S(12);
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
        flash_ = L"log copied";
        SetTimer(hwnd_, T_FLASH, 2000, nullptr);
        relayout();
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
    int rx = iconButton(a.right, y, I_Close, false, [this] {
        overlay_ = Overlay::None;
        relayout(true);
    }) - S(12);
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
    iconButton(a.right, y, I_Close, false, [this] {
        overlay_ = Overlay::None;
        relayout(true);
    });
    y += rh + S(4);
    RECT view{a.left, y, a.right, a.bottom};
    // Two columns: what the setting is, and its control. Kept to a readable width on wide windows.
    int colL = a.left + S(4);
    int colC = a.left + std::min<int>(S(230), (a.right - a.left) / 3);
    int maxR = std::min<int>(a.right - S(12), colC + S(760));
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
    section(L"QUEUE");
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
                wchar_t buf[MAX_PATH] = L"";
                OPENFILENAMEW of{sizeof of};
                of.hwndOwner = hwnd_;
                of.lpstrFilter = L"cookies.txt\0*.txt\0All files\0*.*\0";
                of.lpstrFile = buf;
                of.nMaxFile = MAX_PATH;
                of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&of)) upd([&](Settings& st) { st.cookiesFile = buf; st.cookiesBrowser.clear(); });
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
        x = button(x, y, L"check now", [this] {
            if (updateThread_.joinable()) updateThread_.join();
            updateThread_ = std::thread([this] {
                updater_.checkAndStage(stop_);
                PostMessageW(hwnd_, WM_APP_UPDATE, 0, 0);
            });
            relayout();
        }, us.checking ? S_Disabled : 0);
        text(x + S(4), y, us.checking ? L"checking..." : us.error.empty() ? us.status : us.error, us.error.empty() ? S_Mid : S_Bold, maxR - x);
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
                workers_.emplace_back([this, d] {
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
                flash_ = note2;
                SetTimer(hwnd_, T_FLASH, 5000, nullptr);
                relayout();
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
