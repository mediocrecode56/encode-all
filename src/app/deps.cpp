#include "deps.h"

#include <algorithm>
#include <chrono>

#include "../media/hwenc.h"
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
    if (!name.empty()) return findHash(text, name);
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
    v.health = s.health;
    v.staged = s.pendingSwap;
    v.path = exeIn(d, joinPath(root(d), L"current"));
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
            s.pendingChannel = o["pendingChannel"].wstr(s.channel);
            s.pendingSha = o["pendingSha"].str();
        }
    }
}

void Deps::save() {
    std::lock_guard<std::mutex> serial(saveMu_);
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
                o.set("pendingChannel", s.pendingChannel);
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
    std::lock_guard<std::mutex> one(maintMu_);
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
            c = runCapture(exe, {L"--ignore-config", L"--version"}, {}, 15000, &stop_);
            if (c.code == 0) version = firstLine(c.out);
            break;
        case Dep::FFmpeg: {
            c = runCapture(exe, {L"-hide_banner", L"-v", L"error", L"-f", L"lavfi", L"-i", L"testsrc2=s=128x72:d=0.2", L"-c:v", L"libx264", L"-f", L"null", L"-"}, {},
                           60000, &stop_);
            if (c.code != 0) break;
            auto pv = runCapture(joinPath(dir, L"ffprobe.exe"), {L"-v", L"error", L"-f", L"lavfi", L"-i", L"color=s=16x16:d=0.1",
                                 L"-show_entries", L"stream=width,height", L"-of", L"json"}, {}, 15000, &stop_);
            json::Value probe;
            if (pv.code == 0 && (!json::parse(pv.out, probe) || probe["streams"].a.empty())) pv.code = -1;
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
            c = runCapture(exe, {L"eval", L"if (3 * 7 !== 21) Deno.exit(1); console.log(Deno.version.deno)"}, {}, 15000, &stop_);
            if (c.code == 0) {
                version = firstLine(c.out);
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
    if (dirExists(staged) || dirExists(dl) || !makeDirs(staged) || !makeDirs(dl)) {
        err = L"Cannot create a clean staging area in " + r + L". Check disk space and folder permissions.";
        return false;
    }
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
                if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
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
    std::wstring version;
    bool installed = false;
    {
        // The check and rename must be atomic with acquire(), including the state publication.
        std::lock_guard<std::mutex> lk(mu_);
        State& s = st_[(int)d];
        if (s.users || !s.pendingSwap) return false;
        if (fileExists(exeIn(d, staged)) && (d != Dep::FFmpeg || fileExists(joinPath(staged, L"ffprobe.exe")))) {
            bool hadCur = dirExists(cur);
            if (hadCur) removeTree(prev);
            if (!hadCur || MoveFileExW(cur.c_str(), prev.c_str(), 0)) {
                installed = MoveFileExW(staged.c_str(), cur.c_str(), 0) != FALSE;
                if (!installed && hadCur) MoveFileExW(prev.c_str(), cur.c_str(), 0);
            }
        }
        if (installed) {
            s.present = true;
            version = s.version = s.pendingVersion;
            s.sha = s.pendingSha;
            s.channel = s.pendingChannel;
            s.installed = nowUnix();
            s.pendingSwap = false;
            s.status.clear();
            s.error.clear();
            s.health.clear();
            s.progress = -1;
        } else {
            s.present = fileExists(exeIn(d, cur)) && (d != Dep::FFmpeg || fileExists(ffprobe()));
            s.error = L"Could not activate the staged tool. Check access to the tool folder, close other app instances, then retry.";
        }
    }
    cv_.notify_all();
    save();
    if (notify_) notify_();
    if (installed) logLine(std::wstring(depName(d)) + L" " + version + L" installed");
    return installed;
}

bool Deps::install(Dep d, bool reinstall, std::wstring& note, const wchar_t* requestedChannel) {
    std::lock_guard<std::mutex> one(installMu_);
    if (stop_) return false;
    if (!reinstall && !requestedChannel && swapIn(d)) {
        set(d, [](State& s) { s.busy = false; });
        note = std::wstring(depName(d)) + L" activated its staged update";
        return true;
    }
    std::wstring channel;
    std::string curSha, skipSha;
    bool present;
    {
        std::lock_guard<std::mutex> lk(mu_);
        State& s = st_[(int)d];
        channel = requestedChannel ? requestedChannel : s.channel;
        curSha = s.sha;
        skipSha = s.skipSha;
        present = s.present;
    }
    if (!reinstall && view(d).staged) {
        auto staged = view(d);
        note = std::wstring(depName(d)) + (staged.error.empty() ? L" has a verified update waiting for active tasks" : L": " + staged.error);
        set(d, [](State& s) { s.busy = false; });
        return false;
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
    if (present && (sha == curSha || sha == skipSha) && !reinstall) {
        set(d, [&](State& s) {
            s.busy = false;
            s.status.clear();
            s.checked = nowUnix();
        });
        save();
        note = std::wstring(depName(d)) + (sha == curSha ? L" is up to date" : L" uses a rollback; the latest build is skipped by preference");
        return false;
    }
    // A previously pending release must never be swapped while its staging folder is rewritten.
    set(d, [](State& s) { s.pendingSwap = false; });
    save();
    if (stage(d, channel, sha, version, err)) {
        set(d, [&](State& s) {
            s.pendingSwap = true;
            s.pendingVersion = version;
            s.pendingChannel = channel;
            s.pendingSha = sha;
            if (reinstall) s.skipSha.clear();
            s.checked = nowUnix();
            s.status = L"ready, waiting for running tasks";
        });
        save();
        changed = swapIn(d);
        auto current = view(d);
        note = std::wstring(depName(d)) + L" " + version + (changed || (current.present && current.status.empty() && current.error.empty())
                   ? L" installed and verified" : !current.error.empty() ? L": " + current.error : L" verified and staged (applies when idle)");
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

bool Deps::launchMaintenance(std::function<void()> work) {
    std::lock_guard<std::mutex> one(maintMu_);
    if (stop_ || maintaining_) return false;
    if (worker_.joinable()) worker_.join();
    maintaining_ = true;
    worker_ = std::thread([this, work = std::move(work)] {
        work();
        maintaining_ = false;
        if (notify_) notify_();
    });
    return true;
}

bool Deps::checkLocal(Dep d, std::wstring& version, std::wstring& error) {
    acquire(d);
    auto v = view(d);
    bool ok = smoke(d, dirName(v.path), version, error);
    set(d, [&](State& s) {
        s.health = !ok ? error : !s.version.empty() && s.version != version ? L"Installed executable does not match the recorded version." : L"";
        if (ok && s.version.empty()) s.version = version;
    });
    release(d);
    return ok && view(d).health.empty();
}

void Deps::startMaintenance(bool autoUpdate, bool force) {
    launchMaintenance([this, autoUpdate, force] {
        // Missing tools first, smallest first so downloads can start early.
        Dep order[] = {Dep::YtDlp, Dep::Deno, Dep::FFmpeg};
        for (Dep d : order) {
            if (stop_) return;
            if (ready(d)) {
                std::wstring version, error;
                checkLocal(d, version, error);
            } else {
                std::wstring n;
                for (int attempt = 0; attempt < 3 && !ready(d) && !stop_; attempt++) {
                    install(d, false, n);
                    if (!ready(d)) {
                        std::unique_lock<std::mutex> lk(mu_);
                        cv_.wait_for(lk, std::chrono::seconds(3 * (attempt + 1)), [&] { return stop_.load(); });
                    }
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
    if (changed || !allowNightly || repairing_ || stop_) return changed;
    auto v = view(Dep::YtDlp);
    if (v.staged || v.channel == L"nightly" || !v.error.empty()) return false;
    changed = install(Dep::YtDlp, false, n, L"nightly");
    note = changed ? L"yt-dlp switched to the nightly build: " + n : n;
    return changed;
}

bool Deps::rollback(Dep d, std::wstring& note) {
    std::lock_guard<std::mutex> serial(installMu_);
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        State& s = st_[(int)d];
        if (s.users || s.pendingSwap) {
            note = L"In use or awaiting an update; try again when it finishes.";
            return false;
        }
        if (dirExists(tmp)) {
            note = L"An interrupted rollback needs attention in " + r + L". Use Auto Repair to stage a clean build.";
            return false;
        }
        if (!MoveFileExW(cur.c_str(), tmp.c_str(), 0)) {
            note = L"Could not move the current tool.";
            return false;
        }
        if (!MoveFileExW(prev.c_str(), cur.c_str(), 0)) {
            MoveFileExW(tmp.c_str(), cur.c_str(), 0);
            note = L"Could not start the previous tool; the current tool was not replaced.";
            return false;
        }
        bool kept = MoveFileExW(tmp.c_str(), prev.c_str(), 0) != FALSE;
        s.version = version;
        if (!s.sha.empty()) s.skipSha = s.sha;
        s.sha.clear();
        s.present = true;
        s.error.clear();
        s.health.clear();
        if (!kept) s.error = L"The replaced version was retained in the rollback folder.";
    }
    save();
    note = std::wstring(depName(d)) + L" rolled back to " + version;
    logLine(note);
    if (notify_) notify_();
    return true;
}

bool Deps::startRepair(bool rebuild, std::vector<std::wstring> recentErrors, const Settings& settings) {
    if (repairing_.exchange(true)) return false;
    if (launchMaintenance([this, rebuild, errors = std::move(recentErrors), settings] {
        repair(rebuild, errors, settings);
        repairing_ = false;
    })) return true;
    repairing_ = false;
    return false;
}

std::vector<std::wstring> Deps::repairReport() const {
    std::lock_guard<std::mutex> lk(mu_);
    return repairReport_;
}

void Deps::repair(bool rebuild, const std::vector<std::wstring>& errors, const Settings& settings) {
    std::vector<std::wstring> report;
    for (Dep d : {Dep::YtDlp, Dep::Deno, Dep::FFmpeg}) {
        if (stop_) return;
        set(d, [](State& s) { s.busy = true; s.status = L"running diagnostics"; });
        std::wstring version, error;
        bool healthy = checkLocal(d, version, error);
        auto before = view(d);
        std::wstring note;
        bool changed = install(d, rebuild || !healthy, note);
        auto current = view(d);
        if (!healthy && error.empty()) error = before.health;
        if (changed) {
            std::wstring installedVersion, installedError;
            if (!checkLocal(d, installedVersion, installedError))
                report.push_back(std::wstring(depName(d)) + L" after activation: " + view(d).health);
        }
        if (d == Dep::YtDlp && settings.nightlyFallback && !changed && !current.staged && current.error.empty() && current.health.empty() && current.channel != L"nightly") {
            bool extraction = std::any_of(errors.begin(), errors.end(), [](const std::wstring& line) {
                auto t = lower(line);
                return t.find(L"unable to extract") != t.npos || t.find(L"signature extraction") != t.npos || t.find(L"nsig") != t.npos;
            });
            if (extraction) {
                std::wstring next;
                if (install(d, false, next, L"nightly") || view(d).staged) report.push_back(L"Recent extraction failure: " + next);
                else report.push_back(L"Nightly fallback did not replace yt-dlp: " + next);
            }
        }
        if (healthy && !version.empty()) report.push_back(std::wstring(depName(d)) + L" " + version + L" passed its local test.");
        else report.push_back(std::wstring(depName(d)) + L": " + error);
        report.push_back(note);
        if (!current.error.empty() && current.error != note) report.push_back(current.error);
    }
    if (!settings.cookiesFile.empty() && !fileExists(settings.cookiesFile))
        report.push_back(L"Cookies file is missing. Choose a replacement or none under Downloads > Sign-in cookies.");
    const std::pair<const wchar_t*, const std::wstring*> outputs[] = {{L"Download", &settings.dlDir}, {L"Convert", &settings.convertDir}, {L"Remux", &settings.remuxDir}};
    for (const auto& [label, path] : outputs) {
        if (path->empty()) continue;
        if (!dirExists(*path)) report.push_back(std::wstring(label) + L" output folder does not exist. Check its drive or choose another folder.");
        else {
            auto free = freeSpace(*path);
            if (free == UINT64_MAX || !free) report.push_back(std::wstring(label) + L" output folder is inaccessible or out of space.");
            else {
                std::wstring probe = joinPath(*path, L".encodeall-write-test-" + std::to_wstring(GetCurrentProcessId()));
                HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (file == INVALID_HANDLE_VALUE) report.push_back(std::wstring(label) + L" output folder did not allow a temporary file. Check write access.");
                else CloseHandle(file);
            }
        }
    }
    if (settings.encoder != Accel::Auto && settings.encoder != Accel::Software && ready(Dep::FFmpeg) && !stop_) {
        acquire(Dep::FFmpeg);
        EncoderCaps caps = detectEncoders(ffmpeg(), &stop_);
        release(Dep::FFmpeg);
        const char* suffix = settings.encoder == Accel::NVENC ? "nvenc" : settings.encoder == Accel::QSV ? "qsv" : "amf";
        if (!caps.has(std::string("h264_") + suffix) && !caps.has(std::string("hevc_") + suffix))
            report.push_back(L"The pinned hardware encoder failed. Choose Automatic or Software in Settings, or update your graphics driver.");
    }
    for (const auto& line : errors) {
        const std::wstring t = lower(line);
        auto has = [&](const wchar_t* word) { return t.find(word) != std::wstring::npos; };
        std::wstring advice;
        if (has(L"cookie") || has(L"sign in") || has(L"sign-in"))
            advice = L"Download: check Sign-in cookies for the site. Auto Repair does not read or change credentials.";
        else if (has(L"no space") || has(L"disk full") || has(L"output folder") || has(L"permission denied"))
            advice = L"Output: check free space and write access, or choose a different Save To folder.";
        else if (has(L"timed out") || has(L"certificate") || has(L"network"))
            advice = L"Network: check connectivity and proxy/certificate settings before retrying.";
        else if (has(L"nvenc") || has(L"qsv") || has(L"amf"))
            advice = L"Convert: if the graphics encoder keeps failing, update its driver or choose Software under Encoding.";
        else if (has(L"channel layout") || has(L"cannot be copied") || has(L"not supported in container"))
            advice = L"Convert/Remux: inspect the failing track in the plan; try MOV with original audio or exact PCM.";
        else if (has(L"extract") || has(L"signature") || has(L"javascript"))
            advice = L"Download: yt-dlp and Deno were tested and checked for updates. If extraction still fails, the site may need an upstream fix.";
        if (!advice.empty() && std::find(report.begin(), report.end(), advice) == report.end()) report.push_back(std::move(advice));
    }
    if (!errors.empty()) report.push_back(L"Reviewed " + std::to_wstring(errors.size()) + L" recent error lines. Existing tasks and preferences were not changed.");
    {
        std::lock_guard<std::mutex> lk(mu_);
        repairReport_ = report;
    }
    for (const auto& line : report) logLine(L"Auto Repair: " + line);
}

}  // namespace ea
