// The task queue: one list for downloads, conversions and remuxes, with per-kind concurrency,
// pause (suspends the process tree), resume, cancel, retry and persistence across restarts.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/json.h"
#include "../core/process.h"
#include "../media/encode.h"
#include "deps.h"
#include "settings.h"

namespace ea {

enum class TaskKind { Download = 0, Convert = 1, Remux = 2 };
enum class TaskState { Waiting = 0, Running = 1, Paused = 2, Done = 3, Failed = 4, Canceled = 5 };
const wchar_t* stateName(TaskState s);

struct TaskView {
    uint64_t id = 0;
    TaskKind kind = TaskKind::Download;
    TaskState state = TaskState::Waiting;
    std::wstring title, stage, output, error, hint, result, speed;
    double progress = -1, eta = -1;
    int attempts = 0;
    QualityClass cls = QualityClass::Copy;
    // From the spec: the list position the item had before it started ("order", "orderSub"), what it
    // came from ("url" or "input"), where it goes ("outDir") and its thumbnail ("thumb").
    int64_t order = 0;
    int sub = 0;
    std::wstring source, outDir;
    std::string thumb;
};

struct Task {
    uint64_t id = 0;
    TaskKind kind = TaskKind::Download;
    TaskState state = TaskState::Waiting;
    std::wstring title, stage, output, error, hint, result, speed;
    double progress = -1, eta = -1;
    int attempts = 0, autoRetries = 0;
    bool triedUpdate = false, triedSoftware = false;
    QualityClass cls = QualityClass::Copy;
    json::Value spec;
    std::vector<std::wstring> log;
    // runtime only
    std::atomic<bool> cancel{false};
    std::atomic<bool> interrupted{false};  // canceled by app shutdown: resume next start, keep partial files
    bool pauseWanted = false;
    Process* proc = nullptr;
    std::thread th;
    int64_t retryAt = 0;  // automatic retry time (ms tick), 0 = none
};

class Queue;

// What a runner sees: progress reporting, logging and process control routed through the queue.
struct RunCtx {
    Queue& q;
    Task& t;
    Deps& deps;
    Settings settings;
    EncoderCaps caps;
    void stage(const std::wstring& s);
    void progress(double p, double eta, const std::wstring& speed);
    void log(const std::wstring& line);
    void setClass(QualityClass c);
    void setOutput(const std::wstring& o);
    void setTitle(const std::wstring& t);
    void saveSpec();  // persist spec changes (e.g. the chosen file name)
    json::Value spec();  // a copy, taken under the queue lock
    void specSet(const std::string& key, json::Value v);
    bool canceled() const { return t.cancel.load(); }
    bool interrupted() const { return t.interrupted.load(); }
    // Runs a process registered with the task (pause/cancel work on it). Returns the exit code, -1 if it never started.
    int run(const std::wstring& exe, const std::vector<std::wstring>& args, const ProcOpts& opts, const Process::LineFn& onLine,
            std::wstring* startErr = nullptr, std::string* rawOut = nullptr);
};

struct RunResult {
    bool ok = false;
    std::wstring error, hint, result;
    bool retryNow = false;     // retry immediately (after a yt-dlp update or a hardware fallback)
    int retryAfterMs = 0;      // automatic retry for transient failures
};

using Runner = std::function<RunResult(RunCtx&)>;

class Queue {
public:
    using Notify = std::function<void(bool force)>;
    void init(Deps* deps, std::function<Settings()> settings, std::function<EncoderCaps()> caps, Notify notify);
    void setRunner(TaskKind k, Runner r) { runners_[(int)k] = std::move(r); }
    void shutdown();

    uint64_t add(TaskKind kind, const std::wstring& title, const json::Value& spec);
    void pause(uint64_t id);
    void resume(uint64_t id);
    void cancel(uint64_t id);
    void retry(uint64_t id);
    void remove(uint64_t id);
    void clearFinished(int kind = -1);  // -1: every kind
    void pauseAll(int kind = -1);
    void resumeAll(int kind = -1);

    std::vector<TaskView> snapshot();
    std::vector<std::wstring> taskLog(uint64_t id);
    int running() const;
    int waiting() const;
    bool busy() const;  // something running, paused or waiting
    void kick();        // re-evaluate scheduling (settings changed, deps ready)

    // Called by RunCtx.
    void update(Task& t, const std::function<void(Task&)>& f, bool force = false);
    void attach(Task& t, Process* p);
    void persist();
    std::mutex& lock() { return mu_; }

private:
    void load();
    void schedule();
    void runTask(Task* t);
    int limitFor(TaskKind k) const;
    Task* find(uint64_t id);
    void scheduler();

    Deps* deps_ = nullptr;
    std::function<Settings()> settings_;
    std::function<EncoderCaps()> caps_;
    Notify notify_;
    Runner runners_[3];
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::list<std::unique_ptr<Task>> tasks_;
    uint64_t nextId_ = 1;
    std::thread sched_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> dirty_{false};
    std::vector<std::thread> finished_;
};

}  // namespace ea
