// Installs and updates yt-dlp, FFmpeg/FFprobe and Deno in the data folder. Each tool lives in
// deps\<tool>\current, with the version before it in deps\<tool>\previous for rollback.
// A swap waits until no task uses the tool.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ea {

enum class Dep { YtDlp = 0, FFmpeg = 1, Deno = 2 };
constexpr int kDeps = 3;
const wchar_t* depName(Dep d);

struct DepView {
    bool present = false, busy = false;
    double progress = -1;
    std::wstring version, status, error, channel;
    int64_t checked = 0;
    bool hasPrevious = false;
};

class Deps {
public:
    using Notify = std::function<void()>;
    void init(const std::wstring& dataDir, Notify notify);
    void shutdown();

    std::wstring ytdlp() const;
    std::wstring ffmpeg() const;
    std::wstring ffprobe() const;
    std::wstring ffmpegDir() const;
    std::wstring deno() const;
    std::string ffmpegHash() const;  // identifies the FFmpeg build (encoder cache key)
    bool ready(Dep d) const;
    DepView view(Dep d) const;

    // Background: install missing tools, then (if due or forced) look for updates.
    void startMaintenance(bool autoUpdate, bool force);
    // Synchronous, from a task thread after an extraction failure. True if yt-dlp changed.
    bool updateYtDlpNow(bool allowNightly, std::wstring& note);
    bool rollback(Dep d, std::wstring& note);

    // Tasks hold a tool while it runs so it is never swapped underneath them.
    void acquire(Dep d);
    void release(Dep d);
    // Blocks until the tool is installed or cancel is set.
    bool waitReady(Dep d, const std::atomic<bool>& cancel);

private:
    struct State {
        std::wstring version, channel = L"stable";
        std::string sha, skipSha;
        int64_t checked = 0, installed = 0;
        bool present = false, busy = false;
        double progress = -1;
        std::wstring status, error;
        int users = 0;
        bool pendingSwap = false;
        std::wstring pendingVersion;
        std::string pendingSha;
    };
    std::wstring root(Dep d) const;
    std::wstring exeIn(Dep d, const std::wstring& dir) const;
    bool install(Dep d, bool force, std::wstring& note);
    bool fetchExpected(Dep d, const std::wstring& channel, std::string& sha, std::wstring& err);
    bool stage(Dep d, const std::wstring& channel, const std::string& sha, std::wstring& version, std::wstring& err);
    bool smoke(Dep d, const std::wstring& dir, std::wstring& version, std::wstring& err);
    bool swapIn(Dep d);
    void load();
    void save();
    void set(Dep d, const std::function<void(State&)>& f);

    std::wstring dir_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    State st_[kDeps];
    Notify notify_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::mutex installMu_;  // one install at a time
    std::mutex maintMu_;    // one maintenance thread at a time
};

std::string extractHash(const std::string& text, const std::string& name);

}  // namespace ea
