// Self-update through GitHub Releases: the zip named in SHA256SUMS is verified, its exe swapped
// in beside the running one (the old one is kept as EncodeAll.exe.old) and used on the next start.
// Three starts of a new version that never reach "healthy" restore the old exe.
#pragma once
#include <atomic>
#include <mutex>
#include <string>

namespace ea {

int compareVersions(const std::string& a, const std::string& b);  // <0, 0, >0
// Finds "EncodeAll-<v>-win64.zip" in a SHA256SUMS listing.
bool parseReleaseSums(const std::string& sums, std::string& version, std::string& zipName, std::string& sha);

struct UpdateState {
    bool checking = false, staged = false;
    std::wstring status, error, stagedVersion;
};

class AppUpdater {
public:
    // Call first thing in wWinMain. Returns true if the process should exit (it relaunched the old exe).
    static bool startupGuard(bool relaunchOld = true);
    static void markHealthy();
    // Worker thread: check, download, verify, swap. Safe while tasks run (they do not use the exe file).
    void checkAndStage(const std::atomic<bool>& stop);
    UpdateState state() const;
    static bool relaunch();

private:
    void set(const std::wstring& status, const std::wstring& err = L"", bool checking = false);
    mutable std::mutex mu_;
    UpdateState st_;
};

}  // namespace ea
