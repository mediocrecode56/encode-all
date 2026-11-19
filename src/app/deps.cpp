#include "deps.h"

#include <algorithm>
#include <windows.h>

#include "../core/json.h"
#include "../core/net.h"
#include "../core/process.h"
#include "../core/util.h"
#include "settings.h"

namespace ea {

const wchar_t* depName(Dep d) {
    switch (d) {
        case Dep::YtDlp: return L"yt-dlp";
        case Dep::FFmpeg: return L"FFmpeg";
        case Dep::Deno: return L"Deno";
    }
    return L"";
}

static const wchar_t* folder(Dep d) {
    switch (d) {
        case Dep::YtDlp: return L"yt-dlp";
        case Dep::FFmpeg: return L"ffmpeg";
        case Dep::Deno: return L"deno";
    }
    return L"";
}

static const int64_t kDay = 86400;

std::string extractHash(const std::string& text, const std::string& name) {
    if (!name.empty()) {
        std::string h = findHash(text, name);
        if (!h.empty()) return h;
    }
    // Deno publishes PowerShell Get-FileHash output: "Hash : CBA8...". Take the first 64-hex run.
    std::string run;
    for (char c : text + " ") {
        bool hx = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (hx) run += c;
        else {
            if (run.size() == 64) return lower(run);
            run.clear();
        }
    }
    return {};
}

std::wstring Deps::root(Dep d) const { return joinPath(joinPath(dir_, L"deps"), folder(d)); }

std::wstring Deps::exeIn(Dep d, const std::wstring& dir) const {
    switch (d) {
        case Dep::YtDlp: return joinPath(dir, L"yt-dlp.exe");
        case Dep::FFmpeg: return joinPath(dir, L"ffmpeg.exe");
        case Dep::Deno: return joinPath(dir, L"deno.exe");
    }
    return L"";
}

std::wstring Deps::ytdlp() const { return exeIn(Dep::YtDlp, joinPath(root(Dep::YtDlp), L"current")); }
std::wstring Deps::ffmpeg() const { return exeIn(Dep::FFmpeg, joinPath(root(Dep::FFmpeg), L"current")); }
std::wstring Deps::ffprobe() const { return joinPath(joinPath(root(Dep::FFmpeg), L"current"), L"ffprobe.exe"); }
std::wstring Deps::ffmpegDir() const { return joinPath(root(Dep::FFmpeg), L"current"); }
std::wstring Deps::deno() const { return exeIn(Dep::Deno, joinPath(root(Dep::Deno), L"current")); }

std::string Deps::ffmpegHash() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_[1].sha;
}

bool Deps::ready(Dep d) const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_[(int)d].present;
}

DepView Deps::view(Dep d) const {
    std::lock_guard<std::mutex> lk(mu_);
    const State& s = st_[(int)d];
    DepView v;
    v.present = s.present;
    v.busy = s.busy;
    v.progress = s.progress;
    v.version = s.version;
    v.status = s.status;
    v.error = s.error;
    v.channel = s.channel;
    v.checked = s.checked;
    v.hasPrevious = fileExists(exeIn(d, joinPath(root(d), L"previous")));
    return v;
}

void Deps::set(Dep d, const std::function<void(State&)>& f) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        f(st_[(int)d]);
    }
    cv_.notify_all();
    if (notify_) notify_();
}

void Deps::load() {
    std::string t;
    json::Value v;
    if (readFile(joinPath(joinPath(dir_, L"deps"), L"deps.json"), t)) json::parse(t, v);
    for (int i = 0; i < kDeps; i++) {
        Dep d = (Dep)i;
        const json::Value& o = v[narrow(folder(d))];
        State& s = st_[i];
        s.version = o["version"].wstr();
        s.channel = o["channel"].wstr(L"stable");
        s.sha = o["sha"].str();
        s.skipSha = o["skipSha"].str();
        s.checked = o["checked"].integer(0);
        s.installed = o["installed"].integer(0);
        s.present = fileExists(exeIn(d, joinPath(root(d), L"current"))) && (d != Dep::FFmpeg || fileExists(ffprobe()));
        // A staged update left from a previous session is swapped in now: nothing uses it yet.
        std::wstring staged = joinPath(root(d), L"new");
        if (fileExists(exeIn(d, staged)) && o["pendingSha"].isStr()) {
            s.pendingSwap = true;
            s.pendingVersion = o["pendingVersion"].wstr();
            s.pendingSha = o["pendingSha"].str();
        }
    }
}

void Deps::save() {
    json::Value v = json::Value::object();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < kDeps; i++) {
            const State& s = st_[i];
            json::Value o = json::Value::object();
            o.set("version", s.version);
            o.set("channel", s.channel);
            o.set("sha", s.sha);
            o.set("skipSha", s.skipSha);
            o.set("checked", (long long)s.checked);
            o.set("installed", (long long)s.installed);
            if (s.pendingSwap) {
                o.set("pendingVersion", s.pendingVersion);
                o.set("pendingSha", s.pendingSha);
            }
            v.set(narrow(folder((Dep)i)), o);
        }
    }
    makeDirs(joinPath(dir_, L"deps"));
    writeFileAtomic(joinPath(joinPath(dir_, L"deps"), L"deps.json"), json::dump(v, true));
}

void Deps::init(const std::wstring& dataDir, Notify notify) {
    dir_ = dataDir;
    notify_ = std::move(notify);
    load();
    for (int i = 0; i < kDeps; i++)
        if (st_[i].pendingSwap) swapIn((Dep)i);
}

void Deps::shutdown() {
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Deps::acquire(Dep d) {
    std::lock_guard<std::mutex> lk(mu_);
    st_[(int)d].users++;
}

void Deps::release(Dep d) {
    bool swap = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        State& s = st_[(int)d];
        if (s.users > 0) s.users--;
        swap = s.users == 0 && s.pendingSwap;
    }
    if (swap) swapIn(d);
}

bool Deps::waitReady(Dep d, const std::atomic<bool>& cancel) {
    std::unique_lock<std::mutex> lk(mu_);
    while (!st_[(int)d].present) {
        if (cancel || stop_) return false;
        cv_.wait_for(lk, std::chrono::milliseconds(250));
    }
    return true;
}

bool Deps::fetchExpected(Dep d, const std::wstring& channel, std::string& sha, std::wstring& err) {
    std::wstring url;
    std::string name;
    switch (d) {
        case Dep::YtDlp:
            url = channel == L"nightly" ? L"https://github.com/yt-dlp/yt-dlp-nightly-builds/releases/latest/download/SHA2-256SUMS"
                                        : L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS";
            name = "yt-dlp.exe";
            break;
        case Dep::FFmpeg:
            url = L"https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/checksums.sha256";
            name = "ffmpeg-master-latest-win64-gpl.zip";
            break;
        case Dep::Deno:
            url = L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip.sha256sum";
            name = "";
            break;
    }
    std::string body;
    auto r = httpGet(url, &body, L"", nullptr, &stop_, 1 << 20);
    if (!r.ok) {
        err = L"Could not reach GitHub (" + r.error + L")";
        return false;
    }
    sha = extractHash(body, name);
    if (sha.empty()) {
        err = L"No published checksum found";
        return false;
    }
    return true;
}

static std::wstring firstLine(const std::string& s) {
    std::string l = s.substr(0, s.find_first_of("\r\n"));
    return widen(trim(l));
}

bool Deps::smoke(Dep d, const std::wstring& dir, std::wstring& version, std::wstring& err) {
    std::wstring exe = exeIn(d, dir);
    Capture c;
    switch (d) {
        case Dep::YtDlp:
            c = runCapture(exe, {L"--version"}, {}, 60000, &stop_);
            if (c.code == 0) version = firstLine(c.out);
            break;
        case Dep::FFmpeg: {
            c = runCapture(exe, {L"-hide_banner", L"-v", L"error", L"-f", L"lavfi", L"-i", L"testsrc2=s=128x72:d=0.2", L"-c:v", L"libx264", L"-f", L"null", L"-"}, {},
                           60000, &stop_);
            if (c.code != 0) break;
            auto pv = runCapture(joinPath(dir, L"ffprobe.exe"), {L"-version"}, {}, 30000, &stop_);
            if (pv.code != 0) {
                c = pv;
                break;
            }
            c = runCapture(exe, {L"-version"}, {}, 30000, &stop_);
            if (c.code == 0) {
                std::wstring l = firstLine(c.out);  // ffmpeg version N-12345-gabc-20261118 Copyright ...
                auto parts = splitLines(replaceAll(l, L" ", L"\n"));
                version = parts.size() > 2 ? parts[2] : l;
            }
            break;
        }
        case Dep::Deno:
            c = runCapture(exe, {L"--version"}, {}, 60000, &stop_);
            if (c.code == 0) {
                std::wstring l = firstLine(c.out);  // deno 2.9.8 (stable, ...)
                auto parts = splitLines(replaceAll(l, L" ", L"\n"));
                version = parts.size() > 1 ? parts[1] : l;
            }
            break;
    }
    if (c.code != 0 || version.empty()) {
        err = L"Self-test failed" + (c.startError.empty() ? (c.err.empty() ? L"" : L": " + widen(trim(c.err).substr(0, 200))) : L": " + c.startError);
        return false;
    }
    return true;
}

bool Deps::stage(Dep d, const std::wstring& channel, const std::string& sha, std::wstring& version, std::wstring& err) {
    std::wstring r = root(d), staged = joinPath(r, L"new"), dl = joinPath(r, L"download");
    removeTree(staged);
    removeTree(dl);
    makeDirs(staged);
    makeDirs(dl);
    std::wstring url, file;
    switch (d) {
        case Dep::YtDlp:
            url = channel == L"nightly" ? L"https://github.com/yt-dlp/yt-dlp-nightly-builds/releases/latest/download/yt-dlp.exe"
                                        : L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
            file = joinPath(dl, L"yt-dlp.exe");
            break;
        case Dep::FFmpeg:
            url = L"https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
            file = joinPath(dl, L"ffmpeg.zip");
            break;
        case Dep::Deno:
            url = L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip";
            file = joinPath(dl, L"deno.zip");
            break;
    }
    uint64_t last = 0;
    auto res = httpGet(url, nullptr, file,
                       [&](uint64_t done, uint64_t total) {
                           if (done - last < (1u << 20) && done != total) return;
                           last = done;
                           set(d, [&](State& s) {
                               s.progress = total ? (double)done / total : -1;
                               s.status = L"downloading " + fmtBytes((double)done) + (total ? L" / " + fmtBytes((double)total) : L"");
                           });
                       },
                       &stop_);
    if (!res.ok) {
        err = L"Download failed: " + res.error;
        return false;
    }
    set(d, [&](State& s) {
        s.status = L"verifying";
        s.progress = -1;
    });
    std::string got = sha256File(file);
    if (got != sha) {
        err = L"Checksum mismatch (expected " + widen(sha.substr(0, 12)) + L"..., got " + widen(got.substr(0, 12)) + L"...). Nothing was changed.";
        removeTree(dl);
        return false;
    }
    if (d == Dep::YtDlp) {
        if (!moveReplace(file, joinPath(staged, L"yt-dlp.exe"))) {
            err = L"Could not stage yt-dlp";
            return false;
        }
    } else {
        set(d, [&](State& s) { s.status = L"unpacking"; });
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring tar = joinPath(sys, L"tar.exe");
        std::wstring unpack = joinPath(dl, L"x");
        makeDirs(unpack);
        auto c = runCapture(tar, {L"-xf", file, L"-C", unpack}, {}, 600000, &stop_);
        if (c.code != 0) {
            err = L"Unpacking failed: " + widen(trim(c.err).substr(0, 200)) + c.startError;
            return false;
        }
        // Find the binaries anywhere in the archive.
        std::vector<std::wstring> want = d == Dep::FFmpeg ? std::vector<std::wstring>{L"ffmpeg.exe", L"ffprobe.exe", L"LICENSE.txt"}
                                                          : std::vector<std::wstring>{L"deno.exe"};
        std::function<void(const std::wstring&)> walk = [&](const std::wstring& dir) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                std::wstring p = joinPath(dir, n);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(p);
                else
                    for (auto& w : want)
                        if (lower(n) == lower(w) && !fileExists(joinPath(staged, w))) moveReplace(p, joinPath(staged, w));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        };
        walk(unpack);
        if (!fileExists(exeIn(d, staged)) || (d == Dep::FFmpeg && !fileExists(joinPath(staged, L"ffprobe.exe")))) {
            err = L"The archive did not contain the expected programs";
            return false;
        }
    }
    removeTree(dl);
    set(d, [&](State& s) { s.status = L"testing"; });
    if (!smoke(d, staged, version, err)) {
        removeTree(staged);
        return false;
    }
    return true;
}

bool Deps::swapIn(Dep d) {
    std::wstring r = root(d), cur = joinPath(r, L"current"), prev = joinPath(r, L"previous"), staged = joinPath(r, L"new");
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_[(int)d].users > 0) return false;
    }
    if (!fileExists(exeIn(d, staged))) return false;
    removeTree(prev);
    bool hadCur = dirExists(cur);
    if (hadCur && !MoveFileExW(cur.c_str(), prev.c_str(), 0)) return false;
    if (!MoveFileExW(staged.c_str(), cur.c_str(), 0)) {
        if (hadCur) MoveFileExW(prev.c_str(), cur.c_str(), 0);  // put the old one back
        return false;
    }
    set(d, [&](State& s) {
        s.present = true;
        s.version = s.pendingVersion;
        s.sha = s.pendingSha;
        s.installed = nowUnix();
        s.pendingSwap = false;
        s.status.clear();
        s.error.clear();
        s.progress = -1;
    });
    save();
    logLine(std::wstring(depName(d)) + L" " + view(d).version + L" installed");
    return true;
}

bool Deps::install(Dep d, bool force, std::wstring& note) {
    std::lock_guard<std::mutex> one(installMu_);
    std::wstring channel;
    std::string curSha, skipSha;
    bool present;
    {
        std::lock_guard<std::mutex> lk(mu_);
        channel = st_[(int)d].channel;
        curSha = st_[(int)d].sha;
        skipSha = st_[(int)d].skipSha;
        present = st_[(int)d].present;
    }
    set(d, [&](State& s) {
        s.busy = true;
        s.error.clear();
        s.status = L"checking";
    });
    std::string sha;
    std::wstring err, version;
    bool changed = false;
    if (!fetchExpected(d, channel, sha, err)) {
        set(d, [&](State& s) {
            s.busy = false;
            s.status.clear();
            s.error = err;
        });
        note = err;
        return false;
    }
    if (present && (sha == curSha || sha == skipSha) && !force) {
        set(d, [&](State& s) {
            s.busy = false;
            s.status.clear();
            s.checked = nowUnix();
        });
        save();
        note = std::wstring(depName(d)) + L" is up to date";
        return false;
    }
    if (present && sha == curSha) {
        set(d, [&](State& s) { s.busy = false; s.status.clear(); s.checked = nowUnix(); });
        save();
        note = std::wstring(depName(d)) + L" is already the latest " + channel;
        return false;
    }
    if (stage(d, channel, sha, version, err)) {
        set(d, [&](State& s) {
            s.pendingSwap = true;
            s.pendingVersion = version;
            s.pendingSha = sha;
            s.checked = nowUnix();
            s.status = L"ready, waiting for running tasks";
        });
        save();
        changed = swapIn(d);
        note = std::wstring(depName(d)) + L" " + version + (changed ? L" installed" : L" staged (applies when idle)");
        set(d, [&](State& s) { s.busy = false; });
        return changed;
    }
    set(d, [&](State& s) {
        s.busy = false;
        s.status.clear();
        s.progress = -1;
        s.error = err;
    });
    logLine(std::wstring(depName(d)) + L" update failed: " + err);
    note = err;
    return false;
}

void Deps::startMaintenance(bool autoUpdate, bool force) {
    std::lock_guard<std::mutex> one(maintMu_);
    if (worker_.joinable()) {
        bool running;
        {
            std::lock_guard<std::mutex> lk(mu_);
            running = st_[0].busy || st_[1].busy || st_[2].busy;
        }
        if (running) return;
        worker_.join();
    }
    worker_ = std::thread([this, autoUpdate, force] {
        // Missing tools first, smallest first so downloads can start early.
        Dep order[] = {Dep::YtDlp, Dep::Deno, Dep::FFmpeg};
        for (Dep d : order) {
            if (stop_) return;
            if (!ready(d)) {
                std::wstring n;
                for (int attempt = 0; attempt < 3 && !ready(d) && !stop_; attempt++) {
                    install(d, false, n);
                    if (!ready(d)) Sleep(3000 * (attempt + 1));
                }
            }
        }
        if (!autoUpdate && !force) return;
        for (Dep d : order) {
            if (stop_) return;
            int64_t age;
            {
                std::lock_guard<std::mutex> lk(mu_);
                age = nowUnix() - st_[(int)d].checked;
            }
            int64_t due = d == Dep::YtDlp ? kDay : 7 * kDay;
            if (force || age > due) {
                std::wstring n;
                install(d, false, n);
            }
        }
    });
}

bool Deps::updateYtDlpNow(bool allowNightly, std::wstring& note) {
    std::wstring n;
    bool changed = install(Dep::YtDlp, false, n);
    note = n;
    if (changed || !allowNightly) return changed;
    bool onNightly;
    {
        std::lock_guard<std::mutex> lk(mu_);
        onNightly = st_[0].channel == L"nightly";
    }
    if (onNightly) return false;
    set(Dep::YtDlp, [](State& s) { s.channel = L"nightly"; });
    changed = install(Dep::YtDlp, false, n);
    if (!changed) set(Dep::YtDlp, [](State& s) { s.channel = L"stable"; });
    note = changed ? L"yt-dlp switched to the nightly build (fixes reach it first): " + n : n;
    save();
    return changed;
}

bool Deps::rollback(Dep d, std::wstring& note) {
    std::wstring r = root(d), cur = joinPath(r, L"current"), prev = joinPath(r, L"previous"), tmp = joinPath(r, L"rollback");
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_[(int)d].users > 0) {
            note = L"In use by a running task; try again when it finishes.";
            return false;
        }
    }
    if (!fileExists(exeIn(d, prev))) {
        note = L"No previous version kept.";
        return false;
    }
    std::wstring version, err;
    if (!smoke(d, prev, version, err)) {
        note = L"The previous version fails its self-test: " + err;
        return false;
    }
    removeTree(tmp);
    if (!MoveFileExW(cur.c_str(), tmp.c_str(), 0) || !MoveFileExW(prev.c_str(), cur.c_str(), 0) || !MoveFileExW(tmp.c_str(), prev.c_str(), 0)) {
        note = L"Could not swap folders.";
        return false;
    }
    set(d, [&](State& s) {
        s.version = version;
        // The build rolled away from is skipped by automatic checks until a newer one is published.
        if (!s.sha.empty()) s.skipSha = s.sha;
        s.sha.clear();
        s.present = true;
        s.error.clear();
    });
    save();
    note = std::wstring(depName(d)) + L" rolled back to " + version;
    logLine(note);
    return true;
}

}  // namespace ea
