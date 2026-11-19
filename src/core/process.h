// Child processes with pipes, run inside a job object so cancel kills the whole tree
// (yt-dlp starts ffmpeg) and nothing outlives the app.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ea {

struct ProcOpts {
    std::wstring cwd;
    std::vector<std::pair<std::wstring, std::wstring>> env;  // added or replaced
    std::vector<std::wstring> pathPrepend;
    bool lowPriority = false;
};

class Process {
public:
    // Called from reader threads, one complete line at a time (\n or \r ends a line).
    using LineFn = std::function<void(const std::string& line, bool isErr)>;
    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool start(const std::wstring& exe, const std::vector<std::wstring>& args, const ProcOpts& opts, LineFn onLine,
               std::wstring* err = nullptr);
    // Collects stdout raw instead of line by line (yt-dlp -J, ffprobe JSON). Use before start().
    void captureStdout(std::string* sink) { rawOut_ = sink; }
    int wait();  // exit code once the process ended and both pipes drained
    bool waitFor(unsigned ms);
    void kill();
    bool suspend();
    bool resume();
    bool suspended() const { return suspended_; }
    bool running() const;

private:
    void reader(void* pipe, bool isErr);
    void* proc_ = nullptr;
    void* job_ = nullptr;
    std::thread tOut_, tErr_;
    LineFn onLine_;
    std::string* rawOut_ = nullptr;
    std::atomic<bool> suspended_{false};
    std::mutex mu_;
    int code_ = -1;
    bool waited_ = false;
};

struct Capture {
    bool started = false;
    bool timedOut = false;
    int code = -1;
    std::string out, err;
    std::wstring startError;
};

// Synchronous helper for worker threads. cancel may be null.
Capture runCapture(const std::wstring& exe, const std::vector<std::wstring>& args, const ProcOpts& opts = {},
                   unsigned timeoutMs = 120000, const std::atomic<bool>* cancel = nullptr);

}  // namespace ea
