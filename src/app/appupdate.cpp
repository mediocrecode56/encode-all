#include "appupdate.h"

#include <algorithm>
#include <windows.h>

#include <vector>

#include "../core/json.h"
#include "../core/net.h"
#include "../core/process.h"
#include "../core/util.h"
#include "../version.h"
#include "settings.h"

namespace ea {

int compareVersions(const std::string& a, const std::string& b) {
    auto pa = split(a, '.'), pb = split(b, '.');
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); i++) {
        long x = i < pa.size() ? atol(pa[i].c_str()) : 0, y = i < pb.size() ? atol(pb[i].c_str()) : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool parseReleaseSums(const std::string& sums, std::string& version, std::string& zipName, std::string& sha) {
    for (auto& raw : split(sums, '\n')) {
        std::string l = trim(raw);
        size_t p = l.find("EncodeAll-");
        if (p == std::string::npos || l.size() < 66) continue;
        std::string name = l.substr(p);
        const std::string suffix = "-win64.zip";
        if (name.size() <= 10 + suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        std::string v = name.substr(10, name.size() - 10 - suffix.size());
        if (v.empty() || v.find_first_not_of("0123456789.") != std::string::npos) continue;
        sha = lower(l.substr(0, 64));
        version = v;
        zipName = name;
        return true;
    }
    return false;
}

static std::wstring markerPath() { return joinPath(dataDir(), L"update.json"); }

bool AppUpdater::startupGuard(bool relaunchOld) {
    std::string t;
    json::Value m;
    if (!readFile(markerPath(), t) || !json::parse(t, m)) return false;
    std::string to = m["to"].str();
    if (to != EA_VERSION) {
        // The swap did not happen (or an older exe is running): forget the marker.
        DeleteFileW(markerPath().c_str());
        return false;
    }
    int trials = (int)m["trials"].num(0) + 1;
    std::wstring exe = exePath(), old = exe + L".old";
    if (trials >= 3 && fileExists(old)) {
        // Two earlier starts of this version never became healthy: put the old exe back.
        std::wstring bad = exe + L".bad";
        DeleteFileW(bad.c_str());
        if (MoveFileExW(exe.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING) && MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(markerPath().c_str());
            logLine(L"update to " + widen(to) + L" failed to start twice; restored the previous version");
            if (relaunchOld) relaunch();
            return true;
        }
    }
    m.set("trials", trials);
    writeFileAtomic(markerPath(), json::dump(m));
    return false;
}

void AppUpdater::markHealthy() {
    if (fileExists(markerPath())) {
        DeleteFileW(markerPath().c_str());
        DeleteFileW((exePath() + L".bad").c_str());
        logLine(L"running " EA_VERSION_W L" (update healthy)");
    }
}

bool AppUpdater::relaunch() {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoteArg(exePath()) + L" --relaunched";
    if (!CreateProcessW(exePath().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

UpdateState AppUpdater::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

void AppUpdater::set(const std::wstring& status, const std::wstring& err, bool checking) {
    std::lock_guard<std::mutex> lk(mu_);
    st_.status = status;
    st_.error = err;
    st_.checking = checking;
}

void AppUpdater::checkAndStage(const std::atomic<bool>& stop) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (st_.checking || st_.staged) return;
        st_.checking = true;
    }
    set(L"checking for updates", L"", true);
    std::string sums;
    std::wstring base = L"https://github.com/" EA_REPO_W L"/releases/latest/download/";
    // Test hook (tests/update-test.ps1): a local folder served over HTTP stands in for the release.
    wchar_t over[512];
    if (GetEnvironmentVariableW(L"EA_UPDATE_BASE", over, 512) > 0) base = over;
    auto r = httpGet(base + L"SHA256SUMS", &sums, L"", nullptr, &stop, 1 << 20);
    if (!r.ok) {
        set(L"", r.status == 404 ? L"" : L"update check failed: " + r.error);
        return;
    }
    std::string version, zip, sha;
    if (!parseReleaseSums(sums, version, zip, sha)) {
        set(L"", L"update check: no package in the latest release");
        return;
    }
    if (compareVersions(version, EA_VERSION) <= 0) {
        set(L"up to date");
        return;
    }
    std::wstring dir = joinPath(joinPath(dataDir(), L"update"), widen(version));
    removeTree(dir);
    makeDirs(dir);
    std::wstring file = joinPath(dir, widen(zip));
    set(L"downloading " + widen(version), L"", true);
    r = httpGet(base + widen(zip), nullptr, file, nullptr, &stop);
    if (!r.ok) {
        set(L"", L"update download failed: " + r.error);
        return;
    }
    if (sha256File(file) != sha) {
        removeTree(dir);
        set(L"", L"update rejected: checksum mismatch");
        return;
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    auto x = runCapture(joinPath(sys, L"tar.exe"), {L"-xf", file, L"-C", dir}, {}, 120000, &stop);
    std::wstring newExe = joinPath(dir, L"EncodeAll.exe");
    if (x.code != 0 || !fileExists(newExe)) {
        set(L"", L"update package unreadable");
        return;
    }
    // The new exe must start and report its version before it replaces anything.
    auto v = runCapture(newExe, {L"--version"}, {}, 30000, &stop);
    if (v.code != 0 || trim(v.out) != version) {
        set(L"", L"update self-test failed");
        return;
    }
    std::wstring exe = exePath(), old = exe + L".old";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        set(L"", L"update: the install folder is not writable");
        return;
    }
    if (!MoveFileExW(newExe.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        set(L"", L"update: could not place the new version");
        return;
    }
    json::Value m = json::Value::object();
    m.set("from", EA_VERSION);
    m.set("to", version);
    m.set("trials", 0);
    writeFileAtomic(markerPath(), json::dump(m));
    removeTree(dir);
    logLine(L"update " + widen(version) + L" staged; applies on next start");
    std::lock_guard<std::mutex> lk(mu_);
    st_.checking = false;
    st_.staged = true;
    st_.stagedVersion = widen(version);
    st_.status = L"version " + widen(version) + L" ready";
    st_.error.clear();
}

}  // namespace ea
