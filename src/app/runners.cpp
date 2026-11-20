#include "runners.h"

#include <algorithm>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <map>

#include "../core/util.h"
#include "../dl/ytdlp.h"
#include "../media/probe.h"

namespace ea {

std::wstring tmpDir() {
    std::wstring d = joinPath(dataDir(), L"tmp");
    makeDirs(d);
    return d;
}

void cleanTmp() {
    std::wstring d = tmpDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(d, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER n{{now.dwLowDateTime, now.dwHighDateTime}};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER t{{fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}};
        if (n.QuadPart - t.QuadPart > 2ull * 24 * 3600 * 10000000) DeleteFileW(joinPath(d, fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

json::Value makeDownloadSpec(const Settings& s, const std::wstring& url, const std::wstring& infoFile, const std::wstring& a,
                             const std::wstring& b) {
    json::Value v = json::Value::object();
    v.set("url", url);
    v.set("out", (int)s.dlOut);
    // At equal quality an H.264 source avoids a re-encode for an H.264 target; Original takes the best codec.
    v.set("preferH264", (s.dlOut == DlOut::MP4 || s.dlOut == DlOut::MOV) && !s.dlHevc);
    v.set("metadata", s.metadata);
    v.set("chapters", s.chapters);
    v.set("thumbnail", s.thumbnail);
    v.set("subs", s.subs);
    v.set("outDir", s.dlDir);
    v.set("keepAudioSource", s.keepAudioSource);
    v.set("crf", s.dlCrf);
    v.set("hevc", s.dlHevc);
    if (!infoFile.empty()) {
        v.set("infoFile", infoFile);
        v.set("infoTime", (long long)nowUnix());
    }
    if (!a.empty()) v.set("sectionStart", a);
    if (!b.empty()) v.set("sectionEnd", b);
    return v;
}

bool parseFfProgress(const std::string& line, double& outSec, double& speed, bool& end) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    std::string k = line.substr(0, eq), v = trim(line.substr(eq + 1));
    if (k == "out_time_us" || k == "out_time_ms") {  // both are microseconds in FFmpeg
        char* e = nullptr;
        double us = strtod(v.c_str(), &e);
        if (e != v.c_str() && us >= 0) {
            outSec = us / 1e6;
            return true;
        }
    } else if (k == "speed") {
        double x = atof(v.c_str());
        if (x > 0) {
            speed = x;
            return true;
        }
    } else if (k == "progress") {
        end = v == "end";
        return true;
    }
    return false;
}

bool isHwEncoderFailure(const std::string& t) {
    std::string l = lower(t);
    const char* keys[] = {"nvenc", "openencodesessionex", "no capable devices", "nvcuda", "cuda", "mfx", "qsv", "amf", "error while opening encoder",
                          "could not open encoder", "error initializing output stream", "hardware", "d3d11", "device creation failed"};
    for (auto k : keys)
        if (l.find(k) != std::string::npos) return true;
    return false;
}

// Releases a dependency when the runner leaves, so updates can swap it.
struct Hold {
    Deps& d;
    Dep dep;
    bool held = false;
    Hold(Deps& deps, Dep x) : d(deps), dep(x) {
        d.acquire(dep);
        held = true;
    }
    void drop() {
        if (held) d.release(dep);
        held = false;
    }
    ~Hold() { drop(); }
};

static RunResult fail(const std::wstring& e, const std::wstring& hint = L"") {
    RunResult r;
    r.error = e;
    r.hint = hint;
    return r;
}

static bool waitTools(RunCtx& c, std::initializer_list<Dep> deps) {
    for (Dep d : deps) {
        if (c.deps.ready(d)) continue;
        c.stage(std::wstring(L"waiting for ") + depName(d) + L" to install");
        if (!c.deps.waitReady(d, c.t.cancel)) return false;
    }
    return true;
}

static void deleteDownloadLeftovers(const std::wstring& dir, const std::wstring& stemName) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(dir, stemName + L".*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        std::wstring ln = lower(n);
        bool partial = ln.size() > 5 && (ln.find(L".part") != std::wstring::npos || ln.find(L".ytdl") != std::wstring::npos || ln.find(L".temp.") != std::wstring::npos);
        if (partial) DeleteFileW(joinPath(dir, n).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Checks the written file against the plan; fills `summary` with what was confirmed.
static bool verify(RunCtx& c, const std::wstring& path, int expectStreams, double expectDur, const std::vector<std::string>& expectCodecs,
                   MediaInfo& out, std::wstring& summary, std::wstring& err) {
    if (!probeFile(c.deps.ffprobe(), path, out, &c.t.cancel)) {
        err = L"Output does not open: " + out.error;
        return false;
    }
    int n = 0;
    for (auto& s : out.streams)
        if (s.type != "data") n++;
    if (expectStreams > 0 && n < expectStreams) {
        err = L"Output has " + std::to_wstring(n) + L" streams, expected " + std::to_wstring(expectStreams);
        return false;
    }
    for (auto& codec : expectCodecs) {
        bool found = false;
        for (auto& s : out.streams) found = found || s.codec == codec;
        if (!found) {
            err = L"Output is missing the " + widen(codec) + L" stream";
            return false;
        }
    }
    if (expectDur > 1 && out.duration > 0) {
        double tol = std::max(1.5, expectDur * 0.02);
        if (std::fabs(out.duration - expectDur) > tol) {
            err = L"Output duration " + fmtDuration(out.duration) + L" differs from the source " + fmtDuration(expectDur);
            return false;
        }
    }
    summary = L"verified: " + std::to_wstring(n) + L" streams, " + fmtDuration(out.duration) + L", " + fmtBytes((double)fileSize(path));
    return true;
}

static bool finalizeTmp(const std::wstring& tmp, std::wstring& finalPath) {
    // Never overwrite: if the name was taken meanwhile, pick the next free one.
    for (int i = 0; i < 5; i++) {
        if (!fileExists(finalPath) && MoveFileExW(tmp.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        DeleteFileW((finalPath + L".tmp.reserve").c_str());
        finalPath = uniquePath(finalPath);
    }
    return false;
}

static std::wstring lastLines(const std::string& text, int n) {
    auto lines = split(text, '\n');
    std::string r;
    int start = std::max(0, (int)lines.size() - n);
    for (int i = start; i < (int)lines.size(); i++)
        if (!trim(lines[i]).empty()) r += trim(lines[i]) + " ";
    return widen(trim(r));
}

// Runs FFmpeg for a planned job: progress, cancel, verification and the final rename.
static RunResult runFfmpegPlan(RunCtx& c, const std::wstring& input, const MediaInfo& src, const Plan& plan, std::wstring outPath, double expectDur,
                               std::wstring& finalPath, MediaInfo& outInfo) {
    std::wstring tmp = outPath + L".tmp";
    DeleteFileW(tmp.c_str());
    double need = plan.estBytes > 0 ? plan.estBytes * 1.1 : (double)src.size;
    uint64_t free = freeSpace(dirName(outPath));
    if (free != UINT64_MAX && need > (double)free)
        return fail(L"Not enough disk space: needs about " + fmtBytes(need) + L", " + fmtBytes((double)free) + L" free",
                    L"Free space or pick another output folder, then Retry.");
    std::vector<std::wstring> args = {L"-hide_banner", L"-nostdin", L"-y", L"-v", L"error", L"-progress", L"pipe:1", L"-nostats", L"-stats_period", L"0.5", L"-i", input};
    args.insert(args.end(), plan.args.begin(), plan.args.end());
    args.push_back(tmp);
    double dur = expectDur > 0 ? expectDur : src.duration;
    double outSec = 0, speed = 0;
    bool end = false;
    std::string errText;
    std::mutex em;
    ProcOpts opts;
    opts.lowPriority = c.settings.lowPriority;
    ULONGLONG t0 = GetTickCount64();
    std::wstring startErr;
    int code = c.run(c.deps.ffmpeg(), args, opts,
                     [&](const std::string& line, bool isErr) {
                         if (isErr) {
                             std::lock_guard<std::mutex> lk(em);
                             if (errText.size() < 65536) errText += line + "\n";
                             c.log(widen(line));
                             return;
                         }
                         if (parseFfProgress(line, outSec, speed, end) && startsWith(line, "progress")) {
                             double p = dur > 0 ? std::clamp(outSec / dur, 0.0, 1.0) : -1;
                             double eta = (speed > 0 && dur > 0) ? (dur - outSec) / speed : -1;
                             std::wstring sp = speed > 0 ? fmtFixed(speed, speed < 10 ? 2 : 1) + L"x" : L"";
                             c.progress(p, eta, sp);
                         }
                     },
                     &startErr);
    if (c.canceled()) {
        DeleteFileW(tmp.c_str());
        return fail(L"Canceled");
    }
    if (code != 0) {
        DeleteFileW(tmp.c_str());
        RunResult r = fail(startErr.empty() ? L"FFmpeg failed: " + lastLines(errText, 4) : startErr);
        std::string l = lower(errText);
        if (contains(l, "no space left") || contains(l, "disk full")) r.hint = L"The disk is full. Free space, then Retry.";
        else if (contains(l, "invalid data found") || contains(l, "moov atom not found")) r.hint = L"The source file is damaged or incomplete.";
        else if (contains(l, "permission denied")) r.hint = L"The output folder is not writable. Pick another folder.";
        else r.hint = L"Open the log for FFmpeg's full message.";
        if (!plan.encoder.empty() && isHwEncoderFailure(errText) && !c.t.triedSoftware && (contains(narrow(plan.encoder), "nvenc") ||
                                                                                           contains(narrow(plan.encoder), "qsv") || contains(narrow(plan.encoder), "amf"))) {
            c.t.triedSoftware = true;
            c.log(L"Hardware encoder failed; retrying with the software encoder.");
            r.retryNow = true;
        }
        return r;
    }
    c.stage(L"verifying");
    std::vector<std::string> codecs;
    for (int idx : plan.copiedSourceStreams)
        for (auto& s : src.streams)
            if (s.index == idx) codecs.push_back(s.codec);
    std::wstring summary, verr;
    if (!verify(c, tmp, plan.outStreams, dur, codecs, outInfo, summary, verr)) {
        DeleteFileW(tmp.c_str());
        return fail(L"Verification failed: " + verr, L"Nothing was kept. Open the log, then Retry.");
    }
    finalPath = outPath;
    if (!finalizeTmp(tmp, finalPath)) {
        DeleteFileW(tmp.c_str());
        return fail(L"Could not rename the finished file into place.");
    }
    double secs = (GetTickCount64() - t0) / 1000.0;
    RunResult r;
    r.ok = true;
    r.result = summary + L" in " + fmtDuration(secs);
    return r;
}

// ---------- download ----------

RunResult runDownload(RunCtx& c) {
    json::Value sp = c.spec();  // copy: the UI thread reads the task's spec under the queue lock
    std::wstring url = sp["url"].wstr();
    DlOut out = (DlOut)std::clamp((int)sp["out"].num(0), 0, 5);
    if (!waitTools(c, {Dep::YtDlp, Dep::FFmpeg, Dep::Deno})) return fail(L"Canceled");
    Hold hy(c.deps, Dep::YtDlp), hf(c.deps, Dep::FFmpeg), hd(c.deps, Dep::Deno);
    DlOptions o;
    o.cookiesBrowser = c.settings.cookiesBrowser;
    o.cookiesFile = c.settings.cookiesFile;
    o.deno = c.deps.deno();
    o.ffmpegDir = c.deps.ffmpegDir();
    ProcOpts popts;
    popts.env = {{L"PYTHONIOENCODING", L"utf-8"}, {L"PYTHONUTF8", L"1"}};
    popts.pathPrepend = {c.deps.ffmpegDir(), dirName(c.deps.deno())};

    auto handleYtError = [&](const std::string& errText) -> RunResult {
        std::wstring msg, hint;
        DlError e = classifyError(errText, msg, hint);
        RunResult r = fail(msg.empty() ? L"yt-dlp failed" : msg, hint);
        if (e == DlError::Extraction && !c.t.triedUpdate) {
            c.t.triedUpdate = true;
            c.stage(L"site changed: updating yt-dlp");
            hy.drop();
            std::wstring note;
            bool changed = c.deps.updateYtDlpNow(c.settings.nightlyFallback, note);
            c.log(note);
            if (changed) {
                c.specSet("infoFile", "");
                r.retryNow = true;
            } else {
                r.hint = L"yt-dlp is already the newest build and still cannot read this page. Try again later; the fix usually ships within days.";
            }
        } else if (e == DlError::Network) {
            r.retryAfterMs = 5000 * (c.t.autoRetries + 1);
        } else if (e == DlError::Format && !c.t.triedUpdate) {
            c.t.triedUpdate = true;
            c.specSet("infoFile", "");
            r.retryNow = true;
        }
        return r;
    };

    // 1. Info (reuse the analysis if it is fresh: format URLs expire after a few hours).
    std::string infoText;
    std::wstring infoFile = sp["infoFile"].wstr();
    bool fresh = !infoFile.empty() && fileExists(infoFile) && nowUnix() - sp["infoTime"].integer(0) < 1800 && readFile(infoFile, infoText);
    if (!fresh) {
        c.stage(L"reading video info");
        std::string errText;
        std::mutex em;
        std::wstring startErr;
        int code = c.run(c.deps.ytdlp(), buildInfoArgs(url, false, o.deno, &o), popts,
                         [&](const std::string& line, bool) {
                             std::lock_guard<std::mutex> lk(em);
                             errText += line + "\n";
                             c.log(widen(line));
                         },
                         &startErr, &infoText);
        if (c.canceled()) return fail(L"Canceled");
        if (code != 0) return startErr.empty() ? handleYtError(errText) : fail(startErr);
        infoFile = joinPath(tmpDir(), L"info-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(c.t.id) + L".json");
        writeFileAtomic(infoFile, infoText);
        c.specSet("infoFile", infoFile);
        c.specSet("infoTime", (long long)nowUnix());
    }
    YtResult yr = parseYtJson(infoText);
    if (!yr.ok) return fail(yr.error);
    if (yr.playlist) return fail(L"This link is a playlist; analyse it to choose entries.");
    DlPrefs prefs;
    prefs.out = out;
    prefs.preferH264 = sp["preferH264"].boolean(true);
    prefs.crf = std::clamp((int)sp["crf"].num(16), 0, 51);
    prefs.hevc = sp["hevc"].boolean(false);
    Selection sel = selectFormats(yr.info, prefs);
    if (!sel.ok) return fail(sel.error);
    c.setTitle(widen(yr.info.title));
    c.log(L"formats: " + widen(sel.formatSpec()) + L"  " + sel.summary);
    for (auto& n : sel.notes) c.log(n);

    // 2. Output name, fixed on the first run so a resume continues the same .part files.
    std::wstring outDir = sp["outDir"].wstr();
    if (outDir.empty()) outDir = c.settings.dlDir;
    if (!makeDirs(outDir)) return fail(L"Cannot create the output folder " + outDir);
    std::wstring finalExt = sel.finalExt.empty() ? sel.ext : sel.finalExt;
    std::wstring secA = sp["sectionStart"].wstr(), secB = sp["sectionEnd"].wstr();
    std::wstring stemName = sp["stem"].wstr();
    if (stemName.empty()) {
        std::wstring base = sanitizeFileName(widen(yr.info.title));
        if (base.size() > 120) base.resize(120);
        base += L" [" + sanitizeFileName(widen(yr.info.id)) + L"]";
        if (!secA.empty() || !secB.empty()) base += L" (" + replaceAll(secA.empty() ? L"0" : secA, L":", L".") + L"-" + replaceAll(secB.empty() ? L"end" : secB, L":", L".") + L")";
        std::wstring cand = base;
        for (int i = 1; i < 10000; i++) {
            bool taken = fileExists(joinPath(outDir, cand + finalExt)) || fileExists(joinPath(outDir, cand + L".src" + sel.ext));
            if (!taken) break;
            cand = base + L" (" + std::to_wstring(i) + L")";
        }
        stemName = cand;
        c.specSet("stem", stemName);
        c.saveSpec();
    }
    // 3. Disk space: the parts plus the merged file.
    double need = sel.estBytes * 2.1;
    if (out == DlOut::AudioWAV) need += yr.info.duration * 48000 * 2 * 4;
    uint64_t freeB = freeSpace(outDir);
    if (need > 0 && freeB != UINT64_MAX && need > (double)freeB)
        return fail(L"Not enough disk space: needs about " + fmtBytes(need) + L", " + fmtBytes((double)freeB) + L" free", L"Free space or change the folder, then Retry.");

    // 4. Download (bit-for-bit; merging and remuxing copy streams).
    o.url = url;
    o.infoJsonPath = infoFile;
    o.outDir = outDir;
    // A download that is converted afterwards gets a ".src" name so the result can take the plain one.
    std::wstring dlStem = sel.needsConvert ? stemName + L".src" : stemName;
    o.outStem = dlStem;
    o.sel = sel;
    o.audioExtract = dlOutIsAudio(out);
    o.metadata = sp["metadata"].boolean(true);
    o.chapters = sp["chapters"].boolean(true) && yr.info.chapters > 0;
    o.thumbnail = sp["thumbnail"].boolean(false);
    o.subs = sp["subs"].boolean(false) && yr.info.hasSubs;
    o.fragments = c.settings.fragments;
    o.sectionStart = secA;
    o.sectionEnd = secB;
    o.pathFile = joinPath(tmpDir(), L"path-" + std::to_wstring(c.t.id) + L"-" + std::to_wstring(GetTickCount64()) + L".txt");
    DeleteFileW(o.pathFile.c_str());
    c.setClass(QualityClass::Copy);
    c.stage(L"downloading");
    std::map<std::string, std::pair<double, double>> parts;  // format id -> done, total
    double vBytes = sel.v.bytes(yr.info.duration), aBytes = sel.audioId.empty() ? 0 : sel.a.bytes(yr.info.duration);
    std::string errText;
    std::mutex em;
    std::wstring startErr;
    int code = c.run(c.deps.ytdlp(), buildDownloadArgs(o), popts,
                     [&](const std::string& line, bool isErr) {
                         DlProgress p;
                         if (parseProgressLine(line, p)) {
                             if (p.isDownload) {
                                 parts[p.formatId] = {p.done, p.total};
                                 double done = 0, total = 0;
                                 bool known = true;
                                 for (auto& kv : parts) {
                                     done += std::max(0.0, kv.second.first);
                                     if (kv.second.second > 0) total += kv.second.second;
                                     else known = false;
                                 }
                                 // Formats not started yet still count toward the total.
                                 if (!sel.audioId.empty() && parts.size() < 2) total += parts.count(sel.videoId) ? aBytes : vBytes;
                                 double frac = (known && total > 0) ? std::clamp(done / total, 0.0, 1.0) : -1;
                                 std::wstring speed = p.speed > 0 ? fmtBytes(p.speed) + L"/s" : L"";
                                 double eta = (p.speed > 0 && total > done) ? (total - done) / p.speed : p.eta;
                                 if (frac < 0 && p.done > 0) speed = fmtBytes(done) + (speed.empty() ? L"" : L"  " + speed);
                                 c.progress(frac, eta, speed);
                             } else if (p.isPost && p.status == "started") {
                                 std::wstring pp = widen(p.postprocessor);
                                 c.stage(pp == L"Merger" ? L"merging streams (copy)" : pp == L"ExtractAudio" ? L"extracting audio (copy)"
                                                         : pp == L"FFmpegMetadata" ? L"writing metadata (copy)" : L"finishing: " + pp);
                             }
                             return;
                         }
                         if (isErr || startsWith(line, "ERROR") || startsWith(line, "WARNING")) {
                             std::lock_guard<std::mutex> lk(em);
                             if (errText.size() < 65536) errText += line + "\n";
                         }
                         c.log(widen(line));
                     },
                     &startErr);
    if (c.canceled()) {
        // A user cancel discards the partial files; an app shutdown keeps them so the next start resumes.
        if (!c.interrupted()) deleteDownloadLeftovers(outDir, dlStem);
        return fail(L"Canceled");
    }
    if (code != 0) return startErr.empty() ? handleYtError(errText) : fail(startErr);

    // 5. Locate and verify.
    std::string pathText;
    std::wstring got;
    if (readFile(o.pathFile, pathText)) {
        auto lines = split(pathText, '\n');
        for (auto& l : lines)
            if (!trim(l).empty()) got = widen(trim(l));
    }
    DeleteFileW(o.pathFile.c_str());
    if (got.empty() || !fileExists(got)) got = joinPath(outDir, dlStem + sel.ext);
    if (!fileExists(got)) return fail(L"yt-dlp finished but the file is missing", L"Open the log; Retry downloads it again.");
    c.setOutput(got);
    c.stage(L"verifying");
    MediaInfo mi;
    std::vector<std::string> codecs;
    std::string vc = normalizeYtCodec(sel.v.vcodec), ac = normalizeYtCodec(sel.a.acodec);
    if (!o.audioExtract && !vc.empty() && vc != "unknown") codecs.push_back(vc);
    if ((o.audioExtract || !sel.audioId.empty()) && !ac.empty() && ac != "unknown") codecs.push_back(ac);
    double expDur = (secA.empty() && secB.empty()) ? yr.info.duration : 0;
    std::wstring summary, verr;
    if (!verify(c, got, (int)codecs.size(), expDur, codecs, mi, summary, verr)) return fail(L"Verification failed: " + verr, L"The file was kept for inspection: " + got);
    if (!o.audioExtract && sel.v.width && mi.video() && (mi.video()->width != sel.v.width || mi.video()->height != sel.v.height))
        c.log(L"note: stream is " + std::to_wstring(mi.video()->width) + L"x" + std::to_wstring(mi.video()->height) + L", listed as " +
              std::to_wstring(sel.v.width) + L"x" + std::to_wstring(sel.v.height));
    std::wstring finalPath = got;
    RunResult r;
    r.ok = true;
    r.result = L"bit-for-bit " + sel.summary + L"; " + summary;

    // 6a. MP4 (H.264 CRF 16 + AAC, copying what already fits) or MOV (truly lossless).
    if (out == DlOut::MP4 || out == DlOut::MOV) {
        ConvertSpec cs;
        cs.container = out == DlOut::MP4 ? Container::MP4 : Container::MOV;
        cs.keepSubs = true;
        const StreamInfo* v = mi.video();
        const StreamInfo* au = mi.audio();
        const char* wantCodec = prefs.hevc ? "hevc" : "h264";
        cs.vcodec = v && v->codec == wantCodec ? VCodec::Copy : prefs.hevc ? VCodec::H265 : VCodec::H264;
        cs.rate = RateMode::Quality;
        cs.quality = prefs.crf;
        cs.speed = "slow";  // slow for downloads: best quality per bit at these CRFs (NVENC p6)
        cs.accel = c.t.triedSoftware ? Accel::Software : c.settings.encoder;  // fastest encoder unless Settings says otherwise
        if (out == DlOut::MP4) {
            cs.acodec = !au || au->codec == "aac" ? ACodec::Copy : ACodec::AAC;
            int ch = au ? std::max(2, au->channels) : 2;
            cs.aacKbps = std::min(512, ch <= 2 ? 320 : ch * 80);
        } else {
            cs.acodec = ACodec::Copy;  // planConvert decodes to exact PCM what MOV cannot copy
        }
        Plan plan = planConvert(mi, cs, c.caps);
        if (!plan.ok) return fail(plan.error);
        c.setClass(plan.cls);
        for (auto& l : plan.lines) c.log(L"plan: " + l);
        for (auto& w : plan.warnings) c.log(L"note: " + w);
        c.stage(plan.cls == QualityClass::Copy ? L"remuxing (copy)" : cs.vcodec == VCodec::Copy ? L"converting audio" : std::wstring(prefs.hevc ? L"encoding H.265 CRF " : L"encoding H.264 CRF ") + std::to_wstring(prefs.crf));
        std::wstring target = joinPath(outDir, stemName + plan.ext);
        if (fileExists(target)) target = uniquePath(target);
        MediaInfo outInfo;
        RunResult rr = runFfmpegPlan(c, got, mi, plan, target, mi.duration, finalPath, outInfo);
        if (!rr.ok) return rr;
        if (!sp["keepAudioSource"].boolean(false)) DeleteFileW(got.c_str());
        c.setOutput(finalPath);
        std::wstring cls = className(plan.cls);
        r.result = cls.substr(0, cls.find(L" -")) + L": " + sel.summary + L"; " + rr.result;
    }

    // 6b. Optional audio conversion (MP3 is a labelled lossy step; WAV is an exact decode).
    if (out == DlOut::AudioMP3 || out == DlOut::AudioWAV) {
        ConvertSpec cs;
        cs.audio = out == DlOut::AudioMP3 ? AudioTarget::MP3 : AudioTarget::WAV;
        cs.mp3Vbr = false;
        cs.mp3Kbps = 320;
        Plan plan = planConvert(mi, cs, EncoderCaps());
        if (!plan.ok) return fail(plan.error);
        c.setClass(plan.cls);
        c.stage(out == DlOut::AudioMP3 ? L"encoding MP3 320 kb/s (lossy)" : L"decoding to WAV (exact)");
        for (auto& w : plan.warnings) c.log(w);
        std::wstring target = joinPath(outDir, stemName + plan.ext);
        if (fileExists(target)) target = uniquePath(target);
        MediaInfo outInfo;
        RunResult rr = runFfmpegPlan(c, got, mi, plan, target, mi.duration, finalPath, outInfo);
        if (!rr.ok) return rr;
        if (!sp["keepAudioSource"].boolean(false)) DeleteFileW(got.c_str());
        c.setOutput(finalPath);
        r.result = (plan.cls == QualityClass::Copy ? L"copied " : out == DlOut::AudioMP3 ? L"MP3 320 kb/s CBR (lossy) from " : L"WAV exact decode of ") +
                   sel.summary + L"; " + rr.result;
    }
    if (!infoFile.empty()) DeleteFileW(infoFile.c_str());
    appendHistory({nowStamp(), widen(yr.info.title), url, finalPath});
    return r;
}

// ---------- convert and remux ----------

static RunResult runEncodeTask(RunCtx& c, bool remux) {
    json::Value sp = c.spec();
    std::wstring input = sp["input"].wstr();
    if (!fileExists(input)) return fail(L"Source file not found: " + input, L"It was moved or deleted.");
    if (!waitTools(c, {Dep::FFmpeg})) return fail(L"Canceled");
    Hold hf(c.deps, Dep::FFmpeg);
    c.stage(L"analysing");
    MediaInfo mi;
    if (!probeFile(c.deps.ffprobe(), input, mi, &c.t.cancel)) return fail(mi.error);
    Plan plan;
    if (remux) {
        plan = planRemux(mi, RemuxSpec::fromJson(sp["remux"]));
    } else {
        ConvertSpec cs = ConvertSpec::fromJson(sp["convert"]);
        cs.accel = c.t.triedSoftware ? Accel::Software : c.settings.encoder;  // the encoder is a global setting
        plan = planConvert(mi, cs, c.caps);
    }
    if (!plan.ok) return fail(plan.error);
    c.setClass(plan.cls);
    for (auto& l : plan.lines) c.log(L"plan: " + l);
    for (auto& w : plan.warnings) c.log(L"note: " + w);
    std::wstring outDir = sp["outDir"].wstr();
    if (outDir.empty()) outDir = dirName(input);
    if (!makeDirs(outDir)) return fail(L"Cannot create the output folder " + outDir);
    std::wstring outPath = uniquePath(joinPath(outDir, stem(input) + plan.ext));
    c.setOutput(outPath);
    c.stage(remux ? L"remuxing" : (plan.cls == QualityClass::Copy ? L"copying" : L"encoding" + (plan.encoder.empty() ? L"" : L" (" + plan.encoder + L")")));
    std::wstring finalPath;
    MediaInfo outInfo;
    RunResult r = runFfmpegPlan(c, input, mi, plan, outPath, mi.duration, finalPath, outInfo);
    if (r.ok) {
        c.setOutput(finalPath);
        r.result = std::wstring(className(plan.cls)).substr(0, std::wstring(className(plan.cls)).find(L" -")) + L"; " + r.result;
    }
    return r;
}

RunResult runConvert(RunCtx& c) { return runEncodeTask(c, false); }
RunResult runRemux(RunCtx& c) { return runEncodeTask(c, true); }

}  // namespace ea
